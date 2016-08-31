#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <getopt.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>

#define TCP 1
#define UDP 2

#define DEFPROTO	(TCP|UDP)
#define DEFPORT		1919
#define BACKLOG		5
#define LSTCHUNK	1024
#define CHSETBEG	' '
#define CHSETEND	'~'
#define FIRSTCH		'!'
#define LINELEN		72
#define LINENUM		(CHSETEND-CHSETBEG+1)
#define PATSIZE		((LINELEN+2)*LINENUM)
#define BUFSIZE		PATSIZE
#define MAXDGRAM	512
#define SIZELEN		16
#define SAFEUSR		"nobody"
#define SAFEGRP		"nobody"

#define XSTR(s)		STR(s)
#define STR(s)		#s

#define ERROR(s)	(fprintf(stderr, "%s:%d ", __FILE__, __LINE__), \
			perror(s), kill(0, SIGTERM), exit(EXIT_FAILURE))

#define LOGF(...)	(printf(__VA_ARGS__), fflush(stdout))

#if DEBUG
#define DEBUGF(...)	fprintf(stderr, __VA_ARGS__)
#else
#define DEBUGF(...)
#endif

#if DEFPROTO & TCP
#define DEFAULT_TCP	" (default)"
#else
#define DEFAULT_TCP
#endif

#if DEFPROTO & UDP
#define DEFAULT_UDP	" (default)"
#else
#define DEFAULT_UDP
#endif

#define CLIENTS_NUM(svr) ((svr)->lst.num - CLIENTS_LIST)

volatile sig_atomic_t running = 1;
volatile sig_atomic_t listing = 0;

void set_exiting(int sig);
void set_listing(int sig);
void set_signal_handler(void);
void drop_privileges(void);

struct config {
	int proto;
	unsigned short port;
};

void display_usage(void);
void parse_args(int argc, char *argv[], struct config *cfg);

struct sockinfo {
	struct sockaddr_in addr;
	size_t rx;
	size_t tx;
};

struct socklist {
	struct pollfd *fds;
	struct sockinfo *infos;
	int num;
	int cap;
};

void socklist_add(struct socklist *lst,
		  int fd, short events, struct sockaddr_in *addr);
void socklist_remove(struct socklist *lst, int pos);
void socklist_clear(struct socklist *lst);

enum server_list {
	TCP_SERVICE,
	UDP_SERVICE,
	CLIENTS_LIST
};

struct server {
	struct config cfg;
	struct socklist lst;
	char *pat;
	char *buf;
};

int create_socket(int type, struct sockaddr_in *addr);
void create_server(struct server *svr);
void destroy_server(struct server *svr);
void connect_client(struct server *svr);
void disconnect_client(struct server *svr, int pos);
void list_clients(struct server *svr);
void serve_clients(struct server *svr);
void serve_datagram(struct server *svr);
char* create_pattern(void);
char* humanize_size(char *buf, size_t len, size_t size);

int main(int argc, char *argv[])
{
	struct server svr = {};

	set_signal_handler();
	parse_args(argc, argv, &svr.cfg);
	create_server(&svr);
	drop_privileges();
	srand(time(NULL));

	while (running) {
		if (listing) {
			list_clients(&svr);
			listing = 0;
		}

		if (poll(svr.lst.fds, svr.lst.num, -1) == -1) {
			if (errno == EINTR) continue;
			ERROR("poll");
		}

		if (svr.lst.fds[TCP_SERVICE].revents & POLLIN)
			connect_client(&svr);

		if (svr.lst.fds[UDP_SERVICE].revents & POLLIN)
			serve_datagram(&svr);

		serve_clients(&svr);
	}

	destroy_server(&svr);
	return EXIT_SUCCESS;
}

void set_exiting(int sig)
{
	running = 0;
}

void set_listing(int sig)
{
	listing = 1;
}

void set_signal_handler(void)
{
	struct sigaction sa = {};

	sa.sa_handler = set_exiting;
	if (sigaction(SIGINT, &sa, NULL) == -1)
		ERROR("sigaction");
	if (sigaction(SIGTERM, &sa, NULL) == -1)
		ERROR("sigaction");

	sa.sa_handler = set_listing;
	if (sigaction(SIGUSR1, &sa, NULL) == -1)
		ERROR("sigaction");

	sa.sa_handler = SIG_IGN;
	if (sigaction(SIGPIPE, &sa, NULL) == -1)
		ERROR("sigaction");
}

void drop_privileges(void)
{
	struct group *grp;
	struct passwd *pwd;

	if (getuid() != 0)
		return;

	if ((grp = getgrnam(SAFEGRP)) == NULL)
		ERROR("getgrnam");
	if (setgid(grp->gr_gid) == -1)
		ERROR("setgid");

	if ((pwd = getpwnam(SAFEUSR)) == NULL)
		ERROR("getpwnam");
	if (setuid(pwd->pw_uid) == -1)
		ERROR("setuid");

	LOGF("Dropped privileges\n");
}

