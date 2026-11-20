# sysrr: the big picture

sysrr is a tool for finding out why a program in a container failed, including
failures caused by races between threads. It records every system call the
program makes, takes a checkpoint of the container while it keeps running, and
later restores that checkpoint, on the same machine or on another one. A
replayer then feeds the restored copy the recorded results of its system calls,
so the copy walks the same path the live program took. If the copy ever does
something the recording does not show, the replayer stops and says exactly
where.

## Key ideas

- A system call (syscall) is how a program talks to the outside world, so a record of all of them holds everything the program learned from outside.
- Recording uses eBPF programs attached to the kernel, so it never gets in the way of CRIU, the checkpoint tool.
- A checkpoint is a snapshot of the whole container taken while it keeps running; everything recorded after checkpoint N is generation N.
- The restored copy is left stopped before it runs a single instruction, and the replayer then makes it install a seccomp filter.
- With the filter in place, every syscall of the copy goes to the replayer, which answers it from the log or lets it run for real.
- A divergence is the first moment the copy asks for something different from the recording; it is always reported, never hidden.
- Threads that share memory run one at a time in the recorded order (serial mode), and an order search fills the gaps the log cannot settle.
- A suite of 30 tests, including the real servers redis, memcached, nginx and Apache httpd, checks the whole cycle across two machines.

## The problem sysrr solves

sysrr lets you run the recent past of a containerized program again, exactly,
after something went wrong.

When a server misbehaves, the interesting part is usually the last seconds or
minutes: which requests arrived, in which order, what the clock said, which
thread got a lock first. Rerunning the program does not repeat that, because the
outside world and the thread timing differ every time. sysrr keeps a snapshot
from before the failure (the checkpoint) and a record of everything the program
got from outside after it (the log). Together they reproduce the run in a copy
that can be studied.

**How it connects:** Checkpoint, Log, Replay, Divergence.

## The four steps

Every use of sysrr goes through the same four steps: record, checkpoint,
restore, replay.

```
machine A                               machine B
container --sysrr--> rec.log ---------> sysrr_replay -g N rec.log
    |  checkpoint N (--leave-running)          ^ seccomp listener
    v                                          |
ckpt.tar.gz -----------------------------> restore (leave-stopped) -> pid
```

### 1. Record

Recording means writing down every syscall of the container as it happens.
`sudo ./sysrr -C CTR -o rec.log` records, for each syscall of container CTR, its
arguments, the data passed in, the data the kernel gave back, and the result.

### 2. Checkpoint

A checkpoint is a file holding the container's complete state.
`sudo podman container checkpoint --leave-running -e ckpt.tar.gz CTR` saves it
while the container goes on running, and the recorder starts a new generation.

### 3. Restore

Restoring brings the snapshot back to life as a new container.
`sudo ./sysrr_restore.sh ckpt.tar.gz NEW` does it on the same machine or another
one and leaves the processes stopped at the exact instruction where the
checkpoint caught them.

### 4. Replay

Replaying runs the stopped copy while answering its syscalls from the log.
`sudo ./sysrr_replay -C NEW -g 1 rec.log` answers each syscall from generation 1
and ends with "end of the recording reached" or a divergence report.

**How it connects:** Recorder, Checkpoint, Generation, Restore, Attach, Seccomp listener, Replay.

## The tools

sysrr is a small set of command-line tools, built with one `make` in the
`syscall-rr` directory.

### sysrr (the recorder)

The recorder writes the log, for one process (`-p PID`, plus `-f` to follow
its children) or a whole container (`-C NAME`). It stops on Ctrl-C or when every
recorded process has ended; exit status 2 means records were lost.

### sysrr_dump

The decoder prints a log: `-g N` one generation, `-s` a summary and integrity
check ("is anything missing?").

### sysrr_restore.sh

The restore helper restores a checkpoint left stopped: it adds `leave-stopped`
to CRIU's configuration for the restore only, switches the clock source to
acpi_pm, and checks that the process really is stopped.

