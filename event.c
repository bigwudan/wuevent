#include "event-internal.h"



struct event_base *
event_init(void)
{
        struct event_base *base = event_base_new();

            if (base != NULL)
                        current_base = base;

                return (base);
}
