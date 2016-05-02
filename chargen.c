#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>

#define DEFPORT 1919
#define BACKLOG 5
#define SVRCHUNK 1024
#define CHSETBEG ' '
#define CHSETEND '~'
#define FIRSTCH '!'
#define LINELEN 72
#define LINENUM (CHSETEND-CHSETBEG+1)
#define PATSIZE ((LINELEN+2)*LINENUM)
#define BUFSIZE PATSIZE

#define ERROR(s)	(fprintf(stderr, "%s:%d ", __FILE__, __LINE__), \
			perror(s), kill(0, SIGTERM), exit(EXIT_FAILURE))

#if DEBUG
#define DEBUGF(...)	fprintf(stderr, __VA_ARGS__)
#else
#define DEBUGF(...)
#endif

volatile sig_atomic_t running = 1;

void handler(int sig);
void set_signal_handler(void);

struct config {
	unsigned short port;
};

void display_usage(void);
void parse_args(int argc, char *argv[], struct config *cfg);

struct server {
	struct config cfg;
	struct pollfd *fds;
	struct sockaddr_in *addrs;
	int num;
	int cap;
};

void create_server(struct server *svr);
void destroy_server(struct server *svr);
void connect_client(struct server *svr);
void disconnect_client(struct server *svr, int pos);
char* create_pattern(void);

int main(int argc, char *argv[])
{
	struct server svr = {};
	char *pat, *buf;
	int i, ret;

	set_signal_handler();
	parse_args(argc, argv, &svr.cfg);
	create_server(&svr);
	pat = create_pattern();
	if ((buf = malloc(BUFSIZE)) == NULL)
		ERROR("malloc");

	while (running) {
		if (poll(svr.fds, svr.num, -1) == -1) {
			if (errno == EINTR) continue;
			ERROR("poll");
		}

		if (svr.fds[0].revents & POLLIN)
			connect_client(&svr);

		for (i = 1; i < svr.num; i++) {
			if (svr.fds[i].revents & POLLHUP) {
				disconnect_client(&svr, i--);
				continue;
			}

			if (svr.fds[i].revents & POLLIN) {
				ret = read(svr.fds[i].fd, buf, BUFSIZE);
				if (ret == 0) {
					disconnect_client(&svr, i--);
					continue;
				}
				if (ret == -1) {
					if (errno == ECONNRESET) {
						disconnect_client(&svr, i--);
						continue;
					}
					if (errno == EINTR) break;
					ERROR("read");
				}
			}

			if (svr.fds[i].revents & POLLOUT) {
				ret = write(svr.fds[i].fd, pat, PATSIZE);
				// TODO: Resume after signal interruption
				if (ret == -1) {
					if (errno == EPIPE ||
					    errno == ECONNRESET) {
						disconnect_client(&svr, i--);
						continue;
					}
					if (errno == EINTR) break;
					ERROR("write");
				}
			}
		}
	}

	destroy_server(&svr);
	free(pat);
	free(buf);
	return EXIT_SUCCESS;
}

void handler(int sig)
{
	running = 0;
}

void set_signal_handler(void)
{
	struct sigaction sa = {};

	sa.sa_handler = handler;
	if (sigaction(SIGINT, &sa, NULL) == -1)
		ERROR("sigaction");
	if (sigaction(SIGTERM, &sa, NULL) == -1)
		ERROR("sigaction");

	sa.sa_handler = SIG_IGN;
	if (sigaction(SIGPIPE, &sa, NULL) == -1)
		ERROR("sigaction");
}

void display_usage(void)
{
	puts("Character Generator by Radosław Dąbrowski\n"
	     "Usage: chargen [OPTION]...\n"
	     "\n"
	     "Options:\n"
	     "  -p, --port=NUM  set port number\n"
	     "  -h, --help      display this help and exit");

	exit(EXIT_SUCCESS);
}

void parse_args(int argc, char *argv[], struct config *cfg)
{
	assert(cfg != NULL);

	static const char optstring[] = "p:h";
	static const struct option longopts[] = {
		{"port", required_argument, NULL, 'p'},
		{"help", no_argument, NULL, 'h'},
		{}
	};
	int ret;

	cfg->port = DEFPORT;

	while ((ret = getopt_long(argc, argv,
				  optstring, longopts, NULL)) != -1) {
		switch (ret) {
			case 'p':
				if (sscanf(optarg, "%hu", &cfg->port) != 1)
					display_usage();
				break;
			default:
				display_usage();
		}
	}
	if (optind < argc)
		display_usage();
}

