/* A catalogue of ways to die.  The first argument picks one.
 *
 * Every mode runs the same normal life first: read random bytes, print a line
 * that depends on them, sleep.  About 3.2 s after the start (after the
 * checkpoint) the bug fires, and it is driven by recorded data wherever that
 * makes sense: a fault address or an overflow length comes from the random
 * bytes, so a replay that got wrong data would die differently or not at all.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int rnd;
static uint64_t acc = 1469598103934665603ULL;

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_REALTIME, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static uint64_t rnd64(void)
{
	uint64_t r = 0;
	if (read(rnd, &r, sizeof(r)) != sizeof(r))
		_exit(99);
	acc = (acc ^ r) * 1099511628211ULL;
	return r;
}

static void say(const char *what)
{
	char line[128];
	int n = snprintf(line, sizeof(line), "%s acc %016llx\n", what, (unsigned long long)acc);
	if (write(1, line, n) < 0)
		_exit(98);
}

static void live_until(double t)
{
	struct timespec d = { 0, 20 * 1000 * 1000 };
	for (unsigned long i = 0; now() < t; i++) {
		char b[32];
		rnd64();
		snprintf(b, sizeof(b), "iter %lu", i);
		say(b);
		nanosleep(&d, NULL);
	}
}

/* ---- faults and aborts: the instruction itself fails ---- */

static void __attribute__((noinline)) smash_stack(size_t n)
{
	char b[16];
	memset(b, 'A', n);
	__asm__ volatile("" : : : "memory");
	if (b[0] == 'A')
		say("smashed");
}

static int recurse(int n)
{
	volatile char pad[4096];
	pad[0] = n;
	return recurse(n + 1) + pad[0];
}

static void m_null(void)    { say("about to fault"); *(volatile int *)0 = (int)rnd64(); }
static void m_wild(void)    { uint64_t a = rnd64() | (1ULL << 63); say("wild"); *(volatile int *)a = 1; }
static void m_stack(void)   { smash_stack(64 + rnd64() % 64); }
static void m_fpe(void)     { volatile int z = 0; say("dividing"); volatile int x = (int)rnd64() / z; (void)x; }
static void m_ill(void)     { say("illegal"); __asm__("ud2"); }
static void m_recurse(void) { say("recursing"); recurse(0); }
static void m_abort(void)   { say("assert"); assert(acc == 0); }
static void m_exit3(void)   { say("giving up"); exit(3); }

static void m_heap(void)
{
	/* big chunks come from the top of the heap, so they are neighbours;
	 * volatile: the compiler may not remove a malloc/free pair it can see */
	char *volatile a = malloc(2048), *volatile b = malloc(2048), *volatile guard = malloc(2048);
	(void)guard;
	while (b != a + 2064) {         /* neighbours in memory, chunk header between */
		a = b;
		b = malloc(2048);
	}
	memset(a, 0, 2048 + 16 + rnd64() % 8);          /* b's chunk size becomes 0 */
	__asm__ volatile("" : : "r"(a) : "memory");     /* or the compiler drops the memset */
	say("freeing a corrupted chunk");
	free(b);
	exit(4);
}

static void m_double(void)
{
	char *volatile a = malloc(32);
	rnd64();
	free(a);
	say("freed once");
	free(a);
	exit(4);
}

/* ---- the outside world answers badly ---- */

static void m_badinput(void)
{
	/* the "config" has vanished: open fails in the log, and the file really
	 * exists in the restored container, so only the log can explain the crash */
	FILE *f = fopen("/tmp/config", "r");
	char line[64] = "";
	if (f && fgets(line, sizeof(line), f) && !strcmp(line, "ok\n")) {
		say("config ok");
		exit(0);
	}
	say("bad config");
	*(volatile int *)0x10 = 1;
}

/* The target is PID 1 of its container, and the kernel does not deliver a signal
 * with the default action to PID 1 of a namespace, except from outside it.  So
 * these run in a child, and PID 1 passes the result on. */
