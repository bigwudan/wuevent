#ifndef _EVENT_H_
#define _EVENT_H_

#include <sys/types.h>

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

TAILQ_HEAD (event_list, event);
TAILQ_HEAD (evkeyvalq, evkeyval);

#define evtimer_add(ev, tv)		event_add(ev, tv)
#define evtimer_del(ev)			event_del(ev)

#define evtimer_set(ev, cb, arg)	event_set(ev, -1, 0, cb, arg)

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


/**
 *  event_loop() flags
 *   */
/*@{*/
#define EVLOOP_ONCE	0x01	/**< Block at most once. */
#define EVLOOP_NONBLOCK	0x02	/**< Do not block. */
/*@}*/




#endif /* _EVENT_H_ */




