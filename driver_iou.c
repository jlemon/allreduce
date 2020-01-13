#include <err.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/poll.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <pthread.h>

#include "util.h"
#include "driver.h"
#include "liburing.h"

#define N_REQS	256

#define USE_FIXED_RECV 0
#define USE_FIXED_SEND 0

#define array_size(x)	(sizeof(x) / sizeof((x)[0]))

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

pthread_mutex_t io_mutex;

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

	if (!req)
		errx(1, "WTF, null req for %d\n", q->head);

	pthread_mutex_unlock(&q->mutex);
	return req;
}

static void
drv_push(struct drv_queue *q, struct drv_req *req)
{
	pthread_mutex_lock(&q->mutex);

	if (req == NULL)
		errx(1, "WTF, pushing null req!");

	if (q->head == q->size)
		errx(1, "push on full stack");
	q->req[q->head++] = req;

	pthread_mutex_unlock(&q->mutex);
}

static void
drv_queue(struct drv_queue *q, struct drv_req *req)
{
	int next;

	pthread_mutex_lock(&q->mutex);

	next = (q->tail + 1) == q->size ? 0 : (q->tail + 1);
	if (next == q->head)
		errx(1, "queue overflow");
	q->req[q->tail] = req;
	q->tail = next;

	pthread_mutex_unlock(&q->mutex);
}

static struct drv_req *
drv_dequeue(struct drv_queue *q)
{
	struct drv_req *req = NULL;

	pthread_mutex_lock(&q->mutex);

	if (drv_empty(q))
		goto unlock;
	req = q->req[q->head++];
	if (q->head == q->size)
		q->head = 0;

	if (drv_empty(q))
		drv_unregister(req->fd);

unlock:
	pthread_mutex_unlock(&q->mutex);
	return req;
}

static struct drv_req *
drv_peek(struct drv_queue *q)
{
	struct drv_req *req = NULL;

	if (!drv_empty(q))
		req = q->req[q->head];

	return req;
}

struct {
	struct io_uring ring;
	bool stop;
	int timeout;
} drv;

void
iou_start(void)
{
	unsigned flags = 0;
	int fd, i;

#if 0
	/* This is incompatible with IORING_OP_POLL */
	if (opt.poll)
		flags |= IORING_SETUP_IOPOLL;
#endif

#if 0
	flags |= IORING_SETUP_SQPOLL;	/* submission polling kernel thread */
	flags |= IORING_SETUP_SQ_AFF;	/* bind poll thread to specific cpu */
#endif

	/* simplified api that only sets flags. */
	fd = io_uring_queue_init(opt.iou.entries, &drv.ring, flags);
	if (fd < 0)
		err_with(fd, "iou_queue_init");

	req_stack.size = N_REQS;
	send_queue.size = N_REQS;
	recv_queue.size = N_REQS;
	pthread_mutex_init(&req_stack.mutex, NULL);
	pthread_mutex_init(&send_queue.mutex, NULL);
	pthread_mutex_init(&recv_queue.mutex, NULL);
	pthread_mutex_init(&io_mutex, NULL);

	for (i = 0; i < N_REQS; i++)
		drv_push(&req_stack, &request[i]);

printf("IOU_START, ring_fd: %d\n", drv.ring.ring_fd);
}

void
iou_stop(void)
{
	drv.stop = true;
}

void
iou_req_register(struct drv_req *req, int events)
{
}

void
iou_unregister(int fd)
{
}

static struct iovec rbuf_iov[8];
static int rbuf_count;

void
rbuf_rangecheck(int idx, struct iovec *iov)
{
#if 0
	uint8_t *start = iov->iov_base;

	printf("RC %d range[%p,%p], iov[%p,%p]\n",
	    idx, rbuf[idx].start, rbuf[idx].end, start, start + iov->iov_len);
	if (idx >= rbuf_count)
		errx(1, "index %d >= max %d\n", idx, rbuf_count);
	if ((start >= rbuf[idx].start) &&
	    (start + iov->iov_len) <= rbuf[idx].end)
		return;
	errx(1, "iov out of range, area %d range[%p,%p], iov[%p,%p]\n",
	    idx, rbuf[idx].start, rbuf[idx].end, start, start + iov->iov_len);
#endif
}

void
iou_register_buffer(struct iovec *iov)
{
	struct io_uring *ring = &drv.ring;
	int rc;

	if (!iov->iov_base) {
		rc = io_uring_register_buffers(ring, rbuf_iov, rbuf_count);
		if (rc < 0)
			err_with(rc, "io_uring_register_buffers");
		return;
	}
	if (rbuf_count == array_size(rbuf_iov))
		errx(1, "rbuf_count == maximum %lu", array_size(rbuf_iov));

	rbuf_iov[rbuf_count++] = *iov;
}

