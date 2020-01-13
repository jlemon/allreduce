#include <err.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/user.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <pthread.h>

#include "sample.h"
#include "driver.h"
#include "util.h"
#include "mesh.h"
#include "tcp_transport.h"

struct {
	unsigned element_size;		/* each element size */
	unsigned tensor_elements;	/* count of elements in tensor */

	unsigned chunk_size;		/* byte size of one chunk */
	unsigned chunk_elements;	/* elements per chunk */
	unsigned chunk_count;		/* how many chunks allocated */

	unsigned tensor_size;		/* byte size of tensor */
	unsigned tensor_count;		/* chunks per tensor */

	unsigned rank_elements;		/* elements per rank */
	unsigned rank_count;		/* chunks per rank */

	unsigned element_modulo;		/* chunks per rank */

	int size;
	int rank;

	uint8_t *tensor;
	uint8_t *chunk;

	struct node *prev;
	struct node *next;
} a = {
	.element_size = sizeof(unsigned),
	.tensor_elements = 7208960,

	.chunk_size = 32 * 1024,
	.chunk_count = 8,
};

/* base + limit are in terms of elements */
struct range {
	int rank;
	int base;
	int stop;
	int count;
};

struct tmut {
	pthread_mutex_t mutex;
	const char *label;
	unsigned long start;
	unsigned long stall_time[2];
	unsigned long stall_count[2];
	unsigned long hold_time;
	unsigned long hold_count;
};

static void
mutex_init(struct tmut *t, char *name)
{
	memset(t, 0, sizeof(*t));
	t->label = strdup(name);
	pthread_mutex_init(&t->mutex, NULL);
}

static void
mutex_lock(struct tmut *t, int ph)
{
	unsigned long start, delay;

	start = nsec();
	if (pthread_mutex_trylock(&t->mutex)) {
		delay = start;
		pthread_mutex_lock(&t->mutex);
		start = nsec();
		t->stall_time[ph] += start - delay;
		t->stall_count[ph]++;
	}
	t->hold_count++;
	t->start = start;
}

static void
mutex_unlock(struct tmut *t)
{
	t->hold_time += nsec() - t->start;
	pthread_mutex_unlock(&t->mutex);
}

struct foo {
	struct tmut mutex;
	struct iovec chunk;
	struct iovec iov;
};
#define FOO_COUNT	256
struct foo foo_chunk[FOO_COUNT];
static int head_foo = 0;
static int tail_foo = 0;

unsigned long
safediv(unsigned long a, unsigned long b)
{
	return b ? (a / b) : 0;
}

void
mutex_stats(void)
{
	struct tmut *t;
	int i;

	for (i = 0; i < FOO_COUNT; i++) {
		t = &foo_chunk[i].mutex;
		if (t->hold_count == 0)
			continue;
		printf("mutex %s: hold(%lu/%lu=%lu)"
			" stall(%lu/%lu=%lu)"
			" stall(%lu/%lu=%lu)\n",
		    t->label,
		    t->hold_time / 1000, t->hold_count,
		    t->hold_time / (t->hold_count * 1000),
		    t->stall_time[0] / 1000, t->stall_count[0],
		    safediv(t->stall_time[0], (t->stall_count[0] * 1000)),
		    t->stall_time[1] / 1000, t->stall_count[1],
		    safediv(t->stall_time[1], (t->stall_count[1] * 1000)));
	}
}


static void *
map_buffer(unsigned size)
{
	void *buffer;

	buffer = mmap(NULL, size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buffer == MAP_FAILED)
		err(1, "mmap");
	return buffer;
}

static void
buffer_register(void *buffer, unsigned size)
{
#if 1
	struct iovec iov = {
		.iov_base = buffer,
		.iov_len = size,
	};

	drv_register_buffer(&iov);
#endif
}

static void
allreduce_alloc(void)
{
	unsigned size;
	char label[80];

	size = a.element_size * a.tensor_elements;
	a.tensor_size = size;

	size = roundup(size, PAGE_SIZE);
	a.tensor = map_buffer(size);
	buffer_register(a.tensor, size);

	size = a.chunk_size * a.chunk_count;
	size = roundup(size, PAGE_SIZE);
	a.chunk = map_buffer(size);
	buffer_register(a.chunk, size);

	buffer_register(NULL, 0);		/* flush */

	/* XXX add sanity checks */
	for (int i = 0; i < FOO_COUNT; i++) {
		sprintf(label, "recv %d", i);
		mutex_init(&foo_chunk[i].mutex, label);
	}
}

#define assign_if_set(x, y)	if (y) x = y

