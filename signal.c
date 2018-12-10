#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <errno.h>

#include "event.h"
#include "event-internal.h"
#include "evsignal.h"
#include "log.h"
#include "evutil.h"

struct event_base *evsignal_base = NULL;

static void evsignal_handler(int sig);




#define FD_CLOSEONEXEC(x) do { \
    if (fcntl(x, F_SETFD, 1) == -1) \
    event_warn("fcntl(%d, F_SETFD)", x); \
} while (0)


/* Helper: set the signal handler for evsignal to handler in base, so that
 *  * we can restore the original handler when we clear the current one. */
int
_evsignal_set_handler(struct event_base *base,
                      int evsignal, void (*handler)(int))
{
    struct sigaction sa;
    struct evsignal_info *sig = &base->sig;
    void *p;

    if (evsignal >= sig->sh_old_max) {
        int new_max = evsignal + 1;
        event_debug(("%s: evsignal (%d) >= sh_old_max (%d), resizing",
                    __func__, evsignal, sig->sh_old_max));
        p = realloc(sig->sh_old, new_max * sizeof(*sig->sh_old));
        if (p == NULL) {
            event_warn("realloc");
            return (-1);
        }

        memset((char *)p + sig->sh_old_max * sizeof(*sig->sh_old),
                0, (new_max - sig->sh_old_max) * sizeof(*sig->sh_old));

        sig->sh_old_max = new_max;
        sig->sh_old = p;
    }

    /* allocate space for previous handler out of dynamic array */
    sig->sh_old[evsignal] = malloc(sizeof *sig->sh_old[evsignal]);
    if (sig->sh_old[evsignal] == NULL) {
        event_warn("malloc");
        return (-1);
    }

    /* save previous handler and setup new handler */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);

    if (sigaction(evsignal, &sa, sig->sh_old[evsignal]) == -1) {
        event_warn("sigaction");
        free(sig->sh_old[evsignal]);
        return (-1);
    }

    return (0);
}










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


int evsignal_add(struct event *ev)
{
    int evsignal;
    struct event_base *base = ev->ev_base;
    struct evsignal_info *sig = &ev->ev_base->sig;

    if (ev->ev_events & (EV_READ | EV_WRITE))
        event_errx(1, "%s: EV_SIGNAL incompatible use", __func__);
    evsignal = EVENT_SIGNAL(ev);
    assert(evsignal >= 0 && evsignal < NSIG);
    if (TAILQ_EMPTY(&sig->evsigevents[evsignal]))
    {
        event_debug(("%s: %p: changing signal handler", __func__, ev));
        if (_evsignal_set_handler(
                    base, evsignal, evsignal_handler) == -1)
            return (-1);

        /* catch signals if they happen quickly */
        evsignal_base = base;

        if (!sig->ev_signal_added)
        {
            if (event_add(&sig->ev_signal, NULL))
                return (-1);
            sig->ev_signal_added = 1;
        }
    }

    /* multiple events may listen to the same signal */
    TAILQ_INSERT_TAIL(&sig->evsigevents[evsignal], ev, ev_signal_next);

    return (0);
}


static void
evsignal_handler(int sig)
{
    int save_errno = errno;

    if (evsignal_base == NULL) {
        event_warn(
                "%s: received signal %d, but have no base configured",
                __func__, sig);
        return;
    }
    evsignal_base->sig.evsigcaught[sig]++;
    evsignal_base->sig.evsignal_caught = 1;
    signal(sig, evsignal_handler);
    /* Wake up our notification mechanism */
    send(evsignal_base->sig.ev_signal_pair[0], "a", 1, 0);
    errno = save_errno;
}