### sysrr_replay (the replayer)

The replayer drives the restored copy through one generation. Main options:
`-g N` (generation), `-S` (recorded syscall order across threads), `-T` (serial
mode: one thread's user code at a time), `-e stop|live` (what happens at the end
of the log).

### sysrr_search.sh

The order search helps when a replay with threads diverges: it restores the
checkpoint again and again, tries other thread orders where the log leaves the
order open, and prints one that fits the whole log as a `-F` option.

**How it connects:** Recorder, Log, Follow mode, Restore, Replay, Serial mode (-T), Order search.

## Generation

A generation is the part of the log that belongs to one checkpoint: generation
N is everything the program did after checkpoint N.

To take a checkpoint, CRIU attaches to every thread with ptrace and runs about
350 syscalls of its own inside the process (its "parasite" code, which dumps
memory and state). The recorder watches CRIU's ptrace calls. Syscalls made while
a thread is held by CRIU are flagged and never replayed, and when CRIU lets the
last thread go, the generation number goes up by one. So restoring checkpoint N
means replaying generation N, and the replay starts exactly where the restored
copy starts.

**How it connects:** Checkpoint, Log, Recorder, Replay.

## Replay

Replay means running the restored copy while answering its syscalls from the
log, so it gets exactly the inputs the live program got.

Each syscall of the copy arrives at the replayer, which takes that thread's next
record and does one of two things:

- **Fake it.** Calls that bring information in or have an effect outside (read,
  recv, accept, clock_gettime, getrandom, stat, write, send, sleep) are not run.
  The replayer writes the recorded output bytes into the copy's memory and
  returns the recorded result.
- **Run it for real.** Calls that change the process itself (memory mappings,
  new threads and processes, signal handlers, futex) must really happen, or the
  copy would lack the mapping or thread it expects.

Sleeps are faked too, so a replay runs faster than the live run. Signals from a
timer or from outside are delivered again from the log. When the log is used
up, `-e stop` (the default) stops the copy for inspection and `-e live` lets it
run on for real.

**How it connects:** Seccomp listener, Divergence, File descriptors, Memory pinning, Pid pinning, Serial mode (-T), Signals from the log, Checkpoint signals.

## Divergence

A divergence is the moment the restored copy asks for something the recording
does not show.

The replayer compares each syscall with the next record of the same thread. A
different syscall number is a divergence, and so is different input data: other
bytes in a write, another path in an open. Input data is the usual sign, because
a copy on another path almost always writes something different soon after. The
report names the thread, the record, the expected and actual call, and the
first differing byte; 20 seconds without progress is reported too. A divergence
marks the first point where replay and live run part ways, often exactly what
the user wants to know.

**How it connects:** Replay, Log, Serial mode (-T), Order search, Test suite.

## Why eBPF to record and seccomp to replay

The two halves use different kernel mechanisms because each half has a
constraint the other mechanism cannot meet.

### Recording: eBPF, not ptrace

eBPF lets small checked programs run inside the kernel at fixed hook points,
here the syscall entry and exit tracepoints. CRIU checkpoints a process by
attaching to every thread with ptrace, and a thread can have only one tracer, so
a ptrace-based recorder would make the checkpoint fail. ptrace would also stop
the program twice per syscall. eBPF hooks belong to the kernel, not to the
process, so CRIU never notices them. In the worst case measured, one million
one-byte reads and writes took 293 ms recorded against 109 ms unrecorded, and
unrelated processes pay about 0.03 µs per syscall.

### Replaying: seccomp user notification

A seccomp filter with user notification hands each syscall of a process to a
supervisor, which answers with a result or lets the call run. eBPF cannot do
this on the project's kernel: there is no way to override a syscall's result,
and writing a process's memory from eBPF is unsafe. A process can only install a
filter on itself, and a restored process never calls execve, so nothing can
launch it under a filter. Instead the replayer makes the stopped copy install
the filter on itself, using ptrace for a few milliseconds after CRIU has
finished.

