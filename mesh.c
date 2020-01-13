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
#include <sys/epoll.h>
#include <net/if.h>
#include <net/ethernet.h>
#include <sys/prctl.h>

#include "util.h"
#include "driver.h"
#include "tcp_transport.h"

/* self node is the root connection */
#define MAX_NODES	128
static struct node mesh_node[MAX_NODES];
static struct driver *drv;
static struct node root;
static int wait_count;

static int self_rank;

enum msg_tag {
	TAG_REGISTER = 1,
	TAG_ACK,
	TAG_QUERY,
	TAG_ANSWER,
	TAG_NODE_UP,
	TAG_ERROR,
};

struct node_msg {
	enum msg_tag tag;
	struct node node;
};

static void
mesh_waitrecv(int fd, struct node_msg *msg)
{
	int n;

	do {
		n = read(fd, msg, sizeof(*msg));
	} while (n == -1 && errno == EAGAIN);
	if (n == -1)
		err(1, "read");
	if (n == 0)
		errx(1, "waitrecv, EOF");
	if (n != sizeof(*msg))
		errx(1, "waitrecv, short read");
}

static void
mesh_send(int fd, struct node_msg *msg)
{
	int n;

	n = write(fd, msg, sizeof(*msg));
	if (n == -1)
		err(1, "write");
	if (n != sizeof(*msg))
		errx(1, "register_node, short write: %d", n);
}

static int
mesh_next_rank(void)
{
	int i;

	for (i = 0; i < MAX_NODES; i++)
		if (!mesh_node[i].addrlen)
			return i;
	return -1;
}


static void
mesh_handle_register(struct node_msg *msg)
{
	struct node *node;
	int rank;

#if 0
	if (msg->node.rank >= opt.size) {
		msg->tag = TAG_ERROR;
		warnx("rank %d > mesh size %d", msg->node.rank, opt.size);
		return;
	}
#endif

	rank = mesh_next_rank();

	node = &mesh_node[rank];
	msg->node.rank = rank;
	*node = msg->node;

	msg->tag = TAG_ACK;
}

static void
mesh_handle_query(struct node_msg *msg)
{
	struct node *node;
	int rank = msg->node.rank;

	if (rank > opt.size) {
		msg->tag = TAG_ERROR;
		return;
	}
	node = &mesh_node[rank];
	if (!node->addrlen) {
		msg->tag = TAG_ERROR;
		return;
	}
	msg->node = *node;

	msg->tag = TAG_ANSWER;
}

static void
mesh_internal_message(struct node_msg *msg)
{

	switch (msg->tag) {
	case TAG_QUERY:
		mesh_handle_query(msg);
		break;
	case TAG_REGISTER:
		mesh_handle_register(msg);
		break;
	case TAG_NODE_UP:
		wait_count++;
		msg->tag = TAG_ACK;
		break;
	default:
		msg->tag = TAG_ERROR;
		break;
	}
}

static void
mesh_handle_message(int fd)
{
	struct node_msg msg;

	mesh_waitrecv(fd, &msg);
	mesh_internal_message(&msg);
	mesh_send(fd, &msg);
}

static void
mesh_message(struct node_msg *msg)
{
	if (!root.addrlen) {
		mesh_internal_message(msg);
		return;
	}
	mesh_send(root.fd, msg);
	mesh_waitrecv(root.fd, msg);
}

static int
mesh_register(struct node *node)
{
	struct node_msg msg = {
		.tag = TAG_REGISTER,
		.node = *node,
	};

	mesh_message(&msg);
	if (msg.tag != TAG_ACK)
		err(1, "register was not ack'd");

	mesh_node[msg.node.rank] = msg.node;
	return msg.node.rank;
}

static void
mesh_node_up(int rank)
{
	struct node_msg msg = {
		.tag = TAG_NODE_UP,
		.node.rank = rank,
	};

	mesh_message(&msg);
	if (msg.tag != TAG_ACK)
		err(1, "up was not ack'd");
}

static void
mesh_query(int rank)
{
	struct node_msg msg = {
		.tag = TAG_QUERY,
		.node.rank = rank,
	};

	mesh_message(&msg);
	if (msg.tag != TAG_ANSWER)
		err(1, "unexpected tag: %d != ANSWER", msg.tag);

	mesh_node[rank] = msg.node;
}

