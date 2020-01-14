
typedef void (*ev_callback)(int rank, int events);
struct driver;

struct driver *ev_start(int size);
void ev_stop(struct driver *d);
void ev_register(struct driver *d, int fd, int rank, ev_callback cb,
	int events);
void ev_unregister(struct driver *d, int fd);
void ev_loop(struct driver *d, bool (*stop_fn)(void));
void ev_poll(struct driver *d);


struct drv_req {
	int fd;			/* XXX not right */
	int arg;
	struct iovec iov;
	bool send;
	void (*callback)(struct drv_req *req, int events);
	void (*done)(int slot);
	int area;
	int scratch;
};

extern long drv_read_count[2];
extern long drv_write_count[2];
extern long drv_read_partial[2];
extern long drv_write_partial[2];
extern long mutex_stall_time;
extern long mutex_stall_count[2];

struct io_driver {
	void (*start)(void);
	void (*stop)(void);
	void (*req_register)(struct drv_req *req, int events);
	void (*unregister)(int fd);
	void (*register_buffer)(struct iovec *iov);
	void *(*loop)(void *arg);
	void (*send)(int fd, struct iovec *iov, int area);
	void (*recv)(int fd, struct iovec *iov, int area,
		void (*done)(int arg), int arg);
};
extern struct io_driver *io_driver;
extern struct io_driver epoll_driver;
extern struct io_driver iou_driver;

static inline void
drv_start(void)
{
	io_driver->start();
}

static inline void drv_stop(void)
{
	io_driver->stop();
}

static inline void
drv_register(struct drv_req *req, int events)
{
	io_driver->req_register(req, events);
}

static inline void
drv_register_buffer(struct iovec *iov)
{
	io_driver->register_buffer(iov);
}

static inline void
drv_unregister(int fd)
{
	io_driver->unregister(fd);
}

#define drv_loop	(io_driver->loop)

static inline void
drv_send(int fd, struct iovec *iov, int area)
{
	return io_driver->send(fd, iov, area);
}

static inline void
drv_recv(int fd, struct iovec *iov, int area, void (*done)(int arg), int arg)
{
	return io_driver->recv(fd, iov, area, done, arg);
}
