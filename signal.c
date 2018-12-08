#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include "event.h"
#include "event-internal.h"
#include "evsignal.h"
#include "log.h"
#include "evutil.h"

struct event_base *evsignal_base = NULL;

#define FD_CLOSEONEXEC(x) do { \
    if (fcntl(x, F_SETFD, 1) == -1) \
    event_warn("fcntl(%d, F_SETFD)", x); \
} while (0)


static void
evsignal_cb(int fd, short what, void *arg)
{
    static char signals[1];
    ssize_t n;
    n = recv(fd, signals, sizeof(signals), 0);
    if (n == -1)
        event_err(1, "%s: read", __func__);
}


int 
evsignal_init(struct event_base *base)
{
    int i;
    if (evutil_socketpair(
                AF_UNIX, SOCK_STREAM, 0, base->sig.ev_signal_pair) == -1) {
        event_warn("%s: socketpair", __func__);
        return -1;
    }

    FD_CLOSEONEXEC(base->sig.ev_signal_pair[0]);
    FD_CLOSEONEXEC(base->sig.ev_signal_pair[1]);
    base->sig.sh_old = NULL;
    base->sig.sh_old_max = 0;
    base->sig.evsignal_caught = 0;
    memset(&base->sig.evsigcaught, 0, sizeof(sig_atomic_t)*NSIG);
    for (i = 0; i < NSIG; ++i)
        TAILQ_INIT(&base->sig.evsigevents[i]);

    evutil_make_socket_nonblocking(base->sig.ev_signal_pair[0]);

    event_set(&base->sig.ev_signal, base->sig.ev_signal_pair[1],
            EV_READ | EV_PERSIST, evsignal_cb, &base->sig.ev_signal);
    base->sig.ev_signal.ev_base = base;
    base->sig.ev_signal.ev_flags |= EVLIST_INTERNAL;

    return 0;

















}


