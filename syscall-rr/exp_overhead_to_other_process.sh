sleep 100000 &
SLEEP_PID=$!

run_dd() {
  for i in 1 2 3 4 5; do
    taskset -c 2 time dd if=/dev/zero of=/dev/null bs=1 count=2000000 2>&1 | tail -1
  done
}

echo "== baseline (nothing attached) =="
run_dd

echo "== tp_btf version =="
sudo ./raw_sysrr $SLEEP_PID -q &
TRACER=$!
sleep 1
run_dd
sudo kill -INT $TRACER; wait $TRACER 2>/dev/null

echo "== old tracepoint version =="
sudo ./sysrr $SLEEP_PID > /dev/null &
TRACER=$!
sleep 1
run_dd
sudo kill $TRACER
