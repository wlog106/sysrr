/* sysrr_replay: drive a restored process through a recorded generation.
 *
 *   sudo ./sysrr_replay -p PID [-g GEN] [-e stop|live|kill] [-S] [-c] [-v]
 *                       [-N] [-w FILE] rec.log
 *
 *     -p PID   host pid of the restored process; restore it stopped
 *              (CRIU "leave-stopped", see DESIGN.md "Replay")
 *     -g GEN   generation to replay (default: the last one in the log)
 *     -e WHAT  at the end of the recording or at a divergence:
 *                stop  SIGSTOP the target and wait for Ctrl-C (default),
 *                live  let it continue with real syscalls,
 *                kill  SIGKILL it and exit (for scripts)
 *     -S       strict order: release syscalls of different threads in the
 *              recorded (seq) order, not just per thread
 *     -T       serial order: one thread runs user code at a time, released
 *              in the order the recorded syscalls returned (ts_exit); orders
 *              the user-space work between syscalls too (shared memory
 *              races across threads/processes).  Implies -S's purpose.
 *     -c       continue after an input/argument mismatch (only warn)
 *     -v       print every replayed syscall
 *     -w FILE  save what the target writes to fd 1 and 2 (those writes are
 *              faked, so nothing reaches the container's real stdout)
 *     -N       no fd installation: faked open/socket/pipe... return the
 *              recorded number but create nothing (old behaviour, for
 *              comparison)
 *
 * How it works, in three steps:
 *
 * 1. attach_listener(): the restored process is stopped.  Seize it with
 *    ptrace for a moment and make it execute, as if it had called them
 *    itself:  mmap (scratch page), prctl(NO_NEW_PRIVS), seccomp(filter,
 *    NEW_LISTENER), munmap, close.  The filter sends every syscall of every
 *    thread to a "listener" fd, which we copy out with pidfd_getfd().  Then
 *    restore registers and code, detach.  No ptrace from here on.
 *
 * 2. load(): read the log, keep the REC_SYSCALLs of generation GEN that were
 *    not made by CRIU's parasite (F_DUMP), attach their REC_DATAs, and build
 *    one queue per thread (ns_tid), ordered by seq.
 *
 * 3. supervise(): for each notification (thread T is entering syscall nr
 *    with args), take T's next recorded syscall and:
 *      - nr differs                         -> divergence
 *      - compare D_IN/D_STR with T's memory -> divergence if different
 *      - process-internal syscall (mmap, futex, clone, signals, exit...)
 *                                           -> let the kernel run it
 *      - anything else                      -> do not run it: write the
 *        recorded D_OUT bytes into T's memory and return the recorded ret
 *      - if that faked call created fds (open, socket, pipe, dup...), put a
 *        real fd at each recorded number with SECCOMP_IOCTL_NOTIF_ADDFD:
 *        the same file from the container's root, or an object of the same
 *        kind, so later real syscalls on it (mmap, close, fchdir) work.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/openat2.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <dirent.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include "sysrr.h"

static const char *names[512] = {
#include "syscall_names.h"
};

static const char *sysname(long nr)
{
	static char buf[24];
	if (nr >= 0 && nr < 512 && names[nr])
		return names[nr];
	snprintf(buf, sizeof(buf), "syscall_%ld", nr);
	return buf;
}

static int opt_verbose, opt_strict, opt_cont, opt_nofd, opt_serial;
static enum { END_STOP, END_LIVE, END_KILL } opt_end = END_STOP;
static FILE *wfile;

#define die(...) do { fprintf(stderr, "sysrr_replay: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)

/* ======================================================================
 * 1. Give the restored process a seccomp listener (ptrace, briefly)
 * ====================================================================== */

#ifndef SECCOMP_FILTER_FLAG_TSYNC_ESRCH
#define SECCOMP_FILTER_FLAG_TSYNC_ESRCH (1UL << 4)
#endif

static long xptrace(int req, pid_t pid, void *addr, void *data)
{
	long r;
	errno = 0;
	r = ptrace(req, pid, addr, data);
	if (r == -1 && errno && req != PTRACE_PEEKDATA)
		die("ptrace(%d, %d): %s", req, pid, strerror(errno));
	return r;
}

/* Answer "go ahead" to one notification: our own filter traps the syscalls we
 * inject after seccomp() returned. */
static void continue_one(int lfd)
{
	struct seccomp_notif req = {};
	struct seccomp_notif_resp resp = {};
	struct pollfd p = { .fd = lfd, .events = POLLIN };

	if (poll(&p, 1, 5000) != 1)
		die("injected syscall did not reach the listener");
	if (ioctl(lfd, SECCOMP_IOCTL_NOTIF_RECV, &req))
		die("NOTIF_RECV: %s", strerror(errno));
	resp.id = req.id;
	resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
	if (ioctl(lfd, SECCOMP_IOCTL_NOTIF_SEND, &resp))
		die("NOTIF_SEND: %s", strerror(errno));
}

/* Make the tracee execute one syscall.  Its rip points at "syscall; int3"
 * that we wrote over its code; the int3 brings it back to us. */
static long inject(pid_t pid, const struct user_regs_struct *base, long nr,
		   long a0, long a1, long a2, long a3, long a4, int lfd)
{
	struct user_regs_struct r = *base;
	int st;

	r.rax = nr;
	r.orig_rax = -1;        /* not inside a syscall: no restart logic on resume */
	r.rdi = a0; r.rsi = a1; r.rdx = a2; r.r10 = a3; r.r8 = a4; r.r9 = 0;
	xptrace(PTRACE_SETREGS, pid, 0, &r);
	xptrace(PTRACE_CONT, pid, 0, 0);
	if (lfd >= 0)
		continue_one(lfd);
	for (;;) {
		if (waitpid(pid, &st, __WALL) < 0)
			die("waitpid: %s", strerror(errno));
		if (!WIFSTOPPED(st))
			die("target died during injection (status %#x)", st);
		if (WSTOPSIG(st) == SIGTRAP && !(st >> 16))
			break;                  /* our int3 */
		xptrace(PTRACE_CONT, pid, 0, 0);   /* group-stop re-report etc. */
	}
	xptrace(PTRACE_GETREGS, pid, 0, &r);
	return r.rax;
}

static int attach_listener(pid_t pid)
{
	/* Every syscall of the native ABI -> USER_NOTIF.  Anything else
	 * (32-bit / x32 calls) is allowed through untouched. */
	struct sock_filter f[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
		BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
	};
	struct user_regs_struct saved;
	long word, code, page, fd;
	int st, pidfd, lfd;

	xptrace(PTRACE_SEIZE, pid, 0, 0);
	xptrace(PTRACE_INTERRUPT, pid, 0, 0);
	if (waitpid(pid, &st, __WALL) < 0 || !WIFSTOPPED(st))
		die("could not stop %d", pid);
	xptrace(PTRACE_GETREGS, pid, 0, &saved);

	/* overwrite 3 bytes at rip with: 0f 05 (syscall)  cc (int3) */
	errno = 0;
	word = ptrace(PTRACE_PEEKDATA, pid, (void *)saved.rip, 0);
	if (errno)
		die("peek at rip %#llx: %s", saved.rip, strerror(errno));
	code = (word & ~0xffffffL) | 0xcc050fL;
	xptrace(PTRACE_POKEDATA, pid, (void *)saved.rip, (void *)code);

	page = inject(pid, &saved, SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, -1);
	if (page < 0 && page > -4096)
		die("injected mmap failed: %s", strerror(-page));

	/* the filter program, in the target's memory: sock_fprog then insns */
	struct sock_fprog prog = { .len = sizeof(f) / sizeof(f[0]),
				   .filter = (struct sock_filter *)(page + 16) };
	struct iovec l[2] = { { &prog, sizeof(prog) }, { f, sizeof(f) } };
	struct iovec rm[2] = { { (void *)page, sizeof(prog) }, { (void *)(page + 16), sizeof(f) } };
	if (process_vm_writev(pid, l, 2, rm, 2, 0) != (ssize_t)(sizeof(prog) + sizeof(f)))
		die("writing the filter: %s", strerror(errno));

	/* without CAP_SYS_ADMIN a process must promise no_new_privs first */
	long r = inject(pid, &saved, SYS_prctl, PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0, -1);
	if (r)
		die("injected prctl(NO_NEW_PRIVS) failed: %s", strerror(-r));
	fd = inject(pid, &saved, SYS_seccomp, SECCOMP_SET_MODE_FILTER,
		    SECCOMP_FILTER_FLAG_NEW_LISTENER | SECCOMP_FILTER_FLAG_TSYNC |
		    SECCOMP_FILTER_FLAG_TSYNC_ESRCH, page, 0, 0, -1);
	if (fd < 0)
		die("injected seccomp() failed: %s", strerror(-fd));

	/* the listener fd lives in the target's fd table: copy it to ours */
	pidfd = syscall(SYS_pidfd_open, pid, 0);
	lfd = pidfd < 0 ? -1 : syscall(SYS_pidfd_getfd, pidfd, fd, 0);
	if (lfd < 0)
		die("pidfd_getfd: %s", strerror(errno));
	close(pidfd);

	/* clean up inside the target; from now on its syscalls come to us */
	inject(pid, &saved, SYS_munmap, page, 4096, 0, 0, 0, lfd);
	inject(pid, &saved, SYS_close, fd, 0, 0, 0, 0, lfd);

	xptrace(PTRACE_POKEDATA, pid, (void *)saved.rip, (void *)word);
	xptrace(PTRACE_SETREGS, pid, 0, &saved);
	xptrace(PTRACE_DETACH, pid, 0, 0);   /* back to its stopped state */
	return lfd;
}

/* ======================================================================
 * 2. Load one generation of the log
 * ====================================================================== */

struct ent {                        /* one recorded syscall */
	const struct rec_syscall *s;
	const struct rec_data **d;
	unsigned nd, cap;
	unsigned done;
	u64 key;                    /* -T: its place in the serial order */
	u64 enter;                  /* -T: ts_enter, made to agree with seq */
};

struct thr {                        /* one recorded thread */
	u32 ns_tid;
	struct ent **q;
	size_t n, pos, cap;
	long held_id;               /* notification we are not answering (0 = none) */
	int held_lfd;               /* ... and the listener it came from */
	int held_nr;
	const struct ent *pinned;   /* its mmap was re-issued at the recorded address */
	int running;                /* -T: answered, runs user code until its next syscall */
	int seen;                   /* it has made a syscall during this replay */
	int past_end;               /* asked for a syscall after its last record */
	int lock_wait;              /* -T: sleeps in a real futex wait the recording lacks */
	u64 wait_addr;              /* ... on this futex */
	pid_t start_tid;            /* -T: held in a ptrace stop until its turn to start */
	u64 start_key;              /* ... which is when it made its first syscall */
};

static struct ent *ents;            /* sorted by seq */
static size_t nents;
static struct thr *thrs;
static size_t nthrs;
static size_t remaining;            /* records not consumed yet */
static size_t gnext;                /* strict mode: index of the next ent in seq order */
static struct ent **byexit;         /* -T: all ents in serial order (order_serial) */
static size_t xnext;                /* -T: index of the next one */
static unsigned long n_skip;        /* signal-interrupted records skipped */
static unsigned long n_child;       /* no entry seen: new threads, calls refused by the container */
static int is_clone(long nr)
{
	return nr == SYS_clone || nr == SYS_clone3 || nr == SYS_fork || nr == SYS_vfork;
}

static int cmp_ent(const void *a, const void *b)
{
	u64 x = ((const struct ent *)a)->s->h.seq, y = ((const struct ent *)b)->s->h.seq;
	return x < y ? -1 : x > y;
}

static struct ent *find_ent(u64 seq)
{
	size_t lo = 0, hi = nents;
	while (lo < hi) {
		size_t m = (lo + hi) / 2;
		if (ents[m].s->h.seq < seq)
			lo = m + 1;
		else
			hi = m;
	}
	return lo < nents && ents[lo].s->h.seq == seq ? &ents[lo] : NULL;
}

static struct thr *thr_of(u32 ns_tid)
{
	for (size_t i = 0; i < nthrs; i++)
		if (thrs[i].ns_tid == ns_tid)
			return &thrs[i];
	return NULL;
}

static int is_restart_ret(s64 r) { return r <= -512 && r >= -516; }

static const struct sysrr_file_hdr *fh;

/* the live run's signals in this generation */
static const struct rec_signal **sigs;
static size_t nsigs;

