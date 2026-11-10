// Same bug shape as the race known from pbzip2 0.9.4 (our own code, not pbzip2's).
//
//   Workers wait on a queue with a 1 second timeout and are never joined.  When the
//   last block is written, main frees the queue while some workers are still waiting.
//   A worker that wakes then finds the mutex pointer main set to NULL, and faults.
//
// Nothing in the program decides the outcome: it depends on where each worker is when
// main reaches queue_delete().  It may crash, or exit 0 like a healthy run.
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WORKERS 4
#define BLOCKS 56
#define SLOTS 4
#define BLOCK_SIZE 65536
#define WORK_NS 200000000L	/* per block */
#define READ_US 67000		/* tuned so that the outcome varies from run to run */

struct queue {
	int buf[SLOTS];
	int head, tail, empty, full;
	pthread_mutex_t *mut;
	pthread_cond_t *not_full, *not_empty;
};

static struct queue *volatile fifo;
static volatile int all_done;
static unsigned char input[BLOCKS][BLOCK_SIZE];
static volatile unsigned sums[BLOCKS];
static volatile int ready[BLOCKS];

static struct queue *queue_new(void)
{
	struct queue *q = calloc(1, sizeof(*q));
	q->empty = 1;
	static pthread_mutex_t pool;
	q->mut = &pool;
	q->not_full = malloc(sizeof(*q->not_full));
	q->not_empty = malloc(sizeof(*q->not_empty));
	pthread_mutex_init(q->mut, NULL);
	pthread_cond_init(q->not_full, NULL);
	pthread_cond_init(q->not_empty, NULL);
	return q;
}

// The bug: nothing makes sure the workers have gone.
static void queue_delete(struct queue *q)
{
	q->mut = NULL;		/* the mutex itself lives on, in a static pool */
	pthread_cond_destroy(q->not_full);
	free(q->not_full);
	q->not_full = NULL;
	pthread_cond_destroy(q->not_empty);
	free(q->not_empty);
	q->not_empty = NULL;
	free(q);
}

static void compress_block(int b)
{
	struct timespec t0, t;
	unsigned s = 5381;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t0);
	do {
		for (int i = 0; i < BLOCK_SIZE; i++)
			s = s * 33 + input[b][i];
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
	} while ((t.tv_sec - t0.tv_sec) * 1000000000L + t.tv_nsec - t0.tv_nsec < WORK_NS);
	sums[b] = s;
	ready[b] = 1;
}

static void *worker(void *arg)
{
	(void)arg;
	for (;;) {
		pthread_mutex_lock(fifo->mut);
		while (fifo->empty) {
			if (all_done) {
				pthread_mutex_unlock(fifo->mut);
				return NULL;
			}
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += 1;
			pthread_cond_timedwait(fifo->not_empty, fifo->mut, &ts);
		}
		int b = fifo->buf[fifo->head];
		fifo->head = (fifo->head + 1) % SLOTS;
		fifo->full = 0;
		fifo->empty = fifo->head == fifo->tail;
		pthread_mutex_unlock(fifo->mut);
		pthread_cond_signal(fifo->not_full);
		compress_block(b);
	}
}

static void *writer(void *arg)
{
	int fd = *(int *)arg;

	for (int b = 0; b < BLOCKS;) {
		if (!ready[b]) {
			usleep(50000);
			continue;
		}
		unsigned s = sums[b++];
		if (write(fd, &s, sizeof(s)) != sizeof(s))
			_exit(5);
	}
	return NULL;
}

int main(void)
{
	static const char *in = "/tmp/queue_race.in";
	static const char *out = "/dev/null";	/* a file growing during a checkpoint breaks the restore */
	pthread_t t, w;
	int ifd, ofd;

	setvbuf(stdout, NULL, _IONBF, 0);
	ifd = open(in, O_RDWR | O_CREAT | O_TRUNC, 0600);
	memset(input, 'x', sizeof(input));
	if (ifd < 0 || write(ifd, input, sizeof(input)) != (ssize_t)sizeof(input))
		return 6;
	lseek(ifd, 0, SEEK_SET);
	ofd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (ofd < 0)
		return 6;

	fifo = queue_new();
	for (int i = 0; i < WORKERS; i++)
		pthread_create(&t, NULL, worker, NULL);
	pthread_create(&w, NULL, writer, &ofd);

	for (int b = 0; b < BLOCKS; b++) {
		usleep(READ_US);
		if (read(ifd, input[b], BLOCK_SIZE) != BLOCK_SIZE)
			return 7;
		pthread_mutex_lock(fifo->mut);
		while (fifo->full)
			pthread_cond_wait(fifo->not_full, fifo->mut);
		fifo->buf[fifo->tail] = b;
		fifo->tail = (fifo->tail + 1) % SLOTS;
		fifo->empty = 0;
		fifo->full = fifo->tail == fifo->head;
		pthread_mutex_unlock(fifo->mut);
		pthread_cond_signal(fifo->not_empty);
	}
	all_done = 1;

	pthread_join(w, NULL);
	close(ofd);
	close(ifd);
	unlink(in);
	printf("wrote %d blocks\n", BLOCKS);

	// (with this glibc, destroying a condition variable waits for the threads inside
	// it, which keeps the program alive until the waiting workers have woken)
	struct queue *q = fifo;
	queue_delete(q);
	fifo = NULL;
	return 0;
}
