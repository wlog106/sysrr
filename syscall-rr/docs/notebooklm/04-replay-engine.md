# sysrr: The replay engine (how sysrr_replay answers syscalls)

sysrr_replay drives a restored copy of a program through the syscalls the live program made after its checkpoint. The kernel stops every syscall of the copy and asks sysrr_replay what to do. For each one, sysrr_replay finds the next recorded syscall of that thread, checks that the copy asks for the same thing, and then answers: either with the recorded result, without running the syscall, or by letting the kernel run it for real. The moment the copy asks for something different from the recording, sysrr_replay stops and reports a divergence.

## Key ideas

- Every syscall of the restored copy comes to sysrr_replay as a notification from its Seccomp listener.
- Each notification is matched to the next recorded syscall of the same thread, by the thread's id inside the container.
- Most syscalls are faked: the recorded output bytes are written into the copy's memory and the recorded return value is given back.
- Syscalls that change the process itself (memory maps, signal setup, threads, futex, exit) run for real.
- The syscall number, the arguments and every input buffer are checked against the recording, byte by byte.
- A mismatch is a Divergence: sysrr_replay prints what was expected and what came, and exits with code 1.
- Real file descriptors, memory addresses and thread ids are put back where the recording had them, so the syscalls that run for real behave as they did live.
- The seccomp filter can never be removed, so sysrr_replay must keep running for as long as the copy runs.

## Seccomp listener

The Seccomp listener is the file descriptor through which the kernel hands each trapped syscall of the copy to sysrr_replay.

At Attach, sysrr_replay installs a seccomp filter in the stopped copy. It sends every native x86-64 syscall of every thread to the listener, and the kernel parks the thread until sysrr_replay answers.

```
restored copy                     kernel                    sysrr_replay
thread 5: read(3, buf, 64) --->  parks thread 5  --->  notification: tid, nr, args
                                                         | find thread 5's next record
                                                         | check nr, args, input bytes
                                 <--- answer: "ret = 29, here are the bytes"
thread 5 continues               <--- or: "run it for real" (CONTINUE)
```

### The supervise loop

