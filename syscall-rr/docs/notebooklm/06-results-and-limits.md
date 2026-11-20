# sysrr: results and limits

This file says what sysrr has been shown to do, and what it cannot do. The
evidence is a suite of 30 tests, each recording a program, checkpointing it,
replaying it on a second machine and comparing the result with the live run.
It also covers real servers, a crash catalogue, and a comparison with crr, a
tool Gemini wrote for the same job. The last part lists every known limit.

## Key ideas

- A test passes only with byte-identical output, no divergence, and the end of the log reached.
- The latest full run on 2026-11-20 passed 30 of 30, recording on one machine and replaying on another.
- Four tests target known limits: they pass only if the replay reports a divergence instead of going wrong silently.
- Thread tests can still fail now and then, when the order search does not find a fitting order of threads.
- Four real servers replay exactly: Redis, memcached, nginx and Apache httpd.
- crr passed 20 of 24 shared targets under its loose rule, failed 5 under the strict one, and never checks written data.
- Most limits come from one fact: the log holds syscalls only, so anything that happens without a syscall is invisible.

## Test suite

The test suite is a set of 30 programs, each put through the whole cycle of record, checkpoint, restore, replay and compare.

The Recorder runs on one machine (claude-dev) and the program is checkpointed
while it keeps running. The checkpoint and the Log go to a second machine
(claude-test), where the copy is restored left stopped and replayed. The pass
rule is strict: output byte-identical to the live run after the checkpoint, no
Divergence reported, and the end of the recording reached.

Some programs hit a known limit on purpose. They are marked "honest": they pass
only if the replay ends by itself and reports the divergence. A silent wrong
replay, or a hang, fails them.

**How it connects:** Recorder, Log, Checkpoint, Restore, Replay, Divergence, Serial mode (-T), Order search.

### Basics

These tests cover single-threaded programs and the basic kinds of syscalls.

| Test | What it proves |
|---|---|
| basic | read, write and sleep replay; a tampered log must be caught |
| tracer | strace on the target is not mistaken for a checkpoint |
| fds | files, sockets and pipes opened after a checkpoint come back |
| syscalls | every syscall's output, about 100 cases |
| exit | the target ends by itself inside the recording |
| crash | the target crashes inside the recording |
| net | TCP and UDP over loopback, epoll, new connections |
| files | a file tree made and removed, big reads, directory listings |
| reexec | the program replaces itself with execve |
| vdso-random (honest) | glibc's getrandom makes no syscall, so the replay must end honestly |

### Processes and threads

These tests cover new processes, new threads, and threads that share memory.

| Test | What it proves |
|---|---|
| threads | two threads sharing memory replay in recorded order |
| fork | hundreds of new processes keep their recorded pids |
| thread-new | hundreds of new threads keep their recorded ids |
| pool | a thread pool handing out jobs one by one (mutex and condition variable) |
| pool-burst | a thread pool with four workers woken at once |
| procs+thr | 2 processes x 3 threads, plus new ones with threads |

### Signals

These tests cover signals from timers, from outside, from the program itself, and during the checkpoint.

| Test | What it proves |
|---|---|
| signal | a timer signal 30 times a second is replayed |
| signal-out | signals sent from outside (podman kill) are replayed |
| self-signal | signals it sends itself: raise, SIGPIPE, SIGSEGV |
| sig-ckpt | a signal that lands while CRIU has the process frozen comes back at the exact place |

### Languages

These tests cover runtimes that do a lot behind the program's back.

| Test | What it proves |
|---|---|
| shell (honest) | dash depends on SIGCHLD timing; the replay must never hang |
| python | subprocess, C extensions loaded late, SQLite |
| perl | fork, pipe, files, a socket to itself |
| asyncio | one epoll loop with many connections |
| py-threads (honest) | Python threads and the GIL |
| py-procs (honest) | Python multiprocessing with locks between processes |

### Real servers

These tests replay real, unmodified servers and check their state, not just their output.

| Test | What is checked |
|---|---|
| redis | Redis 7: the replayed server's dataset digest is identical to the live one |
| memcached | 4 worker threads: the replay rebuilds the live dataset exactly |
| nginx | 3 processes: 220 access-log and 50 error-log lines identical, then it serves live |
| httpd | Apache, processes x threads: every request is served by the same thread of the same process |

**How it connects:** Follow mode, Serial mode (-T), Order search, File descriptors, Pid pinning.

## Latest results

The latest results are the outcomes of the suite runs on 2026-11-20, the day of the comparison with crr.

The latest full run, after the fixes from three code reviews, passed 30 of 30
tests in about 5 minutes. Earlier runs that day passed 29 of 29 (before the
sig-ckpt test existed), 28 of 29, 30 of 30, and twice 28 of 30. Every failure
was either a search miss in a thread test or the python test's checkpoint
catching a vfork (now retried, see Limits).

