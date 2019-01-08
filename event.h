#ifndef _EVENT_H_
#define _EVENT_H_

#include <sys/types.h>
#include <stdarg.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "sys/queue.h"

#ifndef TAILQ_ENTRY
#define _EVENT_DEFINED_TQENTRY
#define TAILQ_ENTRY(type)                       \
    struct {                                \
            struct type *tqe_next;  /* next element */          \
            struct type **tqe_prev; /* address of previous next element */  \
    }
#endif /* !TAILQ_ENTRY */

#define EVLIST_TIMEOUT  0x01
#define EVLIST_INSERTED 0x02
#define EVLIST_SIGNAL   0x04
#define EVLIST_ACTIVE   0x08
#define EVLIST_INTERNAL 0x10
#define EVLIST_INIT 0x80

/* EVLIST_X_ Private space: 0x1000-0xf000 */
#define EVLIST_ALL  (0xf000 | 0x9f)



#define EV_TIMEOUT  0x01
#define EV_READ     0x02
#define EV_WRITE    0x04
#define EV_SIGNAL   0x08
#define EV_PERSIST  0x10    /* Persistant event */



struct event_base;




#ifndef EVENT_NO_STRUCT
struct event {
    TAILQ_ENTRY (event) ev_next;
    TAILQ_ENTRY (event) ev_active_next;
    TAILQ_ENTRY (event) ev_signal_next;
    unsigned int min_heap_idx;  /* for managing timeouts */

    struct event_base *ev_base;

    int ev_fd;
    short ev_events;
    short ev_ncalls;
    short *ev_pncalls;  /* Allows deletes in callback */

    struct timeval ev_timeout;

    int ev_pri;     /* smaller numbers are higher priority */

    void (*ev_callback)(int, short, void *arg);
    void *ev_arg;

    int ev_res;     /* result passed to event callback */
    int ev_flags;
};
#else
struct event;
#endif

struct evkeyval {
	TAILQ_ENTRY(evkeyval) next;

	char *key;
	char *value;
};

TAILQ_HEAD (event_list, event);
TAILQ_HEAD (evkeyvalq, evkeyval);

#define evtimer_add(ev, tv)		event_add(ev, tv)
#define evtimer_del(ev)			event_del(ev)

#define evtimer_set(ev, cb, arg)	event_set(ev, -1, 0, cb, arg)

#define EVENT_SIGNAL(ev)    (int)(ev)->ev_fd
#define EVENT_FD(ev)        (int)(ev)->ev_fd


extern struct event_base *event_base_new(void);
extern int  event_base_priority_init(struct event_base *, int);
extern struct event_base *event_init(void);
void event_set(struct event *, int, short, void (*)(int, short, void *), void *);
int event_add(struct event *ev, const struct timeval *timeout);
int event_del(struct event *);

void event_active(struct event *, int, short);

int event_dispatch(void);
int event_loop(int);
int event_base_loop(struct event_base *, int);
int event_pending(struct event *ev, short event, struct timeval *tv);


/**
 *  event_loop() flags
 *   */
/*@{*/
#define EVLOOP_ONCE	0x01	/**< Block at most once. */
#define EVLOOP_NONBLOCK	0x02	/**< Do not block. */
/*@}*/

/* Just for error reporting - use other constants otherwise */
#define EVBUFFER_READ       0x01
#define EVBUFFER_WRITE      0x02
#define EVBUFFER_EOF        0x10
#define EVBUFFER_ERROR      0x20
#define EVBUFFER_TIMEOUT    0x40



struct evbuffer {
	u_char *buffer;
	u_char *orig_buffer;

	size_t misalign;
	size_t totallen;
	size_t off;

	void (*cb)(struct evbuffer *, size_t, size_t, void *);
	void *cbarg;
};



struct bufferevent;
typedef void (*evbuffercb)(struct bufferevent *, void *);
typedef void (*everrorcb)(struct bufferevent *, short what, void *);


