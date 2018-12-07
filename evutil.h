#ifndef _EVUTIL_H_
#define _EVUTIL_H_



extern const char *evutil_getenv(const char *varname);

int evutil_socketpair(int d, int type, int protocol, int sv[2]);


#endif