static void load(const char *path, int *gen)
{
	int fd = open(path, O_RDONLY);
	struct stat sb;
	const char *p, *end, *base;
	size_t cap = 0;
	int maxgen = 0, flagged = 0;

	if (fd < 0 || fstat(fd, &sb))
		die("%s: %s", path, strerror(errno));
	base = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (base == MAP_FAILED)
		die("mmap %s: %s", path, strerror(errno));
	fh = (const void *)base;
	if (sb.st_size < (off_t)sizeof(*fh) || fh->magic != SYSRR_MAGIC)
		die("%s: not a sysrr log", path);
	if (fh->version != SYSRR_VERSION)
		die("%s: log format version %u, this tool reads %u", path, fh->version, SYSRR_VERSION);
	end = base + sb.st_size;

	/* pass 1: generation number, then the syscalls of that generation */
	for (int pass = 0; pass < 2; pass++) {
		if (pass == 1 && *gen < 0)
			*gen = maxgen;
		for (p = base + fh->hdr_size; p + sizeof(struct rec_hdr) <= end; ) {
			const struct rec_hdr *h = (const void *)p;
			if (h->len < sizeof(*h) || p + h->len > end)
				break;
			p += h->len;
			if (pass == 1 && h->type == REC_SIGNAL && h->len >= sizeof(struct rec_signal) &&
			    (int)((const struct rec_signal *)h)->gen == *gen) {
				sigs = realloc(sigs, (nsigs + 1) * sizeof(*sigs));
				sigs[nsigs++] = (const void *)h;
			}
			if (h->type != REC_SYSCALL)
				continue;
			const struct rec_syscall *s = (const void *)h;
			if (pass == 0) {
				if (!(h->flags & F_DUMP) && (int)s->gen > maxgen)
					maxgen = s->gen;
				continue;
			}
			if ((int)s->gen != *gen || (h->flags & F_DUMP))
				continue;
			if (nents == cap)
				ents = realloc(ents, (cap = cap ? cap * 2 : 1024) * sizeof(*ents));
			ents[nents++] = (struct ent){ .s = s };
			if (h->flags & (F_TRUNC | F_FAULT | F_LOST))
				flagged++;
		}
	}
	if (nents == 0)
		return;
	qsort(ents, nents, sizeof(*ents), cmp_ent);

	/* pass 2: attach the data records */
	for (p = base + fh->hdr_size; p + sizeof(struct rec_hdr) <= end; ) {
		const struct rec_hdr *h = (const void *)p;
		if (h->len < sizeof(*h) || p + h->len > end)
			break;
		p += h->len;
		if (h->type != REC_DATA)
			continue;
		struct ent *e = find_ent(h->seq);
		if (!e)
			continue;
		if (e->nd == e->cap)
			e->d = realloc(e->d, (e->cap = e->cap ? e->cap * 2 : 4) * sizeof(*e->d));
		e->d[e->nd++] = (const void *)h;
	}

	/* per-thread queues */
	for (size_t i = 0; i < nents; i++) {
		struct thr *t = thr_of(ents[i].s->ns_tid);
		if (!t) {
			thrs = realloc(thrs, (nthrs + 1) * sizeof(*thrs));
			t = &thrs[nthrs++];
			*t = (struct thr){ .ns_tid = ents[i].s->ns_tid };
		}
		if (t->n == t->cap)
			t->q = realloc(t->q, (t->cap = t->cap ? t->cap * 2 : 256) * sizeof(*t->q));
		t->q[t->n++] = &ents[i];
		/* interrupted by a signal: the live process ran it again (next
		 * record); the replay delivers no signal, so skip this one */
		if (is_restart_ret(ents[i].s->ret))
			ents[i].done = 1, n_skip++;
		/* The recorder saw this call's exit but not its entry, so the replay
		 * will not see it either:
		 *  - a new thread's first record is its parent's clone returning 0 in
		 *    the child, which is born that way and never enters the syscall;
		 *  - a call the container's own seccomp profile refuses (EPERM) never
		 *    gets to the entry tracepoint, and that filter beats our listener:
		 *    the replayed call gets the same answer without reaching us. */
		else if (ents[i].s->h.flags & F_NOENTER)
			ents[i].done = 1, n_child++;
		else
			remaining++;
	}
	if (flagged)
		fprintf(stderr, "warning: %d syscalls of this generation have "
			"incomplete data (F_TRUNC/F_FAULT/F_LOST)\n", flagged);
}

/* ======================================================================
 * 3. Supervise
 * ====================================================================== */

/* Process-internal syscalls: they change the process itself (memory map,
 * signal state, threads), not what it learns from the outside.  They are
 * run for real; faking them would leave the process without the mapping /
 * thread / handler it expects. */
static int passthrough(long nr)
{
	switch (nr) {
	case SYS_brk: case SYS_mmap: case SYS_munmap: case SYS_mprotect:
	case SYS_mremap: case SYS_madvise: case SYS_mlock: case SYS_munlock:
	case SYS_mlockall: case SYS_munlockall: case SYS_msync:
	case SYS_rt_sigaction: case SYS_rt_sigprocmask: case SYS_rt_sigreturn:
	case SYS_sigaltstack: case SYS_rt_sigsuspend:
	case SYS_clone: case SYS_clone3: case SYS_fork: case SYS_vfork:
	case SYS_execve: case SYS_execveat: case SYS_exit: case SYS_exit_group:
	case SYS_set_tid_address: case SYS_set_robust_list: case SYS_rseq:
	case SYS_arch_prctl: case SYS_prctl: case SYS_futex: case SYS_sched_yield:
	case SYS_getpid: case SYS_gettid: case SYS_kill: case SYS_tgkill:
	case SYS_tkill: case SYS_rt_sigqueueinfo: case SYS_rt_tgsigqueueinfo:
	case SYS_membarrier: case SYS_restart_syscall:
	case SYS_chdir: case SYS_fchdir:
		return 1;
	/* The fd table: faked fd-creating calls install real fds at the
	 * recorded numbers, so closing them for real keeps the table equal to
	 * the recorded one (with -N nothing was installed: fake the close). */
	case SYS_close: case SYS_close_range:
		return !opt_nofd;
	}
	return 0;
}

/* number of meaningful args, for the argument check (0 = do not check) */
/* A file or memfd created after the checkpoint is a memfd here, and calls that
 * change it run for real: its size and contents follow the recording, so a real
 * mmap of it sees what the live process saw (even a SIGBUS after a truncate). */
#define STANDIN_NAME "sysrr-replay"
static unsigned long n_standin;

static int is_standin(pid_t pid, u64 fd)
{
	char p[64], l[128];
	snprintf(p, sizeof(p), "/proc/%d/fd/%llu", pid, (unsigned long long)fd);
	ssize_t n = readlink(p, l, sizeof(l) - 1);
	if (n <= 0)
		return 0;
	l[n] = 0;
	return !strncmp(l, "/memfd:" STANDIN_NAME, sizeof("/memfd:" STANDIN_NAME) - 1);
}

static int futex_waits(u64 op)
{
	op &= 0x7f;
	return op == 0 /* WAIT */ || op == 9 /* WAIT_BITSET */;
}

/* is signal sig waiting for this thread (sent to it or to its process)? */
static int sig_pending(pid_t tid, int sig)
{
	char path[64], line[256];
	int yes = 0;

	if (sig < 1 || sig > 64)
		return 0;
	snprintf(path, sizeof(path), "/proc/%d/status", tid);
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, "SigPnd:", 7) || !strncmp(line, "ShdPnd:", 7))
			yes |= (int)(strtoull(line + 7, NULL, 16) >> (sig - 1)) & 1;
	fclose(f);
	return yes;
}

static int fd_is(pid_t pid, u64 fd, const char *what)
{
	char p[64], l[128];
	snprintf(p, sizeof(p), "/proc/%d/fd/%llu", pid, (unsigned long long)fd);
	ssize_t n = readlink(p, l, sizeof(l) - 1);
	if (n <= 0)
		return 0;
	l[n] = 0;
	return !strcmp(l, what);
}

/* sigwaitinfo, sigtimedwait, or a read from a signalfd, whose recorded signal is
 * pending in the replay as well */
static int takes_pending_signal(const struct seccomp_notif *req, const struct ent *e)
{
	if (req->data.nr == SYS_rt_sigtimedwait)
		return sig_pending(req->pid, e->s->ret);
	if (req->data.nr == SYS_read && e->nd && e->d[0]->kind == D_OUT && e->d[0]->len >= 4 &&
	    fd_is(req->pid, req->data.args[0], "anon_inode:[signalfd]")) {
		u32 signo;
		memcpy(&signo, e->d[0]->data, 4);
		return sig_pending(req->pid, signo);
	}
	return 0;
}

/* run this call for real rather than answer from the log? */
static int runs_real(const struct seccomp_notif *req, const struct ent *e)
{
	long nr = req->data.nr;
	s64 ret = e->s->ret;

	/* -T: a recorded wait is not entered for real.  It is held until the moment
	 * it returned in the recording and then answered, so the woken thread runs
	 * its next stretch of user code in turn like every other (a real wait
	 * returns when the kernel likes, and all the threads woken together would
	 * race for the lock again).  Returning without a real wake is allowed: every
	 * futex user checks the word again. */
	if (opt_serial && nr == SYS_futex && futex_waits(req->data.args[1]))
		return 0;
	/* A signal this process sent to itself is really pending (kill and sigqueue
	 * run for real): the call that takes it must run too, or it stays pending. */
	if (ret > 0 && takes_pending_signal(req, e))
		return 1;
	if (passthrough(nr)) {
		/* It failed live: fail it here too.  A real attempt might succeed: the
		 * limit that made mmap fail was set by a setrlimit that was faked. */
		if (ret < 0 && ret > -4096 && nr != SYS_futex && nr != SYS_rt_sigreturn &&
		    nr != SYS_rt_sigsuspend && nr != SYS_restart_syscall)
			return 0;
		/* brk reports failure by returning the old break */
		if (nr == SYS_brk && e->s->args[0] && (u64)ret != e->s->args[0])
			return 0;
		return 1;
	}
	if (!n_standin)
		return 0;
	switch (nr) {
	case SYS_write: case SYS_pwrite64: case SYS_writev: case SYS_ftruncate:
	case SYS_lseek: case SYS_fallocate:
		return is_standin(req->pid, req->data.args[0]);
	}
	return 0;
}

static int nargs(long nr)
{
	switch (nr) {
	case SYS_close: case SYS_exit_group: case SYS_dup: return 1;
	case SYS_fstat: case SYS_stat: case SYS_lstat: case SYS_access:
	case SYS_clock_gettime: case SYS_clock_getres: case SYS_nanosleep:
	case SYS_dup2: case SYS_listen: case SYS_shutdown: case SYS_gettimeofday:
		return 2;
	case SYS_read: case SYS_write: case SYS_open: case SYS_lseek:
	case SYS_ioctl: case SYS_readv: case SYS_writev: case SYS_poll:
	case SYS_socket: case SYS_connect: case SYS_bind: case SYS_accept:
	case SYS_sendmsg: case SYS_recvmsg: case SYS_fcntl: case SYS_getdents64:
	case SYS_getrandom: case SYS_dup3:
		return 3;
	case SYS_pread64: case SYS_pwrite64: case SYS_openat: case SYS_newfstatat:
	case SYS_clock_nanosleep: case SYS_epoll_wait: case SYS_accept4:
	case SYS_epoll_ctl:
		return 4;
	case SYS_statx: case SYS_select: return 5;
	case SYS_sendto: case SYS_recvfrom: case SYS_pselect6: return 6;
	}
	return 0;
}

static int is_iov_call(long nr)
{
	return nr == SYS_readv || nr == SYS_preadv || nr == SYS_preadv2 ||
	       nr == SYS_writev || nr == SYS_pwritev || nr == SYS_pwritev2;
}

static int is_msg_call(long nr) { return nr == SYS_recvmsg || nr == SYS_sendmsg; }
#define SZ_MMSGHDR 64

static int mem_read(pid_t pid, u64 addr, void *buf, size_t n)
{
	struct iovec l = { buf, n }, r = { (void *)addr, n };
	return process_vm_readv(pid, &l, 1, &r, 1, 0) == (ssize_t)n ? 0 : -1;
}

static int mem_write(pid_t pid, u64 addr, const void *buf, size_t n)
{
	struct iovec l = { (void *)buf, n }, r = { (void *)addr, n };
	return process_vm_writev(pid, &l, 1, &r, 1, 0) == (ssize_t)n ? 0 : -1;
}

/* Where does this piece of recorded memory live in the replayed thread?
 * The address space is the restored one, so the syscall's own arguments
 * (and the iovec / msghdr they point to) give the location.  We read them
 * from the target *now*, before writing anything. */
static int resolve(pid_t pid, long nr, const u64 *a, const struct rec_data *d, u64 *addr)
{
	/* recvmmsg: pieces of message i are tagged arg = 8 + i */
	int mmsg = nr == SYS_recvmmsg && d->arg >= 8;
	u64 mp = mmsg ? a[1] + (d->arg - 8) * SZ_MMSGHDR : a[1];

	if (mmsg && d->idx == 0xfffe) {
		*addr = mp + d->offset;
		return 0;
	}
	if ((is_msg_call(nr) || mmsg) && d->idx != 0xfffe) {
		struct msghdr m;
		if (mem_read(pid, mp, &m, sizeof(m)))
			return -1;
		if (d->idx == 0xfffd)
			*addr = (u64)m.msg_name;
		else if (d->idx == 0xfffc)
			*addr = (u64)m.msg_control;
		else if (d->idx == 0xffff)
			*addr = (u64)m.msg_iov;
		else {
			struct iovec v;
			if (mem_read(pid, (u64)m.msg_iov + d->idx * sizeof(v), &v, sizeof(v)))
				return -1;
			*addr = (u64)v.iov_base;
		}
	} else if (is_iov_call(nr) && d->idx < 0xfff0) {
		struct iovec v;
		if (mem_read(pid, a[1] + d->idx * sizeof(v), &v, sizeof(v)))
			return -1;
		*addr = (u64)v.iov_base;
	} else {
		*addr = a[d->arg];          /* plain buffer, or msghdr itself */
	}
	*addr += d->offset;
	return 0;
}

/* --- reporting --------------------------------------------------------- */

static void fmt_call(char *b, size_t n, long nr, const u64 *a, int na)
{
	int k = snprintf(b, n, "%s(", sysname(nr));
	if (na == 0)
		na = 3;                     /* unknown: show the first three */
	for (int i = 0; i < na && k < (int)n; i++)
		k += snprintf(b + k, n - k, "%s%#llx", i ? ", " : "", (unsigned long long)a[i]);
	snprintf(b + k, n > (size_t)k ? n - k : 0, ")");
}

static void preview(const char *label, const unsigned char *p, size_t n)
{
	printf("       %-9s \"", label);
	for (size_t i = 0; i < n && i < 60; i++)
		putchar(p[i] >= 32 && p[i] < 127 && p[i] != '"' ? p[i] : '.');
	printf("\"%s\n", n > 60 ? "..." : "");
}

