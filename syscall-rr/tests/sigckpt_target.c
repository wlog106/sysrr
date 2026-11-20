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

	sigaction(SIGUSR1, &a, NULL);
	for (unsigned long round = 0;; round++) {
		char line[256];
		int len;

		for (n = 0; n < 100000000UL; n++)       /* no syscall in here */
			;
		len = snprintf(line, sizeof(line), "round %lu signals %d:", round, nat);
		for (int i = 0; i < nat && len < 200; i++)
			len += snprintf(line + len, sizeof(line) - len, " %lu", at[i]);
		line[len++] = '\n';
		write(1, line, len);
		nat = 0;
	}
}
