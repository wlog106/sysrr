BPFTOOL ?= /usr/sbin/bpftool
CLANG   ?= clang
ARCH    := $(shell uname -m | sed 's/x86_64/x86/;s/aarch64/arm64/')
APP     := hello

all: $(APP)

vmlinux.h:
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@

%.bpf.o: %.bpf.c vmlinux.h
	$(CLANG) -g -O2 -target bpf -D__TARGET_ARCH_$(ARCH) -I. -c $< -o $@

%.skel.h: %.bpf.o
	$(BPFTOOL) gen skeleton $< > $@

$(APP): $(APP).c $(APP).skel.h
	$(CC) -g -O2 -Wall -I. $< -o $@ $(shell pkg-config --libs libbpf)

clean:
	rm -f *.o *.skel.h $(APP)

.PHONY: all clean
.SECONDARY:
