#!/bin/bash
# Record + checkpoint + restore + replay of a threaded server: memcached 1.6.
#
#   container A: memcached (a listener thread, 4 worker threads each with its own
#   epoll loop, and maintenance threads; one hash table behind locks)
#   load 100 keys                         gen 0 (state inside the checkpoint)
#   podman checkpoint --leave-running     gen 1 starts
#   4 clients in parallel, 300 operations each (set, get, append, incr, delete),
#   every client on its own keys
#   dump of every key in A                the dataset the clients left behind
#   podman pause A, stop recording
#   restore the checkpoint as B (left stopped), sysrr_replay -g 1 -T -e live
#   when the recording is used up, B runs live: dump of every key in B
#
# PASS if the replay ends without divergence and B's dump equals A's.  No client
# ever talked to B: it rebuilt the dataset from the recorded syscalls alone.
#
# MODE=race puts all the clients on the same keys.  Then two requests for one key
# can reach two threads within microseconds, and which one went first is not in
# the log: the replay may diverge, and says so (REPLAY.md, limitations).
#
# Environment: REPLAY_OPTS (default -T), REMOTE=host, SSH="ssh -i key", OUT, CLIENTS, OPS, MODE
set -e -o pipefail
cd "$(dirname "$0")"
DIR=$PWD
REPLAY_OPTS=${REPLAY_OPTS--T}       # worker threads share the hash table
OUT=${OUT:-/var/tmp/sysrr-memcached}
REMOTE=${REMOTE:-}
SSH=${SSH:-ssh}
CLIENTS=${CLIENTS:-4}
OPS=${OPS:-300}
MODE=${MODE:-work}
IMG=docker.io/library/memcached:1.6
SRC=rrsrc DST=rrdst
CS=/sys/devices/system/clocksource/clocksource0/current_clocksource
CONF=/etc/criu/default.conf

R() {
	if [ -n "$REMOTE" ]; then $SSH "$REMOTE" "cd '$DIR' && $*"; else bash -c "$*"; fi
}
put() { [ -z "$REMOTE" ] || sudo cat "$1" | $SSH "$REMOTE" "cat > '$1'"; }
mc() { sudo podman exec -i "$1" perl - "${@:2}" <mc_client.pl; }

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

echo "--- start memcached (on $(hostname))"
sudo podman run -d --name $SRC $IMG memcached -t 4 >/dev/null
for i in $(seq 50); do
	mc $SRC load 1 2>/dev/null && break
	sleep 0.1
done
PID=$(sudo podman inspect -f '{{.State.Pid}}' $SRC)
echo "memcached pid $PID, threads: $(ls /proc/$PID/task | wc -l)"

echo "--- record; 100 keys before the checkpoint"
sudo ../sysrr -p "$PID" -o "$OUT/rec.log" 2>"$OUT/sysrr.err" &
sleep 1
mc $SRC load 100
sleep 0.5
sudo podman container checkpoint --leave-running -e "$OUT/ckpt.tar.gz" $SRC >/dev/null || {
	# the reason is in dump.log, which goes away with the container: show it now
	echo "FAIL: checkpoint failed; CRIU says:"
	sudo grep -E "Error|Warn" "$(sudo podman inspect -f "{{.StaticDir}}" $SRC)/dump.log" | tail -8
	exit 1
}
echo "--- checkpoint taken; $CLIENTS clients x $OPS operations after it (generation 1)"
mc $SRC "$MODE" "$CLIENTS" "$OPS"
mc $SRC dump >"$OUT/dump_a.txt"
echo "live A: $(grep -c ^VALUE "$OUT/dump_a.txt") keys, md5 $(md5sum <"$OUT/dump_a.txt" | cut -c1-16)"
sleep 0.5
sudo podman pause $SRC >/dev/null
sleep 0.3
sudo pkill -INT -x sysrr
wait
sudo podman rm -f -t 0 $SRC >/dev/null
# (exits non-zero for threads caught mid-syscall at stop: not a failure of the test)
../sysrr_dump -s "$OUT/rec.log" | tail -4 || true

if [ -n "$REMOTE" ]; then
	echo "--- copy to $REMOTE: sysrr_replay, client, checkpoint, log"
	tar -C .. -cf - sysrr_replay tests/mc_client.pl | $SSH "$REMOTE" "tar -C '$DIR/..' -xf -"
	put "$OUT/ckpt.tar.gz"
	put "$OUT/rec.log"
	if ! $SSH "$REMOTE" "sudo podman image exists $IMG"; then
		echo "    image $IMG missing there: copying it"
		sudo podman save $IMG | $SSH "$REMOTE" "sudo podman load -q"
	fi
fi
WHERE=$(R hostname)
R "sudo podman run --rm $IMG true"      # load podman's AppArmor profile there (REPLAY.md)

echo "--- restore on $WHERE, left stopped"
PID2=$(R "echo leave-stopped | sudo tee $CONF >/dev/null;
	   sudo podman container restore --import '$OUT/ckpt.tar.gz' --name $DST >/dev/null; rc=\$?;
	   $RESTORE_CONF; [ \$rc = 0 ] && sudo podman inspect -f '{{.State.Pid}}' $DST")
echo "restored pid $PID2: $(R "grep State /proc/$PID2/status")"

echo "--- replay on $WHERE (-e live: B goes on for real after the recording)"
R "setsid -f sudo ../sysrr_replay $REPLAY_OPTS -p $PID2 -g 1 -e live '$OUT/rec.log' \
	>'$OUT/replay.raw' 2>&1 </dev/null"
for i in $(seq 450); do
	R "grep -qE 'end of the recording|DIVERGENCE|target exited|sysrr_replay:' '$OUT/replay.raw'" && break
	sleep 0.2
done
sleep 0.3
R "cat '$OUT/replay.raw'" >"$OUT/replay.txt"; grep -v '^#' "$OUT/replay.txt" | tail -25

fail=0
if grep -q 'end of the recording reached' "$OUT/replay.txt"; then
	R "sudo podman exec -i $DST perl - dump <mc_client.pl" >"$OUT/dump_b.txt" || true
	echo "replayed B: $(grep -c ^VALUE "$OUT/dump_b.txt") keys, md5 $(md5sum <"$OUT/dump_b.txt" | cut -c1-16)"
	cmp -s "$OUT/dump_a.txt" "$OUT/dump_b.txt" || { echo "FAIL: dataset differs"; fail=1; }
else
	echo "FAIL: replay did not reach the end"; fail=1
fi
grep -q DIVERGENCE "$OUT/replay.txt" && { echo "FAIL: divergence"; fail=1; }
[ $fail = 0 ] && echo "PASS: memcached on $WHERE rebuilt the live dataset from the recording"
exit $fail
