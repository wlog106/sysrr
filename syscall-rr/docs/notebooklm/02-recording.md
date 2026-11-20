# sysrr: How the Recorder works

The Recorder (`sysrr`) watches a program running in a container and writes every syscall it makes into a log file. It uses small eBPF programs that the kernel runs at the entry and exit of each syscall, so the program is never stopped or traced, and CRIU can still checkpoint it. For each syscall the log keeps the arguments, the result, and the memory the kernel read or wrote. The log is cut into generations at each checkpoint, so a restored copy knows which part it has to replay.

## Key ideas

- eBPF tracepoints record every syscall of the target without ptrace, so CRIU can still checkpoint it.
- Each syscall becomes one syscall record, plus data records for the memory the kernel read (copied at entry) and wrote (copied at exit).
- Outputs are what the Replay feeds back; inputs are what it checks to catch a Divergence.
- One global sequence number orders all syscall entries across threads and processes.
- The Recorder spots a Checkpoint by watching criu's own ptrace calls; everything after checkpoint N is Generation N.
- Thread ids are also kept as seen inside the container, because those survive a Restore.
- `-C NAME` records a whole container; Follow mode adds new processes and forgets ended ones.

## The Recorder at a glance

The Recorder has two parts: eBPF programs in the kernel that build the records, and a small user program that copies them into a file.

```
target process (in a container)
    | makes a syscall
    v
kernel --- sys_enter -----> args + input buffers --------+
       --- sys_exit ------> output buffers, then commit -+--> ring buffer --> sysrr --> rec.log
       --- signal, exit, fork --------------------------+
       (criu's ptrace calls are watched too: that is how a checkpoint shows up)
```

The user part is a "dumb pipe": it copies records from the ring buffer to the file and writes a stats record at the end. All the decisions happen in the kernel part.

**How it connects:** Log, Generation, Checkpoint, Replay.

## The eBPF programs

Five small programs, attached to kernel tracepoints, do all the recording.

Every process on the machine runs them on every syscall, so for a process that is not recorded they cost only a couple of compares.

### sys_enter

Takes the next sequence number, the time, the syscall number, the six arguments and the generation, and copies the memory the kernel is about to read. It also watches criu's ptrace calls.

### sys_exit

Copies the memory the kernel wrote, then writes the syscall record that "commits" the syscall.

### signal_deliver

Writes a signal record when a recorded thread receives a signal (CRIU's own stops are skipped).

### sched_process_exit

Writes an exit record for a process leader, or for any thread killed abnormally. In follow mode it forgets a process once its last thread has ended.

### sched_process_fork

In follow mode, a new child of a recorded process is recorded too.

**How it connects:** Recorder, Log, Signals from the log, Follow mode.

## Per-thread state

Each recorded thread has a small private memory area (BPF task storage) where sys_enter leaves notes for sys_exit.

The notes are the sequence number, entry time, syscall number, arguments, generation, flags, whether the thread is inside a syscall, and whether CRIU holds it. It also keeps the thread's ids inside the container, looked up once per thread (and again after an execve, which can change a thread's id).

**How it connects:** Generation, Pid pinning.

## The syscall record

One syscall record describes one finished syscall: what was asked, and what came back.

It is 120 bytes: the sequence number (seq), entry and exit times, the pid and tid on the host and inside the container, the generation, the syscall number, six arguments, the return value, and how many data records belong to it. Data records come just before their syscall record and share its seq.

The seq is one counter for the whole target, taken at entry, so it orders syscall entries across all threads and processes. The ids inside the container stay the same after a restore, so the Replay matches threads by them.

### Inputs, copied at entry

Inputs are memory the kernel reads: write and send data, addresses for connect and bind, paths (up to 4096 bytes), sleep times, epoll_ctl events; 33 syscalls have such a rule. They are copied at entry, because another thread could change them while a slow call blocks (and again at exit if the page was not in memory yet). The Replay compares them with what the restored process passes, to catch a Divergence.

### Outputs, copied at exit

Outputs are memory the kernel wrote: read and recv data (only the bytes the return value says are valid), stat results, clock values, random bytes, directory listings, epoll events, addresses, pipe fds, ioctl results, timers; 90 syscalls have such a rule. The Replay writes these bytes into the restored process instead of running the call.

### Big buffers, iovecs and strings

