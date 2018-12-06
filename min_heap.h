#ifndef _MIN_HEAP_H_
#define _MIN_HEAP_H_



typedef struct min_heap
{
        struct event** p;
            unsigned n, a;
} min_heap_t;


static inline void           min_heap_ctor(min_heap_t* s);


void min_heap_ctor(min_heap_t* s) { s->p = 0; s->n = 0; s->a = 0; }



#endif
