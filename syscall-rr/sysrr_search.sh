#!/bin/bash
# sysrr_search.sh: find an order of threads that replays a recording to its end.
#
# With several threads, sysrr_replay -T runs one thread's user code at a time.
# When two stretches of user code overlapped in the live run, the log does not
# say which one touched shared memory first, and the replay has to choose (by
# default: the one that ended first).  A wrong choice shows up later as a
# divergence.  This script restores the checkpoint again and again, each time
# flipping one of the choices sysrr_replay names as suspects (-F), and keeps a
# flip when the replay gets further.  It stops when a replay reaches the end of
# the recording, or when nothing helps, or after TRIES replays.
#
# Every order it can find is consistent with the whole log, but it is not
# necessarily the order of the live run: the log cannot tell those apart.  The
# flips it needed name the places where the program's behaviour depended on
# which thread came first.
#
#   sudo podman run --rm IMAGE true            once per boot (AppArmor profile)
#   ./sysrr_search.sh -i ckpt.tar.gz -g 1 rec.log
#
# Options: -i checkpoint archive, -g generation (1), -n tries (20), -o "replay
# options" (added to -T), -c "restore options" (e.g. --tcp-established; the
# copy then keeps its original name), -k keep the last copy running (stopped
# at the end of the recording, for inspection).
# Prints the flips found as a ready-to-use -F option, last line:
#   FOUND -F 3297,5334          the recording replays to its end with these
#   BEST  -F 3297 (progress N)  nothing better found
set -u
TRIES=20 GEN=1 CKPT= ROPTS= COPTS= KEEP=
while getopts "i:g:n:o:c:k" o; do
	case $o in
	i) CKPT=$OPTARG ;; g) GEN=$OPTARG ;; n) TRIES=$OPTARG ;;
	o) ROPTS=$OPTARG ;; c) COPTS=$OPTARG ;; k) KEEP=1 ;;
	*) sed -n '2,30p' "$0"; exit 2 ;;
	esac
done
shift $((OPTIND - 1))
LOG=${1:?usage: sysrr_search.sh -i ckpt.tar.gz [-g N] [-n TRIES] rec.log}
[ -n "$CKPT" ] || { echo "sysrr_search: -i ckpt.tar.gz is required" >&2; exit 2; }
cd "$(dirname "$0")"
NAME=rrsearch CONF=/etc/criu/default.conf
WORK=$(mktemp -d /var/tmp/sysrr-search.XXXXXX)
[ -e $CONF ] && sudo cp $CONF "$WORK/criu.conf.bak"
CS=/sys/devices/system/clocksource/clocksource0/current_clocksource
old_cs=$(cat $CS)
echo acpi_pm | sudo tee $CS >/dev/null
cleanup() {
	if [ -e "$WORK/criu.conf.bak" ]; then sudo cp "$WORK/criu.conf.bak" $CONF; else sudo rm -f $CONF; fi
	[ -n "$KEEP" ] || sudo podman rm -f -t 0 "$NAME" >/dev/null 2>&1
	echo "$old_cs" | sudo tee $CS >/dev/null
}
trap cleanup EXIT

# one replay with the flips in $1; prints "progress candidates..." on one line
attempt() {
	local flips=$1 end=${2:-kill} out="$WORK/try.txt" pid name
	sudo podman rm -f -t 0 "$NAME" >/dev/null 2>&1
	sudo mkdir -p /etc/criu
	echo leave-stopped | sudo tee $CONF >/dev/null
	name="--name $NAME"
	case "$COPTS" in *tcp-established*) name= ;; esac
	if ! sudo podman container restore $COPTS --import "$CKPT" $name >"$WORK/restore.txt" 2>&1; then
		sudo rm -f $CONF
		echo "restore failed: $(tail -1 "$WORK/restore.txt")" >&2
		return 1
	fi
	sudo rm -f $CONF
	[ -n "$name" ] || NAME=$(sudo podman ps --latest --format '{{.Names}}')
	pid=$(sudo podman inspect -f '{{.State.Pid}}' "$NAME")
	timeout 120 sudo ./sysrr_replay -T $ROPTS ${flips:+-F $flips} -p "$pid" -g "$GEN" -e "$end" \
		"$LOG" >"$out" 2>&1 &
	if [ "$end" = stop ]; then
		for i in $(seq 600); do grep -q '^SEARCH progress' "$out" && break; sleep 0.2; done
		cp "$out" "$WORK/last.txt"
	else
		wait
	fi
	cp "$out" "$WORK/last.txt"
	echo "$(grep -m1 '^SEARCH progress' "$out" | cut -d' ' -f3) $(grep -m1 '^SEARCH at' "$out" | cut -d' ' -f3) $(grep -m1 '^SEARCH candidates' "$out" | cut -d: -f2)"
}

flips= tries=0
read -r best at cands <<<"$(attempt "")"
tries=1
echo "try 1 (no flips): progress ${best:-?}${at:+ (diverged at #$at)}"
while [ "$best" != all ] && [ $tries -lt "$TRIES" ]; do
	improved=
	for c in $cands; do
		case ",$flips," in *",$c,"*) continue ;; esac
		[ $tries -lt "$TRIES" ] || break
		try=${flips:+$flips,}$c
		read -r p pat nc <<<"$(attempt "$try")"
		tries=$((tries + 1))
		echo "try $tries (-F $try): progress ${p:-?}${pat:+ (diverged at #$pat)}"
		# it counts only if the replay got past the record that diverged: a flip
		# that only lets a few more unrelated records through fixes nothing
		if [ "$p" = all ] || { [ -n "$p" ] && [ "$pat" != "$at" ] && [ "$p" -gt "${best:-0}" ] 2>/dev/null; }; then
			flips=$try best=$p at=$pat cands=$nc improved=1
			break
		fi
	done
	# no single flip helps: some decisions only work together, so try pairs of
	# the six strongest suspects
	if [ -z "$improved" ]; then
		set -- $cands
		top=("${@:1:6}")
		for ((x = 0; x < ${#top[@]} && !${#improved}; x++)); do
			for ((y = x + 1; y < ${#top[@]}; y++)); do
				[ $tries -lt "$TRIES" ] || break 2
				try=${flips:+$flips,}${top[x]},${top[y]}
				read -r p pat nc <<<"$(attempt "$try")"
				tries=$((tries + 1))
				echo "try $tries (-F $try): progress ${p:-?}${pat:+ (diverged at #$pat)}"
				if [ "$p" = all ] || { [ -n "$p" ] && [ "$pat" != "$at" ] && [ "$p" -gt "${best:-0}" ] 2>/dev/null; }; then
					flips=$try best=$p at=$pat cands=$nc improved=1
					break
				fi
			done
		done
	fi
	[ -n "$improved" ] || break
done
if [ -n "$KEEP" ] && [ "$best" = all ]; then
	attempt "$flips" stop >/dev/null
	echo "the copy $NAME is left stopped at the end of the recording (sysrr_replay keeps running)"
fi
if [ "$best" = all ]; then
	echo "FOUND -F ${flips:-none} ($tries replays)"
	exit 0
fi
echo "BEST  -F ${flips:-none} (progress $best of the records, $tries replays)"
exit 1
