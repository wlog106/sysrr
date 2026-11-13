// SPDX-License-Identifier: GPL-2.0
/* sysrr recorder, kernel side.
 *
 * sys_enter: remember seq/timestamp/args/generation of the target thread's
 *            syscall in task-local storage, and copy the user buffers the
 *            kernel is about to read (REC_DATA, D_IN/D_STR).
 * sys_exit:  now that the result is known, copy the user buffers the
 *            kernel wrote (REC_DATA, D_OUT) and commit it (REC_SYSCALL).
 *            With race_check, copy the inputs a second time (DF_EXIT).
 *
 * Every process on the machine runs these programs on every syscall, so the
 * non-target path must stay a couple of compares.
 *
 * CRIU detection: CRIU checkpoints a task by PTRACE_SEIZE-ing every thread,
 * injecting "parasite" code that runs syscalls *inside* the target, then
 * PTRACE_DETACH.  We watch ptrace(2) calls aimed at our target: syscalls made
 * while seized are flagged F_DUMP, and when the last thread is detached the
 * generation counter is bumped.  Generation N = everything after the N-th
 * checkpoint, i.e. what a restore of that checkpoint has to replay.
 */
#include "../vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include <asm/unistd.h>
#include "sysrr.h"

char LICENSE[] SEC("license") = "GPL";

#define PTRACE_ATTACH  16
#define PTRACE_DETACH  17
#define PTRACE_SEIZE   0x4206
#define IOV_MAX_CAP    16

/* set by user space before load */
volatile const u32 target_tgid;
volatile const u32 follow;          /* also record descendants (uses `targets`) */
volatile const u64 target_cgid;     /* 0 = no cgroup pre-filter */
volatile const u64 pidns_dev, pidns_ino;
volatile const u64 wake_thresh;     /* wake user space only past this many bytes */
volatile const u32 race_check;      /* re-copy inputs at sys_exit (-r) */
volatile const u32 any_tracer;      /* -t: any ptrace attach counts as a checkpoint, not only criu's */

u64 seq_ctr;
u32 gen;
s32 seized_cnt;

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 64 << 20);      /* resized by user space */
} events SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 4096);
	__type(key, u32);
	__type(value, u8);
} targets SEC(".maps");

struct task_state {
	u64 seq;
	u64 ts;
	s64 nr;                         /* syscall number at entry (rt_sigreturn overwrites orig_ax) */
	u64 args[6];
	u32 gen;
	u32 gen_min;                    /* detached by CRIU: its syscalls belong to the next gen */
	u16 flags;
	u16 ndata;
	u8  active;                     /* inside a recorded syscall */
	u8  seized;                     /* CRIU currently has this thread */
	u8  in_fault;                   /* an input could not be copied at sys_enter */
};

struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct task_state);
} tstate SEC(".maps");

/* staging area for one REC_DATA record; per-CPU is safe because syscall
 * tracepoints run with preemption disabled */
struct scratch { struct rec_data d; char buf[CHUNK_MAX]; };
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct scratch);
} scratch SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, u64);
} lost SEC(".maps");

extern struct task_struct *bpf_task_from_pid(s32 pid) __ksym;
extern void bpf_task_release(struct task_struct *p) __ksym;

/* Waking the consumer on every record was most of the old overhead (an
 * irq_work + context switch per syscall).  Only wake it when the ring is
 * filling up; otherwise it picks data up on its 10ms poll. */
static __always_inline u64 wake_flags(void)
{
	return bpf_ringbuf_query(&events, BPF_RB_AVAIL_DATA) >= wake_thresh ?
	       BPF_RB_FORCE_WAKEUP : BPF_RB_NO_WAKEUP;
}

static __always_inline void count_lost(void)
{
	u32 zero = 0;
	u64 *v = bpf_map_lookup_elem(&lost, &zero);
	if (v)
		(*v)++;
}

static __always_inline bool is_target(u32 tgid)
{
	if (!follow)
		return tgid == target_tgid;
	return bpf_map_lookup_elem(&targets, &tgid) != NULL;
}

/* ---- capturing user memory ------------------------------------------- */

static __always_inline u64 user_u32(u64 p)
{
	u32 v = 0;
	if (p)
		bpf_probe_read_user(&v, sizeof(v), (void *)p);
	return v;
}

#define META(arg, kind, idx)  ((arg) | ((kind) << 8) | ((u32)(idx) << 16))
/* or-ed into a data kind: this is the sys_exit copy of an input (DF_EXIT) */
#define KX                    0x80
/* emit_* return: F_* flags in the low byte, number of records above it */
#define RES(flags, n)         ((flags) | ((n) << 8))
/* merge two RES values: OR the flags, add the counts */
#define ACC(x) do { int _r = (x); \
	res = ((res | _r) & 0xff) | (((res >> 8) + (_r >> 8)) << 8); } while (0)