static void
mesh_accept_cb(int rank, int events)
{
	int fd;

	fd = tcp_wait_accept(&mesh_node[self_rank]);
	set_blocking_mode(fd, true);
	tcp_read(fd, &rank, sizeof(rank));
	mesh_node[rank].fd = fd;
	mesh_node[rank].rank = rank;
	wait_count++;
}

static bool
check_wait_count(void)
{
	return (wait_count == opt.size);
}

struct node *
mesh_get_node(int rank)
{
	return &mesh_node[rank];
}

struct node *
mesh_match_addr(struct sockaddr_storage *ss, socklen_t addrlen)
{
	int i;

	for (i = 0; i < MAX_NODES; i++)
		if (memcmp(&mesh_node[i].addr, ss, addrlen) == 0)
			return &mesh_node[i];
	return NULL;
}

static void
mesh_wait_cb(int rank, int events)
{
	int one;
	
	tcp_read(mesh_node[rank].fd, &one, sizeof(one));
	wait_count++;
}

void
mesh_wait(void)
{
	int rank, one;

	if (self_rank == 0) {
		wait_count = 1;
		for (rank = 1; rank < opt.size; rank++) {
			ev_register(drv, mesh_node[rank].fd, rank,
			    mesh_wait_cb, EPOLLIN);
		}
		ev_loop(drv, check_wait_count);
		for (rank = 1; rank < opt.size; rank++)
			tcp_write(mesh_node[rank].fd, &one, sizeof(one));
	} else {
		tcp_write(mesh_node[0].fd, &one, sizeof(one));
		tcp_read(mesh_node[0].fd, &one, sizeof(one));
	}
}

static void
mesh_connect_cb(int rank, int events)
{
	struct node *node = &mesh_node[rank];

	if (events && !tcp_complete_connect(node))
		return;
	set_blocking_mode(node->fd, true);
	tcp_write(node->fd, &self_rank, sizeof(self_rank));
	wait_count++;
}

/*
 * Open connection to control, register self node.
 */
int
mesh_join(void)
{
        int socktype = SOCK_STREAM;
	struct node *node, self;
	int rank;

        setup_node(&self, socktype, NULL);
        setup_node(&root, socktype, opt.roothost);

        tcp_wait_connect(&root, opt.rootport);
	set_blocking_mode(root.fd, true);
	self_rank = mesh_register(&self);
	rank = self_rank;

	node = mesh_get_node(rank);
	drv = ev_start(opt.size);

	wait_count = 1;
        tcp_listen(node, opt.port + rank);
	ev_register(drv, node->fd, rank, mesh_accept_cb, EPOLLIN);
        mesh_node_up(rank);

	/* open connections to lower numbered nodes */
	for (rank = self_rank; rank--; ) {
                mesh_query(rank);
		ev_poll(drv);
                if (!tcp_connect(&mesh_node[rank], opt.port + rank)) {
			ev_register(drv, mesh_node[rank].fd, rank,
			    mesh_connect_cb, EPOLLIN);
			continue;
		}
		mesh_connect_cb(rank, 0);
        }

	ev_loop(drv, check_wait_count);

	mesh_wait();

	return self_rank;
}

static void
mesh_ctrl(void)
{
	struct pollfd pfd[opt.size];
        int socktype = SOCK_STREAM;
	int i, fd, n;

	prctl(PR_SET_NAME, "mesh_ctrl", 0, 0, 0);

        setup_node(&root, socktype, NULL);
        tcp_listen(&root, opt.rootport);

	for (i = 0; i < opt.size; i++) {
		fd = tcp_wait_accept(&root);
		set_blocking_mode(fd, true);
		mesh_handle_message(fd);
		mesh_handle_message(fd);
		pfd[i].fd = fd;
		pfd[i].events = POLLIN;
	}
printf("CTRL: All nodes up\n");

	for (;;) {
		n = poll(pfd, opt.size, -1);	//opt.timeout);
		if (n == -1)
			err(1, "poll");
		for (i = 0; i < opt.size; i++) {
			if ((pfd[i].revents & POLLIN) == 0)
				continue;
			mesh_handle_message(pfd[i].fd);
			if (--n == 0)
				break;
		}
	}
	exit(0);
}

void
mesh_init(void)
{
	pid_t pid;

	pid = fork();
	if (pid == -1)
		err(1, "fork");
	if (pid == 0)
		mesh_ctrl();
}
