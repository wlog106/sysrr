#!/bin/bash
# End-to-end test of record + replay:
#
#   container A runs the target              sysrr records it
#   podman checkpoint --leave-running       generation 1 starts
#   A keeps running ~2 s, then is frozen    (podman pause: no more syscalls)
#   recording stops                         gen 1 = everything A did after the checkpoint
#   restore the checkpoint as container B, left stopped by CRIU
#   sysrr_replay -g 1 drives B through gen 1
#
# PASS if the replay reports no divergence, reaches the end of the recording,
# and what B wrote to stdout equals the last lines A printed.
#
# Then a negative test, which must end in a divergence:
#   ckpt_target  replay a copy of the log with one byte of a recorded read()
#                flipped: the target computes a different line, the replay
#                stops at the write() that follows
#   fd_target    replay with -N (no fd installation): the real mmap() of a
#                faked open() fails with EBADF, the line differs
#
# Options (environment):
#   TARGET=mt_target REPLAY_OPTS=-S    another target / replay options
#   REMOTE=host                        restore and replay on another machine
#                                      (over ssh, passwordless sudo there).
#                                      The tests dir is copied to the same
#                                      path (the container bind-mounts it),
#                                      the image is copied if missing.
#   SSH="ssh -i key"                   ssh command for REMOTE
#   SYSRR_OPTS=-f                      recorder options (-f: follow children)
#   ARGS="crash"                       arguments for the target
#   EXPECT=diverge                     the replay must report a divergence
#                                      (a known limitation), not hang or pass
#   EXPECT=honest                      it may pass, or report a divergence, but it
#                                      must end by itself (timing-dependent target)
#   KEEP_GOING=1                       sc_target: run through every mismatch; pass
#                                      if the lines that differ are all listed
#                                      in sc_known.txt
#   KICK=USR1+TERM                     send the target these signals after the
#                                      checkpoint, four times (podman kill)
#   CKPT_KICK=USR1                     send the target this signal from a CRIU action
#                                      script at post-dump (tests/ckpt_kick.sh):
#                                      it lands while the process is frozen
#   SEARCH=40                          with -T: if the replay diverges, look for an
#                                      order of threads that fits the whole
#                                      recording (sysrr_search.sh, at most 40
#                                      replays) and compare the replay made with it
#   CKPT_OPTS=--tcp-established        options for the checkpoint and the restore.
#                                      Without this one CRIU refuses a checkpoint
#                                      that lands while a TCP connection is open
#                                      ("checkpointing failed -52").
#   IMG=docker.io/library/python:3.12-slim   another image
#
# The clocksource is switched to acpi_pm for the run, on both machines, so
# that clock_gettime is a real syscall (DESIGN.md, "vDSO calls are not
# syscalls").
set -e -o pipefail
cd "$(dirname "$0")"
DIR=$PWD
TARGET=${TARGET:-ckpt_target}
REPLAY_OPTS=${REPLAY_OPTS:-}
SYSRR_OPTS=${SYSRR_OPTS:-}
ARGS=${ARGS:-}
EXPECT=${EXPECT:-end}
KEEP_GOING=${KEEP_GOING:-}
KICK=${KICK:-}
CKPT_KICK=${CKPT_KICK:-}
SEARCH=${SEARCH:-}
CKPT_OPTS=${CKPT_OPTS:-}
END_OPT="-e kill"
OUT=${OUT:-/var/tmp/sysrr-replay}
REMOTE=${REMOTE:-}
SSH=${SSH:-ssh}
IMG=${IMG:-docker.io/library/debian:trixie-slim}
SRC=rrsrc DST=rrdst BAD=rrbad
# podman refuses --tcp-established together with --name: the copy then keeps the
# original's name (the original is removed before the restore)
KEEPNAME=
case "$CKPT_OPTS" in *tcp-established*) DST=$SRC BAD=$SRC KEEPNAME=1 ;; esac
CS=/sys/devices/system/clocksource/clocksource0/current_clocksource
CONF=/etc/criu/default.conf