A buffer is copied in chunks of 16 KiB, up to 64 chunks (1 MiB); anything bigger is marked truncated. For readv, writev, recvmsg and sendmsg the iovec array is copied, then each piece (up to 16). Each data record says which argument, which piece and which offset it holds. Flags on the syscall record mark CRIU's parasite calls (F_DUMP), truncated buffers, unreadable memory and lost data.

**How it connects:** Log, Replay, Divergence, File descriptors, Memory pinning, Serial mode (-T).

## Ring buffer and batched wakeups

Records travel from the kernel to the user program through one shared ring buffer, 64 MiB by default (`-b MB`).

Waking the user program for every record made the first version 5.6 times slower. Now the kernel wakes it only when the ring is a quarter full, and the user side also checks every 10 ms; markers and signals wake it at once. If the ring is ever full, records are lost and counted, and sysrr warns at the end.

**How it connects:** Log, Recorder.

## Choosing what to record

The Recorder records one process, a process tree, or a whole container.

### One process: -p PID

That process and all its threads. Recording stops by itself when the process is gone.

### A container: -C NAME

sysrr asks podman for the container's first process and turns on follow mode, so that process and every process it forks are recorded, including new ones. A process started with `podman exec` is not a child of the container's first process, so it is not recorded.

### Follow mode: -f

A table of recorded processes (up to 4096 at a time) decides who is recorded. The programs are attached first; then the processes that already exist under the target are added, so a fork that happens meanwhile is caught by the fork hook instead of being missed. From then on each fork adds its child. A cgroup filter first rejects everything outside the container with one compare.

### Ended processes are forgotten

When a process's last thread ends, it leaves the table. Without this, the table filled up after 4096 processes and new ones went unrecorded, silently: a test that ran /bin/true 6000 times logged only 4094 of them; now it logs all of them. A child that cannot be added because the table is full is counted as lost, so the warning shows. The last threads of a process keep their exit records even after the process has left the table. In follow mode sysrr stops by itself once the table has been empty for three polls in a row (every recorded process has ended).

**How it connects:** Follow mode, Pid pinning, Test suite.

## Generations

A generation is the part of the log after a checkpoint: generation N is what a restore of checkpoint N has to replay.

### Spotting the checkpoint

CRIU checkpoints a process by attaching to every thread with ptrace, running "parasite" code inside it, and detaching. The Recorder sees these calls, counts how many recorded threads CRIU holds, and when the last one is released, the generation goes up by one. An attach counts only when it succeeded (a failed one, on a thread that just died or that strace holds, used to stay counted and freeze the generation), and a thread that dies while CRIU holds it releases its count. Markers in the log show each step. A checkpoint attempt that fails still moves the generation.

### A boundary per thread

