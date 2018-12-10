#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>


#include "event.h"
#include "evutil.h"



int test_okay = 1;
int called = 0;

static void
read_cb(int fd, short event, void *arg)
{
    printf("read_cb start\n");
    char buf[256];
    int len;

    len = recv(fd, buf, sizeof(buf), 0);

    printf("%s: read %d%s\n", __func__,
            len, len ? "" : " - means EOF");

    if (len) {
        if (!called)
            event_add(arg, NULL);
    } else if (called == 1)
        test_okay = 0;

    called++;
}

#ifndef SHUT_WR
#define SHUT_WR 1
#endif

int
main (int argc, char **argv)
{
    struct event ev;
    const char *test = "test sxxxxxxxtring";
    int pair[2];

    if (evutil_socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == -1)
        return (1);


    send(pair[0], test, strlen(test)+1, 0);
    shutdown(pair[0], SHUT_WR);

    /* Initalize the event library */
    event_init();

    /* Initalize one event */
    event_set(&ev, pair[1], EV_READ, read_cb, &ev);

    event_add(&ev, NULL);

    event_dispatch();

    return (test_okay);
}