# R: run a command on the replay machine (here, or $REMOTE in the same dir)
R() {
	if [ -n "$REMOTE" ]; then $SSH "$REMOTE" "cd '$DIR' && $*"; else bash -c "$*"; fi
}
put() { [ -z "$REMOTE" ] || sudo cat "$1" | $SSH "$REMOTE" "cat > '$1'"; }   # copy file there (ckpt is root's)
get() { [ -z "$REMOTE" ] || R "cat '$1'" >"$1"; }                   # and back

rm -rf "$OUT"
mkdir -p "$OUT"
make -s -C .. all

# replay machine setup / cleanup (same machine when REMOTE is empty)
SETUP="echo acpi_pm | sudo tee $CS >/dev/null; mkdir -p '$OUT'; sudo rm -f '$OUT/criu.conf.bak';
       [ -e $CONF ] && sudo cp $CONF '$OUT/criu.conf.bak'; sudo mkdir -p /etc/criu; true"
RESTORE_CONF="if [ -e '$OUT/criu.conf.bak' ]; then sudo cp '$OUT/criu.conf.bak' $CONF; else sudo rm -f $CONF; fi"

[ -z "$REMOTE" ] || $SSH "$REMOTE" "mkdir -p '$DIR'"
old_cs=$(cat $CS)
r_old_cs=$(R "cat $CS")
# CKPT_KICK: put this machine's CRIU config back (also after an interrupted run)
KICK_BAK=/run/sysrr-kick.criu.conf
put_back_rec_conf() {
	if [ -e $KICK_BAK ]; then sudo cp -p $KICK_BAK $CONF; else sudo rm -f $CONF; fi
	sudo rm -f /run/sysrr-kick $KICK_BAK
}
[ ! -e /run/sysrr-kick ] || put_back_rec_conf     # left by an interrupted run
cleanup() {
	[ ! -e /run/sysrr-kick ] || put_back_rec_conf     # interrupted during a CKPT_KICK checkpoint
	echo "$old_cs" | sudo tee $CS >/dev/null
	sudo podman rm -f -t 0 $SRC >/dev/null 2>&1 || true
	R "echo $r_old_cs | sudo tee $CS >/dev/null; $RESTORE_CONF;
	   sudo podman rm -f -t 0 $DST $BAD >/dev/null 2>&1; true" || true
}
trap cleanup EXIT
echo acpi_pm | sudo tee $CS >/dev/null
[ -z "$REMOTE" ] || $SSH "$REMOTE" "rm -rf '$OUT'"
R "$SETUP"
sudo podman rm -f -t 0 $SRC >/dev/null 2>&1 || true
R "sudo podman rm -f -t 0 $DST $BAD >/dev/null 2>&1; true"

echo "--- record (on $(hostname))"
# /scratch: a tmpfs for targets that make files.  CRIU saves it during the dump,
# while the target is frozen; /dev/shm and the container's own layer are archived
# by podman afterwards, with the target running again, and files that change
# meanwhile break the checkpoint.
sudo podman run -d --name $SRC --tmpfs /scratch -v "$DIR":/t:ro $IMG /t/$TARGET $ARGS >/dev/null
PID=$(sudo podman inspect -f '{{.State.Pid}}' $SRC)
# -C NAME records the whole container, as -p PID -f does: use it for those
case " $SYSRR_OPTS " in
*" -f "*) sudo ../sysrr ${SYSRR_OPTS/-f/} -C $SRC -o "$OUT/rec.log" 2>"$OUT/sysrr.err" & ;;
*) sudo ../sysrr $SYSRR_OPTS -p "$PID" -o "$OUT/rec.log" 2>"$OUT/sysrr.err" & ;;
esac
SYSRR_PID=$!                    # (sudo; it passes SIGINT on to our sysrr, and only ours)
sleep 2
if [ -n "$CKPT_KICK" ]; then
	# the hook goes into this machine's CRIU config for the checkpoint only;
	# the original is kept next to /run/sysrr-kick (not in $OUT, which the next
	# run empties), so an interrupted run can still put it back
	sudo rm -f $KICK_BAK
	[ -e $CONF ] && sudo cp -p $CONF $KICK_BAK
	sudo mkdir -p /etc/criu
	{ [ -e $KICK_BAK ] && cat $KICK_BAK; echo "action-script $DIR/ckpt_kick.sh"; } |
		sudo tee $CONF.new >/dev/null
	sudo mv $CONF.new $CONF
	echo "$PID $CKPT_KICK" | sudo tee /run/sysrr-kick >/dev/null
