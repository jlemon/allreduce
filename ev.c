#include <err.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <pthread.h>

#include "driver.h"

#define EV_COUNT	64
#define EV_TIMEOUT	10

#define array_size(x)	(sizeof(x) / sizeof((x)[0]))

typedef void (*ev_callback)(int rank, int events);

struct driver {
	int ep;
	int size;
	int timeout;
	ev_callback cb[0];
};

struct driver *
ev_start(int size)
{
	struct driver *d;
	int rc;

	d = malloc(sizeof(*d) + size * sizeof(ev_callback));
	if (!d)
		err(1, "ev_start(malloc)");

	rc = epoll_create(1);
	if (rc == -1)
		err(1, "epoll_create()");

	d->ep = rc;
	d->size = size;
	d->timeout = EV_TIMEOUT;

	return d;
}

void
ev_stop(struct driver *d)
{
	close(d->ep);
	free(d);
}

void
ev_register(struct driver *d, int fd, int rank, ev_callback cb, int events)
{
	struct epoll_event ev;
	int rc;

	if (rank >= d->size)
		errx(1, "rank %d >= size %d", rank, d->size);
	d->cb[rank] = cb;

	ev.events = events;
	ev.data.ptr = &d->cb[rank];

	rc = epoll_ctl(d->ep, EPOLL_CTL_ADD, fd, &ev);
	if (rc == -1 && errno == EEXIST)
		rc = epoll_ctl(d->ep, EPOLL_CTL_MOD, fd, &ev);
	if (rc == -1)
		err(1, "epoll_add/mod()");
}

void
ev_unregister(struct driver *d, int fd)
{
	int rc;

	rc = epoll_ctl(d->ep, EPOLL_CTL_DEL, fd, NULL);
	if (rc == -1)
		err(1, "epoll_del()");
}

void
ev_loop(struct driver *d, bool (*stop_fn)(void))
{
	struct epoll_event ev[EV_COUNT];
	ev_callback *cb;
	int i, n, rank;

	while (!stop_fn()) {
		n = epoll_wait(d->ep, ev, array_size(ev), d->timeout);
		if (n == 0)
			continue;
		if (n == -1) {
			if (errno == EINTR)
				continue;
			err(1, "epoll_wait");
		}
		for (i = 0; i < n; i++) {
			cb = ev[i].data.ptr;
			rank = cb - d->cb;
			(*cb)(rank, ev[i].events);
		}
	}
}

static bool
ev_poll_toggle(void)
{
	static bool stop = true;

	stop = !stop;
	return stop;
}

void
ev_poll(struct driver *d)
{
	ev_loop(d, ev_poll_toggle);
}

/*---------------------------------------------------------------------------*/

struct io_driver *io_driver;
