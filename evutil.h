#ifndef _EVUTIL_H_
#define _EVUTIL_H_



extern const char *evutil_getenv(const char *varname);

int evutil_socketpair(int d, int type, int protocol, int sv[2]);

int evutil_make_socket_nonblocking(int sock);




#endif