struct ssl_bufferevent;
typedef void (*evbuffercb_ssl)(struct ssl_bufferevent *, void *);
typedef void (*everrorcb_ssl)(struct ssl_bufferevent *, short what, void *);

struct event_watermark {
    size_t low;
    size_t high;
};


#ifndef EVENT_NO_STRUCT
struct bufferevent {
    struct event_base *ev_base;

    struct event ev_read;
    struct event ev_write;

    struct evbuffer *input;
    struct evbuffer *output;

    struct event_watermark wm_read;
    struct event_watermark wm_write;

    evbuffercb readcb;
    evbuffercb writecb;
    everrorcb errorcb;
    void *cbarg;

    int timeout_read;   /* in seconds */
    int timeout_write;  /* in seconds */

    short enabled;  /* events that are currently enabled */
};
#endif

struct ssl_bufferevent {
    struct event_base *ev_base;

    struct event ev_read;
    struct event ev_write;

    struct evbuffer *input;
    struct evbuffer *output;

    struct event_watermark wm_read;
    struct event_watermark wm_write;

    evbuffercb_ssl readcb;
    evbuffercb_ssl writecb;
    everrorcb_ssl errorcb;
    void *cbarg;

    int timeout_read;   /* in seconds */
    int timeout_write;  /* in seconds */

    short enabled;  /* events that are currently enabled */

	//ssl
	SSL *ssl_fd;
	
};



int event_base_loopexit(struct event_base *, const struct timeval *);

struct evbuffer *evbuffer_new(void);
int evbuffer_add(struct evbuffer *, const void *, size_t);
void evbuffer_drain(struct evbuffer *, size_t);
int evbuffer_expand(struct evbuffer *buf, size_t datlen);

void evbuffer_setcb(struct evbuffer *, void (*)(struct evbuffer *, size_t, size_t, void *), void *);



int evbuffer_add_printf(struct evbuffer *buf, const char *fmt, ...);

void evbuffer_free(struct evbuffer *);

int evbuffer_add_buffer(struct evbuffer *, struct evbuffer *);

int evbuffer_write(struct evbuffer *, int);
int evbuffer_read(struct evbuffer *, int, int);

int evbuffer_ssl_write(struct evbuffer *buffer, SSL *fd);
char *evbuffer_readline(struct evbuffer *);

u_char *evbuffer_find(struct evbuffer *, const u_char *, size_t);



int bufferevent_enable(struct bufferevent *bufev, short event);

int bufferevent_disable(struct bufferevent *bufev, short event);

int bufferevent_base_set(struct event_base *base, struct bufferevent *bufev);

int bufferevent_write(struct bufferevent *bufev,
            const void *data, size_t size);



int event_base_once(struct event_base *base, int fd, short events,
        void (*callback)(int, short, void *), void *arg,
        const struct timeval *timeout);

int event_base_set(struct event_base *, struct event *);


int event_once(int, short, void (*)(int, short, void *), void *,
        const struct timeval *);

int event_base_dispatch(struct event_base *);
void event_base_free(struct event_base *);

int event_loopexit(const struct timeval *);

struct bufferevent *bufferevent_new(int fd,
        evbuffercb readcb, evbuffercb writecb, everrorcb errorcb, void *cbarg);

struct ssl_bufferevent *bufferevent_ssl_new(int fd,
        evbuffercb readcb, evbuffercb writecb, everrorcb errorcb, void *cbarg, SSL *ssl_fd);

void bufferevent_setcb(struct bufferevent *bufev,
        evbuffercb readcb, evbuffercb writecb, everrorcb errorcb, void *cbarg);


void bufferevent_free(struct bufferevent *bufev);



#define event_initialized(ev)		((ev)->ev_flags & EVLIST_INIT)

#define EVBUFFER_LENGTH(x)	(x)->off
#define EVBUFFER_DATA(x)	(x)->buffer
#define EVBUFFER_INPUT(x)	(x)->input
#define EVBUFFER_OUTPUT(x)	(x)->output


#endif /* _EVENT_H_ */




