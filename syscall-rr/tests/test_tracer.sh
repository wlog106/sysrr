#!/bin/bash
# test_tracer.sh: a tracer that is not CRIU must not look like a checkpoint.
#
# Records a looping shell, attaches strace to it for a second, and checks the
# log: no CRIU markers, no syscall flagged as a parasite call (F_DUMP), one
# generation.  Then the same with sysrr -t, which must count strace again
# (that proves the test can see the difference).  Record side only, no
# container needed.
set -u
cd "$(dirname "$0")/.."
command -v strace >/dev/null || { echo "SKIP: strace not installed"; exit 0; }
W=$(mktemp -d /var/tmp/sysrr-tracer.XXXXXX)
fail=0

run() {         # $1: extra sysrr options; prints sysrr_dump -s
	bash -c 'while :; do sleep 0.1; done' & local t=$!
	sudo ./sysrr $1 -p $t -o "$W/rec.log" 2>"$W/sysrr.err" & local r=$!
	sleep 1
	sudo timeout 1 strace -p $t -o /dev/null 2>/dev/null
	sleep 1
	sudo kill -INT $r; wait $r
	kill $t; wait $t 2>/dev/null
	./sysrr_dump -s "$W/rec.log"
}

out=$(run "")
echo "== sysrr (default)"; echo "$out" | grep -E 'gen [0-9]|generations'
if echo "$out" | grep -q 'gen 1:'; then echo "FAIL: strace started a generation"; fail=1; fi
if ! echo "$out" | grep -q '(0 by criu parasite)'; then echo "FAIL: syscalls flagged as CRIU's"; fail=1; fi
if grep -q marker "$W/sysrr.err"; then echo "FAIL: CRIU markers written"; fail=1; fi

out=$(run "-t")
echo "== sysrr -t"; echo "$out" | grep -E 'gen [0-9]|generations'
if ! echo "$out" | grep -q 'gen 1:'; then echo "FAIL: with -t, strace should count as a checkpoint"; fail=1; fi

rm -rf "$W"
[ $fail = 0 ] && echo "PASS: only CRIU's ptrace counts; -t counts any tracer"
exit $fail
