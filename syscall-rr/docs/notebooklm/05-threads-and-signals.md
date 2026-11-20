# sysrr: threads and signals

Threads and signals are the two hardest parts of replaying a program. The log
holds only syscalls, but threads also meet through shared memory and locks,
which make no syscall. Signals can land at any instruction, while the log only
says which syscall came before them. This part explains how sysrr puts threads
back in order and delivers recorded signals, and where both still fall short.

## Key ideas

- The log sees syscalls only: what threads do to shared memory between syscalls is invisible.
- Strict order (-S) answers syscalls across threads in the recorded order.
- Serial mode (-T) runs one thread's user code at a time, in an order rebuilt from the log.
- Where two threads ran side by side live, the order is a guess; the order search tries the other guesses.
- The flips the search needs show where the program depended on which thread went first.
- Signals from a timer or from outside are replayed from the log, at the syscall where they landed.
- A signal that landed while CRIU had the process frozen is sent at the exact instruction.
- A signal that landed in the middle of a computation still comes back a little early.

## Why threads are hard

Threads are hard because the log records syscalls, while threads also share
memory, and changing memory makes no syscall.

Between two syscalls a thread runs user code: it reads and writes memory and
takes locks. A lock nobody waits for is taken entirely in user space, with no
futex call. So the log cannot say which thread got a lock first, or which one
updated a counter first. In the replay the threads run at a different pace, the
winner can change, a thread computes something else, makes a different syscall,
and the replay reports a divergence.

```
live:  thread A  ==syscall== .... user code A ....  ==syscall==
       thread B       ==syscall== .... user code B ....  ==syscall==
                                  ^^^^ both touch one counter here:
                                       the log does not say who was first
```

**How it connects:** Replay, Divergence, Strict order (-S), Serial mode (-T), Order search, Log.

## Strict order (-S)

Strict order means syscalls across all threads are answered in the order they
happened live.

Every record has a sequence number that grows across all threads. With `-S`, a
thread's syscall is answered only when its record is next overall; the others
wait. This fixes the order of syscalls, but not the user code between them:
once answered, a thread runs on while the next thread may be answered too. That
is enough when threads mostly meet through syscalls (the `fork` and
`thread-new` tests), not for nginx, whose workers bump a shared counter right
after `accept4`.

**How it connects:** Serial mode (-T), Log, Replay, Test suite.

## Serial mode (-T)

Serial mode runs one thread at a time, so user code that ran side by side live
runs one piece after another in the replay.

A thread runs from the moment its syscall is answered until its next syscall
reaches the replayer; only then may the next thread go. A thread that does not
come back within 2 seconds is no longer waited for, so an unknown blocking call
cannot freeze the replay.

**How it connects:** Strict order (-S), Order search, Seccomp listener, Replay, Divergence, Attach.

### Stretches

A stretch is the user code a thread runs between two of its syscalls.

Each record stands for the stretch that follows it. Stretches that overlapped
live must now run one after the other, and the log cannot say in which order.

### Ordering by the end of stretches

By default a stretch takes its place by when it ended, that is, when the
thread's next syscall started.

A thread leaving a syscall starts at an unsteady pace, while the way into the
next syscall is short, and code under a lock usually makes a syscall right after
releasing it. On a thread pool, ordering by end replayed 4 of 6 recordings to
the end; ordering by start, 1 of 6.

### Lock waits answered from the log

A recorded futex wait is held until its turn and answered with the recorded
result, not run for real.

Run for real, it would return when the kernel liked, and all threads woken by
one broadcast would race for the lock again. Returning without a real wake-up is
allowed, since every lock user checks the lock word again. A stretch that ends
waiting for a taken mutex is placed after the holder.

### Held start

At the start, every thread with records is held in a ptrace stop until its
first recorded syscall is next in the order.

Otherwise every restored thread would run to its first syscall at once.

### Lock holder first

If the thread whose turn it is sleeps in a wait the log lacks, the thread that
holds the lock goes first.

### Waits the recording lacks

A futex wait that is not in the log is emulated by the replayer, never run in
the kernel.

If the lock word already changed, it returns at once, as the kernel would.
Otherwise the thread is held until a wake on that address passes through the
replayer, and is answered at its turn. Run in the kernel, the wait could wake
next to the thread whose turn it is, and two replays of one log would differ.

### Facts that beat guesses

Orders the log does prove are used instead of guesses.

