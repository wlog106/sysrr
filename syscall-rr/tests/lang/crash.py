#!/usr/local/bin/python3
"""Python: an uncaught exception (the interpreter survives and exits 1), or a
NULL write through ctypes (the interpreter itself dies)."""
import ctypes, sys, time

rnd = open("/dev/urandom", "rb", buffering=0)
t0 = time.time()
total = 0
i = 0
while True:
    total ^= int.from_bytes(rnd.read(8), "little")
    print(f"iter {i} {total:016x}", flush=True)
    time.sleep(0.02)
    i += 1
    if time.time() - t0 > 3.2:
        if sys.argv[1] == "segv":
            ctypes.memset(8, 1, 1)
        raise ValueError(f"bad total {total}")
