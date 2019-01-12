#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <assert.h>
#include <time.h>


#include "event.h"
#include "event-internal.h"
#include "evutil.h"
#include "log.h"
#include "http-internal.h"
#include "evhttp.h"


#define EVHTTP_BASE_SET(x, y) do { \
    if ((x)->base != NULL) event_base_set((x)->base, y);    \
} while (0) 

#define MIN(a,b) (((a)<(b))?(a):(b))



static int evhttp_connected(struct evhttp_connection *evcon);
static void evhttp_request_dispatch(struct evhttp_connection* evcon);
static int socket_connect(int fd, const char *address, unsigned short port);
static int bind_socket(const char *address, u_short port, int reuse);
static int bind_socket_ai(struct addrinfo *ai, int reuse);
int evhttp_accept_socket(struct evhttp *http, int fd);
int event_base_set(struct event_base *base, struct event *ev);
static struct addrinfo *make_addrinfo(const char *address, u_short port);
static void accept_socket(int fd, short what, void *arg);
static void name_from_addr(struct sockaddr *, socklen_t, char **, char **);
static int evhttp_associate_new_request_with_connection(
		struct evhttp_connection *evcon);

static void evhttp_connection_stop_detectclose(
		struct evhttp_connection *evcon);

static int evhttp_add_header_internal(struct evkeyvalq *headers,
		const char *key, const char *value);

static void evhttp_connection_start_detectclose(
		struct evhttp_connection *evcon);

static void evhttp_read_firstline(struct evhttp_connection *evcon,
		struct evhttp_request *req);

static void evhttp_read_header(struct evhttp_connection *evcon,
		struct evhttp_request *req);

static inline void evhttp_send(struct evhttp_request *req, struct evbuffer *databuf);

void evhttp_read(int, short, void *);
void evhttp_write(int, short, void *);

static void accept_ssl_socket(int fd, short what, void *arg);

void
evhttp_ssl_start_read(struct evhttp_connection *evcon);

void
evhttp_ssl_read(int fd, short what, void *arg);
int
evhttp_remove_header(struct evkeyvalq *headers, const char *key)
{
	struct evkeyval *header;

	TAILQ_FOREACH(header, headers, next) {
		if (strcasecmp(header->key, key) == 0)
			break;
	}

	if (header == NULL)
		return (-1);

	/* Free and remove the header that we found */
	TAILQ_REMOVE(headers, header, next);
	free(header->key);
	free(header->value);
	free(header);

	return (0);
}


static const char *
evhttp_method(enum evhttp_cmd_type type)
{
	const char *method;

	switch (type) {
		case EVHTTP_REQ_GET:
			method = "GET";
			break;
		case EVHTTP_REQ_POST:
			method = "POST";
			break;
		case EVHTTP_REQ_HEAD:
			method = "HEAD";
			break;
		default:
			method = NULL;
			break;
	}

	return (method);
}

static int
evhttp_append_to_last_header(struct evkeyvalq *headers, const char *line)
{
	struct evkeyval *header = TAILQ_LAST(headers, evkeyvalq);
	char *newval;
	size_t old_len, line_len;

	if (header == NULL)
		return (-1);

	old_len = strlen(header->value);
	line_len = strlen(line);

	newval = realloc(header->value, old_len + line_len + 1);
	if (newval == NULL)
		return (-1);

	memcpy(newval + old_len, line, line_len + 1);
	header->value = newval;

	return (0);
}



enum message_read_status
evhttp_parse_headers(struct evhttp_request *req, struct evbuffer* buffer)
{
	char *line;
	enum message_read_status status = MORE_DATA_EXPECTED;

	struct evkeyvalq* headers = req->input_headers;
	while ((line = evbuffer_readline(buffer))
			!= NULL) {
		char *skey, *svalue;

		if (*line == '\0') { /* Last header - Done */
			status = ALL_DATA_READ;
			free(line);
			break;
		}

		/* Check if this is a continuation line */
		if (*line == ' ' || *line == '\t') {
			if (evhttp_append_to_last_header(headers, line) == -1)
				goto error;
			free(line);
			continue;
		}

		/* Processing of header lines */
		svalue = line;
		skey = strsep(&svalue, ":");
		if (svalue == NULL)
			goto error;

		svalue += strspn(svalue, " ");

		if (evhttp_add_header(headers, skey, svalue) == -1)
			goto error;

		free(line);
	}

	return (status);

error:
	free(line);
	return (DATA_CORRUPTED);
}




static void
evhttp_add_event(struct event *ev, int timeout, int default_timeout)
{
	if (timeout != 0) {
		struct timeval tv;

		evutil_timerclear(&tv);
		tv.tv_sec = timeout != -1 ? timeout : default_timeout;
		event_add(ev, &tv);
	} else {
		event_add(ev, NULL);
	}
}

static void
evhttp_detect_close_cb(int fd, short what, void *arg)
{
	struct evhttp_connection *evcon = arg;
	evhttp_connection_reset(evcon);
}


static void
evhttp_connection_start_detectclose(struct evhttp_connection *evcon)
{
	evcon->flags |= EVHTTP_CON_CLOSEDETECT;

	if (event_initialized(&evcon->close_ev))
		event_del(&evcon->close_ev);
	event_set(&evcon->close_ev, evcon->fd, EV_READ,
			evhttp_detect_close_cb, evcon);
	EVHTTP_BASE_SET(evcon, &evcon->close_ev);
	event_add(&evcon->close_ev, NULL);
}

static int
evhttp_is_connection_close(int flags, struct evkeyvalq* headers)
{
	if (flags & EVHTTP_PROXY_REQUEST) {
		/* proxy connection */
		const char *connection = evhttp_find_header(headers, "Proxy-Connection");
		return (connection == NULL || strcasecmp(connection, "keep-alive") != 0);
	} else {
		const char *connection = evhttp_find_header(headers, "Connection");
		return (connection != NULL && strcasecmp(connection, "close") == 0);
	}
}



static void
evhttp_connection_done(struct evhttp_connection *evcon)
{
	struct evhttp_request *req = TAILQ_FIRST(&evcon->requests);
	int con_outgoing = evcon->flags & EVHTTP_CON_OUTGOING;
	if (con_outgoing) {
		/* idle or close the connection */
		int need_close;
		TAILQ_REMOVE(&evcon->requests, req, next);
		req->evcon = NULL;

		evcon->state = EVCON_IDLE;

		need_close = 
			evhttp_is_connection_close(req->flags, req->input_headers)||
			evhttp_is_connection_close(req->flags, req->output_headers);

		/* check if we got asked to close the connection */
		if (need_close)
			evhttp_connection_reset(evcon);

		if (TAILQ_FIRST(&evcon->requests) != NULL) {
			/*
			 *			 * We have more requests; reset the connection
			 *						 * and deal with the next request.
			 *									 */
			if (!evhttp_connected(evcon))
				evhttp_connection_connect(evcon);
			else
				evhttp_request_dispatch(evcon);
		} else if (!need_close) {
			/*
			 *			 * The connection is going to be persistent, but we
			 *						 * need to detect if the other side closes it.
			 *									 */
			evhttp_connection_start_detectclose(evcon);
		}
	} else {
		/*
		 *		 * incoming connection - we need to leave the request on the
		 *				 * connection so that we can reply to it.
		 *						 */
		evcon->state = EVCON_WRITING;
	}

	/* notify the user of the request */
	(*req->cb)(req, req->cb_arg);
	/* if this was an outgoing request, we own and it's done. so free it */
	if (con_outgoing) {
		evhttp_request_free(req);
	}
}

