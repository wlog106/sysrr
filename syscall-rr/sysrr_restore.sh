#!/bin/bash
# sysrr_restore.sh: restore a checkpoint left stopped, ready for sysrr_replay.
#
#   sudo ./sysrr_restore.sh ckpt.tar.gz NEW       restore as container NEW
#   sudo ./sysrr_replay -C NEW -g 1 rec.log       then replay into it
#
# A replay must attach before the restored process runs a single instruction,
# so CRIU has to leave it stopped.  podman has no option for that; CRIU reads
# it from /etc/criu/default.conf.  This script adds "leave-stopped" to that
# file for the restore only and puts the file back as it was (also when the
# restore fails or is interrupted).  It also switches the clock source to
# acpi_pm, which the replay needs (README, notes), and leaves it so.
#
# Options: -c "restore options" passed to podman (e.g. --tcp-established; podman
# refuses --name with it, so the copy keeps the original's name).
# Last line of output: the restored container's name and host pid.
set -u
COPTS=
while getopts "c:" o; do
	case $o in
	c) COPTS=$OPTARG ;;
	*) sed -n '2,17p' "$0"; exit 2 ;;
	esac
done
shift $((OPTIND - 1))
CKPT=${1:-}
NAME=${2:-}
if [ -z "$CKPT" ] || [ $# -gt 2 ]; then
	echo "usage: sysrr_restore.sh [-c \"podman restore options\"] ckpt.tar.gz [NAME]" >&2
	exit 2
fi
[ -r "$CKPT" ] || { echo "sysrr_restore: cannot read $CKPT" >&2; exit 2; }
[ "$(id -u)" = 0 ] || { echo "sysrr_restore: run it as root (sudo)" >&2; exit 2; }
case "$COPTS" in *tcp-established*)
	[ -z "$NAME" ] || echo "sysrr_restore: --tcp-established: podman keeps the original name, ignoring $NAME" >&2
	NAME= ;;
esac

CS=/sys/devices/system/clocksource/clocksource0
if grep -qw acpi_pm $CS/available_clocksource 2>/dev/null; then
	[ "$(cat $CS/current_clocksource)" = acpi_pm ] ||
		{ echo acpi_pm >$CS/current_clocksource && echo "clock source: acpi_pm"; }
else
	echo "sysrr_restore: no acpi_pm clock source here; clock reads may not be replayed" >&2
fi

CONF=/etc/criu/default.conf
# The original is kept at a fixed place, so a run that was killed half way is
# repaired by the next one (a backup that vanished with a temporary file would
# leave leave-stopped in the config for good).  One restore at a time.
BAK=/run/sysrr-restore.criu.conf
NONE=/run/sysrr-restore.no-conf
exec 9>/run/sysrr-restore.lock
flock 9
put_back() {
	if [ -e $BAK ]; then cp -p $BAK $CONF; elif [ -e $NONE ]; then rm -f $CONF; fi
	rm -f $BAK $NONE
}
[ -e $BAK ] || [ -e $NONE ] && { echo "sysrr_restore: repairing $CONF after an interrupted run" >&2; put_back; }
if [ -e $CONF ]; then cp -p $CONF $BAK; else touch $NONE; fi
trap put_back EXIT
trap 'exit 130' INT TERM
mkdir -p /etc/criu
# Other tools' action scripts stay out of this restore: a hook that freezes or
# attaches to the restored task (crr installs one) would fight the replay.
if [ -e $BAK ] && grep -q '^[[:space:]]*action-script' $BAK; then
	echo "sysrr_restore: action-script lines of $CONF left out for this restore" >&2
fi
{ [ ! -e $BAK ] || grep -vxE 'leave-stopped|[[:space:]]*action-script.*' $BAK; echo leave-stopped; } >$CONF.new
mv $CONF.new $CONF

ERR=$(mktemp /tmp/sysrr-restore.XXXXXX)
# shellcheck disable=SC2086  # COPTS holds several options
if ! id=$(podman container restore $COPTS --import "$CKPT" ${NAME:+--name "$NAME"} 2>"$ERR"); then
	cat "$ERR" >&2
	if grep -qi apparmor "$ERR"; then
		echo "hint: podman loads its AppArmor profile at its first 'podman run' since boot;" >&2
		echo "      run any container once (podman run --rm IMAGE true) and try again" >&2
	fi
	rm -f "$ERR"
	exit 1
fi
rm -f "$ERR"
put_back
trap - EXIT

id=$(echo "$id" | tail -1)
NAME=$(podman inspect -f '{{.Name}}' "$id")
pid=$(podman inspect -f '{{.State.Pid}}' "$id")
state=$(awk '/^State:/ { print $2 }' /proc/"$pid"/status 2>/dev/null)
[ "$state" = T ] || echo "sysrr_restore: warning: pid $pid is in state '$state', not stopped (T)" >&2
echo "restored $NAME pid $pid (stopped); now: sysrr_replay -C $NAME -g GEN rec.log"
