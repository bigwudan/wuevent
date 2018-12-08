#ifndef _EVUTIL_H_
#define _EVUTIL_H_



extern const char *evutil_getenv(const char *varname);

int evutil_socketpair(int d, int type, int protocol, int sv[2]);

int evutil_make_socket_nonblocking(int sock);



/*
 *  * Manipulation functions for struct timeval
 *   */
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



#define	evutil_timercmp(tvp, uvp, cmp)							\
	(((tvp)->tv_sec == (uvp)->tv_sec) ?							\
	 ((tvp)->tv_usec cmp (uvp)->tv_usec) :						\
	 ((tvp)->tv_sec cmp (uvp)->tv_sec))




#endif