/* bpf_loop context.  The callback must not carry counters from one
 * iteration to the next (the verifier would explore every value as a new
 * state); the chunk offset is derived from the loop index instead. */
struct chunk_ctx {
	u64 ptr, len, seq;
	u32 tid, meta;
	u32 flags;
	u32 fail_at;           /* index of the chunk that failed, or ~0 */
};

static long chunk_cb(u64 i, void *c)
{
	struct chunk_ctx *x = c;
	u32 zero = 0;
	u64 off = i * CHUNK_MAX;
	struct scratch *s;

	if (off >= x->len)
		return 1;
	s = bpf_map_lookup_elem(&scratch, &zero);
	if (!s)
		return 1;

	u64 n = x->len - off;
	if (n > CHUNK_MAX)
		n = CHUNK_MAX;

	if (bpf_probe_read_user(s->buf, n, (void *)(x->ptr + off))) {
		/* not resident: tracepoints cannot fault pages in */
		x->flags |= F_FAULT;
		x->fail_at = i;
		return 1;
	}
	s->d.h.type = REC_DATA;
	s->d.h.flags = (x->meta >> 8) & KX ? DF_EXIT : 0;
	s->d.h.len = sizeof(s->d) + n;
	s->d.h.seq = x->seq;
	s->d.tid = x->tid;
	s->d.arg = x->meta & 0xff;
	s->d.kind = (x->meta >> 8) & 0x7f;
	s->d.idx = x->meta >> 16;
	s->d.offset = off;
	s->d.len = n;
	if (bpf_ringbuf_output(&events, s, sizeof(s->d) + n, wake_flags())) {
		count_lost();
		x->flags |= F_LOST;
		x->fail_at = i;
		return 1;
	}
	return 0;
}

/* Copy [ptr, ptr+len) as one or more REC_DATA chunks.  A global (non-static)
 * function is verified once instead of at each of its many call sites. */
__noinline int emit_buf(u64 ptr, u64 len, u64 seq, u32 tid, u32 meta)
{
	struct chunk_ctx x = { .ptr = ptr, .len = len, .seq = seq,
			       .tid = tid, .meta = meta, .fail_at = ~0U };
	u32 n;

	if (!ptr || !len)
		return 0;
	if (len > (u64)CHUNK_MAX * CHUNKS_MAX) {
		x.len = (u64)CHUNK_MAX * CHUNKS_MAX;
		x.flags |= F_TRUNC;
	}
	n = (x.len + CHUNK_MAX - 1) / CHUNK_MAX;
	bpf_loop(n, chunk_cb, &x, 0);
	if (x.fail_at < n)
		n = x.fail_at;
	return RES(x.flags, n);
}

__noinline int emit_str(u64 ptr, u64 seq, u32 tid, u32 meta)
{
	u32 zero = 0;
	struct scratch *s = bpf_map_lookup_elem(&scratch, &zero);
	if (!s || !ptr)
		return 0;

	long n = bpf_probe_read_user_str(s->buf, STR_MAX, (void *)ptr);
	if (n < 0)
		return F_FAULT;
	/* The helper counts the NUL ("abc" -> 4) except for an empty string,
	 * where it returns 0, not 1 (strncpy_from_user_nofault only adds the
	 * NUL when ret > 0).  "" is common: glibc's fstat() on older glibc is
	 * newfstatat(fd, "", buf, AT_EMPTY_PATH).  Record it as 1 byte. */
	if (n == 0) {
		s->buf[0] = 0;
		n = 1;
	}
	if (n > STR_MAX)
		n = STR_MAX;
	s->d.h.type = REC_DATA;
	s->d.h.flags = (meta >> 8) & KX ? DF_EXIT : 0;
	s->d.h.len = sizeof(s->d) + n;
	s->d.h.seq = seq;
	s->d.tid = tid;
	s->d.arg = meta & 0xff;
	s->d.kind = (meta >> 8) & 0x7f;
	s->d.idx = meta >> 16;
	s->d.offset = 0;
	s->d.len = n;
	if (bpf_ringbuf_output(&events, s, sizeof(s->d) + n, wake_flags())) {
		count_lost();
		return F_LOST;
	}
	return RES(0, 1);
}

/* Scatter/gather: copy the iovec array itself, then up to `budget` bytes of
 * the buffers it points to (budget = syscall return value). */
#define TIDK(tid, arg, kind)  ((tid) | ((u64)((arg) | ((kind) << 8)) << 32))

