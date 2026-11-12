/* reexec_target: a program that replaces itself with execve every few rounds
 * (like a server reloading its binary), carrying its state in argv.
 *
 * After execve the address space is new and laid out at random: the replay
 * must still follow, through the dynamic loader's syscalls and with other
 * addresses than in the recording.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	unsigned long iter = argc > 1 ? strtoul(argv[1], NULL, 10) : 0;
	uint64_t acc = argc > 2 ? strtoull(argv[2], NULL, 16) : 1469598103934665603ULL;
	unsigned long execs = argc > 3 ? strtoul(argv[3], NULL, 10) : 0;
	int rnd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);

	for (;; iter++) {
		unsigned char buf[16];
		struct timespec ts;
		char line[128];

		read(rnd, buf, sizeof(buf));
		clock_gettime(CLOCK_REALTIME, &ts);
		for (int i = 0; i < 16; i++)
			acc = (acc ^ buf[i]) * 1099511628211ULL;
		acc ^= ts.tv_nsec;
		void *heap = malloc(1000 + buf[0] * 600);          /* brk and mmap traffic */
		int n = snprintf(line, sizeof(line), "iter %lu execs %lu acc %016llx heap %d\n",
				 iter, execs, (unsigned long long)acc, heap != NULL);
		write(1, line, n);
		struct timespec d = { 0, 100 * 1000 * 1000 };
		nanosleep(&d, NULL);
		if (iter % 5 == 4) {
			char a1[32], a2[32], a3[32];
			snprintf(a1, sizeof(a1), "%lu", iter + 1);
			snprintf(a2, sizeof(a2), "%llx", (unsigned long long)acc);
			snprintf(a3, sizeof(a3), "%lu", execs + 1);
			execl("/proc/self/exe", argv[0], a1, a2, a3, (char *)NULL);
			return 1;
		}
	}
}
