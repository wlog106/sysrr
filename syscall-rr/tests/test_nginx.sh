#!/bin/bash
# Record + checkpoint + restore + replay of a multi-process server: nginx,
# one master and 2 worker processes (tests/nginx/nginx.conf, reuseport so
# both workers accept connections).
#
#   container A: nginx master + 2 workers, sysrr -f records the whole tree
#   podman checkpoint --leave-running     gen 1 starts
#   curl inside A: parallel keep-alive connections + single requests to
#   /, /id (PRNG + clock + worker pid), /50x.html, a 404
#   podman pause A, stop recording
#   restore as B (left stopped), sysrr_replay attaches one listener per
#   process (3), replays gen 1, then lets B run live (-e live)
#
# PASS if the replay ends without divergence, the access log lines B wrote
# during the replay (captured with -w) equal the ones A wrote after the
# checkpoint (request ids, times, worker pids, connection numbers), and B
# then answers a real request.
#
# Environment: REPLAY_OPTS (default -T; -S fails ~1 in 3, see REPLAY.md), REMOTE=host, SSH="ssh -i key", OUT, N
set -e -o pipefail
cd "$(dirname "$0")"
DIR=$PWD
REPLAY_OPTS=${REPLAY_OPTS--T}       # serial order: workers race on a shared counter
OUT=${OUT:-/var/tmp/sysrr-nginx}
REMOTE=${REMOTE:-}
SSH=${SSH:-ssh}
N=${N:-200}
IMG=docker.io/library/nginx:1.27
SRC=rrsrc DST=rrdst
CS=/sys/devices/system/clocksource/clocksource0/current_clocksource
CONF=/etc/criu/default.conf

R() {
	if [ -n "$REMOTE" ]; then $SSH "$REMOTE" "cd '$DIR' && $*"; else bash -c "$*"; fi
}
put() { [ -z "$REMOTE" ] || sudo cat "$1" | $SSH "$REMOTE" "cat > '$1'"; }

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
	sudo podman rm -f -v -t 0 $SRC >/dev/null 2>&1 || true
	R "sudo pkill -INT -x sysrr_replay; sleep 0.3; echo $r_old_cs | sudo tee $CS >/dev/null;
	   $RESTORE_CONF; sudo podman rm -f -v -t 0 $DST >/dev/null 2>&1; true" || true
}
trap cleanup EXIT
echo acpi_pm | sudo tee $CS >/dev/null
[ -z "$REMOTE" ] || $SSH "$REMOTE" "rm -rf '$OUT'"
R "$SETUP"
sudo podman rm -f -v -t 0 $SRC >/dev/null 2>&1 || true
R "sudo podman rm -f -v -t 0 $DST >/dev/null 2>&1; true"

echo "--- start nginx (on $(hostname))"
sudo podman run -d --name $SRC -v "$DIR/nginx/nginx.conf":/etc/nginx/nginx.conf:ro $IMG >/dev/null
for i in $(seq 50); do
	sudo podman exec $SRC curl -sf -o /dev/null localhost/ && break
	sleep 0.1
done
PID=$(sudo podman inspect -f '{{.State.Pid}}' $SRC)
echo "master pid $PID; processes: $(ps --ppid $PID -o pid=,comm= | tr -s ' \n' ' ')"

echo "--- record the tree (-f); a few requests before the checkpoint"
sudo ../sysrr -f -p "$PID" -o "$OUT/rec.log" 2>"$OUT/sysrr.err" &
sleep 1
grep follow "$OUT/sysrr.err" || true
sudo podman exec $SRC sh -c 'for i in 1 2 3 4 5; do curl -s -o /dev/null localhost/id; done'
sleep 0.5
sudo podman container checkpoint --leave-running -e "$OUT/ckpt.tar.gz" $SRC >/dev/null
echo "--- checkpoint taken; $N parallel keep-alive requests + 20 single ones (gen 1)"
sudo podman exec $SRC sh -c '
	args=""
	for i in $(seq '"$N"'); do
		case $((i % 4)) in
		0) u=/ ;; 1) u=/id ;; 2) u=/50x.html ;; 3) u=/nope$i ;;
		esac
		args="$args -o /dev/null http://localhost$u"
	done
	curl -s -Z --parallel-max 8 $args
	for i in $(seq 20); do curl -s localhost/id >/dev/null; done'
sleep 0.5
sudo podman pause $SRC >/dev/null
sleep 0.3
sudo pkill -INT -x sysrr
wait
sudo podman logs $SRC >"$OUT/live_all.txt" 2>"$OUT/live_err.txt"
sudo podman rm -f -v -t 0 $SRC >/dev/null
echo "live access log: $(wc -l <"$OUT/live_all.txt") lines; workers in it: $(grep -o 'pid=[0-9]*' "$OUT/live_all.txt" | sort | uniq -c | tr -s ' \n' ' ')"
# (exits non-zero for threads caught mid-syscall at stop: not a failure of the test)
../sysrr_dump -s "$OUT/rec.log" | tail -4 || true

if [ -n "$REMOTE" ]; then
	echo "--- copy to $REMOTE: sysrr_replay, tests dir, checkpoint, log"
	tar -C .. -cf - sysrr_replay tests | $SSH "$REMOTE" "tar -C '$DIR/..' -xf -"
	put "$OUT/ckpt.tar.gz"
	put "$OUT/rec.log"
	if ! $SSH "$REMOTE" "sudo podman image exists $IMG"; then
		echo "    image $IMG missing there: copying it"
		sudo podman save $IMG | $SSH "$REMOTE" "sudo podman load -q"
	fi
