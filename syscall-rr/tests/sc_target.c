/* Syscall conformance target: one line per syscall, forever.
 *
 *     statfs             ret=0 errno=0 hash=9f3a...
 *
 * Each case fills its output buffer with a pattern, makes the call, and prints
 * the return value, errno and a hash of the buffer.  If the recorder does not
 * capture a call's output, the replay fakes the call but leaves the pattern in
 * the buffer: the hash differs from the live one, and the line names the
 * syscall.  Replay with -c (only warn), then compare the lines: one run shows
 * every gap, not just the first.
 *
 * All the objects are created before the checkpoint, so they are restored for
 * real; a case only reads them.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <mqueue.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/sem.h>
#include <sys/sendfile.h>
#include <sys/shm.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/times.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#define BUFSZ 4096
static unsigned char b[BUFSZ], b2[BUFSZ];

static uint64_t fnv(const void *p, size_t n, uint64_t h)
{
	const unsigned char *c = p;
	while (n--)
		h = (h ^ *c++) * 1099511628211ULL;
	return h;
}

static void emit(const char *name, long ret, int err)
{
	char line[128];
	uint64_t h = fnv(b, sizeof(b), 1469598103934665603ULL);
	h = fnv(b2, sizeof(b2), h);
	int n = snprintf(line, sizeof(line), "%-20s ret=%ld errno=%d hash=%016llx\n",
			 name, ret, err, (unsigned long long)h);
	write(1, line, n);
}

/* every case starts with the same pattern in the output buffers */
#define CASE(name, call) do {                   \
		memset(b, 0xa5, sizeof(b));     \
		memset(b2, 0x5a, sizeof(b2));   \
		errno = 0;                      \
		long r_ = (long)(call);         \
		emit(name, r_, errno);          \
	} while (0)

#define S(...) syscall(__VA_ARGS__)

static int fd_file, fd_file2, fd_dir, fd_tfd, fd_efd, fd_ep, fd_ino, fd_sig, fd_lsn;
static int sp[2], dg[2], pp[2], sc[2];
static int msq, semid, shmid;
static timer_t tm;
static void *page;

static void setup(void)
{
	static const char data[] = "conformance test data, conformance test data\n";
	struct sockaddr_un a = { .sun_family = AF_UNIX, .sun_path = "/tmp/sc.sock" };
	struct itimerspec its = { .it_value = { 3600, 0 } };
	sigset_t ss;

	fd_file = open("/tmp/sc.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
	for (int i = 0; i < 200; i++)
		write(fd_file, data, sizeof(data) - 1);
	fd_file2 = open("/tmp/sc.out", O_RDWR | O_CREAT | O_TRUNC, 0644);
	symlink("/tmp/sc.dat", "/tmp/sc.lnk");
	fsetxattr(fd_file, "user.sc", "val", 3, 0);
	fd_dir = open("/etc", O_RDONLY | O_DIRECTORY);

	fd_tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	timerfd_settime(fd_tfd, 0, &its, NULL);
	fd_efd = eventfd(3, EFD_NONBLOCK);
	fd_ep = epoll_create1(0);
	struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 0x1122334455667788ULL };
	epoll_ctl(fd_ep, EPOLL_CTL_ADD, fd_efd, &ev);
	fd_ino = inotify_init1(IN_NONBLOCK);
	/* no watch: CRIU cannot dump one on an overlay filesystem */
	sigemptyset(&ss);
	sigaddset(&ss, SIGUSR2);
	sigprocmask(SIG_BLOCK, &ss, NULL);
	fd_sig = signalfd(-1, &ss, SFD_NONBLOCK);

	socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
	socketpair(AF_UNIX, SOCK_DGRAM, 0, dg);
	socketpair(AF_UNIX, SOCK_STREAM, 0, sc);
	pipe2(pp, O_NONBLOCK);
	unlink(a.sun_path);
	fd_lsn = socket(AF_UNIX, SOCK_STREAM, 0);
	bind(fd_lsn, (struct sockaddr *)&a, sizeof(a));
	listen(fd_lsn, 16);

	msq = msgget(IPC_PRIVATE, 0600);
	semid = semget(IPC_PRIVATE, 2, 0600);
	shmid = shmget(IPC_PRIVATE, 4096, 0600);
	struct sigevent se = { .sigev_notify = SIGEV_NONE };
	timer_create(CLOCK_MONOTONIC, &se, &tm);
	page = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	*(volatile char *)page = 1;
}

