#include <sys/types.h>


#include <sys/time.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>


#include "evutil.h"
#include "event.h"


void bufferevent_read_pressure_cb(struct evbuffer *, size_t, size_t, void *);


static int
bufferevent_add(struct event *ev, int timeout)
{
    struct timeval tv, *ptv = NULL;

    if (timeout) {
        evutil_timerclear(&tv);
        tv.tv_sec = timeout;
        ptv = &tv;
    }

    return (event_add(ev, ptv));
}



void
bufferevent_read_pressure_cb(struct evbuffer *buf, size_t old, size_t now,
        void *arg) {
    struct bufferevent *bufev = arg;
    /* 
     *   * If we are below the watermark then reschedule reading if it's
     *       * still enabled.
     *           */
    if (bufev->wm_read.high == 0 || now < bufev->wm_read.high) {
        evbuffer_setcb(buf, NULL, NULL);

        if (bufev->enabled & EV_READ)
            bufferevent_add(&bufev->ev_read, bufev->timeout_read);
    }
}






static void
bufferevent_readcb(int fd, short event, void *arg)
{
    struct bufferevent *bufev = arg;
    int res = 0;
    short what = EVBUFFER_READ;
    size_t len;
    int howmuch = -1;

    if (event == EV_TIMEOUT) {
        what |= EVBUFFER_TIMEOUT;
        goto error;
    }

    /*
     *   * If we have a high watermark configured then we don't want to
     *       * read more data than would make us reach the watermark.
     *           */
    if (bufev->wm_read.high != 0) {
        howmuch = bufev->wm_read.high - EVBUFFER_LENGTH(bufev->input);
        /* we might have lowered the watermark, stop reading */
        if (howmuch <= 0) {
            struct evbuffer *buf = bufev->input;
            event_del(&bufev->ev_read);
            evbuffer_setcb(buf,
                    bufferevent_read_pressure_cb, bufev);
            return;
        }
    }

    res = evbuffer_read(bufev->input, fd, howmuch);
    if (res == -1) {
        if (errno == EAGAIN || errno == EINTR)
            goto reschedule;
        /* error case */
        what |= EVBUFFER_ERROR;
    } else if (res == 0) {
        /* eof case */
        what |= EVBUFFER_EOF;
    }

    if (res <= 0)
        goto error;

    bufferevent_add(&bufev->ev_read, bufev->timeout_read);

    /* See if this callbacks meets the water marks */
    len = EVBUFFER_LENGTH(bufev->input);
    if (bufev->wm_read.low != 0 && len < bufev->wm_read.low)
        return;
    if (bufev->wm_read.high != 0 && len >= bufev->wm_read.high) {
        struct evbuffer *buf = bufev->input;
        event_del(&bufev->ev_read);

        /* Now schedule a callback for us when the buffer changes */
        evbuffer_setcb(buf, bufferevent_read_pressure_cb, bufev);
    }

    /* Invoke the user callback - must always be called last */
    if (bufev->readcb != NULL)
        (*bufev->readcb)(bufev, bufev->cbarg);
    return;

reschedule:
    bufferevent_add(&bufev->ev_read, bufev->timeout_read);
    return;

error:
    (*bufev->errorcb)(bufev, what, bufev->cbarg);
}



static void
bufferevent_writecb(int fd, short event, void *arg)
{
    struct bufferevent *bufev = arg;
    int res = 0;
    short what = EVBUFFER_WRITE;

    if (event == EV_TIMEOUT) {
        what |= EVBUFFER_TIMEOUT;
        goto error;
    }

    if (EVBUFFER_LENGTH(bufev->output)) {
        res = evbuffer_write(bufev->output, fd);
        if (res == -1) {

            if (errno == EAGAIN ||
                    errno == EINTR ||
                    errno == EINPROGRESS)
                goto reschedule;
            /* error case */
            what |= EVBUFFER_ERROR;



        } else if (res == 0) {
            /* eof case */
            what |= EVBUFFER_EOF;
        }
        if (res <= 0)
            goto error;
    }

    if (EVBUFFER_LENGTH(bufev->output) != 0)
        bufferevent_add(&bufev->ev_write, bufev->timeout_write);

    /*
     *   * Invoke the user callback if our buffer is drained or below the
     *       * low watermark.
     *           */
    if (bufev->writecb != NULL &&
            EVBUFFER_LENGTH(bufev->output) <= bufev->wm_write.low)
        (*bufev->writecb)(bufev, bufev->cbarg);

    return;

reschedule:
    if (EVBUFFER_LENGTH(bufev->output) != 0)
        bufferevent_add(&bufev->ev_write, bufev->timeout_write);
    return;

error:
    (*bufev->errorcb)(bufev, what, bufev->cbarg);
}





struct bufferevent *
bufferevent_new(int fd, evbuffercb readcb, evbuffercb writecb,
            everrorcb errorcb, void *cbarg)
{
    struct bufferevent *bufev;

    if ((bufev = calloc(1, sizeof(struct bufferevent))) == NULL)
        return (NULL);

    if ((bufev->input = evbuffer_new()) == NULL) {
        free(bufev);
        return (NULL);
    }

    if ((bufev->output = evbuffer_new()) == NULL) {
        evbuffer_free(bufev->input);
        free(bufev);
        return (NULL);
    }

    event_set(&bufev->ev_read, fd, EV_READ, bufferevent_readcb, bufev);
    event_set(&bufev->ev_write, fd, EV_WRITE, bufferevent_writecb, bufev);

    bufferevent_setcb(bufev, readcb, writecb, errorcb, cbarg);

    /*
     *   * Set to EV_WRITE so that using bufferevent_write is going to
     *       * trigger a callback.  Reading needs to be explicitly enabled
     *           * because otherwise no data will be available.
     *               */
    bufev->enabled = EV_WRITE;

    return (bufev);
}


int
bufferevent_disable(struct bufferevent *bufev, short event)
{
    if (event & EV_READ) {
        if (event_del(&bufev->ev_read) == -1)
            return (-1);
    }
    if (event & EV_WRITE) {
        if (event_del(&bufev->ev_write) == -1)
            return (-1);
    }

    bufev->enabled &= ~event;
    return (0);
}