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

/** Create a new HTTP server
 *  *
 *   * @param base (optional) the event base to receive the HTTP events
 *    * @return a pointer to a newly initialized evhttp server structure
 *     */
struct evhttp *evhttp_new(struct event_base *base);


int evhttp_bind_socket(struct evhttp *http, const char *address, u_short port);















#endif
