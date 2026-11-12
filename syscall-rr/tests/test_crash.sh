#!/bin/bash
# Does a replay die the way the live run died?
#
#   the target runs and is recorded      checkpoint at 2 s, the bug fires at about 3.2 s
#   the live run dies                    its exit code, signal, fault address and
#                                        instruction pointer are noted
#   restore; a second recorder watches the replay itself; replay
#   compare the two deaths
#
# Result line (last line of the output):
#   VERDICT same-death      same signal, code, fault address, instruction pointer
#   VERDICT same-exit       same exit code, nothing more to compare (no signal)
#   VERDICT same-fault      the same fault at the same address, then the crash
#                           handler went another way (it depends on signal timing)
#                           (or the live run's exit_group hid the fault: exit code 0)
#   VERDICT differs ...     died in another way, or not at all
#   VERDICT hung            did not end by itself
#
# Usage: tests/test_crash.sh ARGS...   (first word: mode of crash_target)
#   TARGET=name IMG=image OPTS=-T FOLLOW=-f KILL_AT=secs REMOTE=host SSH="ssh -i key"
#   BEFORE="command run in A after the checkpoint", e.g. to trigger the crash
#   Deadlock targets: KILL_AT stops A, since it would never end by itself.
set -o pipefail
cd "$(dirname "$0")"
DIR=$PWD
TARGET=${TARGET:-crash_target}
IMG=${IMG:-docker.io/library/debian:trixie-slim}
OPTS=${OPTS:--T}
FOLLOW=${FOLLOW:--f}
KILL_AT=${KILL_AT:-}
BEFORE=${BEFORE:-}
OUT=${OUT:-/var/tmp/sysrr-crash}
REMOTE=${REMOTE:-}
SSH=${SSH:-ssh}
SRC=crsrc DST=crdst
CS=/sys/devices/system/clocksource/clocksource0/current_clocksource
CONF=/etc/criu/default.conf

R() {
	if [ -n "$REMOTE" ]; then $SSH "$REMOTE" "cd '$DIR' && $*"; else bash -c "$*"; fi
}
put() { [ -z "$REMOTE" ] || sudo cat "$1" | $SSH "$REMOTE" "cat > '$1'"; }

rm -rf "$OUT"; mkdir -p "$OUT"
make -s -C .. all >/dev/null
old_cs=$(cat $CS)
[ -z "$REMOTE" ] || $SSH "$REMOTE" "rm -rf '$OUT'; mkdir -p '$OUT' '$DIR'"
r_old_cs=$(R "cat $CS")
cleanup() {
	echo "$old_cs" | sudo tee $CS >/dev/null
	sudo podman rm -f -t 0 $SRC >/dev/null 2>&1
	R "echo $r_old_cs | sudo tee $CS >/dev/null; sudo rm -f $CONF; sudo pkill -x sysrr_replay; sudo pkill -x sysrr;
	   sudo podman rm -f -t 0 $DST >/dev/null 2>&1; true"
}
trap cleanup EXIT
echo acpi_pm | sudo tee $CS >/dev/null
R "echo acpi_pm | sudo tee $CS >/dev/null; sudo podman rm -f -t 0 $DST >/dev/null 2>&1; true"
sudo podman rm -f -t 0 $SRC >/dev/null 2>&1

sudo podman run -d --name $SRC --image-volume ignore ${PODMAN_OPTS:-} -v "$DIR":/t:ro $IMG ${TARGET_CMD:-/t/$TARGET} "$@" >/dev/null || { echo "VERDICT setup: run failed"; exit 1; }
PID=$(sudo podman inspect -f '{{.State.Pid}}' $SRC)
sudo ../sysrr $FOLLOW -p "$PID" -o "$OUT/rec.log" 2>"$OUT/sysrr.err" &
sleep ${CKPT_AT:-2}
if ! sudo podman container checkpoint --leave-running -e "$OUT/ckpt.tar.gz" $SRC >"$OUT/ckpt.txt" 2>&1; then
	sudo grep -E "Error|Warn" "$(sudo podman inspect -f '{{.StaticDir}}' $SRC)/dump.log" 2>/dev/null | tail -8
	echo "VERDICT setup: checkpoint failed: $(head -c 150 "$OUT/ckpt.txt")"
	exit 1
fi
[ -z "$BEFORE" ] || sudo podman exec $SRC sh -c "$BEFORE" >"$OUT/before.txt" 2>&1
case "$1" in kill9) sleep 0.5; sudo kill -9 "$PID" ;; esac
if [ -n "$KILL_AT" ]; then
	sleep "$KILL_AT"
	sudo podman pause $SRC >/dev/null 2>&1
	sleep 0.5
	sudo pkill -INT -x sysrr
	LIVE_ENDED=no
else
	for i in $(seq 1 100); do
		[ "$(sudo podman inspect -f '{{.State.Running}}' $SRC 2>/dev/null)" = true ] || break
		sleep 0.1
	done
	LIVE_ENDED=yes
	sudo pkill -INT -x sysrr
fi
wait
live_code=$(sudo podman inspect -f '{{.State.ExitCode}}' $SRC)
sudo podman logs $SRC >"$OUT/live_out.txt" 2>&1
sudo podman rm -f -t 0 -v $SRC >/dev/null 2>&1

../sysrr_dump -g 1 "$OUT/rec.log" 2>/dev/null | grep -E '^==== thread' >"$OUT/live_signals.txt"
echo "--- live run: exit code $live_code"
tail -3 "$OUT/live_signals.txt"

