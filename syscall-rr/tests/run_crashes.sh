#!/bin/bash
# Crash catalogue: one line per way to die.  See test_crash.sh for the verdicts.
#
#   tests/run_crashes.sh                 everything, on this machine
#   tests/run_crashes.sh null race       only names containing a word
#   ROUNDS=5 tests/run_crashes.sh race   repeat (races)
#   REMOTE=claude-test SSH="ssh -i ~/.ssh/to-test" tests/run_crashes.sh
cd "$(dirname "$0")"
LOGS=${LOGS:-/var/tmp/sysrr-crashes}
ROUNDS=${ROUNDS:-1}
PY=docker.io/library/python:3.12-slim
RD=docker.io/library/redis:7
mkdir -p "$LOGS"

# name | what happens | environment | arguments of the target
TESTS=(
"null        |NULL write                                  ||null"
"wild        |write to a random bad address               ||wild"
"stack       |stack buffer overflow (stack protector)     ||stack"
"heap        |heap overflow (glibc notices)               ||heap"
"double      |double free (glibc notices)                 ||double"
"fpe         |division by zero                            ||fpe"
"ill         |illegal instruction                         ||ill"
"recurse     |runaway recursion, stack overflow           ||recurse"
"abort       |failed assert                               ||abort"
"exit3       |clean exit with an error code               ||exit3"
"badinput    |file gone from the log, crash on the error  ||badinput"
"ebadwrite   |disk full: write fails, program aborts      ||ebadwrite"
"handler     |fault caught by a crash handler             ||handler"
"thread      |a worker thread faults                      ||thread"
"child       |a child crashes, the parent aborts          ||child"
"sigpipe     |killed by SIGPIPE                           ||sigpipe"
"alarm       |killed by a watchdog timer                  ||alarm"
"kill9       |killed from outside (SIGKILL)               ||kill9"
"sigbus      |file truncated under a mapping              ||sigbus"
"oom         |out of memory: allocation fails             ||oom"
"deadlock    |two threads deadlock (no crash)             |KILL_AT=3|deadlock"
"race-syscall|race with a syscall inside the window       ||race-syscall"
"race-memory |race entirely in user code                  ||race-memory"
"queue-race  |workers still waiting when main frees queue |TARGET=queue_race_target|"
"pbzip2      |real pbzip2 0.9.4, known race, delays added  |TARGET_CMD=/t/real/pbzip2/pbzip2_delay CKPT_AT=6|-c -k -q -p4 /t/real/pbzip2/big.txt"
"cpp         |C++: uncaught exception                     |TARGET=lang/crash_cpp|"
"rust-panic  |Rust: panic, unwinding                      |TARGET=lang/crash_rust|panic"
"rust-segv   |Rust: unsafe NULL write                     |TARGET=lang/crash_rust|segv"
"go-panic    |Go: nil pointer panic                       |TARGET=lang/crash_go|"
"py-exc      |Python: uncaught exception                  |IMG=$PY TARGET=lang/crash.py|exc"
"py-segv     |Python: NULL write through ctypes           |IMG=$PY TARGET=lang/crash.py|segv"
"redis       |Redis: real server, DEBUG SEGFAULT          |IMG=$RD TARGET_CMD=redis-server CKPT_AT=2 OPTS=-S BEFORE=sleep_then_crash|--save '' --appendonly no --enable-debug-command yes"
)

selected() {
	[ $# -eq 1 ] && return 0
	local n=$1; shift
	for w; do [[ $n == *$w* ]] && return 0; done
	return 1
}

printf '%-13s %-44s %s\n' CRASH 'WHAT HAPPENS' 'REPLAY'
for t in "${TESTS[@]}"; do
	IFS='|' read -r name what envs args <<<"$t"
	name=${name// /}
	selected "$name" "$@" || continue
	res=()
	for ((r = 1; r <= ROUNDS; r++)); do
		log=$LOGS/$name.$r.log
		envs2=${envs/BEFORE=sleep_then_crash/}
		if [[ $envs == *sleep_then_crash* ]]; then
			BEFORE='redis-cli set k v; (sleep 0.5; redis-cli debug segfault) >/dev/null 2>&1' \
			eval "timeout 200 env $envs2 ./test_crash.sh $args" >"$log" 2>&1
		else
			eval "timeout 200 env $envs2 ./test_crash.sh $args" >"$log" 2>&1
		fi
		v=$(grep -a '^VERDICT' "$log" | tail -1 | sed 's/^VERDICT //')
		res+=("${v:-no verdict}")
	done
	printf '%-13s %-44s %s\n' "$name" "$what" "$(printf '%s\n' "${res[@]}" | sort | uniq -c | sed -E 's/^ *([0-9]+) /\1x /' | paste -sd';' | cut -c1-110)"
done
