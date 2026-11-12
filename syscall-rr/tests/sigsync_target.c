/* sigsync_target: signals a program causes itself, at an exact point.
 *
 *   raise(SIGUSR1)            handler runs before raise returns
 *   a write to a closed pipe  EPIPE and SIGPIPE, handler runs
 *   a bad pointer             SIGSEGV, handler jumps back (siglongjmp)
 *   sigqueue to itself        handler reads the value sent along
 *
 * Unlike a signal from outside (tests/signal_target.c) these do not depend on
 * timing, so a replay has no excuse: each line counts the handlers that ran.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile int usr1, pipes, segvs, rtval;
static sigjmp_buf jb;
static volatile uintptr_t fault_addr;

static void on_usr1(int s) { usr1++; }
static void on_pipe(int s) { pipes++; }
static void on_segv(int s, siginfo_t *si, void *u)
{
	segvs++;
	fault_addr = (uintptr_t)si->si_addr;
	siglongjmp(jb, 1);
}
static void on_rt(int s, siginfo_t *si, void *u) { rtval += si->si_value.sival_int; }

int main(void)
{
	static char alt[65536];
	stack_t ss = { .ss_sp = alt, .ss_size = sizeof(alt) };
	struct sigaction sa = { .sa_flags = SA_SIGINFO | SA_ONSTACK };
	int rnd = open("/dev/urandom", O_RDONLY);

	sigaltstack(&ss, NULL);
	signal(SIGUSR1, on_usr1);
	signal(SIGPIPE, on_pipe);
	sa.sa_sigaction = on_segv;
	sigaction(SIGSEGV, &sa, NULL);
	sa.sa_sigaction = on_rt;
	sigaction(SIGRTMIN + 1, &sa, NULL);

	for (unsigned long iter = 0;; iter++) {
		unsigned char b[4];
		int p[2];

		read(rnd, b, sizeof(b));
		for (int i = 0; i < 1 + b[0] % 3; i++)
			raise(SIGUSR1);

		pipe(p);
		close(p[0]);
		ssize_t w = write(p[1], "x", 1);
		int werr = w < 0 ? errno : 0;
		close(p[1]);

		if (!sigsetjmp(jb, 1))
			*(volatile char *)(uintptr_t)(0x1000 + b[1] * 16) = 1;

		sigqueue(getpid(), SIGRTMIN + 1, (union sigval){ .sival_int = b[2] });

		/* blocked, then taken synchronously: no handler */
		sigset_t m, old;
		siginfo_t si = { 0 };
		sigemptyset(&m);
		sigaddset(&m, SIGUSR2);
		sigprocmask(SIG_BLOCK, &m, &old);
		sigqueue(getpid(), SIGUSR2, (union sigval){ .sival_int = b[3] });
		int got = sigwaitinfo(&m, &si);
		sigprocmask(SIG_SETMASK, &old, NULL);

		char line[200];
		int n = snprintf(line, sizeof(line),
				 "iter %lu usr1 %d epipe %d pipes %d segvs %d at %lx rt %d waited %d value %d\n",
				 iter, usr1, werr == EPIPE, pipes, segvs, (unsigned long)fault_addr, rtval,
				 got, si.si_value.sival_int);
		write(1, line, n);
		struct timespec d = { 0, 100 * 1000 * 1000 };
		nanosleep(&d, NULL);
	}
}
