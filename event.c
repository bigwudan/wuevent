#include <time.h>
#include <stdio.h>
#include <stdlib.h>

#include "event.h"
#include "event-internal.h"
#include "log.h"
#include "evutil.h"

extern const struct eventop epollops;

/* In order of preference */
static const struct eventop *eventops[] = {
    &epollops,
    NULL
};





/* Global state */
struct event_base *current_base = NULL;
static int use_monotonic;




static void
detect_monotonic(void)
{
    use_monotonic = 1;
}

static int
gettime(struct event_base *base, struct timeval *tp)
{
    if (base->tv_cache.tv_sec) {
        *tp = base->tv_cache;
        return (0);
    }
    if (use_monotonic) {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
            return (-1);
        tp->tv_sec = ts.tv_sec;
        tp->tv_usec = ts.tv_nsec / 1000;
        return (0);
    }
}





struct event_base *
event_init(void)
{
        struct event_base *base = event_base_new();
            if (base != NULL)
                        current_base = base;
                return (base);
}


struct event_base *
event_base_new(void)
{
    int i;
    struct event_base *base;

    if ((base = calloc(1, sizeof(struct event_base))) == NULL)
        event_err(1, "%s: calloc", __func__);
    detect_monotonic();
    gettime(base, &base->event_tv);

    min_heap_ctor(&base->timeheap);
    TAILQ_INIT(&base->eventqueue);
    base->sig.ev_signal_pair[0] = -1;
    base->sig.ev_signal_pair[1] = -1;

    base->evbase = NULL;
    for (i = 0; eventops[i] && !base->evbase; i++) {
        base->evsel = eventops[i];

        base->evbase = base->evsel->init(base);
    }

    if (base->evbase == NULL)
        event_errx(1, "%s: no event mechanism available", __func__);

    if (evutil_getenv("EVENT_SHOW_METHOD")) 
        event_msgx("libevent using: %s\n",
                base->evsel->name);

    /* allocate a single active event queue */
    event_base_priority_init(base, 1);

    return (base);
}

int
event_base_priority_init(struct event_base *base, int npriorities)
{
    int i;
    if (base->event_count_active)
        return (-1);
    if (base->nactivequeues && npriorities != base->nactivequeues) {
        for (i = 0; i < base->nactivequeues; ++i) {
            free(base->activequeues[i]);
        }
        free(base->activequeues);
    }
    /* Allocate our priority queues */
    base->nactivequeues = npriorities;
    base->activequeues = (struct event_list **)
        calloc(base->nactivequeues, sizeof(struct event_list *));
    if (base->activequeues == NULL)
        event_err(1, "%s: calloc", __func__);
    for (i = 0; i < base->nactivequeues; ++i) {
        base->activequeues[i] = malloc(sizeof(struct event_list));
        if (base->activequeues[i] == NULL)
            event_err(1, "%s: malloc", __func__);
        TAILQ_INIT(base->activequeues[i]);
    }
    return (0);
}