An `epoll_wait` result is not given before another thread's `epoll_ctl` that
armed the event. Calls that free a number (close, munmap, dup2) take their
place by when they started. A wrong file descriptor counts as a divergence at
once.

## Order search

The order search restores the checkpoint again and again, trying other guesses
for overlapping stretches, until a replay reaches the end of the log.

A wrong guess in serial mode shows up later as a divergence. The search needs
repeatable replays (same log, same guesses, same result), which emulating the
missing waits made true.

**How it connects:** Serial mode (-T), Divergence, Restore, Checkpoint, Replay, Test suite.

### Suspects and flips (-F)

A flip changes one stretch's place from "by its end" to "by its start", or
back.

When a replay diverges, sysrr_replay names suspects: the diverging thread's last
four stretches, each with the eight stretches of other threads that overlapped
it. `sysrr_replay -T -F 9044,11087` replays with those stretches flipped.

### sysrr_search.sh

The script restores and replays with `-T`, flipping one suspect at a time.

A flip is kept only if the replay gets past the record that diverged before. If
no single flip helps, it tries pairs of the six strongest suspects. It stops at
the end of the log, when nothing helps, or after 20 tries (the tests allow 40),
and prints `FOUND -F ...` or `BEST -F ...` (the furthest it got). Each try takes
a second or a few. Lines from a real search on the `procs+thr` test:

```
try 2 (-F 9057): progress 4938 (diverged at #8354)     not past: dropped
try 3 (-F 9046): progress 4937 (diverged at #8354)     not past: dropped
try 4 (-F 9044): progress 4940 (diverged at #11138)    got past: kept
...
FOUND -F 9044,11087 (7 replays)
```

### What the flips tell you

The flips mark where the program's behaviour depended on which thread went
first.

The order found fits the whole log but may not be the live one; the log cannot
tell them apart. For hunting a race bug, the flips point at the records where
two threads raced.

### How reliable it is

Most thread tests replay without a search, the rest usually with one or two
flips, and sometimes the search misses.

On one machine, three rounds each of eight thread and server tests all passed;
pool-burst needed a search in 2 of 3 rounds, httpd in every round. Across two
machines, 18 of 20 rounds passed. A miss happens when the right flip is outside
the suspects or more than two flips are needed at one point.

## Signals from the log

Signals from the log are signals the live run got from a timer or from outside,
which the replay delivers again where the log shows.

**How it connects:** Log, Replay, Attach, Checkpoint, Restore, Checkpoint signals, Divergence, Test suite.

### Which signals are replayed

Only signals the replay cannot produce by itself come from the log.

Replayed: signals from a timer (alarm, setitimer, timer_create) and signals
sent from outside (podman kill, a shell). Not taken from the log, on purpose:

- a signal the program sent itself: its kill call runs for real and sends it again (a received signal is matched to a recorded kill aimed at that process or thread; a kill to a whole group can match every receiver);
- SIGCHLD: the real children send it;
- SIGPIPE: it goes out with the failed write's EPIPE error;
- faults such as SIGSEGV: the instruction faults again;
- a signal that killed the program: delivered at the end of the recording.

### Where a signal is delivered

A signal goes out at the last syscall the thread made before it, in one of
three ways.

- **It interrupted that call.** The replayer stops the thread briefly with
  ptrace, puts the recorded restart code in its registers and sends the signal;
  the kernel then restarts the call after the handler (SA_RESTART) or fails it
  with EINTR, as live.
- **The call had completed.** The signal goes out with the call's answer.
- **The recorded call is not made in the replay** (a sleep that was only
  timing). The signal goes out before the call the thread makes instead, with
  code -513, "always restart".

Two signals of the same number recorded at one call need care: the kernel
keeps only one of a standard signal pending, so sending both at once would run
the handler once. The replayer sends one and carries the other to the thread's
next call; if that call is inside the handler, the signal waits there until the
handler returns, as live. A signal is marked delivered only once delivery has
worked, so one that could not go out is tried again at the next call. In Serial
mode a signal handler is user code like any other: it runs only in the thread's
turn.

The timer test gets 71 of 71 signals back at the right syscall, the
outside-signal test 8 of 8.

### Timers at attach

At attach the replay switches off the timers the checkpoint restored and
removes pending instances of the planned signals.

A restored timer would tick in real time while the program lives in recorded
time, and one that ran out while the copy sat stopped would add a delivery.

### Signals pending at the checkpoint

