#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <assert.h>


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
static void name_from_addr(struct sockaddr *, socklen_t, char **, char **);
static int evhttp_associate_new_request_with_connection(
		struct evhttp_connection *evcon);


static void
evhttp_handle_request(struct evhttp_request *req, void *arg)
{
	struct evhttp *http = arg;
	struct evhttp_cb *cb = NULL;

	if (req->uri == NULL) {
		evhttp_send_error(req, HTTP_BADREQUEST, "Bad Request");
		return;
	}

	if ((cb = evhttp_dispatch_callback(&http->callbacks, req)) != NULL) {
		(*cb->cb)(req, cb->cbarg);
		return;
	}

	/* Generic call back */
	if (http->gencb) {
		(*http->gencb)(req, http->gencbarg);
		return;
	} else {
		/* We need to send a 404 here */
#define ERR_FORMAT "<html><head>" \
		"<title>404 Not Found</title>" \
		"</head><body>" \
		"<h1>Not Found</h1>" \
		"<p>The requested URL %s was not found on this server.</p>"\
		"</body></html>\n"

		char *escaped_html = evhttp_htmlescape(req->uri);
		struct evbuffer *buf = evbuffer_new();

		evhttp_response_code(req, HTTP_NOTFOUND, "Not Found");

		evbuffer_add_printf(buf, ERR_FORMAT, escaped_html);

		free(escaped_html);

		evhttp_send_page(req, buf);

		evbuffer_free(buf);
#undef ERR_FORMAT
	}
}





int
event_base_set(struct event_base *base, struct event *ev)
{
    /* Only innocent events may be assigned to a different base */
    if (ev->ev_flags != EVLIST_INIT)
        return (-1);

    ev->ev_base = base;
    ev->ev_pri = base->nactivequeues/2;

    return (0);
}

