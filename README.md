# sysrr: record syscalls, replay them after a checkpoint

sysrr records every syscall a container process makes, using eBPF. You
checkpoint the container while it keeps running, then restore the checkpoint,
on the same machine or another one. The replayer then feeds the restored copy
the recorded syscall results, so it follows the same path the live process
took. The goal is to find out what made a program fail, including race bugs.

```
machine A                               machine B
container --sysrr--> rec.log ---------> sysrr_replay -g N rec.log
    |  checkpoint N (--leave-running)          ^ seccomp listener
    v                                          |
ckpt.tar.gz -----------------------------> restore (leave-stopped) -> pid
```

- `syscall-rr/`: the tool, its tests and its notes
- `hello/`: a minimal libbpf example, used to check the build setup

## What you need

- Linux x86_64 with BTF (`/sys/kernel/btf/vmlinux` must exist). Tested on
  Debian 13, kernel 6.12.
- clang, libbpf-dev, bpftool, make, pkg-config
- podman and criu (podman's checkpoint and restore use criu)
- root (sudo) to record and to replay

On Debian:

```
sudo apt install clang libbpf-dev bpftool make pkg-config podman criu
```

## Build

```
cd syscall-rr
make
```

This creates `vmlinux.h` from your kernel and builds four tools, plus the
test targets in `tests/`:

| tool | what it does |
|---|---|
| `sysrr` | records a process's syscalls into a log |
| `sysrr_dump` | prints a log in readable form |
| `sysrr_replay` | replays a log into a restored process |
| `raw_sysrr` | bare recorder, used only to measure overhead |

If bpftool isn't in `/usr/sbin`, run `make BPFTOOL=$(which bpftool)`.

## Run

### 1. Record (machine A)

```
PID=$(sudo podman inspect -f '{{.State.Pid}}' CTR)
sudo ./sysrr -p $PID -o rec.log        # add -f to follow child processes
```

Leave it running. In another terminal, checkpoint the container without
stopping it:

```
sudo podman container checkpoint --leave-running -e ckpt.tar.gz CTR
```

Let the container run as long as you want to record, then press Ctrl-C to stop
`sysrr`. Everything recorded after checkpoint N is "generation N", and that's
what a restore of checkpoint N needs.

Check the log:

```
./sysrr_dump -s rec.log         # summary: is anything missing?
./sysrr_dump -g 1 rec.log       # every syscall in generation 1
```

### 2. Restore and replay (machine B, or the same machine)

Copy `ckpt.tar.gz` and `rec.log` over first. Then:

```
# once per boot: makes podman load its AppArmor profile
sudo podman run --rm docker.io/library/debian:trixie-slim true

# use the same clock source on both machines (see the notes below)
echo acpi_pm | sudo tee /sys/devices/system/clocksource/clocksource0/current_clocksource

# restore the container stopped, so it can't make any syscall yet
echo leave-stopped | sudo tee /etc/criu/default.conf
sudo podman container restore --import ckpt.tar.gz --name NEW
sudo rm /etc/criu/default.conf

# replay
sudo ./sysrr_replay -p $(sudo podman inspect -f '{{.State.Pid}}' NEW) -g 1 rec.log
```

`sysrr_replay` prints each syscall it answers. It reports "diverged" as soon
as the restored process asks for something different from the recording.

Useful options:

| option | meaning |
|---|---|
| `-g N` | which generation to replay (match the checkpoint you restored) |
| `-S` | keep the recorded order of syscalls across threads |
| `-T` | run threads one at a time in recorded order (use this when threads share memory) |
| `-e stop\|live\|kill` | what to do at the end of the log: stop the process (default), let it go on for real, or kill it |
| `-w FILE` | save what the process writes to stdout/stderr (those writes are faked during replay) |
| `-c` | keep going when a syscall's input differs from the recording |
| `-v` | more detail |
| `-N` | don't install new file descriptors (only for testing) |

## Tests

```
cd syscall-rr
tests/run_all.sh                # whole suite: record, checkpoint, restore, replay, compare
tests/run_all.sh fork redis     # only tests whose name contains these words
ROUNDS=5 tests/run_all.sh fork  # repeat to catch flaky results
REMOTE=host SSH="ssh -i KEY" tests/run_all.sh   # restore and replay on another machine
tests/run_crashes.sh            # catalogue of crashing programs
```

Some tests pull container images (debian, python, redis, nginx). The pbzip2
test needs a large input file at `tests/real/pbzip2/big.txt`, which isn't in
the repo (45 MB). Any large file works.

## Notes and limits

- Start `sysrr` before you checkpoint. A restore can only replay syscalls
  recorded after its checkpoint.
- vDSO calls (`clock_gettime`, glibc's `getrandom`) don't enter the kernel, so
  they aren't recorded. The `acpi_pm` clock source forces clock calls through
  a real syscall on both machines.
- Signals that arrive from outside (async signals) can't be replayed yet.
  sysrr reports them instead of hanging.
- Checkpointing while running fails if the program keeps growing a file in
  the container's own writable layer. Write such files to a volume instead.

More detail:

- `syscall-rr/DESIGN.md`: how everything works
- `syscall-rr/RECORD.md`: the recording side
- `syscall-rr/REPLAY.md`: the replay side, test results and known limits
- `syscall-rr/docs/`: study guides (HTML)