fi
WHERE=$(R hostname)
# load podman's AppArmor profile there (see REPLAY.md, "AppArmor")
R "sudo podman run --rm --image-volume ignore $IMG true"

echo "--- restore on $WHERE, left stopped"
PID2=$(R "echo leave-stopped | sudo tee $CONF >/dev/null;
	   sudo podman container restore --import '$OUT/ckpt.tar.gz' --name $DST >/dev/null; rc=\$?;
	   $RESTORE_CONF; [ \$rc = 0 ] && sudo podman inspect -f '{{.State.Pid}}' $DST")
echo "restored master $PID2: $(R "grep State /proc/$PID2/status"); children: $(R "ps --ppid $PID2 -o pid=,stat=,comm=" | tr -s ' \n' ' ')"

echo "--- replay on $WHERE"
R "setsid -f sudo ../sysrr_replay $REPLAY_OPTS -p $PID2 -g 1 -e live -w '$OUT/replay_out.txt' '$OUT/rec.log' \
	>'$OUT/replay.raw' 2>&1 </dev/null"
for i in $(seq 300); do
	R "grep -qE 'end of the recording|DIVERGENCE|target exited|sysrr_replay:' '$OUT/replay.raw'" && break
	sleep 0.2
done
sleep 0.5
R "cat '$OUT/replay.raw'" >"$OUT/replay.txt"   # (a different file: safe here too)
grep -vE '^#|reached the end of its' "$OUT/replay.txt" | tail -22
[ -z "$REMOTE" ] || R "cat '$OUT/replay_out.txt'" >"$OUT/replay_out.txt"

echo "--- compare logs"
# -w captured B's writes to stdout (access log) and stderr (error log: one
# "open() failed" line per 404) interleaved; A's are in two files
grep '^127\.0\.0\.1 ' "$OUT/replay_out.txt" >"$OUT/replay_access.txt" || true
grep -v '^127\.0\.0\.1 ' "$OUT/replay_out.txt" >"$OUT/replay_error.txt" || true
n=$(wc -l <"$OUT/replay_access.txt")
ne=$(wc -l <"$OUT/replay_error.txt")
tail -n "$n" "$OUT/live_all.txt" >"$OUT/live_access_tail.txt"
tail -n "$ne" "$OUT/live_err.txt" >"$OUT/live_error_tail.txt"
echo "replayed nginx wrote $n access + $ne error log lines; first and last access line, live / replayed:"
paste -d'|' "$OUT/live_access_tail.txt" "$OUT/replay_access.txt" | sed -n '1p;$p' | sed 's/|/\n   /'
echo "workers in the replayed access log: $(grep -o 'pid=[0-9]*' "$OUT/replay_access.txt" | sort | uniq -c | tr -s ' \n' ' ')"
fail=0
grep -q 'end of the recording reached' "$OUT/replay.txt" || { echo "FAIL: end not reached"; fail=1; }
grep -q DIVERGENCE "$OUT/replay.txt" && { echo "FAIL: divergence"; fail=1; }
[ "$n" -gt 0 ] || { echo "FAIL: no log lines captured"; fail=1; }
# Each worker's own lines must come in the same order.  Across workers the
# live log has the order in which writes *landed* in the shared pipe; the
# replay follows the recorded order in which the workers *entered* write().
# Two workers writing within microseconds can land the other way round, so
# across workers only the set of lines must match.
for w in $(grep -o 'pid=[0-9]*' "$OUT/live_access_tail.txt" | sort -u); do
	cmp -s <(grep " $w " "$OUT/live_access_tail.txt") <(grep " $w " "$OUT/replay_access.txt") ||
		{ echo "FAIL: worker $w: its access log lines differ"; fail=1; }
done
cmp -s <(sort "$OUT/live_access_tail.txt") <(sort "$OUT/replay_access.txt") ||
	{ echo "FAIL: access logs differ"; fail=1; }
cmp -s "$OUT/live_access_tail.txt" "$OUT/replay_access.txt" ||
	echo "(note: $(diff "$OUT/live_access_tail.txt" "$OUT/replay_access.txt" | grep -c '^<') line(s) of different workers swapped places: write entry order vs pipe order)"
# error log lines name the worker as "24#24:"; same rule as the access log
for w in $(grep -o ' [0-9]*#[0-9]*:' "$OUT/live_error_tail.txt" | sort -u); do
	cmp -s <(grep -F " $w" "$OUT/live_error_tail.txt") <(grep -F " $w" "$OUT/replay_error.txt") ||
		{ echo "FAIL: worker $w: its error log lines differ"; fail=1; }
done
cmp -s <(sort "$OUT/live_error_tail.txt") <(sort "$OUT/replay_error.txt") ||
	{ echo "FAIL: error logs differ"; fail=1; }
cmp -s "$OUT/live_error_tail.txt" "$OUT/replay_error.txt" ||
	echo "(note: error log: $(diff "$OUT/live_error_tail.txt" "$OUT/replay_error.txt" | grep -c '^<') line(s) of different workers swapped places)"

echo "--- after the replay, B serves for real"
live=$(R "sudo podman exec $DST curl -s -m 5 localhost/id" || true)
echo "B answered: ${live:-(nothing)}"
[ -n "$live" ] || { echo "FAIL: B does not answer"; fail=1; }
[ $fail = 0 ] && echo "PASS: nginx (3 processes) on $WHERE: $n access + $ne error log lines identical, then served live"
exit $fail