static int
evhttp_get_body_length(struct evhttp_request *req)
{
	struct evkeyvalq *headers = req->input_headers;
	const char *content_length;
	const char *connection;

	content_length = evhttp_find_header(headers, "Content-Length");
	connection = evhttp_find_header(headers, "Connection");

	if (content_length == NULL && connection == NULL)
		req->ntoread = -1;
	else if (content_length == NULL &&
			strcasecmp(connection, "Close") != 0) {
		/* Bad combination, we don't know when it will end */
		event_warnx("%s: we got no content length, but the "
				"server wants to keep the connection open: %s.",
				__func__, connection);
		return (-1);
	} else if (content_length == NULL) {
		req->ntoread = -1;
	} else {
		char *endp;
		int64_t ntoread = evutil_strtoll(content_length, &endp, 10);
		if (*content_length == '\0' || *endp != '\0' || ntoread < 0) {
			event_debug(("%s: illegal content length: %s",
						__func__, content_length));
			return (-1);
		}
		req->ntoread = ntoread;
	}

	event_debug(("%s: bytes to read: %lld (in buffer %ld)\n",
				__func__, req->ntoread,
				EVBUFFER_LENGTH(req->evcon->input_buffer)));

	return (0);
}


static enum message_read_status
evhttp_handle_chunked_read(struct evhttp_request *req, struct evbuffer *buf)
{
	int len;

	while ((len = EVBUFFER_LENGTH(buf)) > 0) {
		if (req->ntoread < 0) {
			/* Read chunk size */
			ev_int64_t ntoread;
			char *p = evbuffer_readline(buf);
			char *endp;
			int error;
			if (p == NULL)
				break;
			/* the last chunk is on a new line? */
			if (strlen(p) == 0) {
				free(p);
				continue;
			}
			ntoread = evutil_strtoll(p, &endp, 16);
			error = (*p == '\0' ||
					(*endp != '\0' && *endp != ' ') ||
					ntoread < 0);
			free(p);
			if (error) {
				/* could not get chunk size */
				return (DATA_CORRUPTED);
			}
			req->ntoread = ntoread;
			if (req->ntoread == 0) {
				/* Last chunk */
				return (ALL_DATA_READ);
			}
			continue;
		}

		/* don't have enough to complete a chunk; wait for more */
		if (len < req->ntoread)
			return (MORE_DATA_EXPECTED);

		/* Completed chunk */
		evbuffer_add(req->input_buffer,
				EVBUFFER_DATA(buf), (size_t)req->ntoread);
		evbuffer_drain(buf, (size_t)req->ntoread);
		req->ntoread = -1;
		if (req->chunk_cb != NULL) {
			(*req->chunk_cb)(req, req->cb_arg);
			evbuffer_drain(req->input_buffer,
					EVBUFFER_LENGTH(req->input_buffer));
		}
	}

	return (MORE_DATA_EXPECTED);
}



static void
evhttp_read_trailer(struct evhttp_connection *evcon, struct evhttp_request *req)
{
	struct evbuffer *buf = evcon->input_buffer;

	switch (evhttp_parse_headers(req, buf)) {
		case DATA_CORRUPTED:
			evhttp_connection_fail(evcon, EVCON_HTTP_INVALID_HEADER);
			break;
		case ALL_DATA_READ:
			event_del(&evcon->ev);
			evhttp_connection_done(evcon);
			break;
		case MORE_DATA_EXPECTED:
		default:
			evhttp_add_event(&evcon->ev, evcon->timeout,
					HTTP_READ_TIMEOUT);
			break;
	}
}



static void
evhttp_read_body(struct evhttp_connection *evcon, struct evhttp_request *req)
{
	struct evbuffer *buf = evcon->input_buffer;

	if (req->chunked) {
		switch (evhttp_handle_chunked_read(req, buf)) {
			case ALL_DATA_READ:
				/* finished last chunk */
				evcon->state = EVCON_READING_TRAILER;
				evhttp_read_trailer(evcon, req);
				return;
			case DATA_CORRUPTED:
				/* corrupted data */
				evhttp_connection_fail(evcon,
						EVCON_HTTP_INVALID_HEADER);
				return;
			case REQUEST_CANCELED:
				/* request canceled */
				evhttp_request_free(req);
				return;
			case MORE_DATA_EXPECTED:
			default:
				break;
		}
	} else if (req->ntoread < 0) {
		/* Read until connection close. */
		evbuffer_add_buffer(req->input_buffer, buf);
	} else if (EVBUFFER_LENGTH(buf) >= req->ntoread) {
		/* Completed content length */
		evbuffer_add(req->input_buffer, EVBUFFER_DATA(buf),
				(size_t)req->ntoread);
		evbuffer_drain(buf, (size_t)req->ntoread);
		req->ntoread = 0;
		evhttp_connection_done(evcon);
		return;
	}
	/* Read more! */
	event_set(&evcon->ev, evcon->fd, EV_READ, evhttp_read, evcon);
	EVHTTP_BASE_SET(evcon, &evcon->ev);
	evhttp_add_event(&evcon->ev, evcon->timeout, HTTP_READ_TIMEOUT);
}





static void
evhttp_get_body(struct evhttp_connection *evcon, struct evhttp_request *req)
{
	const char *xfer_enc;

	/* If this is a request without a body, then we are done */
	if (req->kind == EVHTTP_REQUEST && req->type != EVHTTP_REQ_POST) {
		evhttp_connection_done(evcon);
		return;
	}
	evcon->state = EVCON_READING_BODY;
	xfer_enc = evhttp_find_header(req->input_headers, "Transfer-Encoding");
	if (xfer_enc != NULL && strcasecmp(xfer_enc, "chunked") == 0) {
		req->chunked = 1;
		req->ntoread = -1;
	} else {
		if (evhttp_get_body_length(req) == -1) {
			evhttp_connection_fail(evcon,
					EVCON_HTTP_INVALID_HEADER);
			return;
		}
	}
	evhttp_read_body(evcon, req);
}



