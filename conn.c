#include "conn.h"
#include "skbuf.h"
#include "timer.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/timerfd.h>

#define INITIAL_NEVENT 32
#define MAX_NEVENT 4096

/*forward declare*/
static void accept_cb(struct connection_pool *cp, struct connection *c);
static void connect_cb(struct connection_pool *cp, struct connection *c);
static void read_cb(struct connection_pool *cp, struct connection *c);
static void write_cb(struct connection_pool *cp, struct connection *c);

static int set_non_blocking(int fd)
{
	int flag = fcntl(fd, F_GETFL, 0);
	if (flag == -1)
		return -1;
	return fcntl(fd, F_SETFL, flag | O_NONBLOCK);
}

static int connection_add(struct connection_pool *cp, int fd, struct connection *c)
{
	struct epoll_event ev;
	ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
	ev.data.ptr = c;

	if (epoll_ctl(cp->epfd, EPOLL_CTL_ADD, fd, &ev) == -1)
		return -1;
	
	/* TODO lock */
	
	c->next = cp->conns;
	if (cp->conns)
		cp->conns->prev = c;
	cp->conns = c;
	c->prev = NULL;
	cp->n_conns++;

	/* TODO unlock */

	c->fd = fd;

	return 0;
}

static int connection_del(struct connection_pool *cp, int fd, struct connection *c)
{
	struct epoll_event ev;

	/* TODO lock */

	if (c->prev == NULL)
		cp->conns = c->next;
	else
		c->prev->next = c->next;
	if (c->next != NULL)
		c->next->prev = c->prev;
	cp->conns--;

	c->next = cp->free_conns;
	if (cp->free_conns)
		cp->free_conns->prev = c;
	cp->free_conns = c;
	c->prev = NULL;
	cp->n_free_conns++;

	/* TODO unlock */

	ev.events = 0;
	ev.data.ptr = NULL;
	if (epoll_ctl(cp->epfd, EPOLL_CTL_DEL, fd, &ev) == -1)
		return -1;
	return 0;
}

static struct connection *get_connection(struct connection_pool *cp)
{
	struct connection *c;

	/* TODO lock */

	if (cp->n_free_conns == 0)
	{
		c = NULL;
		goto done;
	}
	
	c = cp->free_conns;
	cp->free_conns = c->next;
	cp->free_conns->prev = NULL;
	c->next = NULL;
	cp->n_free_conns--;

done:
	/* TODO unlock */
	return c;
}

static void free_connection(struct connection_pool *cp, struct connection *c)
{
	/* TODO lock */

	c->next = cp->free_conns;
	cp->free_conns->prev = c;
	cp->free_conns = c;
	c->prev = NULL;
	cp->n_free_conns++;

	/* TODO unlock */
}

static int get_timeout(struct connection_pool *cp, struct itimerspec *ts)
{
	ts->it_interval.tv_sec = 0;
	ts->it_interval.tv_nsec = 0;

	long usec = wait_duration_usec(cp, 5 * 1000 * 1000);
	ts->it_value.tv_sec = usec / 1000000;
	ts->it_value.tv_nsec = usec ? (usec % 1000000) * 1000 : 1;

	return usec ? 0 : TFD_TIMER_ABSTIME;
}

static void update_timeout(struct connection_pool *cp)
{
	struct itimerspec new_timeout;
	struct itimerspec old_timeout;
	int flags = get_timeout(cp, &new_timeout);
	timerfd_settime(cp->timerfd, flags, &new_timeout, &old_timeout);
}

struct connection_pool *connection_pool_new(int max)
{
	int epfd;
	struct epoll_event *ees;
	int timerfd;
	struct heap* h;
	struct connection_pool *cp;
	struct connection *cs;
	struct connection *next;
	struct skbuf *bufs;
	int i;

	if (max <= 0)
		return NULL;

	cp->max = max;

	/* epollfd */
	if ((epfd = epoll_create(max)) == -1)
		goto failed;
	
	cp->epfd = epfd;