static void cases_files(void)
{
	struct iovec v[2] = { { b, 64 }, { b2, 64 } };
	loff_t o1, o2;

	CASE("read", (lseek(fd_file, 0, SEEK_SET), S(SYS_read, fd_file, b, 128)));
	CASE("pread64", S(SYS_pread64, fd_file, b, 128, 7));
	CASE("readv", (lseek(fd_file, 0, SEEK_SET), S(SYS_readv, fd_file, v, 2)));
	CASE("preadv", S(SYS_preadv, fd_file, v, 2, 3, 0));
	CASE("preadv2", S(SYS_preadv2, fd_file, v, 2, 3, 0, 0));
	CASE("lseek", S(SYS_lseek, fd_file, 5, SEEK_SET));
	CASE("fstat", S(SYS_fstat, fd_file, b));
	CASE("stat", S(SYS_stat, "/etc/passwd", b));
	CASE("lstat", S(SYS_lstat, "/tmp/sc.lnk", b));
	CASE("newfstatat", S(SYS_newfstatat, AT_FDCWD, "/etc/passwd", b, 0));
	CASE("statx", S(SYS_statx, AT_FDCWD, "/etc/passwd", 0, STATX_ALL, b));
	CASE("statfs", S(SYS_statfs, "/etc", b));
	CASE("fstatfs", S(SYS_fstatfs, fd_file, b));
	CASE("access", S(SYS_access, "/etc/passwd", R_OK));
	CASE("faccessat2", S(SYS_faccessat2, AT_FDCWD, "/etc/passwd", R_OK, 0));
	CASE("readlink", S(SYS_readlink, "/tmp/sc.lnk", b, 128));
	CASE("readlinkat", S(SYS_readlinkat, AT_FDCWD, "/tmp/sc.lnk", b, 128));
	CASE("getcwd", S(SYS_getcwd, b, 128));
	CASE("getdents64", (S(SYS_lseek, fd_dir, 0, SEEK_SET), S(SYS_getdents64, fd_dir, b, sizeof(b))));
	CASE("getxattr", S(SYS_getxattr, "/tmp/sc.dat", "user.sc", b, 64));
	CASE("lgetxattr", S(SYS_lgetxattr, "/tmp/sc.dat", "user.sc", b, 64));
	CASE("fgetxattr", S(SYS_fgetxattr, fd_file, "user.sc", b, 64));
	CASE("listxattr", S(SYS_listxattr, "/tmp/sc.dat", b, 64));
	CASE("llistxattr", S(SYS_llistxattr, "/tmp/sc.dat", b, 64));
	CASE("flistxattr", S(SYS_flistxattr, fd_file, b, 64));
	struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_len = 1 };
	CASE("fcntl_getlk", (memcpy(b, &fl, sizeof(fl)), S(SYS_fcntl, fd_file, F_GETLK, b)));
	CASE("fcntl_getfl", S(SYS_fcntl, fd_file, F_GETFL));
	CASE("fcntl_getown_ex", S(SYS_fcntl, fd_file, F_GETOWN_EX, b));
	CASE("ioctl_figetbsz", S(SYS_ioctl, fd_file, 2 /* FIGETBSZ */, b));
	CASE("ioctl_fionread", S(SYS_ioctl, fd_file, FIONREAD, b));
	CASE("name_to_handle_at", (*(unsigned *)b = 128,
				   S(SYS_name_to_handle_at, AT_FDCWD, "/etc/passwd", b, b2, 0)));

	/* the kernel moves an offset the caller passed by pointer */
	/* (the output file must not grow: podman archives it after the dump while
	 * the target keeps running, and CRIU refuses a file whose size changed) */
	lseek(fd_file2, 0, SEEK_SET);
	CASE("sendfile", (o1 = 5, memcpy(b, &o1, 8), S(SYS_sendfile, fd_file2, fd_file, b, 16)));
	CASE("copy_file_range", (o1 = 5, o2 = 0, memcpy(b, &o1, 8), memcpy(b2, &o2, 8),
				 S(SYS_copy_file_range, fd_file, b, fd_file2, b2, 16, 0)));
	CASE("splice", (o1 = 5, memcpy(b, &o1, 8), S(SYS_splice, fd_file, b, pp[1], NULL, 16, 0)));
	CASE("read_pipe", S(SYS_read, pp[0], b, 64));
}

