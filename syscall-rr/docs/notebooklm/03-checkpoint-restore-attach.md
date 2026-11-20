# sysrr: Checkpoint, Restore and Attach

This part covers the bridge between the two machines. On machine A, podman and
CRIU take a checkpoint of the running container while the Recorder keeps
writing the Log. On machine B, the checkpoint is restored but left stopped, so
the copy cannot run a single instruction yet. Then sysrr_replay attaches: it
makes the stopped copy install a seccomp filter on itself, takes the filter's
listener, and only then lets the copy run.

```
machine A                                   machine B
---------                                   ---------
container runs, Recorder writes the Log
        |
podman checkpoint --leave-running
  CRIU: seize -> parasite -> dump -> let go
        |   (the container keeps running:
        |    everything after this is Generation 1)
        v
ckpt.tar.gz + rec.log + image  ---------->  sysrr_restore.sh ckpt.tar.gz NEW
                                              CRIU restores, leaves it stopped (T)
                                                     |
                                            sysrr_replay -C NEW -g 1 rec.log
                                              Attach: inject seccomp filter,
                                              take the listener, deliver
                                              Checkpoint signals
                                                     |
                                              SIGCONT -> Replay begins
```

## Key ideas

- A checkpoint with `--leave-running` saves the container and lets it go on, so the live run continues past the checkpoint.
- Everything the program does after checkpoint N is Generation N, and a restore of checkpoint N needs exactly that part of the Log.
- The restored copy must stay stopped until the replay is attached, or it would make syscalls nobody answers from the Log.
- podman has no option to restore stopped; CRIU's `leave-stopped` setting, put in its config file for the restore only, does it.
- A restored process never calls `execve`, so sysrr_replay makes it call `seccomp()` itself instead of using a launcher.
- ptrace is used only for milliseconds, after CRIU has finished, so it never fights with CRIU.
- A signal that landed while CRIU had the process frozen is sent before the copy resumes, at the same instruction as live.

## Checkpoint

A checkpoint is a saved copy of a running container: its memory, threads, files and registers, packed into one archive.

The command is `sudo podman container checkpoint --leave-running -e ckpt.tar.gz CTR`. With `--leave-running` the container keeps running after the dump, and the Recorder keeps writing what it does.

### What CRIU does to the process

CRIU is the program podman uses for checkpoints. It works in three steps:

1. It freezes the container and attaches to every thread with `PTRACE_SEIZE`.
2. It injects "parasite" code into the process. The parasite makes about 350 syscalls inside the process to read out its memory and state.
3. It removes the parasite and detaches. With `--leave-running`, the process then simply continues.

### What the Recorder sees

The Recorder watches `ptrace` calls made by `criu`. Syscalls made while a thread is seized are the parasite's: they are flagged and never replayed. When the last thread is let go, the Generation number goes up by one.

### The syscall in flight

The program is usually blocked in a syscall when it is frozen. Most such calls are simply issued again, by the live process and by the restored copy alike; an interrupted sleep returns "interrupted" in both. Either way, the new Generation starts exactly where the restored copy starts.

### Why a checkpoint can fail

"CRIU checkpointing failed -52" is CRIU's code for any failed dump. The real reason is in CRIU's `dump.log`, which disappears with the container, so read it before removing the container (the tests print its error lines). Common causes:

- **An open TCP connection.** Add `--tcp-established` to the checkpoint and to the restore. podman does not accept that option together with `--name`, so the copy keeps the original's name.
- **A file the program keeps growing in the container's own writable layer.** CRIU notes its size during the dump, podman archives the file afterwards, and the restore then fails with "bad size". Write such files to a volume or a tmpfs instead.
- **A process stuck in state D** (uninterruptible sleep). CRIU gives up after 10 seconds ("Unseizable non-zombie ... state D"). Trying again usually works.
- CRIU also refuses an open POSIX message queue and an inotify watch on overlayfs.

**How it connects:** Recorder, Log, Generation, Restore, Test suite.

## Moving to machine B

Moving means copying everything the restore and the replay need to the second machine.

