#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <pthread.h>

#include "util.h"
#include "driver.h"

#ifndef MSG_ZEROCOPY
#define MSG_ZEROCOPY     0x4000000  
#endif

long drv_read_count[2];
long drv_write_count[2];
long drv_read_partial[2];
long drv_write_partial[2];
long mutex_stall_time;
long mutex_stall_count[2];

#define EV_COUNT	64
#define EV_TIMEOUT	10

#define array_size(x)	(sizeof(x) / sizeof((x)[0]))

typedef void (*ev_callback)(int rank, int events);

#define N_REQS	256

struct drv_queue {
	pthread_mutex_t mutex;
	int head;
	int tail;
	int size;
	struct drv_req *req[N_REQS];
};

static struct drv_req request[N_REQS];
static struct drv_queue req_stack;
static struct drv_queue send_queue;
static struct drv_queue recv_queue;

static bool
drv_empty(struct drv_queue *q)
{
	return q->head == q->tail;
}

static struct drv_req *
drv_pop(struct drv_queue *q)
{
	struct drv_req *req;

	pthread_mutex_lock(&q->mutex);

	if (!q->head)
		errx(1, "pop on empty stack");
	req = q->req[--q->head];

	pthread_mutex_unlock(&q->mutex);
	return req;
}

static void
drv_push(struct drv_queue *q, struct drv_req *req)
{
	pthread_mutex_lock(&q->mutex);

	if (q->head == q->size)
		errx(1, "push on full stack");
	q->req[q->head++] = req;

	pthread_mutex_unlock(&q->mutex);
}

static void
drv_queue(struct drv_queue *q, struct drv_req *req)
{
	bool empty;
	int next;

	pthread_mutex_lock(&q->mutex);

	empty = drv_empty(q);

	next = (q->tail + 1) == q->size ? 0 : (q->tail + 1);
	if (next == q->head)
		errx(1, "queue overflow");
	q->req[q->tail] = req;
	q->tail = next;

	if (empty) {
		if (req->send)
			drv_register(req, EPOLLOUT);
		else
			drv_register(req, EPOLLIN);
	}

	pthread_mutex_unlock(&q->mutex);
}

static bool
drv_dequeue(struct drv_queue *q)
{
	struct drv_req *req = NULL;
	bool empty;

	if (drv_empty(q))
		errx(1, "dequeue on empty queue");

	pthread_mutex_lock(&q->mutex);

	req = q->req[q->head++];
	if (q->head == q->size)
		q->head = 0;

	empty = drv_empty(q);
	if (empty)
		drv_unregister(req->fd);

	pthread_mutex_unlock(&q->mutex);
	return empty;
}

static struct drv_req *
drv_peek(struct drv_queue *q)
{
	struct drv_req *req = NULL;

	if (!drv_empty(q))
		req = q->req[q->head];

	return req;
}

static bool
drv_send_req(struct drv_req *req, int tr)
{
	int n;

        drv_write_count[tr]++;
//        n = write(req->fd, req->iov.iov_base, req->iov.iov_len);
        n = send(req->fd, req->iov.iov_base, req->iov.iov_len,
	    opt.zerocopy ? MSG_ZEROCOPY : 0);

        if (n == req->iov.iov_len)
                return true;
        if (n < 1)
                err(1, "%d write: %zu", -1, req->iov.iov_len);

        req->iov.iov_len -= n;
        req->iov.iov_base += n;
        drv_write_partial[tr]++;

        return false;
}

static void
drv_send_cb(struct drv_req *req, int events)
{
	bool empty = false;

printf("SEND CB....\n");

	while (!empty) {
		req = drv_peek(&send_queue);
		if (!drv_send_req(req, 1)) {
			return;
		}
		empty = drv_dequeue(&send_queue);
		drv_push(&req_stack, req);
	}
}

static void
epoll_send(int fd, struct iovec *iov, int area)
{
	struct drv_req *req = drv_pop(&req_stack);

	req->fd = fd;
	req->iov = *iov;
	req->callback = drv_send_cb;
	req->send = true;

#if 1
	if (drv_empty(&send_queue) && drv_send_req(req, 0)) {
		drv_push(&req_stack, req);
		return;
	}
#endif
	
	drv_queue(&send_queue, req);
//        drv_register(req, EPOLLOUT | EPOLLONESHOT);
}