A signal pending when CRIU saved the process is in the image: the restore
delivers it, and the copy wakes up inside its handler.

The recorder logged that delivery at the start of the generation, so the replay
skips it when the thread has that signal blocked at its first syscall (a
handler blocks its own signal).

## Checkpoint signals

A checkpoint signal is one that landed while CRIU had the process frozen,
after the pending signals were saved.

It is not in the image. Live, the kernel delivered it the moment the process
went on, at the instruction where the checkpoint caught it. The replay now
sends it before the copy resumes, so the handler runs from that same
instruction. It used to go out at the first syscall, after the thread had
computed on.

**How it connects:** Signals from the log, Checkpoint, Restore, Attach, Generation, Test suite.

### How it is recognized

The log keeps each signal's instruction and stack pointers, and when CRIU let
each thread go.

A leading signal is sent at the checkpoint only if those pointers equal the
restored thread's registers and it arrived within 2 ms of CRIU letting the
thread go. The time check matters: a tight loop passes the same instruction
again and again. Other leading signals go out at the first syscall: exact if
they interrupted that call, a little late otherwise.

### The sig-ckpt test

The test makes the case happen on purpose.

The target loops without syscalls; its handler notes how far the loop got. A
CRIU action script sends SIGUSR1 at "post-dump", when CRIU has saved the
process but not let it go on. Live, the signal landed at loop count 74042352;
the replay prints the same. The old replayer diverged ("signals 0" instead of
"signals 1: 74042352").

## Limits

These are the cases sysrr still cannot replay exactly; it reports a divergence
instead of hiding them.

**How it connects:** Divergence, Serial mode (-T), Order search, Signals from the log, Test suite.

### Signals in the middle of a computation

A signal that landed while the thread computed comes back at its previous
syscall, a little early.

A handler that only sets a flag is fine; one that depends on how far the work
got is not. Exact delivery needs a hardware branch counter, as the debugger rr
uses, and this VM has none.

### SIGCHLD and shell scripts

Real children send SIGCHLD at their own time, and shell scripts often branch on
it.

The `shell` test only requires that the replay ends and reports the divergence.

### Python threads and the GIL

Python passes its global lock between threads on a 5 ms timer inside user
space, so who runs next is not in the log.

The `py-threads` and `py-procs` tests require an honest divergence report.

### Locks taken nanoseconds apart

Stretches that ended within a few hundred nanoseconds of each other cannot
always be put back in the live order.

### SA_NODEFER handlers

A handler with SA_NODEFER does not block its own signal, so the usual sign that
the restore already delivered a pending signal (the signal is blocked in
SigBlk) is missing. The replayer also checks where the restored thread stands:
exactly at that signal's handler means the restore delivered it, so it is not
sent again. A thread restored a few instructions into such a handler would
still get it twice.

## Where in the code

- `order_serial`: the serial order, by the end of stretches.
- `hold_thread`, `start_threads`: the held start.
- `lock_free_yet`, `lock_holder_first`: lock rules.
- `hold_wait`, `note_wake`, `release_woken`: emulated waits.
- `search_report`, `sysrr_search.sh`: suspects and the search.
- `plan_signals`: which signals to replay, and where.
- `return_with_signals`, `signals_then_again`: delivery at a syscall.
- `attach_listener`: timers off, pending signals drained.
- `signals_at_checkpoint`, `landed_in_freeze`: checkpoint signals.

## Common questions

**Q: Do I need -T for a program with threads?**
A: Use -T when threads share memory or locks, which is most real programs.

**Q: What is the difference between -S and -T?**
A: -S orders the syscalls. -T also runs the user code between them one thread at
a time.

**Q: The replay with -T diverged. What now?**
A: Run sysrr_search.sh on the same checkpoint and log. If it prints FOUND,
replay again with that -F list.

**Q: Is the order the search finds the real one?**
A: It fits the whole log, but the log cannot tell it from the live order. Its
flips show where the threads raced.

**Q: Why are SIGCHLD and SIGPIPE not taken from the log?**
A: The replay makes them itself: real children send SIGCHLD, and SIGPIPE comes
with the failed write.

**Q: Can a signal come back at the exact instruction?**
A: Yes, when it landed while CRIU had the process frozen. Elsewhere that would
need a hardware branch counter.

**Q: Why do Python thread programs end in a divergence report?**
A: Python switches threads on a timer the log cannot see, so the replay reports
the divergence instead of guessing.