void
allreduce_init(int rank, int size)
{

	a.rank = rank;
	a.size = size;
	a.prev = mesh_get_node((rank + 1) % size);
	a.next = mesh_get_node(rank == 0 ? size - 1 : rank - 1);

	a.tensor_elements = opt.elements;
	assign_if_set(a.chunk_size, opt.chunk_size);
	assign_if_set(a.chunk_count, opt.chunk_count);
	a.chunk_count = MAX(2, a.chunk_count);

printf("tensor_elements: %d\n", a.tensor_elements);
	/*
	 * Calculate
	 *  maximum elements inflight for this rank,
	 *    chunk size, assuming a minimum of 2 chunks
	 */
	a.rank_elements = howmany(a.tensor_elements, a.size);
printf("rank elements: %d\n", a.rank_elements);

	a.chunk_elements = a.chunk_size / a.element_size;
	if (!a.chunk_elements)
		a.chunk_elements = a.rank_elements;
printf("initial chunk_elements: %d\n", a.chunk_elements);

	a.rank_count = howmany(a.rank_elements, a.chunk_elements);
	a.rank_count = MAX(2, a.rank_count);
printf("rank count: %d\n", a.rank_count);

	a.chunk_elements = howmany(a.rank_elements, a.rank_count);
printf("chunk elts: %d\n", a.chunk_elements);

	a.chunk_size = a.chunk_elements * a.element_size;
printf("chunk size: %d\n", a.chunk_size);

	a.tensor_count = a.rank_count * a.size;
printf("tensor chunks: %d\n", a.tensor_count);

	a.chunk_count = MIN(a.rank_count, a.chunk_count);
printf("chunk count: %d\n", a.chunk_count);

	allreduce_alloc();

	printf("tensor: %d, chunks: %d, chunk_elements: %d chunk_size: %d\n",
	    a.tensor_elements, a.chunk_count, a.chunk_elements, a.chunk_size);
}

static int
assign_chunk(int pos, int *base, int *len)
{
	int next, count;

	count = a.chunk_elements;
	next = pos + 1;
	if (next == a.tensor_count) {
		count = a.tensor_elements - pos * a.chunk_elements;
		next = 0;
	}
	*len = count;
	*base = pos * a.chunk_elements;

	return next;
}

static int
pos2iov(int pos, struct iovec *iov, uint8_t *region, int size)
{
	int base, count;

	pos = assign_chunk(pos, &base, &count);
	iov->iov_base = region + base * a.element_size;
	iov->iov_len = count * size;

	return pos;
}

#if 0
static void
debug_iov(const char *label, uint8_t *region, struct iovec *iov, int size)
{
	int base, len;

	base = ((uint8_t *)iov->iov_base - region) / a.element_size;
	len = iov->iov_len / size;

//	if (a.rank == 0)
		printf("%d %s: [%d, %d]\n", a.rank, label, base, len);
}
#else
static void
debug_iov(const char *label, uint8_t *region, struct iovec *iov, int size)
{
}
#endif

static int
send_chunk(int pos)
{
	struct iovec iov;

//	printf("SEND: [%d, %d] pos %d\n", a.rank, a.next->rank, pos);
	pos = pos2iov(pos, &iov, a.tensor, a.element_size);
	debug_iov("SEND", a.tensor, &iov, a.element_size);

	drv_send(a.next->fd, &iov, 0);

	return pos;
}

#define container_of(ptr, type, member) ({			\
	const typeof( ((type *)0)->member ) *__mptr = (ptr);	\
	(type *)( (char *)__mptr - ((size_t) &((type *)0)->member));})

static void
recv_complete(int slot)
{
	struct foo *f;

	f = &foo_chunk[slot];

	mutex_unlock(&f->mutex);
}

static int
start_recv(int pos)
{
	struct foo *f;

	f = &foo_chunk[tail_foo];
	mutex_lock(&f->mutex, 0);
	
//	printf("RECV: [%d, %d] pos %d\n", a.rank, a.prev->rank, pos);
	pos = pos2iov(pos, &f->chunk, a.tensor, 1);

	f->iov.iov_base = &a.chunk[tail_foo * a.chunk_size];
	f->iov.iov_len = f->chunk.iov_len * a.element_size;

	drv_recv(a.prev->fd, &f->iov, 1, recv_complete, tail_foo);

	tail_foo++;
	tail_foo = tail_foo == a.chunk_count ? 0 : tail_foo;

	return pos;
}

static int
start_inplace_recv(int pos)
{
	struct foo *f;

	f = &foo_chunk[tail_foo];
	mutex_lock(&f->mutex, 0);
	
//	printf("INRECV: [%d, %d] pos %d\n", a.rank, a.prev->rank, pos);
	pos = pos2iov(pos, &f->iov, a.tensor, a.element_size);

	drv_recv(a.prev->fd, &f->iov, 0, recv_complete, tail_foo);

	tail_foo++;
	tail_foo = tail_foo == a.chunk_count ? 0 : tail_foo;

	return pos;
}

static void
finish_recv(struct iovec *chunk, struct iovec *iov)
{
	struct foo *f;

	f = &foo_chunk[head_foo];
	mutex_lock(&f->mutex, 1);

	*chunk = f->chunk;
	*iov = f->iov;

	mutex_unlock(&f->mutex);

	head_foo++;
	head_foo = head_foo == a.chunk_count ? 0 : head_foo;
}

