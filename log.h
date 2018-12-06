#ifndef _LOG_H_
#define _LOG_H_


extern void event_err(int eval, const char *fmt, ...);

extern void event_errx(int eval, const char *fmt, ...);


extern void event_msgx(const char *fmt, ...);

#endif
