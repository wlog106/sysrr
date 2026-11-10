#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <bpf/libbpf.h>
#include <asm/unistd_64.h>
#include "./raw_sysrr.skel.h"

struct syscall_event {
    __u32 pid;
    __u32 is_exit;
    __s64 syscall_id;
    __u64 args[6];
    __u32 data_len;
    __u32 pad;
    char  data[256];
};

static volatile sig_atomic_t stop;
static unsigned long long nevents;
static int quiet;

static void on_sig(int s) { stop = 1; }

static int handle_event(void *ctx, void *data, size_t sz)
{
    struct syscall_event *e = data;
    nevents++;
    if (quiet)
        return 0;

    if (e->is_exit && e->syscall_id == __NR_read) {
        printf("pid=%u read fd=%lld ret=%lld captured=%u: ",
               e->pid, (long long)e->args[1], (long long)e->args[0], e->data_len);
        for (__u32 i = 0; i < e->data_len; i++) {
            unsigned char c = e->data[i];
            putchar((c >= 32 && c < 127) ? c : '.');
        }
        putchar('\n');
    } else if (e->syscall_id == __NR_openat) {
        printf("pid=%u openat(\"%.*s\")\n", e->pid, (int)sizeof(e->data), e->data);
    } else {
        printf("pid=%u syscall=%lld\n", e->pid, (long long)e->syscall_id);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <pid> [-q]\n", argv[0]);
        return 1;
    }
    quiet = (argc > 2 && strcmp(argv[2], "-q") == 0);

    struct raw_sysrr_bpf *skel = raw_sysrr_bpf__open();
    if (!skel) { fprintf(stderr, "open failed\n"); return 1; }
    skel->rodata->target_pid = atoi(argv[1]);      // before load

    if (raw_sysrr_bpf__load(skel)) {
        fprintf(stderr, "load failed\n");
        return 1;
    }
    if (raw_sysrr_bpf__attach(skel)) {             // replaces both perf_event_open blocks
        fprintf(stderr, "attach failed: %d\n", -errno);
        return 1;
    }

    struct ring_buffer *rb = ring_buffer__new(bpf_map__fd(skel->maps.events),
                                              handle_event, NULL, NULL);
    if (!rb) { fprintf(stderr, "ring buffer failed\n"); return 1; }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    while (!stop)
        ring_buffer__poll(rb, 100);

    fprintf(stderr, "events received: %llu\n", nevents);
    ring_buffer__free(rb);
    raw_sysrr_bpf__destroy(skel);
    return 0;
}