static void history(struct thr *t)
{
	size_t from = t->pos > 5 ? t->pos - 5 : 0;
	char b[160];
	printf("     last replayed syscalls of this thread:\n");
	if (from == t->pos)
		printf("       (none)\n");
	for (size_t i = from; i < t->pos; i++) {
		const struct rec_syscall *s = t->q[i]->s;
		fmt_call(b, sizeof(b), s->nr, s->args, nargs(s->nr));
		printf("       #%llu %s = %lld\n", (unsigned long long)s->h.seq, b, (long long)s->ret);
	}
}

/* --- state ------------------------------------------------------------- */

static int lfd;                     /* listener of the notification being handled */
static pid_t tgid;

/* The restored process tree.  A seccomp filter belongs to the process that
 * installs it (TSYNC: all its threads) and to children it forks later.
 * Processes that already exist (nginx's workers) each get their own filter
 * and listener; a process forked during the replay shares its parent's. */
struct proc { pid_t tgid; int lfd, pidfd; };   /* lfd < 0: shares its parent's, or hung up */
static struct proc procs[256];
static int nprocs;

static pid_t tgid_of(pid_t tid)
{
	char path[64], line[128];
	pid_t g = tid;
	snprintf(path, sizeof(path), "/proc/%d/status", tid);
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f))
		if (!strncmp(line, "Tgid:", 5))
			g = atoi(line + 5);
	if (f)
		fclose(f);
	return g;
}

/* pidfd of the process a thread belongs to (for pidfd_getfd) */
static int pidfd_for(pid_t tid)
{
	pid_t g = tgid_of(tid);
	for (int i = 0; i < nprocs; i++)
		if (procs[i].tgid == g)
			return procs[i].pidfd;
	int slot = nprocs;
	if (nprocs == 256) {
		/* a program that keeps forking: reuse the slot of a dead child */
		for (slot = 0; slot < 256; slot++)
			if (procs[slot].lfd < 0 &&
			    syscall(SYS_pidfd_send_signal, procs[slot].pidfd, 0, NULL, 0)) {
				close(procs[slot].pidfd);
				break;
			}
		if (slot == 256)
			return -1;
	} else {
		nprocs++;
	}
	procs[slot] = (struct proc){ .tgid = g, .lfd = -1,
				     .pidfd = syscall(SYS_pidfd_open, g, 0) };
	return procs[slot].pidfd;
}

/* every process of the tree: send sig (stop / continue / kill).  Through the
 * pidfd: a child that has exited can't turn the signal into a shot at whichever
 * host process got its pid since. */
static void kill_all(int sig)
{
	for (int i = 0; i < nprocs; i++)
		if (procs[i].pidfd >= 0)
			syscall(SYS_pidfd_send_signal, procs[i].pidfd, sig, NULL, 0);
}
static enum { RUN, ENDED, DIVERGED, LIVE } state = RUN;
static int relax_order;
static struct thr *jump_thr;        /* -T: this thread may go ahead of its turn (lock_holder_first) */
static unsigned long n_jump, n_ptrdiff;
static unsigned long n_relaxed;
static unsigned long n_fake, n_pass, n_argwarn, n_inwarn, in_bytes, out_bytes;
static unsigned long n_fds, n_fdwarn, n_pin, n_pidpin, n_slack, n_sig;
static int troot = -1;              /* the container's root directory */
static volatile sig_atomic_t interrupted;

/* host tid -> container tid (NSpid's last field) */
static u32 ns_tid_of(pid_t tid)
{
	static struct { pid_t h; u32 ns; } cache[256];
	static int nc;
	char path[64], line[256];
	u32 ns = 0;

	for (int i = 0; i < nc && i < 256; i++)
		if (cache[i].h == tid)
			return cache[i].ns;
	snprintf(path, sizeof(path), "/proc/%d/status", tid);
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, "NSpid:", 6)) {
			char *s = line + 6, *tok, *last = NULL;
			while ((tok = strtok_r(s, " \t\n", &s)))
				last = tok;
			ns = last ? strtoul(last, NULL, 10) : 0;
		}
	fclose(f);
	/* round robin: a program with thousands of short threads keeps its
	 * current ones in the cache */
	cache[nc % 256].h = tid, cache[nc % 256].ns = ns;
	nc++;
	return ns;
}

/* A new thread or process should get the id it had in the recording: then
 * fork()'s real return value, getpid() and the recorded wait4() all agree.
 * set_next_pid() arranges that; this table is the fallback for when it could
 * not (another process took the number): recorded clone() results are queued
 * and given to unknown new threads in order. */
static u32 *pending_child;
static int npending, cpending;
static struct { u32 actual, recorded; } *alias;
static int nalias, calias;

static struct thr *thread_for(u32 ns)
{
	for (int i = 0; i < nalias; i++)
		if (alias[i].actual == ns)
			return thr_of(alias[i].recorded);
	struct thr *t = thr_of(ns);
	if (t) {
		for (int i = 0; i < npending; i++)      /* it got its recorded id */
			if (pending_child[i] == ns) {
				memmove(pending_child + i, pending_child + i + 1,
					(--npending - i) * sizeof(u32));
				break;
			}
		return t;
	}
	if (npending) {
		if (nalias == calias)
			alias = realloc(alias, (calias = calias ? calias * 2 : 64) * sizeof(*alias));
		alias[nalias].actual = ns;
		alias[nalias++].recorded = pending_child[0];
		memmove(pending_child, pending_child + 1, --npending * sizeof(u32));
		printf("   warning: new thread got id %u, in the recording it had %u: "
		       "the program may see the difference\n", ns, alias[nalias - 1].recorded);
		return thr_of(alias[nalias - 1].recorded);
	}
	return NULL;
}

static void expect_child(u32 recorded)
{
	if (npending == cpending)
		pending_child = realloc(pending_child, (cpending = cpending ? cpending * 2 : 64) * sizeof(u32));
	pending_child[npending++] = recorded;
}

/* Make the next pid in the target's namespace be `want`.  /proc/sys/kernel/
 * ns_last_pid acts on the writer's *active* pid namespace, which setns() does
 * not change for the caller, only for its children.  So: a helper joins the
 * namespace, its child (now inside) writes the number and exits, freeing the
 * pid it used. */
static int pidns_fd = -1;

static void set_next_pid(int want)
{
	char v[16];
	int st = 0, n = snprintf(v, sizeof(v), "%d", want - 1);
	pid_t h;

	if (pidns_fd < 0 || want <= 1)
		return;
	fflush(stdout);
	h = fork();
	if (h == 0) {
		if (setns(pidns_fd, CLONE_NEWPID))
			_exit(1);
		pid_t g = fork();
		if (g == 0) {
			int fd = open("/proc/sys/kernel/ns_last_pid", O_WRONLY);
			_exit(fd < 0 || write(fd, v, n) != n);
		}
		if (g < 0 || waitpid(g, &st, 0) < 0)
			_exit(1);
		_exit(WIFEXITED(st) ? WEXITSTATUS(st) : 1);
	}
	if (h > 0 && waitpid(h, &st, 0) == h && WIFEXITED(st) && WEXITSTATUS(st) == 0)
		n_pidpin++;
}

static int respond(u64 id, int cont, s64 ret)
{
	struct seccomp_notif_resp r = { .id = id };
	if (cont)
		r.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
	else if (ret < 0 && ret > -4096)
		r.error = ret;
	else
		r.val = ret;
	if (ioctl(lfd, SECCOMP_IOCTL_NOTIF_SEND, &r) == 0)
		return 0;
	if (errno != ENOENT)
		fprintf(stderr, "NOTIF_SEND: %s\n", strerror(errno));
	return -1;      /* ENOENT: the thread was interrupted (signal) and will retry */
}

/* replay is over: do what -e says.  Held notifications stay unanswered in
 * stop mode (the threads stay frozen at the exact syscall). */
/* --- how the live run ended ---------------------------------------------- */

static struct { unsigned long lo, hi, off; char name[200]; } maps0[1024];
static int nmaps0;

/* the address space at the start of the replay, to name code addresses */
static void snapshot_maps(pid_t pid)
{
	char path[64], line[512];
	snprintf(path, sizeof(path), "/proc/%d/maps", pid);
	FILE *f = fopen(path, "r");
	while (f && nmaps0 < 1024 && fgets(line, sizeof(line), f)) {
		char name[256] = "";
		if (sscanf(line, "%lx-%lx %*s %lx %*s %*s %255s", &maps0[nmaps0].lo, &maps0[nmaps0].hi,
			   &maps0[nmaps0].off, name) >= 3) {
			snprintf(maps0[nmaps0].name, sizeof(maps0[nmaps0].name), "%s", name);
			nmaps0++;
		}
	}
	if (f)
		fclose(f);
}

static const char *where(u64 a)
{
	static char b[300];
	for (int i = 0; i < nmaps0; i++)
		if (a >= maps0[i].lo && a < maps0[i].hi) {
			snprintf(b, sizeof(b), "%s+%#llx", maps0[i].name[0] ? maps0[i].name : "anonymous",
				 (unsigned long long)(a - maps0[i].lo + maps0[i].off));
			return b;
		}
	return "memory mapped after the checkpoint";
}

static int is_fault(const struct rec_signal *r)
{
	return (r->sig == SIGSEGV || r->sig == SIGBUS || r->sig == SIGFPE || r->sig == SIGILL) &&
	       r->code > 0 && r->code < 128;
}

/* The signal that killed the live run.  The first one: when a thread crashes, the
 * kernel then kills its siblings with SIGKILL, and those are not the cause. */
/* how this thread's life ended according to the recording; failing that, how the
 * last recorded thread did; -1 if there is nothing */
static int own_end(const struct rec_signal *d)
{
	int last = -1;

	for (size_t i = 0; i < nsigs; i++)
		if (sigs[i]->sig == SIG_EXIT) {
			if (sigs[i]->tid == d->tid)
				return sigs[i]->code;
			last = sigs[i]->code;   /* the recorder may stop before this thread's */
		}
	return last;
}

static const struct rec_signal *live_death(void)
{
	const struct rec_signal *kill9 = NULL;
	for (size_t i = 0; i < nsigs; i++)
		if (sigs[i]->sig != SIG_EXIT && sigs[i]->fatal) {
			if (sigs[i]->sig != SIGKILL)
				return sigs[i];
			if (!kill9)
				kill9 = sigs[i];
		}
	/* A SIGKILL delivery can be the kernel ending the other threads: a normal
	 * exit_group, or the shortcut for a default-action signal.  The exit status
	 * tells them from a real SIGKILL. */
	if (kill9 && ((own_end(kill9) < 0) || (own_end(kill9) & 0x7f) == SIGKILL))
		return kill9;
	for (size_t i = 0; i < nsigs; i++)
		if (sigs[i]->sig == SIG_EXIT && (sigs[i]->code & 0x7f))
			return sigs[i];
	return NULL;
}

static void report_live_end(void)
{
	const struct rec_signal *d = live_death();
	unsigned long handled = 0;

	for (size_t i = 0; i < nsigs; i++)
		handled += sigs[i]->sig != SIG_EXIT && !sigs[i]->fatal;
	if (handled)
		printf("   the live run received %lu signal(s) that did not kill it; arbitrary signals "
		       "are not replayed\n", handled);
	if (!d)
		return;
	if (d->sig == SIG_EXIT) {
		printf("   the live run ended here: thread %u was killed by signal %d (%s), with no "
		       "delivery record (SIGKILL, or a group exit)\n", d->ns_tid, d->code & 0x7f,
		       strsignal(d->code & 0x7f));
		return;
	}
	printf("   the live run ended here: thread %u died of signal %d (%s), code %d\n",
	       d->ns_tid, d->sig, strsignal(d->sig), d->code);
	if (own_end(d) >= 0 && !(own_end(d) & 0x7f))
		printf("   !! yet the process exited with code %d: another thread's exit_group got "
		       "there first and hid this crash\n", (own_end(d) >> 8) & 0xff);
	if (is_fault(d))
		printf("     fault address %#llx\n", (unsigned long long)d->addr);
	printf("     at %#llx in %s, stack pointer %#llx\n", (unsigned long long)d->rip, where(d->rip),
	       (unsigned long long)d->rsp);
}

static void print_totals(void)
{
	printf("   replayed %lu syscalls (faked %lu, ran %lu), skipped %lu "
	       "interrupted/restart records, %lu child starts or refused calls\n", n_fake + n_pass, n_fake, n_pass, n_skip, n_child);
	printf("   inputs checked %lu bytes, outputs injected %lu bytes, "
	       "warnings: %lu args, %lu inputs\n", in_bytes, out_bytes, n_argwarn, n_inwarn);
	if (n_relaxed)
		printf("   !! recorded order relaxed %lu time(s) to get past a stall: the outcome may "
		       "differ from the live run\n", n_relaxed);
	printf("   fds installed %lu%s, fd warnings %lu, mappings pinned %lu, new thread ids set %lu, "
	       "timing-only differences (futex, sleep, signal return, monotonic clock) %lu\n", n_fds,
	       opt_nofd ? " (-N: installation off)" : "", n_fdwarn, n_pin, n_pidpin, n_slack);
	if (n_ptrdiff)
		printf("   %lu call(s) passed buffers at other addresses than recorded (same bytes: the "
		       "heap is laid out differently)\n", n_ptrdiff);
	if (n_jump)
		printf("   %lu time(s) a thread went ahead of one that was waiting for its lock\n", n_jump);
	if (n_sig)
		printf("   SIGPIPE delivered with a failed write %lu time(s)\n", n_sig);
	fflush(stdout);
}

