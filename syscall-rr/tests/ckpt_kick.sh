#!/bin/sh
# CRIU action script for tests/test_replay.sh CKPT_KICK=SIG.  At post-dump CRIU
# has saved the process (pending signals included) but not let it go on yet:
# send the signal named in /run/sysrr-kick ("PID SIG"), so it lands while the
# process is frozen and is delivered the moment it continues.
[ "$CRTOOLS_SCRIPT_ACTION" = post-dump ] || exit 0
[ -r /run/sysrr-kick ] || exit 0
read -r pid sig </run/sysrr-kick
kill -s "$sig" "$pid"
exit 0
