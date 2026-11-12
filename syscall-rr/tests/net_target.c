/* net_target: a TCP echo server and its client in one thread, over loopback,
 * driven by epoll with non-blocking sockets; plus one UDP datagram per round.
 *
 * Every round makes a new connection (connect -> EINPROGRESS, accept4), sends
 * random bytes through it in pieces, echoes them back and prints a hash of what
 * arrived, the peer's port and the socket options it read.  All of that comes
 * from syscalls, so a replay must print the same lines.  The listening socket
 * exists before the checkpoint (CRIU restores it); the connections are new.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

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
	int ep = epoll_create1(EPOLL_CLOEXEC);
	int ls = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	int us = socket(AF_INET, SOCK_DGRAM, 0);
	int one = 1;
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(7777),
				 .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	struct sockaddr_in ua = a;

	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(ls, (void *)&a, sizeof(a)) || listen(ls, 16)) {
		perror("bind/listen");
		return 1;
	}
	ua.sin_port = htons(7778);
	bind(us, (void *)&ua, sizeof(ua));
	struct epoll_event ev = { .events = EPOLLIN, .data.fd = ls };
	epoll_ctl(ep, EPOLL_CTL_ADD, ls, &ev);

	for (unsigned long iter = 0;; iter++) {
		unsigned char msg[3000], back[3000], srv[3000];
		size_t len, got = 0, sgot = 0, sent = 0;
		uint16_t rl;
		struct epoll_event evs[8];
		struct sockaddr_in peer, me;
		socklen_t pl = sizeof(peer), ml = sizeof(me);
		int s = -1, c, rc, err = -1, nd = -1, pending = -1, waits = 0;
		socklen_t el = sizeof(err), nl = sizeof(nd);

		read(rnd, &rl, sizeof(rl));
		len = 500 + rl % 2500;
		read(rnd, msg, len);

		c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
		setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		rc = connect(c, (void *)&a, sizeof(a));
		int cerr = rc ? errno : 0;
		ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
		ev.data.fd = c;
		epoll_ctl(ep, EPOLL_CTL_ADD, c, &ev);

		while (got < len && waits < 200) {
			int n = epoll_wait(ep, evs, 8, 1000);
			waits++;
			for (int i = 0; i < n; i++) {
				int fd = evs[i].data.fd;
				if (fd == ls) {
					s = accept4(ls, (void *)&peer, &pl, SOCK_NONBLOCK | SOCK_CLOEXEC);
					ev.events = EPOLLIN;
					ev.data.fd = s;
					epoll_ctl(ep, EPOLL_CTL_ADD, s, &ev);
				} else if (fd == s) {             /* server: echo */
					ssize_t r = recv(s, srv, sizeof(srv), 0);
					if (r > 0) {
						struct iovec iv[2] = { { srv, r / 2 }, { srv + r / 2, r - r / 2 } };
						struct msghdr mh = { .msg_iov = iv, .msg_iovlen = 2 };
						sendmsg(s, &mh, MSG_NOSIGNAL);
						sgot += r;
					}
				} else if (fd == c) {
					if (evs[i].events & EPOLLOUT && sent < len) {
						size_t piece = len - sent > 700 ? 700 : len - sent;
						ssize_t w = send(c, msg + sent, piece, MSG_NOSIGNAL);
						if (w > 0)
							sent += w;
						if (sent == len) {        /* stop asking for EPOLLOUT */
							ev.events = EPOLLIN;
							ev.data.fd = c;
							epoll_ctl(ep, EPOLL_CTL_MOD, c, &ev);
						}
					}
					if (evs[i].events & EPOLLIN) {
						ioctl(c, FIONREAD, &pending);
						ssize_t r = read(c, back + got, sizeof(back) - got);
						if (r > 0)
							got += r;
					}
				}
			}
		}
		getsockopt(c, SOL_SOCKET, SO_ERROR, &err, &el);
		getsockopt(c, IPPROTO_TCP, TCP_NODELAY, &nd, &nl);
		getsockname(c, (void *)&me, &ml);

		/* one datagram to ourselves */
		unsigned char dg[64];
		struct sockaddr_in from;
		socklen_t fl = sizeof(from);
		sendto(us, msg, 32, 0, (void *)&ua, sizeof(ua));
		ssize_t dn = recvfrom(us, dg, sizeof(dg), 0, (void *)&from, &fl);

		char line[256];
		int n = snprintf(line, sizeof(line),
				 "iter %lu len %zu conn %d sent %zu srv %zu got %zu waits %d hash %016llx same %d "
				 "peer %d me %d err %d nodelay %d dgram %zd from %d %016llx\n",
				 iter, len, cerr, sent, sgot, got, waits,
				 (unsigned long long)fnv(back, got, 14695981039346656037ULL),
				 got == len && !memcmp(msg, back, len),
				 ntohs(peer.sin_port), ntohs(me.sin_port), err, !!nd, dn, ntohs(from.sin_port),
				 (unsigned long long)fnv(dg, dn > 0 ? dn : 0, 7));
		write(1, line, n);
		shutdown(c, SHUT_RDWR);
		close(c);
		if (s >= 0)
			close(s);
		struct timespec d = { 0, 50 * 1000 * 1000 };
		nanosleep(&d, NULL);
	}
}
