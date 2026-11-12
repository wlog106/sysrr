# sysrr: recording syscalls across a CRIU checkpoint

Goal of the overall project:

```
machine A                                         machine B
container running ──► podman checkpoint -R ──►   podman restore
   sysrr recording          (tarball) ─────────►    replay generation N:
   (gen 0 ... gen N ...)                             fake the recorded syscall
                                                     results until the restored
                                                     process reaches the state
                                                     the live one reached
```

This document covers the **record** side, then the **replay** side
(`sysrr_replay`, section "Replay" at the end). RECORD.md (record) and REPLAY.md (replay) keep the
project's facts, measurements and traps in short form.

## Files

| file | role |
|---|---|
| `sysrr.bpf.c` | kernel side: tracepoint programs, capture table, CRIU detection |
| `sysrr.c` | user side: loads the programs for one pid, writes the log |
| `sysrr_dump.c` | decoder: prints a log, filters by generation, checks integrity |
| `sysrr_replay.c` | replayer: drives a restored process through one generation |
| `sysrr.h` | record format shared by all of them |
| `bench_overhead.sh` | overhead benchmark on the recorded process (dd, 4M syscalls) |
| `tests/ckpt_target.c`, `tests/test_ckpt.sh` | end-to-end checkpoint test |
| `tests/race_target.c`, `tests/test_race.sh` | race check test (`sysrr -r`) |
| `tests/test_replay.sh`, `tests/tamper_log.py`, `tests/mt_target.c` | record → checkpoint → restore → replay tests |
| `raw_sysrr*`, `exp_*.sh` | original prototype, kept for comparison |

```
make
sudo ./sysrr -p <host pid> [-f] [-r] -o rec.log # Ctrl-C to stop; -r = race check
sudo podman container checkpoint --leave-running -e ckpt.tar.gz <ctr>
./sysrr_dump -g 1 rec.log                       # replay log of checkpoint #1
./sysrr_dump -s rec.log                         # summary + integrity check
./sysrr_dump -r rec.log                         # inputs changed during a syscall (-r logs)
```

`podman inspect -f '{{.State.Pid}}' <ctr>` gives the host pid of the
container's main process.

## Why eBPF and not ptrace

CRIU checkpoints a process by `PTRACE_SEIZE`-ing every thread. A thread can
have only one tracer, so a ptrace-based recorder would make the checkpoint
fail. It would also stop the target twice per syscall. eBPF programs are
attached to the kernel's `sys_enter` / `sys_exit` tracepoints, not to the
process, so CRIU never notices them.

## How a syscall is recorded

```
sys_enter (target thread)               sys_exit (same thread)
  seq = global counter++                  copy the memory the kernel wrote
  save ts, args, gen in                   (capture_exit()):
  task-local storage                        REC_DATA ...      D_OUT
  copy the memory the kernel will read    with -r, copy the inputs again:
  (capture_enter()):                        REC_DATA ...      D_IN, DF_EXIT
    REC_DATA ...      D_IN / D_STR        REC_SYSCALL         (commits seq)
```

* **Outputs are captured at exit.** Only then has the kernel filled the
  buffer, and the return value says how much of it is valid.
* **Inputs are captured at entry.** The kernel copies most inputs right
  after entry, and the syscall may then block for a long time (open of a
  FIFO, connect). Copying early gives another thread the smallest window to
  change the buffer before we copy it. The return value is not known yet, so
  the requested size is copied. In/out arguments (`select` fd sets, `poll`,
  `accept` addrlen, `recvmsg` msghdr) are copied at entry *and* exit: the
  kernel overwrites the input with the result. `execve`'s path can only be
  copied at entry: the old memory is gone after exit.
* **Entry copies can fault.** The page may not be resident yet (first touch,
  e.g. a string constant in a program that just started), and tracepoints
  cannot fault pages in. Then the inputs are copied again at exit, after the
  kernel has touched the page. If the kernel never reads the page either (a
  write to `/dev/null`), the record keeps `F_FAULT`.