static void cases_time(void)
{
	struct itimerspec its = { .it_value = { 3600, 0 } };
	struct itimerval itv = { .it_value = { 3600, 0 } };

	CASE("clock_gettime", S(SYS_clock_gettime, CLOCK_REALTIME, b));
	CASE("clock_getres", S(SYS_clock_getres, CLOCK_MONOTONIC, b));
	CASE("gettimeofday", S(SYS_gettimeofday, b, b2));
	CASE("time", S(SYS_time, b));
	CASE("times", S(SYS_times, b));
	CASE("clock_nanosleep", S(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, (struct timespec[]){ { 0, 1000 } }, b));
	CASE("timerfd_gettime", S(SYS_timerfd_gettime, fd_tfd, b));
	CASE("timerfd_settime", S(SYS_timerfd_settime, fd_tfd, 0, &its, b));
	CASE("timer_create", S(SYS_timer_create, CLOCK_MONOTONIC,
			       (struct sigevent[]){ { .sigev_notify = SIGEV_NONE } }, b));
	CASE("timer_gettime", S(SYS_timer_gettime, tm, b));
	CASE("timer_settime", S(SYS_timer_settime, tm, 0, &its, b));
	CASE("getitimer", S(SYS_getitimer, ITIMER_VIRTUAL, b));
	CASE("setitimer", S(SYS_setitimer, ITIMER_VIRTUAL, &itv, b));
}

static void cases_process(void)
{
	struct __user_cap_header_struct { unsigned version; int pid; } ch = { 0x20080522, 0 };

	CASE("uname", S(SYS_uname, b));
	CASE("sysinfo", S(SYS_sysinfo, b));
	CASE("getrusage", S(SYS_getrusage, RUSAGE_SELF, b));
	CASE("getrlimit", S(SYS_getrlimit, RLIMIT_NOFILE, b));
	CASE("prlimit64", S(SYS_prlimit64, 0, RLIMIT_NOFILE, NULL, b));
	CASE("getresuid", S(SYS_getresuid, b, b + 8, b + 16));
	CASE("getresgid", S(SYS_getresgid, b, b + 8, b + 16));
	CASE("getgroups", S(SYS_getgroups, 32, b));
	CASE("capget", S(SYS_capget, &ch, b));
	CASE("getcpu", S(SYS_getcpu, b, b + 8, NULL));
	CASE("sched_getaffinity", S(SYS_sched_getaffinity, 0, 128, b));
	CASE("sched_getparam", S(SYS_sched_getparam, 0, b));
	CASE("sched_getscheduler", S(SYS_sched_getscheduler, 0));
	CASE("sched_getattr", S(SYS_sched_getattr, 0, b, 56, 0));
	CASE("sched_rr_get_interval", S(SYS_sched_rr_get_interval, 0, b));
	CASE("getpriority", S(SYS_getpriority, PRIO_PROCESS, 0));
	CASE("ioprio_get", S(SYS_ioprio_get, 1, 0));
	CASE("getrandom", S(SYS_getrandom, b, 16, 0));
	CASE("rt_sigpending", S(SYS_rt_sigpending, b, 8));
	CASE("rt_sigtimedwait", ({ sigset_t s; sigemptyset(&s); sigaddset(&s, SIGUSR2);
				   S(SYS_rt_sigtimedwait, &s, b, (struct timespec[]){ { 0, 0 } }, 8); }));
	CASE("mincore", S(SYS_mincore, page, 16384, b));
	CASE("get_robust_list", S(SYS_get_robust_list, 0, b, b2));
	CASE("get_mempolicy", S(SYS_get_mempolicy, b, NULL, 0, NULL, 0));
	CASE("sysfs", S(SYS_sysfs, 2, 0, b));
}