- **The checkpoint archive.** It belongs to root, so copy it as root: `sudo cat ckpt.tar.gz | ssh B 'cat > ckpt.tar.gz'`.
- **The Log** (`rec.log`) and the sysrr tools.
- **The container's image.** Use `podman pull`, or `podman save | ssh B podman load`.
- **Every directory the container mounts.** It must exist on B too.
- **The same clock source.** Both machines use `acpi_pm`, so clock reads are real syscalls and the Log has them.

**How it connects:** Checkpoint, Restore, Log.

## Restore

Restore turns the checkpoint archive back into a running container, but here it must be left stopped.

### Left stopped

If the copy ran before the replay attached, its syscalls would happen for real and the replay would start out of step. CRIU's `leave-stopped` setting keeps the restored threads in state T (stopped) before they run a single instruction.

podman has no option for it. But CRIU reads `/etc/criu/default.conf` even when podman drives it, so the setting goes in that file. It must be there for the restore only. On a checkpoint it would leave the live container stopped.

### What sysrr_restore.sh does

`sudo ./sysrr_restore.sh ckpt.tar.gz NEW` does it all in one command:

1. It switches the clock source to `acpi_pm`, like on machine A.
2. It saves `/etc/criu/default.conf` as it is, then writes a copy with `leave-stopped` added. Any other lines stay, for example another tool's settings.
3. It runs `podman container restore --import ckpt.tar.gz --name NEW`.
4. It puts the file back exactly as it was, also when the restore fails or is interrupted.
5. It checks that the restored process really is in state T, and prints its name and pid.

For a checkpoint taken with `--tcp-established`, pass that on with `-c --tcp-established`. The copy then keeps the original's name.

### The AppArmor warm-up

podman loads its AppArmor profile into the kernel only when it starts a container, and the kernel forgets it at reboot. CRIU's restore asks for that profile by name. If it is not loaded, the restore fails with "can't write lsm profile -2". The fix is to run any container once per boot, e.g. `sudo podman run --rm docker.io/library/debian:trixie-slim true`. sysrr_restore.sh prints this hint when it sees the error.

**How it connects:** Checkpoint, Attach, Pid pinning, Test suite.

## Attach

Attach is how sysrr_replay gets between the restored copy and the kernel: it installs a seccomp filter whose listener it holds.

### Why the filter must be injected

With such a filter, the kernel stops each calling thread and hands the call to whoever holds the Seccomp listener. But a process can only install a filter on itself, and a restored process never calls `execve`, so no launcher can set it up first. sysrr_replay makes the stopped process call `seccomp()` itself.

### Step by step

1. **Seize.** Attach with `PTRACE_SEIZE` and stop it with `PTRACE_INTERRUPT`. Save its registers.
2. **Gadget.** Overwrite 3 bytes at its instruction pointer with `syscall; int3`, so each injected call runs one syscall and stops.
3. **Scratch page.** Inject `mmap` for one page, and write the filter program into it.
4. **No new privileges.** Inject `prctl(PR_SET_NO_NEW_PRIVS)`. Without admin rights, a process must promise this before installing a filter.
5. **Disarm timers.** Inject `setitimer` and `timer_settime` to stop every restored timer. They would fire in real time; their signals come from the Log instead.
6. **Drain signals.** Inject `rt_sigtimedwait` with a zero timeout to take out pending copies of the signals the replay will deliver (a timer that ran out while the copy sat stopped would add one).
7. **Install the filter.** Inject `seccomp()` with `NEW_LISTENER` and `TSYNC`. Every native 64-bit syscall of every thread now goes to the listener. podman's own seccomp rules still win where they refuse a call.
8. **Take the listener.** `pidfd_getfd` copies the listener's file descriptor from the process into sysrr_replay.
9. **Clean up.** Inject `munmap` of the page and `close` of the process's copy of the descriptor. The new filter already traps these two, so the injector answers them itself.
10. **Put back and detach.** Restore the code bytes and the registers, then detach. The process returns to its stopped state.

### One listener per process

A filter belongs to one process. sysrr_replay finds the whole restored process tree (zombies skipped: they run nothing), gives every process its own filter before any of them runs, and watches all the listeners at once.