* **What gets copied** is decided per syscall:
  * `D_OUT`: memory the kernel wrote (read buffer, `struct stat`, timespec,
    getrandom bytes, epoll events, ...). Replay must inject this.
  * `D_IN` / `D_STR`: memory the kernel read (write data, paths, sockaddr).
    Replay compares it to detect that the restored process has diverged.
* Buffers are copied in 16 KiB chunks, up to 1 MiB per buffer. Larger
  buffers are flagged `F_TRUNC`.
* **Nothing is silently lost.** If the ring buffer is full the record gets
  `F_LOST` and a counter is incremented. `sysrr_dump -s` also checks that seq
  numbers have no gaps and that each syscall has all its data records.

### Race check (`-r`)

The kernel does not freeze user memory during a syscall: it copies a buffer
once, when it chooses, and then uses its own copy. If another thread changes
the buffer, the entry copy can be wrong (the kernel copied later) and so can
an exit copy (the kernel copied earlier). `tests/race_target.c` has one of
each:

| case | kernel copies | entry copy | exit copy | kernel used |
|---|---|---|---|---|
| `write` to a full pipe | when there is room (late) | `AAAA…` | `BBBB…` | `BBBB…` |
| `open` of a FIFO | path first, then waits (early) | `…/fifo` | `…/XXXX` | `…/fifo` |

With `sysrr -r` every pure input is copied again at exit (`DF_EXIT`), and
`sysrr_dump -r` lists the syscalls whose two copies differ, with the first
differing byte. This flags *a buffer changed while a syscall was using it*.
It cannot see races that never reach a syscall (shared counters, missing
locks: most race bugs), a change undone before exit, or a change to an
output buffer after the kernel filled it. Cost: about 11% more on the dd
worst case. `tests/test_race.sh` checks that exactly the two racy syscalls
are flagged.

### Overhead

dd `bs=1 count=2000000` (≈4M syscalls, worst case), pinned to one CPU:

| recorder | time | slowdown | lost |
|---|---|---|---|
| none | 376 ms | 1.0x | |
| original `raw_sysrr -q` (read data + enter events only) | 2098 ms | 5.6x | up to ~2k |
| `sysrr` → file (full log, 612 MB) | 1107 ms | 2.9x | 0 |

Re-measured after moving inputs to entry (same machine, slower that day):
baseline 982 ms; inputs at exit 2.85x; inputs at entry 2.77x; with `-r`
3.08x. Moving the copy costs nothing; the race check about 11%.

Unrelated processes pay ≈0.03 µs per syscall (dd untraced: 376 → 496 ms)
because the tracepoints fire for every syscall on the machine.

The biggest win was **not waking user space on every record**. With
`bpf_ringbuf_submit(e, 0)`, each event wakes the consumer (irq_work plus a
context switch), and that was most of the original cost. Now records are
submitted with `BPF_RB_NO_WAKEUP` until the ring is ¼ full, and the consumer
polls every 10 ms.

## The checkpoint boundary: generations

During `podman checkpoint`, CRIU:

1. freezes the container and `PTRACE_SEIZE`s each thread of the target,
2. injects **parasite code** into the target. The parasite makes about 350
   syscalls *in the target's own context* (memfd_create, mmap, socket,
   sendmsg, prctl, ...) to dump memory and state,
3. removes the parasite and `PTRACE_DETACH`es (with `--leave-running` the
   process then continues).

`sysrr` watches `ptrace(2)` calls from any process that are aimed at a target
thread:

* While a thread is seized, its syscalls are flagged `F_DUMP`
  (`criu-parasite` in the dump). They are not part of the application's
  history and must never be replayed.
* When the last seized thread is detached, `gen` is incremented and an
  `M_GEN` marker is written.

**Generation N = everything the application did after checkpoint N.** If you
restore checkpoint N, replay the records with `gen == N` and no `F_DUMP`
flag.

### The syscall in flight at checkpoint time