static struct evhttp_connection*
evhttp_get_request_connection(
		struct evhttp* http,
		int fd, struct sockaddr *sa, socklen_t salen)
{
	struct evhttp_connection *evcon;
	char *hostname = NULL, *portname = NULL;

	name_from_addr(sa, salen, &hostname, &portname);
	if (hostname == NULL || portname == NULL) {
		if (hostname) free(hostname);
		if (portname) free(portname);
		return (NULL);
	}

	event_debug(("%s: new request from %s:%s on %d\n",
				__func__, hostname, portname, fd));

	/* we need a connection object to put the http request on */
	evcon = evhttp_connection_new(hostname, atoi(portname));
	free(hostname);
	free(portname);
	if (evcon == NULL)
		return (NULL);

	/* associate the base if we have one*/
	evhttp_connection_set_base(evcon, http->base);

	evcon->flags |= EVHTTP_CON_INCOMING;
	evcon->state = EVCON_READING_FIRSTLINE;

	evcon->fd = fd;

	return (evcon);
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


static void
name_from_addr(struct sockaddr *sa, socklen_t salen,
		char **phost, char **pport)
{
	char ntop[NI_MAXHOST];
	char strport[NI_MAXSERV];
	int ni_result;
	ni_result = getnameinfo(sa, salen,
			ntop, sizeof(ntop), strport, sizeof(strport),
			NI_NUMERICHOST|NI_NUMERICSERV);

	if (ni_result != 0) {
		if (ni_result == EAI_SYSTEM)
			event_err(1, "getnameinfo failed");
		else
			event_errx(1, "getnameinfo failed: %s", gai_strerror(ni_result));
		return;
	}
	*phost = strdup(ntop);
	*pport = strdup(strport);
}

struct evhttp_connection *
evhttp_connection_new(const char *address, unsigned short port)
{
	struct evhttp_connection *evcon = NULL;

	event_debug(("Attempting connection to %s:%d\n", address, port));

	if ((evcon = calloc(1, sizeof(struct evhttp_connection))) == NULL) {
		event_warn("%s: calloc failed", __func__);
		goto error;
	}

	evcon->fd = -1;
	evcon->port = port;

	evcon->timeout = -1;
	evcon->retry_cnt = evcon->retry_max = 0;

	if ((evcon->address = strdup(address)) == NULL) {
		event_warn("%s: strdup failed", __func__);
		goto error;
	}

	if ((evcon->input_buffer = evbuffer_new()) == NULL) {
		event_warn("%s: evbuffer_new failed", __func__);
		goto error;
	}

	if ((evcon->output_buffer = evbuffer_new()) == NULL) {
		event_warn("%s: evbuffer_new failed", __func__);
		goto error;
	}

	evcon->state = EVCON_DISCONNECTED;
	TAILQ_INIT(&evcon->requests);

	return (evcon);

error:
	if (evcon != NULL)
		evhttp_connection_free(evcon);
	return (NULL);
}


void evhttp_connection_set_base(struct evhttp_connection *evcon,
		struct event_base *base)
{
	assert(evcon->base == NULL);
	assert(evcon->state == EVCON_DISCONNECTED);
	evcon->base = base;
}


void
evhttp_connection_set_timeout(struct evhttp_connection *evcon,
		int timeout_in_secs)
{
	evcon->timeout = timeout_in_secs;
}

static int
evhttp_associate_new_request_with_connection(struct evhttp_connection *evcon)
{
	struct evhttp *http = evcon->http_server;
	struct evhttp_request *req;
	if ((req = evhttp_request_new(evhttp_handle_request, http)) == NULL)
		return (-1);

	req->evcon = evcon;	/* the request ends up owning the connection */
	req->flags |= EVHTTP_REQ_OWN_CONNECTION;

	TAILQ_INSERT_TAIL(&evcon->requests, req, next);

	req->kind = EVHTTP_REQUEST;

	if ((req->remote_host = strdup(evcon->address)) == NULL)
		event_err(1, "%s: strdup", __func__);
	req->remote_port = evcon->port;

	evhttp_start_read(evcon);

	return (0);
}

void
evhttp_connection_free(struct evhttp_connection *evcon)
{
	struct evhttp_request *req;

	/* notify interested parties that this connection is going down */
	if (evcon->fd != -1) {
		if (evhttp_connected(evcon) && evcon->closecb != NULL)
			(*evcon->closecb)(evcon, evcon->closecb_arg);
	}

	/* remove all requests that might be queued on this connection */
	while ((req = TAILQ_FIRST(&evcon->requests)) != NULL) {
		TAILQ_REMOVE(&evcon->requests, req, next);
		evhttp_request_free(req);
	}

	if (evcon->http_server != NULL) {
		struct evhttp *http = evcon->http_server;
		TAILQ_REMOVE(&http->connections, evcon, next);
	}

	if (event_initialized(&evcon->close_ev))
		event_del(&evcon->close_ev);

	if (event_initialized(&evcon->ev))
		event_del(&evcon->ev);

	if (evcon->fd != -1)
		EVUTIL_CLOSESOCKET(evcon->fd);

	if (evcon->bind_address != NULL)
		free(evcon->bind_address);

	if (evcon->address != NULL)
		free(evcon->address);

	if (evcon->input_buffer != NULL)
		evbuffer_free(evcon->input_buffer);

	if (evcon->output_buffer != NULL)
		evbuffer_free(evcon->output_buffer);

	free(evcon);
}





/*
 *  * Request related functions
 *   */

struct evhttp_request *
evhttp_request_new(void (*cb)(struct evhttp_request *, void *), void *arg)
{
	struct evhttp_request *req = NULL;

	/* Allocate request structure */
	if ((req = calloc(1, sizeof(struct evhttp_request))) == NULL) {
		event_warn("%s: calloc", __func__);
		goto error;
	}

	req->kind = EVHTTP_RESPONSE;
	req->input_headers = calloc(1, sizeof(struct evkeyvalq));
	if (req->input_headers == NULL) {
		event_warn("%s: calloc", __func__);
		goto error;
	}
	TAILQ_INIT(req->input_headers);

	req->output_headers = calloc(1, sizeof(struct evkeyvalq));
	if (req->output_headers == NULL) {
		event_warn("%s: calloc", __func__);
		goto error;
	}
	TAILQ_INIT(req->output_headers);

	if ((req->input_buffer = evbuffer_new()) == NULL) {
		event_warn("%s: evbuffer_new", __func__);
		goto error;
	}

	if ((req->output_buffer = evbuffer_new()) == NULL) {
		event_warn("%s: evbuffer_new", __func__);
		goto error;
	}

	req->cb = cb;
	req->cb_arg = arg;

	return (req);

error:
	if (req != NULL)
		evhttp_request_free(req);
	return (NULL);
}


