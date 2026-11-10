#!/bin/sh
# A shell script: what most real work is made of.  Every line forks, execs
# real programs and reads them through pipes, so it exercises fork, vfork-free
# exec, wait4, dup2 and pipes together.  Output depends on the recorded time.
i=0
while :; do
	now=$(date +%s.%N)
	n=$(ls /etc | wc -l)
	echo "round $i time $now etc-entries $n"
	i=$((i + 1))
	sleep 0.02
done
