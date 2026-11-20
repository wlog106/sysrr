# sysrr: glossary

Every term you need to follow sysrr, each explained in a few plain sentences and linked to its neighbours. Part 1 covers sysrr's own ideas; part 2 covers the Linux features and outside tools sysrr is built on.

## sysrr concepts

### Recorder
`sysrr`, the program that watches a running process or container and writes down every syscall it makes. It loads small eBPF programs into the kernel at syscall entry and exit, so it needs no ptrace and does not get in CRIU's way when a checkpoint is taken. It keeps recording while the program runs on after the checkpoint.
**Related:** Log, Record, eBPF, Follow mode, Generation

### Log
The file the recorder writes (for example `rec.log`): a header, then one record after another. One log holds every generation, so a single recording can serve a restore of any of its checkpoints.
**Related:** Record, Data record, Generation, sysrr_dump

### Record
One entry in the log. A syscall record holds the syscall number, its six arguments, its return value, the thread's ids, the times it entered and left, its generation and its seq number. Other records mark signals and thread ends, CRIU's actions, and the final statistics.
**Related:** Log, Data record, Seq number, Signals from the log

### Data record
A piece of user memory copied for a syscall: an input (the bytes a `write` sends, a path) at entry, or an output (the bytes a `read` returned) at exit. Data records come before the syscall record that commits them; large buffers are split into chunks of up to 16 KiB, at most 1 MiB per buffer.
**Related:** Record, Faked answer, Divergence, BPF ring buffer

### Seq number
A counter taken at every syscall entry, shared by all threads, so it gives the order in which syscalls started. Strict order and serial mode use it to order threads, and it ties data records to their syscall.
**Related:** Record, Strict order (-S), Serial mode (-T)

### In-flight marker
A note sysrr writes when it stops, one per thread still inside a syscall (blocked in epoll_wait or a futex wait, say). That call took a seq number that no record will commit; the marker tells sysrr_dump it is not a lost record.
**Related:** Seq number, Record, sysrr_dump

### Generation
The part of the log after checkpoint N is generation N; before the first checkpoint it is generation 0. A restore of checkpoint N needs exactly generation N, because that is what the live program did after that moment. The recorder starts a new generation when CRIU lets go of the target's threads.
**Related:** Checkpoint, Log, Replay, parasite code

### Checkpoint
A saved copy of the running container, taken with `podman container checkpoint --leave-running` while the program keeps going. The recorder notices it from CRIU's ptrace calls (only those of a process named criu) and starts a new generation there.
**Related:** Generation, CRIU, podman checkpoint/restore, Checkpoint signals

### Restore
Recreating the container from a checkpoint archive, on the same machine or another one, left stopped so the copy has not run a single instruction yet. `sysrr_restore.sh` does it in one command.
**Related:** sysrr_restore.sh, leave-stopped, Attach, podman checkpoint/restore

### sysrr_restore.sh
The one-command restore. It adds `leave-stopped` to CRIU's config for this restore only and puts the file back afterwards, switches the clock source to acpi_pm, and checks that the restored process really is stopped.
**Related:** Restore, leave-stopped, clocksource (acpi_pm, kvm-clock, tsc)

### Container name (-C)
`sysrr -C CTR` records a whole podman container by name (it implies follow mode); `sysrr_replay -C NEW` attaches to a restored container by name. `-p PID` still works for a single process.
**Related:** Recorder, Follow mode, Attach

### Attach
How `sysrr_replay` takes over a restored process before it runs. It holds the process briefly with ptrace and makes it run a few syscalls: disarm restored timers, drop pending copies of signals that will come from the log, and install a seccomp filter with a listener. It copies the listener out with pidfd_getfd, lets go, and resumes the process.
**Related:** Seccomp listener, ptrace (SEIZE, INTERRUPT, DETACH), pidfd / pidfd_getfd, Checkpoint signals

### Seccomp listener
A file descriptor through which `sysrr_replay` receives every syscall the restored process makes, one listener per process. It is how the replay sits between the program and the kernel.
**Related:** Notification, seccomp user notification (USER_NOTIF, NEW_LISTENER, TSYNC, ADDFD, CONTINUE), Attach

### Notification
One syscall of the restored process, held in the kernel until `sysrr_replay` answers it, either with a faked result or by letting it run for real.
**Related:** Seccomp listener, Faked answer, Run for real (passthrough)

### Replay
What `sysrr_replay` does after attaching. It matches each notification with the thread's next record of the chosen generation, checks the syscall number, arguments and input bytes, and answers from the log. The restored copy then follows the path the live program took, up to the end of the recording.
**Related:** Notification, Faked answer, Divergence, End modes (-e stop/live/kill)