CRIU releases threads one at a time, and a thread released early already makes syscalls before the counter moves (redis's main thread once made 8 in about 0.1 ms). So each thread gets its own minimum generation (`gen_min`) at its release, and a thread or process it creates in that window inherits it.

### Parasite syscalls

While CRIU holds a thread, its syscalls are CRIU's parasite code, a few hundred per checkpoint. They are flagged F_DUMP and never replayed.

### Only criu counts

Only ptrace calls by a process named exactly `criu` count. Otherwise strace would look like a checkpoint: one second of it once marked 163 of the target's own syscalls as parasite calls. `-t` counts any tracer, for a criu binary under another name.

**How it connects:** Checkpoint, Restore, Replay, Checkpoint signals.

## Signal and exit records

A signal record says that a recorded thread received a signal, where it was, and who sent it; an exit record says that a thread ended.

A signal record holds the signal number; its si_code, which tells a timer, the kernel or another process apart; whether it is fatal; the fault address for crashes; the instruction and stack pointers where the thread was; the handler's address; and the time. Its seq is the next one to be handed out, so the signal came after every syscall before it. An exit record holds the exit status.

The Replay uses them to deliver timer and outside signals at the same place, to deliver a signal that landed during the checkpoint at the exact instruction, and to show how a crash ended.

**How it connects:** Signals from the log, Checkpoint signals, Replay, Divergence.

## The log file

The log is a 128-byte header followed by a stream of records, each starting with its type, flags, length and seq.

The header holds a magic number, the format version, the target's pid (host and container), whether follow mode was on, the start time, the kernel version and the program's name.

| record | written when | holds |
|---|---|---|
| syscall | at syscall exit | one finished syscall |
| data | just before its syscall record | a chunk of copied memory |
| marker | CRIU attaches, releases, new generation | thread, tracer, generation |
| signal | a signal is delivered, a thread ends | see above |
| stats | at the end | counts, records lost |

Exit status of sysrr: 0 means the log is complete, 2 that records were lost (record again with a bigger `-b`), 3 that the log could not be written (for example a full disk).

**How it connects:** Log, Generation, Replay, Test suite.

## sysrr_dump

sysrr_dump prints a log in readable form and checks that it is complete.

- `-s`: summary. Syscalls per generation, records lost, missing sequence numbers, incomplete syscalls, calls in flight at stop; "OK: log is complete" or a warning.
- Calls in flight at stop: a thread blocked in a syscall (epoll_wait, a futex wait) when recording stops took a sequence number that no record commits. sysrr writes an "in flight" marker for each such call when it stops, so the dump does not count it as a gap. sysrr also attaches its exit hook before its entry hook, and detaches the entry hook first, so no call made during start or stop leaves a hole.
- `-g N`: only generation N, which is what a restore of checkpoint N replays.
- `-a`: also show CRIU's parasite syscalls. `-x`: hex dump of the buffers.
- `-r`: race check. With a log made by `sysrr -r` (about 11% slower), inputs are copied again at exit; sysrr_dump shows buffers another thread changed while the syscall was using them. It only catches that kind of race.

**How it connects:** Log, Generation, Divergence.

## The vDSO gap and acpi_pm

Some calls never enter the kernel: glibc answers them in user space through the vDSO, so no tracepoint sees them.

With the usual clock sources, clock_gettime and gettimeofday go through the vDSO. Switching the clock source to acpi_pm turns them into real syscalls, so they get recorded; it is needed on both machines (sysrr_restore.sh switches the replay machine). It does not help time() or the coarse clocks, so the replay sees the real time for those. glibc's getrandom (kernel 6.11 and glibc 2.41 or later) also stays in the vDSO; a replay on another machine reports a divergence.

**How it connects:** Replay, Divergence, Restore.

## Overhead

Recording slows the target, because every syscall runs two programs and copies its data.

On the worst case, dd with 1-byte reads and writes (1 million syscalls, median of 5 runs), the run took 109 ms with no recorder, 293 ms with sysrr, and 272 ms with crr, a second implementation written by Gemini for comparison. Per syscall, entry takes about 98 ns (crr: 62) and exit about 120 ns (crr: 130). The difference is copying each write's data, which is what lets the Replay catch a wrong write, so it stays. Other processes barely notice: the same dd took 132 ms instead of 103 ms while sysrr watched an idle target.

**How it connects:** Divergence, Test suite.

## Where in the code

- `sysrr.bpf.c`: sys_enter, sys_exit, capture_enter, capture_exit, emit_buf, emit_iov, emit_syscall, handle_ptrace, on_signal, on_exit, on_fork
- `sysrr.c`: main, handle_rec, add_descendants; `podman_pid.h`: podman_pid
- `sysrr.h`: the log format; `sysrr_dump.c`: on_syscall, check_race

## Common questions

**Why eBPF and not ptrace?**
CRIU needs ptrace to checkpoint the process, and a thread can have only one tracer. ptrace is also slow.

**Why are inputs copied at entry but outputs at exit?**
The kernel reads inputs near entry, and another thread could change them while a slow call blocks. Outputs exist only when the call is done, and the return value says how many bytes are valid.

**What happens if the ring buffer fills up?**
Records are lost and counted; sysrr warns and exits with status 2. Record again with a bigger `-b`.

**How does the Recorder know a checkpoint happened?**
It sees criu attach to the target's threads with ptrace and release them. The last release starts a new generation.

**Can I use strace or gdb on the target while recording?**
Yes. Only ptrace calls by a process named criu count as a checkpoint.

**Why does the log keep two sets of ids?**
Host ids change on another machine. Ids inside the container are restored by CRIU, so the Replay uses those.

**Why not skip cheap syscalls like gettid to go faster?**
In real servers they are at most about 8% of calls (nginx), and every recorded call is one more check that the replay is on the same path.

**Are time() calls recorded?**
No. glibc answers time() in user space even with acpi_pm.