static void finish(const char *why)
{
	printf("\n== %s ==\n", why);
	print_totals();
	report_live_end();
	for (size_t i = 0; i < nthrs; i++)      /* -T: threads that never got to start */
		if (thrs[i].start_tid)
			ptrace(PTRACE_DETACH, thrs[i].start_tid, 0, 0), thrs[i].start_tid = 0;
	switch (opt_end) {
	case END_LIVE:
		state = LIVE;
		for (size_t i = 0; i < nthrs; i++)
			if (thrs[i].held_id) {
				lfd = thrs[i].held_lfd;
				respond(thrs[i].held_id, 1, 0), thrs[i].held_id = 0;
			}
		printf("   target continues live (real syscalls); Ctrl-C ends it\n");
		break;
	case END_KILL:
		kill_all(SIGKILL);
		printf("   target killed (%d process(es))\n", nprocs);
		break;
	case END_STOP:
		kill_all(SIGSTOP);
		printf("   target %d stopped (SIGSTOP) in that state: inspect it with\n"
		       "   gdb -p %d or /proc/%d.  Keep this tool running while you do:\n"
		       "   if it exits, the target's syscalls fail with ENOSYS.  Ctrl-C ends.\n",
		       tgid, tgid, tgid);
		break;
	}
	fflush(stdout);
}

/* did this syscall come from the vDSO's own code? */
static int from_vdso(const struct seccomp_notif *req)
{
	char path[64], line[256];
	int yes = 0;
	snprintf(path, sizeof(path), "/proc/%d/maps", req->pid);
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		unsigned long lo, hi;
		if (strstr(line, "[vdso]") && sscanf(line, "%lx-%lx", &lo, &hi) == 2)
			yes = req->data.instruction_pointer >= lo && req->data.instruction_pointer < hi;
	}
	if (f)
		fclose(f);
	return yes;
}

static void diverge(struct thr *t, const struct ent *e, const struct seccomp_notif *req,
		    const char *what)
{
	char b1[200], b2[200];
	const struct rec_syscall *s = e->s;

	fmt_call(b1, sizeof(b1), s->nr, s->args, nargs(s->nr));
	fmt_call(b2, sizeof(b2), req->data.nr, (const u64 *)req->data.args, nargs(req->data.nr));
	printf("\n!! DIVERGENCE in thread %u at record #%llu (%zu of %zu for this thread): %s\n",
	       t->ns_tid, (unsigned long long)s->h.seq, t->pos + 1, t->n, what);
	printf("     expected  %s = %lld\n", b1, (long long)s->ret);
	printf("     actual    %s\n", b2);
	if (req->data.nr == SYS_getrandom && (long)s->nr != SYS_getrandom && from_vdso(req))
		printf("     this call comes from the vDSO: glibc makes random bytes in user space, without\n"
		       "     syscalls, so they are not in the log.  The kernel's random generation changed\n"
		       "     (another machine, or a reseed), so the vDSO asks for a new key.  See REPLAY.md.\n");
}

/* Compare the recorded inputs (what the original process handed to the
 * kernel) with what the replayed process hands over now. */
static int check_inputs(struct thr *t, const struct ent *e, const struct seccomp_notif *req)
{
	const u64 *a = (const u64 *)req->data.args;
	int bad = 0;

	/* how long a sleep lasts, or until when, is timing: an absolute deadline
	 * is computed from the monotonic clock, which is read for real */
	if (req->data.nr == SYS_nanosleep || req->data.nr == SYS_clock_nanosleep)
		return 0;
	for (unsigned i = 0; i < e->nd; i++) {
		const struct rec_data *d = e->d[i];
		if (d->kind != D_IN && d->kind != D_STR)
			continue;
		if (d->h.flags & DF_EXIT) {
			/* only use the exit copy when there is no entry copy */
			int has_entry = 0;
			for (unsigned j = 0; j < e->nd; j++)
				if (!(e->d[j]->h.flags & DF_EXIT) && e->d[j]->arg == d->arg &&
				    e->d[j]->kind == d->kind && e->d[j]->idx == d->idx &&
				    e->d[j]->offset == d->offset)
					has_entry = 1;
			if (has_entry)
				continue;
		}
		u64 addr;
		unsigned char *now = malloc(d->len ? d->len : 1);
		if (resolve(req->pid, req->data.nr, a, d, &addr) ||
		    mem_read(req->pid, addr, now, d->len)) {
			if (!bad++)
				diverge(t, e, req, "input not readable");
			printf("     arg%u: cannot read %u bytes at %#llx\n", d->arg, d->len,
			       (unsigned long long)addr);
			free(now);
			continue;
		}
		in_bytes += d->len;
		/* An iovec array or a msghdr holds addresses.  Where a buffer sits is
		 * not what the program says: with several threads allocating, the heap
		 * is laid out by who asked first.  The lengths must match, and the
		 * bytes the addresses lead to are compared piece by piece below. */
		if (d->idx == 0xffff || d->idx == 0xfffe) {
			int ptrs = 0;
			for (unsigned k = 0; k + 8 <= d->len; k += 8) {
				unsigned o = (d->offset + k) % (d->idx == 0xffff ? 16 : 56);
				int is_ptr = d->idx == 0xffff ? o == 0 : (o == 0 || o == 16 || o == 32);
				if (is_ptr && memcmp(now + k, d->data + k, 8)) {
					memcpy(now + k, d->data + k, 8);
					ptrs = 1;
				}
			}
			n_ptrdiff += ptrs;
		}
		if (memcmp(now, d->data, d->len)) {
			unsigned k = 0, diff = 0;
			while (k < d->len && now[k] == (unsigned char)d->data[k])
				k++;
			for (unsigned j = 0; j < d->len; j++)
				diff += now[j] != (unsigned char)d->data[j];
			if (!bad++)
				diverge(t, e, req, "input buffer differs");
			printf("     arg%u%s: %u of %u bytes differ, first at byte %u\n",
			       d->arg, d->kind == D_STR ? " (path)" : "", diff, d->len, d->offset + k);
			preview("recorded", (const unsigned char *)d->data, d->len);
			preview("replayed", now, d->len);
		}
		free(now);
	}
	return bad;
}

/* Write the recorded outputs into the replayed thread's memory.
 * Addresses are resolved first, then written, so a msghdr we overwrite
 * cannot redirect the following pieces. */
static int inject_outputs(const struct ent *e, const struct seccomp_notif *req)
{
	const u64 *a = (const u64 *)req->data.args;
	u64 addrs[e->nd ? e->nd : 1];

	for (unsigned i = 0; i < e->nd; i++)
		if (e->d[i]->kind == D_OUT && resolve(req->pid, req->data.nr, a, e->d[i], &addrs[i]))
			return -1;
	for (unsigned i = 0; i < e->nd; i++) {
		const struct rec_data *d = e->d[i];
		if (d->kind != D_OUT)
			continue;
		if (mem_write(req->pid, addrs[i], d->data, d->len)) {
			printf("   cannot write %u bytes at %#llx (record #%llu)\n", d->len,
			       (unsigned long long)addrs[i], (unsigned long long)e->s->h.seq);
			return -1;
		}
		out_bytes += d->len;
	}
	return 0;
}

/* Is this fd the container's stdout or stderr?  Decided by the file, not the
 * number: nginx opens /dev/stdout again as fd 5, and a child whose fd 1 was
 * redirected into a pipe is not writing to the container's stdout. */
static char stdio_file[2][256];

static void remember_stdio(pid_t pid)
{
	char p[64];
	for (int i = 0; i < 2; i++) {
		snprintf(p, sizeof(p), "/proc/%d/fd/%d", pid, i + 1);
		ssize_t n = readlink(p, stdio_file[i], sizeof(stdio_file[i]) - 1);
		stdio_file[i][n > 0 ? n : 0] = 0;
	}
}

static int is_stdio(pid_t pid, u64 fd)
{
	char p[64], l[256];
	snprintf(p, sizeof(p), "/proc/%d/fd/%llu", pid, (unsigned long long)fd);
	ssize_t n = readlink(p, l, sizeof(l) - 1);
	if (n <= 0)
		return 0;
	l[n] = 0;
	return !strcmp(l, stdio_file[0]) || !strcmp(l, stdio_file[1]);
}

/* what the target writes to its stdout/stderr (the write itself is faked) */
static void capture_write(const struct seccomp_notif *req, s64 ret)
{
	const u64 *a = (const u64 *)req->data.args;
	long nr = req->data.nr;
	char buf[65536];

	if (!wfile || ret <= 0 || !is_stdio(req->pid, a[0]))
		return;
	if (nr == SYS_write || nr == SYS_pwrite64) {
		for (u64 off = 0; off < (u64)ret; ) {
			size_t n = (u64)ret - off < sizeof(buf) ? (u64)ret - off : sizeof(buf);
			if (mem_read(req->pid, a[1] + off, buf, n))
				break;
			fwrite(buf, 1, n, wfile);
			off += n;
		}
	} else if (nr == SYS_writev) {
		for (u64 i = 0; i < a[2] && ret > 0; i++) {
			struct iovec v;
			if (mem_read(req->pid, a[1] + i * sizeof(v), &v, sizeof(v)))
				break;
			size_t n = v.iov_len < (size_t)ret ? v.iov_len : (size_t)ret;
			for (size_t off = 0; off < n; ) {
				size_t k = n - off < sizeof(buf) ? n - off : sizeof(buf);
				if (mem_read(req->pid, (u64)v.iov_base + off, buf, k))
					break;
				fwrite(buf, 1, k, wfile);
				off += k;
			}
			ret -= n;
		}
	}
	fflush(wfile);
}

/* --- file descriptors ---------------------------------------------------
 *
 * A faked open() returns the recorded number, say 5, but creates nothing:
 * fd 5 does not exist in the replayed process.  That is fine while every
 * later syscall on fd 5 is faked too (read, fstat, close), but not when one
 * runs for real: mmap(fd 5) fails with EBADF and the process diverges.
 *
 * SECCOMP_IOCTL_NOTIF_ADDFD lets the supervisor put one of *its* fds into
 * the target's fd table while the target is parked in the syscall:
 *   SETFD  at exactly newfd (like dup2: whatever was there is replaced)
 *   SEND   and answer the syscall with that number, atomically
 * So for a recorded "openat(...) = 5" we open the same file ourselves and
 * ADDFD it at 5 with SEND.  The data the process reads stays the recorded
 * one (read is still faked); the fd only has to be real for the syscalls
 * that run for real.
 */

enum { FD_NONE, FD_ONE, FD_PAIR, FD_DUP };

static int fd_kind(long nr, const u64 *a)
{
	switch (nr) {
	case SYS_open: case SYS_openat: case SYS_openat2: case SYS_creat:
	case SYS_open_by_handle_at: case SYS_socket: case SYS_accept: case SYS_accept4:
	case SYS_epoll_create: case SYS_epoll_create1: case SYS_eventfd: case SYS_eventfd2:
	case SYS_timerfd_create: case SYS_memfd_create: case SYS_inotify_init:
	case SYS_inotify_init1: case SYS_fanotify_init: case SYS_userfaultfd:
	case SYS_pidfd_open: case SYS_pidfd_getfd: case SYS_perf_event_open:
	case SYS_io_uring_setup: case SYS_mq_open:
		return FD_ONE;
	case SYS_signalfd: case SYS_signalfd4:      /* fd != -1: updates that fd */
		return (int)a[0] == -1 ? FD_ONE : FD_NONE;
	case SYS_pipe: case SYS_pipe2: case SYS_socketpair:
		return FD_PAIR;
	case SYS_dup: case SYS_dup2: case SYS_dup3:
		return FD_DUP;
	case SYS_fcntl:
		return a[1] == F_DUPFD || a[1] == F_DUPFD_CLOEXEC ? FD_DUP : FD_NONE;
	}
	return FD_NONE;
}

/* does the new fd get close-on-exec?  (every call has its own flag spelling) */
static int fd_cloexec(pid_t pid, long nr, const u64 *a)
{
	struct open_how how;

	switch (nr) {
	case SYS_open:          return !!(a[1] & O_CLOEXEC);
	case SYS_openat:        return !!(a[2] & O_CLOEXEC);
	case SYS_open_by_handle_at: return !!(a[2] & O_CLOEXEC);
	case SYS_openat2:       return !mem_read(pid, a[2], &how, 8) && (how.flags & O_CLOEXEC);
	case SYS_socket: case SYS_socketpair: return !!(a[1] & SOCK_CLOEXEC);
	case SYS_accept4:       return !!(a[3] & SOCK_CLOEXEC);
	case SYS_pipe2: case SYS_eventfd2: case SYS_timerfd_create: case SYS_dup3:
				return !!(a[nr == SYS_pipe2 || nr == SYS_eventfd2 ||
					    nr == SYS_timerfd_create ? 1 : 2] & O_CLOEXEC);
	case SYS_epoll_create1: case SYS_inotify_init1: case SYS_userfaultfd:
				return !!(a[0] & O_CLOEXEC);
	case SYS_signalfd4:     return !!(a[3] & O_CLOEXEC);
	case SYS_memfd_create:  return !!(a[1] & MFD_CLOEXEC);
	case SYS_fanotify_init: return !!(a[0] & 1 /* FAN_CLOEXEC */);
	case SYS_perf_event_open: return !!(a[4] & 8 /* PERF_FLAG_FD_CLOEXEC */);
	case SYS_pidfd_open: case SYS_pidfd_getfd: case SYS_io_uring_setup:
	case SYS_mq_open:
				return 1;
	case SYS_fcntl:         return a[1] == F_DUPFD_CLOEXEC;
	}
	return 0;
}

/* a NUL-terminated string from the target, page by page (a path can end
 * just before an unmapped page) */
static int mem_read_str(pid_t pid, u64 addr, char *buf, size_t max)
{
	size_t got = 0;
	while (got < max - 1) {
		size_t n = 4096 - ((addr + got) & 4095);
		if (n > max - 1 - got)
			n = max - 1 - got;
		if (mem_read(pid, addr + got, buf + got, n))
			return -1;
		void *z = memchr(buf + got, 0, n);
		got += n;
		if (z)
			return 0;
	}
	buf[max - 1] = 0;
	return 0;
}

