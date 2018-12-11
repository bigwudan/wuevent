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
extern struct event_base *evsignal_base;
static int use_monotonic;

/* Prototypes */
static void	event_queue_insert(struct event_base *, struct event *, int);
static void	event_queue_remove(struct event_base *, struct event *, int);
static int	event_haveevents(struct event_base *);

static void	event_process_active(struct event_base *);



static int	timeout_next(struct event_base *, struct timeval **);
static void	timeout_process(struct event_base *);
static void	timeout_correct(struct event_base *, struct timeval *);





static void
detect_monotonic(void)
{
    //默认支持
    use_monotonic = 1;
}

static int
gettime(struct event_base *base, struct timeval *tp)
{
    //如果有缓存时间，直接读取缓存时间
    if (base->tv_cache.tv_sec) {
        *tp = base->tv_cache;
        return (0);
    }
    //没有缓存时间，计算系统开始经历的时间
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
    //创建base堆内存
    if ((base = calloc(1, sizeof(struct event_base))) == NULL)
        event_err(1, "%s: calloc", __func__);
    //检查是否支持monotonic(系统开始计算时间)
    detect_monotonic();
    //得到当前时间
    gettime(base, &base->event_tv);
    //初始化最小堆
    min_heap_ctor(&base->timeheap);
    //初始化任务队列
    TAILQ_INIT(&base->eventqueue);
    //信号管道
    base->sig.ev_signal_pair[0] = -1;
    base->sig.ev_signal_pair[1] = -1;
    //事件驱动引擎
    base->evbase = NULL;
    //依据配置读取事件驱动
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
    //有活动失败
    if (base->event_count_active)
        return (-1);
    //存在活动数量，当前需要调整的优先级不等于现在的优化级，清理原来的数据
    if (base->nactivequeues && npriorities != base->nactivequeues) {
        for (i = 0; i < base->nactivequeues; ++i) {
            free(base->activequeues[i]);
        }
        free(base->activequeues);
    }
    /* Allocate our priority queues */
    base->nactivequeues = npriorities;
    //根据优先数，指针数组初始化，
    base->activequeues = (struct event_list **)
        calloc(base->nactivequeues, sizeof(struct event_list *));
    if (base->activequeues == NULL)
        event_err(1, "%s: calloc", __func__);
    //初始化每个优先级数组
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
    //主属性
    ev->ev_base = current_base;
    //回调函数
    ev->ev_callback = callback;
    //参数
    ev->ev_arg = arg;
    //句柄
    ev->ev_fd = fd;
    //事件内心
    ev->ev_events = events;
    //激活事件
    ev->ev_res = 0;
    //事件状态
    ev->ev_flags = EVLIST_INIT;
    //回调次数
    ev->ev_ncalls = 0;
    //回调是否结束
    ev->ev_pncalls = NULL;
    //初始化事件最小堆站,事件对应数组索引
    min_heap_elem_init(ev);

    /* by default, we put new events into the middle priority */
    if(current_base)
        //初始化优先权限
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

	if (res != -1 && tv != NULL) {
		struct timeval now;

		if (ev->ev_flags & EVLIST_TIMEOUT)
			event_queue_remove(base, ev, EVLIST_TIMEOUT);

		if ((ev->ev_flags & EVLIST_ACTIVE) &&
				(ev->ev_res & EV_TIMEOUT)) {
			if (ev->ev_ncalls && ev->ev_pncalls) {
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


int
event_haveevents(struct event_base *base)
{
		return (base->event_count > 0);
}


static void
event_process_active(struct event_base *base)
{
	struct event *ev;
	struct event_list *activeq = NULL;
	int i;
	short ncalls;

	for (i = 0; i < base->nactivequeues; ++i) {
		if (TAILQ_FIRST(base->activequeues[i]) != NULL) {
			activeq = base->activequeues[i];
			break;
		}
	}

	assert(activeq != NULL);

	for (ev = TAILQ_FIRST(activeq); ev; ev = TAILQ_FIRST(activeq)) {
		if (ev->ev_events & EV_PERSIST)
			event_queue_remove(base, ev, EVLIST_ACTIVE);
		else
			event_del(ev);

		/* Allows deletes to work */
		ncalls = ev->ev_ncalls;
		ev->ev_pncalls = &ncalls;
		while (ncalls) {
			ncalls--;
			ev->ev_ncalls = ncalls;
			(*ev->ev_callback)((int)ev->ev_fd, ev->ev_res, ev->ev_arg);
			if (base->event_break)
				return;
		}
	}
}




int
event_dispatch(void)
{
		return (event_loop(0));
}

int
event_loop(int flags)
{
		return event_base_loop(current_base, flags);
}


int
event_base_loop(struct event_base *base, int flags)
{
	const struct eventop *evsel = base->evsel;
	void *evbase = base->evbase;
	struct timeval tv;
	struct timeval *tv_p;
	int res, done;

	/* clear time cache */
	base->tv_cache.tv_sec = 0;

	if (base->sig.ev_signal_added)
		evsignal_base = base;
	done = 0;
	while (!done) {
		/* Terminate the loop if we have been asked to */
		if (base->event_gotterm) {
			base->event_gotterm = 0;
			break;
		}

		if (base->event_break) {
			base->event_break = 0;
			break;
		}

		timeout_correct(base, &tv);

		tv_p = &tv;
		if (!base->event_count_active && !(flags & EVLOOP_NONBLOCK)) {
			timeout_next(base, &tv_p);
		} else {
			/* 
			 *			 * if we have active events, we just poll new events
			 *						 * without waiting.
			 *									 */
			evutil_timerclear(&tv);
		}

		/* If we have no events, we just exit */
		if (!event_haveevents(base)) {
			event_debug(("%s: no events registered.", __func__));
			return (1);
		}

		/* update last old time */
		gettime(base, &base->event_tv);

		/* clear time cache */
		base->tv_cache.tv_sec = 0;

		res = evsel->dispatch(base, evbase, tv_p);

		if (res == -1)
			return (-1);
		gettime(base, &base->tv_cache);

		timeout_process(base);

		if (base->event_count_active) {
			event_process_active(base);
			if (!base->event_count_active && (flags & EVLOOP_ONCE))
				done = 1;
		} else if (flags & EVLOOP_NONBLOCK)
			done = 1;
	}

	/* clear time cache */
	base->tv_cache.tv_sec = 0;

	event_debug(("%s: asked to terminate loop.", __func__));
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
timeout_process(struct event_base *base)
{
	struct timeval now;
	struct event *ev;

	if (min_heap_empty(&base->timeheap))
		return;

	gettime(base, &now);

	while ((ev = min_heap_top(&base->timeheap))) {
		if (evutil_timercmp(&ev->ev_timeout, &now, >))
			break;

		/* delete this event from the I/O queues */
		event_del(ev);

		event_debug(("timeout_process: call %p",
					ev->ev_callback));
		event_active(ev, EV_TIMEOUT, 1);
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


static void
timeout_correct(struct event_base *base, struct timeval *tv)
{
	struct event **pev;
	unsigned int size;
	struct timeval off;

	if (use_monotonic)
		return;

	/* Check if time is running backwards */
	gettime(base, tv);
	if (evutil_timercmp(tv, &base->event_tv, >=)) {
		base->event_tv = *tv;
		return;
	}

	event_debug(("%s: time is running backwards, corrected",
				__func__));
	evutil_timersub(&base->event_tv, tv, &off);

	/*
	 *	 * We can modify the key element of the node without destroying
	 *		 * the key, beause we apply it to all in the right order.
	 *			 */
	pev = base->timeheap.p;
	size = base->timeheap.n;
	for (; size-- > 0; ++pev) {
		struct timeval *ev_tv = &(**pev).ev_timeout;
		evutil_timersub(ev_tv, &off, ev_tv);
	}
	/* Now remember what the new time turned out to be. */
	base->event_tv = *tv;
}


void
event_active(struct event *ev, int res, short ncalls)
{
	/* We get different kinds of events, add them together */
	if (ev->ev_flags & EVLIST_ACTIVE) {
		ev->ev_res |= res;
		return;
	}

	ev->ev_res = res;
	ev->ev_ncalls = ncalls;
	ev->ev_pncalls = NULL;
	event_queue_insert(ev->ev_base, ev, EVLIST_ACTIVE);
}

static int
timeout_next(struct event_base *base, struct timeval **tv_p)
{
	struct timeval now;
	struct event *ev;
	struct timeval *tv = *tv_p;

	if ((ev = min_heap_top(&base->timeheap)) == NULL) {
		/* if no time-based events are active wait for I/O */
		*tv_p = NULL;
		return (0);
	}

	if (gettime(base, &now) == -1)
		return (-1);

	if (evutil_timercmp(&ev->ev_timeout, &now, <=)) {
		evutil_timerclear(tv);
		return (0);
	}

	evutil_timersub(&ev->ev_timeout, &now, tv);

	assert(tv->tv_sec >= 0);
	assert(tv->tv_usec >= 0);

	event_debug(("timeout_next: in %ld seconds", tv->tv_sec));
	return (0);
}