static void
evhttp_read_header(struct evhttp_connection *evcon, struct evhttp_request *req)
{
	enum message_read_status res;
	int fd = evcon->fd;

	res = evhttp_parse_headers(req, evcon->input_buffer);

	if (res == DATA_CORRUPTED) {
		/* Error while reading, terminate */
		event_debug(("%s: bad header lines on %d\n", __func__, fd));
		evhttp_connection_fail(evcon, EVCON_HTTP_INVALID_HEADER);
		return;
	} else if (res == MORE_DATA_EXPECTED) {
		/* Need more header lines */
		evhttp_add_event(&evcon->ev, 
				evcon->timeout, HTTP_READ_TIMEOUT);
		return;
	}

	
	/* Done reading headers, do the real work */
	switch (req->kind) {
		case EVHTTP_REQUEST:
			event_debug(("%s: checking for post data on %d\n",
						__func__, fd));
			evhttp_get_body(evcon, req);
			break;

		case EVHTTP_RESPONSE:
			if (req->response_code == HTTP_NOCONTENT ||
					req->response_code == HTTP_NOTMODIFIED ||
					(req->response_code >= 100 && req->response_code < 200)) {
				event_debug(("%s: skipping body for code %d\n",
							__func__, req->response_code));
				evhttp_connection_done(evcon);
			} else {
				event_debug(("%s: start of read body for %s on %d\n",
							__func__, req->remote_host, fd));
				evhttp_get_body(evcon, req);
			}
			break;

		default:
			event_warnx("%s: bad header on %d", __func__, fd);
			evhttp_connection_fail(evcon, EVCON_HTTP_INVALID_HEADER);
			break;
	}
}





static int
evhttp_is_connection_keepalive(struct evkeyvalq* headers)
{
	const char *connection = evhttp_find_header(headers, "Connection");
	return (connection != NULL 
			&& strncasecmp(connection, "keep-alive", 10) == 0);
}


static void
evhttp_connection_stop_detectclose(struct evhttp_connection *evcon)
{
	evcon->flags &= ~EVHTTP_CON_CLOSEDETECT;
	event_del(&evcon->close_ev);
}


static int
evhttp_connected(struct evhttp_connection *evcon)
{
	switch (evcon->state) {
		case EVCON_DISCONNECTED:
		case EVCON_CONNECTING:
			return (0);
		case EVCON_IDLE:
		case EVCON_READING_FIRSTLINE:
		case EVCON_READING_HEADERS:
		case EVCON_READING_BODY:
		case EVCON_READING_TRAILER:
		case EVCON_WRITING:
		default:
			return (1);
	}
}

static void
evhttp_make_header_request(struct evhttp_connection *evcon,
		struct evhttp_request *req)
{
	const char *method;

	evhttp_remove_header(req->output_headers, "Proxy-Connection");

	/* Generate request line */
	method = evhttp_method(req->type);
	evbuffer_add_printf(evcon->output_buffer, "%s %s HTTP/%d.%d\r\n",
			method, req->uri, req->major, req->minor);

	/* Add the content length on a post request if missing */
	if (req->type == EVHTTP_REQ_POST &&
			evhttp_find_header(req->output_headers, "Content-Length") == NULL){
		char size[12];
		evutil_snprintf(size, sizeof(size), "%ld",
				(long)EVBUFFER_LENGTH(req->output_buffer));
		evhttp_add_header(req->output_headers, "Content-Length", size);
	}
}

const char *
evhttp_find_header(const struct evkeyvalq *headers, const char *key)
{
	struct evkeyval *header;

	TAILQ_FOREACH(header, headers, next) {
		if (strcasecmp(header->key, key) == 0)
			return (header->value);
	}

	return (NULL);
}


static const char *
html_replace(char ch, char *buf)
{
	switch (ch) {
		case '<':
			return "&lt;";
		case '>':
			return "&gt;";
		case '"':
			return "&quot;";
		case '\'':
			return "&#039;";
		case '&':
			return "&amp;";
		default:
			break;
	}

	/* Echo the character back */
	buf[0] = ch;
	buf[1] = '\0';

	return buf;
}


static struct evhttp_cb *
evhttp_dispatch_callback(struct httpcbq *callbacks, struct evhttp_request *req)
{
	struct evhttp_cb *cb;
	size_t offset = 0;

	/* Test for different URLs */
	char *p = strchr(req->uri, '?');
	if (p != NULL)
		offset = (size_t)(p - req->uri);

	TAILQ_FOREACH(cb, callbacks, next) {
		int res = 0;
		if (p == NULL) {
			res = strcmp(cb->what, req->uri) == 0;
		} else {
			res = ((strncmp(cb->what, req->uri, offset) == 0) &&
					(cb->what[offset] == '\0'));
		}

		if (res)
			return (cb);
	}

	return (NULL);
}


void
evhttp_response_code(struct evhttp_request *req, int code, const char *reason)
{
	req->kind = EVHTTP_RESPONSE;
	req->response_code = code;
	if (req->response_code_line != NULL)
		free(req->response_code_line);
	req->response_code_line = strdup(reason);
}