	if ((ees = malloc(sizeof(struct epoll_event) * INITIAL_NEVENT)) == NULL)
		goto failed;

	cp->events = ees;
	cp->nevents = INITIAL_NEVENT;

	/* timerfd */
	if ((timerfd = timerfd_create(CLOCK_MONOTONIC, 0)) == -1)
		goto failed;
	
	cp->timerfd = timerfd;

	if (fcntl(timerfd, F_SETFD, FD_CLOEXEC) == -1)
		goto failed;

	if ((h = malloc(sizeof(struct heap))) == NULL)
		goto failed;
	
	cp->timer_queue = h;

	/* connection_pool */
	if ((cp = malloc(sizeof(struct connection_pool))) == NULL)
		goto failed;

	if ((cs = malloc(sizeof(struct connection) * max)) == NULL)
		goto failed;
	
	cp->cs = cs;

	if ((bufs = malloc(sizeof(struct skbuf) * 2 * max)) == NULL)
		goto failed;
	
	cp->bufs = bufs;
	
	/* ----------------initialize from here--------------- */

	/* add timer descriptor to epoll. */
	struct epoll_event ev;
	ev.events = EPOLLIN | EPOLLERR;
	ev.data.ptr = &timerfd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, timerfd, &ev);

	timer_queue_init(cp);
	update_timeout(cp);

	/* connection && skbuf */
	i = max;
	next = NULL;
	do
	{
		i--;

		/* double link */
		cp->cs[i].next = next;
		cp->cs[i].prev = NULL;
		if (next != NULL)
			next->prev = &cp->cs[i];

		cp->cs[i].fd = -1;
		cp->cs[i].recv_buf = &bufs[i * 2];
		cp->cs[i].send_buf = &bufs[i * 2 + 1];

		/* next */
		next = &cp->cs[i];

	} while (i);

	cp->free_conns = next;
	cp->n_free_conns = max;
	cp->conns = NULL;
	cp->n_conns = 0;

	return cp;

failed:
	if (epfd >= 0)
		close(epfd);
	if (ees)
		free(ees);
	if (timerfd >= 0)
		close(timerfd);
	if (h)
		free(h);
	if (cp)
		free(cp);
	if (cs)
		free(cs);
	if (bufs)
		free(bufs);
	return NULL;
}

void connection_pool_free(struct connection_pool *cp)
{
	int i;
	struct connection *c;

	if (cp->cs != NULL)
	{
		for (i = 0; i < cp->max; i++)
		{
			c = &cp->cs[i];
			skbuf_free_all_chains(c->recv_buf->first);
			skbuf_free_all_chains(c->send_buf->first);
		}
		free(cp->cs);
	}
	if (cp->events != NULL)
		free(cp->events);
	timer_queue_destroy(cp);
	if (cp->timerfd >= 0)
		close(cp->timerfd);
	if (cp->epfd >= 0)
		close(cp->epfd);

	free(cp);
}

static void connect_cb(struct connection_pool* cp, struct connection *c)
{
	int res;
	struct connecting *ci;
	socklen_t len = sizeof(int);

	ci = (struct connecting *)c->data;

	if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &res, &len) == -1)
	{
		/* TODO log */
		goto conti;
	}

	if (res != 0)
		goto conti;
	
	ci->status = CONNECT_SUCCESS;
	c->read_cb = read_cb;
	c->write_cb = write_cb;

	/* callback */

	return;
conti:
	ci->status = CONNECT_NONE;
}

static void accept_cb(struct connection_pool *cp, struct connection *c)
{
	struct sockaddr_in remote_addr;
	socklen_t len;
	struct connection *cc;
	
	len = sizeof(struct sockaddr_in);

	/* system call */
	int client_fd = accept(c->fd, (struct sockaddr *)&remote_addr, &len);

	if (client_fd >= 0)
	{
		if (set_non_blocking(client_fd) == -1)
		{
			/* TODO log */
			goto failed;
		}

		cc = get_connection(cp);
		if (cc == NULL)
		{
			/* TODO log */
			goto failed;
		}

		if (connection_add(cp, client_fd, cc) == -1)
		{
			free_connection(cp, cc);
			goto failed;
		}

		cc->data = c->data;
		cc->read_cb = read_cb;
		cc->write_cb = write_cb;
	}

	return;

failed:
	close(client_fd);
}

