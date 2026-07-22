#ifndef MPY_GC_H
#define MPY_GC_H

#include "value.h"

/* Mark-sweep garbage collector over the Obj heap.
   Collection runs only at a VM safe point (top of the dispatch loop), where
   every live value is reachable from a root (VM stack, frames, globals,
   builtins, modules, pending exception) - never from a C local. That makes
   precise rooting unnecessary.

   On KolibriOS free() is a no-op (page allocator), so GC is compiled out
   there; the host build reclaims memory. */

void gc_track(Obj *o);         /* register a freshly allocated object */
void gc_maybe_collect(void);   /* collect if the heap grew past the threshold */
void gc_collect(void);         /* force a collection */
void gc_set_stack_base(void *p); /* record the C stack base (call once in main) */

#endif /* MPY_GC_H */