/* (BPF functions take at most 5 arguments, hence tid/arg/kind packed in tidk) */
__noinline int emit_iov(u64 iovp, u64 cnt, u64 budget, u64 seq, u64 tidk)
{
	int res = 0;
	u32 tid = (u32)tidk, arg = (tidk >> 32) & 0xff, kind = (tidk >> 40) & 0xff;

	if (!iovp)
		return 0;
	if (cnt > IOV_MAX_CAP) {
		res |= F_TRUNC;
		cnt = IOV_MAX_CAP;
	}
	ACC(emit_buf(iovp, cnt * sizeof(struct iovec), seq, tid, META(arg, D_IN | (kind & KX), 0xffff)));
	for (int i = 0; i < IOV_MAX_CAP; i++) {
		struct iovec iov;
		if (i >= cnt || !budget)
			break;
		if (bpf_probe_read_user(&iov, sizeof(iov), (void *)(iovp + i * sizeof(iov))))
			return res | F_FAULT;
		u64 l = iov.iov_len < budget ? iov.iov_len : budget;
		ACC(emit_buf((u64)iov.iov_base, l, seq, tid, META(arg, kind, i)));
		budget -= l;
	}
	return res;
}

/* recvmsg/sendmsg: the msghdr (for recvmsg after the call, so namelen/
 * controllen/flags are the kernel's values), the peer address, ancillary data
 * and up to `ret` bytes of payload */
__noinline int emit_msghdr(u64 mp, s64 ret, u64 seq, u32 tid, u32 kind_arg)
{
	struct user_msghdr m;
	u32 kind = kind_arg & 0xff, arg = kind_arg >> 8 ? kind_arg >> 8 : 1;
	int res;

	if (!mp)
		return 0;
	if (bpf_probe_read_user(&m, sizeof(m), (void *)mp))
		return F_FAULT;
	res = emit_buf(mp, sizeof(m), seq, tid, META(arg, kind, 0xfffe));
	ACC(emit_buf((u64)m.msg_name, m.msg_namelen & 0xff, seq, tid, META(arg, kind, 0xfffd)));
	ACC(emit_buf((u64)m.msg_control, m.msg_controllen & 0xffff, seq, tid, META(arg, kind, 0xfffc)));
	if (ret > 0)
		ACC(emit_iov((u64)m.msg_iov, m.msg_iovlen, ret, seq, TIDK(tid, arg, kind)));
	return res;
}

/* recvmmsg: an array of { msghdr, msg_len }.  Message i is tagged arg = 8 + i
 * so the replay finds its msghdr; msg_len is how much of it holds data. */
#define SZ_MMSGHDR    64
#define MMSG_MAX      8
__noinline int emit_mmsg(u64 vec, u64 cnt, u64 seq, u32 tid)
{
	int res = 0;

	for (int i = 0; i < MMSG_MAX; i++) {
		if (i >= cnt)
			break;
		u64 mp = vec + i * SZ_MMSGHDR;
		ACC(emit_msghdr(mp, user_u32(mp + 56), seq, tid, D_OUT | ((8 + i) << 8)));
	}
	if (cnt > MMSG_MAX)
		res |= F_TRUNC;
	return res;
}

/* ---- per-syscall tables: which memory matters for replay -----------------
 * D_OUT: written by the kernel. Replay injects it instead of running the call.
 * D_IN / D_STR: read by the kernel. Replay compares it to detect divergence.
 * Struct sizes are x86_64 ABI sizes. */

#define OUT(i, p, l)  ACC(emit_buf((p), (l), seq, tid, META(i, D_OUT, 0)))
#define IN(i, p, l)   ACC(emit_buf((p), (l), seq, tid, META(i, D_IN | x, 0)))
#define STR(i, p)     ACC(emit_str((p), seq, tid, META(i, D_STR | x, 0)))
/* in/out argument: the kernel overwrites it, so only the sys_enter copy is
 * the input (the result is captured by capture_exit) */
#define INOUT(i, p, l) if (!x) ACC(emit_buf((p), (l), seq, tid, META(i, D_IN, 0)))
#define OK            (ret >= 0)
#define RLEN          (ret > 0 ? (u64)ret : 0)

#define SZ_STAT       144
#define SZ_STATX      256
#define SZ_SOCKADDR   128
#define SZ_RUSAGE     144
#define SZ_SIGINFO    128
#define SZ_TIMESPEC   16

/* Inputs, captured at sys_enter.  The kernel copies most inputs right after
 * entry and the syscall may then block for a long time (open of a FIFO,
 * connect), so entry is the narrower window for another thread to change the
 * buffer before we copy it.  The size actually consumed (ret) is not known
 * yet, so the requested size is copied.
 *
 * Cost of copying early: the page may not be resident yet (first touch, e.g.
 * a string constant of a freshly exec'd binary) and tracepoints cannot fault
 * it in.  Then the inputs are copied again at sys_exit, after the kernel has
 * touched the page.
 *
 * x = 0 at sys_enter.  x = KX when race_check copies the pure inputs a second
 * time at sys_exit; in/out arguments are skipped then. */