static void timer_expire_cb(struct connection_pool *cp, struct timer_handler_node *n)
{
	printf("timer_expire_cb!\n");
	struct timer_handler_node *tmp;
	
	tmp = n;
	while (tmp)
	{
		(*(tmp->handler))(cp, tmp->data);
		tmp = tmp->next;
	}
}

static void read_cb(struct connection_pool *cp, struct connection *c)
{
	ssize_t res;

	while (1)
	{
		res = skbuf_read(c->recv_buf, c->fd, -1);
		if (res > 0 || res == -2)
			break;
		else if (res == 0 || res == -3)
		{/*close fd*/
			if (connection_del(cp, c->fd, c) == -1)
			{
				/* TODO log */
			}
			break;
		}
		else if (res == -1)
			continue;
	}
}

static void write_cb(struct connection_pool *cp, struct connection *c)
{
	ssize_t res;

	while (1)
	{
		res = skbuf_write(c->send_buf, c->fd);
		if (res >= 0 || res == -2 || res == -4)
			break;
		else if (res == 3)
		{/*close fd*/
			if (connection_del(cp, c->fd, c) == -1)
			{
				/* TODO log */
			}
			break;
		}
		else if (res == -1)
			continue;
	}
}

int connection_pool_dispatch(struct connection_pool *cp, int timeout)
{
	int i, res;
	void *data;
	struct timer_handler_node *n;
	struct connection *c;
	uint32_t ev;

	/* system call */
	res = epoll_wait(cp->epfd, cp->events, cp->nevents, timeout);

	if (res == -1)
		return -1;

	for (i = 0; i < res; ++i)
	{
		data = cp->events[i].data.ptr;

		if (data == &cp->timerfd)
		{
			get_ready_timers(cp, &n);
			timer_expire_cb(cp, n);

			/* continue */
			update_timeout(cp);
		}
		else
		{
			c = data;

			if (c->fd == -1)
				continue;

			ev = cp->events[i].events;

			if (ev & (EPOLLERR | EPOLLHUP) && (ev & (EPOLLIN | EPOLLOUT)) == 0)
				ev = (EPOLLIN | EPOLLOUT);

			if (ev & EPOLLIN)
				c->read_cb(cp, c);

			if (ev & EPOLLOUT)
				c->write_cb(cp, c);
		}
	}

	/* expand the event queue */
	if (res == cp->nevents && cp->nevents < MAX_NEVENT)
	{
		int new_nevents = cp->nevents * 2;
		struct epoll_event *new_events;

		new_events = realloc(cp->events, sizeof(struct epoll_event) * new_nevents);
		if (new_events)
		{
			cp->events = new_events;
			cp->nevents = new_nevents;
		}
	}

	return 0;
}

#define DEFAULT_LISTEN_BACKLOG 20

struct listening *create_listening(struct connection_pool *cp, void* sockaddr, socklen_t socklen)
{
	struct listening *l;
	struct sockaddr *sa;

	l = malloc(sizeof(struct listening));
	if (l == NULL)
		return NULL;
	
	memset(l, 0, sizeof(struct listening));


	l->next = NULL;
	l->fd = -1;

	sa = malloc(sizeof(socklen));
	if (sa == NULL)
	{
		free(l);
		return NULL;
	}

	memcpy(sa, sockaddr, socklen);

	l->sockaddr = sa;
	l->socklen = socklen;

	l->backlog = DEFAULT_LISTEN_BACKLOG;
	l->conn = NULL;

	/* add to pool->ls */
	l->next = cp->ls;
	cp->ls = l;

	return l;
}

