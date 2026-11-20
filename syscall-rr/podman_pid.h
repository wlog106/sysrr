/* podman_pid.h: the host pid of a podman container's first process, so the
 * tools take a container name (-C NAME) as well as a pid (-p PID). */
#ifndef PODMAN_PID_H
#define PODMAN_PID_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* > 0: the pid; 0: no such running container (podman said why on stderr) */
static int podman_pid(const char *name)
{
	int fds[2], pid = 0, st;
	char buf[32] = "";
	ssize_t n;

	if (pipe(fds))
		return 0;
	pid_t c = fork();
	if (c < 0)
		return 0;
	if (c == 0) {
		dup2(fds[1], 1);
		close(fds[0]);
		close(fds[1]);
		/* no shell: the name goes to podman as one argument, whatever it holds */
		execlp("podman", "podman", "inspect", "--type", "container", "-f",
		       "{{.State.Pid}}", "--", name, (char *)NULL);
		perror("podman");
		_exit(127);
	}
	close(fds[1]);
	/* read to the end: podman may write the pid and the newline separately, and
	 * closing the pipe early would kill it with SIGPIPE */
	size_t got = 0;
	while (got < sizeof(buf) - 1 && (n = read(fds[0], buf + got, sizeof(buf) - 1 - got)) > 0)
		got += n;
	while (read(fds[0], (char[64]){0}, 64) > 0)
		;
	n = got;
	close(fds[0]);
	waitpid(c, &st, 0);
	if (n > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0)
		pid = atoi(buf);
	if (pid <= 0)
		fprintf(stderr, "container %s: not found or not running (podman %s %d, said \"%.*s\")\n",
			name, WIFEXITED(st) ? "exit" : "signal",
			WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st), (int)(n > 0 ? n : 0), buf);
	return pid > 0 ? pid : 0;
}

#endif