static __always_inline int capture_enter(s64 nr, u64 *a, u64 seq, u32 tid, u32 x)
{
	int res = 0;

	switch (nr) {
	/* paths */
	case __NR_open:
	case __NR_access:
	case __NR_stat:
	case __NR_lstat:
	case __NR_readlink:         STR(0, a[0]); break;
	case __NR_openat:
	case __NR_faccessat:
	case __NR_faccessat2:
	case __NR_newfstatat:
	case __NR_statx:
	case __NR_readlinkat:       STR(1, a[1]); break;

	/* data going out: recorded so replay can check it is on the same path */
	case __NR_write:
	case __NR_pwrite64:         IN(1, a[1], a[2]); break;
	case __NR_writev:
	case __NR_pwritev:
	case __NR_pwritev2:         ACC(emit_iov(a[1], a[2], ~0ULL, seq, TIDK(tid, 1, D_IN | x))); break;
	case __NR_sendto:           IN(1, a[1], a[2]); IN(4, a[4], a[5] & 0xff); break;
	case __NR_sendmsg:          ACC(emit_msghdr(a[1], 1LL << 40, seq, tid, D_IN | x)); break;
	case __NR_connect:
	case __NR_bind:             IN(1, a[1], a[2] & 0xff); break;
	case __NR_nanosleep:        IN(0, a[0], SZ_TIMESPEC); break;
	case __NR_clock_nanosleep:  IN(2, a[2], SZ_TIMESPEC); break;
	/* what an fd is armed for, and the data its events will carry back (often a
	 * pointer): the replay checks it, and ties each recorded event to its ADD */
	case __NR_epoll_ctl:        if (a[1] != 2 /* DEL */ && a[3]) IN(3, a[3], 12); break;

	/* in/out */
	case __NR_poll:
	case __NR_ppoll:            INOUT(0, a[0], a[1] * 8); break;
	case __NR_select:
	case __NR_pselect6:         { u64 l = (a[0] + 7) / 8;
				      INOUT(1, a[1], l); INOUT(2, a[2], l); INOUT(3, a[3], l); }
				    break;
	case __NR_accept:
	case __NR_accept4:
	case __NR_getsockname:
	case __NR_getpeername:      INOUT(2, a[2], 4); break;
	case __NR_getsockopt:       INOUT(4, a[4], 4); break;
	/* (idx 0xfffe = "the msghdr itself", same as its output copy) */
	case __NR_recvmsg:          if (!x) ACC(emit_buf(a[1], sizeof(struct user_msghdr), seq, tid, META(1, D_IN, 0xfffe))); break;
	}
	return res;
}

/* Outputs, captured at sys_exit: only now does the buffer hold the result,
 * and ret says how much of it is valid. */
