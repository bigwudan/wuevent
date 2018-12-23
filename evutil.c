#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <fcntl.h>

#include "log.h"
#include "evutil.h"

const char *
evutil_getenv(const char *varname)
{
    return getenv(varname);
}


int
evutil_socketpair(int family, int type, int protocol, int fd[2])
{
    return socketpair(family, type, protocol, fd);
}


int
evutil_make_socket_nonblocking(int fd)
{
    if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1) {
        event_warn("fcntl(O_NONBLOCK)");
        return -1;
    }   
    return 0;
}

int
evutil_snprintf(char *buf, size_t buflen, const char *format, ...)
{
    return 0;
}

int
evutil_vsnprintf(char *buf, size_t buflen, const char *format, va_list ap)
{
	return 0;
}

ev_int64_t
evutil_strtoll(const char *s, char **endptr, int base)
{
	return (ev_int64_t)strtol(s, endptr, base);
}




