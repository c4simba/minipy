/* ========================= Mark-sweep garbage collector ========================= */

#include "gc.h"
#include "vm.h"
#include "bytecode.h"

#if defined(MPY_KOLIBRI)

/* free() is a no-op on KolibriOS, so collection would reclaim nothing. */
void gc_track(Obj *o){ (void)o; }
void gc_maybe_collect(void){}
void gc_collect(void){}
void gc_set_stack_base(void *p){ (void)p; }

#else

static Obj *gc_all = NULL;   /* singly-linked list of every live object */
static int  gc_count = 0;
static int  gc_threshold = 512;

void gc_track(Obj *o){ o->gc_next=gc_all; o->gc_mark=0; gc_all=o; gc_count++; }

static void *gc_stack_base = NULL;
void gc_set_stack_base(void *p){ gc_stack_base=p; }

static Obj *class_obj(Class *k){ return class_to_value(k).as.obj; }
static void gc_mark_value(Value v);
static void gc_mark_dict(Dict *d){ if(!d) return; for(int i=0;i<d->count;i++) gc_mark_value(d->vals[i]); }
static void gc_mark_list(List *l){ for(int i=0;i<l->count;i++) gc_mark_value(l->items[i]); }

static void gc_mark_obj(Obj *o){
    if(!o || o->gc_mark) return;
    o->gc_mark=1;
    switch(o->type){
        case O_STRING: break;
        case O_LIST:  gc_mark_list(&o->as.list); break;
        case O_TUPLE: gc_mark_list(&o->as.tuple); break;
        case O_SET:   gc_mark_list(&o->as.set); break;
        case O_DICT:  gc_mark_dict(&o->as.dict); break;
        case O_FUNCTION:{ Function *f=&o->as.fn; gc_mark_dict(f->globals); gc_mark_dict(f->closure);
            if(f->defaults) for(int i=0;i<f->default_count;i++) gc_mark_value(f->defaults[i]);
            if(f->chunk) for(int i=0;i<f->chunk->ccount;i++) gc_mark_value(f->chunk->consts[i]);
            if(f->defining_class) gc_mark_obj(class_obj(f->defining_class)); break; }
        case O_CLASS:{ Class *k=&o->as.klass; gc_mark_dict(k->methods); if(k->base) gc_mark_obj(class_obj(k->base)); break; }
        case O_INSTANCE: gc_mark_obj(class_obj(o->as.inst.klass)); gc_mark_dict(o->as.inst.fields); break;
        case O_BOUND_METHOD: gc_mark_value(o->as.bm.receiver); if(o->as.bm.fn) gc_mark_obj(o->as.bm.fn->owner); break;
        case O_BOUND_NATIVE: gc_mark_value(o->as.bn.receiver); break;
        case O_MODULE: gc_mark_dict(o->as.mod.dict); break;
        case O_ITER: gc_mark_value(o->as.iter.iterable); break;
        case O_GENERATOR: if(o->as.gen.fn) gc_mark_obj(o->as.gen.fn->owner); gc_mark_dict(o->as.gen.locals); break;
        case O_EXCEPTION: gc_mark_value(o->as.exc.payload); break;
        case O_SUPER: gc_mark_value(o->as.super.self); if(o->as.super.start) gc_mark_obj(class_obj(o->as.super.start)); break;
        case O_METHWRAP: gc_mark_value(o->as.mw.fn); break;
        case O_BUFFER: break;
    }
}
static void gc_mark_value(Value v){ if(v.type==V_OBJ && v.as.obj) gc_mark_obj(v.as.obj); }

/* Free only heap owned exclusively by this object. Chunks, params and shared
   namespace dicts (globals) are compile-time/shared and are never freed here. */
