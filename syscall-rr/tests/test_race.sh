#!/bin/bash
# Race test: record tests/race_target with the race check on (sysrr -r) and
# verify that sysrr_dump -r flags exactly its two racy syscalls (the pipe
# write and the FIFO open) and not the control write.
set -e
cd "$(dirname "$0")"
OUT=${OUT:-/var/tmp/sysrr-race}
rm -rf "$OUT"
mkdir -p "$OUT"
make -s -C .. sysrr sysrr_dump tests/race_target

# same trick as bench_overhead.sh: stop, let the recorder attach, then exec
bash -c 'kill -STOP $$; exec ./race_target "$0"' "$OUT" >"$OUT/target.out" &
P=$!
sleep 0.3
sudo ../sysrr -r -p "$P" -o "$OUT/rec.log" 2>"$OUT/sysrr.err" &
sleep 1
kill -CONT "$P"
wait "$P"
wait                                    # sysrr stops by itself when the target exits

echo "--- target output"
cat "$OUT/target.out"
echo "--- syscalls whose input changed during the call (sysrr_dump -r)"
../sysrr_dump -r "$OUT/rec.log" | tee "$OUT/races.txt" | grep -v '^# columns'

n=$(grep -c '!! RACE' "$OUT/races.txt" || true)
fail=0
grep -qE '^#[0-9]+ .* write\(' "$OUT/races.txt" || { echo "FAIL: pipe write not flagged"; fail=1; }
grep -qE '^#[0-9]+ .* openat?\(' "$OUT/races.txt" || { echo "FAIL: FIFO open not flagged"; fail=1; }
[ "$n" = 2 ] || { echo "FAIL: expected 2 races, got $n"; fail=1; }
[ $fail = 0 ] && echo "PASS: 2 races flagged, control write clean"
exit $fail
