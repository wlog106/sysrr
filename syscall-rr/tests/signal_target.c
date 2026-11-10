/* A timer signal interrupts the program; its handler changes what the program
 * prints.  Signals are not replayed (REPLAY.md, limitations), so this replay
 * cannot succeed.  What this target checks is how it fails: the supervisor
 * must report a divergence, not hang and not claim success.
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
