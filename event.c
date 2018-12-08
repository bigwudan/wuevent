#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "event.h"
#include "event-internal.h"
#include "log.h"
#include "evutil.h"
#include "min_heap.h"

extern const struct eventop epollops;

/* In order of preference */
static const struct eventop *eventops[] = {
    &epollops,
    NULL
};





/* Global state */
struct event_base *current_base = NULL;
static int use_monotonic;

/* Prototypes */
static void	event_queue_insert(struct event_base *, struct event *, int);
static void	event_queue_remove(struct event_base *, struct event *, int);

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

void
event_set(struct event *ev, int fd, short events,
        void (*callback)(int, short, void *), void *arg)
{
    /* Take the current base - caller needs to set the real base later */
    ev->ev_base = current_base;

    ev->ev_callback = callback;
    ev->ev_arg = arg;
    ev->ev_fd = fd;
    ev->ev_events = events;
    ev->ev_res = 0;
    ev->ev_flags = EVLIST_INIT;
    ev->ev_ncalls = 0;
    ev->ev_pncalls = NULL;
    min_heap_elem_init(ev);

    /* by default, we put new events into the middle priority */
    if(current_base)
        ev->ev_pri = current_base->nactivequeues/2;
}

int
event_add(struct event *ev, const struct timeval *tv)
{
	struct event_base *base = ev->ev_base;
	const struct eventop *evsel = base->evsel;
	void *evbase = base->evbase;
	int res = 0;

	event_debug((
				"event_add: event: %p, %s%s%scall %p",
				ev,
				ev->ev_events & EV_READ ? "EV_READ " : " ",
				ev->ev_events & EV_WRITE ? "EV_WRITE " : " ",
				tv ? "EV_TIMEOUT " : " ",
				ev->ev_callback));

	assert(!(ev->ev_flags & ~EVLIST_ALL));

	/*
	 *	 * prepare for timeout insertion further below, if we get a
	 *		 * failure on any step, we should not change any state.
	 *			 */
	if (tv != NULL && !(ev->ev_flags & EVLIST_TIMEOUT)) {
		if (min_heap_reserve(&base->timeheap,
					1 + min_heap_size(&base->timeheap)) == -1)
			return (-1);  /* ENOMEM == errno */
	}

	if ((ev->ev_events & (EV_READ|EV_WRITE|EV_SIGNAL)) &&
			!(ev->ev_flags & (EVLIST_INSERTED|EVLIST_ACTIVE))) {
		res = evsel->add(evbase, ev);
		if (res != -1)
			event_queue_insert(base, ev, EVLIST_INSERTED);
	}

	/* 
	 *	 * we should change the timout state only if the previous event
	 *		 * addition succeeded.
	 *			 */
	if (res != -1 && tv != NULL) {
		struct timeval now;

		/* 
		 *		 * we already reserved memory above for the case where we
		 *				 * are not replacing an exisiting timeout.
		 *						 */
		if (ev->ev_flags & EVLIST_TIMEOUT)
			event_queue_remove(base, ev, EVLIST_TIMEOUT);

		/* Check if it is active due to a timeout.  Rescheduling
		 *		 * this timeout before the callback can be executed
		 *				 * removes it from the active list. */
		if ((ev->ev_flags & EVLIST_ACTIVE) &&
				(ev->ev_res & EV_TIMEOUT)) {
			/* See if we are just active executing this
			 *			 * event in a loop
			 *						 */
			if (ev->ev_ncalls && ev->ev_pncalls) {
				/* Abort loop */
				*ev->ev_pncalls = 0;
			}

			event_queue_remove(base, ev, EVLIST_ACTIVE);
		}

		gettime(base, &now);
		evutil_timeradd(&now, tv, &ev->ev_timeout);

		event_debug((
					"event_add: timeout in %ld seconds, call %p",
					tv->tv_sec, ev->ev_callback));

		event_queue_insert(base, ev, EVLIST_TIMEOUT);
	}

	return (res);
}

int
event_del(struct event *ev)
{
	struct event_base *base;
	const struct eventop *evsel;
	void *evbase;

	event_debug(("event_del: %p, callback %p",
				ev, ev->ev_callback));

	/* An event without a base has not been added */
	if (ev->ev_base == NULL)
		return (-1);

	base = ev->ev_base;
	evsel = base->evsel;
	evbase = base->evbase;

	assert(!(ev->ev_flags & ~EVLIST_ALL));

	/* See if we are just active executing this event in a loop */
	if (ev->ev_ncalls && ev->ev_pncalls) {
		/* Abort loop */
		*ev->ev_pncalls = 0;
	}

	if (ev->ev_flags & EVLIST_TIMEOUT)
		event_queue_remove(base, ev, EVLIST_TIMEOUT);

	if (ev->ev_flags & EVLIST_ACTIVE)
		event_queue_remove(base, ev, EVLIST_ACTIVE);

	if (ev->ev_flags & EVLIST_INSERTED) {
		event_queue_remove(base, ev, EVLIST_INSERTED);
		return (evsel->del(evbase, ev));
	}

	return (0);
}








void
event_queue_insert(struct event_base *base, struct event *ev, int queue)
{
	if (ev->ev_flags & queue) {
		/* Double insertion is possible for active events */
		if (queue & EVLIST_ACTIVE)
			return;

		event_errx(1, "%s: %p(fd %d) already on queue %x", __func__,
				ev, ev->ev_fd, queue);
	}

	if (~ev->ev_flags & EVLIST_INTERNAL)
		base->event_count++;

	ev->ev_flags |= queue;
	switch (queue) {
		case EVLIST_INSERTED:
			TAILQ_INSERT_TAIL(&base->eventqueue, ev, ev_next);
			break;
		case EVLIST_ACTIVE:
			base->event_count_active++;
			TAILQ_INSERT_TAIL(base->activequeues[ev->ev_pri],
					ev,ev_active_next);
			break;
		case EVLIST_TIMEOUT: {
								 min_heap_push(&base->timeheap, ev);
								 break;
							 }
		default:
							 event_errx(1, "%s: unknown queue %x", __func__, queue);
	}
}





void
event_queue_remove(struct event_base *base, struct event *ev, int queue)
{
	if (!(ev->ev_flags & queue))
		event_errx(1, "%s: %p(fd %d) not on queue %x", __func__,
				ev, ev->ev_fd, queue);

	if (~ev->ev_flags & EVLIST_INTERNAL)
		base->event_count--;

	ev->ev_flags &= ~queue;
	switch (queue) {
		case EVLIST_INSERTED:
			TAILQ_REMOVE(&base->eventqueue, ev, ev_next);
			break;
		case EVLIST_ACTIVE:
			base->event_count_active--;
			TAILQ_REMOVE(base->activequeues[ev->ev_pri],
					ev, ev_active_next);
			break;
		case EVLIST_TIMEOUT:
			min_heap_erase(&base->timeheap, ev);
			break;
		default:
			event_errx(1, "%s: unknown queue %x", __func__, queue);
	}
}


