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
#include <netdb.h>
#include <openssl/ssl.h>
#include <openssl/err.h>



#include "event.h"
#include "evutil.h"
#include "http-internal.h"
#include "evhttp.h"
#include "log.h"


int called = 0;
int test_ok;
static struct evhttp *http;
static struct event_base *base;


void http_basic_cb(struct evhttp_request *req, void *arg);

static void
http_errorcb(struct bufferevent *bev, short what, void *arg)
{
    test_ok = -2;
    event_loopexit(NULL);
}



void
http_basic_cb(struct evhttp_request *req, void *arg)
{

	printf("req->url=%s\n", req->uri);	


    printf("run http_basic_cb..\n ");

	struct evbuffer *evb = evbuffer_new();
	int empty = evhttp_find_header(req->input_headers, "Empty") != NULL;
	event_debug(("%s: called\n", __func__));
	evbuffer_add_printf(evb, "This is funny");

	/* For multi-line headers test */
	{
		const char *multi =
			evhttp_find_header(req->input_headers,"X-multi");
		if (multi) {
			if (strcmp("END", multi + strlen(multi) - 3) == 0)
				test_ok++;
			if (evhttp_find_header(req->input_headers, "X-Last"))
				test_ok++;
		}
	}

	/* injecting a bad content-length */
	if (evhttp_find_header(req->input_headers, "X-Negative"))
		evhttp_add_header(req->output_headers,
				"Content-Length", "-100");

	/* allow sending of an empty reply */
	evhttp_send_reply(req, HTTP_OK, "Everything is fine",
			!empty ? evb : NULL);

	evbuffer_free(evb);






}


static void
http_writecb(struct bufferevent *bev, void *arg)
{
    if (EVBUFFER_LENGTH(bev->output) == 0) {
        /* enable reading of the reply */
        bufferevent_enable(bev, EV_READ);
        test_ok++;
    }
}



static void
http_readcb(struct bufferevent *bev, void *arg)
{
    const char *what = "This is funny";

    event_debug(("%s: %s\n", __func__, EVBUFFER_DATA(bev->input)));

    if (evbuffer_find(bev->input,
                (const unsigned char*) what, strlen(what)) != NULL) {
        struct evhttp_request *req = evhttp_request_new(NULL, NULL);
        enum message_read_status done;

        req->kind = EVHTTP_RESPONSE;
        done = evhttp_parse_firstline(req, bev->input);
        if (done != ALL_DATA_READ)
            goto out;

        done = evhttp_parse_headers(req, bev->input);
        if (done != ALL_DATA_READ)
            goto out;

        if (done == 1 &&
                evhttp_find_header(req->input_headers,
                    "Content-Type") != NULL)
            test_ok++;

out:
        evhttp_request_free(req);
        bufferevent_disable(bev, EV_READ);
        if (base)
            event_base_loopexit(base, NULL);
        else
            event_loopexit(NULL);
    }
}








#ifndef NI_MAXSERV
#define NI_MAXSERV 1024
#endif

static int
http_connect(const char *address, u_short port)
{

    struct addrinfo ai, *aitop;
    char strport[NI_MAXSERV];

    struct sockaddr *sa;
        int slen;
    int fd;

    memset(&ai, 0, sizeof (ai));
    ai.ai_family = AF_INET;
    ai.ai_socktype = SOCK_STREAM;
    snprintf(strport, sizeof (strport), "%d", port);
    if (getaddrinfo(address, strport, &ai, &aitop) != 0) {
        event_warn("getaddrinfo");
        return (-1);
    }
    sa = aitop->ai_addr;
    slen = aitop->ai_addrlen;


    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1)
        event_err(1, "socket failed");

    if (connect(fd, sa, slen) == -1)
        event_err(1, "connect failed");


    freeaddrinfo(aitop);


    return (fd);
}





static struct evhttp *
http_setup(short *pport, struct event_base *base)
{
    int i = 0;
    struct evhttp *myhttp;
    short port = -1;

    /* Try a few different ports */
    myhttp = evhttp_new(base);

    for (i = 0; i < 2; ++i) {
        if (evhttp_bind_socket(myhttp, "127.0.0.1", 8080 + i) != -1) {
            port = 8080 + i;
            break;
        }
    }
    if(port == -1){
        exit(1);
    }
    evhttp_set_cb(myhttp, "/index.php", http_basic_cb, NULL);
    *pport = port;
    return (myhttp);
}




static void
ssl_base_test(void)
{
	int fd;
	int port = 8890;
	SSL *ssl;


	SSL_CTX *sslContext;
	SSL_load_error_strings ();
	SSL_library_init ();
	sslContext = SSL_CTX_new (SSLv23_client_method ());

	if (SSL_CTX_use_certificate_file(sslContext,"key/client.pem", SSL_FILETYPE_PEM) <= 0)  
	{  
		ERR_print_errors_fp(stdout);  
		exit(1);  
	}  


	if (SSL_CTX_use_PrivateKey_file(sslContext, "key/clientkey.pem", SSL_FILETYPE_PEM) <= 0)  
	{  
		ERR_print_errors_fp(stdout);  
		exit(1);  
	}

	if (!SSL_CTX_check_private_key(sslContext))  
	{  
		ERR_print_errors_fp(stdout);  
		exit(1);  
	}  



	if (sslContext == NULL)
		ERR_print_errors_fp (stderr);
    fd = http_connect("127.0.0.1", port);
	ssl = SSL_new (sslContext);
	if(ssl == NULL){
		ERR_print_errors_fp(stderr);
		exit(1);
	}
	if(!SSL_set_fd(ssl, fd) ){
		ERR_print_errors_fp(stderr);
		exit(1);
	}
	if(SSL_connect(ssl) != 1){
		ERR_print_errors_fp(stderr);
		exit(1);
	}



    base = event_init();

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
	printf("port=%d\n", port);
    event_base_dispatch(base);
	while(1);
    fd = http_connect("127.0.0.1", port);

    bev = bufferevent_new(fd, http_readcb, http_writecb,
            http_errorcb, NULL);


    bufferevent_base_set(base, bev);

    http_request =
        "GET /test2 HTTP/1.1\r\n"
        "Host: somehost\r\n"
        "Connection: close\r\n"
        "\r\n";

    bufferevent_write(bev, http_request, strlen(http_request));
    event_base_dispatch(base);

    bufferevent_free(bev);
    EVUTIL_CLOSESOCKET(fd);

    evhttp_free(http);

    event_base_free(base);



    printf("test\n");

}



int
main (int argc, char **argv)
{
    http_base_test();


    return (0);
}





