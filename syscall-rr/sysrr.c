/* sysrr recorder, user side: load the BPF programs for one target process and
 * stream its syscall records into a log file.
 *
 *   sudo ./sysrr -p <host pid> [-f] [-r] [-o sysrr.log] [-b ring_mb]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "sysrr.h"
#include "sysrr.skel.h"

static volatile sig_atomic_t stop;
static FILE *out;
static struct rec_stats stats = { .h = { .type = REC_STATS, .len = sizeof(struct rec_stats) } };

static void on_sig(int s) { stop = 1; }

static int handle_rec(void *ctx, void *data, size_t sz)
{
	struct rec_hdr *h = data;

	switch (h->type) {
	case REC_SYSCALL:
		stats.syscalls++;
		break;
	case REC_DATA:
		stats.data_records++;
		stats.data_bytes += ((struct rec_data *)data)->len;
		break;
	case REC_MARKER: {
		struct rec_marker *m = data;
		static const char *k[] = { "?", "CRIU seized", "CRIU detached", "new generation" };
		fprintf(stderr, "[marker] %s tid=%u by pid %u -> gen %u\n",
			k[m->kind < 4 ? m->kind : 0], m->tid, m->tracer, m->gen);
		break;
	}
	}
	if (fwrite(data, h->len, 1, out) != 1) {
		perror("write");
		stop = 1;
		return -1;
	}
	stats.bytes_written += h->len;
	return 0;
}

/* cgroup v2 id of a process = inode number of its cgroup directory */
static unsigned long long cgroup_id_of(int pid)
{
	char path[64], line[4096], dir[4200];
	struct stat st;
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/cgroup", pid);
	if (!(f = fopen(path, "r")))
		return 0;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "0::", 3))
			continue;
		line[strcspn(line, "\n")] = 0;
		snprintf(dir, sizeof(dir), "/sys/fs/cgroup%s", line + 3);
		fclose(f);
		return stat(dir, &st) ? 0 : st.st_ino;
	}
	fclose(f);
	return 0;
}

/* NSpid line of /proc/<pid>/status: host pid followed by the pid in each
 * nested namespace; the last one is the innermost (the container's) */
static int ns_pid_of(int pid)
{
	char path[64], line[256];
	int nspid = pid;
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/status", pid);
	if (!(f = fopen(path, "r")))
		return pid;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "NSpid:", 6))
			continue;
		char *p = line + 6, *end;
		for (;;) {
			long v = strtol(p, &end, 10);
			if (end == p)
				break;
			nspid = v;
			p = end;
		}
	}
	fclose(f);
	return nspid;
}

/* ppid of a process, from /proc/<pid>/stat ("pid (comm) state ppid ...";
 * comm may contain spaces and parentheses: parse after the last ')') */
static int ppid_of(int pid)
{
	char path[64], buf[512], *p;
	int ppid = -1;
	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	if (fgets(buf, sizeof(buf), f) && (p = strrchr(buf, ')')))
		sscanf(p + 2, "%*c %d", &ppid);
	fclose(f);
	return ppid;
}

/* follow mode: the processes the target already forked (nginx's workers)
 * are targets from the start; later forks are added by on_fork */
static int add_descendants(struct sysrr_bpf *skel, int root)
{
	int added = 0, grew = 1;
	__u8 one = 1;

	while (grew) {                      /* until no new child is found */
		grew = 0;
		DIR *d = opendir("/proc");
		for (struct dirent *de; d && (de = readdir(d)); ) {
			__u32 pid = strtoul(de->d_name, NULL, 10), pp;
			__u8 v;
			if (!pid || pid == (__u32)root ||
			    !bpf_map__lookup_elem(skel->maps.targets, &pid, sizeof(pid), &v, sizeof(v), 0))
				continue;
			pp = ppid_of(pid);
			if (bpf_map__lookup_elem(skel->maps.targets, &pp, sizeof(pp), &v, sizeof(v), 0))
				continue;
			bpf_map__update_elem(skel->maps.targets, &pid, sizeof(pid), &one, sizeof(one), BPF_ANY);
			added++, grew = 1;
		}
		if (d)
			closedir(d);
	}
	return added;
}

static void usage(const char *p)
{
	fprintf(stderr,
		"usage: %s -p PID [-f] [-r] [-o FILE] [-b MB]\n"
		"  -p PID   host pid of the process to record\n"
		"  -f       follow: also record processes it forks\n"
		"  -r       race check: copy input buffers again at syscall exit, so\n"
		"           sysrr_dump can show inputs changed during the syscall\n"
		"  -o FILE  output log (default sysrr.log)\n"
		"  -b MB    ring buffer size in MiB, power of two (default 64)\n", p);
	exit(1);
}

