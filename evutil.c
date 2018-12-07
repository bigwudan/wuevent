#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>



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



