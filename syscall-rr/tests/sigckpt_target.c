/* sigckpt_target: a signal that lands while CRIU has the process frozen.
 *
 * The thread computes without syscalls for a while, then prints a line.  The
 * test sends SIGUSR1 from a CRIU action script at post-dump: CRIU has saved the
 * pending signals already, so the signal is not in the checkpoint; live, the
 * kernel delivers it the moment the process goes on, at the instruction where
 * the checkpoint caught it.  The handler notes how far the loop had got, and the
 * line prints it: a replay matches only if the signal comes back at that same
 * instruction, not at the next syscall.  Run with CKPT_KICK=USR1.
 */
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static volatile unsigned long n;
static volatile unsigned long at[8];
static volatile int nat;

static void on_usr1(int s)
{
	(void)s;
	if (nat < 8)
		at[nat++] = n;
}

int main(void)
{
	struct sigaction a = { .sa_handler = on_usr1, .sa_flags = SA_RESTART };
	sigset_t usr1;

	sigaction(SIGUSR1, &a, NULL);
	sigemptyset(&usr1);
	sigaddset(&usr1, SIGUSR1);
	for (unsigned long round = 0;; round++) {
		unsigned long seen[8];
		char line[256];
		int len, k;

		for (n = 0; n < 100000000UL; n++)       /* no syscall in here */
			;
		/* take the round's signals and start counting afresh, with SIGUSR1
		 * held: one landing meanwhile counts in the next round, not nowhere */
		sigprocmask(SIG_BLOCK, &usr1, NULL);
		k = nat;
		for (int i = 0; i < k; i++)
			seen[i] = at[i];
		nat = 0;
		sigprocmask(SIG_UNBLOCK, &usr1, NULL);
		len = snprintf(line, sizeof(line), "round %lu signals %d:", round, k);
		for (int i = 0; i < k && len < 200; i++)
			len += snprintf(line + len, sizeof(line) - len, " %lu", seen[i]);
		line[len++] = '\n';
		if (write(1, line, len) < 0)
			return 1;
	}
}
