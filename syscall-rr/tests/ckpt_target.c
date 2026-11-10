/* Test workload for the checkpoint/record workflow.
 * Its state (iter, acc) evolves only from syscall results: random bytes,
 * the clock and the content of a file.  After a restore, replaying the
 * recorded syscalls must reproduce exactly the lines the live process
 * printed after the checkpoint. */
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
	uint64_t acc = 1469598103934665603ULL;   /* FNV offset basis */
	int rnd = open("/dev/urandom", O_RDONLY);

	for (unsigned long iter = 0;; iter++) {
		unsigned char buf[16];
		struct timespec ts;
		char line[128];

		if (read(rnd, buf, sizeof(buf)) != sizeof(buf))
			return 1;
		clock_gettime(CLOCK_REALTIME, &ts);
		for (int i = 0; i < 16; i++)
			acc = (acc ^ buf[i]) * 1099511628211ULL;
		acc ^= ts.tv_nsec;
		int n = snprintf(line, sizeof(line), "iter %lu acc %016llx\n",
				 iter, (unsigned long long)acc);
		write(1, line, n);
		struct timespec d = { 0, 200 * 1000 * 1000 };
		nanosleep(&d, NULL);
	}
}