### Faked answer
The syscall does not run. `sysrr_replay` writes the recorded output bytes into the process's memory and returns the recorded value. Reads, network calls, clocks and random numbers are answered this way, so the copy sees what the live program saw.
**Related:** Replay, process_vm_readv/writev, Data record

### Run for real (passthrough)
Calls that change the process itself (memory maps, signal handlers, new threads, futex, exit) are let through with CONTINUE, because faking them would leave the process without the mapping, thread or handler it expects. If such a call failed in the live run, the replay fakes the same failure.
**Related:** Faked answer, Memory pinning, Pid pinning, futex

### Divergence
The restored process asks for something other than the recording says: another syscall, other arguments, or different bytes to write. `sysrr_replay` stops and reports where, with the expected and the actual call side by side. It also reports one when no record has been used for 20 seconds.
**Related:** Replay, Order search, EXPECT=honest

### Strict order (-S)
Releases syscalls in their recorded order across threads (by seq number). It orders syscall entries only: the user code between syscalls is ordered just because each thread waits at its next syscall.
**Related:** Serial mode (-T), Seq number, Stretch

### Serial mode (-T)
Runs one thread's user code at a time, in the order the stretches ended in the live run. Use it when threads share memory or locks. Recorded futex waits are answered from the log, so lock handoffs follow the recording.
**Related:** Stretch, Strict order (-S), Order search, futex

### Stretch
The user code a thread runs between two syscalls. The log cannot see inside it, so when two stretches of different threads overlapped live, it does not say which touched shared memory first; serial mode has to choose an order.
**Related:** Serial mode (-T), Flip (-F), Order search

### Order search
`sysrr_search.sh`: when a serial-mode replay diverges, it restores the checkpoint again and again, trying the other order at the suspect places, until a replay reaches the end of the recording. The order it finds fits the whole log, though it may not be the live one.
**Related:** Flip (-F), Serial mode (-T), Divergence

### Flip (-F)
`-F SEQ,...` tells serial mode to put the other of two overlapping stretches first at those records. The order search finds them; the flips a recording needs mark where the program depended on which thread came first.
**Related:** Order search, Stretch, Serial mode (-T)

### Signals from the log
Timer signals and signals sent from outside (such as `podman kill`) are delivered by `sysrr_replay` from the log, at the syscall they interrupted, with ptrace and the recorded restart code. Signals a program sends itself happen for real; SIGCHLD comes from the real children. A signal that landed while the thread computed comes back at its previous syscall, a little early.
**Related:** Checkpoint signals, ERESTARTSYS / ERESTARTNOINTR (-512 / -513), SA_RESTART, SIGCHLD

### Checkpoint signals
A signal that arrived while CRIU had the process frozen, after it had saved the pending signals, is not in the checkpoint; live, the kernel delivered it the instant the process went on. If its recorded instruction and stack pointer match the restored thread, and it reached the thread within 2 ms of CRIU letting go, `sysrr_replay` sends it before resuming the copy, so the handler runs from the exact same instruction.
**Related:** Signals from the log, Attach, SigBlk, Checkpoint

### Follow mode
`-f` (and `-C`) records the processes the target forks as well, the ones that already exist and new ones. Ended processes are forgotten, and the recorder stops by itself once every recorded process has ended.
**Related:** Targets map, Container name (-C), Recorder

### Targets map
The BPF table of process ids recorded in follow mode. A child is added when it is forked and removed when its last thread exits; it holds 4096 processes at a time, and one it could not add counts as lost.
**Related:** Follow mode, eBPF, cgroup v2

### File descriptors (installed fds, stand-ins)
When a faked call would create a file descriptor (open, socket, accept, pipe), `sysrr_replay` installs a stand-in at the same number with ADDFD, so later calls and mmap find something there. A stand-in is the real file if it exists, otherwise `/dev/null`, an unconnected socket or an empty memfd; with `-e live` the program goes on talking to these stand-ins.
**Related:** Faked answer, seccomp user notification (USER_NOTIF, NEW_LISTENER, TSYNC, ADDFD, CONTINUE), End modes (-e stop/live/kill)

### Memory pinning
An mmap without a fixed address would land somewhere else in the restored copy, because CRIU restores the process with a different base for new mappings. `sysrr_replay` re-issues it at the recorded address with a brief ptrace, so pointers in the program stay the same as in the recording.
**Related:** Run for real (passthrough), ptrace (SEIZE, INTERRUPT, DETACH)

### Pid pinning
Before a real fork or thread creation, `sysrr_replay` writes the recorded id minus one into ns_last_pid of the container's pid namespace, so the new process or thread gets the same id it had in the live run.
**Related:** ns_last_pid, PID namespace / NSpid, Run for real (passthrough)

