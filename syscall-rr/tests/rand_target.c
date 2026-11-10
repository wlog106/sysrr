/* glibc's getrandom() with a recent kernel (6.11+) and glibc (2.41+): the bytes
 * are made in user space by the vDSO, like clock_gettime, so no syscall records
 * them.  The state lives in the process's memory, so a replay on the same
 * machine gets the same bytes, unless the kernel reseeded its generator in
 * between.  On another machine the vDSO sees a new generation and asks for a new
 * key with a real getrandom syscall that the log does not have.  The replay
 * must report that, not hang and not pass with different bytes.
 */
#include <stdint.h>
#include <stdio.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
	struct timespec d = { 0, 20 * 1000 * 1000 };

	for (unsigned long i = 0;; i++) {
		uint64_t r = 0;
		char line[64];
		getrandom(&r, sizeof(r), 0);
		int n = snprintf(line, sizeof(line), "iter %lu rnd %016llx\n", i, (unsigned long long)r);
		write(1, line, n);
		nanosleep(&d, NULL);
	}
}
