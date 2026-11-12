#!/bin/bash
# Record + checkpoint + restore + replay of a server with several processes that
# each run several threads: Apache httpd 2.4, event MPM.
#
#   container A: httpd (a parent, 2 server processes; in each a listener thread
#   and 4 worker threads fed through a queue with a mutex and a condition)
#   a few requests                        gen 0 (state inside the checkpoint)
#   podman checkpoint --leave-running     gen 1 starts
#   8 clients in parallel, 40 requests each over keep-alive connections,
#   every URL different, one in five a 404
#   podman pause A, stop recording
#   restore the checkpoint as B (left stopped), sysrr_replay -g 1 -T -e live -w
#
# The access log names the process and the thread that served each request.
# PASS if the replay ends without divergence and B logged the same lines as A:
# every request was served by the same thread of the same process.  The lines of
# different processes may come in another order (they share one pipe), so the
# sorted lines are compared.  No client ever talked to B.
#
# If the replay diverges, sysrr_search.sh looks for an order of the threads that
# fits the whole recording (SEARCH=tries, default 40, 0 = off), and the final
# replay uses it.
#
# Environment: REPLAY_OPTS (default -T), REMOTE=host, SSH="ssh -i key", OUT, CLIENTS, REQS, SEARCH
set -e -o pipefail
cd "$(dirname "$0")"
DIR=$PWD
REPLAY_OPTS=${REPLAY_OPTS--T}
OUT=${OUT:-/var/tmp/sysrr-httpd}
REMOTE=${REMOTE:-}
SSH=${SSH:-ssh}
CLIENTS=${CLIENTS:-8}
REQS=${REQS:-40}
IMG=docker.io/library/httpd:2.4
SRC=rrsrc DST=rrdst
CS=/sys/devices/system/clocksource/clocksource0/current_clocksource
CONF=/etc/criu/default.conf

R() {
	if [ -n "$REMOTE" ]; then $SSH "$REMOTE" "cd '$DIR' && $*"; else bash -c "$*"; fi
}
put() { [ -z "$REMOTE" ] || sudo cat "$1" | $SSH "$REMOTE" "cat > '$1'"; }
hc() { sudo podman exec -i "$1" perl - "${@:2}" <httpd_client.pl; }

rm -rf "$OUT"
mkdir -p "$OUT"
make -s -C .. sysrr sysrr_dump sysrr_replay

SETUP="echo acpi_pm | sudo tee $CS >/dev/null; mkdir -p '$OUT'; sudo rm -f '$OUT/criu.conf.bak';
       [ -e $CONF ] && sudo cp $CONF '$OUT/criu.conf.bak'; sudo mkdir -p /etc/criu; true"
RESTORE_CONF="if [ -e '$OUT/criu.conf.bak' ]; then sudo cp '$OUT/criu.conf.bak' $CONF; else sudo rm -f $CONF; fi"

[ -z "$REMOTE" ] || $SSH "$REMOTE" "mkdir -p '$DIR'"
old_cs=$(cat $CS)
r_old_cs=$(R "cat $CS")
cleanup() {
	sudo pkill -INT -x sysrr 2>/dev/null || true
	echo "$old_cs" | sudo tee $CS >/dev/null
	sudo podman rm -f -t 0 $SRC >/dev/null 2>&1 || true
	R "sudo pkill -INT -x sysrr_replay; sleep 0.3; echo $r_old_cs | sudo tee $CS >/dev/null;
	   $RESTORE_CONF; sudo podman rm -f -t 0 $DST >/dev/null 2>&1; true" || true
}
trap cleanup EXIT
echo acpi_pm | sudo tee $CS >/dev/null
[ -z "$REMOTE" ] || $SSH "$REMOTE" "rm -rf '$OUT'"
R "$SETUP"
sudo podman rm -f -t 0 $SRC >/dev/null 2>&1 || true
R "sudo podman rm -f -t 0 $DST >/dev/null 2>&1; true"

echo "--- start httpd (on $(hostname))"
sudo podman run -d --name $SRC $IMG httpd -DFOREGROUND \
	-c 'StartServers 2' -c 'ServerLimit 2' -c 'ThreadsPerChild 3' -c 'ThreadLimit 3' \
	-c 'MinSpareThreads 1' -c 'MaxSpareThreads 8' -c 'MaxRequestWorkers 6' \
	-c 'LogFormat "%h %t \"%r\" %>s %b pid=%P tid=%{tid}P" rr' \
	-c 'CustomLog /proc/self/fd/1 rr' >/dev/null
for i in $(seq 50); do
	hc $SRC 1 1 2>/dev/null && break
	sleep 0.1
done
PID=$(sudo podman inspect -f '{{.State.Pid}}' $SRC)
echo "httpd pid $PID: $(sudo podman top $SRC hpid | tail -n +2 | wc -l) processes, $(for p in $(sudo podman top $SRC hpid | tail -n +2); do ls /proc/$p/task; done | wc -l) threads"

