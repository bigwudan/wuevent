#ifndef _EVUTIL_H_
#define _EVUTIL_H_

#include <errno.h>
#include <sys/time.h>

extern const char *evutil_getenv(const char *varname);

int evutil_socketpair(int d, int type, int protocol, int sv[2]);

int evutil_make_socket_nonblocking(int sock);

int evutil_snprintf(char *buf, size_t buflen, const char *format, ...);
int evutil_vsnprintf(char *buf, size_t buflen, const char *format, va_list ap);



/*
 *  * Manipulation functions for struct timeval
 *   */
#define evutil_gettimeofday(tv, tz) gettimeofday((tv), (tz))

#ifdef _EVENT_HAVE_TIMERADD
#define evutil_timeradd(tvp, uvp, vvp) timeradd((tvp), (uvp), (vvp))
#define evutil_timersub(tvp, uvp, vvp) timersub((tvp), (uvp), (vvp))
#else
#define evutil_timeradd(tvp, uvp, vvp)							\
	do {														\
		(vvp)->tv_sec = (tvp)->tv_sec + (uvp)->tv_sec;			\
		(vvp)->tv_usec = (tvp)->tv_usec + (uvp)->tv_usec;       \
		if ((vvp)->tv_usec >= 1000000) {						\
			(vvp)->tv_sec++;									\
			(vvp)->tv_usec -= 1000000;							\
		}														\
	} while (0)
#define	evutil_timersub(tvp, uvp, vvp)						\
	do {													\
		(vvp)->tv_sec = (tvp)->tv_sec - (uvp)->tv_sec;		\
		(vvp)->tv_usec = (tvp)->tv_usec - (uvp)->tv_usec;	\
		if ((vvp)->tv_usec < 0) {							\
			(vvp)->tv_sec--;								\
			(vvp)->tv_usec += 1000000;						\
		}													\
	} while (0)
#endif /* !_EVENT_HAVE_HAVE_TIMERADD */

#ifdef _EVENT_HAVE_TIMERCLEAR
#define evutil_timerclear(tvp) timerclear(tvp)
#else
#define	evutil_timerclear(tvp)	(tvp)->tv_sec = (tvp)->tv_usec = 0
#endif

#define EVUTIL_CLOSESOCKET(s) close(s)

#define	evutil_timercmp(tvp, uvp, cmp)							\
	(((tvp)->tv_sec == (uvp)->tv_sec) ?							\
	 ((tvp)->tv_usec cmp (uvp)->tv_usec) :						\
	 ((tvp)->tv_sec cmp (uvp)->tv_sec))


#define EVUTIL_SOCKET_ERROR() (errno)
#define EVUTIL_SET_SOCKET_ERROR(errcode)        \
    do { errno = (errcode); } while (0)



#endif
