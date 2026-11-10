#!/bin/bash
# The whole test suite.  Every target goes through the same cycle: record,
# checkpoint, restore left stopped, replay, compare with the live run.
#
#   tests/run_all.sh                 all tests, here
#   tests/run_all.sh fork redis      only tests whose name contains a word
#   REMOTE=claude-test SSH="ssh -i ~/.ssh/to-test" tests/run_all.sh
#                                    restore and replay on another machine
#   ROUNDS=5 tests/run_all.sh fork   repeat, to catch flaky behaviour
#
# Logs: /var/tmp/sysrr-suite/<test>.<round>.log.  Exit code: failed tests.
cd "$(dirname "$0")"
LOGS=${LOGS:-/var/tmp/sysrr-suite}
ROUNDS=${ROUNDS:-1}
PY=docker.io/library/python:3.12-slim
mkdir -p "$LOGS"
total=0 failed=0
failures=()

# name | what it proves | environment and script
TESTS=(
"basic      |read, write, sleep; a tampered log must be caught |TARGET=ckpt_target test_replay.sh"
"threads    |two threads sharing memory, in recorded order     |TARGET=mt_target REPLAY_OPTS=-T test_replay.sh"
"fds        |files, sockets and pipes opened after a checkpoint|TARGET=fd_target test_replay.sh"
"syscalls   |every syscall's output, about 100 cases           |TARGET=sc_target REPLAY_OPTS=-c KEEP_GOING=1 test_replay.sh"
"fork       |hundreds of new processes keep their recorded pids|TARGET=fork_target SYSRR_OPTS=-f REPLAY_OPTS=-S test_replay.sh"
"thread-new |hundreds of new threads keep their recorded ids   |TARGET=thread_target REPLAY_OPTS=-S test_replay.sh"
"exit       |target ends by itself inside the recording        |TARGET=exit_target test_replay.sh"
"crash      |target crashes inside the recording               |TARGET=exit_target ARGS=crash test_replay.sh"
"signal     |async signal: must be reported, not hang or pass  |TARGET=signal_target EXPECT=diverge test_replay.sh"
"vdso-random|glibc getrandom makes no syscall: must end honestly |TARGET=rand_target EXPECT=honest test_replay.sh"
"shell      |dash: depends on SIGCHLD timing, must never hang  |TARGET=sh_target.sh SYSRR_OPTS=-f REPLAY_OPTS=-T EXPECT=honest test_replay.sh"
"python     |subprocess, C extensions loaded late, SQLite      |TARGET=py_target.py IMG=$PY SYSRR_OPTS=-f REPLAY_OPTS=-T test_replay.sh"
"redis      |real server: dataset digest identical             |test_redis.sh"
"nginx      |real server, 3 processes: logs identical          |test_nginx.sh"
)

selected() {
	[ $# -eq 1 ] && return 0
	local name=$1; shift
	for w; do [[ $name == *$w* ]] && return 0; done
	return 1
}

printf '%-11s %-52s %s\n' TEST 'WHAT IT PROVES' RESULT
for t in "${TESTS[@]}"; do
	IFS='|' read -r name what cmd <<<"$t"
	name=${name// /}
	selected "$name" "$@" || continue
	pass=0
	for ((r = 1; r <= ROUNDS; r++)); do
		log=$LOGS/$name.$r.log
		read -r -a words <<<"$cmd"
		script=./${words[-1]}
		unset 'words[-1]'
		if timeout 300 env "${words[@]}" "$script" >"$log" 2>&1; then
			pass=$((pass + 1))
		else
			failures+=("$log")
		fi
	done
	total=$((total + 1))
	if [ "$pass" = "$ROUNDS" ]; then res=PASS; else res="FAIL ($((ROUNDS - pass)) of $ROUNDS)"; failed=$((failed + 1)); fi
	[ "$ROUNDS" = 1 ] || [ "$pass" != "$ROUNDS" ] || res="PASS ($ROUNDS of $ROUNDS)"
	printf '%-11s %-52s %s\n' "$name" "$what" "$res"
done
echo
echo "$((total - failed)) of $total tests passed on ${REMOTE:-this machine}"
for f in "${failures[@]}"; do echo "  failed log: $f"; done
exit "$failed"
