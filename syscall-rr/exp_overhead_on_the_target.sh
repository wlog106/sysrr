bash -c 'kill -STOP $$; exec taskset -c 2 time dd if=/dev/zero of=/dev/null bs=1 count=2000000' &
DD_PID=$!
sleep 0.5
sudo ./raw_sysrr $DD_PID -q &
sleep 1
kill -CONT $DD_PID
wait $DD_PID
sudo pkill -INT raw_sysrr

