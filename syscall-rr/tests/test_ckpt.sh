#!/bin/bash
# End-to-end test of the record side of the workflow:
#   container running -> sysrr recording -> podman checkpoint --leave-running
#   -> container keeps running -> stop recording -> inspect generation 1
set -e
cd "$(dirname "$0")"
OUT=${OUT:-/var/tmp/sysrr-test}
NAME=rrtest
mkdir -p "$OUT"

sudo podman rm -f -t 0 $NAME >/dev/null 2>&1 || true
sudo podman run -d --name $NAME -v "$PWD":/t:ro docker.io/library/debian:trixie-slim /t/ckpt_target >/dev/null
PID=$(sudo podman inspect -f '{{.State.Pid}}' $NAME)
echo "container $NAME, target host pid $PID"

sudo ../sysrr -p "$PID" -o "$OUT/rec.log" &
sleep 2

echo "--- checkpoint (leave running) ---"
sudo podman container checkpoint --leave-running -e "$OUT/ckpt.tar.gz" $NAME >/dev/null
CKPT_LINE=$(sudo podman logs $NAME | tail -1)
echo "last line before checkpoint finished: $CKPT_LINE"

sleep 2
sudo pkill -INT -x sysrr
wait 2>/dev/null || true

echo "--- live output after the checkpoint (what a replay must reproduce) ---"
sudo podman logs $NAME | sed -n "/^${CKPT_LINE}\$/,\$p" | tail -n +2 | tee "$OUT/live_after.txt"
sudo podman rm -f -t 0 $NAME >/dev/null
ls -l "$OUT"