static void in_child(void (*fn)(void))
{
	int st = 0;
	pid_t c = fork();
	if (c == 0) {
		fn();
		_exit(42);
	}
	waitpid(c, &st, 0);
	_exit(WIFSIGNALED(st) ? 128 + WTERMSIG(st) : WEXITSTATUS(st));
}

static void sigpipe_body(void)
{
	int p[2];
	signal(SIGPIPE, SIG_DFL);       /* podman starts containers with it ignored */
	if (pipe(p))
		exit(5);
	close(p[0]);
	say("writing to a closed pipe");
	if (write(p[1], "x", 1) < 0)
		exit(6);        /* not reached: SIGPIPE kills first */
	exit(7);
}
static void m_sigpipe(void) { in_child(sigpipe_body); }

static void m_ebadwrite(void)
{
	/* a disk-full error: recorded, then the program gives up */
	int fd = open("/dev/full", O_WRONLY);
	char b[8] = "abcdefg";
	if (write(fd, b, sizeof(b)) < 0 && errno == ENOSPC) {
		say("disk full");
		abort();
	}
	exit(8);
}

static void m_sigbus(void)
{
	/* the file shrinks under a mapping: touching it is SIGBUS */
	int fd = open("/tmp/bus", O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (ftruncate(fd, 8192))
		exit(9);
	volatile char *m = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	m[0] = 1;
	if (ftruncate(fd, 0))
		exit(9);
	say("touching a truncated file");
	m[4096] = 1;
	exit(10);
}

static void m_oom(void)
{
	/* out of memory: the limit is set, so the allocation fails */
	struct rlimit r = { 64 << 20, 64 << 20 };
	setrlimit(RLIMIT_AS, &r);
	say("limit set");
	void *p = malloc(1 << 30);
	if (!p) {
		say("out of memory");
		*(volatile int *)0 = 1;
	}
	exit(11);
}

/* ---- killed by a signal that nothing in the program raises ---- */

static void alarm_body(void)
{
	sigset_t s;
	signal(SIGALRM, SIG_DFL);
	sigemptyset(&s);
	sigaddset(&s, SIGALRM);
	sigprocmask(SIG_UNBLOCK, &s, NULL);
	say("watchdog armed");
	alarm(1);
	live_until(now() + 5);
}
static void m_alarm(void) { in_child(alarm_body); }
static void m_kill9(void)   { say("waiting to be killed"); live_until(now() + 3); exit(12); }

/* ---- a handler sees the crash first, like redis and most servers ---- */

static void on_segv(int sig)
{
	static const char m[] = "crash handler: dumping state\n";
	if (write(2, m, sizeof(m) - 1) < 0)
		_exit(97);
	signal(sig, SIG_DFL);
	raise(sig);
}

static void m_handler(void)
{
	signal(SIGSEGV, on_segv);
	say("fault with a handler");
	*(volatile int *)0 = 1;
}

/* ---- another thread, another process ---- */

static void *faulting_thread(void *a)
{
	(void)a;
	rnd64();
	say("worker faults");
	*(volatile int *)0x20 = 1;
	return NULL;
}

static void m_thread(void)
{
	pthread_t t;
	pthread_create(&t, NULL, faulting_thread, NULL);
	live_until(now() + 5);
}

static void m_child(void)
{
	/* the child crashes; the parent notices, and aborts on purpose */
	int st = 0;
	pid_t c = fork();
	if (c == 0) {
		rnd64();
		*(volatile int *)0x30 = 1;
		_exit(0);
	}
	waitpid(c, &st, 0);
	if (WIFSIGNALED(st)) {
		char b[64];
		snprintf(b, sizeof(b), "child died of signal %d", WTERMSIG(st));
		say(b);
		abort();
	}
	exit(13);
}

/* ---- deadlock: no crash, the program just stops ---- */

static pthread_mutex_t A = PTHREAD_MUTEX_INITIALIZER, B = PTHREAD_MUTEX_INITIALIZER;

static void *lock_ba(void *x)
{
	struct timespec d = { 0, 50 * 1000 * 1000 };
	(void)x;
	pthread_mutex_lock(&B);
	nanosleep(&d, NULL);
	pthread_mutex_lock(&A);
	return NULL;
}

static void m_deadlock(void)
{
	pthread_t t;
	struct timespec d = { 0, 50 * 1000 * 1000 };
	pthread_mutex_lock(&A);
	pthread_create(&t, NULL, lock_ba, NULL);
	nanosleep(&d, NULL);
	say("about to deadlock");
	pthread_mutex_lock(&B);
	exit(14);
}

/* ---- races ---- */

static char *volatile shared;
static volatile int stop_race;

/* A: a syscall sits inside the window.  The reader checks the pointer, makes a
 * syscall, then uses it; the other thread frees it during that syscall. */
static void *freer_syscall(void *x)
{
	(void)x;
	for (;;) {
		rnd64();
		char *p = shared;
		if (p) {
			shared = NULL;
			free(p);
			p = NULL;
		}
		usleep(1500);
		char *n = malloc(32);
		memset(n, 1, 32);
		shared = n;
		usleep(1500);
	}
	return NULL;
}

static void m_race_syscall(void)
{
	pthread_t t;
	shared = malloc(32);
	pthread_create(&t, NULL, freer_syscall, NULL);
	for (;;) {
		char *p = shared;
		if (p) {
			char b[16];
			if (read(rnd, b, sizeof(b)) != sizeof(b))    /* the window */
				_exit(96);
			usleep(1000);
			if (!shared)                       /* it was freed: fault on purpose */
				*(volatile char *)(p - p + 0x40) = 1;
		}
		usleep(300);
	}
}

/* B: nothing but user code in the window: a torn update of two fields.  Between
 * syscalls, one thread writes both fields and the other checks they agree. */
static volatile uint64_t f1, f2;

static void *writer_nosys(void *x)
{
	(void)x;
	for (uint64_t i = 1;; i++) {
		f1 = i;
		for (volatile int k = 0; k < 20; k++)
			;
		f2 = i;
		if (i % 256 == 0)
			rnd64();
	}
	return NULL;
}

static void m_race_memory(void)
{
	pthread_t t;
	pthread_create(&t, NULL, writer_nosys, NULL);
	for (;;) {
		uint64_t a = f2, b = f1;
		/* only after the writer has been at it for a while: the tear is then a
		 * rare event, with values no replay can get right by luck */
		if (a != b && b > 100000 && b - a > 0) {
			char l[64];
			snprintf(l, sizeof(l), "torn %llu %llu", (unsigned long long)a, (unsigned long long)b);
			say(l);
			*(volatile int *)0x50 = 1;
		}
	}
}

struct mode { const char *name; void (*fn)(void); };
static const struct mode modes[] = {
	{ "null", m_null }, { "wild", m_wild }, { "stack", m_stack }, { "fpe", m_fpe }, { "ill", m_ill },
	{ "recurse", m_recurse }, { "abort", m_abort }, { "exit3", m_exit3 }, { "heap", m_heap },
	{ "double", m_double }, { "badinput", m_badinput }, { "sigpipe", m_sigpipe },
	{ "ebadwrite", m_ebadwrite }, { "sigbus", m_sigbus }, { "oom", m_oom }, { "alarm", m_alarm },
	{ "kill9", m_kill9 }, { "handler", m_handler }, { "thread", m_thread }, { "child", m_child },
	{ "deadlock", m_deadlock }, { "race-syscall", m_race_syscall }, { "race-memory", m_race_memory },
};

int main(int argc, char **argv)
{
	double t0 = now();

	rnd = open("/dev/urandom", O_RDONLY);
	if (argc < 2)
		return 2;
	if (!strcmp(argv[1], "badinput")) {
		/* present until just after the checkpoint, then gone */
		int fd = open("/tmp/config", O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (write(fd, "ok\n", 3) != 3)
			return 2;
		close(fd);
	}
	live_until(t0 + 3.2);
	if (!strcmp(argv[1], "badinput"))
		unlink("/tmp/config");
	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
		if (!strcmp(argv[1], modes[i].name)) {
			modes[i].fn();
			return 42;              /* the bug did not fire */
		}
	return 2;
}