#if 0
static void
recv_chunk(struct range *r, struct iovec *chunk, struct iovec *iov)
{
	finish_recv(chunk, iov);
	start_recv(r);
}
#endif

static void
apply_chunk(struct iovec *chunk, struct iovec *iov)
{
	unsigned *tensor, *data;
	int i;

	tensor = chunk->iov_base;
	data = (unsigned *)iov->iov_base;
	for (i = 0; i < chunk->iov_len; i++)
		tensor[i] += data[i];
}

#if 0
static void
copy_chunk(struct iovec *chunk, struct iovec *iov)
{
	memcpy(chunk->iov_base, iov->iov_base, iov->iov_len);
}
#endif

static void
examine(unsigned *ptr)
{
	char *p, *end, outbuf[256];
	unsigned last;
	int i, n;

	n = 0;
	last = -1;
	p = outbuf;
	end = outbuf + sizeof(outbuf);
	p += snprintf(p, (end - p), "rank: %d", a.rank);
	for (i = 0; i < a.tensor_elements; i++) {
		if (ptr[i] == last) {
			n++;
			continue;
		}
		if (n != 0) {
			p += snprintf(p, (end - p), " %d*%u", n, last);
			if (p >= end)
				goto fail;
		}
		last = ptr[i];
		n = 1;
	}
	p += snprintf(p, (end - p), " %d*%u", n, last);
fail:
	printf("%s\n", outbuf);
}

static void
verify(bool end)
{
	unsigned *ptr = (unsigned *)a.tensor;
	unsigned i, k;
	unsigned sum, e;

	if (!end) {
		for (i = 0; i < a.tensor_elements; i++)
			ptr[i] = (a.rank + 1) * i;
		return;
	}

	if (false)
		examine(ptr);

	sum = 0;
	for (i = 0; i < a.tensor_elements; i++)
		sum += ptr[i];

	k = (a.size * (a.size + 1)) / 2;

	e = 0;
	for (i = 0; i < a.tensor_elements; i++)
		e += k * i;

	if (sum != e) {
		examine(ptr);
		errx(1, "rank %d sum: %u, expect %u", a.rank, sum, e);
	}
}

void
allreduce(bool check)
{
	int send_pos, recv_pos, stop_pos;
	struct iovec iov, chunk;
	int i;
int pos;

	/* sending from rank (R + 1) to rank R */
	send_pos = a.rank * a.rank_count;
	recv_pos = ((a.rank + 1) * a.rank_count) % a.tensor_count;
	stop_pos = ((a.rank + a.size - 1) * a.rank_count) % a.tensor_count;

	if (check)
		verify(false);

	/* Start in-flight chunks */
	for (i = 0; i < a.chunk_count; i++) {
		recv_pos = start_recv(recv_pos);
		send_pos = send_chunk(send_pos);
	}

	/* Reduce */
	while (send_pos != stop_pos) {
		finish_recv(&chunk, &iov);
pos = ((unsigned *)chunk.iov_base - (unsigned *)a.tensor) / a.chunk_elements;
if (pos != send_pos)
  printf("spatial gap: recv %d, send %d\n", pos, send_pos);
		apply_chunk(&chunk, &iov);
		recv_pos = start_recv(recv_pos);
		send_pos = send_chunk(send_pos);
	}

	stop_pos = (stop_pos + (a.size - 1) * a.rank_count) % a.tensor_count;

	/* Complete Reduce + Start Broadcast */
	for (i = 0; i < a.chunk_count; i++) {
		finish_recv(&chunk, &iov);
		apply_chunk(&chunk, &iov);
		recv_pos = start_inplace_recv(recv_pos);
		send_pos = send_chunk(send_pos);
	}

	/* Broadcast */
	while (send_pos != stop_pos) {
		finish_recv(&chunk, &iov);
//		copy_chunk(&chunk, &iov);
		recv_pos = start_inplace_recv(recv_pos);
		send_pos = send_chunk(send_pos);
	}

	/* Complete Broadcast */
	for (i = 0; i < a.chunk_count; i++) {
		finish_recv(&chunk, &iov);
//		copy_chunk(&chunk, &iov);
	}

	if (check)
		verify(true);
}

void
run(void)
{
	allreduce(true);
}

#if 0
int
main(int argc, char **argv)
{
	pid_t pid;
	int i;

	if (argc != 2)
		err(1, "Usage: %s size", argv[0]);

	a.size = atoi(argv[1]);
	printf("starting mesh of size %d\n", a.size);

	for (i = 1; i < a.size; i++) {
		pid = fork();
		if (pid == -1)
			err(1, "fork");
		if (pid == 0) {
			run(i);
			goto out;
		}
	}
	run(0);
out:
	return 0;
}
#endif
