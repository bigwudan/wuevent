#include <time.h>
#include <errno.h>
#include <sys/epoll.h> 
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>



#include "event.h"
#include "event-internal.h"
#include "evutil.h"
#include "log.h"

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

    //evsignal_init(base);

    return (epollop);









}

static int 
epoll_add    (void *a, struct event *p)
{

}

static int 
epoll_del    (void *a, struct event *p)
{



}


static int 
epoll_dispatch   (struct event_base *p, void *a, struct timeval *b)
{


}

static void 
epoll_dealloc   (struct event_base *p, void *a)
{
}