char *
evhttp_htmlescape(const char *html)
{
	int i, new_size = 0, old_size = strlen(html);
	char *escaped_html, *p;
	char scratch_space[2];

	for (i = 0; i < old_size; ++i)
		new_size += strlen(html_replace(html[i], scratch_space));

	p = escaped_html = malloc(new_size + 1);
	if (escaped_html == NULL)
		event_err(1, "%s: malloc(%d)", __func__, new_size + 1);
	for (i = 0; i < old_size; ++i) {
		const char *replaced = html_replace(html[i], scratch_space);
		/* this is length checked */
		strcpy(p, replaced);
		p += strlen(replaced);
	}

	*p = '\0';

	return (escaped_html);
}



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
		printf("http run...\n");
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
evhttp_ssl_bind_socket(struct evhttp *http, const char *address, u_short port)
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
    res = evhttp_ssl_accept_socket(http, fd);

    if (res != -1)
        event_debug(("Bound to port %d - Awaiting connections ... ",
                    port));

    return (res);
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
evhttp_ssl_accept_socket(struct evhttp *http, int fd)
{
    struct evhttp_bound_socket *bound;
    struct event *ev;
    int res;

    bound = malloc(sizeof(struct evhttp_bound_socket));
    if (bound == NULL)
        return (-1);

    ev = &bound->bind_ev;

    /* Schedule the socket for accepting */
    event_set(ev, fd, EV_READ | EV_PERSIST, accept_ssl_socket, http);
    EVHTTP_BASE_SET(http, ev);

    res = event_add(ev, NULL);

    if (res == -1) {
        free(bound);
        return (-1);
    }

    TAILQ_INSERT_TAIL(&http->sockets, bound, next);

    return (0);
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


//ssl_accept成功结束后的处理
static int
evhttp_ssl_associate_new_request_with_connection(struct evhttp_connection *evcon)
{
    struct evhttp *http = evcon->http_server;
    struct evhttp_request *req;
    if ((req = evhttp_request_new(evhttp_handle_request, http)) == NULL)
        return (-1);

    req->evcon = evcon; /* the request ends up owning the connection */
    req->flags |= EVHTTP_REQ_OWN_CONNECTION;

    TAILQ_INSERT_TAIL(&evcon->requests, req, next);

    req->kind = EVHTTP_REQUEST;

    if ((req->remote_host = strdup(evcon->address)) == NULL)
        event_err(1, "%s: strdup", __func__);
    req->remote_port = evcon->port;
    evhttp_ssl_start_read(evcon);
    return (0);
}





static void
accept_ssl_cb(int fd, short event, void *arg)
{
    struct evhttp_connection *evcon;
    evcon = (struct evhttp_connection *)arg;
    int len= 0;
    int r = SSL_do_handshake(evcon->ssl);
    if (r == 1) {
        printf("ssl connect finished-2\n");
        if (evhttp_ssl_associate_new_request_with_connection(evcon) == -1)
            evhttp_connection_free(evcon);
        return;
    }    
    int err = SSL_get_error(evcon->ssl, r);    
    if (err == SSL_ERROR_WANT_WRITE) {
        event_set(&(evcon->ev), fd, EV_READ, accept_ssl_cb, evcon);
        event_add(&(evcon->ev), NULL);
    } else if (err == SSL_ERROR_WANT_READ) {
        event_set(&(evcon->ev), fd, EV_WRITE, accept_ssl_cb, evcon);
        event_add(&(evcon->ev), NULL);
    } else {
        
        printf("error SSL_do_handshake-2, error=%d\n", err);
        exit(1);
    }


}



static void
accept_ssl_handshake(struct evhttp *http, int fd, struct sockaddr *sa, socklen_t salen)
{

    struct evhttp_connection *evcon;

    evcon = evhttp_get_request_connection(http, fd, sa, salen);
    if (evcon == NULL)
        return;

    if (http->timeout != -1)
        evhttp_connection_set_timeout(evcon, http->timeout);

    evcon->http_server = http;
    TAILQ_INSERT_TAIL(&http->connections, evcon, next);

    evcon->ssl = SSL_new (http->ctx);

    printf("evcon=%p\n", evcon->ssl);
    int r = SSL_set_fd(evcon->ssl, fd);

    assert(evcon->ssl);

    SSL_set_accept_state(evcon->ssl);
    r = SSL_do_handshake(evcon->ssl);

    if (r == 1) {
        if (evhttp_ssl_associate_new_request_with_connection(evcon) == -1)
            evhttp_connection_free(evcon);
        return;
    }    
    
    int err = SSL_get_error(evcon->ssl, r);    

    if (err == SSL_ERROR_WANT_WRITE) {
        printf("write...\n");
        event_set(&(evcon->ev), fd, EV_READ, accept_ssl_cb, evcon);
        event_add(&(evcon->ev), NULL);
    } else if (err == SSL_ERROR_WANT_READ) {

        printf("read...\n");
        event_set(&(evcon->ev), fd, EV_WRITE, accept_ssl_cb, evcon);
        event_add(&(evcon->ev), NULL);
    } else {

        ERR_print_errors_fp(stderr);  

        printf("error SSL_do_handshake-1, error=%d r=%d \n", err, r);
        exit(1);
    }






}

static void
accept_ssl_socket(int fd, short what, void *arg)
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
    accept_ssl_handshake(http, nfd, (struct sockaddr *)&ss, addrlen);
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


void
evhttp_request_free(struct evhttp_request *req)
{
	if (req->remote_host != NULL)
		free(req->remote_host);
	if (req->uri != NULL)
		free(req->uri);
	if (req->response_code_line != NULL)
		free(req->response_code_line);

	evhttp_clear_headers(req->input_headers);
	free(req->input_headers);

	evhttp_clear_headers(req->output_headers);
	free(req->output_headers);

	if (req->input_buffer != NULL)
		evbuffer_free(req->input_buffer);

	if (req->output_buffer != NULL)
		evbuffer_free(req->output_buffer);

	free(req);
}


/*
 *  * Returns an error page.
 *   */

void
evhttp_send_error(struct evhttp_request *req, int error, const char *reason)
{
#define ERR_FORMAT "<HTML><HEAD>\n" \
	"<TITLE>%d %s</TITLE>\n" \
	"</HEAD><BODY>\n" \
	"<H1>Method Not Implemented</H1>\n" \
	"Invalid method in request<P>\n" \
	"</BODY></HTML>\n"

	struct evbuffer *buf = evbuffer_new();

	/* close the connection on error */
	evhttp_add_header(req->output_headers, "Connection", "close");

	evhttp_response_code(req, error, reason);

	evbuffer_add_printf(buf, ERR_FORMAT, error, reason);

	evhttp_send_page(req, buf);

	evbuffer_free(buf);
#undef ERR_FORMAT
}

#define SWAP(x,y) do { \
	(x)->buffer = (y)->buffer; \
	(x)->orig_buffer = (y)->orig_buffer; \
	(x)->misalign = (y)->misalign; \
	(x)->totallen = (y)->totallen; \
	(x)->off = (y)->off; \
} while (0)



static int
evhttp_connection_incoming_fail(struct evhttp_request *req,
		    enum evhttp_connection_error error)
{
	switch (error) {
		case EVCON_HTTP_TIMEOUT:
		case EVCON_HTTP_EOF:
			/* 
			 *		 * these are cases in which we probably should just
			 *				 * close the connection and not send a reply.  this
			 *						 * case may happen when a browser keeps a persistent
			 *								 * connection open and we timeout on the read.
			 *										 */
			return (-1);
		case EVCON_HTTP_INVALID_HEADER:
		default:	/* xxx: probably should just error on default */
			/* the callback looks at the uri to determine errors */
			if (req->uri) {
				free(req->uri);
				req->uri = NULL;
			}

			/* 
			 *		 * the callback needs to send a reply, once the reply has
			 *				 * been send, the connection should get freed.
			 *						 */
			(*req->cb)(req, req->cb_arg);
	}

	return (0);
}


static void
evhttp_connection_retry(int fd, short what, void *arg)
{
	struct evhttp_connection *evcon = arg;

	evcon->state = EVCON_DISCONNECTED;
	evhttp_connection_connect(evcon);
}