if [ -n "$REMOTE" ]; then
	tar -C .. -cf - sysrr sysrr_replay sysrr_dump tests | $SSH "$REMOTE" "tar -C '$DIR/..' -xf -"
	put "$OUT/ckpt.tar.gz"; put "$OUT/rec.log"
	$SSH "$REMOTE" "sudo podman image exists $IMG" || sudo podman save $IMG | $SSH "$REMOTE" "sudo podman load -q" >/dev/null
fi
R "sudo podman run --rm --image-volume ignore $IMG true" >/dev/null 2>&1
if ! PID2=$(R "echo leave-stopped | sudo tee $CONF >/dev/null;
	sudo podman container restore --import '$OUT/ckpt.tar.gz' --name $DST >/dev/null 2>'$OUT/restore.txt'; rc=\$?;
	sudo rm -f $CONF; [ \$rc = 0 ] && sudo podman inspect -f '{{.State.Pid}}' $DST"); then
	echo "VERDICT setup: restore failed: $(R "head -c 150 '$OUT/restore.txt'")"
	exit 1
fi

# A second recorder watches the replayed process: it sees its real signals.
# (no open stdout/stderr left behind: ssh would wait for them to close)
R "sudo setsid -f sh -c '../sysrr -f -p $PID2 -o $OUT/replay_rec.log' </dev/null >/dev/null 2>&1"
sleep 1
echo "--- replay"
rc=0
R "timeout 60 sudo ../sysrr_replay $OPTS -p $PID2 -g 1 -e kill '$OUT/rec.log'" 2>&1 | tee "$OUT/replay.txt" |
	grep -vE '^ *warning: #' | tail -22 | cut -c1-170
rc=${PIPESTATUS[0]}
sleep 0.5
replay_code=$(R "sudo podman inspect -f '{{.State.ExitCode}}' $DST" 2>/dev/null)
R "sudo pkill -INT -x sysrr; sleep 0.5; ../sysrr_dump '$OUT/replay_rec.log' 2>/dev/null | grep -E '^==== thread'" >"$OUT/replay_signals.txt" 2>/dev/null
echo "--- replay's own signals"
tail -3 "$OUT/replay_signals.txt"
echo "--- exit code: live $live_code, replay $replay_code (replay tool exit $rc)"

# the fatal signal lines, without the generation number at the end
# the first one: when a thread crashes, its siblings get SIGKILL afterwards
fatal() { { grep FATAL "$1" | grep -v 'signal 9 ' || grep FATAL "$1"; } | head -1 | sed -E 's/ g[0-9]+ =+$//; s/^==== thread [0-9]+ //'; }
lf=$(fatal "$OUT/live_signals.txt"); rf=$(fatal "$OUT/replay_signals.txt")
# (the second recorder sees the injected listener as a checkpoint: generation 2)
# A default-action signal is turned into SIGKILL by the kernel: the real one is in
# the exit status.  An asynchronous signal has no address worth comparing, either.
ended() { grep -E 'ended: status' "$1" | grep -v 'signal 0,' | grep -v 'signal 9,' | head -1 | sed -E 's/ g[0-9]+ =+$//; s/^==== thread [0-9]+ //'; }
case "$lf" in *'signal 9 '*) l2=$(ended "$OUT/live_signals.txt"); [ -z "$l2" ] || { lf=$l2; rf=$(ended "$OUT/replay_signals.txt"); } ;; esac
faultline() { grep -E 'got signal (4|7|8|11) ' "$1" | head -1 | sed -E 's/, FATAL//; s/ g[0-9]+ =+$//; s/^==== thread [0-9]+ //'; }
lfault=$(faultline "$OUT/live_signals.txt"); rfault=$(faultline "$OUT/replay_signals.txt")
echo "live:   ${lf:-(no fatal signal)}"
echo "replay: ${rf:-(no fatal signal)}"
if [ "$rc" = 124 ]; then
	echo "VERDICT hung"
elif grep -q 'no recorded syscall was used' "$OUT/replay.txt" && { [ -z "$lfault" ] || [ "$lfault" != "$rfault" ]; }; then
	echo "VERDICT differs: reported no progress"
elif [ -n "$KILL_AT" ]; then
	if grep -q 'end of the recording reached' "$OUT/replay.txt"; then
		echo "VERDICT same-state: reached the end of the recording"
	else
		echo "VERDICT differs: $(grep -m1 -E 'DIVERGENCE' "$OUT/replay.txt" | cut -c1-90)"
	fi
elif [ -n "$lf" ] && [ "$lf" = "$rf" ] && [ "$live_code" = "$replay_code" ]; then
	echo "VERDICT same-death"
elif [ -n "$lf" ] && [ "$live_code" = "$replay_code" ]; then
	echo "VERDICT same-signal: other details differ"
elif [ -z "$lf" ] && [ "$live_code" = "$replay_code" ] && ! grep -q DIVERGENCE "$OUT/replay.txt"; then
	echo "VERDICT same-exit"
elif [ -n "$lfault" ] && [ "$lfault" = "$rfault" ]; then
	if [ "$live_code" = 0 ]; then
		echo "VERDICT same-fault: the live exit_group hid it (exit code 0)"
	else
		echo "VERDICT same-fault: then $(grep -m1 -E 'DIVERGENCE' "$OUT/replay.txt" | cut -c4-60)"
	fi
else
	why=$(grep -m1 -E 'DIVERGENCE|without that fault' "$OUT/replay.txt" | cut -c1-90)
	echo "VERDICT differs: exit $live_code vs ${replay_code:-?}; ${why:-no report}"
fi
