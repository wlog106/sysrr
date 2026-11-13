# RECORD.md: project knowledge that must survive

Working notes for whoever picks this project up next (including Claude after
a context reset). DESIGN.md explains the mechanisms; this file keeps the
**facts, decisions, measurements and traps** in one place. Keep it current.

This file: goal, environment, files, and the **record side**. The replay
side (sysrr_replay, ADDFD, cross-machine restore) is in **REPLAY.md**.

Project dir: `~/src/ebpf-dev/syscall-rr` (it was `~/ebpf-dev/syscall-rr`
before 2026-11; old notes and the guide may still say that).
Walkthrough for newcomers (interactive, 2026-11-12): `docs/tour.html`,
https://claude.ai/artifact/YJn3muzGnxV1wMH1Sd2ycT.
Study guides: record side https://claude.ai/artifact/6ZebwgVF4vFLJRKdhWRsFA,
replay side `docs/replay-guide.html`, published at
https://claude.ai/artifact/BrqbnVeMN3qqisG1nxenKb (publish the same file
path to update it; from another conversation pass that URL).

## Goal

Record a container process's syscalls with eBPF, `podman container checkpoint
--leave-running`, restore the tarball elsewhere, then make the restored
process reach the live process's later state by feeding it the recorded
syscall results. The user's purpose: **find what made the original target
fail** (including race bugs). The user wants to understand the concepts, not
just get code, so explain mechanisms.

## Environment

Debian 13 VM, kernel 6.12 (`CONFIG_FUNCTION_ERROR_INJECTION` **off**, so no
`bpf_override_return` / fmod_ret on syscalls; `CONFIG_BPF_LSM=y`,
`CONFIG_SECCOMP_FILTER=y`), libbpf 1.5, clang 19, bpftool in /usr/sbin,
podman 5.4.2 + crun 1.21, criu 4.1.1. User xuanhh, passwordless sudo.
`grep` is aliased to ugrep (no PCRE lookahead). No gdb installed.

## Files

| file | what |
|---|---|
| `sysrr.bpf.c`, `sysrr.c` | recorder (BPF side, loader/writer) |
| `sysrr_dump.c` | decoder: `-g GEN`, `-s` integrity summary, `-r` races, `-x` hex |
| `sysrr_replay.c` | replayer (seccomp user notification supervisor) |
| `sysrr.h` | log format shared by all |
| `tests/run_all.sh` | **the test suite: start here.** Every target, one results table (`REMOTE=`, `ROUNDS=`, name filters) |
| `tests/sc_target.c`, `sc_known.txt` | syscall conformance: about 100 cases, one line + output hash each; a gap names its syscall; known limits listed in the txt |
| `tests/fork_target.c`, `thread_target.c` | hundreds of new processes / threads inside a generation |
| `tests/rand_target.c` | glibc getrandom via the vDSO: not in the log (known limit) |
| `tests/exit_target.c`, `signal_target.c` | target ends or crashes inside the recording; async signal (must be reported) |
| `tests/sh_target.sh`, `py_target.py` | dash script (SIGCHLD timing: may diverge, must end by itself); single-threaded Python (subprocess, late dlopen, SQLite) |
| `tests/test_ckpt.sh` | record across a checkpoint (old, record-only) |
| `tests/test_race.sh`, `race_target.c` | race check (`sysrr -r`) |
| `tests/test_replay.sh`, `tamper_log.py` | record → checkpoint → restore → replay, plus tampered-log negative test |
| `tests/mt_target.c` | 2-thread target for `-S` (strict order) |
| `tests/fd_target.c` | creates fds after the checkpoint and mmaps one (needs ADDFD; `-N` shows the failure) |
| `tests/test_nginx.sh`, `tests/nginx/nginx.conf` | nginx master + 2 workers recorded with `-f`, replayed with `-T`, access/error logs compared |
| `tests/net_target.c`, `file_target.c` | TCP/UDP over loopback with epoll; a file tree with big reads, listings, rename, links |
| `tests/reexec_target.c`, `sigsync_target.c` | execve of itself every few rounds; signals a program sends itself (raise, SIGPIPE, SIGSEGV, sigqueue, sigwaitinfo) |
| `tests/sigext_target.c` | signals from outside (test_replay.sh `KICK=USR1+TERM` sends them with podman kill) |
| `tests/pool_target.c` | thread pool (mutex + condition variable); `burst` argument wakes all workers at once |
| `tests/perl_target.pl`, `py_async_target.py`, `py_threads_target.py`, `py_mp_target.py` | Perl; Python asyncio; Python threads (GIL); multiprocessing |
| `tests/test_memcached.sh`, `mc_client.pl` | memcached 1.6, 4 worker threads, 4 parallel clients, dataset compared (`MODE=race`: clients share keys) |
| `tests/test_redis.sh` | redis 7: record, checkpoint, redis-benchmark, restore, replay -S -e live, compare DEBUG DIGEST (REMOTE= supported) |
| `docs/guide.html` | record-side study guide (published artifact) |
| `docs/replay-guide.html` | replay-side guide with self-test (published artifact) |
| `docs/old/` | the longer guides from before the 2026-11-06 simplification (diagrams, line-by-line detail) |

## Record side: key facts

* Tracepoints `tp_btf/sys_enter` + `sys_exit`, filtered by tgid (or a
  `targets` map with `-f`). Per-thread state in TASK_STORAGE `tstate`.
  Records go to a RINGBUF; user space (`sysrr.c`) just copies the ring to the
  file (it is a dumb pipe).
* Log = 128 B file header + records. `REC_SYSCALL` (120 B, commits a seq),
  `REC_DATA` (32 B header + ≤16 KiB chunk, via PERCPU `scratch` +
  `bpf_ringbuf_output` because size is variable), `REC_MARKER` (CRIU events),
  `REC_STATS`. DATA records of a seq come **before** its SYSCALL record.
* `seq` is a global counter taken at sys_enter → total order of syscall
  **entries** across threads. Replay strict mode relies on this.
* Inputs (`D_IN`, `D_STR`) copied at **entry**, outputs (`D_OUT`) at
  **exit**, in/out args at both. Entry copy faults on non-resident pages
  → re-copied at exit (`DF_EXIT`). `-r` re-copies all inputs at exit to
  detect buffers changed mid-syscall (race check). A write to /dev/null of a
  never-touched page stays `F_FAULT`.
* Data piece addressing: `arg` (which syscall arg), `idx` (iovec number;
  0xffff = the iovec array, 0xfffe = msghdr itself, 0xfffd = msg_name,
  0xfffc = msg_control), `offset` inside the buffer.
* CRIU detection: ptrace(SEIZE/ATTACH/DETACH) aimed at a target thread.
  While seized, its syscalls get `F_DUMP` (parasite, ~350 syscalls, never
  replay them). Last DETACH → `gen++` and an `M_GEN` marker.
  **Generation N = what a restore of checkpoint N must replay.**
* The syscall in flight at checkpoint: -ERESTARTSYS-like → re-run by both
  live and restored (first record of gen N); -ERESTART_RESTARTBLOCK (-516,
  nanosleep) → CRIU saves it as -EINTR, both continue with the next syscall.
* Records carry host tid and `ns_tid` (container pid namespace). CRIU
  restores ns pids, so replay matches threads on `ns_tid`.
* **`-f` (follow)**: the targets map holds every tgid of the tree. At start
  `add_descendants()` adds the processes that *already* exist under `-p`
  (nginx's workers; before 2026-11-03 only later forks were added, by the
  `sched_process_fork` hook). CRIU detection works across processes:
  `seized_cnt` counts seized threads of all targets.
* **Generation boundary is per thread (fixed 2026-11-03)**: CRIU detaches
  threads one by one and `gen` moves at the last detach. A thread detached
  earlier already runs; its syscalls in that window (seen: redis main
  thread, 8 syscalls in ~0.1 ms) were stamped gen 0 and lost to the replay
  ("expected openat, actual clock_gettime" at the first record). Now each
  thread gets `gen_min = gen + 1` at its own DETACH and records
  `max(gen, gen_min)`.
* **Empty string quirk (fixed 2026-11-03)**: `bpf_probe_read_user_str()`
  returns 0, not 1, for `""` (strncpy_from_user_nofault adds the NUL only
  when ret > 0). The recorder took 0 as a fault: every
  `newfstatat(fd, "", buf, AT_EMPTY_PATH)` (glibc < 2.39 `fstat`, e.g. the
  nginx image's Debian 12) was F_FAULT and the log "incomplete". Now ""
  is recorded as 1 byte.
* **Syscall number at exit (fixed 2026-11-06)**: `rt_sigreturn` restores
  `orig_ax` to -1, so its exit looked like "syscall_-1". sys_enter now saves
  `st->nr`; sys_exit uses it when the entry was seen.
* **`F_NOENTER` has two more meanings in gen >= 1**: a new thread/process's first
  record is `clone = 0` (it never entered the call), and a call refused by the
  container's seccomp profile skips the entry tracepoint but not the exit one.
  Neither reaches a seccomp listener; replay skips all of them at load.
* **Output table (extended 2026-11-06)**: about 40 calls were missing from
  `capture_exit`: statfs, xattr, F_GETLK, sendfile/copy_file_range/splice
  offsets, timers, SysV/POSIX IPC, getgroups, capget, sched_get*, mincore,
  recvmmsg/sendmmsg, plus generic `_IOR` ioctls. `tests/sc_target` checks them.
  recvmmsg tags pieces of message i with arg = 8 + i. Still missing:
  `semctl(GETALL)` (size known only in the kernel).
* **recvmsg's input msghdr uses idx 0xfffe (fixed 2026-11-06)**: it had idx 0,
  which replay read as "iovec 0" and compared with the wrong memory.
* Overhead (dd bs=1, worst case): ~2.8× (was 5.6× in the original
  prototype; the fix was not waking user space per record:
  `BPF_RB_NO_WAKEUP` until ¼ full + 10 ms poll). `-r` adds ~11%. Unrelated
  processes pay ~0.03 µs/syscall.
* **vDSO gap**: clock_gettime/gettimeofday/time don't enter the kernel.
  Workaround for experiments: `echo acpi_pm > .../clocksource0/current_clocksource`
  (restore `kvm-clock` afterwards). Needed on **both** record and replay
  machines. Tests switch it themselves.
  **It does not cover `time()` or the `*_COARSE` clocks** (seen 2026-11-12 with
  Perl's `time`): the vDSO answers those from its data page whatever the
  clock source, so they are never in the log and the replay gets the real
  time. glibc's `time()` is that vDSO call; Python's `time.time()` uses
  clock_gettime and is fine.
* **vDSO getrandom**: the same gap for random bytes, and no clocksource trick
  helps (REPLAY.md, limitations).
* Race check answer given to the user: comparing entry vs exit copies does
  **not** catch 99.99% of race failures; only buffers changed while a
  syscall uses them. Misses races with no syscall, ABA, output-buffer races.

## Commands

```
make
sudo ./sysrr -p PID [-f] [-r] -o rec.log          # record (Ctrl-C stops)
sudo podman container checkpoint --leave-running -e ckpt.tar.gz CTR
./sysrr_dump -s rec.log                             # integrity
sudo ./sysrr -f -p MASTER -o rec.log                # a process tree (nginx)
tests/test_race.sh                                  # race check
tests/test_ckpt.sh                                  # record across a checkpoint
# replay commands: REPLAY.md
tests/run_all.sh                                    # the whole suite
```

**Checkpoint trap**: with `--leave-running`, a file the target keeps growing on
the container's writable layer makes the restore fail (`File x has bad size`):
CRIU notes the size during the dump, podman archives the file afterwards. CRIU
also refuses an open POSIX message queue, a non-empty mqueue filesystem, and an
inotify watch on overlayfs.

## Signals and exits (2026-11-06)

`tp_btf/signal_deliver` and `tp_btf/sched_process_exit` write REC_SIGNAL records
(`sig == SIG_EXIT` for an exit, `code` = status). sysrr_dump prints them as `==== thread N
got signal ...` and `==== thread N ended: status ...`. The siginfo pointer in the tracepoint
is a scalar to the verifier, so it needs bpf_core_read. Default-action fatal async signals
skip delivery: threads get SIGKILL and the real signal is only in the exit status. PID 1 of a
pid namespace ignores default-action signals sent from inside.

## Only criu's ptrace counts (2026-11-13)

handle_ptrace used to treat any PTRACE_SEIZE/ATTACH/DETACH aimed at a target
thread as CRIU: one second of `strace -p` marked 163 of the target's own
syscalls F_DUMP and started a generation no checkpoint belongs to.  It now
returns unless bpf_get_current_comm() of the caller is exactly "criu".
`sysrr -t` restores the old rule (any tracer), for a criu binary that runs
under another name.  Still open: a SEIZE that fails is counted (state changes
at the caller's sys_enter, before ptrace returns).