The target is usually blocked in a syscall when it is frozen. That syscall
returns with an internal restart code, which you can see in generation N‑1:

* `-ERESTARTSYS` / `-ERESTARTNOHAND` / `-ERESTARTNOINTR`: CRIU saves the
  registers rewound to re-execute the syscall. The live process and the
  restored one both issue it again, so it is the first record of generation N.
* `-ERESTART_RESTARTBLOCK` (-516, e.g. `clock_nanosleep`): CRIU saves the
  registers with `-EINTR` instead. Both processes see the sleep interrupted
  and continue, so generation N starts with the syscall after it. The test
  run shows this case.

Either way, generation N starts exactly where the restored process starts.

### Pids

Records carry both the host tid and the tid inside the container's pid
namespace (`ns_tid`). CRIU restores the namespace pids, while the host pids
on machine B will differ, so replay must match on `ns_tid`.

## Validation

`tests/test_ckpt.sh` runs `tests/ckpt_target` in a container, records it,
checkpoints with `--leave-running`, and saves the lines the live process
printed after the checkpoint. The program's state depends only on syscall
results (random bytes and the clock). Recomputing the state from the
generation‑1 records alone reproduced all 10 lines the live process printed.
So the log contains every input the program used.

## Known gaps of the recording

* **vDSO calls are not syscalls.** `clock_gettime`, `gettimeofday`, `time`
  and `getcpu` are normally answered in user space by the vDSO and never
  reach the tracepoint. For experiments, switch to a clocksource the vDSO
  cannot read, which makes glibc fall back to the real syscall:

  ```
  echo acpi_pm | sudo tee /sys/devices/system/clocksource/clocksource0/current_clocksource
  # back: echo kvm-clock | sudo tee ...
  ```

  This is system-wide and makes time reads slower. `*_COARSE` clocks and
  `time()` still read the vDSO data page. Alternatives: boot with `vdso=0`,
  or uretprobes on libc's `clock_gettime`/`gettimeofday`/`time`. On the replay
  side the restored vDSO will have to be redirected either way.
* Not captured yet: `ioctl` beyond FIONREAD/TCGETS/TIOCGWINSZ, `recvmmsg`,
  `io_uring` (completions bypass syscalls), shared memory written by other
  processes, signals (delivery points are not recorded), `rdtsc`.
* Multi-threaded targets: records are per thread (`ns_tid`) and globally
  ordered by `seq`. Reproducing the interleaving across threads is a replay
  problem.
* A syscall still blocked when recording stops has its input records in the
  log but no `REC_SYSCALL`. `sysrr_dump` reports it as "in flight at stop"
  (its seq is newer than every committed syscall of that thread), not as
  an orphan.
* Follow mode (`-f`) adds children on fork but never removes exited ones
  (4096-entry map).
* Capture uses `bpf_probe_read_user`, which cannot fault pages in. At exit,
  buffers the kernel just read or wrote are resident. At entry they may not
  be; see "Entry copies can fault" above. A buffer nobody ever touched
  (written to `/dev/null`) stays `F_FAULT`.

## Replay

```
echo leave-stopped | sudo tee /etc/criu/default.conf     # restore only!
sudo podman container restore --import ckpt.tar.gz --name NEW
sudo rm /etc/criu/default.conf
sudo ./sysrr_replay -p <pid of NEW> -g 1 [-S] [-v] [-e stop|live|kill] [-w out.txt] rec.log
```

### Mechanism: seccomp user notification

A seccomp filter that returns `SECCOMP_RET_USER_NOTIF` makes the kernel park
the calling thread at syscall entry and hand `{tid, nr, args}` to whoever
holds the filter's *listener* fd. That supervisor (`sysrr_replay`) answers
either with a result (the syscall is never executed, the thread sees `ret`)
or with `SECCOMP_USER_NOTIF_FLAG_CONTINUE` (the kernel runs it for real).
Before answering, the supervisor can read and write the thread's memory with
`process_vm_readv/writev`, which is how recorded output buffers are injected.

