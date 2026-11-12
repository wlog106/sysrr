# REPLAY.md: replay-side knowledge that must survive

Companion of RECORD.md (goal, environment, files, record side). Same
purpose: the **facts, decisions, results and traps** of `sysrr_replay`, so
they survive a context reset. DESIGN.md "Replay" explains the mechanisms;
the study guide is `docs/replay-guide.html`, published at
https://claude.ai/artifact/BrqbnVeMN3qqisG1nxenKb (publish the same file
path to update it; from another conversation pass that URL). Keep this
file current when the replay changes.

Status (2026-11-12): 11 more targets added to the suite (25 tests); they found
the bugs listed under "More targets (2026-11-12)". `-T` changed a lot: read that
section before touching it.
Status (2026-11-06): working; `tests/run_all.sh` is the suite (see "Test suite"
below). Earlier status (2026-11-03): working. Same-machine and cross-machine
(claude-dev → claude-test) restore + replay pass for ckpt_target,
mt_target (-S), fd_target, **redis 7** (-S or -T, dataset digest identical)
and **nginx** master + 2 workers (multi-process, -T, logs identical).

## Workflow in one picture

```
machine A                               machine B
container --sysrr--> rec.log ---------> sysrr_replay -g N rec.log
    |  checkpoint N (--leave-running)          ^ seccomp listener
    v                                          |
ckpt.tar.gz -----------------------------> restore (leave-stopped) -> pid
```

## Replay side: key facts (sysrr_replay, 2026-11-03)

* **Mechanism: seccomp user notification** (`SECCOMP_RET_USER_NOTIF`).
  The kernel parks each syscall of the target and asks our supervisor; we
  answer "here is the result" (val/error) or "run it for real"
  (`SECCOMP_USER_NOTIF_FLAG_CONTINUE`). Outputs are written into the
  target's memory with `process_vm_writev`; inputs read with
  `process_vm_readv`.
* Why not eBPF: no `bpf_override_return` on this kernel (and it can only
  change return values); `bpf_probe_write_user` is unsafe/tainting.
  Why not a launcher: a restored process never execve()s, so the filter
  must be **injected**.
* **Getting the restored process stopped**: CRIU reads
  `/etc/criu/default.conf` even when podman/crun drive it through libcriu.
  Put `leave-stopped` there for the restore only (it would also leave dumped
  tasks stopped, so remove it right after). Then `podman container restore`
  returns with the process in state `T`. Verified.
* **Injecting the filter** (`attach_listener`): PTRACE_SEIZE +
  INTERRUPT, save regs, write `0f 05 cc` (syscall; int3) at rip, then make
  the target run: `mmap` (scratch page) → write sock_fprog → `prctl(NO_NEW_PRIVS)`
  (container lacks CAP_SYS_ADMIN) → `seccomp(SET_MODE_FILTER,
  NEW_LISTENER|TSYNC|TSYNC_ESRCH)` → `pidfd_getfd` copies the listener fd →
  `munmap`, `close` (these two are trapped by our own new filter, so the
  injector answers them with CONTINUE) → restore code & regs (incl.
  `orig_rax`; injected calls use `orig_rax=-1` to avoid restart logic) →
  PTRACE_DETACH (task returns to group-stop) → `kill(SIGCONT)`.
  ptrace is used for milliseconds, after CRIU is done: no conflict.
* Filter: x86_64 native syscalls → USER_NOTIF; other arches/x32 → ALLOW.
  Precedence: podman's own seccomp ERRNO/KILL still wins over USER_NOTIF.
* **Per syscall**: ns_tid from `/proc/<tid>/status` NSpid → that thread's
  queue (gen N, no F_DUMP, ordered by seq) → next record:
  1. nr differs → DIVERGENCE (hard stop);
  2. args differ (table of arg counts, pointers included: the address space
     is the restored one so they must match) → warning;
  3. recorded inputs vs replayed memory differ → DIVERGENCE (or warning
     with `-c`). This is what catches a wrong state: the program writes
     something different;
  4. process-internal syscall (`passthrough()`: mmap/brk/mprotect, signals
     setup, clone, futex, exit, getpid...) → CONTINUE (run for real);
  5. everything else → **fake**: write D_OUT pieces, reply recorded ret.
  Addresses of pieces are resolved from the *current* args (and iovec /
  msghdr read from the target) before writing anything.
* Records with ret -512..-516 (signal-interrupted) are skipped at load; a
  leading `restart_syscall` that the replay never issues is skipped
  on mismatch. Signals themselves are **not** replayed.
