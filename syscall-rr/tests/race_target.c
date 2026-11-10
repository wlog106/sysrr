/* Race test target: another thread changes an input buffer while a syscall
 * is using it.  Two cases, chosen so that each capture point is wrong once:
 *
 * 1. write() to a full pipe.  The kernel copies the buffer only when the
 *    pipe has room, i.e. late:
 *      writer:   write(pipe, buf = "AAAA...", 4096)   blocks, pipe is full
 *      main:     buf = "BBBB..."                       while write() waits
 *      main:     drain the pipe                        write() copies "BBBB"
 *    The sys_enter copy says 'A', but the reader receives 'B'.
 *
 * 2. open() of a FIFO.  The kernel copies the path first, then waits for a
 *    writer to open the other end:
 *      opener:   open(path = "<dir>/fifo", O_RDONLY)  blocks, no writer yet
 *      main:     path = "<dir>/XXXX"                  while open() waits
 *      main:     open the FIFO for writing            open() returns
 *    The sys_exit copy says "XXXX", but the kernel opened "fifo".
 *
 * Before both, a control write that nobody touches.
 *
 *   race_target DIR       (DIR must exist; the FIFO is created in it)
 */
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char buf[4096];
static char path[256];
static int p[2];

static void *writer(void *arg)
{
	return (void *)write(p[1], buf, sizeof(buf));
}

static void *opener(void *arg)
{
	return (void *)(long)open(path, O_RDONLY);
}

int main(int argc, char **argv)
{
	static char drain[1 << 16];
	char fill[4096], got[4096], fifo[256];
	pthread_t t;
	void *rv;
	long filled = 0, n;
	int fd;

	if (argc < 2) {
		fprintf(stderr, "usage: %s DIR\n", argv[0]);
		return 1;
	}

	/* control: a write whose buffer nobody changes */
	static const char ctl[] = "control write: nobody touches this buffer\n";
	write(1, ctl, sizeof(ctl) - 1);

	/* case 1 */
	if (pipe(p))
		return 1;
	memset(fill, 'F', sizeof(fill));
	fcntl(p[1], F_SETFL, O_NONBLOCK);
	while ((n = write(p[1], fill, sizeof(fill))) > 0)
		filled += n;                            /* fill the pipe */
	fcntl(p[1], F_SETFL, 0);

	memset(buf, 'A', sizeof(buf));
	pthread_create(&t, NULL, writer, NULL);
	usleep(100000);                                 /* writer is blocked in write() */
	memset(buf, 'B', sizeof(buf));                  /* the race */
	usleep(100000);
	while (filled > 0 && (n = read(p[0], drain, filled < (long)sizeof(drain) ?
				    filled : (long)sizeof(drain))) > 0)
		filled -= n;                            /* drain exactly the 'F' bytes */
	pthread_join(t, &rv);
	read(p[0], got, sizeof(got));                   /* what write() put in the pipe */
	printf("case 1: write() returned %ld; reader got '%c' (buffer was 'A' at sys_enter)\n",
	       (long)rv, got[0]);

	/* case 2 */
	snprintf(fifo, sizeof(fifo), "%s/fifo", argv[1]);
	unlink(fifo);
	if (mkfifo(fifo, 0600))
		return 1;
	strcpy(path, fifo);
	pthread_create(&t, NULL, opener, NULL);
	usleep(100000);                                 /* opener is blocked in open() */
	memcpy(path + strlen(path) - 4, "XXXX", 4);     /* the race */
	usleep(100000);
	fd = open(fifo, O_WRONLY);                      /* let the opener through */
	pthread_join(t, &rv);
	printf("case 2: open() returned fd %ld: the kernel opened %s (buffer now says %s)\n",
	       (long)rv, fifo, path);
	close(fd);
	unlink(fifo);
	return 0;
}
