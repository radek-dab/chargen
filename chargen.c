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
#define LSTCHUNK 1024
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

struct socklist {
	struct pollfd *fds;
	struct sockaddr_in *addrs;
	int num;
	int cap;
};

void socklist_add(struct socklist *lst,
		  int fd, short events, struct sockaddr_in *addr);
void socklist_remove(struct socklist *lst, int pos);
void socklist_clear(struct socklist *lst);

struct server {
	struct config cfg;
	struct socklist lst;
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
		if (poll(svr.lst.fds, svr.lst.num, -1) == -1) {
			if (errno == EINTR) continue;
			ERROR("poll");
		}

		if (svr.lst.fds[0].revents & POLLIN)
			connect_client(&svr);

		for (i = 1; i < svr.lst.num; i++) {
			if (svr.lst.fds[i].revents & POLLHUP) {
				disconnect_client(&svr, i--);
				continue;
			}

			if (svr.lst.fds[i].revents & POLLIN) {
				ret = read(svr.lst.fds[i].fd, buf, BUFSIZE);
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

			if (svr.lst.fds[i].revents & POLLOUT) {
				ret = write(svr.lst.fds[i].fd, pat, PATSIZE);
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

void socklist_add(struct socklist *lst,
		  int fd, short events, struct sockaddr_in *addr)
{
	assert(lst != NULL);

	if (++lst->num > lst->cap) {
		lst->cap += LSTCHUNK;

		if ((lst->fds = realloc(lst->fds, lst->cap *
					sizeof(struct pollfd))) == NULL)
			ERROR("realloc");

		if ((lst->addrs = realloc(lst->addrs, lst->cap *
					  sizeof(struct sockaddr_in))) == NULL)
			ERROR("realloc");
	}

	lst->fds[lst->num-1].fd      = fd;
	lst->fds[lst->num-1].events  = events;
	lst->fds[lst->num-1].revents = 0;
	memcpy(&lst->addrs[lst->num-1], addr, sizeof(struct sockaddr_in));
}

void socklist_remove(struct socklist *lst, int pos)
{
	assert(lst != NULL);
	assert(0 <= pos && pos < lst->num);

	if (pos != lst->num-1) {
		lst->fds[pos].fd      = lst->fds[lst->num-1].fd;
		lst->fds[pos].events  = lst->fds[lst->num-1].events;
		lst->fds[pos].revents = lst->fds[lst->num-1].revents;
		memcpy(&lst->addrs[pos], &lst->addrs[lst->num-1],
		       sizeof(struct sockaddr_in));
	}

	if (--lst->num <= lst->cap-2*LSTCHUNK) {
		lst->cap -= LSTCHUNK;

		if ((lst->fds = realloc(lst->fds, lst->cap *
					sizeof(struct pollfd))) == NULL)
			ERROR("realloc");

		if ((lst->addrs = realloc(lst->addrs, lst->cap *
					  sizeof(struct sockaddr_in))) == NULL)
			ERROR("realloc");
	}
}

void socklist_clear(struct socklist *lst)
{
	assert(lst != NULL);

	if (lst->fds != NULL) {
		free(lst->fds);
		lst->fds = NULL;
	}
	if (lst->addrs != NULL) {
		free(lst->addrs);
		lst->addrs = NULL;
	}
	lst->num = 0;
	lst->cap = 0;
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

	socklist_add(&svr->lst, fd, POLLIN, &addr);

	printf("Listening on port %hu\n", svr->cfg.port);
}

void destroy_server(struct server *svr)
{
	assert(svr != NULL);

	int i;

	for (i = 0; i < svr->lst.num; i++)
		if (TEMP_FAILURE_RETRY(close(svr->lst.fds[i].fd)) == -1)
			ERROR("close");

	socklist_clear(&svr->lst);
}

void connect_client(struct server *svr)
{
	assert(svr != NULL);

	struct sockaddr_in addr;
	socklen_t addrlen = sizeof(addr);
	int fd;

	if ((fd = accept(svr->lst.fds[0].fd, &addr, &addrlen)) == -1)
		ERROR("accept");

	printf("%s:%hu connected\n",
	       inet_ntoa(addr.sin_addr),
	       ntohs(addr.sin_port));

	socklist_add(&svr->lst, fd, POLLIN|POLLOUT, &addr);
}

void disconnect_client(struct server *svr, int pos)
{
	assert(svr != NULL);
	assert(0 <= pos && pos < svr->lst.num);

	if (TEMP_FAILURE_RETRY(close(svr->lst.fds[pos].fd)) == -1)
		ERROR("close");

	printf("%s:%hu disconnected\n",
	       inet_ntoa(svr->lst.addrs[pos].sin_addr),
	       ntohs(svr->lst.addrs[pos].sin_port));

	socklist_remove(&svr->lst, pos);
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
