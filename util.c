#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <netdb.h>
#include <poll.h>
#include <getopt.h>
#include <libgen.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <net/if.h>
#include <net/ethernet.h>

#include "util.h"
#include "tcp_transport.h"

void
set_blocking_mode(int fd, bool on)
{
	int rc, flag;

	flag = fcntl(fd, F_GETFL);
	if (flag == -1)
		err(1, "fcntl(F_GETFL)");

	if (on)
		flag &= ~O_NONBLOCK;
	else
		flag |= O_NONBLOCK;

	rc = fcntl(fd, F_SETFL, flag);
	if (rc == -1)
		err(1, "fcntl(F_SETFL)");

	flag = fcntl(fd, F_GETFL);
	if (!!(flag & O_NONBLOCK) != !on)
		errx(1, "mode 0x%x, 0x%x != %d", flag, O_NONBLOCK, !on);
}

bool
fd_read_ready(int fd)
{
	struct pollfd pfd = {
		.fd = fd,
		.events = POLLIN,
	};
	int n;

	if (!opt.poll)
		return true;
	
	for (;;) {
		n = poll(&pfd, 1, opt.timeout);
		if (n == 1)
			return true;
		if (n == -1)
			return false;
	}
}

const char *
show_node_addr(struct node *node)
{
	static char host[NI_MAXHOST];
	int rc;

	rc = getnameinfo((struct sockaddr *)&node->addr,
	    (node->family == AF_INET) ? sizeof(struct sockaddr_in) :
					sizeof(struct sockaddr_in6),
	    host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
	if (rc != 0)
		errx(1, "getnameinfo: %s", gai_strerror(rc));
	return host;
}

static bool
name2addr(const char *name, struct node *node, bool local)
{
	struct addrinfo hints, *result, *ai;
	int s, rc;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = node->family;
	hints.ai_socktype = node->socktype;
	node->addrlen = 0;

	rc = getaddrinfo(name, NULL, &hints, &result);
	if (rc != 0)
		errx(1, "getaddrinfo: %s", gai_strerror(rc));

	for (ai = result; ai != NULL; ai = ai->ai_next) {
		if (!local)
			break;

		s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (s == -1)
			continue;

		rc = bind(s, ai->ai_addr, ai->ai_addrlen);
		close(s);

		if (rc == 0)
			break;
	}

	if (ai != NULL) {
		node->addrlen = ai->ai_addrlen;
		node->protocol = ai->ai_protocol;
		memcpy(&node->addr, ai->ai_addr, ai->ai_addrlen);
	}

	freeaddrinfo(result);

	return node->addrlen != 0;
}

void
set_port(struct node *node, int port)
{
	struct sockaddr_in *sin;
	struct sockaddr_in6 *sin6;

	if (node->family == AF_INET6) {
		sin6 = (struct sockaddr_in6 *)&node->addr;
		sin6->sin6_port = htons(port);
	} else {
		sin = (struct sockaddr_in *)&node->addr;
		sin->sin_port = htons(port);
	}
}

void
setup_node(struct node *node, int socktype, const char *hostname)
{
	char name[HOST_NAME_MAX];
	bool local = !hostname;

	if (local) {
		if (gethostname(name, sizeof(name)) == -1)
			err(1, "gethostname");
		hostname = name;
	}

	node->family = AF_INET6;
	node->socktype = socktype;
	if (!name2addr(hostname, node, local)) {
		node->family = AF_INET;
		if (!name2addr(hostname, node, local))
			errx(1, "could not get IP of %s", hostname);
	}
}
