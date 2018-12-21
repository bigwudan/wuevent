
enum evhttp_connection_state {
    EVCON_DISCONNECTED, /**< not currently connected not trying either*/
    EVCON_CONNECTING,   /**< tries to currently connect */
    EVCON_IDLE,     /**< connection is established */
    EVCON_READING_FIRSTLINE,/**< reading Request-Line (incoming conn) or
                             **< Status-Line (outgoing conn) */
    EVCON_READING_HEADERS,  /**< reading request/response headers */
    EVCON_READING_BODY, /**< reading request/response body */
    EVCON_READING_TRAILER,  /**< reading request/response chunked trailer */
    EVCON_WRITING       /**< writing request/response headers/body */
};




struct evhttp_connection {
    /* we use tailq only if they were created for an http server */
    TAILQ_ENTRY(evhttp_connection) (next);

    int fd;
    struct event ev;
    struct event close_ev;
    struct evbuffer *input_buffer;
    struct evbuffer *output_buffer;

    char *bind_address;     /* address to use for binding the src */
    u_short bind_port;      /* local port for binding the src */

    char *address;          /* address to connect to */
    u_short port;

    int flags;
#define EVHTTP_CON_INCOMING 0x0001  /* only one request on it ever */
#define EVHTTP_CON_OUTGOING 0x0002  /* multiple requests possible */
#define EVHTTP_CON_CLOSEDETECT  0x0004  /* detecting if persistent close */

    int timeout;            /* timeout in seconds for events */
    int retry_cnt;          /* retry count */
    int retry_max;          /* maximum number of retries */

    enum evhttp_connection_state state;

    /* for server connections, the http server they are connected with */
    struct evhttp *http_server;

    TAILQ_HEAD(evcon_requestq, evhttp_request) requests;

    void (*cb)(struct evhttp_connection *, void *);
    void *cb_arg;

    void (*closecb)(struct evhttp_connection *, void *);
    void *closecb_arg;

    struct event_base *base;
};


struct evhttp_cb {
    TAILQ_ENTRY(evhttp_cb) next;

    char *what;

    void (*cb)(struct evhttp_request *req, void *);
    void *cbarg;
};



/* both the http server as well as the rpc system need to queue connections */
TAILQ_HEAD(evconq, evhttp_connection);


/* each bound socket is stored in one of these */
struct evhttp_bound_socket {
    TAILQ_ENTRY(evhttp_bound_socket) (next);

    struct event  bind_ev;
};



struct evhttp {
    TAILQ_HEAD(boundq, evhttp_bound_socket) sockets;

    TAILQ_HEAD(httpcbq, evhttp_cb) callbacks;
    struct evconq connections;

    int timeout;

    void (*gencb)(struct evhttp_request *req, void *);
    void *gencbarg;

    struct event_base *base;
};


void evhttp_get_request(struct evhttp *, int, struct sockaddr *, socklen_t);


