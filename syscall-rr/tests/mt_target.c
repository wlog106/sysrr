/* Multi-threaded replay target.  Two threads share one accumulator and
 * update it with atomics (no lock, so no syscall marks the update):
 *
 *     read(urandom, 8 bytes)  ->  acc = f(acc, bytes)  ->  write("T<n> ... acc")
 *
 * Each line depends on the order in which the two threads updated acc, so a
 * replay reproduces the output only if the threads run in the recorded
 * order.  Per-thread replay (default) does not guarantee that; strict order
 * (sysrr_replay -S) does, because every update sits between two syscalls of
 * its own thread. */
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static uint64_t acc = 1469598103934665603ULL;
static int rnd;

static void *worker(void *arg)
{
	long id = (long)arg;
	struct timespec d = { 0, (40 + 13 * id) * 1000 * 1000 };

	for (unsigned long iter = 0;; iter++) {
		uint64_t r, old, new;
		char line[96];

		if (read(rnd, &r, sizeof(r)) != sizeof(r))
			return NULL;
		old = __atomic_load_n(&acc, __ATOMIC_RELAXED);
		do
			new = (old ^ r) * 1099511628211ULL;
		while (!__atomic_compare_exchange_n(&acc, &old, new, 0,
						    __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));
		int n = snprintf(line, sizeof(line), "T%ld iter %lu acc %016llx\n",
				 id, iter, (unsigned long long)new);
		write(1, line, n);
		nanosleep(&d, NULL);
	}
}

int main(void)
{
	pthread_t t[2];

	rnd = open("/dev/urandom", O_RDONLY);
	for (long i = 0; i < 2; i++)
		pthread_create(&t[i], NULL, worker, (void *)i);
	pthread_join(t[0], NULL);
	return 0;
}