static int devnull(int acc, char *what, size_t wn, const char *why)
{
	snprintf(what, wn, "/dev/null (%s)", why);
	return open("/dev/null", acc | O_CLOEXEC);
}

/* The file an open() names, opened by us inside the target's root: same
 * file the real call would have reached on this machine.  RESOLVE_IN_ROOT
 * makes "/" and ".." and absolute symlinks stay inside the container (we
 * are root on the host: an escape would hand host files to the target).
 * Side effects of the original call (O_CREAT, O_TRUNC) are not repeated:
 * the replay only needs an fd, the file contents come from the log. */
static int open_like(pid_t pid, long nr, const u64 *a, char *what, size_t wn)
{
	char path[4096];
	u64 flags;
	int dirfd, base, fd;

	switch (nr) {
	case SYS_open:   dirfd = AT_FDCWD; flags = a[1]; break;
	case SYS_creat:  dirfd = AT_FDCWD; flags = O_WRONLY; break;
	case SYS_openat: dirfd = a[0]; flags = a[2]; break;
	case SYS_openat2: {
		struct open_how how;
		if (mem_read(pid, a[2], &how, 8))
			return devnull(O_RDWR, what, wn, "open_how unreadable");
		dirfd = a[0]; flags = how.flags; break;
	}
	default:
		return devnull(O_RDWR, what, wn, "open_by_handle_at");
	}
	if (mem_read_str(pid, a[nr == SYS_open || nr == SYS_creat ? 0 : 1], path, sizeof(path)))
		return devnull(O_RDWR, what, wn, "path unreadable");

	int acc = flags & O_ACCMODE;
	if ((flags & __O_TMPFILE) == __O_TMPFILE)   /* (it includes O_DIRECTORY) */
		return devnull(acc, what, wn, "O_TMPFILE");
	if (path[0] == '/')
		base = troot, dirfd = -1;
	else if (dirfd == AT_FDCWD) {
		char cwd[64];
		snprintf(cwd, sizeof(cwd), "/proc/%d/cwd", pid);
		base = open(cwd, O_PATH | O_DIRECTORY | O_CLOEXEC);
	} else
		base = syscall(SYS_pidfd_getfd, pidfd_for(pid), dirfd, 0);
	if (base < 0)
		return devnull(acc, what, wn, "no base directory");

	/* O_NONBLOCK so a FIFO without a writer cannot block us */
	struct open_how how = {
		.flags = (flags & (O_ACCMODE | O_DIRECTORY | O_NOFOLLOW | O_PATH | O_NONBLOCK |
				   O_APPEND | O_DIRECT | O_NOATIME)) | O_NONBLOCK | O_CLOEXEC,
		.resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS,
	};
	fd = syscall(SYS_openat2, base, path, &how, sizeof(how));
	if (fd < 0 && (errno == EACCES || errno == EROFS || errno == ETXTBSY) && acc != O_RDONLY) {
		how.flags = (how.flags & ~O_ACCMODE) | O_RDONLY;   /* an fd, even if read-only */
		fd = syscall(SYS_openat2, base, path, &how, sizeof(how));
	}
	int err = errno;
	if (base != troot)
		close(base);
	if (fd < 0 && err == ENOENT && (flags & O_CREAT)) {
		/* created after the checkpoint: nothing on disk here */
		fd = memfd_create(STANDIN_NAME, MFD_CLOEXEC);
		if (fd >= 0) {
			n_standin++;
			snprintf(what, wn, "memfd for new file %.230s", path);
			return fd;
		}
	}
	if (fd < 0)
		return devnull(acc, what, wn, strerror(err));
	if (!(flags & O_NONBLOCK))
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
	snprintf(what, wn, "file %.250s", path);
	return fd;
}

/* An fd for a faked fd-creating call: the real thing where we can make it
 * (same file, same kind of object), /dev/null otherwise. */
static int make_fd(pid_t pid, long nr, const u64 *a, char *what, size_t wn)
{
	int fd = -1;

	switch (nr) {
	case SYS_open: case SYS_openat: case SYS_openat2: case SYS_creat:
	case SYS_open_by_handle_at:
		return open_like(pid, nr, a, what, wn);
	case SYS_socket:
		/* made in our network namespace: it only has to be a socket of
		 * that type, its traffic is faked */
		fd = socket(a[0], (a[1] & ~SOCK_CLOEXEC) | SOCK_CLOEXEC, a[2]);
		snprintf(what, wn, "socket(%llu, %llu)", (unsigned long long)a[0],
			 (unsigned long long)(a[1] & 0xf));
		break;
	case SYS_accept: case SYS_accept4: {
		/* an unconnected socket like the listening one */
		int l = syscall(SYS_pidfd_getfd, pidfd_for(pid), (int)a[0], 0), d = 0, t = 0, p = 0;
		socklen_t sl = sizeof(int);
		if (l >= 0 && !getsockopt(l, SOL_SOCKET, SO_DOMAIN, &d, &sl) &&
		    !getsockopt(l, SOL_SOCKET, SO_TYPE, &t, &sl) &&
		    !getsockopt(l, SOL_SOCKET, SO_PROTOCOL, &p, &sl))
			fd = socket(d, t | SOCK_CLOEXEC | (nr == SYS_accept4 ? a[3] & SOCK_NONBLOCK : 0), p);
		if (l >= 0)
			close(l);
		snprintf(what, wn, "unconnected socket (accepted connection)");
		break;
	}
	case SYS_epoll_create: case SYS_epoll_create1:
		fd = epoll_create1(EPOLL_CLOEXEC);
		snprintf(what, wn, "epoll");
		break;
	case SYS_eventfd: case SYS_eventfd2:
		fd = eventfd(0, EFD_CLOEXEC | (nr == SYS_eventfd2 ? a[1] & (EFD_NONBLOCK | EFD_SEMAPHORE) : 0));
		snprintf(what, wn, "eventfd");
		break;
	case SYS_timerfd_create:
		fd = timerfd_create(a[0], TFD_CLOEXEC | (a[1] & TFD_NONBLOCK));
		snprintf(what, wn, "timerfd");
		break;
	case SYS_memfd_create:
		fd = memfd_create(STANDIN_NAME, MFD_CLOEXEC | (a[1] & MFD_ALLOW_SEALING));
		if (fd >= 0)
			n_standin++;
		snprintf(what, wn, "memfd");
		break;
	case SYS_inotify_init: case SYS_inotify_init1:
		fd = inotify_init1(IN_CLOEXEC | (nr == SYS_inotify_init1 ? a[0] & IN_NONBLOCK : 0));
		snprintf(what, wn, "inotify");
		break;
	}
	if (fd < 0)
		fd = devnull(O_RDWR, what, wn, fd == -1 && errno ? strerror(errno) : sysname(nr));
	return fd;
}

/* is fd n open in the target right now? */
static int target_has_fd(pid_t pid, int n)
{
	char p[64];
	snprintf(p, sizeof(p), "/proc/%d/fd/%d", pid, n);
	return access(p, F_OK) == 0;
}

/* SETFD: at exactly that number; SEND: also answer the syscall with it */
static int addfd(u64 id, int src, int at, int cloexec, int send)
{
	struct seccomp_notif_addfd a = {
		.id = id, .srcfd = src, .newfd = at,
		.flags = SECCOMP_ADDFD_FLAG_SETFD | (send ? SECCOMP_ADDFD_FLAG_SEND : 0),
		.newfd_flags = cloexec ? O_CLOEXEC : 0,
	};
	return ioctl(lfd, SECCOMP_IOCTL_NOTIF_ADDFD, &a);
}

/* Answer a faked fd-creating call: real fds at the recorded numbers, then
 * the recorded return value.  Returns 0 answered, -1 the notification is
 * gone (interrupted, will come again), 1 could not install (caller fakes
 * it the old way). */
static int install_fds(const struct ent *e, const struct seccomp_notif *req, int kind)
{
	const u64 *a = (const u64 *)req->data.args;
	long nr = req->data.nr;
	pid_t pid = req->pid;
	int cloexec = fd_cloexec(pid, nr, a);
	char what[300] = "";
	int src[2] = { -1, -1 }, at[2] = { -1, -1 }, n = 1, r = 0;

	switch (kind) {
	case FD_ONE:
		src[0] = make_fd(pid, nr, a, what, sizeof(what));
		at[0] = e->s->ret;
		break;
	case FD_DUP:            /* a copy of the target's own fd: the same open file */
		src[0] = syscall(SYS_pidfd_getfd, pidfd_for(pid), (int)a[0], 0);
		if (src[0] < 0)
			src[0] = devnull(O_RDWR, what, sizeof(what), "source fd missing");
		else
			snprintf(what, sizeof(what), "copy of fd %d", (int)a[0]);
		at[0] = e->s->ret;
		break;
	case FD_PAIR: {         /* the two numbers are in the recorded int[2] */
		int p[2] = { -1, -1 }, num[2];
		const struct rec_data *d = NULL;
		for (unsigned i = 0; i < e->nd; i++)
			if (e->d[i]->kind == D_OUT && e->d[i]->len == 8)
				d = e->d[i];
		if (!d)
			return 1;
		memcpy(num, d->data, 8);
		if (nr == SYS_socketpair) {
			if (socketpair(a[0], (a[1] & ~SOCK_CLOEXEC) | SOCK_CLOEXEC, a[2], p))
				p[0] = p[1] = -1;
			snprintf(what, sizeof(what), "socketpair");
		} else {
			if (pipe2(p, O_CLOEXEC | (nr == SYS_pipe2 ? a[1] & (O_NONBLOCK | O_DIRECT) : 0)))
				p[0] = p[1] = -1;
			snprintf(what, sizeof(what), "pipe");
		}
		for (int i = 0; i < 2; i++)
			if ((src[i] = p[i]) < 0)
				src[i] = devnull(O_RDWR, what, sizeof(what), strerror(errno));
		at[0] = num[0], at[1] = num[1], n = 2;
		break;
	}
	}

	for (int i = 0; i < n; i++) {
		/* dup2/dup3 replace on purpose; anything else should land on a
		 * free number, or the fd tables have drifted apart */
		if (kind != FD_DUP && target_has_fd(pid, at[i])) {
			n_fdwarn++;
			printf("   warning: #%llu %s: fd %d is already open in the replayed process "
			       "(it was free in the recording); replacing it\n",
			       (unsigned long long)e->s->h.seq, sysname(nr), at[i]);
		}
		/* single fd: install and answer in one step (SEND) */
		int send = kind != FD_PAIR;
		int rc = src[i] < 0 ? -1 : addfd(req->id, src[i], at[i], cloexec, send);
		if (rc < 0) {
			r = errno == ENOENT ? -1 : 1;
			if (r > 0) {
				n_fdwarn++;
				printf("   warning: #%llu %s: could not install fd %d: %s\n",
				       (unsigned long long)e->s->h.seq, sysname(nr), at[i], strerror(errno));
			}
			break;
		}
		n_fds++;
		if (opt_verbose)
			printf("         fd %d <- %s%s\n", at[i], what, cloexec ? " (cloexec)" : "");
	}
	for (int i = 0; i < 2; i++)
		if (src[i] >= 0)
			close(src[i]);
	if (r == 0 && kind == FD_PAIR && respond(req->id, 0, e->s->ret))
		r = -1;
	return r;
}

/* recvmsg that received fds (SCM_RIGHTS): the numbers are in the recorded
 * control data, but the files were in another process's hands.  Put a stand-in
 * at each number, so close() and the fd table follow the recording.  (Without
 * knowing the file, a real mmap of it still fails: REPLAY.md, limitations.) */
static void install_scm_fds(const struct ent *e, const struct seccomp_notif *req)
{
	for (unsigned i = 0; i < e->nd; i++) {
		const struct rec_data *d = e->d[i];
		size_t pos = 0;

		if (d->kind != D_OUT || d->idx != 0xfffc)
			continue;
		while (pos + sizeof(struct cmsghdr) <= d->len) {
			struct cmsghdr h;
			memcpy(&h, d->data + pos, sizeof(h));
			if (h.cmsg_len < sizeof(h) || pos + h.cmsg_len > d->len)
				break;
			if (h.cmsg_level == SOL_SOCKET && h.cmsg_type == SCM_RIGHTS) {
				size_t nfd = (h.cmsg_len - sizeof(h)) / sizeof(int);
				for (size_t k = 0; k < nfd; k++) {
					int fd, src = open("/dev/null", O_RDWR | O_CLOEXEC);
					memcpy(&fd, d->data + pos + sizeof(h) + k * sizeof(int), sizeof(int));
					if (src >= 0 && addfd(req->id, src, fd, 0, 0) == 0)
						n_fds++;
					if (src >= 0)
						close(src);
				}
			}
			pos += CMSG_ALIGN(h.cmsg_len);
		}
	}
}

/* --- addresses of new mappings --------------------------------------------
 *
 * mmap(NULL, ...) runs for real, and the kernel picks the address: the
 * highest free gap below mm->mmap_base.  mmap_base is randomized at execve.
 * CRIU puts every old mapping back at its old address but cannot restore
 * mmap_base (the restored task inherits CRIU's), so new mappings land
 * elsewhere than in the recording.  Allocators notice: jemalloc wants
 * aligned chunks and takes another path when an address is misaligned.
 *
 * seccomp cannot change a syscall's arguments, ptrace can.  The thread is
 * parked in the notification wait (interruptible).  We seize it and
 * PTRACE_INTERRUPT it: the wait returns -ERESTARTSYS, the thread stops on
 * its way back to user space, and after the stop the kernel restarts the
 * syscall from the registers as they are then.  We set the address to the
 * recorded one with MAP_FIXED_NOREPLACE (never clobbers a mapping) and
 * detach.  The restarted mmap comes back to us as a new notification, which
 * we let run.  A few hundred microseconds of ptrace per pinned mapping.
 */
