#ifndef _LOG_H_
#define _LOG_H_


extern void event_err(int eval, const char *fmt, ...);

extern void event_errx(int eval, const char *fmt, ...);

void event_warnx(const char *fmt, ...);

extern void event_msgx(const char *fmt, ...);
extern void event_warn(const char *fmt, ...);
#ifdef USE_DEBUG
#define event_debug(x) _event_debugx x
#else
#define event_debug(x) do {;} while (0)
#endif

#endif
