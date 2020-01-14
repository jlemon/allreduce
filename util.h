#ifndef _UTIL_H
#define _UTIL_H

#define err_with(rc, ...) do {						\
	errno = -rc;							\
	err(1, __VA_ARGS__);						\
} while (0)

extern long read_count;
extern long write_count;
extern long read_partial;
extern long write_partial;

struct node {
	int fd;
	int rank;
	int family;
	int socktype;
	int protocol;
	socklen_t addrlen;
	struct sockaddr_storage addr;
//	struct ether_addr mac_addr;
};

enum rw_type {
	RW_BASIC,
	RW_MSG,
	RW_FIXED,
};

enum transport_type {
	TRANSPORT_TCP,
	TRANSPORT_UDP,
	TRANSPORT_XDP,
	TRANSPORT_IOU,
};

struct opt {
	const char *iface;
	bool poll;
	bool nonblock;
	bool dual;
	bool end_summary;
	int timeout;
	int port;
	int rank;
	enum transport_type transport;
	const char *driver;
	bool zerocopy;

	/* test parameters */
	struct {
		int iterations;
		int elements;
		int chunk_size;
		int chunk_count;
	};

	/* mpi/mesh parameters */
	struct {
		int size;				/* mesh size */
		bool root;
		int rootport;				/* control port */
		const char *roothost;
	};

	/* iou parameters */
	struct {
		enum rw_type rw_api;
		int entries;
	} iou;

	/* xdp parameters */
	struct {
		bool use_wakeup;
		unsigned xdp_flags;
		unsigned bind_flags;
		unsigned mmap_flags;
		unsigned umem_flags;
		int frame_size;
		int num_frames;
		int num_xsks;
		int queue;
		int ifindex;
	} xsk;
};

extern struct opt opt;

void set_blocking_mode(int fd, bool on);
bool fd_read_ready(int fd);
const char *show_node_addr(struct node *node);
void setup_node(struct node *node, int socktype, const char *selfname);
void set_port(struct node *node, int port);

#endif /* _UTIL_H */