Some sizes from one run: syscalls reproduced 3900 lines, thread-new 884, fork
373. In sig-ckpt the live signal landed when a loop counter stood at 74,042,352,
and the replay delivered it at exactly that count; the previous replayer
diverged on the same recording.

**How it connects:** Test suite, Order search, Checkpoint signals.

### Thread tests and search misses

A search miss is a failed thread test where the replay diverged and the order search found no order of threads that fits the whole log.

The log does not always say which of two overlapping stretches of user code
touched shared memory first. Serial mode (-T) guesses, and the order search
tries other guesses after a divergence; it can run out. Measured rounds:

| When | Test | Rounds passed |
|---|---|---|
| 2026-11-13, same machine | threads, pool, pool-burst, procs+thr, redis, memcached, nginx, httpd | 3 of 3 each |
| 2026-11-13, across machines | the same ten thread tests, two rounds | 18 of 20 |
| 2026-11-20, across machines | procs+thr, previous version | 4 of 5 |
| 2026-11-20, across machines | procs+thr, new version | 5 of 5 (one via search) |
| 2026-11-20, across machines | pool, pool-burst, procs+thr, sig-ckpt, py-threads (honest) | 3 of 3 each |
| 2026-11-20, across machines | threads | 2 of 3 |
| 2026-11-20, after the review fixes | procs+thr, pool-burst | 3 of 3 each |
| 2026-11-20, after the review fixes | pool | 2 of 3 |
| 2026-11-20, with the checkpoint retry | python | 4 of 4 (one needed a second checkpoint) |

In the threads failure the replay reached the end without a divergence (every
write matched the log), yet the output comparison failed. The likely cause is
two threads writing at nearly the same moment, whose order in the pipe differs
from their order in the log; the test now prints the differing lines to tell.

**How it connects:** Serial mode (-T), Order search, Divergence.

## Crash catalogue

The crash catalogue is a set of 32 ways a program can die, each recorded and replayed to see whether the replay dies the same way.

A second recorder watches the replay, and the verdict names how close it came,
from "same death" to "differs" or "hung".
Most entries are reliable on both machines. The exceptions involve races decided
in user space: race-memory, queue-race, a real pbzip2 bug, a Go panic, and
Redis's crash handler.

The pbzip2 case is a real program. Unmodified, it crashed 0 times in 365 runs;
with delays added, about 80% of runs crashed. The log alone pinned the fault to a
mutex lock through a null queue pointer, but the replay matched the crash in only
3 of 9 crashing rounds: who wins a mutex is not in the log.

**How it connects:** Replay, Divergence, Signals from the log, Serial mode (-T).

## Cross-machine replay

Cross-machine replay means restoring and replaying on a different machine from the one that recorded.

The container's image must exist on the replay machine, and so must any
directory the container mounts. Podman loads its AppArmor profile only when it
starts a container, not on restore, so after a reboot one throwaway `podman run`
is needed first, or CRIU fails to restore.

**How it connects:** Checkpoint, Restore, Attach.

## Comparison with crr

crr is a tool Gemini wrote for the same job, after being told to absorb sysrr.

Both tools were run on the same 24 targets, recording on claude-dev and
replaying on claude-test.

**How it connects:** Recorder, Replay, Divergence, Seccomp listener.

### How crr replays

crr checks much less than sysrr: it matches each syscall by number only.

crr traps and records about 90 syscalls. It compares a syscall's number (with
a lookahead of 8 records), and on a mismatch quietly runs on for real. It never
compares arguments or written data. Writes to pipes and sockets are faked;
writes to the screen and regular files run for real.

### Results on the same targets

These are the outcomes when both tools faced the same 24 programs.

| | crr | sysrr |
|---|---|---|
| Its own pass rule | 20 of 24 (70% of lines in order, or 80% in any order) | 29 of 29 then (30 of 30 now) |
| Strict rule (identical output) | fails syscalls (37 of 3939 lines in order), shell, vdso-random, py-threads, py-procs | 25 full replays; 4 limits reported as divergences |
| Detecting a wrong replay | none for data; drifted on vdso-random with 0 mismatches reported | checks number, arguments and written data of every syscall |

crr's pass on pool proves nothing. The workers send results to the main thread
through a pipe, and crr fakes both ends of the pipe from the log. So the main
thread prints the recorded results whichever worker did the job. Pool is hard
for sysrr precisely because sysrr checks each write against the log.

### Recording overhead

Recording overhead is the extra time a program takes while it is recorded.

Measured with `dd` making 1 million one-byte syscalls, median of 5 runs:

| Setup | Time |
|---|---|
| No recorder | 109 ms |
| crr | 272 ms |
| sysrr (before 2026-11-20) | 299 ms |
| sysrr (after) | 293 ms |

Timing each BPF program explains the gap. At syscall exit both cost about the
same (120 ns for sysrr, 130 ns for crr). At syscall entry sysrr takes 98 ns
against crr's 62 ns, and almost all of that is copying each write's data into
the log. That copy is what lets sysrr catch a wrong write. On unrelated
processes sysrr is cheaper: 132 ms against 148 ms (103 ms with no recorder).