fi
# A checkpoint that lands between a vfork and its exec fails after CRIU's 10 s
# timeout: the parent sleeps (state D) until the child execs, and CRIU has
# stopped the child ("Unseizable non-zombie ... state D").  Try again.  A failed
# attempt still starts a generation in the log (CRIU seized and released the
# threads), so the replay uses the generation of the attempt that worked.
GEN=0
for attempt in 1 2 3; do
	GEN=$attempt ckpt_rc=0
	sudo podman container checkpoint --leave-running $CKPT_OPTS -e "$OUT/ckpt.tar.gz" $SRC \
		>"$OUT/ckpt.txt" 2>&1 || ckpt_rc=$?
	[ $ckpt_rc != 0 ] || break
	D=$(sudo podman inspect -f '{{.StaticDir}}' $SRC 2>/dev/null)
	sudo grep -q 'Unseizable' "$D/dump.log" 2>/dev/null || break
	echo "checkpoint attempt $attempt failed: it caught a vfork before its exec; again"
done
[ $GEN = 1 ] || echo "replaying generation $GEN (one per checkpoint attempt)"
[ -z "$CKPT_KICK" ] || put_back_rec_conf
if [ $ckpt_rc != 0 ]; then
	cat "$OUT/ckpt.txt"
	# "CRIU checkpointing failed -52" covers every cause: the reason is in dump.log,
	# which goes away with the container, so show it now
	D=$(sudo podman inspect -f '{{.StaticDir}}' $SRC 2>/dev/null)
	echo "FAIL: checkpoint failed; CRIU says:"
	sudo grep -E 'Error|Warn' "$D/dump.log" 2>/dev/null | tail -8
	sudo cp "$D/dump.log" "$OUT/dump.log" 2>/dev/null && sudo chmod a+r "$OUT/dump.log"
	sudo kill -INT $SYSRR_PID 2>/dev/null || true
	exit 1
