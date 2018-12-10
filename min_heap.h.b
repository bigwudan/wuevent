#ifndef _MIN_HEAP_H_
#define _MIN_HEAP_H_



typedef struct min_heap
{
        struct event** p;
            unsigned n, a;
} min_heap_t;


static inline void           min_heap_ctor(min_heap_t* s);
static inline void           min_heap_dtor(min_heap_t* s);
static inline void           min_heap_elem_init(struct event* e);
static inline int            min_heap_elem_greater(struct event *a, struct event *b);
static inline int            min_heap_empty(min_heap_t* s);
static inline unsigned       min_heap_size(min_heap_t* s);
static inline struct event*  min_heap_top(min_heap_t* s);
static inline int            min_heap_reserve(min_heap_t* s, unsigned n);
static inline int            min_heap_push(min_heap_t* s, struct event* e);
static inline struct event*  min_heap_pop(min_heap_t* s);
static inline int            min_heap_erase(min_heap_t* s, struct event* e);
static inline void           min_heap_shift_up_(min_heap_t* s, unsigned hole_index, struct event* e);
static inline void           min_heap_shift_down_(min_heap_t* s, unsigned hole_index, struct event* e);


int min_heap_push(min_heap_t* s, struct event* e)
{
	if(min_heap_reserve(s, s->n + 1))
		return -1;
	min_heap_shift_up_(s, s->n++, e);
	return 0;
}


void min_heap_ctor(min_heap_t* s) { s->p = 0; s->n = 0; s->a = 0; }



void min_heap_elem_init(struct event* e) { e->min_heap_idx = -1; }
unsigned min_heap_size(min_heap_t* s) { return s->n; }

int min_heap_reserve(min_heap_t* s, unsigned n)
{
	if(s->a < n)
	{
		struct event** p;
		unsigned a = s->a ? s->a * 2 : 8;
		if(a < n)
			a = n;
		if(!(p = (struct event**)realloc(s->p, a * sizeof *p)))
			return -1;
		s->p = p;
		s->a = a;
	}
	return 0;
}

int min_heap_erase(min_heap_t* s, struct event* e)
{
	if(((unsigned int)-1) != e->min_heap_idx)
	{
		struct event *last = s->p[--s->n];
		unsigned parent = (e->min_heap_idx - 1) / 2;
		/* we replace e with the last element in the heap.  We might need to
		 *	   shift it upward if it is less than its parent, or downward if it is
		 *		   greater than one or both its children. Since the children are known
		 *			   to be less than the parent, it can't need to shift both up and
		 *				   down. */
		if (e->min_heap_idx > 0 && min_heap_elem_greater(s->p[parent], last))
			min_heap_shift_up_(s, e->min_heap_idx, last);
		else
			min_heap_shift_down_(s, e->min_heap_idx, last);
		e->min_heap_idx = -1;
		return 0;
	}
	return -1;
}



#endif