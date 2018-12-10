#ifndef _EVSIGNAL_H_
#define _EVSIGNAL_H_

#include <signal.h>


struct evsignal_info {
    struct event ev_signal;
    int ev_signal_pair[2];
    int ev_signal_added;
    volatile sig_atomic_t evsignal_caught;
    struct event_list evsigevents[NSIG];
    sig_atomic_t evsigcaught[NSIG];
    struct sigaction **sh_old;
    int sh_old_max;
};


extern int evsignal_init(struct event_base *);

void evsignal_process(struct event_base *);
int evsignal_add(struct event *);
int evsignal_del(struct event *);
void evsignal_dealloc(struct event_base *);




#endif
