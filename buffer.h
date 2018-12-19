struct evbuffer {
	u_char *buffer;
	u_char *orig_buffer;

	size_t misalign;
	size_t totallen;
	size_t off;

	void (*cb)(struct evbuffer *, size_t, size_t, void *);
	void *cbarg;
};

static void evbuffer_align(struct evbuffer *buf);

int evbuffer_expand(struct evbuffer *buf, size_t datlen);


void evbuffer_drain(struct evbuffer *buf, size_t len);
