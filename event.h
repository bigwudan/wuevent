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

extern struct event_base *event_base_new(void);

extern int  event_base_priority_init(struct event_base *, int);


extern struct event_base *event_init(void);





#endif /* _EVENT_H_ */