static void cases_fd_objects(void)
{
	struct pollfd pf = { .fd = fd_efd, .events = POLLIN };
	fd_set rs;

	CASE("poll", (memcpy(b, &pf, sizeof(pf)), S(SYS_poll, b, 1, 0)));
	CASE("ppoll", (memcpy(b, &pf, sizeof(pf)),
		       S(SYS_ppoll, b, 1, (struct timespec[]){ { 0, 0 } }, NULL, 8)));
	FD_ZERO(&rs);
	FD_SET(fd_efd, &rs);
	CASE("select", (memcpy(b, &rs, sizeof(rs)),
			S(SYS_select, fd_efd + 1, b, NULL, NULL, (struct timeval[]){ { 0, 0 } })));
	CASE("pselect6", (memcpy(b, &rs, sizeof(rs)),
			  S(SYS_pselect6, fd_efd + 1, b, NULL, NULL, (struct timespec[]){ { 0, 0 } }, NULL)));
	CASE("epoll_wait", S(SYS_epoll_wait, fd_ep, b, 8, 0));
	CASE("epoll_pwait", S(SYS_epoll_pwait, fd_ep, b, 8, 0, NULL, 8));
	CASE("read_inotify", S(SYS_read, fd_ino, b, 256));
	CASE("read_signalfd", S(SYS_read, fd_sig, b, 128));
	CASE("read_timerfd", S(SYS_read, fd_tfd, b, 8));
	CASE("pipe2", S(SYS_pipe2, b, O_CLOEXEC));
	CASE("socketpair", S(SYS_socketpair, AF_UNIX, SOCK_STREAM, 0, b));
	/* the two fds just created must exist afterwards, or the fd tables drift */
	for (int fd = 3; fd < 128; fd++)
		if (fd == *(int *)b || fd == ((int *)b)[1])
			close(fd);
}