static void
evhttp_connectioncb(int fd, short what, void *arg)
{
	struct evhttp_connection *evcon = arg;
	int error;
	socklen_t errsz = sizeof(error);

	if (what == EV_TIMEOUT) {
		event_debug(("%s: connection timeout for \"%s:%d\" on %d",
					__func__, evcon->address, evcon->port, evcon->fd));
		goto cleanup;
	}

	/* Check if the connection completed */
	if (getsockopt(evcon->fd, SOL_SOCKET, SO_ERROR, (void*)&error,
				&errsz) == -1) {
		event_debug(("%s: getsockopt for \"%s:%d\" on %d",
					__func__, evcon->address, evcon->port, evcon->fd));
		goto cleanup;
	}

	if (error) {
		event_debug(("%s: connect failed for \"%s:%d\" on %d: %s",
					__func__, evcon->address, evcon->port, evcon->fd,
					strerror(error)));
		goto cleanup;
	}

	/* We are connected to the server now */
	event_debug(("%s: connected to \"%s:%d\" on %d\n",
				__func__, evcon->address, evcon->port, evcon->fd));

	/* Reset the retry count as we were successful in connecting */
	evcon->retry_cnt = 0;
	evcon->state = EVCON_IDLE;

	/* try to start requests that have queued up on this connection */
	evhttp_request_dispatch(evcon);
	return;

cleanup:
	if (evcon->retry_max < 0 || evcon->retry_cnt < evcon->retry_max) {
		evtimer_set(&evcon->ev, evhttp_connection_retry, evcon);
		EVHTTP_BASE_SET(evcon, &evcon->ev);
		evhttp_add_event(&evcon->ev, MIN(3600, 2 << evcon->retry_cnt),
				HTTP_CONNECT_TIMEOUT);
		evcon->retry_cnt++;
		return;
	}
	evhttp_connection_reset(evcon);

	/* for now, we just signal all requests by executing their callbacks */
	while (TAILQ_FIRST(&evcon->requests) != NULL) {
		struct evhttp_request *request = TAILQ_FIRST(&evcon->requests);
		TAILQ_REMOVE(&evcon->requests, request, next);
		request->evcon = NULL;

		/* we might want to set an error here */
		request->cb(request, request->cb_arg);
		evhttp_request_free(request);
	}
}



int
evhttp_connection_connect(struct evhttp_connection *evcon)
{
	if (evcon->state == EVCON_CONNECTING)
		return (0);

	evhttp_connection_reset(evcon);

	assert(!(evcon->flags & EVHTTP_CON_INCOMING));
	evcon->flags |= EVHTTP_CON_OUTGOING;

	evcon->fd = bind_socket(
			evcon->bind_address, evcon->bind_port, 0 /*reuse*/);
	if (evcon->fd == -1) {
		event_debug(("%s: failed to bind to \"%s\"",
					__func__, evcon->bind_address));
		return (-1);
	}

	if (socket_connect(evcon->fd, evcon->address, evcon->port) == -1) {
		EVUTIL_CLOSESOCKET(evcon->fd); evcon->fd = -1;
		return (-1);
	}

	/* Set up a callback for successful connection setup */
	event_set(&evcon->ev, evcon->fd, EV_WRITE, evhttp_connectioncb, evcon);
	EVHTTP_BASE_SET(evcon, &evcon->ev);
	evhttp_add_event(&evcon->ev, evcon->timeout, HTTP_CONNECT_TIMEOUT);

	evcon->state = EVCON_CONNECTING;

	return (0);
}




void
evhttp_connection_reset(struct evhttp_connection *evcon)
{
	if (event_initialized(&evcon->ev))
		event_del(&evcon->ev);

	if (evcon->fd != -1) {
		/* inform interested parties about connection close */
		if (evhttp_connected(evcon) && evcon->closecb != NULL)
			(*evcon->closecb)(evcon, evcon->closecb_arg);

		EVUTIL_CLOSESOCKET(evcon->fd);
		evcon->fd = -1;
	}
	evcon->state = EVCON_DISCONNECTED;

	evbuffer_drain(evcon->input_buffer,
			EVBUFFER_LENGTH(evcon->input_buffer));
	evbuffer_drain(evcon->output_buffer,
			EVBUFFER_LENGTH(evcon->output_buffer));
}



void
evhttp_connection_fail(struct evhttp_connection *evcon,
		enum evhttp_connection_error error)
{
	struct evhttp_request* req = TAILQ_FIRST(&evcon->requests);
	void (*cb)(struct evhttp_request *, void *);
	void *cb_arg;
	assert(req != NULL);

	if (evcon->flags & EVHTTP_CON_INCOMING) {
		/* 
		 *		 * for incoming requests, there are two different
		 *				 * failure cases.  it's either a network level error
		 *						 * or an http layer error. for problems on the network
		 *								 * layer like timeouts we just drop the connections.
		 *										 * For HTTP problems, we might have to send back a
		 *												 * reply before the connection can be freed.
		 *														 */
		if (evhttp_connection_incoming_fail(req, error) == -1)
			evhttp_connection_free(evcon);
		return;
	}

	/* save the callback for later; the cb might free our object */
	cb = req->cb;
	cb_arg = req->cb_arg;

	TAILQ_REMOVE(&evcon->requests, req, next);
	evhttp_request_free(req);

	/* xxx: maybe we should fail all requests??? */

	/* reset the connection */
	evhttp_connection_reset(evcon);

	/* We are trying the next request that was queued on us */
	if (TAILQ_FIRST(&evcon->requests) != NULL)
		evhttp_connection_connect(evcon);

	/* inform the user */
	if (cb != NULL)
		(*cb)(NULL, cb_arg);
}


void
evhttp_write(int fd, short what, void *arg)
{
	struct evhttp_connection *evcon = arg;
	int n;

	if (what == EV_TIMEOUT) {
		evhttp_connection_fail(evcon, EVCON_HTTP_TIMEOUT);
		return;
	}

	n = evbuffer_write(evcon->output_buffer, fd);
	if (n == -1) {
		event_debug(("%s: evbuffer_write", __func__));
		evhttp_connection_fail(evcon, EVCON_HTTP_EOF);
		return;
	}

	if (n == 0) {
		event_debug(("%s: write nothing", __func__));
		evhttp_connection_fail(evcon, EVCON_HTTP_EOF);
		return;
	}

	if (EVBUFFER_LENGTH(evcon->output_buffer) != 0) {
		evhttp_add_event(&evcon->ev, 
				evcon->timeout, HTTP_WRITE_TIMEOUT);
		return;
	}

	/* Activate our call back */
	if (evcon->cb != NULL)
		(*evcon->cb)(evcon, evcon->cb_arg);
}

void
evhttp_ssl_start_read(struct evhttp_connection *evcon)
{
	/* Set up an event to read the headers */
	if (event_initialized(&evcon->ev))
		event_del(&evcon->ev);

	event_set(&evcon->ev, evcon->fd, EV_READ, evhttp_ssl_read, evcon);
	EVHTTP_BASE_SET(evcon, &evcon->ev);

	evhttp_add_event(&evcon->ev, evcon->timeout, HTTP_READ_TIMEOUT);
	evcon->state = EVCON_READING_FIRSTLINE;
}

void
evhttp_start_read(struct evhttp_connection *evcon)
{
	/* Set up an event to read the headers */
	if (event_initialized(&evcon->ev))
		event_del(&evcon->ev);
	event_set(&evcon->ev, evcon->fd, EV_READ, evhttp_read, evcon);
	EVHTTP_BASE_SET(evcon, &evcon->ev);

	evhttp_add_event(&evcon->ev, evcon->timeout, HTTP_READ_TIMEOUT);
	evcon->state = EVCON_READING_FIRSTLINE;
}



void
evhttp_send_reply(struct evhttp_request *req, int code, const char *reason,
		struct evbuffer *databuf)
{
	evhttp_response_code(req, code, reason);

	evhttp_send(req, databuf);
}


static void
evhttp_send_done(struct evhttp_connection *evcon, void *arg)
{
	int need_close;
	struct evhttp_request *req = TAILQ_FIRST(&evcon->requests);
	TAILQ_REMOVE(&evcon->requests, req, next);

	/* delete possible close detection events */
	evhttp_connection_stop_detectclose(evcon);

	need_close =
		(req->minor == 0 &&
		 !evhttp_is_connection_keepalive(req->input_headers))||
		evhttp_is_connection_close(req->flags, req->input_headers) ||
		evhttp_is_connection_close(req->flags, req->output_headers);

	assert(req->flags & EVHTTP_REQ_OWN_CONNECTION);
	evhttp_request_free(req);

	if (need_close) {
		evhttp_connection_free(evcon);
		return;
	} 

	/* we have a persistent connection; try to accept another request. */
	if (evhttp_associate_new_request_with_connection(evcon) == -1)
		evhttp_connection_free(evcon);
}



static inline void
evhttp_send(struct evhttp_request *req, struct evbuffer *databuf)
{
	struct evhttp_connection *evcon = req->evcon;

	assert(TAILQ_FIRST(&evcon->requests) == req);

	/* xxx: not sure if we really should expose the data buffer this way */
	if (databuf != NULL)
		evbuffer_add_buffer(req->output_buffer, databuf);

	/* Adds headers to the response */
	evhttp_make_header(evcon, req);

	evhttp_write_buffer(evcon, evhttp_send_done, NULL);
}

void
evhttp_send_page(struct evhttp_request *req, struct evbuffer *databuf)
{
	if (!req->major || !req->minor) {
		req->major = 1;
		req->minor = 1;
	}

	if (req->kind != EVHTTP_RESPONSE)
		evhttp_response_code(req, 200, "OK");

	evhttp_clear_headers(req->output_headers);
	evhttp_add_header(req->output_headers, "Content-Type", "text/html");
	evhttp_add_header(req->output_headers, "Connection", "close");

	evhttp_send(req, databuf);
}


void
evhttp_clear_headers(struct evkeyvalq *headers)
{
	struct evkeyval *header;

	for (header = TAILQ_FIRST(headers);
			header != NULL;
			header = TAILQ_FIRST(headers)) {
		TAILQ_REMOVE(headers, header, next);
		free(header->key);
		free(header->value);
		free(header);
	}
}


static int
evhttp_header_is_valid_value(const char *value)
{
	const char *p = value;

	while ((p = strpbrk(p, "\r\n")) != NULL) {
		/* we really expect only one new line */
		p += strspn(p, "\r\n");
		/* we expect a space or tab for continuation */
		if (*p != ' ' && *p != '\t')
			return (0);
	}
	return (1);
}

static int
evhttp_add_header_internal(struct evkeyvalq *headers,
		const char *key, const char *value)
{
	struct evkeyval *header = calloc(1, sizeof(struct evkeyval));
	if (header == NULL) {
		event_warn("%s: calloc", __func__);
		return (-1);
	}
	if ((header->key = strdup(key)) == NULL) {
		free(header);
		event_warn("%s: strdup", __func__);
		return (-1);
	}
	if ((header->value = strdup(value)) == NULL) {
		free(header->key);
		free(header);
		event_warn("%s: strdup", __func__);
		return (-1);
	}

	TAILQ_INSERT_TAIL(headers, header, next);

	return (0);
}



int
evhttp_add_header(struct evkeyvalq *headers,
		    const char *key, const char *value)
{
	event_debug(("%s: key: %s val: %s\n", __func__, key, value));

	if (strchr(key, '\r') != NULL || strchr(key, '\n') != NULL) {
		/* drop illegal headers */
		event_debug(("%s: dropping illegal header key\n", __func__));
		return (-1);
	}

	if (!evhttp_header_is_valid_value(value)) {
		event_debug(("%s: dropping illegal header value\n", __func__));
		return (-1);
	}

	return (evhttp_add_header_internal(headers, key, value));
}

static void
evhttp_maybe_add_date_header(struct evkeyvalq *headers)
{
	if (evhttp_find_header(headers, "Date") == NULL) {
		char date[50];
#ifndef WIN32
		struct tm cur;
#endif
		struct tm *cur_p;
		time_t t = time(NULL);
#ifdef WIN32
		cur_p = gmtime(&t);
#else
		gmtime_r(&t, &cur);
		cur_p = &cur;
#endif
		if (strftime(date, sizeof(date),
					"%a, %d %b %Y %H:%M:%S GMT", cur_p) != 0) {
			evhttp_add_header(headers, "Date", date);
		}
	}
}

static void
evhttp_maybe_add_content_length_header(struct evkeyvalq *headers,
		long content_length)
{
	if (evhttp_find_header(headers, "Transfer-Encoding") == NULL &&
			evhttp_find_header(headers,	"Content-Length") == NULL) {
		char len[12];
		evutil_snprintf(len, sizeof(len), "%ld", content_length);
		evhttp_add_header(headers, "Content-Length", len);
	}
}


static void
evhttp_make_header_response(struct evhttp_connection *evcon,
		struct evhttp_request *req)
{
	int is_keepalive = evhttp_is_connection_keepalive(req->input_headers);
	evbuffer_add_printf(evcon->output_buffer, "HTTP/%d.%d %d %s\r\n",
			req->major, req->minor, req->response_code,
			req->response_code_line);

	if (req->major == 1) {
		if (req->minor == 1)
			evhttp_maybe_add_date_header(req->output_headers);

		/*
		 *		 * if the protocol is 1.0; and the connection was keep-alive
		 *				 * we need to add a keep-alive header, too.
		 *						 */
		if (req->minor == 0 && is_keepalive)
			evhttp_add_header(req->output_headers,
					"Connection", "keep-alive");

		if (req->minor == 1 || is_keepalive) {
			/* 
			 *			 * we need to add the content length if the
			 *						 * user did not give it, this is required for
			 *									 * persistent connections to work.
			 *												 */
			evhttp_maybe_add_content_length_header(
					req->output_headers,
					(long)EVBUFFER_LENGTH(req->output_buffer));
		}
	}