echo "--- record; a few requests before the checkpoint"
sudo ../sysrr -f -p "$PID" -o "$OUT/rec.log" 2>"$OUT/sysrr.err" &
sleep 1
hc $SRC 2 3
sleep 0.5
before=$(sudo podman logs $SRC 2>/dev/null | wc -l)
sudo podman container checkpoint --leave-running -e "$OUT/ckpt.tar.gz" $SRC >/dev/null || {
	echo "FAIL: checkpoint failed; CRIU says:"
	sudo grep -E "Error|Warn" "$(sudo podman inspect -f "{{.StaticDir}}" $SRC)/dump.log" | tail -8
	exit 1
}
echo "--- checkpoint taken; $CLIENTS clients x $REQS requests after it (generation 1)"
hc $SRC "$CLIENTS" "$REQS"
sleep 0.5
sudo podman pause $SRC >/dev/null
sleep 0.3
sudo pkill -INT -x sysrr
wait
sudo podman logs $SRC 2>/dev/null | tail -n +$((before + 1)) | grep "pid=" | sort >"$OUT/live.txt"
echo "live A: $(wc -l <"$OUT/live.txt") log lines after the checkpoint, by $(grep -o "tid=[0-9]*" "$OUT/live.txt" | sort -u | wc -l) threads in $(grep -o "pid=[0-9]*" "$OUT/live.txt" | sort -u | wc -l) processes"
sudo podman rm -f -t 0 $SRC >/dev/null
# (exits non-zero for threads caught mid-syscall at stop: not a failure of the test)
../sysrr_dump -s "$OUT/rec.log" | tail -4 || true

if [ -n "$REMOTE" ]; then
	echo "--- copy to $REMOTE: sysrr_replay, client, checkpoint, log"
	tar -C .. -cf - sysrr_replay sysrr_search.sh | $SSH "$REMOTE" "tar -C '$DIR/..' -xf -"
	put "$OUT/ckpt.tar.gz"
	put "$OUT/rec.log"
	if ! $SSH "$REMOTE" "sudo podman image exists $IMG"; then
		echo "    image $IMG missing there: copying it"
		sudo podman save $IMG | $SSH "$REMOTE" "sudo podman load -q"
	fi
fi
WHERE=$(R hostname)
R "sudo podman run --rm $IMG true"      # load podman's AppArmor profile there (REPLAY.md)

# restore the checkpoint as $DST, left stopped, and replay it live with flips $1
replay_live() {
	R "sudo pkill -INT -x sysrr_replay; sleep 0.3; sudo podman rm -f -t 0 $DST >/dev/null 2>&1; true"
	PID2=$(R "echo leave-stopped | sudo tee $CONF >/dev/null;
		   sudo podman container restore --import '$OUT/ckpt.tar.gz' --name $DST >/dev/null; rc=\$?;
		   $RESTORE_CONF; [ \$rc = 0 ] && sudo podman inspect -f '{{.State.Pid}}' $DST")
	echo "restored pid $PID2: $(R "grep State /proc/$PID2/status")"
	R "rm -f '$OUT/replay_out.txt'; setsid -f sudo ../sysrr_replay $REPLAY_OPTS ${1:+-F $1} -p $PID2 -g 1 -e live \
		-w '$OUT/replay_out.txt' '$OUT/rec.log' >'$OUT/replay.raw' 2>&1 </dev/null"
	for i in $(seq 450); do
		R "grep -qE 'end of the recording|DIVERGENCE|target exited|sysrr_replay:' '$OUT/replay.raw'" && break
		sleep 0.2
	done
	sleep 0.3
	R "cat '$OUT/replay.raw'" >"$OUT/replay.txt"
}

echo "--- restore on $WHERE, left stopped; replay (-e live: B goes on for real after the recording)"
replay_live
grep -v '^#' "$OUT/replay.txt" | tail -25
FLIPS=
if grep -q DIVERGENCE "$OUT/replay.txt" && [ "${SEARCH:-40}" != 0 ]; then
	# Which thread went first, where two overlapped, is a guess: search for an
	# order of threads that replays the whole recording (sysrr_search.sh)
	echo "--- diverged: searching for an order of threads that fits the recording"
	R "sudo pkill -INT -x sysrr_replay; sleep 0.3; sudo podman rm -f -t 0 $DST >/dev/null 2>&1;
	   ../sysrr_search.sh -n ${SEARCH:-40} -o '$(echo $REPLAY_OPTS | sed 's/-T//')' \
	   -i '$OUT/ckpt.tar.gz' '$OUT/rec.log'" | tee "$OUT/search.txt" | tail -3 || true
	FLIPS=$(sed -n 's/^FOUND -F \([0-9,]*\).*/\1/p' "$OUT/search.txt")
	[ "$FLIPS" != none ] || FLIPS=
	if grep -q '^FOUND' "$OUT/search.txt"; then
		echo "--- replay again${FLIPS:+ with -F $FLIPS}"
		replay_live "$FLIPS"
		grep -v '^#' "$OUT/replay.txt" | tail -6
	fi
fi

fail=0
[ -z "$REMOTE" ] || R "cat '$OUT/replay_out.txt'" >"$OUT/replay_out.txt"
grep "pid=" "$OUT/replay_out.txt" | sort >"$OUT/replayed.txt"
echo "replayed B: $(wc -l <"$OUT/replayed.txt") log lines"
grep -q 'end of the recording reached' "$OUT/replay.txt" || { echo "FAIL: replay did not reach the end"; fail=1; }
cmp -s "$OUT/live.txt" "$OUT/replayed.txt" || { echo "FAIL: log lines differ ($(comm -3 "$OUT/live.txt" "$OUT/replayed.txt" | wc -l) lines)"; fail=1; }
grep -q DIVERGENCE "$OUT/replay.txt" && { echo "FAIL: divergence"; fail=1; }
[ $fail = 0 ] && echo "PASS: httpd on $WHERE served every request with the same thread of the same process${FLIPS:+ (order found by search: -F $FLIPS)}"
exit $fail
