#!/bin/bash
# Record + checkpoint + restore + replay of a real server: redis 7.
#
#   container A: redis-server (main thread + background threads, epoll loop)
#   load 200 keys                         gen 0 (state inside the checkpoint)
#   podman checkpoint --leave-running     gen 1 starts
#   redis-benchmark: 20 clients, SET/INCR/LPUSH/GET on 1000 random keys,
#   plus a few redis-cli commands         all inside A, all in gen 1
#   DEBUG DIGEST of A                     fingerprint of the whole dataset
#   podman pause A, stop recording
#   restore the checkpoint as B (left stopped), sysrr_replay -g 1 -e live
#   when the recording is used up, B runs live: DEBUG DIGEST of B
#
# PASS if the replay ends without divergence and B's digest equals A's:
# B rebuilt A's dataset from the recorded syscalls alone (no client ever
# talked to B; every accept/read of those clients is faked).
#
# Environment: REPLAY_OPTS (default -S; REPLAY_OPTS= for none), REMOTE=host, SSH="ssh -i key", OUT, N
set -e -o pipefail
cd "$(dirname "$0")"
DIR=$PWD
REPLAY_OPTS=${REPLAY_OPTS--S}       # strict order by default: redis has 6 threads
OUT=${OUT:-/var/tmp/sysrr-redis}
REMOTE=${REMOTE:-}
SSH=${SSH:-ssh}
N=${N:-3000}
IMG=docker.io/library/redis:7
SRC=rrsrc DST=rrdst
CS=/sys/devices/system/clocksource/clocksource0/current_clocksource
CONF=/etc/criu/default.conf
CLI="redis-cli --no-raw"

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
	   $RESTORE_CONF; v=\$(sudo podman inspect -f '{{range .Mounts}}{{.Name}} {{end}}' $DST 2>/dev/null);
	   sudo podman rm -f -v -t 0 $DST >/dev/null 2>&1; [ -n \"\$v\" ] && sudo podman volume rm \$v >/dev/null 2>&1;
	   true" || true    # a restore recreates /data as a *named* volume: rm -v keeps it
}
trap cleanup EXIT
echo acpi_pm | sudo tee $CS >/dev/null
[ -z "$REMOTE" ] || $SSH "$REMOTE" "rm -rf '$OUT'"
R "$SETUP"
sudo podman rm -f -v -t 0 $SRC >/dev/null 2>&1 || true
R "sudo podman rm -f -v -t 0 $DST >/dev/null 2>&1; true"

echo "--- start redis (on $(hostname))"
sudo podman run -d --name $SRC $IMG redis-server --save '' --appendonly no \
	--enable-debug-command local >/dev/null
for i in $(seq 50); do
	sudo podman exec $SRC $CLI ping 2>/dev/null | grep -q PONG && break
	sleep 0.1
done
PID=$(sudo podman inspect -f '{{.State.Pid}}' $SRC)
echo "redis pid $PID, threads: $(ls /proc/$PID/task | wc -l) ($(cat /proc/$PID/task/*/comm | tr '\n' ' '))"

echo "--- record; 200 keys before the checkpoint"
sudo ../sysrr -p "$PID" -o "$OUT/rec.log" 2>"$OUT/sysrr.err" &
sleep 1
sudo podman exec $SRC sh -c 'for i in $(seq 200); do echo "SET pre:$i v$i"; done | redis-cli >/dev/null'
sudo podman exec $SRC $CLI set phase before-checkpoint >/dev/null
sleep 0.5
sudo podman container checkpoint --leave-running -e "$OUT/ckpt.tar.gz" $SRC >/dev/null
echo "--- checkpoint taken; workload after it (generation 1)"
sudo podman exec $SRC redis-benchmark -q -n $N -r 1000 -c 20 -t set,incr,lpush,get 2>&1 \
	| tr '\r' '\n' | grep -E 'requests per second' || true
sudo podman exec $SRC $CLI set phase after-checkpoint >/dev/null
sudo podman exec $SRC $CLI incrby counter 42 >/dev/null
sudo podman exec $SRC $CLI del pre:7 >/dev/null
DIG_A=$(sudo podman exec $SRC $CLI debug digest)
DB_A=$(sudo podman exec $SRC $CLI dbsize)
echo "live A: dbsize $DB_A, digest $DIG_A"
sleep 0.5
sudo podman pause $SRC >/dev/null
sleep 0.3
sudo pkill -INT -x sysrr
wait
sudo podman rm -f -v -t 0 $SRC >/dev/null
# (exits non-zero for threads caught mid-syscall at stop: not a failure of the test)
../sysrr_dump -s "$OUT/rec.log" | tail -4 || true

if [ -n "$REMOTE" ]; then
	echo "--- copy to $REMOTE: sysrr_replay, checkpoint, log"
	tar -C .. -cf - sysrr_replay | $SSH "$REMOTE" "tar -C '$DIR/..' -xf -"
	put "$OUT/ckpt.tar.gz"
	put "$OUT/rec.log"
	if ! $SSH "$REMOTE" "sudo podman image exists $IMG"; then
		echo "    image $IMG missing there: copying it"
		sudo podman save $IMG | $SSH "$REMOTE" "sudo podman load -q"
	fi
fi
WHERE=$(R hostname)
# load podman's AppArmor profile there (see REPLAY.md, "AppArmor")
R "sudo podman run --rm --image-volume ignore $IMG true"   # (no leftover /data volume)

echo "--- restore on $WHERE, left stopped"
PID2=$(R "echo leave-stopped | sudo tee $CONF >/dev/null;
	   sudo podman container restore --import '$OUT/ckpt.tar.gz' --name $DST >/dev/null; rc=\$?;
	   $RESTORE_CONF; [ \$rc = 0 ] && sudo podman inspect -f '{{.State.Pid}}' $DST")
echo "restored pid $PID2: $(R "grep State /proc/$PID2/status")"

echo "--- replay on $WHERE (-e live: B goes on for real after the recording)"
# setsid -f: fully detached, so ssh does not wait for the (never ending)
# live replay; a plain "cmd >file &" kept ssh's channel open
R "setsid -f sudo ../sysrr_replay $REPLAY_OPTS -p $PID2 -g 1 -e live '$OUT/rec.log' \
	>'$OUT/replay.raw' 2>&1 </dev/null"
for i in $(seq 300); do
	R "grep -qE 'end of the recording|DIVERGENCE|target exited|sysrr_replay:' '$OUT/replay.raw'" && break
	sleep 0.2
done
sleep 0.3
R "cat '$OUT/replay.raw'" >"$OUT/replay.txt"; grep -v '^#' "$OUT/replay.txt" | tail -25

fail=0
if grep -q 'end of the recording reached' "$OUT/replay.txt"; then
	DIG_B=$(R "sudo podman exec $DST $CLI debug digest")
	DB_B=$(R "sudo podman exec $DST $CLI dbsize")
	PH_B=$(R "sudo podman exec $DST $CLI get phase")
	echo "replayed B: dbsize $DB_B, digest $DIG_B, phase $PH_B"
	[ "$DIG_A" = "$DIG_B" ] || { echo "FAIL: digest differs"; fail=1; }
	[ "$DB_A" = "$DB_B" ] || { echo "FAIL: dbsize differs"; fail=1; }
else
	echo "FAIL: replay did not reach the end"; fail=1
fi
grep -q DIVERGENCE "$OUT/replay.txt" && { echo "FAIL: divergence"; fail=1; }
[ $fail = 0 ] && echo "PASS: redis on $WHERE rebuilt the live dataset from the recording (digest $DIG_B)"
exit $fail