static void cases_sockets(void)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX, .sun_path = "/tmp/sc.sock" };
	struct iovec iv = { b, 64 };
	struct msghdr m = { .msg_iov = &iv, .msg_iovlen = 1, .msg_control = b2, .msg_controllen = 256 };
	struct mmsghdr mm[2];
	struct iovec siv[2] = { { "one", 3 }, { "two", 3 } };
	unsigned len;

	write(sp[0], "hello-stream", 12);
	CASE("recvfrom", S(SYS_recvfrom, sp[1], b, 64, 0, NULL, NULL));
	write(dg[0], "hello-dgram", 11);
	len = 64;
	CASE("recvfrom_addr", (memcpy(b2, &len, 4), S(SYS_recvfrom, dg[1], b, 64, 0, b + 128, b2)));
	write(sp[0], "hello-msg", 9);
	CASE("recvmsg", S(SYS_recvmsg, sp[1], &m, 0));
	CASE("getsockname", (len = 64, memcpy(b2, &len, 4), S(SYS_getsockname, fd_lsn, b, b2)));
	CASE("getpeername", (len = 64, memcpy(b2, &len, 4), S(SYS_getpeername, sp[0], b, b2)));
	CASE("getsockopt", (len = 4, memcpy(b2, &len, 4), S(SYS_getsockopt, sp[0], SOL_SOCKET, SO_TYPE, b, b2)));

	/* sendmmsg writes msg_len back into the caller's array */
	memset(mm, 0, sizeof(mm));
	mm[0].msg_hdr.msg_iov = &siv[0], mm[0].msg_hdr.msg_iovlen = 1;
	mm[1].msg_hdr.msg_iov = &siv[1], mm[1].msg_hdr.msg_iovlen = 1;
	CASE("sendmmsg", (memcpy(b, mm, sizeof(mm)), S(SYS_sendmmsg, dg[0], b, 2, 0)));

	/* recvmmsg writes the data, and msg_len in the array */
	static unsigned char r1[64], r2[64];
	struct iovec riv[2] = { { r1, 64 }, { r2, 64 } };
	memset(r1, 0xa5, 64), memset(r2, 0xa5, 64);
	memset(mm, 0, sizeof(mm));
	mm[0].msg_hdr.msg_iov = &riv[0], mm[0].msg_hdr.msg_iovlen = 1;
	mm[1].msg_hdr.msg_iov = &riv[1], mm[1].msg_hdr.msg_iovlen = 1;
	CASE("recvmmsg", ({ long r = S(SYS_recvmmsg, dg[1], mm, 2, MSG_DONTWAIT, NULL);
			    memcpy(b, r1, 64); memcpy(b + 64, r2, 64); memcpy(b + 128, mm, sizeof(mm)); r; }));

	/* a connection: accept must give a usable fd number */
	int c = socket(AF_UNIX, SOCK_STREAM, 0);
	connect(c, (struct sockaddr *)&a, sizeof(a));
	CASE("accept4", ({ len = 64; memcpy(b2, &len, 4);
			   int fd = S(SYS_accept4, fd_lsn, b, b2, SOCK_CLOEXEC);
			   if (fd >= 0) close(fd); fd; }));
	close(c);

	/* fd passing: the number comes in msg_control, the fd must really exist */
	char cm[CMSG_SPACE(sizeof(int))] = { 0 };
	struct msghdr sm = { .msg_iov = &(struct iovec){ "x", 1 }, .msg_iovlen = 1,
			     .msg_control = cm, .msg_controllen = sizeof(cm) };
	struct cmsghdr *h = CMSG_FIRSTHDR(&sm);
	h->cmsg_level = SOL_SOCKET, h->cmsg_type = SCM_RIGHTS, h->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(h), &fd_file, sizeof(int));
	sendmsg(sc[0], &sm, 0);
	char rcm[CMSG_SPACE(sizeof(int))];
	struct iovec riov = { b2, 16 };
	struct msghdr rm = { .msg_iov = &riov, .msg_iovlen = 1, .msg_control = rcm, .msg_controllen = sizeof(rcm) };
	CASE("scm_rights_recv", ({ long r = S(SYS_recvmsg, sc[1], &rm, 0);
				   int nfd = -1;
				   struct cmsghdr *g = CMSG_FIRSTHDR(&rm);
				   if (g) memcpy(&nfd, CMSG_DATA(g), sizeof(int));
				   /* mmap runs for real: only works with a real fd */
				   void *mp = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, nfd, 0);
				   snprintf((char *)b, 64, "got=%d mapped=%d", nfd >= 0, mp != MAP_FAILED);
				   /* unconditional: the same syscalls whether it worked or not */
				   munmap(mp, 4096);
				   if (nfd >= 0) close(nfd);
				   r; }));
}

static void cases_ipc(void)
{
	struct { long t; char s[32]; } mb = { 1, "ipc" };

	msgsnd(msq, &mb, sizeof(mb.s), 0);
	CASE("msgrcv", S(SYS_msgrcv, msq, b, 32, 0, IPC_NOWAIT));
	CASE("msgctl_stat", S(SYS_msgctl, msq, IPC_STAT, b));
	CASE("semctl_stat", S(SYS_semctl, semid, 0, IPC_STAT, b));
	CASE("semctl_getall", S(SYS_semctl, semid, 0, GETALL, b));
	CASE("shmctl_stat", S(SYS_shmctl, shmid, IPC_STAT, b));
	/* opened only for a moment: CRIU cannot dump an open message queue */
	struct mq_attr qa = { .mq_maxmsg = 4, .mq_msgsize = 64 };
	mqd_t mq = mq_open("/sc", O_RDWR | O_CREAT | O_NONBLOCK, 0600, &qa);
	mq_unlink("/sc");      /* CRIU also wants the mqueue filesystem empty */
	mq_send(mq, "mq-data", 7, 3);
	CASE("mq_timedreceive", S(SYS_mq_timedreceive, mq, b, 64, b2, (struct timespec[]){ { 0, 0 } }));
	CASE("mq_getsetattr", S(SYS_mq_getsetattr, mq, NULL, b));
	mq_close(mq);
}

int main(void)
{
	setup();
	sleep(1);
	for (;;) {
		cases_files();
		cases_time();
		cases_process();
		cases_fd_objects();
		cases_sockets();
		cases_ipc();
		write(1, "--- end of round\n", 17);
		usleep(50000);
	}
}