static int needs_pin(const struct seccomp_notif *req, const struct ent *e)
{
	const u64 *a = (const u64 *)req->data.args;
	s64 r = e->s->ret;

	if (r < 0 && r > -4096)
		return 0;
	if (req->data.nr == SYS_mmap)
		return !(a[3] & (MAP_FIXED | MAP_FIXED_NOREPLACE)) && a[0] != (u64)r;
	if (req->data.nr == SYS_mremap)       /* moved: pin the new place */
		return (a[3] & MREMAP_MAYMOVE) && !(a[3] & MREMAP_FIXED) && a[0] != (u64)r;
	return 0;
}

static int pin_address(const struct seccomp_notif *req, const struct ent *e)
{
	pid_t tid = req->pid;
	struct user_regs_struct r;
	int st;

	if (ptrace(PTRACE_SEIZE, tid, 0, 0) || ptrace(PTRACE_INTERRUPT, tid, 0, 0))
		return -1;
	for (;;) {
		if (waitpid(tid, &st, __WALL) < 0 || !WIFSTOPPED(st)) {
			ptrace(PTRACE_DETACH, tid, 0, 0);
			return -1;
		}
		if (st >> 16 == PTRACE_EVENT_STOP)
			break;
		/* a real signal arrived first: let it through, wait for our stop */
		ptrace(PTRACE_CONT, tid, 0, (void *)(long)WSTOPSIG(st));
	}
	ptrace(PTRACE_GETREGS, tid, 0, &r);
	if ((long)r.orig_rax != req->data.nr || (long)r.rax != -512 /* ERESTARTSYS */) {
		ptrace(PTRACE_DETACH, tid, 0, 0);
		return -1;
	}
	if (req->data.nr == SYS_mmap) {
		r.rdi = e->s->ret;
		r.r10 |= MAP_FIXED_NOREPLACE;
	} else {
		r.r10 |= MREMAP_FIXED;
		r.r8 = e->s->ret;
	}
	ptrace(PTRACE_SETREGS, tid, 0, &r);
	ptrace(PTRACE_DETACH, tid, 0, 0);
	return 0;
}

/* --- a signal that comes with a syscall's result -----------------------------
 *
 * A write to a pipe without readers fails with EPIPE and the kernel sends the
 * writer SIGPIPE, which is handled before the write returns to the program.  A
 * faked write must do the same.  Sending the signal while the thread is parked
 * would interrupt the notification wait and make the kernel restart the call;
 * sending it after the answer lets the program run a little first.  So, as for
 * pin_address: seize the thread, interrupt it (the wait returns -ERESTARTSYS and
 * the thread stops on its way out), put the recorded result in rax with no
 * syscall to restart, queue the signal and detach.  The kernel then delivers the
 * signal on top of that result, as it did live: handler first, then the program
 * sees EPIPE; or the default action, death by SIGPIPE.  The notification is gone
 * (interrupted), so it is not answered. */
static int sends_sigpipe(const struct seccomp_notif *req)
{
	const u64 *a = (const u64 *)req->data.args;

	switch (req->data.nr) {
	case SYS_write: case SYS_writev: case SYS_pwrite64: case SYS_pwritev:
	case SYS_pwritev2: case SYS_sendfile: case SYS_splice: case SYS_vmsplice:
	case SYS_tee: case SYS_copy_file_range:
		return 1;
	case SYS_sendto: case SYS_sendmmsg:
		return !(a[3] & MSG_NOSIGNAL);
	case SYS_sendmsg:
		return !(a[2] & MSG_NOSIGNAL);
	}
	return 0;
}

static int return_with_signal(const struct seccomp_notif *req, s64 ret, int sig)
{
	pid_t tid = req->pid;
	struct user_regs_struct r;
	int st;

	if (ptrace(PTRACE_SEIZE, tid, 0, 0) || ptrace(PTRACE_INTERRUPT, tid, 0, 0))
		return -1;
	for (;;) {
		if (waitpid(tid, &st, __WALL) < 0 || !WIFSTOPPED(st)) {
			ptrace(PTRACE_DETACH, tid, 0, 0);
			return -1;
		}
		if (st >> 16 == PTRACE_EVENT_STOP)
			break;
		ptrace(PTRACE_CONT, tid, 0, (void *)(long)WSTOPSIG(st));
	}
	ptrace(PTRACE_GETREGS, tid, 0, &r);
	if ((long)r.orig_rax != req->data.nr || (long)r.rax != -512 /* ERESTARTSYS */) {
		ptrace(PTRACE_DETACH, tid, 0, 0);
		return -1;
	}
	r.rax = ret;
	r.orig_rax = -1;                /* not a syscall any more: nothing to restart */
	ptrace(PTRACE_SETREGS, tid, 0, &r);
	syscall(SYS_tgkill, tgid_of(tid), tid, sig);
	ptrace(PTRACE_DETACH, tid, 0, 0);
	return 0;
}

static struct ent *next_ent(struct thr *t)
{
	while (t->pos < t->n && t->q[t->pos]->done)
		t->pos++;
	return t->pos < t->n ? t->q[t->pos] : NULL;
}

/* -T.  A thread's user-space work runs from one syscall's exit to the next
 * one's entry; work of different threads that overlapped in time is where
 * races on shared memory happen (nginx: workers bump a connection counter in
 * a MAP_SHARED page right after accept4 returns).  -S orders only entries.
 * -T lets exactly one thread run user code at a time and releases syscalls
 * in the order they *returned* in the recording, so the stretches of user
 * code run one after the other in their recorded start order. */
/* The moment a record counts for -T: normally when the call returned, but two
 * kinds of call make something else possible, so they count from when they
 * started.  A clone: its child's first syscalls return before the parent's
 * clone does, but the child only exists once that clone has been released.  An
 * execve: after vfork the parent wakes in the middle of the child's execve and
 * its next calls return first, but it cannot wake until the execve has run. */
static u64 release_time(const struct ent *e)
{
	const struct rec_syscall *r = e->s;
	int early = is_clone(r->nr) || r->nr == SYS_execve || r->nr == SYS_execveat;
	return early && e->enter ? e->enter : r->ts_exit;
}

static int cmp_exit(const void *a, const void *b)
{
	const struct rec_syscall *x = (*(struct ent * const *)a)->s, *y = (*(struct ent * const *)b)->s;
	u64 tx = (*(struct ent * const *)a)->key, ty = (*(struct ent * const *)b)->key;
	if (tx != ty)
		return tx < ty ? -1 : 1;
	return x->h.seq < y->h.seq ? -1 : x->h.seq > y->h.seq;
}

/* Each record stands for the stretch of user code that follows it, up to the
 * thread's next syscall, and -T runs the stretches one after the other.  Two that
 * overlapped in the live run need an order, and the log cannot give it: what they
 * did to shared memory made no syscall.  The guess is the order in which they
 * *ended* (the next syscall was entered: seq gives that order exactly).  A thread
 * coming out of a syscall starts cold and at an unsteady pace; the way into the
 * next syscall is short.  And code that works under a lock usually ends with a
 * syscall right after releasing it (a futex wake, a write).  Measured on thread
 * pools (2026-11-12): by end 4 of 6 recordings replayed to the last line, by start
 * 1 of 6; the nginx and two-thread tests pass either way.
 * The place is a moment inside the stretch, so stretches that did not overlap
 * keep their order whatever the rule. */
static void order_serial(void)
{
	/* seq is the true order of syscall entries.  The entry timestamp is read
	 * just after seq is taken, so two entries less than a microsecond apart
	 * can have timestamps the other way round: pull the earlier one down. */
	u64 next = ~0ULL;
	for (size_t i = nents; i-- > 0;) {
		ents[i].enter = ents[i].s->ts_enter;
		if (!ents[i].enter)
			continue;
		if (ents[i].enter >= next)
			ents[i].enter = next - 1;
		next = ents[i].enter;
	}
	for (size_t i = 0; i < nthrs; i++) {
		struct thr *t = &thrs[i];
		const struct ent *nx = NULL;            /* the thread's next record */

		for (size_t j = t->n; j-- > 0;) {
			struct ent *e = t->q[j];
			const struct rec_syscall *r = e->s, *n = nx ? nx->s : NULL;

			if (e->done)
				continue;
			e->key = release_time(e);
			if (is_clone(r->nr) || r->nr == SYS_execve || r->nr == SYS_execveat || !n)
				;
			/* It ends by waiting for a lock (value 2: "locked, with waiters",
			 * the convention of glibc, musl, Go, Rust): it found the lock
			 * taken.  One at a time, nobody can find a lock taken, so it goes
			 * where the wait ended and it got the lock: after the holder. */
			else if (n->nr == SYS_futex && futex_waits(n->args[1]) && (u32)n->args[2] == 2 &&
				 nx->key > e->key)
				e->key = nx->key;
			else if (nx->enter > e->key)
				e->key = nx->enter;
			nx = e;
		}
	}
	byexit = malloc(nents * sizeof(*byexit));
	for (size_t i = 0; i < nents; i++)
		byexit[i] = &ents[i];
	qsort(byexit, nents, sizeof(*byexit), cmp_exit);
}

/* -T, the start.  A restored thread that was in user code runs until its first
 * syscall before any listener can hold it, and all the threads would do that at
 * once: the first stretch of each would be a free-for-all (one takes a lock and
 * parks at its syscall, the others pile up behind it).  So each thread with
 * records is kept in a ptrace stop from before the process is resumed, and
 * released when its first recorded syscall is the next thing in the order. */
static void hold_thread(struct thr *t, pid_t tid)
{
	const struct ent *first = NULL;
	int st;

	for (size_t j = 0; j < t->n && !first; j++)
		if (!t->q[j]->done)
			first = t->q[j];
	if (!first || !first->enter)
		return;
	if (ptrace(PTRACE_SEIZE, tid, 0, 0))
		return;
	if (ptrace(PTRACE_INTERRUPT, tid, 0, 0) || waitpid(tid, &st, __WALL) < 0 || !WIFSTOPPED(st)) {
		ptrace(PTRACE_DETACH, tid, 0, 0);
		return;
	}
	t->start_tid = tid;
	t->start_key = first->enter;
}

static struct thr *first_held(void)
{
	struct thr *best = NULL;
	for (size_t i = 0; i < nthrs; i++)
		if (thrs[i].start_tid && (!best || thrs[i].start_key < best->start_key))
			best = &thrs[i];
	return best;
}

static struct thr *running_thread(const struct thr *except);

/* start the next held thread if its turn has come (always, if force) */
static int start_threads(int force)
{
	struct thr *t = first_held();

	if (!t || running_thread(NULL))
		return 0;
	while (xnext < nents && byexit[xnext]->done)
		xnext++;
	if (!force && xnext < nents && byexit[xnext]->key < t->start_key)
		return 0;
	ptrace(PTRACE_DETACH, t->start_tid, 0, 0);
	t->start_tid = 0;
	t->running = 1;
	return 1;
}

static int next_exit_is(const struct ent *e)
{
	struct thr *h = first_held();
	if (h && h->start_key <= e->key)
		return 0;               /* a thread has to start first */
	while (xnext < nents && byexit[xnext]->done)
		xnext++;
	return xnext < nents && byexit[xnext] == e;
}

static struct thr *running_thread(const struct thr *except)
{
	for (size_t i = 0; i < nthrs; i++)
		if (thrs[i].running && &thrs[i] != except)
			return &thrs[i];
	return NULL;
}

/* after this syscall is let through for real, the thread may sleep in the
 * kernel for a long time: it is not running user code */
static int blocks(const struct seccomp_notif *req)
{
	switch (req->data.nr) {
	case SYS_futex: {
		int op = req->data.args[1] & 0x7f;
		return op == 0 /* WAIT */ || op == 9 /* WAIT_BITSET */ || op == 6 /* LOCK_PI */ ||
		       op == 11 /* WAIT_REQUEUE_PI */ || op == 13 /* LOCK_PI2 */;
	}
	case SYS_rt_sigsuspend: case SYS_pause: case SYS_wait4: case SYS_waitid:
	case SYS_exit: case SYS_exit_group:
	case SYS_vfork:         /* the parent sleeps until the child execs or exits */
		return 1;
	case SYS_clone:
		return !!(req->data.args[0] & CLONE_VFORK);
	case SYS_clone3: {
		u64 flags = 0;
		mem_read(req->pid, req->data.args[0], &flags, sizeof(flags));
		return !!(flags & CLONE_VFORK);
	}
	}
	return 0;
}

static int global_next_seq_is(const struct ent *e)
{
	while (gnext < nents && ents[gnext].done)
		gnext++;
	return gnext < nents && &ents[gnext] == e;
}

static void consume(struct ent *e)
{
	e->done = 1;
	remaining--;
}

/* Some calls only show how a thread got through a lock, and that depends on
 * nanosecond timing: futex (did it have to sleep?), sched_yield (how long did
 * it spin?), rt_sigreturn (did a signal of its own child arrive just then?) and
 * reading the monotonic clock (a timed
 * wait computes its deadline only when the lock is taken: pthread_cond_timedwait,
 * Python's GIL).  Their records are hints, not part of what the thread must do:
 * a recorded one the replay does not make is dropped, and one it makes that is
 * not recorded still runs (a clock read gets the real clock: a program spinning
 * until a deadline needs time to move).  What decides the outcome is the process's memory,
 * which the waker sees too.
 * CLOCK_REALTIME stays strict: programs print it and store it. */
/* Records can be left over from threads that never started: the recording stopped
 * inside the fork that creates them (its parent has no record of it). */
static int only_unstarted_left(void)
{
	for (size_t i = 0; remaining && i < nthrs; i++)
		if (thrs[i].seen && next_ent(&thrs[i]))
			return 0;
	return 1;
}

