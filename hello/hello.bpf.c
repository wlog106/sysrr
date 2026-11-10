// SPDX-License-Identifier: GPL-2.0
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

char LICENSE[] SEC("license") = "GPL";

SEC("tp/syscalls/sys_enter_execve")
int handle_execve(struct trace_event_raw_sys_enter *ctx)
{
	char comm[16];
	const char *filename = (const char *)ctx->args[0];
	char path[64];

	bpf_get_current_comm(comm, sizeof(comm));
	bpf_probe_read_user_str(path, sizeof(path), filename);
	bpf_printk("pid=%d comm=%s exec=%s", bpf_get_current_pid_tgid() >> 32, comm, path);
	return 0;
}
