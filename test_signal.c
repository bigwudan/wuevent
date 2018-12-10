#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

#include "event.h"



int called = 0;

static void
signal_cb(int fd, short event, void *arg)
{
    struct event *signal = arg;

    printf("%s: got signal %d\n", __func__, EVENT_SIGNAL(signal));

    if (called >= 2)
        event_del(signal);

    called++;
}

int
main (int argc, char **argv)
{
    

    struct event signal_int;

    /* Initalize the event library */
    event_init();

    /* Initalize one event */
    event_set(&signal_int, SIGINT, EV_SIGNAL|EV_PERSIST, signal_cb,
            &signal_int);

    event_add(&signal_int, NULL);

    event_dispatch();

    return (0);
}





