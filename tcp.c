#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "util.h"
#include "tcp_transport.h"

struct node ctrl_root;
struct node ctrl_node;

struct node self_node;

long read_count;
long write_count;
long read_partial;
long write_partial;

#define TIMEOUT_MSEC	10

static void
tcp_normalize(int fd)
{
	int rc;
	int one = 1;
	struct timeval tv = {
		.tv_sec = TIMEOUT_MSEC / 1000,
		.tv_usec = (TIMEOUT_MSEC % 1000) * 1000,
	};
	size_t bufsize = 32 * 1024 * 1024;

	set_blocking_mode(fd, !opt.nonblock);

	rc = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	if (rc == -1)
		err(1, "setsockopt(TCP_NODELAY)");

	rc = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (rc == -1)
		err(1, "setsockopt(SO_RCVTIMEO)");

	rc = setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	if (rc == -1)
		err(1, "setsockopt(SO_SNDTIMEO)");

	rc = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
	if (rc == -1)
		err(1, "setsockopt(SO_SNDBUF)");

	rc = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
	if (rc == -1)
		err(1, "setsockopt(SO_RCVBUF)");
}

int
tcp_wait_accept(struct node *listen)
{
	struct sockaddr_storage ss;
	socklen_t addrlen;
	int fd;

	fd = accept(listen->fd, (struct sockaddr *)&ss, &addrlen);
	if (fd == -1)
		err(1, "accept");

	tcp_normalize(fd);

	return fd;
}

void
tcp_listen(struct node *node, int port)
{
	int fd, rc;
	int one = 1;

	set_port(node, port);

	fd = socket(node->family, node->socktype, node->protocol);
	if (fd == -1)
		err(1, "socket(listen)");

	rc = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (rc == -1)
		err(1, "setsockopt(REUSEADDR)");

	rc = bind(fd, (struct sockaddr *)&node->addr, node->addrlen);
	if (rc == -1)
		err(1, "bind, port %d", port);

	if (opt.zerocopy) {
		rc = setsockopt(fd, SOL_SOCKET, SO_ZEROCOPY, &one, sizeof(one));
		if (rc == -1)
			err(1, "setsockopt(SO_ZEROCOPY)");
	}

	rc = listen(fd, 1);
	if (rc == -1)
		err(1, "listen");

	node->fd = fd;
}

bool
tcp_complete_connect(struct node *node)
{
	int rc;

	rc = connect(node->fd, (struct sockaddr *)&node->addr, node->addrlen);
	if (rc == -1) {
		if (errno == EINPROGRESS || errno == ECONNREFUSED)
			return false;
		err(1, "connect");
	}
	tcp_normalize(node->fd);

	return true;
}

bool
tcp_connect(struct node *node, int port)
{
	int fd, rc;
	int one = 1;

	set_port(node, port);

	fd = socket(node->family, node->socktype, node->protocol);
	if (fd == -1)
		err(1, "socket(connect)");

	node->fd = fd;

	rc = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (rc == -1)
		err(1, "setsockopt(REUSEADDR)");

	if (opt.zerocopy) {
		rc = setsockopt(fd, SOL_SOCKET, SO_ZEROCOPY, &one, sizeof(one));
		if (rc == -1)
			err(1, "setsockopt(SO_ZEROCOPY)");
	}

	return tcp_complete_connect(node);
}

void
tcp_wait_connect(struct node *node, int port)
{
	int count = 50;
	bool connected;

	connected = tcp_connect(node, port);
	while (!connected && count--) {
		usleep(200 * 1000);
		connected = tcp_complete_connect(node);
	}
	if (!connected)
		errx(1, "can't connect to %s:%d", show_node_addr(node), port);
}

void
tcp_write(int fd, void *buf, int len)
{
	int n;

	write_count++;
	for (;;) {
		n = write(fd, buf, len);
		if (n == len)
			break;
		if (n < 1)
			err(1, "tcp_write: %d", n);
		len -= n;
		buf += n;
		write_partial++;
	}
}

void
tcp_read(int fd, void *buf, int len)
{
	int n;

	read_count++;
	for (;;) {
		n = read(fd, buf, len);
		if (n == len)
			break;
		if (n == -1 && errno == EAGAIN)
			continue;
		if (n < 1)
			err(1, "tcp_read: %d", n);
		len -= n;
		buf += n;
		read_partial++;
	}
}
