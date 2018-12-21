#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>


#include "event.h"
#include "event-internal.h"
#include "evutil.h"
#include "log.h"
#include "http-internal.h"
#include "evhttp.h"


#define EVHTTP_BASE_SET(x, y) do { \
    if ((x)->base != NULL) event_base_set((x)->base, y);    \
} while (0) 



static int bind_socket(const char *address, u_short port, int reuse);
static int bind_socket_ai(struct addrinfo *ai, int reuse);
int evhttp_accept_socket(struct evhttp *http, int fd);
int event_base_set(struct event_base *base, struct event *ev);
static struct addrinfo *make_addrinfo(const char *address, u_short port);
static void accept_socket(int fd, short what, void *arg);


int
event_base_set(struct event_base *base, struct event *ev)
{
    base->nactivequeues;
    /* Only innocent events may be assigned to a different base */
/*    if (ev->ev_flags != EVLIST_INIT)
        return (-1);

    ev->ev_base = base;
    ev->ev_pri = base->nactivequeues/2;*/

    return (0);
}


static struct evhttp*
evhttp_new_object(void)
{
    struct evhttp *http = NULL;

    if ((http = calloc(1, sizeof(struct evhttp))) == NULL) {
        event_warn("%s: calloc", __func__);
        return (NULL);
    }

    http->timeout = -1;

    TAILQ_INIT(&http->sockets);
    TAILQ_INIT(&http->callbacks);
    TAILQ_INIT(&http->connections);

    return (http);
}



struct evhttp *
evhttp_new(struct event_base *base)
{
    struct evhttp *http = evhttp_new_object();

    http->base = base;

    return (http);
}

int
evhttp_bind_socket(struct evhttp *http, const char *address, u_short port)
{
    int fd;
    int res;

    if ((fd = bind_socket(address, port, 1 /*reuse*/)) == -1)
        return (-1);

    if (listen(fd, 128) == -1) {
        event_warn("%s: listen", __func__);
        EVUTIL_CLOSESOCKET(fd);
        return (-1);
    }

    res = evhttp_accept_socket(http, fd);

    if (res != -1)
        event_debug(("Bound to port %d - Awaiting connections ... ",
                    port));

    return (res);
}

static int
bind_socket(const char *address, u_short port, int reuse)
{
    int fd;
    struct addrinfo *aitop = NULL;

    /* just create an unbound socket */
    if (address == NULL && port == 0)
        return bind_socket_ai(NULL, 0);
    aitop = make_addrinfo(address, port);
    if (aitop == NULL)
        return (-1);
    fd = bind_socket_ai(aitop, reuse);
    freeaddrinfo(aitop);
    return (fd);
}

static int
bind_socket_ai(struct addrinfo *ai, int reuse)
{
    int fd, on = 1, r;
    int serrno;

    /* Create listen socket */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        event_warn("socket");
        return (-1);
    }

    if (evutil_make_socket_nonblocking(fd) < 0)
        goto out;

    if (fcntl(fd, F_SETFD, 1) == -1) {
        event_warn("fcntl(F_SETFD)");
        goto out;
    }

    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&on, sizeof(on));
    if (reuse) {
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                (void *)&on, sizeof(on));
    }

    if (ai != NULL) {
        r = bind(fd, ai->ai_addr, ai->ai_addrlen);
        if (r == -1)
            goto out;
    }

    return (fd);

out:
    serrno = EVUTIL_SOCKET_ERROR();
    EVUTIL_CLOSESOCKET(fd);
    EVUTIL_SET_SOCKET_ERROR(serrno);
    return (-1);
}

int
evhttp_accept_socket(struct evhttp *http, int fd)
{
    struct evhttp_bound_socket *bound;
    struct event *ev;
    int res;

    bound = malloc(sizeof(struct evhttp_bound_socket));
    if (bound == NULL)
        return (-1);

    ev = &bound->bind_ev;

    /* Schedule the socket for accepting */
    event_set(ev, fd, EV_READ | EV_PERSIST, accept_socket, http);
    EVHTTP_BASE_SET(http, ev);

    res = event_add(ev, NULL);

    if (res == -1) {
        free(bound);
        return (-1);
    }

    TAILQ_INSERT_TAIL(&http->sockets, bound, next);

    return (0);
}


static struct addrinfo *
make_addrinfo(const char *address, u_short port)
{
    struct addrinfo *aitop = NULL;

    struct addrinfo ai;
    char strport[NI_MAXSERV];
    int ai_result;

    memset(&ai, 0, sizeof(ai));
    ai.ai_family = AF_INET;
    ai.ai_socktype = SOCK_STREAM;
    ai.ai_flags = AI_PASSIVE;  /* turn NULL host name into INADDR_ANY */
    evutil_snprintf(strport, sizeof(strport), "%d", port);
    if ((ai_result = getaddrinfo(address, strport, &ai, &aitop)) != 0) {
        if ( ai_result == EAI_SYSTEM )
            event_warn("getaddrinfo");
        else
            event_warnx("getaddrinfo: %s", gai_strerror(ai_result));
        return (NULL);
    }


    return (aitop);
}

static void
accept_socket(int fd, short what, void *arg)
{
    struct evhttp *http = arg;
    struct sockaddr_storage ss;
    socklen_t addrlen = sizeof(ss);
    int nfd;

    if ((nfd = accept(fd, (struct sockaddr *)&ss, &addrlen)) == -1) {
        if (errno != EAGAIN && errno != EINTR)
            event_warn("%s: bad accept", __func__);
        return;
    }
    if (evutil_make_socket_nonblocking(nfd) < 0)
        return;

    evhttp_get_request(http, nfd, (struct sockaddr *)&ss, addrlen);
}

void
evhttp_get_request(struct evhttp *http, int fd,
            struct sockaddr *sa, socklen_t salen)
{
    struct evhttp_connection *evcon;

    evcon = evhttp_get_request_connection(http, fd, sa, salen);
    if (evcon == NULL)
        return;

    /* the timeout can be used by the server to close idle connections */
    if (http->timeout != -1)
        evhttp_connection_set_timeout(evcon, http->timeout);

    /* 
     *   * if we want to accept more than one request on a connection,
     *       * we need to know which http server it belongs to.
     *           */
    evcon->http_server = http;
    TAILQ_INSERT_TAIL(&http->connections, evcon, next);

    if (evhttp_associate_new_request_with_connection(evcon) == -1)
        evhttp_connection_free(evcon);
}







