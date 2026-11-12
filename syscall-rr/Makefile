BPFTOOL   ?= /usr/sbin/bpftool
CLANG     ?= clang
ARCH      := $(shell uname -m | sed 's/x86_64/x86/;s/aarch64/arm64/')
MULTIARCH := $(shell $(CC) -print-multiarch)
BPF_CFLAGS := -g -O2 -target bpf -mcpu=v3 -D__TARGET_ARCH_$(ARCH) -I/usr/include/$(MULTIARCH) -Wno-gnu-variable-sized-type-not-at-end
CFLAGS    ?= -g -O2 -Wall
LIBBPF    := $(shell pkg-config --libs libbpf)

APPS := sysrr sysrr_dump sysrr_replay raw_sysrr

TARGETS := tests/ckpt_target tests/mpmt_target tests/sigext_target tests/net_target tests/file_target tests/reexec_target tests/sigsync_target tests/pool_target tests/race_target tests/mt_target tests/fd_target tests/sc_target \
	   tests/fork_target tests/thread_target tests/signal_target tests/exit_target tests/rand_target tests/crash_target tests/queue_race_target

all: $(APPS) $(TARGETS)

../vmlinux.h:
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@

%.bpf.o: %.bpf.c ../vmlinux.h sysrr.h
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@

%.skel.h: %.bpf.o
	$(BPFTOOL) gen skeleton $< > $@

syscall_names.h: /usr/include/$(MULTIARCH)/asm/unistd_64.h
	awk '/^#define __NR_/ { sub("__NR_", "", $$2); printf "[%s] = \"%s\",\n", $$3, $$2 }' $< > $@

sysrr: sysrr.c sysrr.skel.h sysrr.h
	$(CC) $(CFLAGS) $< -o $@ $(LIBBPF)

sysrr_dump: sysrr_dump.c sysrr.h syscall_names.h
	$(CC) $(CFLAGS) $< -o $@

sysrr_replay: sysrr_replay.c sysrr.h syscall_names.h
	$(CC) $(CFLAGS) $< -o $@

raw_sysrr: raw_sysrr.c raw_sysrr.skel.h
	$(CC) $(CFLAGS) $< -o $@ $(LIBBPF)

$(TARGETS): %: %.c
	$(CC) $(CFLAGS) -pthread $< -o $@ -lrt

clean:
	rm -f *.bpf.o *.skel.h syscall_names.h $(APPS) $(TARGETS)

.PHONY: all clean
.SECONDARY:
