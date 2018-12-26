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
#include "http-internal.h"
#include "evhttp.h"


int called = 0;
int test_ok;
static struct evhttp *http;
static struct event_base *base;


void http_basic_cb(struct evhttp_request *req, void *arg);

void
http_basic_cb(struct evhttp_request *req, void *arg)
{
    printf("run http_basic_cb..\n ");
}




static struct evhttp *
http_setup(short *pport, struct event_base *base)
{
    int i = 0;
    struct evhttp *myhttp;
    short port = -1;

    /* Try a few different ports */
    myhttp = evhttp_new(base);

    int t_a = evhttp_bind_socket(myhttp, "127.0.0.1", 8080 + i) ;
	printf("t_a=%d\n", t_a);
    
    evhttp_set_cb(myhttp, "/test", http_basic_cb, NULL);





    return (myhttp);
}





static void
http_base_test(void)
{
    struct bufferevent *bev;

    int fd;
    const char *http_request;
    short port = -1;

    test_ok = 0;
    fprintf(stdout, "Testing HTTP Server Event Base: ");

    base = event_init();


    http = http_setup(&port, base);


    printf("test\n");

}



int
main (int argc, char **argv)
{
    http_base_test();


    return (0);
}