### Timing-only record
futex, rt_sigreturn, sched_yield, sleeps and monotonic clock reads only show how a thread got through a lock or a wait. A recorded one that the replay does not make is dropped, and an extra one runs for real, instead of counting as a divergence.
**Related:** futex, Serial mode (-T), Divergence

### End modes (-e stop/live/kill)
What `sysrr_replay` does when the recording runs out. `stop` (the default) stops the process so it can be inspected; `live` lets it go on for real from the replayed state; `kill` ends it. The tool must keep running in every mode, because a seccomp filter cannot be removed.
**Related:** Replay, seccomp, File descriptors (installed fds, stand-ins)

### sysrr_dump
Prints a log in readable form: `-s` for a summary and integrity check (lost records, missing seq numbers), `-g N` for one generation's syscalls.
**Related:** Log, Generation, Record

### Test suite
`tests/run_all.sh`, 30 tests. Each one runs a target in a container, records it, checkpoints, restores (on a second machine with `REMOTE=`), replays and compares: the output must be byte-identical, with no divergence and the end reached.
**Related:** EXPECT=honest, Divergence, Replay

### EXPECT=honest
A test setting for targets with known limits (the vDSO's getrandom, a shell's SIGCHLD timing, Python threads). The test passes if the replay matches or reports a divergence and ends by itself; it must never hang or differ silently.
**Related:** Test suite, Divergence, vDSO, SIGCHLD

## Linux and tool terms

### eBPF
Small, kernel-verified programs that run inside Linux at hook points. sysrr's recorder is built from them, which is why it can record without ptrace and still let CRIU take a checkpoint.
**Related:** tracepoint / tp_btf, BPF ring buffer, task storage, Recorder

### tracepoint / tp_btf
Fixed hook points in the kernel. sysrr attaches its eBPF programs to `sys_enter`, `sys_exit`, `signal_deliver`, `sched_process_fork` and `sched_process_exit`; tp_btf is the form that gives the programs typed access to kernel structures.
**Related:** eBPF, Recorder, Signals from the log

### BPF ring buffer
A shared-memory queue from the kernel's eBPF programs to a user program. The recorder writes records into it (64 MiB by default, `-b` to change), and sysrr's user side empties it into the log every 10 ms; anything that did not fit is counted as lost.
**Related:** eBPF, Log, Data record

### task storage
Per-thread storage attached to the kernel's own thread structure. The recorder keeps the syscall in progress there (number, arguments, seq number) between entry and exit, plus the thread's cached container ids.
**Related:** eBPF, Record, PID namespace / NSpid

### CRIU
Checkpoint/Restore In Userspace: saves a running process tree (memory, registers, files and more) to image files and recreates it later. podman runs it; it uses ptrace and parasite code to do its work.
**Related:** podman checkpoint/restore, parasite code, leave-stopped, Checkpoint

### podman checkpoint/restore
`podman container checkpoint --leave-running -e ckpt.tar.gz CTR` exports a checkpoint without stopping the container; `podman container restore --import` recreates it, under a new name if you like.
**Related:** CRIU, Checkpoint, Restore, sysrr_restore.sh

### leave-stopped
A CRIU option, read from `/etc/criu/default.conf`, that leaves the restored processes stopped instead of running. It lets `sysrr_replay` attach before the copy runs a single instruction.
**Related:** Restore, sysrr_restore.sh, Attach

### parasite code
Code CRIU injects into each process during a checkpoint to collect its state from inside. Its syscalls are marked in the log (criu-parasite) and kept out of the replay.
**Related:** CRIU, Generation, Checkpoint

### ptrace (SEIZE, INTERRUPT, DETACH)
The debugger interface: SEIZE attaches without stopping, INTERRUPT stops the thread, DETACH lets go. `sysrr_replay` uses it only briefly (attach, signal delivery, memory pinning, holding threads in serial mode); the recorder never uses it, but watches CRIU's ptrace calls to see checkpoints.
**Related:** Attach, Memory pinning, Signals from the log, CRIU

### seccomp
A kernel filter on the syscalls of a process, written as a small classic BPF program. Once installed it can never be removed, which is why `sysrr_replay` must keep running.
**Related:** seccomp user notification (USER_NOTIF, NEW_LISTENER, TSYNC, ADDFD, CONTINUE), End modes (-e stop/live/kill)

### seccomp user notification (USER_NOTIF, NEW_LISTENER, TSYNC, ADDFD, CONTINUE)
The filter result USER_NOTIF hands a syscall to a supervisor through a listener fd (created with NEW_LISTENER); TSYNC applies the filter to every thread. The supervisor answers with a value, lets the call run (CONTINUE), or installs a file descriptor in the process (ADDFD). The whole replay is built on this.
**Related:** seccomp, Seccomp listener, Notification, File descriptors (installed fds, stand-ins)