* End of a thread's queue → its next syscall is held (unanswered). When all
  records are consumed: `-e stop` (default) SIGSTOP the target, keep the
  tool running (if it exits, the target's syscalls get ENOSYS); `-e live`
  answer CONTINUE forever (target goes on for real); `-e kill`.
* `-S` strict order: a thread's syscall is released only when its record is
  the globally next seq; others are parked (unanswered). Orders syscall
  **entries** only; user-space work between syscalls is ordered only
  because each thread is blocked at its next syscall.
* **New threads and processes (2026-11-06)**: their first record (`clone = 0`,
  F_NOENTER) is skipped at load. Just before a real clone, `set_next_pid` writes
  recorded-1 to `/proc/sys/kernel/ns_last_pid`. That file acts on the writer's
  *active* pid ns, which setns() changes only for children: fork helper, setns,
  fork again, write, exit. So fork()'s real result, getpid and the recorded wait4
  agree. Fallback if the id is taken: recorded clone results are queued and given
  to unknown new threads in order, with a warning. `wait4` stays faked (a real
  wait4(-1) does not follow the recording once WNOHANG polls are involved), so
  children stay zombies until the parent exits.
* **Timing-only calls (2026-11-06)**: futex, rt_sigreturn and
  clock_gettime(MONOTONIC) only show how a thread got through a lock. A recorded
  one the replay does not make is dropped; one it makes that is not recorded runs
  for real (the real clock: a frozen clock made a timeout loop spin forever).
  Matched by nr + futex address + op. Counted in the totals line.
* **No-progress report (2026-11-06)**: no record used for 20 s -> reported as a
  divergence, with who is waiting for whom (`explain_stall`).
* Zombies in the restored tree are skipped (ptrace refuses them: EPERM).
* Signals to the tree go through pidfds; `procs[]` reuses slots of dead children;
  the poll array is rebuilt each loop.
* `-w` compares the file behind the fd with the target's original stdout/stderr
  (a child's fd 1 redirected into a pipe is not stdout), and reads the buffer
  before answering (the target may reuse it once it runs).
* recvmsg with SCM_RIGHTS: /dev/null stand-ins at the received numbers.
* **Threads with no record in gen N** (redis bio_close_file etc.): they were
  blocked in one syscall the whole recorded period, and a syscall that never
  returns is never recorded. At attach, every thread in /proc/PID/task
  without a queue gets an empty one → its syscall is held (stays blocked).
  Before this fix they were reported as "not in the recording" (only
  threads created during the replay should be). Whether a blocked bio
  thread has a record (its CRIU-interrupted -512 exit) varies run to run.
* **mmap address pinning (2026-11-03)**: a passthrough `mmap` without
  MAP_FIXED (or `mremap` MAYMOVE that moved) lands where the kernel chooses:
  below `mm->mmap_base`, randomized at execve. CRIU restores every mapping
  at its old address but NOT mmap_base (the restored task has CRIU's), so
  new mappings land elsewhere than in the recording. jemalloc then takes
  another path (misaligned chunk → maps 0x600000 to trim; seen in redis as
  "expected read, actual mmap"). Fix (`needs_pin`, `pin_address`): don't
  answer; PTRACE_SEIZE + PTRACE_INTERRUPT that thread → the seccomp wait
  returns -ERESTARTSYS → ptrace stop with rax=-512, orig_rax=nr → set
  rdi=recorded address, r10|=MAP_FIXED_NOREPLACE (mremap: r10|=MREMAP_FIXED,
  r8=address) → DETACH → kernel restarts the syscall with those registers
  → new notification, pinned → CONTINUE (arg check skipped for it).
  ptrace again, but per mapping only, after CRIU, few hundred µs.
* Faked writes don't reach the container's stdout: `-w FILE` saves what
  the target writes to fd 1/2, and to any fd open on the same file
  (`is_stdio`: nginx opens /dev/stdout as its own fd).
* **Multi-process (2026-11-03)**: a seccomp filter belongs to the process
  that installs it (TSYNC = its threads) and to children forked *later*.
  Processes that already exist at restore (nginx workers) need their own:
  `main` collects the tree under `-p` (ppid walk over /proc, all stopped by
  CRIU), runs `attach_listener` on each before any resumes, keeps
  `procs[] = {tgid, lfd, pidfd}`; `supervise` polls all listeners; `lfd`
  = the listener of the notification being handled (held/parked ones
  remember theirs); `pidfd_for(tid)` (Tgid from /proc) for pidfd_getfd;
  stop/kill/SIGCONT go to every process; the replay ends when every
  listener hung up. Queues are per ns_tid, so processes need nothing else.
* **`-T` serial order (2026-11-03, reworked 2026-11-12: see "More targets")**:
  one thread runs user code at a time. Until 2026-11-12
  syscalls were released in the order they *returned* in the recording
  (`ts_exit`); now in the order the thread's *next* syscall was entered
  (`order_serial`, `byexit[]` keeps its name). A thread is "running" from its answer until its
  next notification, except after a CONTINUE that blocks (futex
  WAIT/WAIT_BITSET/LOCK_PI*, rt_sigsuspend, pause, wait4, waitid, exit*).
  Safety valve: a running thread silent for 2 s is no longer waited for.
  clone and execve are ordered by ts_enter, not ts_exit: a child's first calls
  return before the parent's clone, and a vfork parent wakes in the middle of the
  child's execve. Ordered by return time, each side waits for the other.
  Why: -S orders syscall *entries*; after A's syscall is answered, B's next
  syscall can be released while A still runs user code, so writes to shared
  memory between syscalls race. -T runs the stretches of user code one
  after the other in their recorded start order. Not a guarantee (two
  stretches that overlapped live are serialized by start time), but it
  fixed nginx's case completely.
* **fd installation (ADDFD, 2026-11-03)**: a faked fd-creating call (open*,
  creat, socket, accept*, pipe*, socketpair, dup*, fcntl F_DUPFD*, eventfd,
  epoll_create, timerfd, memfd, inotify, ...) with recorded ret >= 0 gets
  a **real fd at the recorded number** via `SECCOMP_IOCTL_NOTIF_ADDFD`
  (`SETFD` = exactly that number, `SEND` = also answers the syscall with it,
  atomically; pipe/socketpair: two ADDFDs without SEND, numbers from the
  recorded int[2], then a normal reply). What is installed (`install_fds`,
  `make_fd`, `open_like`):
  - open family: the same path opened by the supervisor with
    `openat2(/proc/PID/root or cwd or pidfd_getfd(dirfd), RESOLVE_IN_ROOT |
    RESOLVE_NO_MAGICLINKS)` (we are host root: no escapes via symlinks),
    without O_CREAT/O_TRUNC/O_EXCL (no side effects), O_NONBLOCK during the
    open (FIFOs), retried read-only on EACCES/EROFS; missing file → /dev/null.
  - socket: `socket()` of the same domain/type in *our* netns (inert, its
    traffic is faked); accept: unconnected socket like the listener
    (SO_DOMAIN/TYPE/PROTOCOL via pidfd_getfd); pipe/socketpair/eventfd/epoll/
    timerfd/memfd/inotify: a fresh object of that kind; dup*: pidfd_getfd of
    the target's own source fd (same open file); everything else /dev/null.
  - cloexec from each call's own flag (`fd_cloexec`).
  - Data still comes from the log: the fd only has to be real for syscalls
    that run for real (mmap, close, fchdir, and everything in `-e live`).
* Consequence: `close`/`close_range` are now **passthrough** (real), plus
  `chdir`/`fchdir`, so the fd table and cwd follow the recording. A warning
  "fd N is already open ... (it was free in the recording)" = the fd tables
  drifted. `-N` turns installation off (old behaviour, close faked again).

## Replay results (2026-11-03)

* `tests/test_replay.sh` (ckpt_target): gen 1 = 44 records, 43 replayed
  (1 = the -516 nanosleep interrupted by `podman pause` at the end), all
  faked; 11/11 lines identical to the live process; then target held at
  its next `clock_nanosleep` (seen in /proc/PID/syscall = 230).
* Negative test: one flipped byte in the 4th gen-1 read → replay stops at
  record #411 `write`: "16 of 29 bytes differ" after exactly 3 good lines.
* `TARGET=mt_target` (2 workers + main, shared atomic acc): without `-S`
  diverges at the first write whose value depends on the other thread
  (expected; a demonstration, not a bug); with `-S` 274/274 replayed, 92/92
  lines identical.
* `-e live`: after replay the restored container printed iter 22, 23, ...
  for real (live process had stopped at 21).

## nginx (2026-11-03): tests/test_nginx.sh

* nginx 1.27.5 image, `tests/nginx/nginx.conf`: `worker_processes 2`,
  `listen 80 reuseport` (each worker its own listen socket, both get
  connections), access log line with `$request_id` (PRNG state),
  `$msec`, `$pid`, `$connection/$connection_requests`, error log to stderr.
  Record with `sysrr -f -p MASTER` (follow: 2 existing workers added).
  After the checkpoint: 200 requests over 8 parallel keep-alive
  connections (`curl -Z`) to /, /id, /50x.html, /nopeN (404) + 20 single
  requests, all via `podman exec`. Replay `-T -e live -w`; compare the
  captured access + error lines with A's; then `curl localhost/id` in B.
* gen 1 ≈ 2400 syscalls over 3 processes; ~2000 faked, ~400 run, ~128 fds
  installed (accepted sockets, opened html files).
* **The race**: nginx's `$connection` = `ngx_connection_counter`, an atomic
  in a MAP_SHARED page made by the master before fork; each worker bumps it
  in user space right after accept4 returns, no syscall. With -S on
  claude-test: 2 of 3 runs passed, 1 diverged on "conn=" (one byte); no
  -S: 0 of 2 (error log "*11" vs "*10"). With -T: all runs passed
  (6/6, then 7/7 more after the recorder fixes; one run failed only
  because the test still compared the error log in strict order).
* **Log order across workers is not syscall order**: the replay's -w
  capture is in recorded seq (write *entry*) order; the live log has the
  order writes *landed* in the shared pipe. Two workers writing within µs
  can swap (seen: 1-5 adjacent pairs, sometimes 96 without -T). So the test
  compares each worker's own lines in order + the set of all lines.
* Recorder bugs found by this test: the empty-string quirk and the
  per-thread generation boundary (RECORD.md).

## Redis 7 (2026-11-03): tests/test_redis.sh

* Flow: redis:7 (`--save '' --appendonly no --enable-debug-command local`,
  6 threads: main, bio_close_file, bio_aof, bio_lazy_free, 2×
  jemalloc_bg_thd) → record → 200 SETs → checkpoint → redis-benchmark
  -n 3000 -r 1000 -c 20 (SET/GET/INCR/LPUSH) + a few redis-cli commands,
  all via `podman exec` inside A → `DEBUG DIGEST` (hash of the whole
  dataset) → pause. Restore B, replay `-S -e live`; when the recording is
  used up B runs for real and `podman exec B redis-cli DEBUG DIGEST` must
  equal A's. No client ever talks to B during the replay.
* gen 1 ≈ 61.5k syscalls, 1.27 MB of data, log 11 MB. Replay: ~61.4k
  faked, ~118 run, 114 fds installed (accepted client sockets, /proc files),
  1-2 mappings pinned, 5 restart records skipped.
* Results: local PASS; claude-test with -S **5/5 PASS** (digest and dbsize
  identical). Without -S: 1 of 5 runs failed (claude-test): a
  jemalloc_bg_thd woke from its condvar and hit the arena mutex held by the
  main thread (`futex(.., FUTEX_WAIT_PRIVATE)` instead of the recorded
  `gettimeofday`): the main thread had raced ahead. Test defaults to -S.
* Traps: the image declares `VOLUME /data` → removing A without `-v`
  leaves an anonymous volume and the restore fails ("volume ... already
  exists"); `podman run --rm` also leaves it → the throwaway AppArmor
  container uses `--image-volume ignore`; a *restored* container's /data is
  recreated as a named volume that `rm -v` keeps → the test removes it by
  name (inspect .Mounts first). `ssh host "cmd > f &"` keeps the
  ssh channel open (live replay never ends): use `setsid -f`.

## Cross-machine (2026-11-03): claude-dev → claude-test

* `REMOTE=claude-test SSH="ssh -i ~/.ssh/to-test" tests/test_replay.sh`:
  record + checkpoint here, restore + replay there. **All PASS** on
  claude-test: ckpt_target 11/11 + tampered byte caught after 3 lines;
  mt_target -S 92/92; fd_target 7/7 with 49 fds installed, and `-N`
  diverges at the first write ("map=EBADF").
* Both VMs: Intel Core Ultra 9 185H, Debian 13, podman 5.4.2, criu 4.1.1;
  kernels 6.12.113 (dev) / 6.12.115 (test). claude-test has no compiler:
  binaries are built here and copied.
* **Traps found**:
  - CRIU restore fails with `can't write lsm profile -2` (then "BUG at
    restorer.c", segfault) on a machine that has not *run* a container since
    boot: podman loads its AppArmor profile `containers-default-0.62.2` only
    when it starts a container. Fix: `podman run --rm IMG true` once first
    (the test does it).
  - The container bind-mounts the tests dir (`-v $DIR:/t`): the same path
    must exist on the replay machine (CRIU checks the binary), so the test
    copies `tests/` and `sysrr_replay` to the same absolute path.
  - The image must exist there: test does `podman save | ssh podman load`.
  - `ckpt.tar.gz` is root-owned 0600: copy with `sudo cat`.
  - `~/.ssh/config` has `IdentityFile .ssh/to-test` (relative to cwd): plain
    `ssh claude-test` works only from `~`; pass `-i ~/.ssh/to-test`.
  - acpi_pm must be set on both machines (test does it).

## AppArmor and "since boot" (explained to the user 2026-11-03)

* Podman confines each container with an AppArmor profile named
  `containers-default-<version>` (here 0.62.2). Podman does **not** install
  it as a file in /etc/apparmor.d (verified on claude-test: nothing there);
  it generates the profile text and loads it into the kernel with
  apparmor_parser the first time it *starts* a container. Loaded profiles
  live only in kernel memory (`/sys/kernel/security/apparmor/profiles`), so
  after a reboot it is gone until podman starts a container again.
* CRIU's dump records the task's LSM label; on restore its restorer writes
  that profile name to `/proc/self/attr/current`. If the kernel does not
  know the profile: ENOENT (-2) → "can't write lsm profile -2" → BUG in
  restorer.c → task SIGSEGV → "Restoring FAILED". `podman container restore`
  does not load the profile itself (it does not go through the start path
  that does).
* Seen on claude-test: profile list empty before, present after one
  `podman run --rm debian:trixie-slim true`; then every restore worked.
  The test runs that throwaway container on the replay machine every time.
* Other possible fixes (not tried): restore with AppArmor relaxed, or load
  the profile by hand. The throwaway container is simplest and keeps the
  same confinement as the recorded container.


## More targets (2026-11-12)

The user asked for more targets. Eleven were added; what each one found:

| target | result | what it found |
|---|---|---|
| `net_target` (TCP+UDP loopback, epoll) | pass | nothing |
| `file_target` (file tree, 200 KB reads) | pass | nothing |
| `reexec_target` (execve of itself) | pass | hundreds of pointer warnings after execve (now silent: addresses are new after an execve of the process itself, `has_execed`) |
| `sigsync_target` (self-sent signals) | pass after fixes | SIGPIPE never arrived with a faked EPIPE; `rt_sigqueueinfo` was faked, so the signal was never sent; a real pending signal was never taken by the faked `sigwaitinfo` |
| `pool_target` (thread pool) | 4 of 6 one-by-one, burst rarely | the whole `-T` rework below |
| `perl_target.pl` | pass | `time()` is not recorded even with acpi_pm (RECORD.md, vDSO gap) |
| `py_async_target.py` (asyncio) | pass | nothing |
| `py_threads_target.py` (GIL) | reported | a sleep's absolute deadline was compared as data (fixed); the GIL itself stays a limit |
| `py_mp_target.py` (multiprocessing) | reported | same GIL limit (each process has a queue feeder thread) |
| memcached 1.6, 4 clients | pass 3 of 3 | lock held across a syscall stalled the replay; buffer addresses inside an iovec array were compared as data |

Fixes, all in `sysrr_replay.c`:

* **SIGPIPE** (`return_with_signal`): a faked write-like call with recorded
  -EPIPE (and no MSG_NOSIGNAL) gets the signal the kernel would send. Same ptrace
  move as `pin_address`: seize, interrupt (the notification wait returns
  -ERESTARTSYS), set rax to the recorded result and orig_rax to -1 (nothing to
  restart), tgkill, detach. The kernel delivers the signal on top of that result:
  handler first, then the program sees EPIPE; or death by SIGPIPE. Sending the
  signal while the thread is parked would restart the call; after the answer the
  program would run a little first. This primitive would also serve to replay
  timer and outside signals at a syscall boundary (not done).
* `rt_sigqueueinfo`, `rt_tgsigqueueinfo` run for real like `kill`.
  `rt_sigtimedwait` and a read from a signalfd run for real when the recorded
  signal is really pending (`takes_pending_signal`), else they stay faked.
* Sleeps are timing-only, so their time argument is not checked either.
* iovec arrays and msghdrs: lengths are compared, addresses are not (counted in
  the totals: "passed buffers at other addresses"). With threads the heap is
  laid out by who allocated first; the bytes are still compared piece by piece.
* **`-T`**, see DESIGN.md "Serial order" for the rules: order by the end of
  each stretch; recorded futex waits answered at their place instead of run;
  a mutex wait (value 2) moves its stretch behind the holder; threads held by
  ptrace at restore and started in order; a lock holder parked behind the
  thread in turn goes first; a wait never left in the recording is held.
  `-v` now prints timing-only calls ("drop" = recorded, not made; "real" = made,
  not recorded): the fastest way to see where a replay leaves the recording.

Measured (same machine, 2026-11-12). Old = the replayer before this work, on
the same recordings:

* pool one by one: old 0 of 6 (died in the first 30 rounds, often the first);
  new 4 of 6 to the last line during the work, 5 of 8 with the final code. Of
  the 3 failures, 2 came within the first 30 records: the start of a generation
  is still weak when threads were restarted inside condition waits (-EAGAIN on
  the restarted wait, main in its first stretch). To get a case: rerun
  `TARGET=pool_target REPLAY_OPTS="-T -v" tests/test_replay.sh` until one fails.
* pool burst (4 workers woken by one broadcast): old stopped within 2 lines;
  new typically reaches line 25 to 55 of 55, 2 of 6 complete. What is left is
  physical: two workers release the lock and enter the kernel within the same
  few hundred nanoseconds, in the other order.
* Order by end / mixed / by start, same 6 pool recordings: 4 / 3 / 1 complete.
  The two-thread test: 4 of 4 with each rule.
* memcached, 4 clients on their own keys: 3 of 3. With shared keys
  (`MODE=race`): 1 of 3 with 2 clients, diverges with 4 or 8, after 1300 to 4600
  of about 4700 syscalls: two requests for one key reach two threads within
  microseconds.

Method that worked: keep one recording and its checkpoint, restore and replay it
again after each change (a replay of the same log is deterministic now), and
compare with the old binary on the same recording. Rebuilding a target binary
invalidates saved checkpoints (CRIU checks the file: restore fails -52).

## Signals from the log (2026-11-12, later the same day)

The user asked to replay signals. Now replayed: handled signals from a **timer**
(alarm, setitimer, timer_create: si_code SI_KERNEL or SI_TIMER) and from
**outside** the recorded processes (podman kill, a shell). `plan_signals` picks
them from the REC_SIGNAL records; `-v` prints each delivery.

* **Where**: the last syscall record of the thread with seq below the signal's.
  - That call was interrupted (ret -512..-516, skipped at load): when the thread
    makes the call, seize + interrupt as for SIGPIPE, put the *recorded* restart
    code in rax, leave orig_rax alone, tgkill, detach. The kernel's own restart
    logic then does what it did live (restart after the handler with SA_RESTART,
    EINTR otherwise).
  - That call completed: the signal goes out with its answer
    (`return_with_signals`, several signals at once if needed). If the call ran
    for real or created fds, the signal is sent just after the answer.
  - It hangs on a timing-only record the replay does not make: delivered before
    the call the thread makes instead (rax -513, always restart).
* **Timers are disarmed at attach** (`attach_listener`: setitimer x3 and
  timer_settime for every id in /proc/PID/timers, injected before the filter).
  A restored timer runs in real time; the program lives in recorded time. Calls
  that arm timers later are faked anyway. With `-e live` the timers stay off.
* **Pending instances are drained at attach** (injected rt_sigtimedwait with a
  zero timeout over the planned signal numbers): a timer that ran out while the
  copy sat stopped would add one delivery.
* **A signal pending at the checkpoint is in the image.** The restore delivers
  it, and the copy wakes up inside the handler; the recorder logged the same
  delivery at the start of the generation. A recorded signal from before the
  thread's first syscall is therefore skipped when that signal is blocked at the
  thread's first notification (it is inside the handler). Found the hard way:
  one tick too many.
* Not replayed this way, on purpose: signals a recorded thread sent (matched to
  the latest kill-family record with that number; the kill runs for real),
  SIGCHLD (real children send it, at their own time: shell scripts still drift),
  SIGPIPE (sent with the EPIPE), faults, and fatal signals (end of recording).
* Approximate case: a signal that arrived while the thread was computing, not in
  a syscall, is delivered at the previous syscall: early by that computation.
  Counted in the totals ("arrived while the thread was computing"). A handler
  that only sets a flag is fine; one whose effect depends on how far the loop
  got is not. The delivered siginfo says SI_TKILL, not the original code.
* Results: signal_target (30 ms itimer, SA_RESTART) 71 of 71 signals, 211 of 211
  lines, 3 of 3 runs; sigext_target (podman kill USR1 and TERM, one restarting
  handler, one not) 8 of 8, 121 lines. Both are suite tests now (`signal`,
  `signal-out`), must pass.

## Replay limitations (tell the user when relevant)

* CONTINUE'd syscalls: their return value can't be checked (seccomp never
  shows it). mmap/mremap addresses are now pinned; MAP_FIXED_NOREPLACE
  failing (address taken) would still only surface later. brk relies on
  CRIU restoring the heap bounds (it does). shmat/other address-choosing
  calls are not pinned.
* Installed fds are stand-ins: a socket is unconnected, a memfd empty (its
  ftruncate/write were faked), a file missing on the replay machine is
  /dev/null (mmap of it fails: ENODEV). Fine during replay; in `-e live`
  the process then talks to those stand-ins. open_by_handle_at, userfaultfd,
  pidfd_open, perf_event_open, io_uring_setup, fanotify → /dev/null.
* The recorder has no path for `creat`/`openat2`; replay reads it from the
  target's memory (not checked against the log).
* Signals, io_uring, shared memory written by others, rdtsc, vDSO time: not
  replayed. Signal-interrupted records are skipped. A real SIGCHLD still
  arrives, at its own time: dash's control flow depends on it, so shell scripts
  often diverge (reported; tests/sh_target.sh checks it never hangs).
* **vDSO getrandom (found 2026-11-06)**: with kernel >= 6.11 and glibc >= 2.41,
  glibc's getrandom() makes the bytes in user space, so they are not in the log.
  The state is in the process's memory, so a same-machine replay gets the same
  bytes unless the kernel's generation moved. On another machine the vDSO reseeds
  with a real `getrandom(key, 32, 0)` the log lacks -> divergence, with a hint
  (`from_vdso`, instruction pointer inside [vdso]). A programme calling the raw
  syscall or reading /dev/urandom is fine. No per-process way to turn the vDSO
  off; booting the record machine with `vdso=0` would turn it into syscalls
  (clock too), not tried. tests/rand_target.c.
* Who won a contended lock is not in the log. `-T` infers it from the futex calls
  (2026-11-12) and gets thread pools and memcached through most of the time; two
  lock releases within the same few hundred nanoseconds can still come out the
  other way. Python threads (GIL, handed over on a 5 ms timer): followed for a
  few dozen calls, then "no progress" after 20 s or a divergence.
* `time()` and the `*_COARSE` clocks come from the vDSO data page even with
  acpi_pm: not recorded, the replay sees the real time.
* Signals: see "Signals from the log". Left: SIGCHLD timing, signals that land
  in the middle of a computation (delivered early), original siginfo.
* A file received over a socket (SCM_RIGHTS): only the number is recorded, the
  stand-in is /dev/null, a real mmap of it fails (sc_known.txt).
* semctl(GETALL): output not recorded (sc_known.txt).
* Faked writes never reach files: a later real mmap sees the old contents.
* The filter cannot be removed: the tool must keep running; CRIU cannot
  checkpoint a process with a seccomp listener. `no_new_privs` stays set.
* `-S` controls syscall entry order only; `-T` also serializes the user
  code between syscalls in recorded start order, but cannot know the order
  of memory accesses *inside* two stretches that overlapped live (a race
  decided by nanoseconds inside overlapping code can still differ). Shared
  memory with processes outside the replayed tree is not covered at all.
* fd_target's `-N` negative test is skipped when the checkpoint lands
  between open() and close() (fd 3 restored for real).
* With `-S` only, a race entirely in user space between two syscalls of
  different threads is timing-dependent (mt_target passes because each
  update sits between two syscalls of its own thread and the other thread
  is parked at a syscall; nginx's counter does not).
* Without `-S`/`-T`, any program whose threads or processes share state
  may diverge.

## Crash diagnosis (2026-11-06): tests/run_crashes.sh, tests/test_crash.sh

Catalogue of 32 deaths. A second recorder watches the replay, verdicts are the last
line (same-death, same-exit, same-fault, same-signal, same-state, differs, hung).
Run with ROUNDS=, REMOTE=, name filters. Reliable on both machines: everything except
race-memory, queue-race, pbzip2, Go and Redis's crash handler.
Last full suite: 14 of 14, 2 rounds, both machines (2026-11-06). See the guide's table.

* **queue-race** (tests/queue_race_target.c, READ_US tuned for a mix), last run, 18 rounds
  past setup: live crashed 15 (13 same-death, 2 reported another way), live exit 0: 3 (replay
  crashed, report says so). A later 12 rounds after output moved to /dev/null, on a different
  timing mix: 12 ran, 3 same-death, 3 same-fault with hidden crash, the rest reported.
  Restore failed -52 in 3 of 22 before that move (growing output file, likely `bad size`; log
  was gone), none in 12 after.
* **Hidden crash**: a thread faults as another calls exit_group: exit 0, but
  signal_deliver saw it. The thread's own exit status has no signal in it, which is how
  `own_end()` detects this. The same check tells a normal exit_group or a default-action
  shortcut (the kernel SIGKILLs the threads) from a real SIGKILL.
* **Real pbzip2 0.9.4** (tests/real/pbzip2, user approved 2026-11-06): source from a third-party
  GitHub copy (guoyuqi020, same as HiGGBORN), audited for calls beyond compression, not compared
  with the official tarball; three added sleep(10) removed; run in containers only. Unmodified:
  0 crashes of 365. pbzip2_delay (-DBUG_WORKER_US=150000 -DBUG_MAIN_US=130000, forced timing):
  about 80% crash. Log alone: addr 0x30 (offset of mut) at pbzip2.cpp:886,
  pthread_mutex_lock(fifo->mut). Replay: same crash 3 of 9 crashing rounds in the 10-round test;
  1 of 4 in the catalogue (a clean-exit match); mutex winners aren't in the log. Binaries are
  not rebuilt by make (libbz2.a came from an unpacked deb in the session scratchpad).
* Fixes from this work: brk failure is judged against the recorded request (after
  execve the live address differs); end reached is also checked when idle for 6 s;
  leftover records of never-started threads don't block the end (recording cut inside
  vfork); timing-only calls past a thread's last record run for real; after 2 s of
  silence under -S/-T one parked thread is released, MAX_RELAX 8 times, reported; sleeps are
  timing-only (poll loops); "quiet" means no record used for 2 s, not only no notification.
* `CRIU checkpointing failed -52` is CRIU's code for *any* failed dump; the reason
  is only in dump.log. Causes confirmed so far: an open TCP connection at the
  checkpoint (`--tcp-established`), and files changing in /dev/shm or the
  container layer while podman archives them. On the python test (no TCP) the
  2026-11-06 note said about 1 in 10; on 2026-11-12 it **did not reproduce**:
  0 failures in 205 looped checkpoints of py_target.py (60 plain, 70 with
  `sysrr -f` attached, 75 also with acpi_pm) and 14 of 14 full test rounds. At
  1 in 10 that cannot be chance, so whatever caused it then is gone or needed
  conditions not present now (a guess: both machines were running suites at the
  time; never verified). All test scripts now print dump.log's Error lines when a
  checkpoint fails (test_replay.sh also keeps `$OUT/dump.log`), so the next one
  will name its cause.
* Harness trap: anything left running on the remote must have no open stdout/stderr, or
  ssh never returns. Watch long runs with a Monitor: a hang here cost 90 minutes.

## Test suite (2026-11-06): tests/run_all.sh

2026-11-12, across machines (record on claude-dev, restore + replay on
claude-test, reachable again): suite 25 of 26 in one round; then `signal`,
`signal-out`, `self-signal`, `asyncio`, `memcached`, `net`, `perl`, `basic` 3 of 3
each. Crash catalogue across machines: same picture as on one machine (differs:
race-memory, pbzip2, go-panic; redis same-fault then divergence).
The one suite failure (`asyncio`) was on the recording side, and its cause is
now known: **"CRIU checkpointing failed -52" = the checkpoint landed while a TCP
connection was open** (dump.log: "Connected TCP socket, consider using
--tcp-established"). Caught by looping run + checkpoint and reading dump.log
before the container is removed. Fix: `CKPT_OPTS=--tcp-established`
(test_replay.sh passes it to checkpoint and restore); podman refuses that
option together with `--name`, so the copy keeps the original's name. Used by
`net`, `perl`, `asyncio`. This probably explains the old "unconfirmed -52" too
where TCP was involved; the `python` test has no TCP, so its -52 is still open.

2026-11-12, after signal replay: 26 tests (`signal` must now pass; `signal-out`
added). One round: 25 passed, `asyncio` hit the known `CRIU checkpointing failed
-52` before any replay and then passed 3 of 3; `signal`, `signal-out`,
`self-signal` 3 of 3. Crash catalogue, one round: as before (differs: race-memory,
pbzip2, go-panic; redis same-fault then divergence; `alarm` same-signal with the
same exit code 142).

2026-11-12: 25 tests. Two rounds here: 24 passed both; `files` failed one round
because the target kept its tree in /dev/shm, which podman archives after the
dump while the target runs on ("Can't add file ... to tar"). Targets that make
files now use `/scratch`, a tmpfs test_replay.sh mounts (CRIU saves a tmpfs
during the dump): `files` and `perl` then 4 of 4. claude-test was unreachable
(no route to host), so nothing new was run across machines. `pool`,
`pool-burst`, `py-threads`, `py-procs` are EXPECT=honest: a pass means "matched,
or stopped by itself with a report". The old replayer's 14 tests passed before
the work (baseline) and after.

Measured flake rates: mt_target -S failed 2 of 9 runs on this machine (user code
overlaps: see limitations), -T passed 6 of 6, so the suite uses -T and it is the
first thing to try for shared memory. nginx -T failed 1 of 9 on claude-test, on
one byte of `conn=`: the documented race inside two stretches that overlapped
live. Rerun a timing-sensitive failure before calling it a regression.

Prefer this over the single scripts. Each test: record, checkpoint, restore
stopped, replay, compare with the live run. `sc_target` is the gap finder: one
line per syscall with a hash of its outputs, replayed with `-c` so one run shows
every gap; fails on anything not in `sc_known.txt`. Do not compare its captured
file line by line: a line of another length is cut at the recorded length and
merges with the next. test_replay.sh options: SYSRR_OPTS, ARGS, IMG,
EXPECT=diverge|honest, KEEP_GOING. Replay is wrapped in `timeout 60`.

## Commands

```
tests/run_all.sh [name...]; REMOTE=claude-test SSH="ssh -i ~/.ssh/to-test" ROUNDS=2 tests/run_all.sh
make
# on the replay machine (once per boot: load podman's AppArmor profile)
sudo podman run --rm docker.io/library/debian:trixie-slim true
echo acpi_pm | sudo tee /sys/devices/system/clocksource/clocksource0/current_clocksource
echo leave-stopped | sudo tee /etc/criu/default.conf
sudo podman container restore --import ckpt.tar.gz --name NEW
sudo rm /etc/criu/default.conf
sudo ./sysrr_replay -p $(sudo podman inspect -f '{{.State.Pid}}' NEW) -g 1 \
     [-S|-T] [-v] [-N] [-c] [-e stop|live|kill] [-w out.txt] rec.log

tests/test_replay.sh                                # ckpt_target + tampered log
TARGET=mt_target REPLAY_OPTS=-S tests/test_replay.sh
TARGET=fd_target tests/test_replay.sh               # ADDFD + -N negative test
REMOTE=claude-test SSH="ssh -i ~/.ssh/to-test" TARGET=... tests/test_replay.sh
tests/test_redis.sh ; REMOTE=claude-test SSH="ssh -i ~/.ssh/to-test" tests/test_redis.sh
tests/test_nginx.sh ; REMOTE=claude-test SSH="ssh -i ~/.ssh/to-test" tests/test_nginx.sh   # -T default
```
