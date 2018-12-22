#ifndef _EVHTTP_H_
#define _EVHTTP_H_

#include "event.h"



/* Response codes */
#define HTTP_OK         200
#define HTTP_NOCONTENT      204
#define HTTP_MOVEPERM       301
#define HTTP_MOVETEMP       302
#define HTTP_NOTMODIFIED    304
#define HTTP_BADREQUEST     400
#define HTTP_NOTFOUND       404
#define HTTP_SERVUNAVAIL    503

struct evhttp;
struct evhttp_request;
struct evkeyvalq;

enum evhttp_cmd_type { EVHTTP_REQ_GET, EVHTTP_REQ_POST, EVHTTP_REQ_HEAD };

enum evhttp_request_kind { EVHTTP_REQUEST, EVHTTP_RESPONSE };

struct evhttp_request {
	struct {
		struct evhttp_request *tqe_next;
		struct evhttp_request **tqe_prev;
	}       next;


	/* the connection object that this request belongs to */
	struct evhttp_connection *evcon;
	int flags;
#define EVHTTP_REQ_OWN_CONNECTION	0x0001
#define EVHTTP_PROXY_REQUEST		0x0002

	struct evkeyvalq *input_headers;
	struct evkeyvalq *output_headers;

	/* address of the remote host and the port connection came from */
	char *remote_host;
	u_short remote_port;

	enum evhttp_request_kind kind;
	enum evhttp_cmd_type type;

	char *uri;			/* uri after HTTP request was parsed */

	char major;			/* HTTP Major number */
	char minor;			/* HTTP Minor number */

	int response_code;		/* HTTP Response code */
	char *response_code_line;	/* Readable response */

	struct evbuffer *input_buffer;	/* read data */
	uint64_t ntoread;
	int chunked;

	struct evbuffer *output_buffer;	/* outgoing post or data */

	/* Callback */
	void (*cb)(struct evhttp_request *, void *);
	void *cb_arg;

	/*
	 *	 * Chunked data callback - call for each completed chunk if
	 *		 * specified.  If not specified, all the data is delivered via
	 *			 * the regular callback.
	 *				 */
	void (*chunk_cb)(struct evhttp_request *, void *);
};






/** Create a new HTTP server
 *  *
 *   * @param base (optional) the event base to receive the HTTP events
 *    * @return a pointer to a newly initialized evhttp server structure
 *     */
struct evhttp *evhttp_new(struct event_base *base);


int evhttp_bind_socket(struct evhttp *http, const char *address, u_short port);
struct evhttp_connection *evhttp_connection_new(
		const char *address, unsigned short port);

void evhttp_connection_set_base(struct evhttp_connection *evcon,
		struct event_base *base);
/** Sets the timeout for events related to this connection */
void evhttp_connection_set_timeout(struct evhttp_connection *evcon,
		int timeout_in_secs);

/** Frees an http connection */
void evhttp_connection_free(struct evhttp_connection *evcon);


struct evhttp_request *evhttp_request_new(
		void (*cb)(struct evhttp_request *, void *), void *arg);


void evhttp_send_error(struct evhttp_request *req, int error,
		const char *reason);


char *evhttp_htmlescape(const char *html);



#endif