int open_listening_sockets(struct connection_pool *cp)
{
	struct listening *l;
	int fd;
	int reuseaddr;
	struct connection *c;

	reuseaddr = 1;

	l = cp->ls;

	while (l)
	{
		if (l->fd != -1)
			goto next;

		/* create socket */
		fd = socket(l->sockaddr->sa_family, SOCK_STREAM, 0);
		if (fd == -1)
		{
			/* TODO log */
			return -1;
		}

		/* set nonblocking */
		if (set_non_blocking(fd) == -1)
		{
			/* TODO log */
			return -1;
		}

		/* set options */
		if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
				(const void *)&reuseaddr, sizeof(int)) == -1)
		{
			/* TODO log */
			close(fd);
			return -1;
		}

		/* bind */
		if (bind(fd, l->sockaddr, l->socklen) == -1)
		{
			/* TODO log */
			close(fd);
			return -1;
		}

		/* listen */
		if (listen(fd, l->backlog) == -1)
		{
			/* TODO log */
			close(fd);
			return -1;
		}

		l->fd = fd;

		c = get_connection(cp);
		if (c == NULL)
		{
			close(fd);
			return -1;
		}

		c->fd = fd;
		c->data = l;
		l->conn = c;
		c->read_cb = accept_cb;
		c->write_cb = NULL;

		/* add to epoll */
		if (connection_add(cp, fd, c) == -1)
		{
			free_connection(cp, c);
			close(fd);
			return -1;
		}
next:
		l = l->next;
	}

	return 0;
}

void close_listening_sockets(struct connection_pool *cp)
{
	struct listening *l;
	struct connection *c;

	l = cp->ls;

	while (l)
	{
		c = l->conn;
		if (c)
		{
			free_connection(cp, c);
			c->fd = -1;
		}

		close(l->fd);

		/* next */
		l = l->next;
	}
}

struct connecting *create_connecting(struct connection_pool *cp, void* sockaddr, socklen_t socklen)
{
	
	struct connecting *ci;
	struct sockaddr *sa;

	ci = malloc(sizeof(struct connecting));
	if (ci == NULL)
		return NULL;
	
	memset(ci, 0, sizeof(struct connecting));

	ci->next = NULL;

	sa = malloc(sizeof(socklen));
	if (sa == NULL)
	{
		free(ci);
		return NULL;
	}

	memcpy(sa, sockaddr, socklen);

	ci->sockaddr = sa;
	ci->socklen = socklen;

	ci->status = CONNECT_NONE;
	ci->conn = NULL;

	/* add to pool->cis */
	ci->next = cp->cis;
	cp->cis = ci;

	return ci;
}

void connecting_peer(struct connection_pool* cp, struct connecting *ci)
{
	int fd;
	int rc;
	int err;
	struct connection *c;

	do
	{
		if (ci->status != CONNECT_NONE)
		{
			/* TODO log */
			break;
		}

		fd = socket(ci->sockaddr->sa_family, SOCK_STREAM, 0);
		if (fd == -1)
		{
			/* TODO log */
			break;
		}

		c = get_connection(cp);
		if (c == NULL)
		{
			/* TODO log */
			close(fd);
			break;
		}

		/* set nonblocking */
		if (set_non_blocking(fd) == -1)
		{
			/* TODO log */
			close(fd);
			break;
		}

		ci->conn = c;
		c->data = ci;
		c->fd = fd;
		c->read_cb = NULL;
		c->write_cb = connect_cb;

		/* system call */
		rc = connect(fd, ci->sockaddr, ci->socklen);
		if (rc == -1)
		{
			err = errno;
			if (err != EINPROGRESS)
			{
				free_connection(cp, c);
				ci->conn = NULL;
				close(fd);
				break;
			}
		}

		/* add to epoll */
		if (connection_add(cp, fd, c) == -1)
		{
			free_connection(cp, c);
			ci->conn = NULL;
			close(fd);
			break;
		}

		ci->status = CONNECT_PENDING;
		return;

	} while (0);
}

int send_msg(struct connection_pool *cp, struct connection *c, char* msg, size_t sz)
{
	return 0;
}
