#include <time.h>

#include "event.h"
#include "event-internal.h"



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


static 
void *epoll_init (struct event_base *p)
{


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