static __always_inline int capture_exit(s64 nr, u64 *a, s64 ret, u64 seq, u32 tid)
{
	int res = 0;

	switch (nr) {
	/* data coming in */
	case __NR_read:
	case __NR_pread64:          OUT(1, a[1], RLEN); break;
	case __NR_readv:
	case __NR_preadv:
	case __NR_preadv2:          ACC(emit_iov(a[1], a[2], RLEN, seq, TIDK(tid, 1, D_OUT))); break;
	case __NR_recvfrom:         OUT(1, a[1], RLEN);
				    if (OK && a[4]) { OUT(4, a[4], SZ_SOCKADDR); OUT(5, a[5], 4); }
				    break;
	case __NR_recvmsg:          if (OK) ACC(emit_msghdr(a[1], ret, seq, tid, D_OUT)); break;
	case __NR_getrandom:        OUT(0, a[0], RLEN); break;
	case __NR_getdents64:
	case __NR_getdents:         OUT(1, a[1], RLEN); break;
	case __NR_getcwd:           OUT(0, a[0], RLEN); break;
	case __NR_readlink:         OUT(1, a[1], RLEN); break;
	case __NR_readlinkat:       OUT(2, a[2], RLEN); break;
	case __NR_sched_getaffinity: OUT(2, a[2], RLEN); break;

	/* time */
	case __NR_clock_gettime:
	case __NR_clock_getres:     if (OK) OUT(1, a[1], SZ_TIMESPEC); break;
	case __NR_gettimeofday:     if (OK) { OUT(0, a[0], 16); OUT(1, a[1], 8); } break;
	case __NR_time:             OUT(0, a[0], 8); break;
	case __NR_nanosleep:        OUT(1, a[1], SZ_TIMESPEC); break;
	case __NR_clock_nanosleep:  OUT(3, a[3], SZ_TIMESPEC); break;
	case __NR_times:            OUT(0, a[0], 32); break;

	/* file metadata */
	case __NR_stat:
	case __NR_lstat:
	case __NR_fstat:            if (OK) OUT(1, a[1], SZ_STAT); break;
	case __NR_newfstatat:       if (OK) OUT(2, a[2], SZ_STAT); break;
	case __NR_statx:            if (OK) OUT(4, a[4], SZ_STATX); break;

	/* readiness / waiting */
	case __NR_poll:
	case __NR_ppoll:            OUT(0, a[0], a[1] * 8); break;
	case __NR_select:
	case __NR_pselect6:         { u64 l = (a[0] + 7) / 8;
				      OUT(1, a[1], l); OUT(2, a[2], l); OUT(3, a[3], l); }
				    break;
	case __NR_epoll_wait:
	case __NR_epoll_pwait:
	case __NR_epoll_pwait2:     OUT(1, a[1], RLEN * 12); break;
	case __NR_wait4:            if (OK) { OUT(1, a[1], 4); OUT(3, a[3], SZ_RUSAGE); } break;
	case __NR_waitid:           if (OK) { OUT(2, a[2], SZ_SIGINFO); OUT(4, a[4], SZ_RUSAGE); } break;
	case __NR_rt_sigtimedwait:  OUT(1, a[1], SZ_SIGINFO); break;

	/* sockets & fds */
	case __NR_accept:
	case __NR_accept4:
	case __NR_getsockname:
	case __NR_getpeername:      if (OK && a[1]) { OUT(1, a[1], user_u32(a[2]) & 0xff); OUT(2, a[2], 4); } break;
	case __NR_getsockopt:       if (OK) { OUT(3, a[3], user_u32(a[4]) & 0xfff); OUT(4, a[4], 4); } break;
	case __NR_pipe:
	case __NR_pipe2:            if (OK) OUT(0, a[0], 8); break;
	case __NR_socketpair:       if (OK) OUT(3, a[3], 8); break;
	case __NR_ioctl:            if (!OK) break;
				    if (a[1] == 0x541B /* FIONREAD */)   OUT(2, a[2], 4);
				    if (a[1] == 0x5401 /* TCGETS */)     OUT(2, a[2], 36);
				    if (a[1] == 0x5413 /* TIOCGWINSZ */) OUT(2, a[2], 8);
				    if (a[1] == 2 /* FIGETBSZ */)        OUT(2, a[2], 4);
				    /* SIOCGIF*: all take a struct ifreq */
				    if ((a[1] & 0xff00) == 0x8900 && a[1] != 0x8912) OUT(2, a[2], 40);
				    /* new-style numbers carry direction and size: _IOR, _IOWR */
				    if ((a[1] >> 30) & 2)                OUT(2, a[2], (a[1] >> 16) & 0x3fff);
				    break;
	case __NR_recvmmsg:         if (OK) { OUT(1, a[1], RLEN * SZ_MMSGHDR); ACC(emit_mmsg(a[1], RLEN, seq, tid)); } break;
	case __NR_sendmmsg:         if (OK) OUT(1, a[1], RLEN * SZ_MMSGHDR); break;

	/* more file metadata */
	case __NR_statfs:
	case __NR_fstatfs:          if (OK) OUT(1, a[1], 120); break;
	case __NR_getxattr:
	case __NR_lgetxattr:
	case __NR_fgetxattr:        OUT(2, a[2], RLEN); break;
	case __NR_listxattr:
	case __NR_llistxattr:
	case __NR_flistxattr:       OUT(1, a[1], RLEN); break;
	case __NR_name_to_handle_at: if (OK) { OUT(2, a[2], (user_u32(a[2]) & 0xfff) + 8); OUT(3, a[3], 4); } break;
	case __NR_fcntl:            if (!OK) break;
				    if (a[1] == 5 /* F_GETLK */ || a[1] == 36 /* F_OFD_GETLK */) OUT(2, a[2], 32);
				    if (a[1] == 16 /* F_GETOWN_EX */) OUT(2, a[2], 8);
				    break;

	/* the kernel moves an offset passed by pointer */
	case __NR_sendfile:         if (OK) OUT(2, a[2], 8); break;
	case __NR_copy_file_range:
	case __NR_splice:           if (OK) { OUT(1, a[1], 8); OUT(3, a[3], 8); } break;

	/* timers */
	case __NR_timerfd_gettime:
	case __NR_timer_gettime:
	case __NR_getitimer:        if (OK) OUT(1, a[1], 32); break;
	case __NR_timerfd_settime:
	case __NR_timer_settime:    if (OK) OUT(3, a[3], 32); break;
	case __NR_setitimer:        if (OK) OUT(2, a[2], 32); break;
	case __NR_timer_create:     if (OK) OUT(2, a[2], 4); break;

	/* System V and POSIX IPC */
	case __NR_msgrcv:           if (OK) OUT(1, a[1], RLEN + 8); break;
	case __NR_msgctl:           if (OK && (a[1] & 0xff) == 2 /* IPC_STAT */) OUT(2, a[2], 120); break;
	case __NR_shmctl:           if (OK && (a[1] & 0xff) == 2) OUT(2, a[2], 112); break;
	case __NR_semctl:           if (OK && (a[2] & 0xff) == 2) OUT(3, a[3], 104); break;
	case __NR_mq_timedreceive:  OUT(1, a[1], RLEN); if (OK) OUT(3, a[3], 4); break;
	case __NR_mq_getsetattr:    if (OK) OUT(2, a[2], 64); break;

	/* system info / limits / identity */
	case __NR_uname:            OUT(0, a[0], 390); break;
	case __NR_sysinfo:          OUT(0, a[0], 112); break;
	case __NR_getrusage:        OUT(1, a[1], SZ_RUSAGE); break;
	case __NR_getrlimit:        OUT(1, a[1], 16); break;
	case __NR_prlimit64:        OUT(3, a[3], 16); break;
	case __NR_getresuid:
	case __NR_getresgid:        if (OK) { OUT(0, a[0], 4); OUT(1, a[1], 4); OUT(2, a[2], 4); } break;
	case __NR_getgroups:        OUT(1, a[1], RLEN * 4); break;
	case __NR_capget:           if (OK) OUT(1, a[1], 24); break;
	case __NR_getcpu:           if (OK) { OUT(0, a[0], 4); OUT(1, a[1], 4); } break;
	case __NR_sched_getparam:   if (OK) OUT(1, a[1], 4); break;
	case __NR_sched_getattr:    if (OK) OUT(1, a[1], a[2] & 0xfff); break;
	case __NR_sched_rr_get_interval: if (OK) OUT(1, a[1], SZ_TIMESPEC); break;
	case __NR_rt_sigpending:    if (OK) OUT(0, a[0], a[1] & 0xff); break;
	case __NR_mincore:          if (OK) OUT(2, a[2], (a[1] + 4095) / 4096); break;
	case __NR_get_robust_list:  if (OK) { OUT(1, a[1], 8); OUT(2, a[2], 8); } break;
	case __NR_get_mempolicy:    if (OK) { OUT(0, a[0], 4); OUT(1, a[1], (a[2] + 7) / 8); } break;
	}
	return res;
}