int main(int argc, char **argv)
{
	const char *outpath = "sysrr.log";
	int pid = 0, follow = 0, race = 0, ring_mb = 64, opt;

	while ((opt = getopt(argc, argv, "p:fro:b:")) != -1) {
		switch (opt) {
		case 'p': pid = atoi(optarg); break;
		case 'f': follow = 1; break;
		case 'r': race = 1; break;
		case 'o': outpath = optarg; break;
		case 'b': ring_mb = atoi(optarg); break;
		default: usage(argv[0]);
		}
	}
	if (pid <= 0)
		usage(argv[0]);

	char nspath[64];
	struct stat nsst;
	snprintf(nspath, sizeof(nspath), "/proc/%d/ns/pid", pid);
	if (stat(nspath, &nsst)) {
		fprintf(stderr, "no such process %d: %s\n", pid, strerror(errno));
		return 1;
	}

	struct sysrr_bpf *skel = sysrr_bpf__open();
	if (!skel) {
		fprintf(stderr, "open failed\n");
		return 1;
	}
	skel->rodata->target_tgid = pid;
	skel->rodata->follow = follow;
	skel->rodata->race_check = race;
	skel->rodata->pidns_dev = nsst.st_dev;
	skel->rodata->pidns_ino = nsst.st_ino;
	/* follow mode does a hash lookup per syscall; first reject everything
	 * outside the target's cgroup with one compare */
	skel->rodata->target_cgid = follow ? cgroup_id_of(pid) : 0;
	skel->rodata->wake_thresh = ((unsigned long long)ring_mb << 20) / 4;
	bpf_map__set_max_entries(skel->maps.events, ring_mb << 20);

	if (sysrr_bpf__load(skel)) {
		fprintf(stderr, "load failed\n");
		return 1;
	}
	if (follow) {
		__u32 k = pid;
		__u8 one = 1;
		bpf_map__update_elem(skel->maps.targets, &k, sizeof(k), &one, sizeof(one), BPF_ANY);
		int n = add_descendants(skel, pid);
		if (n)
			fprintf(stderr, "follow: %d existing descendant process(es) of %d added\n", n, pid);
	}

	if (!(out = fopen(outpath, "w"))) {
		perror(outpath);
		return 1;
	}
	setvbuf(out, NULL, _IOFBF, 4 << 20);

	struct sysrr_file_hdr fh = {
		.magic = SYSRR_MAGIC, .version = SYSRR_VERSION, .hdr_size = sizeof(fh),
		.target_tgid = pid, .target_ns_tgid = ns_pid_of(pid),
		.pidns_ino = nsst.st_ino, .follow = follow,
		.opts = race ? OPT_RACECHECK : 0,
	};
	struct timespec ts;
	struct utsname u;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	fh.start_mono_ns = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
	uname(&u);
	snprintf(fh.kernel, sizeof(fh.kernel), "%s", u.release);
	snprintf(nspath, sizeof(nspath), "/proc/%d/comm", pid);
	FILE *cf = fopen(nspath, "r");
	if (cf) {
		if (fgets(fh.comm, sizeof(fh.comm), cf))
			fh.comm[strcspn(fh.comm, "\n")] = 0;
		fclose(cf);
	}
	fwrite(&fh, sizeof(fh), 1, out);

	struct ring_buffer *rb = ring_buffer__new(bpf_map__fd(skel->maps.events),
						  handle_rec, NULL, NULL);
	if (!rb) {
		fprintf(stderr, "ring buffer failed\n");
		return 1;
	}
	if (sysrr_bpf__attach(skel)) {
		fprintf(stderr, "attach failed: %s\n", strerror(errno));
		return 1;
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	fprintf(stderr, "recording pid %d (ns pid %d, %s)%s%s -> %s\n", pid,
		fh.target_ns_tgid, fh.comm, follow ? " +children" : "",
		race ? " +race-check" : "", outpath);

	/* the BPF side only wakes us when the ring is 1/4 full, so poll with
	 * a short timeout to keep latency (and ring usage) low */
	while (!stop) {
		int err = ring_buffer__poll(rb, 10);
		if (err < 0 && err != -EINTR) {
			fprintf(stderr, "poll: %d\n", err);
			break;
		}
		if (kill(pid, 0) && errno == ESRCH && !follow)
			stop = 1;
	}
	sysrr_bpf__detach(skel);
	ring_buffer__consume(rb);

	/* sum the per-CPU lost counters */
	int ncpu = libbpf_num_possible_cpus();
	__u64 vals[ncpu];
	__u32 zero = 0;
	if (!bpf_map__lookup_elem(skel->maps.lost, &zero, sizeof(zero), vals, sizeof(vals), 0))
		for (int i = 0; i < ncpu; i++)
			stats.lost += vals[i];
	stats.bytes_written += sizeof(stats);
	fwrite(&stats, sizeof(stats), 1, out);
	fclose(out);

	fprintf(stderr, "syscalls=%llu data_records=%llu data_bytes=%llu lost=%llu "
		"generations=%u file=%.1f MiB\n",
		(unsigned long long)stats.syscalls, (unsigned long long)stats.data_records,
		(unsigned long long)stats.data_bytes, (unsigned long long)stats.lost,
		skel->bss->gen + 1, stats.bytes_written / 1048576.0);
	if (stats.lost)
		fprintf(stderr, "WARNING: records were lost, this log cannot be replayed reliably "
			"(try a bigger -b)\n");

	ring_buffer__free(rb);
	sysrr_bpf__destroy(skel);
	return stats.lost ? 2 : 0;
}
