#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <locale.h>
#include <libgen.h>
#include <pthread.h>
#include <sys/resource.h>

#include <sys/socket.h>
#include <sys/param.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "mesh.h"
#include "util.h"
#include "sample.h"
#include "driver.h"

struct opt opt = {
	.transport = TRANSPORT_TCP,
	.port = 7810,
	.rootport = 7800,
	.iterations = 100,
	.elements = 30 * 1000,
	.nonblock = true,
	.rank = -1,

	.driver = "epoll",
	.iou.entries = 64,
};

/* XXX move to header */
void allreduce(bool check);
void allreduce_init(int rank, int size);

/* sigh - split out all mpi setup into fully connected mesh
 * then each test allocates its data structures and refs a specific rank.
 */

static void statistics(void);
static void progress(void);
void mutex_stats(void);

static void
run(void)
{
	unsigned long start;
	int i; //, rank;

#if 0
	rank = mesh_join();
printf("mesh formed, rank %d\n", rank);

	allreduce_init(rank, opt.size);
#endif

	allreduce(true);

	for (i = 0; i < opt.iterations; i++) {
                start = nsec();
		allreduce(false);
                stat_add(elapsed(start));
	}
	statistics();

	printf("r,w: (%ld,%ld)/(%ld,%ld)  partial: (%ld,%ld)/(%ld,%ld)\n",
	    drv_read_count[0], drv_write_count[0],
	    drv_read_count[1], drv_write_count[1],
	    drv_read_partial[0], drv_write_partial[0],
	    drv_read_partial[1], drv_write_partial[1]);
	mutex_stats();

	mesh_leave();
}

static void
start(void)
{
	pthread_t drv_thread;
	int rc, rank;

	drv_start();

	rank = mesh_join();
printf("mesh formed, rank %d\n", rank);
	if (opt.rank == -1) opt.rank = rank;		/* XXX fix this */
	allreduce_init(rank, opt.size);

	rc = pthread_create(&drv_thread, NULL, drv_loop, NULL);
	if (rc)
		err(1, "pthread_create");

	run();

	drv_stop();
	pthread_join(drv_thread, NULL);
}

static void
handle_signal(int sig)
{
	drv_stop();
}

static void
initialize(void)
{
	struct rlimit r = {RLIM_INFINITY, RLIM_INFINITY};
	struct sigaction sa = {
		.sa_handler = handle_signal,
	};
	int rc;

	rc = setrlimit(RLIMIT_MEMLOCK, &r);
	if (rc)
		warn("setrlimit(RLIMIT_MEMLOCK)");

	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);

#if 0
	/* Sigh, Linux automatically sets SA_RESTART */
	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);
	signal(SIGABRT, handle_signal);
#endif

	setlocale(LC_ALL, "");
}

static void
statistics(void)
{
	char *scale_str = "(us)";
	static int last = 0;
	int scale = 1000;
	int count = stat_count();

	if (count == last)
		return;

	if (last == 0) {
		printf("\n");
		printf("%-10.10s %4s%4s %4s%4s %4s%4s %4s%4s %4s%4s %4s%4s %9s\n",
		    "test",
		    "min", scale_str,
		    "p25", scale_str,
		    "p50", scale_str,
		    "p90", scale_str,
		    "p99", scale_str,
		    "max", scale_str,
		    "samples");
	}
	printf("%-10.10s %8d %8d %8d %8d %8d %8d %8d\n",
	    "TCP", //transport_name[opt.transport],
	    stat_pct(0.0) / scale,
	    stat_pct(25.0) / scale,
	    stat_pct(50.0) / scale,
	    stat_pct(90.0) / scale,
	    stat_pct(99.0) / scale,
	    stat_pct(100.0) / scale,
	    count);

	last = count;
}

static void
progress(void)
{
	int count;

	if (opt.end_summary)
		return;

	count = stat_count();
	if ((count % 1000) == 0)
		statistics();
}

enum {
	OPT_INTEGER = 1,
	OPT_STRING,
};

static struct option options[] = {
	{"block", no_argument, 0, 'b'},
	{"tcp", no_argument, 0, 't'},
	{"udp", no_argument, 0, 'u'},
	{"xdp", no_argument, 0, 'x'},
	{"iou", no_argument, 0, 'o'},
	{"poll", no_argument, 0, 'p'},
	{"port", required_argument, 0, 'P'},
	{"dual", no_argument, 0, '2'},
	{"end", no_argument, 0, 'e'},
	{"msg", no_argument, 0, 'm'},
	{"fixed", no_argument, 0, 'f'},

	{"host", required_argument, 0, 'h'},
	{"rank", required_argument, 0, 'r'},
	{"root", no_argument, 0, 'R'},
	{"size", required_argument, 0, 's'},

	{"iterations", required_argument, 0, 'I'},
	{"elements", required_argument, 0, 'e'},
	{"zc", no_argument, 0, 'z'},

