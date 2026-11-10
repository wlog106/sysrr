/* The target finishes by itself, a known time after it starts, so it exits
 * inside the recorded generation.  The replay must follow it to the end and
 * report that it exited, rather than wait forever for syscalls that never
 * come.  (The purpose of this tool is a target that fails: a process that ends
 * is the normal last scene.)
 *
 * With an argument, it dies by a signal instead of a clean exit: a NULL write,
 * the kind of crash this tool exists to explain.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int rnd = open("/dev/urandom", O_RDONLY);
	struct timespec d = { 0, 20 * 1000 * 1000 };

	/* start at about 0 s, checkpoint at 2 s, end at about 3 s */
	for (int i = 0; i < 150; i++) {
		uint64_t r = 0;
		char line[64];
		read(rnd, &r, sizeof(r));
		int n = snprintf(line, sizeof(line), "iter %d rnd %016llx\n", i, (unsigned long long)r);
		write(1, line, n);
		nanosleep(&d, NULL);
	}
	if (argc > 1)
		*(volatile int *)0 = 1;
	return 7;
}