	/* Potentially add headers for unidentified content. */
	if (EVBUFFER_LENGTH(req->output_buffer)) {
		if (evhttp_find_header(req->output_headers,
					"Content-Type") == NULL) {
			evhttp_add_header(req->output_headers,
					"Content-Type", "text/html; charset=ISO-8859-1");
		}
	}

	/* if the request asked for a close, we send a close, too */
	if (evhttp_is_connection_close(req->flags, req->input_headers)) {
		evhttp_remove_header(req->output_headers, "Connection");
		if (!(req->flags & EVHTTP_PROXY_REQUEST))
			evhttp_add_header(req->output_headers, "Connection", "close");
		evhttp_remove_header(req->output_headers, "Proxy-Connection");
	}
}


void
evhttp_make_header(struct evhttp_connection *evcon, struct evhttp_request *req)
{
	struct evkeyval *header;

	/*
	 *	 * Depending if this is a HTTP request or response, we might need to
	 *		 * add some new headers or remove existing headers.
	 *			 */
	if (req->kind == EVHTTP_REQUEST) {
		evhttp_make_header_request(evcon, req);
	} else {
		evhttp_make_header_response(evcon, req);
	}

	TAILQ_FOREACH(header, req->output_headers, next) {
		evbuffer_add_printf(evcon->output_buffer, "%s: %s\r\n",
				header->key, header->value);
	}
	evbuffer_add(evcon->output_buffer, "\r\n", 2);

	if (EVBUFFER_LENGTH(req->output_buffer) > 0) {
		/*
		 *		 * For a request, we add the POST data, for a reply, this
		 *				 * is the regular data.
		 *						 */
		evbuffer_add_buffer(evcon->output_buffer, req->output_buffer);
	}
}

void
evhttp_write_buffer(struct evhttp_connection *evcon,
		void (*cb)(struct evhttp_connection *, void *), void *arg)
{
	event_debug(("%s: preparing to write buffer\n", __func__));

	/* Set call back */
	evcon->cb = cb;
	evcon->cb_arg = arg;

	/* check if the event is already pending */
	if (event_pending(&evcon->ev, EV_WRITE|EV_TIMEOUT, NULL))
		event_del(&evcon->ev);

	event_set(&evcon->ev, evcon->fd, EV_WRITE, evhttp_write, evcon);
	EVHTTP_BASE_SET(evcon, &evcon->ev);
	evhttp_add_event(&evcon->ev, evcon->timeout, HTTP_WRITE_TIMEOUT);
}


static int
socket_connect(int fd, const char *address, unsigned short port)
{
	struct addrinfo *ai = make_addrinfo(address, port);
	int res = -1;

	if (ai == NULL) {
		event_debug(("%s: make_addrinfo: \"%s:%d\"",
					__func__, address, port));
		return (-1);
	}

	if (connect(fd, ai->ai_addr, ai->ai_addrlen) == -1) {
		if (errno != EINPROGRESS) {
			goto out;
		}
	}

	/* everything is fine */
	res = 0;

out:
	freeaddrinfo(ai);
	return (res);
}




static int
evhttp_parse_request_line(struct evhttp_request *req, char *line)
{
	char *method;
	char *uri;
	char *version;

	/* Parse the request line */
	method = strsep(&line, " ");
	if (line == NULL)
		return (-1);
	uri = strsep(&line, " ");
	if (line == NULL)
		return (-1);
	version = strsep(&line, " ");

	if (line != NULL)
		return (-1);
	/* First line */
	if (strcmp(method, "GET") == 0) {
		req->type = EVHTTP_REQ_GET;
	} else if (strcmp(method, "POST") == 0) {
		req->type = EVHTTP_REQ_POST;
	} else if (strcmp(method, "HEAD") == 0) {
		req->type = EVHTTP_REQ_HEAD;
	} else {
		event_debug(("%s: bad method %s on request %p from %s",
					__func__, method, req, req->remote_host));
		return (-1);
	}

	if (strcmp(version, "HTTP/1.0") == 0) {
		req->major = 1;
		req->minor = 0;
	} else if (strcmp(version, "HTTP/1.1") == 0) {
		req->major = 1;
		req->minor = 1;
	} else {
		event_debug(("%s: bad version %s on request %p from %s",
					__func__, version, req, req->remote_host));
		return (-1);
	}

	if ((req->uri = strdup(uri)) == NULL) {
		event_debug(("%s: evhttp_decode_uri", __func__));
		return (-1);
	}

	/* determine if it's a proxy request */
	if (strlen(req->uri) > 0 && req->uri[0] != '/')
		req->flags |= EVHTTP_PROXY_REQUEST;

	return (0);
}

static int
evhttp_valid_response_code(int code)
{
	if (code == 0)
		return (0);

	return (1);
}



static int
evhttp_parse_response_line(struct evhttp_request *req, char *line)
{
	char *protocol;
	char *number;
	char *readable;

	protocol = strsep(&line, " ");
	if (line == NULL)
		return (-1);
	number = strsep(&line, " ");
	if (line == NULL)
		return (-1);
	readable = line;

	if (strcmp(protocol, "HTTP/1.0") == 0) {
		req->major = 1;
		req->minor = 0;
	} else if (strcmp(protocol, "HTTP/1.1") == 0) {
		req->major = 1;
		req->minor = 1;
	} else {
		event_debug(("%s: bad protocol \"%s\"",
					__func__, protocol));
		return (-1);
	}

	req->response_code = atoi(number);
	if (!evhttp_valid_response_code(req->response_code)) {
		event_debug(("%s: bad response code \"%s\"",
					__func__, number));
		return (-1);
	}

	if ((req->response_code_line = strdup(readable)) == NULL)
		event_err(1, "%s: strdup", __func__);

	return (0);
}



enum message_read_status
evhttp_parse_firstline(struct evhttp_request *req, struct evbuffer *buffer)
{
	char *line;
	enum message_read_status status = ALL_DATA_READ;

	line = evbuffer_readline(buffer);
	if (line == NULL)
		return (MORE_DATA_EXPECTED);

	switch (req->kind) {
		case EVHTTP_REQUEST:
			if (evhttp_parse_request_line(req, line) == -1)
				status = DATA_CORRUPTED;
			break;
		case EVHTTP_RESPONSE:
			if (evhttp_parse_response_line(req, line) == -1)
				status = DATA_CORRUPTED;
			break;
		default:
			status = DATA_CORRUPTED;
	}

	free(line);
	return (status);
}


static void
evhttp_read_firstline(struct evhttp_connection *evcon,
				      struct evhttp_request *req)
{
	enum message_read_status res;