static void
iou_recv_req(struct drv_req *req)
{
	struct io_uring *ring = &drv.ring;
	struct io_uring_sqe *sqe;
	int rc;

	pthread_mutex_lock(&io_mutex);

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_poll_add(sqe, req->fd, POLLIN);
	io_uring_sqe_set_flags(sqe, IOSQE_IO_LINK);

	sqe = io_uring_get_sqe(ring);
#if USE_FIXED_RECV
	rbuf_rangecheck(req->area, &req->iov);
	io_uring_prep_read_fixed(sqe, req->fd,
	    req->iov.iov_base, req->iov.iov_len, 0, req->area);
#else
	io_uring_prep_readv(sqe, req->fd, &req->iov, 1, 0);
#endif
	io_uring_sqe_set_data(sqe, req);

//printf("IOU_RECV_REQ  %p %zu\n", req->iov.iov_base, req->iov.iov_len);
	rc = io_uring_submit(ring);
/* rc may be zero if something else picked up the submission */
	if (rc != 2)
		err(1, "recv submit: %d", rc);

	pthread_mutex_unlock(&io_mutex);
}

static void
drv_recv_cb(struct drv_req *req, int events)
{
//printf("RECV COMPLETE\n");
	req->done(req->arg);
	req = drv_dequeue(&recv_queue);
	drv_push(&req_stack, req);
	req = drv_peek(&recv_queue);
	if (req)
		iou_recv_req(req);
}

void
iou_recv(int fd, struct iovec *iov, int area, void (*done)(int), int arg)
{
	struct drv_req *req = drv_pop(&req_stack);
	bool empty;

	req->fd = fd;
	req->arg = arg;
	req->iov = *iov;
	req->callback = drv_recv_cb;
	req->send = false;
	req->done = done;
	req->area = area;

	empty = drv_empty(&recv_queue);
	drv_queue(&recv_queue, req);
	if (empty)
		iou_recv_req(req);
}

static void
iou_send_req(struct drv_req *req)
{
	struct io_uring *ring = &drv.ring;
	struct io_uring_sqe *sqe;
	int rc;

	pthread_mutex_lock(&io_mutex);

	sqe = io_uring_get_sqe(ring);
#if USE_FIXED_SEND
	rbuf_rangecheck(req->area, &req->iov);
	io_uring_prep_write_fixed(sqe, req->fd,
	    req->iov.iov_base, req->iov.iov_len, 0, req->area);
#else
	io_uring_prep_writev(sqe, req->fd, &req->iov, 1, 0);
#endif
	io_uring_sqe_set_data(sqe, req);

//printf("IOU_SEND_REQ  %p %zu\n", req->iov.iov_base, req->iov.iov_len);
	rc = io_uring_submit(ring);
/* rc may be zero if something else picked up the submission */
	if (rc != 1)
		err(1, "send submit: %d", rc);

	pthread_mutex_unlock(&io_mutex);
}

static void
drv_send_cb(struct drv_req *req, int events)
{
//printf("SEND COMPLETE\n");
//	req->done(req->arg);
	req = drv_dequeue(&send_queue);
	drv_push(&req_stack, req);
	req = drv_peek(&send_queue);
	if (req)
		iou_send_req(req);
}

void
iou_send(int fd, struct iovec *iov, int area)
{
	struct drv_req *req = drv_pop(&req_stack);
	bool empty;

	req->fd = fd;
	req->iov = *iov;
	req->callback = drv_send_cb;
	req->send = true;
	req->area = area;

	empty = drv_empty(&send_queue);
	drv_queue(&send_queue, req);
	if (empty)
		iou_send_req(req);
}

static void
iou_handle_cqe(struct io_uring_cqe *cqe)
{
	struct drv_req *req;

	req = io_uring_cqe_get_data(cqe);
	if (!req)
		return;			/* POLLIN link, ignore, continue */

	if (cqe->res != req->iov.iov_len) {
		if (cqe->res == 0) {
			printf("type: %s\n", req->send ? "SEND" : "RECV");
			errx(1, "WTF, returned cqe with 0 bytes");
		}
		/* short operation, resubmit */
		req->iov.iov_base += cqe->res;
		req->iov.iov_len -= cqe->res;
		if (req->send)
			iou_send_req(req);
		else
			iou_recv_req(req);
		return;
	}
	req->callback(req, 0);
}

void *
iou_loop(void *arg)
{
	struct io_uring *ring = &drv.ring;
	struct io_uring_cqe *cqe;
	int rc;

	while (!drv.stop) {
		rc = io_uring_wait_cqe(ring, &cqe);
		if (rc < 0)
			err(1, "wait_cqe: %d", rc);
		if (cqe->res < 0) {
			struct drv_req *req = io_uring_cqe_get_data(cqe);
			if (req) {
				printf("res: %p %zu\n",
				     req->iov.iov_base, req->iov.iov_len);
				printf("type: %s\n", req->send ? "SEND" : "RECV");
			}
			err_with(cqe->res, "res iou_loop");
		}
		iou_handle_cqe(cqe);
		io_uring_cqe_seen(ring, cqe);
	}

	return NULL;
}

struct io_driver iou_driver = {
	.start = iou_start,
	.stop = iou_stop,
	.req_register = iou_req_register,
	.unregister = iou_unregister,
	.register_buffer = iou_register_buffer,
	.loop = iou_loop,
	.send = iou_send,
	.recv = iou_recv,
};