void display_usage(void)
{
	puts("Character Generator by Radosław Dąbrowski\n"
	     "Usage: chargen [OPTION]...\n"
	     "\n"
	     "Options:\n"
	     "  -t, --tcp       TCP service"DEFAULT_TCP"\n"
	     "  -u, --udp       UDP service"DEFAULT_UDP"\n"
	     "  -p, --port=NUM  port number (default "XSTR(DEFPORT)")\n"
	     "  -h, --help      display this help and exit\n"
	     "\n"
	     "Send USR1 signal to display list of active clients\n"
	     "and their transfer statistics.");

	exit(EXIT_SUCCESS);
}

void parse_args(int argc, char *argv[], struct config *cfg)
{
	assert(cfg != NULL);

	static const char optstring[] = "tup:h";
	static const struct option longopts[] = {
		{"tcp", no_argument, NULL, 't'},
		{"udp", no_argument, NULL, 'u'},
		{"port", required_argument, NULL, 'p'},
		{"help", no_argument, NULL, 'h'},
		{}
	};
	int ret;

	cfg->proto = 0;
	cfg->port = DEFPORT;

	while ((ret = getopt_long(argc, argv,
				  optstring, longopts, NULL)) != -1) {
		switch (ret) {
			case 't':
				cfg->proto |= TCP;
				break;
			case 'u':
				cfg->proto |= UDP;
				break;
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

	if (!cfg->proto)
		cfg->proto = DEFPROTO;
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

		if ((lst->infos = realloc(lst->infos, lst->cap *
					  sizeof(struct sockinfo))) == NULL)
			ERROR("realloc");
	}

	lst->fds[lst->num-1].fd      = fd;
	lst->fds[lst->num-1].events  = events;
	lst->fds[lst->num-1].revents = 0;
	memcpy(&lst->infos[lst->num-1].addr, addr, sizeof(struct sockaddr_in));
	lst->infos[lst->num-1].rx = 0;
	lst->infos[lst->num-1].tx = 0;
}

void socklist_remove(struct socklist *lst, int pos)
{
	assert(lst != NULL);
	assert(0 <= pos && pos < lst->num);

	if (pos != lst->num-1) {
		lst->fds[pos].fd      = lst->fds[lst->num-1].fd;
		lst->fds[pos].events  = lst->fds[lst->num-1].events;
		lst->fds[pos].revents = lst->fds[lst->num-1].revents;
		memcpy(&lst->infos[pos], &lst->infos[lst->num-1],
		       sizeof(struct sockinfo));
	}

	if (--lst->num <= lst->cap-2*LSTCHUNK) {
		lst->cap -= LSTCHUNK;

		if ((lst->fds = realloc(lst->fds, lst->cap *
					sizeof(struct pollfd))) == NULL)
			ERROR("realloc");

		if ((lst->infos = realloc(lst->infos, lst->cap *
					  sizeof(struct sockinfo))) == NULL)
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
	if (lst->infos != NULL) {
		free(lst->infos);
		lst->infos = NULL;
	}
	lst->num = 0;
	lst->cap = 0;
}

int create_socket(int type, struct sockaddr_in *addr)
{
	assert(type == SOCK_STREAM || type == SOCK_DGRAM);
	assert(addr != NULL);

	int reuseaddr = 1, fd;

	if ((fd = socket(PF_INET, type, 0)) == -1)
		ERROR("socket");

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
		       &reuseaddr, sizeof(reuseaddr)) == -1)
		ERROR("setsockopt");

	if (bind(fd, (struct sockaddr*)addr, sizeof(*addr)) == -1)
		ERROR("bind");

	if (type == SOCK_STREAM) {
		if (listen(fd, BACKLOG) == -1)
			ERROR("listen");
	}

	return fd;
}