static void gc_free_obj(Obj *o){
    switch(o->type){
        case O_STRING: free(o->as.str.s); break;
        case O_LIST:  free(o->as.list.items); break;
        case O_TUPLE: free(o->as.tuple.items); break;
        case O_SET:   free(o->as.set.items); break;
        case O_DICT:  free(o->as.dict.keys); free(o->as.dict.vals); break;
        case O_INSTANCE: if(o->as.inst.fields){ free(o->as.inst.fields->keys); free(o->as.inst.fields->vals); free(o->as.inst.fields); } break;
        case O_GENERATOR: if(o->as.gen.locals){ free(o->as.gen.locals->keys); free(o->as.gen.locals->vals); free(o->as.gen.locals); } break;
        case O_EXCEPTION: free(o->as.exc.type_name); free(o->as.exc.message); break;
        case O_BOUND_NATIVE: free(o->as.bn.name); break;
        case O_MODULE: free(o->as.mod.name); break;
        case O_BUFFER: free(o->as.buf.data); break;
        case O_CLASS: free(o->as.klass.name); break;   /* methods dict is shared/permanent */
        default: break;                                 /* function/bound/iter/super/methwrap: struct only */
    }
    free(o);
}

/* Sorted array of all live object addresses, for conservative membership. */
static Obj **gc_sorted=NULL; static int gc_sorted_n=0;
static int gc_ptrcmp(const void *a,const void *b){ const char *x=*(char *const*)a, *y=*(char *const*)b; return x<y?-1:(x>y?1:0); }
static int gc_is_tracked(void *w){
    int lo=0,hi=gc_sorted_n-1;
    while(lo<=hi){ int m=(lo+hi)/2; void *pm=(void*)gc_sorted[m]; if(pm==w) return 1; if(pm<w) lo=m+1; else hi=m-1; }
    return 0;
}
/* Reading across stack-frame boundaries is intentional here; exempt it from
   ASan's stack-redzone checks (real use-after-frees elsewhere still trip). */
#if defined(__has_feature)
# if __has_feature(address_sanitizer)
#  define GC_NO_ASAN __attribute__((no_sanitize("address")))
# endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(GC_NO_ASAN)
# define GC_NO_ASAN __attribute__((no_sanitize_address))
#endif
#ifndef GC_NO_ASAN
# define GC_NO_ASAN
#endif
/* Conservatively mark any machine word in [lo,hi) that points at a live object.
   This catches Values held in C locals across a nested VM call (e.g. dunder
   dispatch, or a builtin calling back into Python) which are not on vm.stack. */
GC_NO_ASAN static void gc_scan_range(void **lo, void **hi){ for(void **p=lo;p<hi;p++){ void *w=*p; if(w && gc_is_tracked(w)) gc_mark_obj((Obj*)w); } }

void gc_collect(void){
    gc_sorted=(Obj**)xrealloc(gc_sorted,sizeof(Obj*)*(size_t)(gc_count>0?gc_count:1)); gc_sorted_n=0;
    for(Obj *o=gc_all;o;o=o->gc_next){ o->gc_mark=0; gc_sorted[gc_sorted_n++]=o; }
    qsort(gc_sorted,(size_t)gc_sorted_n,sizeof(Obj*),gc_ptrcmp);
    /* precise roots: VM stack, frames, namespaces, pending exception */
    for(int i=0;i<vm.sp;i++) gc_mark_value(vm.stack[i]);
    for(int i=0;i<vm.fcount;i++){ Frame *fr=&vm.frames[i]; if(fr->fn) gc_mark_obj(fr->fn->owner); gc_mark_dict(fr->locals); }
    gc_mark_dict(vm.builtins);
    gc_mark_dict(vm.modules);
    gc_mark_value(vm.pending_exception);
    /* conservative roots: callee-saved registers + the C stack */
    jmp_buf regs; memset(&regs,0,sizeof(regs)); (void)setjmp(regs);
    gc_scan_range((void**)&regs,(void**)((char*)&regs+sizeof(regs)));
    void *sp_local; void *sp=(void*)&sp_local;
    if(gc_stack_base && (void**)sp<(void**)gc_stack_base) gc_scan_range((void**)sp,(void**)gc_stack_base);
    /* sweep */
    Obj **link=&gc_all;
    while(*link){ Obj *o=*link; if(o->gc_mark){ link=&o->gc_next; } else { *link=o->gc_next; gc_free_obj(o); gc_count--; } }
    gc_threshold = gc_count*2 + 512;
}

void gc_maybe_collect(void){
    static int stress=-1;
    if(stress<0) stress = getenv("MPY_GC_STRESS") ? 1 : 0;   /* collect on every safe point */
    if(stress || gc_count >= gc_threshold) gc_collect();
}

#endif /* MPY_KOLIBRI */
