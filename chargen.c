#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>

#define DEFPORT 1919
#define BACKLOG 5
#define FDSCHUNK 1024
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

const char usage[] =
"Character Generator by Radosław Dąbrowski\n"
"Usage: chargen";

volatile sig_atomic_t running = 1;

void handler(int sig);
void set_signal_handler(void);

struct fd_list {
	struct pollfd *fds;
	int num;
	int cap;
};

void fd_list_add(struct fd_list *fds, int fd, short events);
void fd_list_remove(struct fd_list *fds, int pos);
void fd_list_clear(struct fd_list *fds);

void create_socket(struct fd_list *fds);
void destroy_socket(struct fd_list *fds);
void connect_client(struct fd_list *fds);
void disconnect_client(struct fd_list *fds, int pos);
char* create_pattern(void);

int main(int argc, char *argv[])
{
	struct fd_list fds = {};
	char *pat, *buf;
	int i, ret;

	set_signal_handler();

	if (argc != 1) {
		puts(usage);
		return EXIT_SUCCESS;
	}

	create_socket(&fds);
	pat = create_pattern();
	if ((buf = malloc(BUFSIZE)) == NULL)
		ERROR("malloc");

	while (running) {
		if (poll(fds.fds, fds.num, -1) == -1) {
			if (errno == EINTR) continue;
			ERROR("poll");
		}

		if (fds.fds[0].revents & POLLIN)
			connect_client(&fds);

		for (i = 1; i < fds.num; i++) {
			if (fds.fds[i].revents & POLLHUP) {
				disconnect_client(&fds, i--);
				continue;
			}

			if (fds.fds[i].revents & POLLIN) {
				ret = read(fds.fds[i].fd, buf, BUFSIZE);
				if (ret == 0) {
					disconnect_client(&fds, i--);
					continue;
				}
				if (ret == -1) {
					if (errno == EINTR) break;
					ERROR("read");
				}
			}

			if (fds.fds[i].revents & POLLOUT) {
				ret = write(fds.fds[i].fd, pat, PATSIZE);
				if (ret == -1) {
					if (errno == EPIPE) {
						disconnect_client(&fds, i--);
						continue;
					}
					if (errno == EINTR) break;
					ERROR("write");
				}
			}
		}
	}

	destroy_socket(&fds);
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

void fd_list_add(struct fd_list *fds, int fd, short events)
{
	assert(fds != NULL);

	if (++fds->num > fds->cap) {
		fds->cap += FDSCHUNK;
		fds->fds = realloc(fds->fds, fds->cap*sizeof(struct pollfd));
		if (fds->fds == NULL)
			ERROR("realloc");
	}

	fds->fds[fds->num-1].fd      = fd;
	fds->fds[fds->num-1].events  = events;
	fds->fds[fds->num-1].revents = 0;
}

void fd_list_remove(struct fd_list *fds, int pos)
{
	assert(fds != NULL);
	assert(0 <= pos && pos < fds->num);

	if (pos != fds->num-1) {
		fds->fds[pos].fd      = fds->fds[fds->num-1].fd;
		fds->fds[pos].events  = fds->fds[fds->num-1].events;
		fds->fds[pos].revents = fds->fds[fds->num-1].revents;
	}

	if (--fds->num <= fds->cap-2*FDSCHUNK) {
		fds->cap -= FDSCHUNK;
		fds->fds = realloc(fds->fds, fds->cap*sizeof(struct pollfd));
		if (fds->fds == NULL)
			ERROR("realloc");
	}
}

void fd_list_clear(struct fd_list *fds)
{
	assert(fds != NULL);

	if (fds->fds != NULL) {
		free(fds->fds);
		fds->fds = NULL;
	}

	fds->num = 0;
	fds->cap = 0;
}

void create_socket(struct fd_list *fds)
{
	assert(fds != NULL);
	assert(fds->num == 0);

	struct sockaddr_in addr = {AF_INET, htons(DEFPORT), {INADDR_ANY}};
	int fd;

	if ((fd = socket(PF_INET, SOCK_STREAM, 0)) == -1)
		ERROR("socket");

	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1)
		ERROR("bind");

	if (listen(fd, BACKLOG) == -1)
		ERROR("listen");

	fd_list_add(fds, fd, POLLIN);
}

void destroy_socket(struct fd_list *fds)
{
	assert(fds != NULL);

	int i;

	for (i = 0; i < fds->num; i++)
		if (TEMP_FAILURE_RETRY(close(fds->fds[i].fd)) == -1)
			ERROR("close");

	fd_list_clear(fds);
}

void connect_client(struct fd_list *fds)
{
	assert(fds != NULL);

	struct sockaddr_in addr;
	socklen_t addrlen = sizeof(addr);
	int fd;

	if ((fd = accept(fds->fds[0].fd, &addr, &addrlen)) == -1)
		ERROR("accept");
	fd_list_add(fds, fd, POLLIN|POLLOUT);
	printf("%s:%hu connected\n",
	       inet_ntoa(addr.sin_addr),
	       ntohs(addr.sin_port));
}

void disconnect_client(struct fd_list *fds, int pos)
{
	assert(fds != NULL);
	assert(0 <= pos && pos < fds->num);

	if (TEMP_FAILURE_RETRY(close(fds->fds[pos].fd)) == -1)
		ERROR("close");
	fd_list_remove(fds, pos);
	printf("disconnected\n"); // TODO: print address of disconnected client
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