/* Every record that can be used has been. */
static void end_reached(pid_t g)
{
	if (remaining)
		printf("   %zu records left belong to threads that never started: the recording "
		       "stopped inside the fork that creates them\n", remaining);

	const struct rec_signal *d = live_death();
	int dsig = !d ? 0 : d->sig == SIG_EXIT ? d->code & 0x7f : d->sig;

	state = ENDED;
	if (d && !is_fault(d) && opt_end != END_STOP) {
		/* killed from outside, or by a timer or SIGPIPE: nothing in the
		 * replay will send it, and it is what the live run did next */
		printf("\n== end of the recording reached: delivering the signal that killed "
		       "the live run ==\n");
		print_totals();
		report_live_end();
		fflush(stdout);
		kill(g, dsig);
		return;
	}
	finish("end of the recording reached");
	if (d && d->sig != SIG_EXIT && is_fault(d))
		printf("   !! the replay reached the end without that fault: what caused it "
		       "is not in the log (a race in user space, or memory written by "
		       "another process)\n");
}

static int is_timing_only(long nr, u64 a0)
{
	/* a sleep too: how often a thread polls shared memory is a matter of timing */
	return nr == SYS_futex || nr == SYS_rt_sigreturn || nr == SYS_sched_yield ||
	       nr == SYS_nanosleep || nr == SYS_clock_nanosleep ||
	       (nr == SYS_clock_gettime && a0 == CLOCK_MONOTONIC);
}

static int same_call(const struct ent *e, const struct seccomp_notif *req)
{
	if ((long)e->s->nr != req->data.nr)
		return 0;
	if (req->data.nr == SYS_clock_gettime)
		return e->s->args[0] == req->data.args[0];
	return req->data.nr != SYS_futex || (e->s->args[0] == req->data.args[0] &&
					     (e->s->args[1] & 0x7f) == (req->data.args[1] & 0x7f));
}

/* processes that have called execve during the replay */
static pid_t execed[256];
static int nexeced;

static int has_execed(pid_t g)
{
	for (int i = 0; i < nexeced; i++)
		if (execed[i] == g)
			return 1;
	return 0;
}

static void note_exec(pid_t g)
{
	if (!has_execed(g))
		execed[nexeced++ % 256] = g, nexeced = nexeced > 256 ? 256 : nexeced;
}

/* Handle one notification.  Returns 1 if it was answered or deliberately
 * held, 0 if it must wait for its turn (strict mode). */
static int handle(struct seccomp_notif *req)
{
	u32 ns = ns_tid_of(req->pid);
	struct thr *t;
	struct ent *e;
	char b[200];

	if (state == LIVE)
		return respond(req->id, 1, 0), 1;
	if (state != RUN)
		return 1;                       /* hold: never answered */

	t = thread_for(ns);
	if (t) {
		t->running = 0;                 /* it is at a syscall now */
		t->lock_wait = 0;
		if (!t->seen) {                 /* new process: can be signalled, its fds read */
			t->seen = 1;
			pidfd_for(req->pid);
		}
	}
	if (!t) {
		fmt_call(b, sizeof(b), req->data.nr, (const u64 *)req->data.args, nargs(req->data.nr));
		printf("\n!! DIVERGENCE: thread %u (host %u) is not in the recording; it calls %s\n",
		       ns, req->pid, b);
		if (!fh->follow)
			printf("     the log was recorded without -f: if the target forks, record "
			       "with sysrr -f\n");
		state = DIVERGED;
		finish("diverged: unknown thread");
		if (opt_end == END_LIVE)
			respond(req->id, 1, 0);
		return 1;
	}

	e = next_ent(t);
	/* The live process re-ran a signal-interrupted sleep via restart_syscall;
	 * without the signal the replayed one never does. */
	while (e && e->s->nr == SYS_restart_syscall && req->data.nr != SYS_restart_syscall) {
		consume(e);
		n_skip++;
		e = next_ent(t);
	}
	while (e && is_timing_only(e->s->nr, e->s->args[0]) && !same_call(e, req)) {
		if (opt_verbose) {
			fmt_call(b, sizeof(b), e->s->nr, e->s->args, 3);
			printf("#%-7llu %-5u drop %s (recorded, not made now: timing only)\n",
			       (unsigned long long)e->s->h.seq, ns, b);
		}
		consume(e);
		n_slack++;
		e = next_ent(t);
	}
	int timing = is_timing_only(req->data.nr, req->data.args[0]);
	/* -T: the wait this thread was still in when the recording stopped (its
	 * record has no result and was skipped at load).  It never returned, so it is
	 * held.  Run for real it would take a wake-up meant for a thread whose
	 * recorded wait is being answered from the log. */
	if (opt_serial && !e && t->n && req->data.nr == SYS_futex && futex_waits(req->data.args[1]) &&
	    is_restart_ret(t->q[t->n - 1]->s->ret) && same_call(t->q[t->n - 1], req)) {
		if (opt_verbose)
			printf("         %-5u held in the wait it never left in the recording\n", ns);
		t->held_id = req->id, t->held_lfd = lfd;
		t->held_nr = req->data.nr;
		t->past_end = 1;
		if (remaining == 0)
			end_reached(tgid_of(req->pid));
		return 1;
	}
	if (timing && !e && remaining > 0) {
		/* A wait runs for real too: holding it would keep another thread waiting
		 * for this one (pthread_cond_destroy does).  It can do no harm: anything
		 * else this thread calls is held below. */
		t->past_end = 1;
		return n_slack++, respond(req->id, 1, 0), 1;
	}
	if (timing && e && !same_call(e, req)) {        /* not in the recording: it still runs */
		n_slack++;
		if (opt_verbose) {
			fmt_call(b, sizeof(b), req->data.nr, (const u64 *)req->data.args, 3);
			printf("         %-5u real %s (not in the recording: timing only)\n", ns, b);
		}
		respond(req->id, 1, 0);
		/* (a real sleep: the others go on, it may be polling for their work) */
		if (opt_serial)
			t->running = !blocks(req) && req->data.nr != SYS_nanosleep &&
				     req->data.nr != SYS_clock_nanosleep;
		if (opt_serial && req->data.nr == SYS_futex && blocks(req))
			t->lock_wait = 1, t->wait_addr = req->data.args[0];
		return 1;
	}
	if (!e) {
		fmt_call(b, sizeof(b), req->data.nr, (const u64 *)req->data.args, nargs(req->data.nr));
		printf("   thread %u reached the end of its recording; next it calls %s\n", ns, b);
		t->held_id = req->id, t->held_lfd = lfd;
		t->held_nr = req->data.nr;
		t->past_end = 1;
		if (remaining == 0)
			end_reached(tgid_of(req->pid));
		return 1;
	}
	if (!relax_order) {
		if (opt_strict && !global_next_seq_is(e))
			return 0;               /* not its turn yet */
		if (opt_serial && (running_thread(t) || (t != jump_thr && !next_exit_is(e))))
			return 0;               /* someone else runs, or not its turn */
	}

	if ((long)e->s->nr != req->data.nr) {
		diverge(t, e, req, "different syscall");
		history(t);
		state = DIVERGED;
		t->held_id = req->id, t->held_lfd = lfd;
		finish("diverged");
		return 1;
	}

	/* a new mapping must land where it landed in the recording */
	if (t->pinned != e && needs_pin(req, e)) {
		if (pin_address(req, e) == 0) {
			t->pinned = e;
			n_pin++;
			if (opt_verbose)
				printf("         #%llu %s re-issued at the recorded address %#llx\n",
				       (unsigned long long)e->s->h.seq, sysname(e->s->nr),
				       (unsigned long long)e->s->ret);
			return 1;               /* it comes back, pinned */
		}
		n_argwarn++;
		printf("   warning: #%llu %s: could not pin the address\n",
		       (unsigned long long)e->s->h.seq, sysname(e->s->nr));
	}

	/* arguments: pointers too, the address space is the restored one */
	int na = t->pinned == e ? 0 : nargs(req->data.nr);
	int fresh = na && has_execed(tgid_of(req->pid));
	for (int i = 0; i < na; i++)
		if (e->s->args[i] != req->data.args[i]) {
			/* after an execve of its own the process is laid out at random
			 * again: addresses differ from the recording, and that is fine */
			if (fresh && e->s->args[i] >= 0x10000 && req->data.args[i] >= 0x10000)
				continue;
			n_argwarn++;
			printf("   warning: #%llu %s arg%d is %#llx, recorded %#llx\n",
			       (unsigned long long)e->s->h.seq, sysname(e->s->nr), i,
			       (unsigned long long)req->data.args[i], (unsigned long long)e->s->args[i]);
		}

	if (check_inputs(t, e, req)) {
		n_inwarn++;
		if (!opt_cont) {
			history(t);
			state = DIVERGED;
			t->held_id = req->id, t->held_lfd = lfd;
			finish("diverged");
			return 1;
		}
	}

	int cont = runs_real(req, e);
	if (!cont && inject_outputs(e, req)) {
		diverge(t, e, req, "output buffer not writable");
		state = DIVERGED;
		t->held_id = req->id, t->held_lfd = lfd;
		finish("diverged");
		return 1;
	}
	/* what the target hands over must be read while it is still parked */
	if (!cont)
		capture_write(req, e->s->ret);
	if (!cont && !opt_nofd && req->data.nr == SYS_recvmsg && e->s->ret >= 0)
		install_scm_fds(e, req);
	if (cont && is_clone(req->data.nr) && e->s->ret > 0)
		set_next_pid(e->s->ret);
	/* a faked call that created fds: install real ones (answers it too) */
	int fk = cont || opt_nofd || e->s->ret < 0 ? FD_NONE : fd_kind(req->data.nr,
								     (const u64 *)req->data.args);
	int fr = fk == FD_NONE ? 1 : install_fds(e, req, fk);
	if (fr < 0)
		return 1;                       /* interrupted: it will come again */
	/* a write to a pipe or socket nobody reads: the kernel sends SIGPIPE too */
	int piped = fr > 0 && !cont && e->s->ret == -EPIPE && sends_sigpipe(req);
	if (piped && return_with_signal(req, e->s->ret, SIGPIPE) == 0) {
		n_sig++;
	} else if (fr > 0) {
		if (respond(req->id, cont, e->s->ret))
			return 1;
		if (piped) {            /* second best: just after the call returns */
			syscall(SYS_tgkill, tgid_of(req->pid), req->pid, SIGPIPE);
			n_sig++;
		}
	}
	if (opt_serial)
		t->running = !blocks(req) || (!cont && req->data.nr == SYS_futex);

	if (is_clone(req->data.nr) && e->s->ret > 0)
		expect_child(e->s->ret);
	if (cont && (req->data.nr == SYS_execve || req->data.nr == SYS_execveat))
		note_exec(tgid_of(req->pid));
	cont ? n_pass++ : n_fake++;
	if (opt_verbose) {
		fmt_call(b, sizeof(b), e->s->nr, e->s->args, nargs(e->s->nr));
		printf("#%-7llu %-5u %s %s = %lld\n", (unsigned long long)e->s->h.seq, t->ns_tid,
		       cont ? "run " : "fake", b, (long long)e->s->ret);
	}
	consume(e);
	return 1;
}

static void on_int(int sig) { (void)sig; interrupted = 1; }


#define NO_PROGRESS_SECS 20
#define MAX_RELAX 8                 /* each takes 2 s: more would look like a hang */

/* Nothing has moved for a while: say who everyone is waiting for. */
static void explain_stall(int nparked)
{
	const struct ent *want = NULL;
	char b[200];

	if (opt_serial) {
		while (xnext < nents && byexit[xnext]->done)
			xnext++;
		want = xnext < nents ? byexit[xnext] : NULL;
	} else {
		while (gnext < nents && ents[gnext].done)
			gnext++;
		want = gnext < nents ? &ents[gnext] : NULL;
	}
	printf("   stalled: %d thread(s) parked, waiting for their turn\n", nparked);
	if (want) {
		fmt_call(b, sizeof(b), want->s->nr, want->s->args, nargs(want->s->nr));
		printf("   next in the recording: thread %u, record #%llu %s = %lld\n", want->s->ns_tid,
		       (unsigned long long)want->s->h.seq, b, (long long)want->s->ret);
	}
	for (size_t i = 0; i < nthrs; i++) {
		struct ent *e = next_ent(&thrs[i]);
		if (thrs[i].running)
			printf("   thread %u has been answered and has not come back\n", thrs[i].ns_tid);
		if (e && e == want)
			printf("   thread %u owes that record%s\n", thrs[i].ns_tid,
			       thrs[i].seen ? "; it is blocked in a real syscall or in user code" :
					      "; it has not started yet");
	}
}

/* The target is gone, but the live run went on to exit normally. */
static void report_early_end(void)
{
	char b[200];

	if (live_death())
		return;
	for (size_t i = 0; i < nents; i++)
		if (!ents[i].done && ents[i].s->nr == SYS_exit_group) {
			fmt_call(b, sizeof(b), ents[i].s->nr, ents[i].s->args, nargs(ents[i].s->nr));
			printf("   !! DIVERGENCE: the target ended before thread %u's recorded %s: the "
			       "live run exited normally, this one did not (a crash, most likely; a "
			       "race in user space can go the other way)\n", ents[i].s->ns_tid, b);
			state = DIVERGED;
			return;
		}
}

static struct { struct seccomp_notif req; int lfd; } parked[4096];
static int nparked;

/* -T.  The thread whose turn it is sleeps in a lock wait that is not in the
 * recording: the lock is held by a thread parked at a syscall behind it.  Live,
 * that thread came back from its syscall and released the lock in time; the end
 * of its stretch came later, which is why it is ordered later.  Let a parked
 * thread go first if its recorded call had returned before the waiting thread's
 * stretch ended (their stretches overlapped, so this order is as true to the log
 * as the other).  Returns the thread to let go, or NULL. */
