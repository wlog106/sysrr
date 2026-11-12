/* pool_target: a thread pool, the usual shape of a threaded server.
 *
 * main puts jobs on a queue guarded by a mutex and a condition variable; four
 * workers take them, read random bytes, sleep a little and send the result
 * back through a pipe; main collects the round and prints it.  Which worker
 * got which job depends on who won the lock, so the replay needs -T.
 *
 *   pool_target         jobs are handed out one by one (pthread_cond_signal),
 *                       1 ms apart, as requests arrive at a server
 *   pool_target burst   all six at once (pthread_cond_broadcast): four workers
 *                       wake together and take the lock within nanoseconds of
 *                       each other.  The log cannot always tell who was first
 *                       (REPLAY.md, limitations): the replay may diverge, and
 *                       must say so.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define WORKERS 4
#define JOBS 6

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int queue[64], head, tail;
static int res[2], rnd;
static uint64_t total;          /* shared, updated under the lock */

struct result { int job, worker; uint64_t val, total; };

static void *worker(void *arg)
{
	int id = (long)arg;

	for (;;) {
		pthread_mutex_lock(&mu);
		while (head == tail)
			pthread_cond_wait(&cv, &mu);
		int job = queue[head++ % 64];
		pthread_mutex_unlock(&mu);

		uint64_t r;
		read(rnd, &r, sizeof(r));
		struct timespec d = { 0, (r % 5) * 1000 * 1000 };
		nanosleep(&d, NULL);

		pthread_mutex_lock(&mu);
		total = (total ^ r) * 1099511628211ULL + job;
		struct result out = { job, id, r, total };
		pthread_mutex_unlock(&mu);
		write(res[1], &out, sizeof(out));
	}
	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t t;
	int burst = argc > 1;

	rnd = open("/dev/urandom", O_RDONLY);
	pipe(res);
	for (long i = 0; i < WORKERS; i++)
		pthread_create(&t, NULL, worker, (void *)i);

	for (unsigned long iter = 0;; iter++) {
		char line[512];
		int n = snprintf(line, sizeof(line), "iter %lu", iter);

		if (burst) {
			pthread_mutex_lock(&mu);
			for (int j = 0; j < JOBS; j++)
				queue[tail++ % 64] = iter * JOBS + j;
			pthread_cond_broadcast(&cv);
			pthread_mutex_unlock(&mu);
		}
		for (int j = 0; j < JOBS && !burst; j++) {
			pthread_mutex_lock(&mu);
			queue[tail++ % 64] = iter * JOBS + j;
			pthread_cond_signal(&cv);
			pthread_mutex_unlock(&mu);
			struct timespec gap = { 0, 1000 * 1000 };
			nanosleep(&gap, NULL);
		}

		for (int j = 0; j < JOBS; j++) {
			struct result in;
			read(res[0], &in, sizeof(in));
			n += snprintf(line + n, sizeof(line) - n, " j%d:w%d:%04x:%04x", in.job, in.worker,
				      (unsigned)(in.val & 0xffff), (unsigned)(in.total & 0xffff));
		}
		line[n++] = '\n';
		write(1, line, n);
		struct timespec d = { 0, 30 * 1000 * 1000 };
		nanosleep(&d, NULL);
	}
}