crr's "2x faster, 171x smaller log" comes from a loop of 200,000 `gettid`
calls that crr does not record at all. In real server logs such calls are at most
about 8% of syscalls (nginx), not worth losing checks for.

### What sysrr took from crr

sysrr adopted crr's ideas only where they did not weaken its checks.

- Naming the container: `sysrr -C NAME` and `sysrr_replay -C NAME`.
- A one-command restore: `sysrr_restore.sh`.
- Delivering a signal that landed during the freeze before the copy resumes, used only where the log proves it is exact.

Not taken: the pruned filter (it gives up checks), the CRIU action-script boundary and live continuation (sysrr already has both in its own form). Two signal ideas from crr's documents were already in sysrr.

**How it connects:** Checkpoint signals, Follow mode, Restore, Attach.

## Limits

A limit is something sysrr cannot replay exactly, with the reason.

**How it connects:** Log, Replay, Divergence, Signals from the log, Serial mode (-T).

### Invisible to syscalls

These things never pass through a syscall, so they are not in the log.

- `time()`, the coarse clocks and glibc's `getrandom` come from the vDSO in user space. The acpi_pm clock source makes `clock_gettime` a real syscall, but not these; across machines the vDSO getrandom causes a reported divergence.
- The `rdtsc` instruction reads the CPU clock without the kernel.
- Shared memory written by a process outside the recording.
- io_uring, which does I/O without a syscall per operation.

### Threads

These limits come from user-space ordering that the log cannot see.

- Two threads taking the same lock within a few hundred nanoseconds of each other cannot always be put back in order. The order search fixes most cases but can miss.
- Python's GIL is handed over on a 5 ms timer in user space. Python threads replay for a few dozen calls, then end in an honest divergence.

### Signals

These limits concern when a recorded signal reaches the restored thread.

- A signal that arrived while a thread was computing comes back at its previous syscall, so a little early. Exact delivery would need a hardware branch counter, as the rr debugger uses.
- SIGCHLD from real children arrives at its own time, so shell scripts often diverge (reported, never a hang).
- The delivered signal's details say it came from the replayer, not the original sender.
- A signal pending at the checkpoint with an SA_NODEFER handler is recognised only if the restored thread stands exactly at the handler's first instruction; otherwise it could be delivered twice (not seen in a test).

### Checkpoint

These limits make the checkpoint itself fail, before any replay.

- An open TCP connection makes CRIU refuse ("checkpointing failed -52") unless `--tcp-established` is given to both the checkpoint and the restore.
- A file the program keeps growing in the container's own layer breaks the restore. Write it to a volume instead.
- A checkpoint that catches a vfork before its exec fails after CRIU's 10 seconds: the parent sleeps in state D waiting for the child, and CRIU has stopped the child. This was the "python" test's flake (2 of 30 checkpoints while recording, 0 of 30 without). Retrying works; the tests retry up to three times and replay the Generation of the attempt that worked (a failed attempt still counts as a Generation).

### Live mode and the filter

These limits apply when the restored copy keeps running after the replay ends.

- File descriptors installed during replay are stand-ins (an unconnected socket, an empty memfd, /dev/null for a file received over a socket). In `-e live` mode the program goes on talking to them.
- The seccomp filter cannot be removed, so the replayer must keep running, and CRIU cannot checkpoint a process with a seccomp listener.

### The test machines

The test VMs expose no hardware performance counters ("no PMU driver, software events only"), so the branch-counter approach cannot even be tried there.

**How it connects:** Signals from the log, Checkpoint signals, File descriptors, Memory pinning, Seccomp listener.

## Common questions

**What does "passed" mean in the suite?**
The replay's output is byte-identical to the live run after the checkpoint, no divergence was reported, and the end of the log was reached. Honest tests instead pass by reporting the divergence they are built to cause.

**How reliable are the thread tests?**
Usually, not always. Across machines on 2026-11-13, 18 of 20 rounds passed; on 2026-11-20 most thread tests passed 3 of 3 or 5 of 5, and threads passed 2 of 3.

**Does sysrr work on real software?**
Yes. Redis, memcached, nginx and Apache httpd replay with identical datasets, identical logs, or the same thread serving each request.

**Is crr better because it records faster?**
It is somewhat faster on syscalls both record (272 ms against 293 ms on 1 million syscalls), because it skips copying written data. That same skip means it cannot tell when a replay goes wrong.

**Why did crr pass the thread pool test so easily?**
Because it fakes both ends of the pipe the workers use. The main thread prints the recorded results no matter which worker did each job, so the test cannot fail.

**What is the biggest limit?**
Anything that happens without a syscall: user-space clocks and random numbers, races inside user code, and memory shared with outside processes.

**Why not deliver signals at the exact instruction every time?**
That needs a hardware branch counter to find the same point in a loop, and the test machines have none. sysrr is exact when a signal interrupted a syscall or landed during the checkpoint freeze.
