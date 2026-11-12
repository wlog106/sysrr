/* mpmt_target: several processes, each with several threads.
 *
 *   parent ── socketpair ── worker process 1: a dispatcher thread + 2 worker threads
 *          └─ socketpair ── worker process 2: the same
 *          └─ every 4th round: a new process that starts 2 threads and exits
 *
 * The two worker processes exist before the checkpoint; the short-lived ones are
 * born during the recording.  The parent sends jobs to the worker processes in
 * turn, 1 ms apart.  In each, the dispatcher puts the job on a queue (mutex and
 * condition variable), a worker thread takes it, reads random bytes, adds them
 * to a total its process keeps under the mutex, and answers on the socket.  The
 * parent prints who did what.  Record with sysrr -f, replay with -T.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PROCS 2
#define THREADS 2
#define JOBS 6

struct result { int job, pid, tid; uint32_t val, total; };

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int queue[64], head, tail, sock, rnd;
static uint32_t total;

static void *worker(void *arg)
{
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&mu);
		while (head == tail)
			pthread_cond_wait(&cv, &mu);
		int job = queue[head++ % 64];
		pthread_mutex_unlock(&mu);

		uint32_t r;
		read(rnd, &r, sizeof(r));
		struct timespec d = { 0, (r % 3) * 1000 * 1000 };
		nanosleep(&d, NULL);

		pthread_mutex_lock(&mu);
		total = (total ^ r) * 16777619u + job;
		struct result out = { job, getpid(), syscall(SYS_gettid), r, total };
		write(sock, &out, sizeof(out));         /* under the lock: answers stay whole */
		pthread_mutex_unlock(&mu);
	}
	return NULL;
}

static void worker_process(int s)
{
	pthread_t t;
	int job;

	sock = s;
	rnd = open("/dev/urandom", O_RDONLY);
	for (int i = 0; i < THREADS; i++)
		pthread_create(&t, NULL, worker, NULL);
	while (read(sock, &job, sizeof(job)) == sizeof(job)) {      /* the dispatcher */
		pthread_mutex_lock(&mu);
		queue[tail++ % 64] = job;
		pthread_cond_signal(&cv);
		pthread_mutex_unlock(&mu);
	}
	_exit(0);
}

static void *brief(void *arg)
{
	uint16_t r;
	int fd = open("/dev/urandom", O_RDONLY);
	read(fd, &r, sizeof(r));
	close(fd);
	struct result out = { -1, getpid(), syscall(SYS_gettid), r, 0 };
	write(*(int *)arg, &out, sizeof(out));
	return NULL;
}

int main(void)
{
	int sv[PROCS][2];

	for (int p = 0; p < PROCS; p++) {
		socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv[p]);
		if (fork() == 0) {
			for (int q = 0; q <= p; q++)
				close(sv[q][0]);
			worker_process(sv[p][1]);
		}
		close(sv[p][1]);
	}

	for (unsigned long iter = 0;; iter++) {
		char line[700];
		int n = snprintf(line, sizeof(line), "iter %lu", iter);

		for (int j = 0; j < JOBS; j++) {
			int job = iter * JOBS + j;
			write(sv[j % PROCS][0], &job, sizeof(job));
			struct timespec gap = { 0, 1000 * 1000 };
			nanosleep(&gap, NULL);
		}
		for (int p = 0; p < PROCS; p++)
			for (int j = 0; j < JOBS / PROCS; j++) {
				struct result in;
				read(sv[p][0], &in, sizeof(in));
				n += snprintf(line + n, sizeof(line) - n, " j%d:p%d.t%d:%04x:%04x", in.job, in.pid,
					      in.tid, in.val & 0xffff, in.total & 0xffff);
			}
		if (iter % 4 == 3) {                    /* a new process with new threads */
			int pp[2];
			pipe(pp);
			pid_t c = fork();
			if (c == 0) {
				pthread_t t[2];
				for (int i = 0; i < 2; i++)
					pthread_create(&t[i], NULL, brief, &pp[1]);
				for (int i = 0; i < 2; i++)
					pthread_join(t[i], NULL);
				_exit(iter % 7);
			}
			close(pp[1]);
			uint32_t sum = 0;
			struct result in;
			int tids = 0, st = 0;
			while (read(pp[0], &in, sizeof(in)) == sizeof(in))
				sum += in.val, tids += in.tid - in.pid;
			close(pp[0]);
			waitpid(c, &st, 0);
			n += snprintf(line + n, sizeof(line) - n, " new:p%d:%d:%04x:exit%d", c, tids, sum & 0xffff,
				      WEXITSTATUS(st));
		}
		line[n++] = '\n';
		write(1, line, n);
		struct timespec d = { 0, 30 * 1000 * 1000 };
		nanosleep(&d, NULL);
	}
}
