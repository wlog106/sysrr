/* fd_target: creates fds after the checkpoint and uses them in a syscall
 * that the replay runs for real (mmap), so a faked open() alone is not
 * enough: the fd must exist.
 *
 * Every 300 ms:
 *   f = open("/etc/debian_version")      faked, fd installed with ADDFD
 *   read(f) / fstat(f)                   faked (recorded bytes)
 *   mmap(f)                              REAL: needs fd f to be a real file
 *   pipe2 / socketpair / socket / dup    faked, fds installed with ADDFD
 *   write one line: fd numbers, what read() saw, what the mapping shows
 *   munmap (real), close x7 (real)
 *
 * Replay without fd installation (sysrr_replay -N): mmap fails with EBADF,
 * the line says "map=EBADF" instead of the file's first bytes, and the
 * replay reports a divergence at that write().
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
	for (int i = 0;; i++) {
		int f = open("/etc/debian_version", O_RDONLY | O_CLOEXEC);
		char rb[16] = "";
		ssize_t n = read(f, rb, 8);
		struct stat st;
		fstat(f, &st);

		char *m = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, f, 0);
		char mb[16];
		if (m == MAP_FAILED)
			snprintf(mb, sizeof(mb), "%s", strerrorname_np(errno));
		else
			snprintf(mb, sizeof(mb), "%.4s", m);

		int p[2], s[2];
		pipe2(p, O_CLOEXEC);
		socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, s);
		int u = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
		int d = dup(f);

		char line[200];
		int len = snprintf(line, sizeof(line),
				   "iter %d map=%s read=%.4s size=%lld f=%d p=%d,%d s=%d,%d u=%d d=%d\n",
				   i, mb, n > 0 ? rb : "?", (long long)st.st_size,
				   f, p[0], p[1], s[0], s[1], u, d);
		write(1, line, len);

		if (m != MAP_FAILED)
			munmap(m, 4096);
		close(d); close(u); close(s[1]); close(s[0]); close(p[1]); close(p[0]); close(f);
		struct timespec ts = { 0, 300 * 1000 * 1000 };
		nanosleep(&ts, NULL);
	}
}