static struct thr *lock_holder_first(void)
{
	while (xnext < nents && byexit[xnext]->done)
		xnext++;
	if (!opt_serial || state != RUN || xnext >= nents || running_thread(NULL))
		return NULL;
	const struct ent *want = byexit[xnext];
	struct thr *owner = thr_of(want->s->ns_tid), *best = NULL;
	u64 best_exit = 0;

	if (!owner || !owner->lock_wait)
		return NULL;
	/* the wake it is waiting for comes first, whenever it was recorded */
	for (int j = 0; j < nparked; j++)
		if (parked[j].req.data.nr == SYS_futex && parked[j].req.data.args[0] == owner->wait_addr &&
		    !futex_waits(parked[j].req.data.args[1]))
			return thread_for(ns_tid_of(parked[j].req.pid));
	for (size_t i = 0; i < nthrs; i++) {
		struct thr *t = &thrs[i];
		const struct ent *e = t != owner && t->seen && !t->lock_wait ? next_ent(t) : NULL;
		if (!e || !e->s->ts_exit || e->s->ts_exit >= want->key)
			continue;
		if (!best || e->s->ts_exit < best_exit)
			best = t, best_exit = e->s->ts_exit;
	}
	return best;
}

static void supervise(void)
{
	struct pollfd p[256];
	int stalled = 0, open = 0, idle_at_end = 0;

	for (int i = 0; i < nprocs; i++)
		open += procs[i].lfd >= 0;
	size_t seen_remaining = remaining;
	time_t last_progress = time(NULL), last_quiet = 0;
	for (;;) {
		/* Threads can keep busy without getting anywhere: one spins on a lock
		 * whose holder is parked behind it in the recorded order.  The order
		 * in which threads won a contended lock is not in the log. */
		if (remaining != seen_remaining) {
			seen_remaining = remaining;
			last_progress = time(NULL);
		} else if (state == RUN && time(NULL) - last_progress >= NO_PROGRESS_SECS) {
			printf("\n!! DIVERGENCE: no recorded syscall was used for %d s\n", NO_PROGRESS_SECS);
			explain_stall(nparked);
			state = DIVERGED;
			finish("diverged: no progress");
		}
		/* rebuilt each time: processes that appear during the replay extend
		 * procs[] (they share a listener: lfd -1, which poll skips) */
		int n = nprocs;
		for (int i = 0; i < n; i++)
			p[i] = (struct pollfd){ .fd = procs[i].lfd, .events = POLLIN };
		if (state == RUN)
			start_threads(0);
		int r = poll(p, n, 2000);

		if (interrupted)
			return;
		if (r < 0 && errno == EINTR)
			continue;
		/* Quiet: no notification, or a thread polling (sleep, look, sleep) while
		 * no record gets used. */
		time_t now = time(NULL);
		int quiet = r == 0 || (nparked && state == RUN && now - last_progress >= 2 &&
				       now - last_quiet >= 2);
		if (!quiet)
			idle_at_end = 0;
		if (quiet) {
			last_quiet = now;
			struct thr *rt = opt_serial ? running_thread(NULL) : NULL;
			if (nparked && state == RUN && rt) {
				/* -T: the running thread did not reach a syscall in 2 s
				 * (blocked in a real syscall we did not expect to block):
				 * stop waiting for it, or everyone waits forever */
				printf("   serial order: thread %u has not come back for 2 s; "
				       "not waiting for it any more\n", rt->ns_tid);
				rt->running = 0;
				for (int j = 0; j < nparked; ) {
					lfd = parked[j].lfd;
					if (ioctl(lfd, SECCOMP_IOCTL_NOTIF_ID_VALID, &parked[j].req.id) ||
					    handle(&parked[j].req)) {
						parked[j] = parked[--nparked];
						j = 0;
					} else {
						j++;
					}
				}
			} else if (nparked && state == RUN && n_relaxed >= MAX_RELAX) {
				/* no use: the no-progress check below ends it */
			} else if (nparked && state == RUN) {
				/* Nobody moved for 2 s: the thread whose turn it is waits for
				 * a lock held by a thread parked behind it.  The order in which
				 * threads won a contended lock is not in the log, so the
				 * recorded order can't always be followed.  Let one go. */
				if (!stalled++)
					explain_stall(nparked);
				relax_order = 1;
				int released = 0;
				for (int j = 0; j < nparked && !released; ) {
					lfd = parked[j].lfd;
					if (ioctl(lfd, SECCOMP_IOCTL_NOTIF_ID_VALID, &parked[j].req.id)) {
						parked[j] = parked[--nparked];
						continue;
					}
					released = handle(&parked[j].req);
					if (released)
						parked[j] = parked[--nparked];
					else
						j++;
				}
				relax_order = 0;
				if (!released)
					released = start_threads(1);
				if (released) {
					n_relaxed++;
					printf("   order relaxed: let a parked thread go ahead of its turn\n");
				}
			}
			/* The last usable record was used and nobody calls anything more:
			 * threads past the end sit in a real wait, or in a hold. */
			if (state == RUN && only_unstarted_left() && ++idle_at_end >= 3)
				for (size_t i = 0; i < nthrs; i++)
					if (thrs[i].past_end) {
						end_reached(procs[0].tgid);
						break;
					}
			continue;
		}
		for (int i = 0; i < n; i++) {
			if (!p[i].revents)
				continue;
			if (p[i].revents & (POLLHUP | POLLERR)) {
				/* every process using this filter is gone */
				procs[i].lfd = -1;
				if (--open == 0) {
					if (state == RUN) {
						state = ENDED;
						printf("\n== the target exited; %zu records were not used ==\n", remaining);
						print_totals();
						report_live_end();
						report_early_end();
					} else
						printf("   target exited\n");
					return;
				}
				continue;
			}
			struct seccomp_notif req = {};
			lfd = procs[i].lfd;
			if (ioctl(lfd, SECCOMP_IOCTL_NOTIF_RECV, &req)) {
				if (errno == EINTR || errno == ENOENT)
					continue;
				die("NOTIF_RECV: %s", strerror(errno));
			}
			if (handle(&req))
				stalled = 0;
			else if (nparked < 4096)
				parked[nparked].req = req, parked[nparked++].lfd = lfd;
			else
				die("too many threads waiting for their turn");

			/* strict mode: did that unblock a parked thread? */
			for (int j = 0; j < nparked; ) {
				lfd = parked[j].lfd;
				if (ioctl(lfd, SECCOMP_IOCTL_NOTIF_ID_VALID, &parked[j].req.id) ||
				    handle(&parked[j].req)) {
					parked[j] = parked[--nparked];
					j = 0;
				} else {
					j++;
				}
			}
			while ((jump_thr = lock_holder_first())) {
				int went = 0;
				for (int j = 0; j < nparked && !went; j++) {
					if (thread_for(ns_tid_of(parked[j].req.pid)) != jump_thr)
						continue;
					lfd = parked[j].lfd;
					if (ioctl(lfd, SECCOMP_IOCTL_NOTIF_ID_VALID, &parked[j].req.id) ||
					    handle(&parked[j].req)) {
						parked[j] = parked[--nparked];
						went = 1;
					}
				}
				if (!went)
					break;
				n_jump++;
				if (opt_verbose)
					printf("         %-5u went ahead: a thread whose turn it was waits "
					       "for a lock\n", jump_thr->ns_tid);
				if (running_thread(NULL))
					break;
			}
			jump_thr = NULL;
		}
	}
}

int main(int argc, char **argv)
{
	int gen = -1, c;
	pid_t pid = 0;

	while ((c = getopt(argc, argv, "p:g:e:STcvNw:")) != -1) {
		switch (c) {
		case 'p': pid = atoi(optarg); break;
		case 'g': gen = atoi(optarg); break;
		case 'e':
			if (!strcmp(optarg, "stop")) opt_end = END_STOP;
			else if (!strcmp(optarg, "live")) opt_end = END_LIVE;
			else if (!strcmp(optarg, "kill")) opt_end = END_KILL;
			else die("-e takes stop, live or kill");
			break;
		case 'S': opt_strict = 1; break;
		case 'T': opt_serial = 1; break;
		case 'c': opt_cont = 1; break;
		case 'v': opt_verbose = 1; break;
		case 'N': opt_nofd = 1; break;
		case 'w':
			if (!(wfile = fopen(optarg, "w")))
				die("%s: %s", optarg, strerror(errno));
			break;
		default: goto usage;
		}
	}
	if (!pid || optind != argc - 1) {
usage:
		fprintf(stderr, "usage: %s -p PID [-g GEN] [-e stop|live|kill] [-S|-T] [-c] [-v] "
			"[-N] [-w FILE] rec.log\n", argv[0]);
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	tgid = pid;

	load(argv[optind], &gen);
	if (!nents)
		die("generation %d has no syscalls in %s", gen, argv[optind]);
	order_serial();
	printf("replaying generation %d: %zu syscalls in %zu thread(s), recorded from %s\n",
	       gen, remaining, nthrs, fh->comm);

	/* is it the restored copy of the recorded process, and is it stopped? */
	char path[64], line[256], st = '?';
	snprintf(path, sizeof(path), "/proc/%d/status", pid);
	FILE *f = fopen(path, "r");
	if (!f)
		die("no process %d", pid);
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, "State:", 6))
			sscanf(line + 6, " %c", &st);
	fclose(f);
	if (st != 'T')
		fprintf(stderr, "warning: %d is not stopped (state %c): it may already have made "
			"syscalls the replay will not see\n", pid, st);
	if (ns_tid_of(pid) != fh->target_ns_tgid)
		fprintf(stderr, "warning: %d has pid %u in its namespace, the recorded process "
			"had %u\n", pid, ns_tid_of(pid), fh->target_ns_tgid);

	/* the root directory, for fd installation (shared by the container) */
	snprintf(path, sizeof(path), "/proc/%d/root", pid);
	troot = open(path, O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (troot < 0)
		die("%s: %s", path, strerror(errno));
	snprintf(path, sizeof(path), "/proc/%d/ns/pid", pid);
	pidns_fd = open(path, O_RDONLY | O_CLOEXEC);

	/* the process tree under PID (all stopped by CRIU): PID first, then
	 * every process whose parent is already in the list */
	procs[nprocs++] = (struct proc){ .tgid = pid };
	for (int grew = 1; grew; ) {
		grew = 0;
		DIR *pd = opendir("/proc");
		for (struct dirent *de; pd && (de = readdir(pd)); ) {
			pid_t c = atoi(de->d_name), pp = -1;
			int known = 0;
			char pst = '?';
			if (c <= 0)
				continue;
			for (int i = 0; i < nprocs; i++)
				known |= procs[i].tgid == c;
			snprintf(path, sizeof(path), "/proc/%d/stat", c);
			FILE *sf = fopen(path, "r");
			char sb[512], *q;
			if (sf && fgets(sb, sizeof(sb), sf) && (q = strrchr(sb, ')')))
				sscanf(q + 2, "%c %d", &pst, &pp);
			if (sf)
				fclose(sf);
			/* a child that had exited but was not waited for yet comes back
			 * as a zombie: it runs nothing, and ptrace refuses it */
			if (pst == 'Z')
				continue;
			for (int i = 0; !known && i < nprocs && nprocs < 256; i++)
				if (procs[i].tgid == pp) {
					procs[nprocs++] = (struct proc){ .tgid = c };
					grew = known = 1;
				}
		}
		if (pd)
			closedir(pd);
	}

	remember_stdio(pid);
	snapshot_maps(pid);

	/* a listener per process, before any of them runs again */
	for (int i = 0; i < nprocs; i++) {
		procs[i].pidfd = syscall(SYS_pidfd_open, procs[i].tgid, 0);
		if (procs[i].pidfd < 0)
			die("pidfd_open(%d): %s", procs[i].tgid, strerror(errno));
		procs[i].lfd = attach_listener(procs[i].tgid);
	}

	/* Threads that exist now but made no syscall in this generation: in
	 * the recording they stayed blocked in one syscall the whole time (a
	 * syscall that never returns is never recorded).  Give them an empty
	 * queue, so their first syscall is held, i.e. they stay blocked.  Only
	 * threads created during the replay are "not in the recording". */
	for (int i = 0; i < nprocs; i++) {
		snprintf(path, sizeof(path), "/proc/%d/task", procs[i].tgid);
		DIR *td = opendir(path);
		for (struct dirent *de; td && (de = readdir(td)); ) {
			u32 ns = de->d_name[0] != '.' ? ns_tid_of(atoi(de->d_name)) : 0;
			if (!ns || thr_of(ns))
				continue;
			thrs = realloc(thrs, (nthrs + 1) * sizeof(*thrs));
			thrs[nthrs++] = (struct thr){ .ns_tid = ns };
			printf("thread %u has no syscall in generation %d: blocked all along in the "
			       "recording, it will be held\n", ns, gen);
		}
		if (td)
			closedir(td);
	}
	if (opt_serial)
		for (int i = 0; i < nprocs; i++) {
			snprintf(path, sizeof(path), "/proc/%d/task", procs[i].tgid);
			DIR *td = opendir(path);
			for (struct dirent *de; td && (de = readdir(td)); ) {
				struct thr *t = de->d_name[0] != '.' ? thr_of(ns_tid_of(atoi(de->d_name))) : NULL;
				if (t)
					hold_thread(t, atoi(de->d_name));
			}
			if (td)
				closedir(td);
		}
	printf("seccomp listener attached to %d process(es):", nprocs);
	for (int i = 0; i < nprocs; i++)
		printf(" %d", procs[i].tgid);
	printf("; resuming\n");

	signal(SIGINT, on_int);
	signal(SIGTERM, on_int);
	kill_all(SIGCONT);
	supervise();
	if (state == RUN)
		finish("interrupted");
	return state == DIVERGED;
}
