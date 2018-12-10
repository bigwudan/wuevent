#include <time.h>
#include <errno.h>
#include <sys/epoll.h> 
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>



#include "event.h"
#include "event-internal.h"
#include "evutil.h"
#include "log.h"
#include "evsignal.h"

/* due to limitations in the epoll interface, we need to keep track of
 *  * all file descriptors outself.
 *   */
struct evepoll {
    struct event *evread;
    struct event *evwrite;
};

struct epollop {
    struct evepoll *fds;
    int nfds;
    struct epoll_event *events;
    int nevents;
    int epfd;
};  




static void *epoll_init (struct event_base *);
static int epoll_add    (void *, struct event *);
static int epoll_del    (void *, struct event *);
static int epoll_dispatch   (struct event_base *, void *, struct timeval *);
static void epoll_dealloc   (struct event_base *, void *);

const struct eventop epollops = {
    "epoll",
    epoll_init,
    epoll_add,
    epoll_del,
    epoll_dispatch,
    epoll_dealloc,
    1 /* need reinit */
};


#ifndef HAVE_SETFD
#define FD_CLOSEONEXEC(x) do { \
    if (fcntl(x, F_SETFD, 1) == -1) \
    event_warn("fcntl(%d, F_SETFD)", x); \
} while (0)
#else
#define FD_CLOSEONEXEC(x)
#endif

#define INITIAL_NFILES 32
#define INITIAL_NEVENTS 32
#define MAX_NEVENTS 4096




static int
epoll_recalc(struct event_base *base, void *arg, int max)
{
    struct epollop *epollop = arg;

    if (max >= epollop->nfds) {
        struct evepoll *fds;
        int nfds;

        nfds = epollop->nfds;
        while (nfds <= max)
            nfds <<= 1;

        fds = realloc(epollop->fds, nfds * sizeof(struct evepoll));
        if (fds == NULL) {
            event_warn("realloc");
            return (-1);
        }
        epollop->fds = fds;
        memset(fds + epollop->nfds, 0,
                (nfds - epollop->nfds) * sizeof(struct evepoll));
        epollop->nfds = nfds;
    }

    return (0);
}


static 
void *epoll_init (struct event_base *base)
{
    int epfd;
    struct epollop *epollop;

    /* Disable epollueue when this environment variable is set */
    if (evutil_getenv("EVENT_NOEPOLL"))
        return (NULL);
    if ((epfd = epoll_create(32000)) == -1) {
        if (errno != ENOSYS)
            event_warn("epoll_create");
        return (NULL);
    }

    FD_CLOSEONEXEC(epfd);
    
    if (!(epollop = calloc(1, sizeof(struct epollop))))
        return (NULL);

    epollop->epfd = epfd;

    /* Initalize fields */
    epollop->events = malloc(INITIAL_NEVENTS * sizeof(struct epoll_event));
    if (epollop->events == NULL) {
        free(epollop);
        return (NULL);
    }
    epollop->nevents = INITIAL_NEVENTS;

    epollop->fds = calloc(INITIAL_NFILES, sizeof(struct evepoll));
    if (epollop->fds == NULL) {
        free(epollop->events);
        free(epollop);
        return (NULL);
    }
    epollop->nfds = INITIAL_NFILES;
    evsignal_init(base);
    return (epollop);
}

static int 
epoll_add    (void *arg, struct event *ev)
{
    struct epollop *epollop = arg;
    struct epoll_event epev = {0, {0}};
    struct evepoll *evep;
    int fd, op, events;

    if (ev->ev_events & EV_SIGNAL)
        return (evsignal_add(ev));

    fd = ev->ev_fd;
    if (fd >= epollop->nfds) {
        /* Extent the file descriptor array as necessary */
        if (epoll_recalc(ev->ev_base, epollop, fd) == -1)
            return (-1);
    }
    evep = &epollop->fds[fd];
    op = EPOLL_CTL_ADD;
    events = 0;
    if (evep->evread != NULL) {
        events |= EPOLLIN;
        op = EPOLL_CTL_MOD;
    }
    if (evep->evwrite != NULL) {
        events |= EPOLLOUT;
        op = EPOLL_CTL_MOD;
    }

    if (ev->ev_events & EV_READ)
        events |= EPOLLIN;
    if (ev->ev_events & EV_WRITE)
        events |= EPOLLOUT;

    epev.data.fd = fd;
    epev.events = events;
    if (epoll_ctl(epollop->epfd, op, ev->ev_fd, &epev) == -1)
        return (-1);
    /* Update events responsible */
    if (ev->ev_events & EV_READ)
        evep->evread = ev;
    if (ev->ev_events & EV_WRITE)
        evep->evwrite = ev;
    return (0);
}

static int 
epoll_del    (void *arg, struct event *ev)
{

    struct epollop *epollop = arg;
    struct epoll_event epev = {0, {0}};
    struct evepoll *evep;
    int fd, events, op;
    int needwritedelete = 1, needreaddelete = 1;

    if (ev->ev_events & EV_SIGNAL)
        return (evsignal_del(ev));

    fd = ev->ev_fd;
    if (fd >= epollop->nfds)
        return (0);
    evep = &epollop->fds[fd];

    op = EPOLL_CTL_DEL;
    events = 0;

    if (ev->ev_events & EV_READ)
        events |= EPOLLIN;
    if (ev->ev_events & EV_WRITE)
        events |= EPOLLOUT;

    if ((events & (EPOLLIN|EPOLLOUT)) != (EPOLLIN|EPOLLOUT)) {
        if ((events & EPOLLIN) && evep->evwrite != NULL) {
            needwritedelete = 0;
            events = EPOLLOUT;
            op = EPOLL_CTL_MOD;
        } else if ((events & EPOLLOUT) && evep->evread != NULL) {
            needreaddelete = 0;
            events = EPOLLIN;
            op = EPOLL_CTL_MOD;
        }
    }

    epev.events = events;
    epev.data.fd = fd;

    if (needreaddelete)
        evep->evread = NULL;
    if (needwritedelete)
        evep->evwrite = NULL;
    if (epoll_ctl(epollop->epfd, op, fd, &epev) == -1)
        return (-1);
    return (0);

}


static int 
epoll_dispatch   (struct event_base *p, void *a, struct timeval *b)
{


}

static void 
epoll_dealloc   (struct event_base *p, void *a)
{
}