eBPF could not do this here: without `CONFIG_FUNCTION_ERROR_INJECTION`
there is no `bpf_override_return`, and writing user memory from BPF
(`bpf_probe_write_user`) is unsafe.

### Getting the filter into a restored process

A process can only install a seccomp filter on itself, and a restored
process never runs `execve`, so a launcher is not possible. Instead:

1. **Restore it stopped.** CRIU reads `/etc/criu/default.conf` even when
   podman → crun → libcriu drive it; `leave-stopped` there makes CRIU leave
   the restored tasks in `T` state, before they run a single instruction.
   (Remove the file right after: on a dump it would leave the live process
   stopped.)
2. **Make it call seccomp() itself** (`attach_listener()`): ptrace-seize the
   stopped process, overwrite 3 bytes at its rip with `syscall; int3`, and
   run, one by one: `mmap` (scratch page for the filter program),
   `prctl(PR_SET_NO_NEW_PRIVS)`, `seccomp(SET_MODE_FILTER, NEW_LISTENER |
   TSYNC)`. The listener fd is now in the target's fd table;
   `pidfd_getfd()` copies it into ours. Then `munmap` and `close` it in the
   target (our own filter already traps those two, so the injector answers
   them with CONTINUE), put the code bytes and registers back and detach.
   ptrace is used for a few milliseconds, after CRIU has finished, so it
   conflicts with nothing.
3. `SIGCONT`. From now on every syscall of every thread comes to us.

### Answering one syscall

```
notification {host tid, nr, args}
  └─ ns_tid (NSpid in /proc/tid/status) → that thread's queue of gen-N records
       next record r:
       r.nr != nr                          → DIVERGENCE
       args differ                         → warning (pointers too: same address space)
       r's D_IN/D_STR != memory now        → DIVERGENCE  (the program computed something else)
       nr is process-internal              → CONTINUE   (mmap, brk, mprotect, signals setup,
                                                         clone, futex, exit, getpid, ...)
       otherwise                           → write r's D_OUT pieces into memory,
                                             reply r.ret                 (fake)
       ...and it created fds (r.ret >= 0)  → ADDFD a real fd at each recorded
                                             number; SEND answers with it
```

* **Fake vs run.** Anything that brings information *into* the process
  (read, recv, clock_gettime, getrandom, stat, poll...) and anything with an
  outside effect (write, send, sleep) is faked. Calls that change the process
  itself (its memory map, threads, signal handlers) must really happen, or
  the process would lack the mapping or thread it expects.
* **Where to write.** A piece is identified by `(arg, idx, offset)`. The
  destination is resolved from the *current* syscall's args, or the iovec /
  msghdr they point to, read from the target before anything is written.
* **Divergence is checked on inputs.** A replayed process that went off the
  recorded path almost always shows it at the next syscall that hands data
  to the kernel: the bytes of a `write`, a path, a sockaddr.
* **Sleeps are faked**, so replay runs much faster than real time.
* **Signal-interrupted records** (ret -512..-516) are skipped, and so is a
  `restart_syscall` the replayed thread never makes: the replay does not
  deliver signals.

### File descriptors: ADDFD

A faked `openat(...) = 5` returns 5 but creates nothing. As long as every
later syscall on fd 5 is faked too (read, fstat, close) nobody notices, but
the replay runs some syscalls for real, and `mmap(fd 5)` would fail with
EBADF. The process then computes something else, and the replay diverges.

`SECCOMP_IOCTL_NOTIF_ADDFD` lets the supervisor copy one of its own fds into
the target's fd table while the target is parked in the notification:

```
supervisor                                     target (parked in openat)
  fd = openat2(/proc/PID/root, "/etc/x",
               RESOLVE_IN_ROOT)
  ioctl(ADDFD, {id, srcfd=fd, newfd=5,
        SETFD | SEND, O_CLOEXEC?})  ───────►   fd 5 = same open file,
                                               openat returns 5
  close(fd)
```

