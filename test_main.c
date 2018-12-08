#include <stdio.h>
#include <stdlib.h>

#include "event.h"




int called = 0;
#define NEVENT	20000

struct event *ev[NEVENT];


static int
rand_int(int n)
{
	return (int)(random() % n);
}



static void
time_cb(int fd, short event, void *arg)
{
	struct timeval tv;
	int i, j;
	called++;
	if (called < 10*NEVENT) {
		for (i = 0; i < 10; i++) {
			j = rand_int(NEVENT);
			tv.tv_sec = 0;
			tv.tv_usec = rand_int(50000);
			if (tv.tv_usec % 2)
				evtimer_add(ev[j], &tv);
			else
				evtimer_del(ev[j]);
		}
	}
}




int main(int argv ,char* agrs[])
{
	struct timeval tv;
	int i;
	event_init();
	for (i = 0; i < NEVENT; i++) {
		ev[i] = malloc(sizeof(struct event));
		/* Initalize one event */
		evtimer_set(ev[i], time_cb, ev[i]);
		tv.tv_sec = 0;
		tv.tv_usec = rand_int(50000);
		evtimer_add(ev[i], &tv);
	}
	event_dispatch();
    printf("test\n");
}