	{"chunk_size", required_argument, &opt.chunk_size, OPT_INTEGER},
	{"chunk_count", required_argument, &opt.chunk_count, OPT_INTEGER},
	{"driver", required_argument, (int *)&opt.driver, OPT_STRING},
#if 0
	{"l2fwd", no_argument, 0, 'l'},
	{"interface", required_argument, 0, 'i'},
	{"queue", required_argument, 0, 'q'},
	{"poll", no_argument, 0, 'p'},
	{"xdp-skb", no_argument, 0, 'S'},
	{"xdp-native", no_argument, 0, 'N'},
	{"interval", required_argument, 0, 'n'},
	{"thread", required_argument, 0, 't'},
	{"zero-copy", no_argument, 0, 'z'},
	{"copy", no_argument, 0, 'c'},
	{"frame-size", required_argument, 0, 'f'},
#endif
	{0, 0, 0, 0}
};

#define OPTSTR "2befh:mopP:r:Rs:tux"

static void
usage(const char *prog)
{
	fprintf(stderr, "usage\n");
	exit(1);
}

static void
parse_cmdline(int argc, char **argv)
{
	int index, c;
	int *ptr;

	opterr = 0;

	for (;;) {
		c = getopt_long(argc, argv, OPTSTR, options, &index);
		if (c == -1)
			break;

		switch (c) {
		case 0:
			ptr = options[index].flag;
			switch (*ptr) {
			case OPT_INTEGER:
				*ptr = atoi(optarg);
				break;
			case OPT_STRING:
				*(char **)ptr = optarg;
				break;
			default:
				errx(1, "unkown longopt type %d", *ptr);
			}
			break;
		case '2':
			opt.dual = true;
			break;
		case 'b':
			opt.nonblock = false;
			break;
		case 'e':
			opt.elements = atoi(optarg);
			break;
#if 0
		case 'f':
			opt.rw_api = RW_FIXED;
			break;
#endif
		case 'h':
			opt.roothost = optarg;
			break;
		case 'I':
			opt.iterations = atoi(optarg);
			break;
#if 0
		case 'o':
			opt.transport = TRANSPORT_IOU;
			break;
		case 'm':
			opt.rw_api = RW_MSG;
			break;
#endif
		case 'p':
			opt.poll = true;
			break;
		case 'P':
			opt.port = atoi(optarg);
			break;
		case 'r':
			opt.rank = atoi(optarg);
			break;
		case 'R':
			opt.root = true;
			break;
		case 's':
			opt.size = atoi(optarg);
			break;
		case 't':
			opt.transport = TRANSPORT_TCP;
			break;
		case 'u':
			opt.transport = TRANSPORT_UDP;
			break;
		case 'x':
			opt.transport = TRANSPORT_XDP;
			break;
		case 'z':
			opt.zerocopy = true;
			break;
		default:
			usage(basename(argv[0]));
		}
	}
}

static void
handle_options(void)
{

	/* move per-option checking to each file */
	if (!opt.size)
		errx(1, "Mesh size is not set");

	if (opt.iterations < 1)
		errx(1, "Invalid iteration count: %d", opt.iterations);

	if (!strcmp(opt.driver, "iou"))
		io_driver = &iou_driver;
	else if (!strcmp(opt.driver, "epoll"))
		io_driver = &epoll_driver;
	else
		errx(1, "Unknown driver: %s", opt.driver);

#if 0
	opt.ifindex = if_nametoindex(opt.iface);
	if (!opt.ifindex)
		errx(1, "Interface '%s' does not exist", opt.iface);

	switch (opt.transport) {
	case TRANSPORT_TCP:
		run.socktype = SOCK_STREAM;
		break;
	case TRANSPORT_UDP:
		run.socktype = SOCK_DGRAM;
		break;
	case TRANSPORT_XDP:
		run.socktype = SOCK_DGRAM;
		break;
	case TRANSPORT_IOU:
		run.socktype = SOCK_STREAM;
		break;
	}

	if (opt.use_wakeup)
		opt.bind_flags = XDP_USE_NEED_WAKEUP;
#endif
}

static void
create_processes(void)
{
	pid_t pid;
	int i;

	/* no roothost, run everything locally */
	for (i = 1; i < opt.size; i++) {
		pid = fork();
		if (pid == -1)
			err(1, "fork");
		if (pid == 0)
			break;
	}
}

int
main(int argc, char **argv)
{

	initialize();
	parse_cmdline(argc, argv);
	handle_options();

#if 0
	printf("%s transport,%s%s%s%s%s\n",
	    "TCP", //transport_name[opt.transport],
	    optind == argc ? "server" : argv[optind],
	    opt.nonblock ? ",nonblocking" : ",blocking",
	    opt.poll ? ",poll" : "",
 	    "", 
#if 0
	    opt.rw_api == RW_BASIC ? "" :
	        opt.rw_api == RW_MSG ? ",*msg" : ",fixed",
#endif
	    opt.dual ? ",dual" : "");
#endif

	/* DB handling/registration */
	if (opt.root || !opt.roothost)
		mesh_init();

	if (!opt.root && !opt.roothost)
		create_processes();

	start();
	if (0)
		progress();
	sleep(1);

	return 0;
}
