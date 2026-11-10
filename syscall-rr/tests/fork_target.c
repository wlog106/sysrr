/* Process churn: every round forks a child, which reports over a pipe, and the
 * parent waits for it.  Round after round, so a generation holds more children
 * than any small fixed table.
 *
 * A line holds what the parent learned three ways: the pid fork() returned
 * (runs for real), the pid wait4() returned (recorded) and the pid the child
 * reports about itself (getpid runs for real, the pipe read is recorded).
 * ok=1 means they agree, as they must in a real process.  A replay that lets a
 * new process get another pid than in the recording shows ok=0.
 *
 * Every few rounds the child also opens a file relative to a directory fd:
 * the supervisor needs that child's fd table, which adds a process to its
 * list while it runs.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
	int dirfd = open("/etc", O_RDONLY | O_DIRECTORY);
	struct timespec d = { 0, 5 * 1000 * 1000 };

	for (unsigned long round = 0;; round++) {
		int p[2], st = -1;
		pid_t got = -1, c;
		char line[128];

		if (pipe(p))
			return 1;
		c = fork();
		if (c == 0) {
			pid_t me = getpid();
			close(p[0]);
			if (round % 4 == 0) {
				int fd = openat(dirfd, "passwd", O_RDONLY);
				if (fd >= 0)
					close(fd);
			}
			write(p[1], &me, sizeof(me));
			close(p[1]);
			_exit((round & 0x7f) + 1);
		}
		close(p[1]);
		if (read(p[0], &got, sizeof(got)) != sizeof(got))
			got = -2;
		close(p[0]);
		pid_t w = waitpid(c, &st, 0);
		int n = snprintf(line, sizeof(line), "round %lu ok=%d exit=%d\n", round,
				 c == w && c == got, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
		write(1, line, n);
		nanosleep(&d, NULL);
	}
}
