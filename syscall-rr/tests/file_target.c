/* file_target: a small file tree made, read back and removed in every round.
 *
 * The tree lives in /scratch, a tmpfs the test mounts (CRIU saves it during the
 * dump; /dev/shm and the container's own layer are archived later, while the
 * target runs again, and that breaks the checkpoint).  Files made after the
 * checkpoint do not exist in the replay, so every result must come from the
 * log: big reads (several 16 KiB data chunks), readv with many pieces,
 * directory listings, rename, links, truncate, timestamps.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define BIG (200 * 1024)
static unsigned char src[BIG], dst[BIG];

static uint64_t fnv(const void *p, size_t n, uint64_t h)
{
	const unsigned char *c = p;
	while (n--)
		h = (h ^ *c++) * 1099511628211ULL;
	return h;
}

int main(void)
{
	int rnd = open("/dev/urandom", O_RDONLY);
	char dir[64], p[128], q[128];

	for (unsigned long iter = 0;; iter++) {
		uint64_t h = 14695981039346656037ULL;
		struct stat st;
		int e = 0;

		snprintf(dir, sizeof(dir), "/scratch/tree%lu", iter % 3);
		e += mkdir(dir, 0755) != 0;
		read(rnd, src, 64);
		for (size_t i = 64; i < BIG; i++)          /* expand 64 random bytes */
			src[i] = src[i - 64] * 31 + i;

		/* big file: written in 3 pieces, read back 3 ways */
		snprintf(p, sizeof(p), "%s/big", dir);
		int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0644);
		ssize_t w = write(fd, src, 70000);
		w += pwrite(fd, src + 70000, 60000, 70000);
		lseek(fd, 0, SEEK_END);
		struct iovec wv[2] = { { src + 130000, 30000 }, { src + 160000, BIG - 160000 } };
		w += writev(fd, wv, 2);
		fstat(fd, &st);
		off_t end = lseek(fd, 0, SEEK_CUR);
		lseek(fd, 0, SEEK_SET);
		ssize_t r = read(fd, dst, 100000);                  /* 7 chunks */
		struct iovec rv[5];
		for (int i = 0; i < 5; i++)
			rv[i] = (struct iovec){ dst + 100000 + i * 10000, 10000 };
		r += readv(fd, rv, 5);
		r += pread(fd, dst + 150000, BIG - 150000, 150000);
		h = fnv(dst, BIG, h);
		int same = !memcmp(src, dst, BIG);
		ftruncate(fd, 12345);
		struct stat st2;
		fstat(fd, &st2);
		ssize_t past = pread(fd, dst, 100, 20000);          /* past the end: 0 */
		close(fd);

		/* many small files, listed, renamed, linked, removed */
		for (int i = 0; i < 40; i++) {
			snprintf(p, sizeof(p), "%s/f%02d-%02x", dir, i, src[i]);
			int f = open(p, O_WRONLY | O_CREAT | O_EXCL, 0600);
			e += f < 0;
			write(f, src + i, 10 + i);
			close(f);
		}
		snprintf(p, sizeof(p), "%s/f00-%02x", dir, src[0]);
		snprintf(q, sizeof(q), "%s/renamed", dir);
		e += rename(p, q) != 0;
		snprintf(p, sizeof(p), "%s/sym", dir);
		e += symlink("renamed", p) != 0;
		char lk[64] = "";
		ssize_t ll = readlink(p, lk, sizeof(lk) - 1);
		snprintf(p, sizeof(p), "%s/hard", dir);
		e += link(q, p) != 0;
		stat(p, &st);
		int nlink = st.st_nlink;
		e += access(q, R_OK) != 0;
		snprintf(p, sizeof(p), "%s/missing", dir);
		int miss = open(p, O_RDONLY);
		int merr = errno;
		e += mkdir(dir, 0755) == 0;                         /* EEXIST */

		int dfd = open(dir, O_RDONLY | O_DIRECTORY);
		char db[1024];                                      /* small: several calls */
		int names = 0, calls = 0;
		for (;;) {
			long n = syscall(SYS_getdents64, dfd, db, sizeof(db));
			if (n <= 0)
				break;
			calls++;
			for (long o = 0; o < n;) {
				struct dirent64 *d = (void *)(db + o);
				if (d->d_name[0] != '.') {
					names++;
					/* order of a tmpfs listing is not sorted: sum, not chain */
					h += fnv(d->d_name, strlen(d->d_name), d->d_type);
					struct stat fs;
					if (!fstatat(dfd, d->d_name, &fs, AT_SYMLINK_NOFOLLOW)) {
						h += fs.st_size * 1000003ULL + fs.st_ino;
						if (unlinkat(dfd, d->d_name, 0))
							e++;
					}
				}
				o += d->d_reclen;
			}
		}
		fstat(dfd, &st);
		close(dfd);
		e += rmdir(dir) != 0;
		int gone = stat(dir, &st) ? errno : 0;

		char line[256];
		int n = snprintf(line, sizeof(line),
				 "iter %lu wrote %zd end %lld read %zd same %d trunc %lld past %zd link %.*s "
				 "nlink %d miss %d/%d names %d calls %d gone %d errs %d hash %016llx\n",
				 iter, w, (long long)end, r, same, (long long)st2.st_size, past,
				 (int)(ll > 0 ? ll : 0), lk, nlink, miss, merr, names, calls, gone, e,
				 (unsigned long long)h);
		write(1, line, n);
		struct timespec d = { 0, 100 * 1000 * 1000 };
		nanosleep(&d, NULL);
	}
}