`SETFD` places it at exactly the recorded number (replacing whatever was
there, like dup2); `SEND` answers the syscall with that number in the same
step. `pipe`/`socketpair` return two numbers through memory: two ADDFDs
without SEND (numbers from the recorded int[2]), then the normal reply.

What gets installed: the open family gets the *same file* opened inside the
container's root (`RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS`, because the
supervisor is host root and must not follow a container symlink out of it),
without `O_CREAT`/`O_TRUNC` side effects; sockets, pipes, eventfd, epoll,
timerfd, memfd, inotify get a fresh object of the same kind; `dup*` and
`F_DUPFD` get the target's own source fd (via `pidfd_getfd`); anything that
cannot be made gets `/dev/null`. The bytes read through these fds still
come from the log; the fd only has to be real for syscalls that run for
real.

Because installed fds are real, `close` and `close_range` run for real too,
so the fd table follows the recording; `chdir`/`fchdir` also run for real.
If a number about to be installed is already open in the target, the fd
tables have drifted apart, and the replay warns. `-N` disables all this.

### Addresses of new mappings: pinning

`mmap(NULL, ...)` runs for real and the kernel picks the address: the
highest free gap below `mm->mmap_base`, which is randomized at `execve`.
CRIU restores every mapping at its old address, but not `mmap_base` (the
restored task keeps CRIU's), so new mappings land elsewhere than in the
recording. Allocators notice: jemalloc wants aligned chunks, and in redis a
differently aligned address made it map a larger region instead of going on
to the recorded `read`.

seccomp cannot change a syscall's arguments; ptrace can. The thread is
parked in the notification wait, which is interruptible:

```
notification: mmap(0, len, ...)   recorded ret R, args have no MAP_FIXED
  PTRACE_SEIZE + PTRACE_INTERRUPT that thread
     wait returns -ERESTARTSYS, thread stops before returning to user space
  regs: rax = -512, orig_rax = mmap   → set rdi = R, r10 |= MAP_FIXED_NOREPLACE
  PTRACE_DETACH: the kernel restarts the syscall from these registers
notification: mmap(R, len, ... | MAP_FIXED_NOREPLACE)  → CONTINUE
```

`mremap` that moved a mapping is pinned the same way (`MREMAP_FIXED`, new
address in r8). This is ptrace again, but for one thread, for one syscall,
after CRIU is done: a few hundred microseconds per new mapping.

### Process trees

A seccomp filter belongs to the process that installs it, with all its
threads (`TSYNC`), and to the children it forks *afterwards*. nginx's
workers already exist when the checkpoint is restored, so the replay
collects the whole tree under `-p` (parent links in `/proc`, every process
stopped by CRIU) and injects a filter into each one before any of them
resumes. Each process has its own listener; the supervisor polls all of
them, answers each notification on the listener it came from, and uses the
process's own pidfd for `pidfd_getfd`. Recorded queues are per container
tid, so nothing else changes. On the recording side, `sysrr -f` now adds the
processes that already exist under `-p`, not only later forks.

### Serial order (-T)

`-S` orders syscall entries. When thread A's syscall is answered, thread
B's next syscall may be released while A still runs user code, so the two
stretches of user code overlap, as they did live, but in an uncontrolled
order. nginx shows the problem: every worker bumps a connection counter in
a `MAP_SHARED` page right after `accept4` returns, with no syscall.

`-T` runs one thread at a time. A thread runs user code from the moment its
syscall is answered until its next syscall reaches the supervisor (or until
it enters a CONTINUE'd syscall that blocks: `rt_sigsuspend`, `wait4`, exit,
a futex wait the recording lacks). A thread that doesn't come back within
2 s is no longer waited for, so a blocking call missing from that list can't
deadlock the replay.

Each record stands for the stretch of user code that follows it, up to the
thread's next syscall. Two stretches that overlapped live become sequential,
and the log can't say in which order: what they did to shared memory made no
syscall. Since 2026-11-12 the order is the one in which the stretches
*ended* (`order_serial`: a record's place is the entry of the thread's next
syscall; `seq` gives that order exactly, and entry timestamps are pulled
into line with it). Before, it was the order in which they started
(`ts_exit`). Why the end: a thread coming out of a syscall starts cold and at
an unsteady pace, while the way into the next syscall is short; and code
that works under a lock usually makes a syscall right after releasing it (a
futex wake, a write). Measured on a thread pool: by end 4 of 6 recordings
replayed to the last line, by start 1 of 6; nginx and the two-thread test
pass either way. It still can't reproduce an interleaving decided inside two
stretches that ended within the same few hundred nanoseconds.

More rules make `-T` work for programs built on locks (2026-11-12):

* **Recorded lock waits are answered, not run.** A `FUTEX_WAIT` in the log
  is held until its place in the order and then answered with the recorded
  result. Run for real it would return when the kernel likes, and all the
  threads woken by one broadcast would race for the mutex again. Returning
  from a wait without a real wake is allowed: every futex user checks the
  word again. Wakes still run for real (a wait the recording lacks, which
  runs for real, needs them).
* **A stretch that ends by waiting for a mutex** (futex value 2, "locked,
  with waiters") takes the place of the stretch after the wait: one at a
  time nobody can find a lock taken, so it must come after the holder.
* **The start is held.** A restored thread that was in user code runs until
  its first syscall before any listener can stop it, and all threads would
  do that at once. With `-T` every thread with records is put in a ptrace
  stop before the process is resumed and released when its first recorded
  syscall is next in the order (`hold_thread`, `start_threads`).
* **A lock held across a syscall.** If the thread whose turn it is sleeps in
  a futex wait that isn't in the recording, the lock's holder is parked
  behind it. A parked thread whose recorded call had returned before the
  waiter's stretch ended goes first; a parked futex wake on the same address
  goes first whenever it was recorded (`lock_holder_first`). This replaced a
  2 s stall followed by a blind guess.
* A wait the thread never left in the recording (in flight when it stopped)
  is held, not run: run for real it would take the wake-up meant for a
  thread whose recorded wait is being answered.

Added 2026-11-13 (httpd, a server with several processes of several threads):

* **Calls that free a number or an address** (close, close_range, munmap,
  dup2, dup3) take their place by their start: the number is free as soon as
  they start, and another thread's open may be recorded with it.
* **A thread asleep in a wait the recording lacks** is not waited for: the
  others go on in recorded order until one wakes it. The wait is emulated by
  the replayer (held; marked woken by any wake on its address that passes
  through; answered at its turn), never run in the kernel, where a wake-up
  would let it run next to the thread whose turn it is and make replays of the
  same log differ.
* **Facts beat guesses**: an epoll_wait is not answered before the epoll_ctl
  ADD/MOD (another thread's) that armed each event it returns.
* **A wrong file descriptor is a divergence**, not a warning.
* **The order is searched** where it is a guess: `-F` flips the place of given
  stretches (start <-> end); `sysrr_search.sh` restores and replays with one
  flip at a time, keeping flips that get the replay past the record that
  diverged, until it reaches the end. REPLAY.md, "Order search".

### Threads with nothing recorded

A thread blocked in one syscall for the whole generation (redis's
background threads waiting on a condition variable) has no record in it:
a syscall that never returns is never recorded. When the replay attaches,
every existing thread without a queue gets an empty one, so its syscall is
held and it stays blocked, as it did in the recording. Only threads created
during the replay count as "not in the recording".

### End of the recording, strict order

When a thread has no record left, its next syscall is held (not answered).
When every record is consumed:
`-e stop` (default) sends SIGSTOP and keeps the supervisor running; the
target sits exactly at the recorded end state for gdb or /proc. `-e live`
answers CONTINUE from then on: the target carries on with real syscalls.
`-e kill` is for scripts.

Without `-S`, each thread follows its own queue and threads run at their own
pace. With `-S` a thread is answered only when its record is the globally
next `seq`; the others wait at their syscall. This fixes the order of
syscall *entries* across threads, and with it all user-space work that lies
between two syscalls of one thread. It does not order user-space code
that races with no syscall in between.

### Validation

`tests/test_replay.sh` records `ckpt_target` in a container, checkpoints
with `--leave-running`, freezes the live container 2 s later
(`podman pause`, so its output ends where the recording ends), restores
the checkpoint stopped and replays generation 1:

* 43 of 44 records replayed (the last one is the `nanosleep` interrupted by
  the freeze), all 11 lines the replayed process wrote are identical to the
  live process's last 11 lines, and the target ends held at its next
  `clock_nanosleep`;
* negative test: one byte of the 4th recorded `read` flipped → replay
  stops at the following `write` ("16 of 29 bytes differ") after exactly 3
  correct lines.

`TARGET=mt_target REPLAY_OPTS=-S tests/test_replay.sh`: two threads sharing
an atomically updated accumulator. With `-S`, 274/274 records and 92/92
lines reproduced. Without `-S` it diverges at the first line that depends on
the other thread, as expected.

`TARGET=fd_target tests/test_replay.sh`: after the checkpoint the target
opens a file, `mmap`s it (real), makes a pipe, a socketpair, a socket and a
dup, prints the fd numbers and the mapped bytes, closes everything (real).
49 fds installed, 7/7 lines identical. With `-N` the replay diverges at the
first line: `map=EBADF` instead of the file's first bytes.

**Redis**: `tests/test_redis.sh` checkpoints a redis 7 server holding 200
keys, then drives it with `redis-benchmark` (20 clients, SET/GET/INCR/LPUSH
on 1000 random keys) and a few `redis-cli` commands: about 61 500 syscalls
in generation 1. The restored copy is replayed with `-S -e live`; when the
recording is used up it runs for real, and its `DEBUG DIGEST` (a hash of
the whole dataset) must equal the live server's. It does, locally and on
claude-test (5 of 5 runs with `-S`). Without `-S`, 1 run in 5 diverged when
a jemalloc background thread found a mutex held by the main thread.

**nginx**: `tests/test_nginx.sh` records a master and two workers
(`-f`), drives them with 220 requests after the checkpoint and replays the
three-process tree with `-T`. The access log lines (request ids from
nginx's PRNG, times, worker pids, connection numbers) and error log lines
the replayed workers write are identical to the live ones, worker by
worker; afterwards the restored nginx serves real requests. With `-S`, 1 run
in 3 diverged on the shared connection counter. Lines of *different*
workers can appear in another order than in the live log: the replay
follows the order in which the workers entered `write`, while the live
log shows the order in which those writes landed in the shared pipe.

**Across machines**: `REMOTE=host tests/test_replay.sh` records and
checkpoints here, restores and replays on `host` over ssh. All three targets
pass from claude-dev to claude-test (same CPU model, Debian 13, kernels
6.12.113 / 6.12.115). A machine that has not started a podman container
since boot lacks podman's AppArmor profile and CRIU's restore fails ("can't
write lsm profile -2"); the test starts a throwaway container first.

### Replay limitations

* A CONTINUE'd syscall's result is not visible to the supervisor, so it is
  not compared with the recording.
* Installed fds are stand-ins: sockets are unconnected, a memfd is empty,
  a file missing on the replay machine becomes `/dev/null`. Fine during the
  replay; with `-e live` the process continues with those stand-ins.
* Not replayed: signals, io_uring, shared memory written by other processes,
  rdtsc, vDSO time (use the acpi_pm workaround on the replay machine too).
* The filter cannot be removed: the supervisor must outlive the target, CRIU
  cannot checkpoint it again, and `no_new_privs` stays set.
* Threads created during replay may get other pids than recorded; they are
  matched to recorded `clone` results in creation order.
