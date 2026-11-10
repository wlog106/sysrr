#include "../vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <asm/unistd.h>

char LICENSE[] SEC("license") = "GPL";

volatile const __u32 target_pid = 0;

struct syscall_event {
    __u32 pid;
    __u32 is_exit;
    __s64 syscall_id;
    __u64 args[6];
    __u32 data_len;
    __u32 pad;
    char  data[256];
};

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 256 * 1024);
} events SEC(".maps");

struct pending_read {
    __u64 buf;
    __s32 fd;
};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, __u64);
    __type(value, struct pending_read);
} pending SEC(".maps");

static __always_inline void capture(struct syscall_event *e, const void *ptr, __u32 len)
{
    if (len > sizeof(e->data))
        len = sizeof(e->data);
    long ret = bpf_probe_read_user(e->data, len, ptr);
    e->data_len = (ret == 0) ? len : 0;
}

SEC("tp_btf/sys_enter")
int BPF_PROG(trace_enter, struct pt_regs *regs, long id)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    if ((__u32)(pid_tgid >> 32) != target_pid)
        return 0;                       // cheap exit for every other process

    // x86_64 syscall argument registers (note r10, not rcx)
    __u64 a0 = regs->di;
    __u64 a1 = regs->si;
    __u64 a2 = regs->dx;
    __u64 a3 = regs->r10;
    __u64 a4 = regs->r8;
    __u64 a5 = regs->r9;

    if (id == __NR_read) {
        struct pending_read pr = { .buf = a1, .fd = (__s32)a0 };
        bpf_map_update_elem(&pending, &pid_tgid, &pr, BPF_ANY);
        return 0;
    }

    struct syscall_event *e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e) return 0;

    __builtin_memset(e, 0, sizeof(*e));
    e->pid = pid_tgid >> 32;
    e->syscall_id = id;
    e->args[0] = a0;
    e->args[1] = a1;
    e->args[2] = a2;
    e->args[3] = a3;
    e->args[4] = a4;
    e->args[5] = a5;

    switch (id) {
    case __NR_write:
        capture(e, (void *)a1, (__u32)a2);
        break;
    case __NR_openat:
        bpf_probe_read_user_str(e->data, sizeof(e->data), (void *)a1);
        e->data_len = sizeof(e->data);
        break;
    }

    bpf_ringbuf_submit(e, 0);
    return 0;
}

SEC("tp_btf/sys_exit")
int BPF_PROG(trace_exit, struct pt_regs *regs, long ret)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    if ((__u32)(pid_tgid >> 32) != target_pid)
        return 0;

    struct pending_read *pr = bpf_map_lookup_elem(&pending, &pid_tgid);
    if (!pr)
        return 0;

    struct pending_read local = *pr;
    bpf_map_delete_elem(&pending, &pid_tgid);

    if (ret <= 0)
        return 0;

    struct syscall_event *e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e) return 0;

    __builtin_memset(e, 0, sizeof(*e));
    e->pid = pid_tgid >> 32;
    e->is_exit = 1;
    e->syscall_id = __NR_read;
    e->args[0] = ret;
    e->args[1] = local.fd;
    capture(e, (void *)local.buf, (__u32)ret);

    bpf_ringbuf_submit(e, 0);
    return 0;
}