	res = evhttp_parse_firstline(req, evcon->input_buffer);


	if (res == DATA_CORRUPTED) {
		/* Error while reading, terminate */
		event_debug(("%s: bad header lines on %d\n",
					__func__, evcon->fd));
		evhttp_connection_fail(evcon, EVCON_HTTP_INVALID_HEADER);
		return;
	} else if (res == MORE_DATA_EXPECTED) {
		/* Need more header lines */
		evhttp_add_event(&evcon->ev, 
				evcon->timeout, HTTP_READ_TIMEOUT);
		return;
	}

	evcon->state = EVCON_READING_HEADERS;
	evhttp_read_header(evcon, req);
}


void
evhttp_ssl_read(int fd, short what, void *arg)
{
	struct evhttp_connection *evcon = arg;
	struct evhttp_request *req = TAILQ_FIRST(&evcon->requests);
	struct evbuffer *buf = evcon->input_buffer;
	int n, len;

	if (what == EV_TIMEOUT) {
		evhttp_connection_fail(evcon, EVCON_HTTP_TIMEOUT);
		return;
	}
	n = evbuffer_ssl_read(buf, fd, -1, evcon->ssl);
	len = EVBUFFER_LENGTH(buf);
	event_debug(("%s: got %d on %d\n", __func__, n, fd));
	if (n == -1) {
		if (errno != EINTR && errno != EAGAIN) {
			event_debug(("%s: evbuffer_read", __func__));
			evhttp_connection_fail(evcon, EVCON_HTTP_EOF);
		} else {
			evhttp_add_event(&evcon->ev, evcon->timeout,
					HTTP_READ_TIMEOUT);	       
		}
		return;
	} else if (n == 0) {
		/* Connection closed */
		evhttp_connection_done(evcon);
		return;
	}

	switch (evcon->state) {
		case EVCON_READING_FIRSTLINE:
			evhttp_read_firstline(evcon, req);
			break;
		case EVCON_READING_HEADERS:
			evhttp_read_header(evcon, req);
			break;
		case EVCON_READING_BODY:
			evhttp_read_body(evcon, req);
			break;
		case EVCON_READING_TRAILER:
			evhttp_read_trailer(evcon, req);
			break;
		case EVCON_DISCONNECTED:
		case EVCON_CONNECTING:
		case EVCON_IDLE:
		case EVCON_WRITING:
		default:
			event_errx(1, "%s: illegal connection state %d",
					__func__, evcon->state);
	}
}





void
evhttp_read(int fd, short what, void *arg)
{
	struct evhttp_connection *evcon = arg;
	struct evhttp_request *req = TAILQ_FIRST(&evcon->requests);
	struct evbuffer *buf = evcon->input_buffer;
	int n, len;

	if (what == EV_TIMEOUT) {
		evhttp_connection_fail(evcon, EVCON_HTTP_TIMEOUT);
		return;
	}
	n = evbuffer_read(buf, fd, -1);
	len = EVBUFFER_LENGTH(buf);
	event_debug(("%s: got %d on %d\n", __func__, n, fd));

	if (n == -1) {
		if (errno != EINTR && errno != EAGAIN) {
			event_debug(("%s: evbuffer_read", __func__));
			evhttp_connection_fail(evcon, EVCON_HTTP_EOF);
		} else {
			evhttp_add_event(&evcon->ev, evcon->timeout,
					HTTP_READ_TIMEOUT);	       
		}
		return;
	} else if (n == 0) {
		/* Connection closed */
		evhttp_connection_done(evcon);
		return;
	}

	switch (evcon->state) {
		case EVCON_READING_FIRSTLINE:
			evhttp_read_firstline(evcon, req);
			break;
		case EVCON_READING_HEADERS:
			evhttp_read_header(evcon, req);
			break;
		case EVCON_READING_BODY:
			evhttp_read_body(evcon, req);
			break;
		case EVCON_READING_TRAILER:
			evhttp_read_trailer(evcon, req);
			break;
		case EVCON_DISCONNECTED:
		case EVCON_CONNECTING:
		case EVCON_IDLE:
		case EVCON_WRITING:
		default:
			event_errx(1, "%s: illegal connection state %d",
					__func__, evcon->state);
	}
}





static void
evhttp_write_connectioncb(struct evhttp_connection *evcon, void *arg)
{
	/* This is after writing the request to the server */
	struct evhttp_request *req = TAILQ_FIRST(&evcon->requests);
	assert(req != NULL);

	assert(evcon->state == EVCON_WRITING);

	/* We are done writing our header and are now expecting the response */
	req->kind = EVHTTP_RESPONSE;

	evhttp_start_read(evcon);
}



static void
evhttp_request_dispatch(struct evhttp_connection* evcon)
{
	struct evhttp_request *req = TAILQ_FIRST(&evcon->requests);

	/* this should not usually happy but it's possible */
	if (req == NULL)
		return;

	/* delete possible close detection events */
	evhttp_connection_stop_detectclose(evcon);

	/* we assume that the connection is connected already */
	assert(evcon->state == EVCON_IDLE);

	evcon->state = EVCON_WRITING;

	/* Create the header from the store arguments */
	evhttp_make_header(evcon, req);

	evhttp_write_buffer(evcon, evhttp_write_connectioncb, NULL);
}


void
evhttp_set_cb(struct evhttp *http, const char *uri,
        void (*cb)(struct evhttp_request *, void *), void *cbarg)
{
    struct evhttp_cb *http_cb;

    if ((http_cb = calloc(1, sizeof(struct evhttp_cb))) == NULL)
        event_err(1, "%s: calloc", __func__);

    http_cb->what = strdup(uri);
    http_cb->cb = cb;
    http_cb->cbarg = cbarg;

    TAILQ_INSERT_TAIL(&http->callbacks, http_cb, next);
}


void
evhttp_free(struct evhttp* http)
{
    struct evhttp_cb *http_cb;
    struct evhttp_connection *evcon;
    struct evhttp_bound_socket *bound;
    int fd;

    /* Remove the accepting part */
    while ((bound = TAILQ_FIRST(&http->sockets)) != NULL) {
        TAILQ_REMOVE(&http->sockets, bound, next);

        fd = bound->bind_ev.ev_fd;
        event_del(&bound->bind_ev);
        EVUTIL_CLOSESOCKET(fd);

        free(bound);
    }

    while ((evcon = TAILQ_FIRST(&http->connections)) != NULL) {
        /* evhttp_connection_free removes the connection */
        evhttp_connection_free(evcon);
    }

    while ((http_cb = TAILQ_FIRST(&http->callbacks)) != NULL) {
        TAILQ_REMOVE(&http->callbacks, http_cb, next);
        free(http_cb->what);
        free(http_cb);
    }

    free(http);
}




