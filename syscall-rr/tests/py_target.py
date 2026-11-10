#!/usr/local/bin/python3
"""A Python program: subprocess (vfork + exec), C extensions loaded after the
checkpoint (dlopen: real mmap of real files), SQLite in memory.  Each line
depends on recorded random numbers and time.

Random bytes come from /dev/urandom, not os.urandom: glibc's getrandom is made
by the vDSO, without syscalls (tests/rand_target.c tests that on its own).

Single-threaded on purpose: threads fight over the GIL, and the order in which
they won a contended lock is not in the log (REPLAY.md, limitations)."""
import hashlib, importlib, sqlite3, subprocess, time

LATE = ["csv", "decimal", "statistics", "uuid", "zlib", "bz2", "lzma", "array", "ctypes"]
db = sqlite3.connect(":memory:")
db.execute("create table t (k integer, v text)")
rnd = open("/dev/urandom", "rb", buffering=0)
time.sleep(1)
i = 0
while True:
    h = hashlib.sha256(rnd.read(16)).hexdigest()
    db.execute("insert into t values (?, ?)", (i, h))
    out = subprocess.run(["/bin/echo", f"child {i}"], capture_output=True, text=True).stdout.strip()
    late = LATE[i % len(LATE)]
    importlib.import_module(late)
    rows, = db.execute("select count(*) from t").fetchone()
    print(f"round {i} time {time.time()} hash {h[:16]} rows {rows} {out} imported {late}", flush=True)
    i += 1
    time.sleep(0.02)