fi
if [ -n "$KICK" ]; then         # signals from outside, while generation 1 is recorded
	for i in 1 2 3 4; do
		for sig in ${KICK//+/ }; do sudo podman kill -s "$sig" $SRC >/dev/null 2>&1 || true; done
	done
fi
sleep 2
sudo podman pause $SRC >/dev/null 2>&1 || true   # freeze A: its log now ends where the recording ends
                                            # (exit_target has already ended by itself)
sleep 0.5
sudo kill -INT $SYSRR_PID 2>/dev/null || true   # (it stops by itself if the target ended)
wait || true
sudo podman logs $SRC >"$OUT/live_all.txt"
sudo podman rm -f -t 0 $SRC >/dev/null
# (exits non-zero for threads caught mid-syscall at stop: not a failure of the test)
../sysrr_dump -s "$OUT/rec.log" | tail -4 || true

if [ -n "$REMOTE" ]; then
	echo "--- copy to $REMOTE: tests dir + sysrr_replay (same path), checkpoint, log"
	tar -C .. -cf - sysrr_replay sysrr_search.sh sysrr_restore.sh tests | $SSH "$REMOTE" "tar -C '$DIR/..' -xf -"
	put "$OUT/ckpt.tar.gz"
	put "$OUT/rec.log"
	if ! $SSH "$REMOTE" "sudo podman image exists $IMG"; then
		echo "    image $IMG missing there: copying it"
		sudo podman save $IMG | $SSH "$REMOTE" "sudo podman load -q"
	fi
fi
WHERE=$(R hostname)
# podman loads its AppArmor profile (containers-default-*) when it *starts*
# a container, not on restore.  On a machine that has not run one since boot
# the restored task cannot switch to the profile and CRIU fails with
# "can't write lsm profile -2".  One throwaway container loads it.
R "sudo podman run --rm $IMG true"

# restore the checkpoint as container $1, left stopped by CRIU (sysrr_restore.sh,
# as in the README); prints its pid
restore_stopped() {
	local name=$1
	[ -z "$KEEPNAME" ] || name=
	R "sudo ../sysrr_restore.sh ${CKPT_OPTS:+-c '$CKPT_OPTS'} '$OUT/ckpt.tar.gz' $name" |
		tee -a "$OUT/restore.txt" | sed -n 's/^restored .* pid \([0-9]*\) .*/\1/p'
}

echo "--- restore on $WHERE, left stopped"
PID2=$(restore_stopped $DST) && [ -n "$PID2" ] || { echo "FAIL: restore failed (see above)"; exit 1; }
echo "restored pid $PID2: $(R "grep State /proc/$PID2/status")"

echo "--- replay on $WHERE"
rc=0
R "timeout 60 sudo ../sysrr_replay $REPLAY_OPTS -C $DST -g $GEN $END_OPT -w '$OUT/replay_out.txt' '$OUT/rec.log'" \
	| tee "$OUT/replay.txt" || rc=$?
get "$OUT/replay_out.txt"
FLIPS=
if [ -n "$SEARCH" ] && [ "$SEARCH" != 0 ] && [ $rc = 1 ] && grep -q DIVERGENCE "$OUT/replay.txt"; then
	# With -T, which thread went first where two overlapped is a guess: look
	# for an order of threads that fits the whole recording (sysrr_search.sh)
	echo "--- diverged: searching for an order of threads that fits the recording"
	R "sudo podman rm -f -t 0 $DST >/dev/null 2>&1; ../sysrr_search.sh -n $SEARCH -g $GEN \
	   -o '$(echo $REPLAY_OPTS | sed 's/-T//')' ${CKPT_OPTS:+-c '$CKPT_OPTS'} \
	   -i '$OUT/ckpt.tar.gz' '$OUT/rec.log'" | tee "$OUT/search.txt" | tail -3 || true
	FLIPS=$(sed -n 's/^FOUND -F \([0-9,]*\).*/\1/p' "$OUT/search.txt")
	[ "$FLIPS" != none ] || FLIPS=
	if grep -q '^FOUND' "$OUT/search.txt"; then
		echo "--- replay again${FLIPS:+ with -F $FLIPS}"
		PID2=$(restore_stopped $DST) && [ -n "$PID2" ] || { echo "FAIL: restore failed (see above)"; exit 1; }
		rc=0
		R "timeout 60 sudo ../sysrr_replay $REPLAY_OPTS ${FLIPS:+-F $FLIPS} -p $PID2 -g $GEN $END_OPT \
		   -w '$OUT/replay_out.txt' '$OUT/rec.log'" >"$OUT/replay.txt" || rc=$?
		tail -4 "$OUT/replay.txt"
		get "$OUT/replay_out.txt"
	fi
fi

echo "--- compare"
n=$(wc -l <"$OUT/replay_out.txt")
tail -n "$n" "$OUT/live_all.txt" >"$OUT/live_tail.txt"
echo "the replayed process wrote $n lines; live process, last $n lines:"
[ -n "$KEEP_GOING" ] || [ "$n" -gt 40 ] || paste -d'|' "$OUT/live_tail.txt" "$OUT/replay_out.txt" | sed 's/|/   |   /'
fail=0
if [ "$EXPECT" = honest ] && [ $rc = 1 ] && grep -q 'DIVERGENCE' "$OUT/replay.txt"; then
	echo "PASS: known limitation, reported as a divergence:"
	grep -m1 -A2 'DIVERGENCE' "$OUT/replay.txt"
	R "sudo podman rm -f -t 0 $DST >/dev/null"
	exit 0
fi
if [ "$EXPECT" = diverge ]; then
	# a known limitation: what matters is that the tool says so
	if [ $rc = 1 ] && grep -q 'DIVERGENCE' "$OUT/replay.txt"; then
		echo "PASS: divergence reported, as expected for this target"
	else
		echo "FAIL: expected a reported divergence, got exit $rc"; fail=1
	fi
	R "sudo podman rm -f -t 0 $DST >/dev/null"
	exit $fail
fi
if [ -n "$KEEP_GOING" ]; then
	# The write() input check names each line that differs.  (Comparing the
	# captured files would not do: a line of another length is cut at the
	# recorded length and merges with the next one.)
	grep -oE '^ +recorded +"[a-z_0-9]+' "$OUT/replay.txt" | awk -F'"' '{ print $2 }' | sort -u >"$OUT/differ.txt"
	grep -vE '^(#|$)' sc_known.txt | sort -u >"$OUT/known.txt"
	echo "$(grep -c ret= "$OUT/replay_out.txt") lines; cases that differ: $(tr '\n' ' ' <"$OUT/differ.txt")"
	new=$(comm -23 "$OUT/differ.txt" "$OUT/known.txt")
	fixed=$(comm -13 "$OUT/differ.txt" "$OUT/known.txt")
	[ -z "$new" ] || { echo "FAIL: new gaps: $new"; fail=1; }
	[ -z "$fixed" ] || echo "note: listed in sc_known.txt but no longer differing: $fixed"
	cmp() { true; }           # the input check above replaces the file comparison
fi
if [ $TARGET = exit_target ]; then
	# the target ends inside the recording: the replay must follow it there,
	# use every record and end the same way (7 = exit code, 139 = SIGSEGV)
	grep -q 'the target exited; 0 records were not used' "$OUT/replay.txt" || { echo "FAIL: records left at exit"; fail=1; }
	want=7; [ -z "$ARGS" ] || want=139
	got=$(R "sudo podman inspect -f '{{.State.ExitCode}}' $DST")
	[ "$got" = "$want" ] && echo "exit code $got, as in the live run" || { echo "FAIL: exit code $got, wanted $want"; fail=1; }
else
	grep -q 'end of the recording reached' "$OUT/replay.txt" || { echo "FAIL: end not reached"; fail=1; }
fi
[ "$n" -gt 0 ] || { echo "FAIL: no output"; fail=1; }
if ! cmp -s "$OUT/live_tail.txt" "$OUT/replay_out.txt"; then
	echo "FAIL: output differs (- live, + replay):"
	diff "$OUT/live_tail.txt" "$OUT/replay_out.txt" | head -12 || true
	fail=1
fi
[ $fail = 0 ] && echo "PASS: $n lines reproduced on $WHERE, no divergence${FLIPS:+ (order found by search: -F $FLIPS)}"
R "sudo podman rm -f -t 0 $DST >/dev/null"

case $TARGET in
ckpt_target)
	echo
	echo "--- negative test: replay a log with one flipped byte"
	./tamper_log.py "$OUT/rec.log" "$OUT/bad.log" $GEN 3
	put "$OUT/bad.log"
	LOG=$OUT/bad.log NEG_OPTS=
	;;