void create_server(struct server *svr)
{
	assert(svr != NULL);
	assert(svr->lst.num == 0);

	struct sockaddr_in addr = {};
	int fd;

	addr.sin_family = AF_INET;
	addr.sin_port = htons(svr->cfg.port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (svr->cfg.proto & TCP) {
		fd = create_socket(SOCK_STREAM, &addr);
		LOGF("Started TCP service\n");
	} else {
		fd = -1;
	}
	socklist_add(&svr->lst, fd, POLLIN, &addr);

	if (svr->cfg.proto & UDP) {
		fd = create_socket(SOCK_DGRAM, &addr);
		LOGF("Started UDP service\n");
	} else {
		fd = -1;
	}
	socklist_add(&svr->lst, fd, POLLIN, &addr);

	svr->pat = create_pattern();
	if ((svr->buf = malloc(BUFSIZE)) == NULL)
		ERROR("malloc");

	LOGF("Listening on port %hu\n", svr->cfg.port);
}

void destroy_server(struct server *svr)
{
	assert(svr != NULL);

	int i;

	for (i = 0; i < svr->lst.num; i++)
		if (TEMP_FAILURE_RETRY(close(svr->lst.fds[i].fd)) == -1)
			ERROR("close");

	socklist_clear(&svr->lst);

	free(svr->pat);
	free(svr->buf);
}

void connect_client(struct server *svr)
{
	assert(svr != NULL);

	struct sockaddr_in addr;
	socklen_t addrlen = sizeof(addr);
	int fd;

	if ((fd = accept(svr->lst.fds[0].fd, &addr, &addrlen)) == -1)
		ERROR("accept");

	LOGF("%s:%hu connected, %d active client%s\n",
	     inet_ntoa(addr.sin_addr),
	     ntohs(addr.sin_port),
	     CLIENTS_NUM(svr)+1,
	     CLIENTS_NUM(svr)+1 == 1 ? "" : "s");

	socklist_add(&svr->lst, fd, POLLIN|POLLOUT, &addr);
}

void disconnect_client(struct server *svr, int pos)
{
	assert(svr != NULL);
	assert(CLIENTS_LIST <= pos && pos < svr->lst.num);

	if (TEMP_FAILURE_RETRY(close(svr->lst.fds[pos].fd)) == -1)
		ERROR("close");

	LOGF("%s:%hu disconnected, %d active client%s\n",
	     inet_ntoa(svr->lst.infos[pos].addr.sin_addr),
	     ntohs(svr->lst.infos[pos].addr.sin_port),
	     CLIENTS_NUM(svr)-1,
	     CLIENTS_NUM(svr)-1 == 1 ? "" : "s");

	socklist_remove(&svr->lst, pos);
}

void list_clients(struct server *svr)
{
	assert(svr != NULL);

	struct sockinfo *info;
	char buf[SIZELEN];
	int i;

	printf("%d active client%s\n",
	       CLIENTS_NUM(svr),
	       CLIENTS_NUM(svr) == 1 ? "" : "s");

	for (i = CLIENTS_LIST; i < svr->lst.num; i++) {
		info = &svr->lst.infos[i];
		printf("  %s:%hu\t",
		       inet_ntoa(info->addr.sin_addr),
		       ntohs(info->addr.sin_port));
		printf("RX: %s\t", humanize_size(buf, SIZELEN, info->rx));
		printf("TX: %s\n", humanize_size(buf, SIZELEN, info->tx));
	}

	fflush(stdout);
}

void serve_clients(struct server *svr)
{
	int i, ret;
	size_t off;

	for (i = CLIENTS_LIST; i < svr->lst.num; i++) {
		if (svr->lst.fds[i].revents & POLLHUP) {
			disconnect_client(svr, i--);
			continue;
		}

		if (svr->lst.fds[i].revents & POLLIN) {
			ret = read(svr->lst.fds[i].fd, svr->buf, BUFSIZE);
			if (ret == 0) {
				disconnect_client(svr, i--);
				continue;
			}
			if (ret == -1) {
				if (errno == ECONNRESET) {
					disconnect_client(svr, i--);
					continue;
				}
				if (errno == EINTR) break;
				ERROR("read");
			}
			svr->lst.infos[i].rx += ret;
		}

		if (svr->lst.fds[i].revents & POLLOUT) {
			off = svr->lst.infos[i].tx % PATSIZE;
			ret = write(svr->lst.fds[i].fd,
				    svr->pat + off, PATSIZE - off);
			if (ret == -1) {
				if (errno == EPIPE ||
				    errno == ECONNRESET) {
					disconnect_client(svr, i--);
					continue;
				}
				if (errno == EINTR) break;
				ERROR("write");
			}
			svr->lst.infos[i].tx += ret;
		}
	}
}

void serve_datagram(struct server *svr)
{
	int fd = svr->lst.fds[UDP_SERVICE].fd;
	struct sockaddr_in addr;
	socklen_t addrlen = sizeof(addr);
	ssize_t size;

	if (recvfrom(fd, svr->buf, BUFSIZE, 0, &addr, &addrlen) == -1)
		ERROR("recvfrom");

	size = rand() % (MAXDGRAM+1);
	memcpy(svr->buf, svr->pat, size);
	// TODO: End with CRLF

	if (sendto(fd, svr->buf, size, 0, &addr, addrlen) == -1)
		ERROR("sendto");
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

char* humanize_size(char *buf, size_t len, size_t size)
{
	assert(buf != NULL);

	static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB",
				      "PiB", "EiB", "ZiB", "YiB", NULL};
	double val = size;
	int i = 0;

	while (val >= 1024 && units[i+1]) {
		val /= 1024;
		i++;
	}
	snprintf(buf, len, "%.2lf %s", val, units[i]);

	return buf;
}