### Threads with nothing recorded

A thread that stayed blocked in one syscall the whole Generation has no records, because a syscall that never returns is never recorded. At attach, every existing thread without records gets an empty queue. Its first syscall is held, so it stays blocked, as it did live.

### Threads held for Serial mode

With `-T`, threads run their user code one at a time in recorded order. So at attach, threads are held in a ptrace stop. Each one is released when its first recorded syscall is next in the order.

### Matching threads

CRIU restores the pids inside the container, but host pids differ on machine B, so threads are matched by their pid inside the container, which every record carries.

**How it connects:** Seccomp listener, Replay, Serial mode (-T), Signals from the log, Checkpoint signals, Pid pinning, File descriptors.

## Checkpoint signals

A checkpoint signal is a signal that landed while CRIU had the process frozen, after CRIU had already saved its pending signals.

### Three cases

A signal that reached a thread before its first syscall of the Generation is one of three kinds:

- **Pending when CRIU saved the process.** It is in the checkpoint: the restore delivers it, and the copy wakes up inside its handler. Nothing more to do.
- **Landed during the freeze, after the save.** It is not in the checkpoint. Live, the kernel delivered it the moment the process went on, at the exact instruction where the checkpoint caught it. sysrr_replay sends it now, while the copy is still stopped, so the handler runs from the same instruction.
- **Anything else** (it interrupted the first call, or came later while the thread computed): delivered at the thread's first syscall.

### The match test

The Log records each signal's instruction pointer and stack pointer. A signal is sent at the checkpoint only if all of these hold:

- both pointers equal the restored thread's registers;
- it reached the thread within 2 ms of CRIU letting that thread go;
- it is not blocked.

The time test matters in a tight loop, where a thread passes the same instruction again and again.

**How it connects:** Signals from the log, Attach, Checkpoint, Test suite.

## The resume

The resume is the moment the replay lets the copy run: it sends `SIGCONT` to every process of the tree.

From then on, every syscall of every thread comes to sysrr_replay. It answers each one from the Log or lets it run for real, and reports a Divergence when the copy asks for something the Log does not have.

**How it connects:** Replay, Divergence, Seccomp listener.

## Where in the code

- `sysrr_restore.sh`: restore left stopped, clock source, put the config back, check state T.
- `attach_listener` in sysrr_replay.c: the injection steps.
- `main` in sysrr_replay.c: finds the process tree, attaches every listener, holds threads, then `SIGCONT`.
- `signals_at_checkpoint` and `landed_in_freeze` in sysrr_replay.c: Checkpoint signals.
- `hold_thread` and `start_threads` in sysrr_replay.c: threads held for Serial mode.
- `handle_ptrace` in sysrr.bpf.c: how the Recorder notices CRIU and starts a new Generation.

## Common questions

**Why not restore normally and attach afterwards?**
The copy would run, and make syscalls, before the filter is in. Those calls would happen for real, and the replay would start out of step with the Log.

**Why edit /etc/criu/default.conf instead of passing a podman option?**
podman has no option for `leave-stopped`. CRIU reads that file even when podman drives it.

**What does "CRIU checkpointing failed -52" mean?**
That the dump failed, for any reason. Read `dump.log` before the container is removed. Common causes are an open TCP connection, a growing file in the container's layer, and a process stuck in state D.

**Why does a restore fail with "can't write lsm profile -2"?**
podman's AppArmor profile is not loaded since the last boot. Run any container once, then restore again.

**Does ptrace clash with CRIU?**
No. The Recorder uses eBPF, not ptrace. sysrr_replay uses ptrace for a few milliseconds, after CRIU has finished.

**Why disarm the restored timers?**
They would fire in real time, while the replayed program lives in recorded time. Their signals are delivered from the Log instead, at the recorded places.

**Can machine B be the same machine?**
Yes. With `--tcp-established` the copy keeps the original's name, so remove the original first.

**What if the restored process is not stopped?**
sysrr_restore.sh and sysrr_replay both warn. The copy may already have made syscalls the replay never saw, so restore again.
