#ifndef _EVENT_INTERNAL_H_
#define _EVENT_INTERNAL_H_

#include "min_heap.h"


struct eventop {
    const char *name;
    void *(*init)(struct event_base *);
    int (*add)(void *, struct event *);
    int (*del)(void *, struct event *);
    int (*dispatch)(struct event_base *, void *, struct timeval *);
    void (*dealloc)(struct event_base *, void *);
    /* set if we need to reinitialize the event base */
    int need_reinit;
};



struct event_base {
    const struct eventop *evsel;
    void *evbase;
    int event_count;        /* counts number of total events */
    int event_count_active; /* counts number of active events */

    int event_gotterm;      /* Set to terminate loop */
    int event_break;        /* Set to terminate loop immediately */

    /* active event management */
    struct event_list **activequeues;
    int nactivequeues;

    /* signal handling info */
    //struct evsignal_info sig;

    struct event_list eventqueue;
    struct timeval event_tv;

    struct min_heap timeheap;

    struct timeval tv_cache;
};


#endif /* _EVENT_INTERNAL_H_ */

