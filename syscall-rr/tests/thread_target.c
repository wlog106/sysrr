/* Thread churn: each round starts a short-lived thread and joins it.  The
 * thread prints a line that depends on its own gettid() and on a read from
 * /dev/urandom.  A new thread gets a tid during the replay, which must be the
 * recorded one for its syscalls to find their queue and for the line to match.
 * Hundreds of threads per generation: more than any small fixed table.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int rnd;
static unsigned long round_no;

static void *worker(void *arg)
{
	uint64_t r = 0;
	char line[96];

	(void)arg;
	if (read(rnd, &r, sizeof(r)) != sizeof(r))
		return NULL;
	int n = snprintf(line, sizeof(line), "round %lu tid %ld rnd %016llx\n", round_no,
			 syscall(SYS_gettid), (unsigned long long)r);
	write(1, line, n);
	return NULL;
}

int main(void)
{
	struct timespec d = { 0, 2 * 1000 * 1000 };

	rnd = open("/dev/urandom", O_RDONLY);
	for (;; round_no++) {
		pthread_t t;
		if (pthread_create(&t, NULL, worker, NULL))
			return 1;
		pthread_join(t, NULL);
		nanosleep(&d, NULL);
	}
}
