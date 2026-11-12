#!/usr/local/bin/python3
"""Python threads: three workers take jobs from a queue.Queue and answer on
another.  They share the interpreter lock (GIL), which changes hands on a timer
and whenever a thread blocks; who got it when is not in the log, only the futex
calls around it are.  The replay may fail to follow (REPLAY.md, limitations);
it must say so and never hang."""
import hashlib, queue, threading, time

rnd = open("/dev/urandom", "rb", buffering=0)
jobs, done = queue.Queue(), queue.Queue()
total = 0
lock = threading.Lock()

def worker(wid):
    global total
    while True:
        j = jobs.get()
        r = rnd.read(8)
        h = hashlib.sha256(r).hexdigest()
        time.sleep(r[0] / 100000)
        with lock:
            total = (total * 31 + r[1] + j) % 1000003
            t = total
        done.put((j, wid, h[:6], t))

for w in range(3):
    threading.Thread(target=worker, args=(w,), daemon=True).start()
time.sleep(1)
i = 0
while True:
    for j in range(5):
        jobs.put(i * 5 + j)
    out = [done.get() for _ in range(5)]
    print(f"round {i} " + " ".join(f"j{j}:w{w}:{h}:{t}" for j, w, h, t in out), flush=True)
    i += 1
    time.sleep(0.05)