/* ---- tracepoints ---------------------------------------------------------- */

static __always_inline void emit_marker(u32 kind, u32 tid, u32 g)
{
	struct rec_marker *m = bpf_ringbuf_reserve(&events, sizeof(*m), 0);
	if (!m) {
		count_lost();
		return;
	}
	m->h.type = REC_MARKER;
	m->h.flags = 0;
	m->h.len = sizeof(*m);
	m->h.seq = 0;
	m->ts = bpf_ktime_get_ns();
	m->kind = kind;
	m->tid = tid;
	m->tracer = bpf_get_current_pid_tgid() >> 32;
	m->gen = g;
	bpf_ringbuf_submit(m, BPF_RB_FORCE_WAKEUP);    /* rare: wake immediately */
}

/* Called for every ptrace(2) on the machine (by criu, gdb, strace, ...).
 * Only criu's count: a debugger or strace attaching to the target is not a
 * checkpoint, and counting it would mark the target's own syscalls as
 * parasite calls (F_DUMP) and start a generation no photo belongs to. */
static __noinline void handle_ptrace(long req, s32 pid)
{
	struct task_struct *t;
	struct task_state *st;
	u32 kind = 0, tid;
	bool new_gen = false;
	char comm[16];

	if (req != PTRACE_SEIZE && req != PTRACE_ATTACH && req != PTRACE_DETACH)
		return;
	if (!any_tracer) {
		if (bpf_get_current_comm(comm, sizeof(comm)))
			return;
		if (comm[0] != 'c' || comm[1] != 'r' || comm[2] != 'i' || comm[3] != 'u' || comm[4])
			return;
	}
	t = bpf_task_from_pid(pid);
	if (!t)
		return;
	if (!is_target(t->tgid))
		goto out;
	st = bpf_task_storage_get(&tstate, t, 0, BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (!st)
		goto out;
	tid = t->pid;
	if (req == PTRACE_DETACH) {
		if (!st->seized)
			goto out;
		st->seized = 0;
		kind = M_DETACH;
		/* CRIU detaches threads one by one, and gen moves only at the
		 * last detach.  A thread detached earlier already runs and makes
		 * syscalls of the next generation (a restore would make them too):
		 * stamp them with it, not with the old one. */
		st->gen_min = gen + 1;
		if (__sync_sub_and_fetch(&seized_cnt, 1) == 0) {
			__sync_fetch_and_add(&gen, 1);
			new_gen = true;
		}
	} else {
		if (st->seized)
			goto out;
		st->seized = 1;
		kind = M_SEIZE;
		__sync_fetch_and_add(&seized_cnt, 1);
	}
	emit_marker(kind, tid, gen);
	if (new_gen)
		emit_marker(M_GEN, tid, gen);
out:
	bpf_task_release(t);
}

static __always_inline void emit_syscall(struct task_state *st, s64 nr, s64 ret,
					 u64 pid_tgid, u16 extra_flags)
{
	struct bpf_pidns_info ns = {};
	struct rec_syscall *r = bpf_ringbuf_reserve(&events, sizeof(*r), 0);
	if (!r) {
		count_lost();
		return;
	}
	bpf_get_ns_current_pid_tgid(pidns_dev, pidns_ino, &ns, sizeof(ns));
	r->h.type = REC_SYSCALL;
	r->h.flags = st->flags | extra_flags;
	r->h.len = sizeof(*r);
	r->h.seq = st->seq;
	r->ts_enter = st->ts;
	r->ts_exit = bpf_ktime_get_ns();
	r->tgid = pid_tgid >> 32;
	r->tid = (u32)pid_tgid;
	r->ns_tgid = ns.tgid;
	r->ns_tid = ns.pid;
	r->gen = st->gen;
	r->ndata = st->ndata;
	r->nr = nr;
	__builtin_memcpy(r->args, st->args, sizeof(r->args));
	r->ret = ret;
	bpf_ringbuf_submit(r, wake_flags());
}

SEC("tp_btf/sys_enter")
int BPF_PROG(sys_enter, struct pt_regs *regs, long id)
{
	u64 pid_tgid = bpf_get_current_pid_tgid();
	struct task_state *st;
	int r;

	if (id == __NR_ptrace)
		handle_ptrace(regs->di, regs->si);
	if (target_cgid && bpf_get_current_cgroup_id() != target_cgid)
		return 0;
	if (!is_target(pid_tgid >> 32))
		return 0;                       /* every other process stops here */

	st = bpf_task_storage_get(&tstate, bpf_get_current_task_btf(), 0,
				  BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (!st)
		return 0;
	st->seq = __sync_fetch_and_add(&seq_ctr, 1);
	st->ts = bpf_ktime_get_ns();
	st->gen = gen > st->gen_min ? gen : st->gen_min;
	st->flags = st->seized ? F_DUMP : 0;
	st->ndata = 0;
	st->nr = id;
	st->active = 1;
	st->args[0] = regs->di;
	st->args[1] = regs->si;
	st->args[2] = regs->dx;
	st->args[3] = regs->r10;                /* r10, not rcx, for syscalls */
	st->args[4] = regs->r8;
	st->args[5] = regs->r9;

	r = capture_enter(id, st->args, st->seq, (u32)pid_tgid, 0);
	st->flags |= r & 0xff & ~F_FAULT;       /* F_FAULT only if sys_exit fails too */
	st->in_fault = !!(r & F_FAULT);
	st->ndata += r >> 8;

	switch (id) {
	case __NR_execve:                       /* path is gone after a successful exec */
	case __NR_execveat: {
		r = emit_str(id == __NR_execve ? st->args[0] : st->args[1], st->seq,
				 (u32)pid_tgid, META(id == __NR_execve ? 0 : 1, D_STR, 0));
		st->flags |= r & 0xff;
		st->ndata += r >> 8;
		break;
	}
	case __NR_exit:
	case __NR_exit_group:                   /* no sys_exit will follow */
		emit_syscall(st, id, 0, pid_tgid, F_NOEXIT);
		st->active = 0;
		break;
	}
	return 0;
}

SEC("tp_btf/sys_exit")
int BPF_PROG(sys_exit, struct pt_regs *regs, long ret)
{
	u64 pid_tgid = bpf_get_current_pid_tgid();
	struct task_state *st;
	s64 nr = regs->orig_ax;
	int r;

	if (target_cgid && bpf_get_current_cgroup_id() != target_cgid)
		return 0;
	if (!is_target(pid_tgid >> 32))
		return 0;

	st = bpf_task_storage_get(&tstate, bpf_get_current_task_btf(), 0,
				  BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (!st)
		return 0;
	/* at the exit of rt_sigreturn, orig_ax is the restored -1 */
	if (st->active)
		nr = st->nr;
	/* no entry: the recorder attached mid-syscall, or this is a new thread
	 * or process coming out of clone (ret 0) */
	if (!st->active) {
		st->seq = __sync_fetch_and_add(&seq_ctr, 1);
		st->ts = 0;
		st->gen = gen > st->gen_min ? gen : st->gen_min;
		st->flags = F_NOENTER | (st->seized ? F_DUMP : 0);
		st->ndata = 0;
		st->args[0] = regs->di;
		st->args[1] = regs->si;
		st->args[2] = regs->dx;
		st->args[3] = regs->r10;
		st->args[4] = regs->r8;
		st->args[5] = regs->r9;
		/* entry missed: the exit copy of the inputs is all we can get */
		r = capture_enter(nr, st->args, st->seq, (u32)pid_tgid, 0);
		st->flags |= r & 0xff;
		st->ndata += r >> 8;
	} else if (race_check || st->in_fault) {
		r = capture_enter(nr, st->args, st->seq, (u32)pid_tgid, KX);
		st->flags |= r & 0xff;
		st->ndata += r >> 8;
	}
	st->active = 0;

	r = capture_exit(nr, st->args, ret, st->seq, (u32)pid_tgid);
	st->flags |= r & 0xff;
	st->ndata += r >> 8;
	emit_syscall(st, nr, ret, pid_tgid, 0);
	return 0;
}

/* ---- how a thread ends ---------------------------------------------------- */

#define IGNORED_BY_DEFAULT ((1ULL << 17) | (1ULL << 18) | (1ULL << 19) | (1ULL << 20) | \
			    (1ULL << 21) | (1ULL << 22) | (1ULL << 23) | (1ULL << 28))
			    /* SIGCHLD, CONT, STOP, TSTP, TTIN, TTOU, URG, WINCH */

static __always_inline u32 current_gen(void)
{
	struct task_state *st = bpf_task_storage_get(&tstate, bpf_get_current_task_btf(), 0, 0);
	return st && st->gen_min > gen ? st->gen_min : gen;
}

static __always_inline void emit_signal(s32 sig, s32 code, u32 fatal, u64 addr, u64 handler)
{
	u64 pid_tgid = bpf_get_current_pid_tgid();
	struct bpf_pidns_info ns = {};
	struct task_struct *t = bpf_get_current_task_btf();
	struct pt_regs *regs = (struct pt_regs *)bpf_task_pt_regs(t);
	struct rec_signal *r = bpf_ringbuf_reserve(&events, sizeof(*r), 0);

	if (!r) {
		count_lost();
		return;
	}
	bpf_get_ns_current_pid_tgid(pidns_dev, pidns_ino, &ns, sizeof(ns));
	r->h.type = REC_SIGNAL;
	r->h.flags = 0;
	r->h.len = sizeof(*r);
	r->h.seq = seq_ctr;
	r->ts = bpf_ktime_get_ns();
	r->tid = (u32)pid_tgid;
	r->ns_tid = ns.pid;
	r->gen = current_gen();
	r->sig = sig;
	r->code = code;
	r->fatal = fatal;
	r->addr = addr;
	r->rip = regs->ip;
	r->rsp = regs->sp;
	r->handler = handler;
	bpf_ringbuf_submit(r, BPF_RB_FORCE_WAKEUP);
}

SEC("tp_btf/signal_deliver")
int BPF_PROG(on_signal, int sig, struct kernel_siginfo *info, struct k_sigaction *ka)
{
	struct task_state *st;
	u64 handler = (u64)ka->sa.sa_handler, addr = 0;
	int code = 0;

	if (!is_target(bpf_get_current_pid_tgid() >> 32))
		return 0;
	/* CRIU's own stops and traps are not the program's signals */
	st = bpf_task_storage_get(&tstate, bpf_get_current_task_btf(), 0, 0);
	if (st && st->seized)
		return 0;
	/* the verifier does not type this tracepoint's siginfo as a pointer */
	bpf_core_read(&code, sizeof(code), &info->si_code);
	bpf_core_read(&addr, sizeof(addr), &info->_sifields._sigfault._addr);
	emit_signal(sig, code,
		    handler == 0 && sig > 0 && sig < 64 && !((IGNORED_BY_DEFAULT >> sig) & 1),
		    addr, handler);
	return 0;
}

SEC("tp_btf/sched_process_exit")
int BPF_PROG(on_exit, struct task_struct *p)
{
	/* only the last word of a process that ended abnormally, or its leader */
	if (!is_target(p->tgid) || (u32)bpf_get_current_pid_tgid() != p->pid)
		return 0;
	if (p->pid != p->tgid && !(p->exit_code & 0x7f))
		return 0;
	emit_signal(SIG_EXIT, p->exit_code, !!(p->exit_code & 0x7f), 0, 0);
	return 0;
}

/* follow mode: children of a target become targets */
SEC("tp_btf/sched_process_fork")
int BPF_PROG(on_fork, struct task_struct *parent, struct task_struct *child)
{
	u32 ptgid = parent->tgid, ctgid = child->tgid;
	u8 one = 1;

	if (!follow || ptgid == ctgid)
		return 0;
	if (bpf_map_lookup_elem(&targets, &ptgid))
		bpf_map_update_elem(&targets, &ctgid, &one, BPF_ANY);
	return 0;
}