fd_target)
	echo
	echo "--- negative test: replay without fd installation (-N)"
	LOG=$OUT/rec.log NEG_OPTS=-N
	;;
*)
	exit $fail
esac
PID3=$(restore_stopped $BAD) && [ -n "$PID3" ] || { echo "FAIL: restore failed (see above)"; exit 1; }
if [ $TARGET = fd_target ] && R "sudo test -e /proc/$PID3/fd/3"; then
	# the checkpoint fell between open() and close(): fd 3 was restored for
	# real, so even without fd installation the mmap works
	echo "SKIP: the checkpoint was taken while fd 3 was open (inside an iteration)"
	exit $fail
fi
rc=0
R "sudo ../sysrr_replay $NEG_OPTS -p $PID3 -g $GEN -e kill -w '$OUT/bad_out.txt' '$LOG'" \
	| tee "$OUT/bad.txt" || rc=$?
get "$OUT/bad_out.txt"
nb=$(wc -l <"$OUT/bad_out.txt")
[ $rc = 1 ] || { echo "FAIL: expected exit 1 (diverged), got $rc"; fail=1; }
grep -q 'DIVERGENCE.*input buffer differs' "$OUT/bad.txt" &&
	grep -q 'expected  write(' "$OUT/bad.txt" || { echo "FAIL: write divergence not reported"; fail=1; }
if [ $TARGET = ckpt_target ]; then
	[ "$nb" = 3 ] || { echo "FAIL: expected 3 good lines before the divergence, got $nb"; fail=1; }
	[ $fail = 0 ] && echo "PASS: tampered read caught at the next write, after $nb good lines"
else
	grep -q 'replayed .*map=EBADF' "$OUT/bad.txt" || { echo "FAIL: no map=EBADF"; fail=1; }
	[ $fail = 0 ] && echo "PASS: without fds, mmap failed (EBADF) and the replay diverged after $nb good lines"
fi
exit $fail
