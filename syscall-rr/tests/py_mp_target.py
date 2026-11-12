#!/usr/local/bin/python3
"""multiprocessing: a parent and three worker processes forked before the
checkpoint, talking through pipes guarded by semaphores that live in shared
memory (/dev/shm): locks between processes.  Needs sysrr -f and -T."""
import hashlib, multiprocessing as mp, os, time

def worker(wid, jobs, done):
    rnd = open("/dev/urandom", "rb", buffering=0)
    while True:
        j = jobs.get()
        r = rnd.read(8)
        time.sleep(r[0] / 100000)
        done.put((j, wid, os.getpid(), hashlib.sha256(r).hexdigest()[:6]))

if __name__ == "__main__":
    mp.set_start_method("fork")
    jobs, done = mp.Queue(), mp.Queue()
    for w in range(3):
        mp.Process(target=worker, args=(w, jobs, done), daemon=True).start()
    time.sleep(1)
    i = 0
    while True:
        for j in range(5):
            jobs.put(i * 5 + j)
        out = [done.get() for _ in range(5)]
        print(f"round {i} " + " ".join(f"j{j}:w{w}:p{p}:{h}" for j, w, p, h in out), flush=True)
        i += 1
        time.sleep(0.05)