The supervise loop waits on all listeners at once and handles one notification at a time. A filter covers the process that installed it and children it forks later, so each process that existed at the checkpoint (for example nginx's workers) gets its own listener at Attach.

Some notifications are not answered right away. A thread whose queue is empty is held: its syscall stays unanswered, so the thread stays blocked, as it was in the recording. Under -S and Serial mode (-T), a notification that is not yet next in the recorded order is parked until its turn.

### Matching a notification to a record

Matching means finding which recorded syscall this notification stands for. sysrr_replay reads the thread's id inside the container from /proc (the last field of NSpid). Host ids change after a Restore, but CRIU keeps the container's ids. Each thread has its own queue of recorded syscalls of the chosen Generation, in recorded order. The notification is compared with the head of that queue.

**How it connects:** Attach, Restore, Generation, Log, Replay, Serial mode (-T), Follow mode.

## Faking a syscall

Faking means answering a syscall from the Log without letting the kernel run it.

sysrr_replay writes the recorded output pieces (the bytes a read returned, a stat struct, a sockaddr) into the copy's memory with process_vm_writev, at the addresses the copy's current arguments give. Then it answers with the recorded return value. So the copy sees the same network data, file contents, clock readings and random bytes as the live program, even on another machine.

Some outputs hold the program's own pointers: a message header (msghdr) names the buffers to fill. The copy's heap can be laid out differently from the live one when several threads allocate, so those pointers are kept as they are in the copy; only the kernel's fields (lengths, flags, how much arrived) come from the Log.

**How it connects:** Log, Recorder, Replay, File descriptors.

## Running a syscall for real

Running for real means answering with SECCOMP_USER_NOTIF_FLAG_CONTINUE, so the kernel executes the syscall normally.

A fixed passthrough list runs for real: the syscalls that change the process itself rather than tell it about the world. That is the memory map (brk, mmap, munmap, mprotect, madvise), signal setup, threads and processes (clone, fork, execve, exit), futex, getpid, the kill family, close and chdir. Faking these would leave the process without the mapping, thread or handler it expects. One that failed live is faked as a failure, since a real attempt might succeed.

**How it connects:** Memory pinning, Pid pinning, File descriptors, Signals from the log.

## Checks

The checks are three comparisons made for every notification before it is answered.

### Syscall number

The syscall must be the one recorded next for this thread. A different number is a Divergence right away. A few timing-only records are allowed to differ (see Timing-only records).

### Arguments

The arguments are compared with the recorded ones, pointers included, because the restored address space is the live one. A different file descriptor number is a Divergence: the thread is serving another file or connection. Other argument differences are warnings. Buffers passed at other addresses but holding the same bytes are only counted, since the heap can be laid out differently.

### Input buffers

Every buffer the program hands to the kernel (what it writes, a path, a sockaddr) is read from the copy's memory and compared byte for byte with the recorded one. This is the check that catches a wrong state: if the program computed something different, it writes something different. It is also why a faked pipe or socket write is still checked.

**How it connects:** Divergence, Recorder, Log, Serial mode (-T).

## Divergence

A Divergence is the moment the copy asks for something the recording does not have next.

### What the report shows

sysrr_replay prints `!! DIVERGENCE in thread N at record #S` with the reason (for example "input buffer differs"), the expected call and the actual one. For a buffer it shows how many bytes differ and both versions side by side. It lists the thread's last five replayed syscalls, applies the end mode and exits with code 1. A thread not in the recording, or 20 seconds without progress, is also a divergence.

### The tampered-log test

The Test suite proves the checks work. It flips one byte of a recorded read in a copy of the log and replays it. The copy computes a wrong line, and the replay must report the divergence at the next write, after exactly three good lines.

**How it connects:** Checks, Order search, Test suite, Replay.

## File descriptors

File descriptors are put back at the numbers the recording had, so that later real syscalls on them work.

### Installing real fds

When a faked call created file descriptors (open, socket, accept, pipe, dup, eventfd, memfd and similar), sysrr_replay installs a real fd at each recorded number with SECCOMP_IOCTL_NOTIF_ADDFD. For open, it is the same file from the container's root, opened without creating or truncating anything. A relative path is resolved from the container's root after the directory's own path (checked by comparing device and inode), so `..` and absolute symlinks lead where they led live. The data still comes from the Log; the fd only has to be real for syscalls that run for real (mmap, close, fchdir) and for live mode.

### Stand-ins

When the real thing cannot exist here, a stand-in takes its place. A socket is an unconnected socket of the same kind, since its traffic is faked. A file created after the checkpoint becomes a memfd that follows the recorded size and contents. A missing file becomes /dev/null.

### close, and -N

close runs for real, so the fd table stays equal to the recorded one. A warning that an fd "is already open" means the tables drifted. The option -N turns installation off: fd-creating calls return the recorded number but create nothing, and close is faked again.

**How it connects:** Faking a syscall, Running a syscall for real, Checks, Follow mode.

## Memory pinning

Memory pinning makes new mappings land at the address the recording had.

### Re-issuing mmap at the recorded address

CRIU restores every mapping at its old address, but not the base the kernel uses to place new ones, so a real mmap would land elsewhere and allocators such as jemalloc would take another path. sysrr_replay briefly seizes the thread with ptrace, rewrites the call to ask for the recorded address (MAP_FIXED_NOREPLACE), and lets the kernel restart it. The restarted call runs for real at the right place, in a few hundred microseconds. A moving mremap is pinned the same way (MREMAP_FIXED), but only after checking that the recorded place is free: MREMAP_FIXED would silently unmap whatever is there. If the place is in use, the kernel chooses.

### Allocator slack

Allocators shared by threads map or unmap memory depending on which thread asked first. An extra or missing anonymous mmap or munmap is tolerated and counted, and a mapping is never forced onto an address in use. The data is still checked as usual.

**How it connects:** Running a syscall for real, Restore, Serial mode (-T).

## Pid pinning

Pid pinning gives new processes and threads the ids they had in the recording.

Just before a real clone or fork, sysrr_replay writes the recorded id minus one to the container's /proc/sys/kernel/ns_last_pid, through a short-lived helper inside the container's pid namespace. The kernel then hands out exactly the recorded id. So fork's real result, getpid and the recorded wait4 all agree. If the id is taken, recorded ids are handed to new threads in order, with a warning.

**How it connects:** Follow mode, Running a syscall for real, Test suite.

## Timing-only records

Timing-only records are syscalls that only show how a thread got through a wait, not what it learned.

futex, rt_sigreturn, sleeps and reading the monotonic clock fall in this group. How often a thread polls or waits is a matter of timing, not of data. A recorded one that the replay does not make is dropped. One the replay makes that is not recorded runs for real. Each difference is counted in the totals.

**How it connects:** Checks, Serial mode (-T), Signals from the log.

## End of the recording

The end mode, set with -e, decides what happens when the recording is used up or a divergence is found.

- `-e stop` (default): stop the copy with SIGSTOP and keep sysrr_replay running, so the stopped state can be inspected. Threads held under Serial mode go straight from their hold into the stop, so the frozen state is the one at the end or the divergence.
- `-e live`: answer every later syscall with "run it for real", so the copy goes on as a normal program from the recorded state. Threads that were waiting for their turn are let go at once.
- `-e kill`: kill the copy and exit, for scripts.

If the live program was killed by a signal from outside (or a timer, or SIGPIPE), that signal is delivered at the end of the recording, to the process that died live and not to another one.

### Why the filter cannot be removed

Linux cannot remove a seccomp filter. If sysrr_replay exited, the copy's syscalls would fail with ENOSYS, so in live mode it must keep running as long as the copy runs.

**How it connects:** Seccomp listener, Divergence, Replay.

## Options that change the answers

- `-c` keeps going after an argument or input mismatch, with a warning instead of a stop.
- `-w FILE` saves what the copy writes to stdout and stderr (those writes are faked); the Test suite compares it with the live run's output.
- `-v` prints every replayed syscall.
- `-S` and `-T` order syscalls across threads (see Serial mode (-T)).

**How it connects:** Checks, Test suite, Serial mode (-T).

## The totals

The totals are the summary sysrr_replay prints at the end: syscalls faked and run for real, bytes checked and injected, warnings, fds installed, mappings pinned, thread ids set, timing-only differences, and signals delivered (at the checkpoint, earlier or later than live). Every approximation is counted, so a clean run shows at a glance.

**How it connects:** Divergence, Signals from the log, Checkpoint signals, Memory pinning, Pid pinning.

## Compared with crr

crr is a similar tool written by Gemini. It matches each notification by syscall number only and silently "continues live" on a mismatch. It never compares arguments or written bytes, so a drifting replay can report zero mismatches. sysrr's byte-for-byte checks are what make its passes mean something.

**How it connects:** Checks, Divergence, Test suite.

## Where in the code

- `attach_listener`: installs the filter and copies out the listener.
- `load`: reads one Generation of the Log into per-thread queues.
- `supervise` and `handle`: the loop and the answer to one notification.
- `passthrough` and `runs_real`: which syscalls run for real.
- `check_inputs` and `diverge`: the checks and the report.
- `install_fds` and `make_fd`: file descriptors and stand-ins.
- `needs_pin` and `pin_address`: memory pinning.
- `is_timing_only`: timing-only records.
- `print_totals`: the summary at the end.

## Common questions

**Does sysrr_replay re-run the program's network traffic?**
No. Reads from sockets are faked: the recorded bytes are written into the copy's memory. The copy's sockets are unconnected stand-ins.

**Why not fake every syscall?**
Some syscalls build the process itself: memory mappings, threads, signal handlers. If they were faked, the copy would lack the memory or thread it then uses, so they run for real.

**What exactly counts as a divergence?**
A different syscall number, a different file descriptor argument, or input bytes that differ from the recording. Other argument differences are only warnings.

**Why compare what the program writes, if writes are faked anyway?**
Because a wrong write is the first visible sign that the program's state went wrong. It catches the problem at the moment it shows, not much later.

**What does exit code 1 mean?**
The replay found a divergence. Exit code 0 means it reached the end of the recording, or was interrupted.

**Can I use the copy normally after the replay?**
Yes, with `-e live`. Keep sysrr_replay running, because the filter stays in the copy and needs someone to answer it.