**How it connects:** Recorder, Attach, Seccomp listener, Checkpoint, Restore.

## A worked example: redis

The redis test shows the whole cycle on a real server with six threads.

On machine A, a redis 7 container gets 200 keys, which end up inside the
checkpoint. The checkpoint is taken with `--leave-running`, and then
redis-benchmark sends 3000 requests from 20 clients (SET, GET, INCR and LPUSH on
1000 random keys), plus a few redis-cli commands. All of that is generation 1.
`DEBUG DIGEST` then gives a fingerprint of the whole dataset, and the container
is paused. Generation 1 holds about 61,500 syscalls and 1.27 MB of data, in an
11 MB log.

On machine B (claude-test), the checkpoint is restored left stopped and replayed
with strict order (`-S`) and `-e live`. The replayer fakes about 61,400
syscalls, runs about 118 for real, and installs 114 file descriptors (the
accepted client sockets and some /proc files). When the log is used up, redis
runs live, and its `DEBUG DIGEST` equals machine A's, although no client ever
talked to it: it rebuilt the dataset from the recorded syscalls alone. With `-S`
this passed 5 of 5 times on claude-test. Without it, 1 of 5 runs diverged
because a background thread raced ahead of the main thread, which is why
threads need an order.

**How it connects:** Recorder, Checkpoint, Restore, Replay, File descriptors, Serial mode (-T), Test suite.

## How sysrr compares

This section places sysrr next to two related tools: rr and Gemini's crr.

### rr

rr is a record-and-replay debugger that records a program from its start,
under ptrace, using hardware performance counters to put every thread switch and
signal back at the exact instruction. sysrr records only syscalls, never uses
ptrace while recording, and starts from a checkpoint, possibly on another
machine. It needs no performance counters (the development VM has none); the
price is that a race decided purely in user space, between two syscalls, is not
in the log.

### Gemini's crr

crr is a rewrite of the same idea by Gemini, with sysrr merged in. It traps
about 90 syscalls, matches them by number only (never arguments or data), and
quietly runs on for real when they disagree. On the same 24 targets it passed 20
under its own loose rule, but under sysrr's rule (identical output, no
divergence) it failed five and missed one drift entirely. It records a little
faster (272 ms against 293 ms on the one-million-syscall test) because it does
not copy what writes send, which is what sysrr uses to catch a wrong replay.
sysrr took two of its conveniences: `-C NAME` and a one-command restore.

**How it connects:** Recorder, Replay, Divergence, Test suite.

## Where in the code

- `sysrr.bpf.c`: `sys_enter`, `sys_exit`, `capture_enter`, `capture_exit` (what each syscall records), `handle_ptrace` (generations), `on_fork` and `on_exit` (follow mode), `on_signal`.
- `sysrr.c`: loads the eBPF programs and writes the log.
- `sysrr_replay.c`: `load`, `attach_listener` (injecting the filter), `handle` (answering one syscall), `runs_real` and `passthrough` (fake or run), `order_serial` (serial mode), `plan_signals` and `signals_at_checkpoint` (signals), `supervise`, `finish`.
- `sysrr_restore.sh`, `sysrr_search.sh`, `tests/run_all.sh` (the test suite).

## Common questions

**Do I need two machines?**
No. Restore and replay work on the same machine too, and the test suite runs
either way.

**Does the program have to be changed or rebuilt?**
No. sysrr works on unmodified programs running in podman containers.

**What happens when the replay reaches the end of the log?**
By default the copy is stopped so it can be inspected. With `-e live` it goes on
running for real from that state.

**Why must the restored copy start stopped?**
The filter has to be in place before the copy makes its first syscall. A syscall
the replayer never saw would put the copy off the recorded path.

**What does `-g 1` mean?**
Generation 1: the part of the log after the first checkpoint. With several
checkpoints, use the number of the one that was restored.

**Is a finished replay proof that the copy reached the same state?**
It is strong evidence: every syscall matched the recording, including every byte
the program sent out. The real-server tests also compare datasets or logs at the
end.
