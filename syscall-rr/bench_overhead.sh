#!/bin/bash
# Overhead of recording on the recorded process itself.
#
# dd with bs=1 is the worst case: ~4M syscalls (2M read + 2M write of 1 byte).
# The traced pid must be dd's own pid, so the shell stops itself and then
# exec()s dd in place: same pid, and dd only starts once the recorder is
# attached.  (Running `taskset ... time dd` under `bash -c` would trace
# /usr/bin/time instead, because time forks dd as a child.)
#
# usage: ./bench_overhead.sh [runs] [count]
cd "$(dirname "$0")"
RUNS=${1:-5}
COUNT=${2:-2000000}
LOG=${LOG:-/var/tmp/sysrr-bench.log}

run() {   # $1 = label, rest = recorder command (PID is substituted)
  local label=$1; shift
  local sum=0
  for i in $(seq "$RUNS"); do
    taskset -c 2 bash -c "kill -STOP \$\$; exec dd if=/dev/zero of=/dev/null bs=1 count=$COUNT status=none" &
    local P=$!
    sleep 0.3
    if [ $# -gt 0 ]; then
      sudo "${@/PID/$P}" 2>/var/tmp/sysrr-bench.err &
      sleep 1
    fi
    local t0=$(date +%s%N); kill -CONT $P; wait $P; local t1=$(date +%s%N)
    local extra=""
    if [ $# -gt 0 ]; then
      sleep 0.3; sudo pkill -INT -x "$(basename "$1")"; sleep 0.5
      extra=$(grep -oE '(events received: [0-9]+|syscalls=[0-9]+|lost=[0-9]+)' /var/tmp/sysrr-bench.err | tr '\n' ' ')
    fi
    local ms=$(( (t1 - t0) / 1000000 ))
    sum=$((sum + ms))
    printf "%-22s run %d: %5d ms  %s\n" "$label" "$i" "$ms" "$extra"
  done
  printf "%-22s mean:  %5d ms\n\n" "$label" $((sum / RUNS))
}

run "baseline"
run "raw_sysrr (original)"  ./raw_sysrr PID -q
run "sysrr -> /dev/null"    ./sysrr -p PID -o /dev/null
run "sysrr -> file"         ./sysrr -p PID -o "$LOG"
ls -l "$LOG"