static bool
drv_recv_req(struct drv_req *req, int tr)
{
        int n;

        drv_read_count[tr]++;
//        n = read(req->fd, req->iov.iov_base, req->iov.iov_len);
        n = recv(req->fd, req->iov.iov_base, req->iov.iov_len, 0);
//printf("RECV: %d / %zu bytes\n", n, req->iov.iov_len);

        if (n == req->iov.iov_len) {
		req->done(req->arg);
                return true;
	}
        if (n < 1) {
		if (errno == EAGAIN)
			return false;
                err(1, "%d read: %zu", -1, req->iov.iov_len);
	}

        req->iov.iov_len -= n;
        req->iov.iov_base += n;
        drv_read_partial[tr]++;

        return false;
}

static void
drv_recv_cb(struct drv_req *req, int events)
{
	bool empty = false;

	while (!empty) {
		req = drv_peek(&recv_queue);
		if (!drv_recv_req(req, 1)) {
			return;
		}
		empty = drv_dequeue(&recv_queue);
		drv_push(&req_stack, req);
	}
}

static void
epoll_recv(int fd, struct iovec *iov, int area, void (*done)(int), int arg)
{
	struct drv_req *req = drv_pop(&req_stack);

	req->fd = fd;
	req->arg = arg;
	req->iov = *iov;
	req->callback = drv_recv_cb;
	req->send = false;
	req->done = done;

#if 0
	if (drv_empty(&recv_queue) && drv_recv_req(req, 0)) {
		drv_push(&req_stack, req);
		return;
	}
#endif
	
	drv_queue(&recv_queue, req);
//        drv_register(req, EPOLLIN | EPOLLONESHOT);
}

struct {
	int ep;
	bool stop;
	int timeout;
} drv;

static void
epoll_start(void)
{
	int i, rc;

	rc = epoll_create(1);
	if (rc == -1)
		err(1, "epoll_create()");

	drv.ep = rc;
	drv.timeout = EV_TIMEOUT;

	req_stack.size = N_REQS;
	send_queue.size = N_REQS;
	recv_queue.size = N_REQS;
	pthread_mutex_init(&req_stack.mutex, NULL);
	pthread_mutex_init(&send_queue.mutex, NULL);
	pthread_mutex_init(&recv_queue.mutex, NULL);

	for (i = 0; i < N_REQS; i++)
		drv_push(&req_stack, &request[i]);
}

static void
epoll_stop(void)
{
	drv.stop = true;
}

static void
epoll_register_buffer(struct iovec *iov)
{
}

static void
epoll_register(struct drv_req *req, int events)
{
	struct epoll_event ev;
	int rc;

	ev.events = events;
	ev.data.ptr = req;

	rc = epoll_ctl(drv.ep, EPOLL_CTL_ADD, req->fd, &ev);
	if (rc == -1 && errno == EEXIST)
		rc = epoll_ctl(drv.ep, EPOLL_CTL_MOD, req->fd, &ev);
	if (rc == -1)
		err(1, "epoll_add/mod()");
}

static void
epoll_unregister(int fd)
{
	int rc;

	rc = epoll_ctl(drv.ep, EPOLL_CTL_DEL, fd, NULL);
	if (rc == -1)
		err(1, "epoll_del()");
}

static void *
epoll_loop(void *arg)
{
	struct epoll_event ev[EV_COUNT];
	struct drv_req *req;
	int i, n;
	int count = 0;

	while (!drv.stop) {
		n = epoll_wait(drv.ep, ev, array_size(ev), drv.timeout);
		if (n == 0)
			continue;
		if (n == -1) {
			if (errno == EINTR)
				continue;
			err(1, "epoll_wait");
		}
		count++;
		for (i = 0; i < n; i++) {
			req = ev[i].data.ptr;
			req->callback(req, ev[i].events);
		}
	}
	printf("driver: %d\n", count);
	close(drv.ep);

	return NULL;
}

struct io_driver epoll_driver = {
	.start = epoll_start,
	.stop = epoll_stop,
	.req_register = epoll_register,
	.unregister = epoll_unregister,
	.register_buffer = epoll_register_buffer,
	.loop = epoll_loop,
	.send = epoll_send,
	.recv = epoll_recv,
};