### pidfd / pidfd_getfd
A pidfd is a file descriptor that refers to a process; pidfd_getfd copies a file descriptor out of another process. `sysrr_replay` takes the listener created inside the target this way, and sends signals through pidfds so a reused pid is never hit.
**Related:** Attach, Seccomp listener

### process_vm_readv/writev
Read or write another process's memory directly. `sysrr_replay` reads syscall inputs to check them against the log and writes recorded outputs for faked answers.
**Related:** Faked answer, Divergence, Data record

### PID namespace / NSpid
A container has its own pid numbering. `NSpid` in `/proc/PID/status` lists a thread's host id and its container id; sysrr matches threads by container ids, which CRIU keeps the same across machines.
**Related:** Pid pinning, ns_last_pid, task storage

### ns_last_pid
`/proc/sys/kernel/ns_last_pid`, the last pid handed out in a pid namespace. Writing N-1 there makes the next new process or thread get id N.
**Related:** Pid pinning, PID namespace / NSpid

### vDSO
Kernel code mapped into every process, so that some calls (`clock_gettime`, `time()`, glibc's `getrandom`) run without a syscall. Neither eBPF nor seccomp sees them: acpi_pm turns clock reads back into syscalls, but `time()` and the vDSO's getrandom stay gaps.
**Related:** clocksource (acpi_pm, kvm-clock, tsc), EXPECT=honest

### clocksource (acpi_pm, kvm-clock, tsc)
The hardware clock the kernel reads. With tsc or kvm-clock the vDSO answers clock reads by itself; with acpi_pm it cannot, so clock reads become real syscalls that are recorded and replayed. sysrr uses acpi_pm on both machines.
**Related:** vDSO, sysrr_restore.sh, Faked answer

### vfork
A fork in which the parent sleeps (state D) until the child calls exec; Python's subprocess uses it. A checkpoint that stops the child in that gap cannot seize the parent and fails after CRIU's 10 seconds; trying again works.
**Related:** CRIU, Checkpoint, Generation

### futex
The kernel's wait-and-wake primitive under mutexes and condition variables. In sysrr, futex calls show how locks passed between threads: they are timing-only, and serial mode answers recorded waits from the log.
**Related:** Serial mode (-T), Timing-only record, Run for real (passthrough)

### ERESTARTSYS / ERESTARTNOINTR (-512 / -513)
Kernel-internal results of a syscall interrupted by a signal: -512 means restart after the handler if it has SA_RESTART, otherwise fail with EINTR; -513 means always restart. `sysrr_replay` puts these codes back in place under ptrace, so a call resumes after a replayed signal just as it did live.
**Related:** SA_RESTART, Signals from the log, ptrace (SEIZE, INTERRUPT, DETACH)

### SA_RESTART
A signal-handler flag: a syscall interrupted by the signal restarts after the handler instead of failing with EINTR.
**Related:** ERESTARTSYS / ERESTARTNOINTR (-512 / -513), Signals from the log

### SigBlk
The line of `/proc/TID/status` listing the signals a thread has blocked, including a handler's own signal while it runs. `sysrr_replay` reads it to tell whether the restore already delivered a signal that was pending at the checkpoint.
**Related:** Checkpoint signals, Signals from the log

### SIGCHLD
The signal a parent gets when a child ends. During a replay the children are real, so it arrives at its own time; shell scripts that depend on its timing may diverge, which the replay reports.
**Related:** Signals from the log, EXPECT=honest

### AppArmor profile
podman loads its containers-default profile when it starts a container. On a machine that has not started one since boot, a restore fails ("can't write lsm profile"); running any container once fixes it.
**Related:** Restore, podman checkpoint/restore

### cgroup v2
The kernel's grouping of processes; every container has its own group. In follow mode the recorder first checks the container's cgroup id, one quick compare that skips every other process on the host.
**Related:** Follow mode, Targets map, eBPF

### rr
Mozilla's record-and-replay debugger. It records with ptrace and uses hardware branch counters to replay signals at the exact instruction. sysrr differs: it records with eBPF so CRIU can checkpoint, and replays into a restored copy rather than from the program's start.
**Related:** PMU / branch counter, Replay, ptrace (SEIZE, INTERRUPT, DETACH)

### crr
A tool Gemini built for the same job, compared with sysrr on 2026-11-20. It checks only syscall numbers, never arguments or data, so it can drift unnoticed; sysrr adopted its container naming, its one-command restore and the idea of delivering signals at the checkpoint.
**Related:** Container name (-C), sysrr_restore.sh, Checkpoint signals

### PMU / branch counter
The CPU's performance monitoring unit; counting retired branches lets a tool stop a thread at the exact instruction where a signal landed. The test VM has no PMU, so sysrr delivers a signal that landed mid-computation at the previous syscall instead.
**Related:** rr, Signals from the log
