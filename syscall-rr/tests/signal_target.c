/* A timer signal interrupts the program 30 times a second; its handler changes
 * what the program prints.  The replay disarms the restored timer and delivers
 * each recorded signal at the syscall it interrupted (REPLAY.md, "Signals from
 * the log").  Until 2026-11-12 signals were not replayed and this target could
 * only check that the failure was reported.
 */
#include <signal.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t ticks;

static void on_alarm(int sig)
{
	(void)sig;
	ticks++;
}

int main(void)
{
	struct sigaction sa = { .sa_handler = on_alarm, .sa_flags = SA_RESTART };
	struct itimerval t = { { 0, 30000 }, { 0, 30000 } };
	struct timespec d = { 0, 10 * 1000 * 1000 };

	sigaction(SIGALRM, &sa, NULL);
	setitimer(ITIMER_REAL, &t, NULL);
	for (unsigned long i = 0;; i++) {
		char line[64];
		int n = snprintf(line, sizeof(line), "iter %lu ticks %d\n", i, (int)ticks);
		write(1, line, n);
		nanosleep(&d, NULL);
	}
}