void create_server(struct server *svr)
{
	assert(svr != NULL);

	struct sockaddr_in addr = {};
	int reuseaddr = 1, fd;

	if ((fd = socket(PF_INET, SOCK_STREAM, 0)) == -1)
		ERROR("socket");

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
		       &reuseaddr, sizeof(reuseaddr)) == -1)
		ERROR("setsockopt");

	addr.sin_family = AF_INET;
	addr.sin_port = htons(svr->cfg.port);
	addr.sin_addr.s_addr = INADDR_ANY;
	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1)
		ERROR("bind");

	if (listen(fd, BACKLOG) == -1)
		ERROR("listen");

	svr->num = 1;
	svr->cap = SVRCHUNK;
	if ((svr->fds = malloc(svr->cap*sizeof(struct pollfd))) == NULL)
		ERROR("malloc");
	if ((svr->addrs = malloc(svr->cap*sizeof(struct sockaddr_in))) == NULL)
		ERROR("malloc");

	svr->fds[0].fd = fd;
	svr->fds[0].events = POLLIN;
	svr->fds[0].revents = 0;
	memcpy(&svr->addrs[0], &addr, sizeof(addr));

	printf("Listening on port %hu\n", svr->cfg.port);
}

void destroy_server(struct server *svr)
{
	assert(svr != NULL);

	int i;

	for (i = 0; i < svr->num; i++)
		if (TEMP_FAILURE_RETRY(close(svr->fds[i].fd)) == -1)
			ERROR("close");

	free(svr->fds);
	free(svr->addrs);
	svr->num = 0;
	svr->cap = 0;
}

void connect_client(struct server *svr)
{
	assert(svr != NULL);

	struct sockaddr_in addr;
	socklen_t addrlen = sizeof(addr);
	int fd;

	if ((fd = accept(svr->fds[0].fd, &addr, &addrlen)) == -1)
		ERROR("accept");

	printf("%s:%hu connected\n",
	       inet_ntoa(addr.sin_addr),
	       ntohs(addr.sin_port));

	if (++svr->num > svr->cap) {
		svr->cap += SVRCHUNK;

		svr->fds = realloc(svr->fds,
				   svr->cap * sizeof(struct pollfd));
		if (svr->fds == NULL) ERROR("realloc");

		svr->addrs = realloc(svr->addrs,
				     svr->cap * sizeof(struct sockaddr_in));
		if (svr->addrs == NULL) ERROR("realloc");
	}

	svr->fds[svr->num-1].fd = fd;
	svr->fds[svr->num-1].events = POLLIN|POLLOUT;
	svr->fds[svr->num-1].revents = 0;
	memcpy(&svr->addrs[svr->num-1], &addr, sizeof(struct sockaddr_in));
}

void disconnect_client(struct server *svr, int pos)
{
	assert(svr != NULL);
	assert(0 <= pos && pos < svr->num);

	if (TEMP_FAILURE_RETRY(close(svr->fds[pos].fd)) == -1)
		ERROR("close");

	printf("%s:%hu disconnected\n",
	       inet_ntoa(svr->addrs[pos].sin_addr),
	       ntohs(svr->addrs[pos].sin_port));

	if (pos != svr->num-1) {
		svr->fds[pos].fd      = svr->fds[svr->num-1].fd;
		svr->fds[pos].events  = svr->fds[svr->num-1].events;
		svr->fds[pos].revents = svr->fds[svr->num-1].revents;
		memcpy(&svr->addrs[pos], &svr->addrs[svr->num-1],
		       sizeof(struct sockaddr_in));
	}

	if (--svr->num <= svr->cap-2*SVRCHUNK) {
		svr->cap -= SVRCHUNK;

		svr->fds = realloc(svr->fds,
				   svr->cap * sizeof(struct pollfd));
		if (svr->fds == NULL) ERROR("realloc");

		svr->addrs = realloc(svr->addrs,
				     svr->cap * sizeof(struct sockaddr_in));
		if (svr->addrs == NULL) ERROR("realloc");
	}
}

char* create_pattern(void)
{
	char *pat, *p, c, d;
	int i, j;

	if ((pat = malloc(PATSIZE)) == NULL)
		ERROR("malloc");

	p = pat;
	c = FIRSTCH;

	for (i = 0; i < LINENUM; i++) {
		d = c;

		for (j = 0; j < LINELEN; j++) {
			*p++ = d;

			if (++d > CHSETEND)
				d = CHSETBEG;
		}

		*p++ = '\r';
		*p++ = '\n';

		if (++c > CHSETEND)
			c = CHSETBEG;
	}

	return pat;
}
