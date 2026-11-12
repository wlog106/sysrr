/* sigext_target: signals from outside the container (podman kill), at moments
 * nobody chose.  SIGUSR1 has a restarting handler, SIGTERM a plain one, so a
 * sleep interrupted by SIGTERM returns early with EINTR.  Each line prints the
 * counts: a replay matches only if every signal comes back at the syscall where
 * the live thread received it.  Run with KICK=USR1+TERM (tests/test_replay.sh).
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t usr1, term;

static void on_usr1(int s) { (void)s; usr1++; }
static void on_term(int s) { (void)s; term++; }

int main(void)
{
	struct sigaction a = { .sa_handler = on_usr1, .sa_flags = SA_RESTART };
	struct sigaction b = { .sa_handler = on_term };
	int rnd = open("/dev/urandom", O_RDONLY), eintr = 0;

	sigaction(SIGUSR1, &a, NULL);
	sigaction(SIGTERM, &b, NULL);
	for (unsigned long i = 0;; i++) {
		unsigned char r;
		struct timespec d = { 0, 20 * 1000 * 1000 };
		char line[96];

		read(rnd, &r, 1);
		if (nanosleep(&d, NULL) && errno == EINTR)
			eintr++;
		int n = snprintf(line, sizeof(line), "iter %lu byte %02x usr1 %d term %d eintr %d\n",
				 i, r, (int)usr1, (int)term, eintr);
		write(1, line, n);
	}
}
