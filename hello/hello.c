#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <bpf/libbpf.h>
#include "hello.skel.h"

static volatile sig_atomic_t stop;
static void sig_int(int signo) { stop = 1; }

int main(void)
{
	struct hello_bpf *skel;
	int err;

	skel = hello_bpf__open_and_load();
	if (!skel) {
		fprintf(stderr, "failed to open/load BPF skeleton\n");
		return 1;
	}
	err = hello_bpf__attach(skel);
	if (err) {
		fprintf(stderr, "failed to attach: %d\n", err);
		goto cleanup;
	}
	signal(SIGINT, sig_int);
	signal(SIGTERM, sig_int);
	printf("Attached. Run `sudo cat /sys/kernel/tracing/trace_pipe` to see output. Ctrl-C to stop.\n");
	while (!stop)
		sleep(1);
cleanup:
	hello_bpf__destroy(skel);
	return err != 0;
}
