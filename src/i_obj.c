/* ========================= Interpreter: objects and memory =========================
   Allocation and the collector (mark-sweep: precise from the roots - modules,
   builtins, every thread's frames and exception state - plus a conservative
   scan of each thread's C stack, which keeps what natives hold in locals);
   str / bytes, tuple, list, dict (insertion-ordered: an entry array and an
   index table), set (CPython's own table, so sets iterate in its order),
   range, slice, cell; the hashes CPython computes for int, float, tuple and
   frozenset. */

#include "interp.h"
#include <math.h>

Type *T_object, *T_type, *T_none, *T_bool, *T_int, *T_float, *T_complex, *T_str, *T_bytes, *T_tuple, *T_list,
    *T_dict, *T_set, *T_frozenset, *T_range, *T_slice, *T_function, *T_native, *T_method, *T_module, *T_code,
    *T_cell, *T_generator, *T_coroutine, *T_asyncgen, *T_property, *T_staticmethod, *T_classmethod, *T_super,
    *T_iter, *T_file, *T_buffer, *T_notimpl, *T_ellipsis, *T_dict_keys, *T_dict_values, *T_dict_items, *T_box;
DictObj *mp_builtins, *mp_modules;
Value mp_NotImplemented, mp_Ellipsis;

/* ---------------------------------------------------------------- threads */
Thread *mp_ts;
mpy_lock mp_gil;
Thread *mp_threads[MP_MAX_THREADS];
int mp_nthreads;
void mp_thread_add(Thread *t){ if(mp_nthreads<MP_MAX_THREADS) mp_threads[mp_nthreads++]=t; }
void mp_thread_remove(Thread *t){
    for(int i=0;i<mp_nthreads;i++) if(mp_threads[i]==t){ mp_threads[i]=mp_threads[--mp_nthreads]; break; }
}
static jmp_buf regs_of[MP_MAX_THREADS];
void mp_gil_release(void){
    Thread *t=mp_ts;
    int slot=0; for(int i=0;i<mp_nthreads;i++) if(mp_threads[i]==t) slot=i;
    (void)setjmp(regs_of[slot]);                          /* its registers, where the collector sees them */
    volatile char here; t->cstack_top=(void*)&here;
    mpy_lock_release(&mp_gil);
}
void mp_gil_acquire(Thread *self){
    mpy_lock_acquire(&mp_gil);
    mp_ts=self;
    self->cstack_top=NULL;
}

/* ---------------------------------------------------------------- memory */
static Obj *gc_head;
static int64_t gc_count, gc_threshold=20000;
static Value *extra_roots[256]; static int nextra;
void mp_gc_add_root(Value *v){ if(nextra<256) extra_roots[nextra++]=v; }

void *mp_alloc(Type *t, size_t size){
    Obj *o=(Obj*)xmalloc(size);
    memset(o,0,size);
    o->type=t; o->size=(uint32_t)size;
    o->gcnext=gc_head; gc_head=o; gc_count++;
    return o;
}

/* objects of subclasses of the built-in value types: the built-in's layout, then an instance dict pointer */
static void *alloc_sub(Type *t, size_t size){
    size=(size+7)&~(size_t)7;
    return mp_alloc(t,size+sizeof(DictObj*));
}
DictObj **mp_sub_slot(Obj *o){ return (DictObj**)((char*)o+o->size-sizeof(DictObj*)); }

#if defined(MPY_KOLIBRI)
int mp_gc_enabled=1;
void mp_gc_maybe(void){}
void mp_gc_collect(void){}
void mp_gc_set_stack_base(void *p){ (void)p; }
int64_t mp_gc_objects(void){ return 0; }
#else
void mp_gc_set_stack_base(void *p){ mp_ts->cstack_base=p; }

static Obj **mstack; static int64_t msp, mcap;
static Value *final; static int nfinal, cfinal, finalizing;      /* unreachable generators to close */
static void mark_obj(Obj *o){
    if(!o || o->mark) return;
    o->mark=1;
    if(msp==mcap){ mcap=mcap?mcap*2:4096; mstack=(Obj**)xrealloc(mstack,sizeof(Obj*)*(size_t)mcap); }
    mstack[msp++]=o;
}
static inline void mark_v(Value v){ if(v.k==V_OBJ) mark_obj(v.u.o); }
static void mark_frame(Frame *f){
    if(f){
        mark_obj((Obj*)f->code); mark_obj((Obj*)f->func); mark_obj((Obj*)f->globals); mark_obj((Obj*)f->builtins); mark_obj((Obj*)f->locals);
        if(f->code){
            for(int i=0;i<f->code->nlocals;i++) mark_v(f->fast[i]);
            for(int i=0;i<f->code->ncells+f->code->nfrees;i++) mark_v(f->cells[i]);
        }
        for(int i=0;i<f->sp;i++) mark_v(f->stack[i]);
        for(int i=0;i<f->nsaved;i++) mark_v(f->saved_exc[i]);
        mark_v(f->yf);
        mark_obj(f->gen);
    }
}
static void mark_children(Obj *o){
    mark_obj((Obj*)o->type);
    if(o->type->flags&TF_SUBVAL) mark_obj((Obj*)*mp_sub_slot(o));
    switch(o->type->layout){
        case LY_TYPE:{ Type *t=(Type*)o; mark_obj((Obj*)t->name); mark_obj((Obj*)t->qualname); mark_obj((Obj*)t->base); mark_obj((Obj*)t->bases);
            for(int i=0;i<t->nmro;i++) mark_obj((Obj*)t->mro[i]);
            mark_obj((Obj*)t->dict); mark_v(t->subs); break; }
        case LY_TUPLE:{ TupleObj *t=(TupleObj*)o; for(int64_t i=0;i<t->len;i++) mark_v(t->items[i]); break; }
        case LY_LIST:{ ListObj *l=(ListObj*)o; for(int64_t i=0;i<l->len;i++) mark_v(l->items[i]); break; }
        case LY_DICT:{ DictObj *d=(DictObj*)o; for(int64_t i=0;i<d->nent;i++){ mark_v(d->ent[i].key); mark_v(d->ent[i].val); } break; }
        case LY_SET:{ SetObj *s=(SetObj*)o; if(s->table) for(int64_t i=0;i<=s->mask;i++) mark_v(s->table[i].key); break; }
        case LY_SLICE:{ SliceObj *s=(SliceObj*)o; mark_v(s->start); mark_v(s->stop); mark_v(s->step); break; }
        case LY_CELL: mark_v(((CellObj*)o)->v); break;
        case LY_BOX: case LY_STATICMETHOD: case LY_CLASSMETHOD: mark_v(((BoxObj*)o)->v); break;
        case LY_PROPERTY:{ PropertyObj *p=(PropertyObj*)o; mark_v(p->get); mark_v(p->set); mark_v(p->del); mark_v(p->doc); break; }
        case LY_FUNC:{ FuncObj *f=(FuncObj*)o; mark_obj((Obj*)f->code); mark_obj((Obj*)f->globals); mark_obj((Obj*)f->defaults); mark_obj((Obj*)f->kwdefaults);
            mark_obj((Obj*)f->closure); mark_obj((Obj*)f->name); mark_obj((Obj*)f->qualname); mark_v(f->doc); mark_v(f->module); mark_obj((Obj*)f->dict); break; }
        case LY_NATIVE: mark_v(((NativeObj*)o)->self); break;
        case LY_METHOD: mark_v(((MethodObj*)o)->func); mark_v(((MethodObj*)o)->self); break;
        case LY_MODULE: mark_obj((Obj*)((ModuleObj*)o)->dict); mark_obj((Obj*)((ModuleObj*)o)->name); break;
        case LY_CODE:{ CodeObj *c=(CodeObj*)o; mark_obj((Obj*)c->name); mark_obj((Obj*)c->qualname);
            for(int i=0;i<c->nconsts;i++) mark_v(c->consts[i]);
            for(int i=0;i<c->nnames;i++) mark_obj((Obj*)c->names[i]);
            for(int i=0;i<c->nlocals;i++) mark_obj((Obj*)c->varnames[i]);
            for(int i=0;i<c->ncells;i++) mark_obj((Obj*)c->cellnames[i]);
            for(int i=0;i<c->nfrees;i++) mark_obj((Obj*)c->freenames[i]);
            mark_v(c->doc);
            break; }
        case LY_GEN:{ GenObj *g=(GenObj*)o; if(g->f) mark_frame(g->f); mark_obj((Obj*)g->name); mark_obj((Obj*)g->qualname); mark_v(g->awaiting); break; }
        case LY_SUPER:{ SuperObj *s=(SuperObj*)o; mark_obj((Obj*)s->start); mark_v(s->obj); mark_obj((Obj*)s->objtype); break; }
        case LY_ITER:{ IterObj *it=(IterObj*)o; mark_v(it->src); mark_v(it->aux); mark_v(it->aux2); break; }
        case LY_FILE: mark_v(((FileObj*)o)->name); break;
        case LY_INSTANCE: mark_obj((Obj*)((InstObj*)o)->dict); break;
        case LY_NUM: break;
        case LY_EXC:{ ExcObj *e=(ExcObj*)o; mark_obj((Obj*)e->dict); mark_v(e->args); mark_v(e->cause); mark_v(e->context); mark_v(e->tb); mark_v(e->notes); mark_v(e->hint); break; }
        default: break;
    }
}
static void free_obj(Obj *o){
    switch(o->type->layout){
        case LY_TYPE: free(((Type*)o)->mro); break;
        case LY_LIST: free(((ListObj*)o)->items); break;
        case LY_DICT: free(((DictObj*)o)->ent); free(((DictObj*)o)->idx); break;
        case LY_SET: free(((SetObj*)o)->table); break;
        case LY_CODE:{ CodeObj *c=(CodeObj*)o; free(c->code); free(c->lines); free(c->pos); free(c->anc); free(c->consts); free(c->names); free(c->varnames); free(c->cellnames); free(c->freenames); free(c->cellarg);
            if(c->annots){ for(int i=0;i<c->argc+c->kwonly+2;i++) free(c->annots[i]); free(c->annots); }
            for(int i=0;i<c->nann;i++){ free(c->annname[i]); free(c->anntext[i]); }
            free(c->annname); free(c->anntext); break; }
        case LY_GEN:{ GenObj *g=(GenObj*)o; if(g->f) free(g->f); break; }
        case LY_BUFFER: free(((BufferObj*)o)->data); break;
        case LY_BYTEARRAY: free(((ByteArrayObj*)o)->data); break;
        case LY_FILE: free(((FileObj*)o)->buf); break;
        default: break;
    }
    free(o);
}
/* the objects, sorted: is a word a pointer into one? */
static Obj **sorted; static int64_t nsorted;
static int ptrcmp(const void *a, const void *b){ uintptr_t x=(uintptr_t)*(Obj*const*)a, y=(uintptr_t)*(Obj*const*)b; return x<y?-1:x>y?1:0; }
static Obj *find_obj(uintptr_t w){
    int64_t lo=0, hi=nsorted-1, best=-1;
    while(lo<=hi){ int64_t m=(lo+hi)/2; if((uintptr_t)sorted[m]<=w){ best=m; lo=m+1; } else hi=m-1; }
    if(best<0) return NULL;
    Obj *o=sorted[best];
    return w<(uintptr_t)o+o->size ? o : NULL;
}
#if defined(__has_feature)
# if __has_feature(address_sanitizer)
#  define NO_ASAN __attribute__((no_sanitize("address")))
# endif
#endif
#ifndef NO_ASAN
# define NO_ASAN
#endif
NO_ASAN static void scan_words(void *lo, void *hi){
    uintptr_t a=(uintptr_t)lo & ~(uintptr_t)(sizeof(void*)-1);
    for(void **p=(void**)a;(void*)p<hi;p++){ Obj *o=find_obj((uintptr_t)*p); if(o) mark_obj(o); }
}
void mp_gc_collect(void){
    sorted=(Obj**)xrealloc(sorted,sizeof(Obj*)*(size_t)(gc_count>0?gc_count:1)); nsorted=0;
    for(Obj *o=gc_head;o;o=o->gcnext){ o->mark=0; sorted[nsorted++]=o; }
    qsort(sorted,(size_t)nsorted,sizeof(Obj*),ptrcmp);
    msp=0;
    mark_obj((Obj*)mp_builtins); mark_obj((Obj*)mp_modules);
    for(Obj *o=gc_head;o;o=o->gcnext)                        /* the interpreter's own types stay, instances or not */
        if(o->type==T_type && (((Type*)o)->flags&TF_BUILTIN)) mark_obj(o);
    mark_v(mp_NotImplemented); mark_v(mp_Ellipsis);
    for(int i=0;i<nextra;i++) mark_v(*extra_roots[i]);
    for(int t=0;t<mp_nthreads;t++){ Thread *th=mp_threads[t];
        for(Frame *f=th->frame;f;f=f->back) mark_frame(f);
        mark_v(th->exc);
        for(int i=0;i<th->nhandled;i++) mark_v(th->handled[i]);
        for(int i=0;i<8;i++) mark_v(th->roots[i]);
        if(th==mp_ts){
            jmp_buf regs; memset(&regs,0,sizeof regs); (void)setjmp(regs);
            scan_words(&regs,(char*)&regs+sizeof regs);
            volatile char here;
            if(th->cstack_base && (void*)&here<th->cstack_base) scan_words((void*)&here,th->cstack_base);
        } else {
            scan_words(&regs_of[t],(char*)&regs_of[t]+sizeof(jmp_buf));
            if(th->cstack_base && th->cstack_top && th->cstack_top<th->cstack_base) scan_words(th->cstack_top,th->cstack_base);
        }
    }
    /* generator frames are reached through their objects; frames on a thread's chain above */
    while(msp>0) mark_children(mstack[--msp]);
    for(int i=0;i<nfinal;i++) mark_v(final[i]);                    /* (waiting to be closed) */
    for(Obj *o=gc_head;o;o=o->gcnext)                              /* suspended generators no one can reach: closed (their finally blocks run), then freed */
        if(!o->mark && o->type->layout==LY_GEN && mp_gen_needs_close(v_obj(o))){
            if(nfinal==cfinal){ cfinal=cfinal?cfinal*2:16; final=(Value*)xrealloc(final,sizeof(Value)*(size_t)cfinal); }
            final[nfinal++]=v_obj(o); mark_obj(o); }
    while(msp>0) mark_children(mstack[--msp]);
    Obj **link=&gc_head;
    while(*link){ Obj *o=*link; if(o->mark) link=&o->gcnext; else { *link=o->gcnext; free_obj(o); gc_count--; } }
    gc_threshold=gc_count*2+20000;
    if(!finalizing){ finalizing=1;
        while(nfinal>0){ Value g=final[--nfinal]; mp_gen_close_quietly(g); }
        finalizing=0; }
}
int mp_gc_enabled=1;                                      /* gc.disable(): no collections while allocating */
void mp_gc_maybe(void){
    static int stress=-1;
    if(stress<0) stress=getenv("MPY_GC_STRESS")?1:0;
    if(!mp_gc_enabled) return;
    if(stress || gc_count>=gc_threshold) mp_gc_collect();
}
int64_t mp_gc_objects(void){ return gc_count; }
#endif

/* ---------------------------------------------------------------- text buffers */
void sb_put(SBuf *b, const char *s, int64_t n){
    if(b->n+n+1>b->cap){ b->cap=(b->n+n+1)*2+16; b->s=(char*)xrealloc(b->s,(size_t)b->cap); }
    memcpy(b->s+b->n,s,(size_t)n); b->n+=n; b->s[b->n]=0;
}
void sb_puts(SBuf *b, const char *s){ sb_put(b,s,(int64_t)strlen(s)); }
void sb_putc(SBuf *b, char c){ sb_put(b,&c,1); }
void sb_printf(SBuf *b, const char *fmt, ...){
    char tmp[512]; va_list ap; va_start(ap,fmt); int n=vsnprintf(tmp,sizeof tmp,fmt,ap); va_end(ap);
    if(n<(int)sizeof tmp){ sb_put(b,tmp,n); return; }
    char *big=(char*)xmalloc((size_t)n+1); va_start(ap,fmt); vsnprintf(big,(size_t)n+1,fmt,ap); va_end(ap); sb_put(b,big,n); free(big);
}
Value sb_value(SBuf *b){ Value v=mp_strn(b->s?b->s:"",b->n); free(b->s); b->s=NULL; b->n=b->cap=0; return v; }

/* ---------------------------------------------------------------- strings */
int64_t mp_utf8_decode(const char *s, int64_t n, int64_t *pos){
    const unsigned char *u=(const unsigned char*)s; int64_t i=*pos; uint32_t c=u[i];
    int len= c<0x80?1 : (c>>5)==6?2 : (c>>4)==14?3 : (c>>3)==30?4 : 1;
    if(len==1 || i+len>n){ *pos=i+1; return c; }
    uint32_t v= len==2?(c&31u) : len==3?(c&15u) : (c&7u);
    for(int k=1;k<len;k++){ if((u[i+k]&0xC0)!=0x80){ *pos=i+1; return c; } v=(v<<6)|(u[i+k]&63u); }
    *pos=i+len; return v;
}
int mp_utf8_encode(char *o, uint32_t cp){
    if(cp<0x80){ o[0]=(char)cp; return 1; }
    if(cp<0x800){ o[0]=(char)(0xC0|(cp>>6)); o[1]=(char)(0x80|(cp&0x3F)); return 2; }
    if(cp<0x10000){ o[0]=(char)(0xE0|(cp>>12)); o[1]=(char)(0x80|((cp>>6)&0x3F)); o[2]=(char)(0x80|(cp&0x3F)); return 3; }
    o[0]=(char)(0xF0|(cp>>18)); o[1]=(char)(0x80|((cp>>12)&0x3F)); o[2]=(char)(0x80|((cp>>6)&0x3F)); o[3]=(char)(0x80|(cp&0x3F)); return 4;
}
Value mp_strn(const char *s, int64_t n){
    StrObj *o=(StrObj*)mp_alloc(T_str,sizeof(StrObj)+(size_t)n+1);
    memcpy(o->s,s,(size_t)n); o->s[n]=0; o->len=n;
    int ascii=1; for(int64_t i=0;i<n;i++) if((unsigned char)s[i]>=0x80){ ascii=0; break; }
    o->ascii=ascii;
    if(ascii) o->cplen=n;
    else { int64_t c=0, p=0; while(p<n){ mp_utf8_decode(s,n,&p); c++; } o->cplen=c; }
    return v_obj(o);
}
Value mp_str(const char *s){ return mp_strn(s,(int64_t)strlen(s)); }
Value mp_strf(const char *fmt, ...){
    char tmp[1024]; va_list ap; va_start(ap,fmt); int n=vsnprintf(tmp,sizeof tmp,fmt,ap); va_end(ap);
    if(n<(int)sizeof tmp) return mp_strn(tmp,n);
    char *big=(char*)xmalloc((size_t)n+1); va_start(ap,fmt); vsnprintf(big,(size_t)n+1,fmt,ap); va_end(ap);
    Value v=mp_strn(big,n); free(big); return v;
}
const char *mp_cstr(Value s){ return AS_STR(s)->s; }
int mp_str_eq_c(Value s, const char *c){ return IS_STR(s) && !strcmp(AS_STR(s)->s,c); }
int64_t mp_str_byteoff(StrObj *s, int64_t cpi){
    if(s->ascii) return cpi;
    int64_t p=0; for(int64_t i=0;i<cpi && p<s->len;i++) mp_utf8_decode(s->s,s->len,&p);
    return p;
}
static DictObj *interned; static Value interned_v;
Value mp_intern(const char *s){
    if(!interned){ interned_v=mp_dict(); interned=AS_DICT(interned_v); mp_gc_add_root(&interned_v); }
    Value k=mp_str(s), out;
    if(mp_dict_get(interned,k,&out)) return out;
    mp_dict_set(interned,k,k);
    return k;
}
Value mp_bytes(const void *s, int64_t n){
    BytesObj *o=(BytesObj*)mp_alloc(T_bytes,sizeof(BytesObj)+(size_t)n+1);
    if(n) memcpy(o->s,s,(size_t)n);
    o->s[n]=0; o->len=n;
    return v_obj(o);
}

void mp_set_merge(SetObj *s, SetObj *o);
/* plain (a value of t's built-in base) as an object of t: a copy with t's layout and an instance dict slot */
Value mp_sub_value(Type *t, Value plain){
    switch(t->layout){
        case LY_STR:{ StrObj *x=AS_STR(plain); StrObj *o=(StrObj*)alloc_sub(t,sizeof(StrObj)+(size_t)x->len+1);
            memcpy(o->s,x->s,(size_t)x->len+1); o->len=x->len; o->cplen=x->cplen; o->ascii=x->ascii; return v_obj(o); }
        case LY_BYTES:{ BytesObj *x=AS_BYTES(plain); BytesObj *o=(BytesObj*)alloc_sub(t,sizeof(BytesObj)+(size_t)x->len+1);
            memcpy(o->s,x->s,(size_t)x->len+1); o->len=x->len; return v_obj(o); }
        case LY_TUPLE:{ TupleObj *x=AS_TUPLE(plain); TupleObj *o=(TupleObj*)alloc_sub(t,sizeof(TupleObj)+sizeof(Value)*(size_t)x->len);
            o->len=x->len; memcpy(o->items,x->items,sizeof(Value)*(size_t)x->len); return v_obj(o); }
        case LY_LIST:{ ListObj *x=AS_LIST(plain); ListObj *o=(ListObj*)alloc_sub(t,sizeof(ListObj));
            o->cap=x->len>4?x->len:4; o->items=(Value*)xmalloc(sizeof(Value)*(size_t)o->cap); o->len=x->len;
            memcpy(o->items,x->items,sizeof(Value)*(size_t)x->len); return v_obj(o); }
        case LY_DICT:{ DictObj *o=(DictObj*)alloc_sub(t,sizeof(DictObj)); Value r=v_obj(o);
            int64_t pos=0; Value k, val; while(mp_dict_next(AS_DICT(plain),&pos,&k,&val)) mp_dict_set(o,k,val); return r; }
        case LY_SET:{ SetObj *o=(SetObj*)alloc_sub(t,sizeof(SetObj)); Value r=v_obj(o);
            o->mask=7; o->table=(SEnt*)xmalloc(sizeof(SEnt)*8); memset(o->table,0,sizeof(SEnt)*8);
            mp_set_merge(o,(SetObj*)plain.u.o); return r; }
        case LY_NUM:{ NumObj *o=(NumObj*)alloc_sub(t,sizeof(NumObj)); o->v=plain; return v_obj(o); }
        case LY_STATICMETHOD: case LY_CLASSMETHOD:{ BoxObj *o=(BoxObj*)alloc_sub(t,sizeof(BoxObj)); o->v=((BoxObj*)plain.u.o)->v; return v_obj(o); }
        case LY_PROPERTY:{ PropertyObj *x=(PropertyObj*)plain.u.o, *o=(PropertyObj*)alloc_sub(t,sizeof(PropertyObj));
            o->get=x->get; o->set=x->set; o->del=x->del; o->doc=x->doc; return v_obj(o); }
        case LY_BYTEARRAY:{ ByteArrayObj *x=(ByteArrayObj*)plain.u.o, *o=(ByteArrayObj*)alloc_sub(t,sizeof(ByteArrayObj));
            o->len=x->len; o->cap=x->len; o->data=(unsigned char*)xmalloc((size_t)x->len+1); if(x->len) memcpy(o->data,x->data,(size_t)x->len); return v_obj(o); }
        default: break;
    }
    mp_raise_t(E_TypeError,"cannot make a '%s' object",t->name->s);
}

/* ---------------------------------------------------------------- tuple, list */
Value mp_tuple(int64_t n, const Value *items){
    TupleObj *t=(TupleObj*)mp_alloc(T_tuple,sizeof(TupleObj)+sizeof(Value)*(size_t)n);
    t->len=n;
    if(items) memcpy(t->items,items,sizeof(Value)*(size_t)n);
    return v_obj(t);
}
Value mp_list(int64_t n, const Value *items){
    ListObj *l=(ListObj*)mp_alloc(T_list,sizeof(ListObj));
    l->cap=n>4?n:4; l->items=(Value*)xmalloc(sizeof(Value)*(size_t)l->cap); l->len=n;
    if(items) memcpy(l->items,items,sizeof(Value)*(size_t)n);
    else memset(l->items,0,sizeof(Value)*(size_t)l->cap);
    return v_obj(l);
}
void mp_list_append(Value lv, Value v){
    ListObj *l=AS_LIST(lv);
    if(l->len==l->cap){ l->cap=l->cap*2+4; l->items=(Value*)xrealloc(l->items,sizeof(Value)*(size_t)l->cap); }
    l->items[l->len++]=v;
}

/* ---------------------------------------------------------------- hashing (as CPython) */
#define HASH_BITS 61
#define HASH_MOD (((uint64_t)1<<HASH_BITS)-1)
static uint64_t hash_int(int64_t x){
    uint64_t ux= x<0 ? (uint64_t)(-(x+1))+1 : (uint64_t)x;
    uint64_t h=ux%HASH_MOD;
    int64_t r= x<0 ? -(int64_t)h : (int64_t)h;
    if(r==-1) r=-2;
    return (uint64_t)r;
}
static uint64_t hash_double(double v){
    if(!isfinite(v)){ if(isinf(v)) return v>0 ? 314159 : (uint64_t)-314159; return 0; }
    int e; double m=frexp(v,&e);
    int sign=1; if(m<0){ sign=-1; m=-m; }
    uint64_t x=0;
    while(m){
        x=((x<<28)&HASH_MOD)|x>>(HASH_BITS-28);
        m*=268435456.0; e-=28;
        uint64_t y=(uint64_t)m; m-=(double)y; x+=y;
        if(x>=HASH_MOD) x-=HASH_MOD;
    }
    e= e>=0 ? e%HASH_BITS : HASH_BITS-1-((-1-e)%HASH_BITS);
    x=((x<<e)&HASH_MOD)|x>>(HASH_BITS-e);
    x=x*(uint64_t)(int64_t)sign;
    if(x==(uint64_t)-1) x=(uint64_t)-2;
    return x;
}
static uint64_t hash_bytes(const unsigned char *s, int64_t n){
    uint64_t h=14695981039346656037ULL;      /* FNV-1a, as compiled programs hash */
    for(int64_t i=0;i<n;i++){ h^=s[i]; h*=1099511628211ULL; }
    if(h==(uint64_t)-1) h=(uint64_t)-2;
    return h;
}
#define XXPRIME_1 ((uint64_t)11400714785074694791ULL)
#define XXPRIME_2 ((uint64_t)14029467366897019727ULL)
#define XXPRIME_5 ((uint64_t)2870177450012600261ULL)
#define XXROTATE(x) (((x)<<31)|((x)>>33))
static uint64_t shuffle_bits(uint64_t h){ return ((h ^ 89869747UL) ^ (h << 16)) * 3644798167UL; }
uint64_t mp_hash(Value v){
    switch(v.k){
        case V_NONE: return 0xFCA86420ULL;
        case V_BOOL: case V_INT: return hash_int(v.u.i);
        case V_FLOAT: return hash_double(v.u.f);
        default: break;
    }
    Obj *o=v.u.o; Type *t=o->type;
    if(t->flags&TF_SUBVAL){                                  /* a subclass of a built-in: its __hash__ first */
        Value h=mp_type_lookup_s(t,"__hash__");
        if(h.k==V_NONE) mp_raise_t(E_TypeError,"unhashable type: '%s'",t->name->s);
        if(h.k!=V_UNDEF && !IS(h,T_native)){
            Value r=mp_call1(h,v); r=mp_unbox(r);
            if(!IS_INTLIKE(r)) mp_raise_t(E_TypeError,"__hash__ method should return an integer");
            return r.u.i==-1 ? (uint64_t)-2 : (uint64_t)r.u.i; }
        if(t->layout==LY_NUM) return mp_hash(((NumObj*)o)->v);
    }
    switch(t->layout){
        case LY_STR:{ StrObj *s=(StrObj*)o; if(!s->hashed){ s->hash=hash_bytes((unsigned char*)s->s,s->len); s->hashed=1; } return s->hash; }
        case LY_BYTES:{ BytesObj *s=(BytesObj*)o; if(!s->hashed){ s->hash=hash_bytes(s->s,s->len); s->hashed=1; } return s->hash; }
        case LY_TUPLE:{ TupleObj *tp=(TupleObj*)o;
            if(tp->hashed) return tp->hash;
            uint64_t acc=XXPRIME_5;
            for(int64_t i=0;i<tp->len;i++){ uint64_t lane=mp_hash(tp->items[i]); acc+=lane*XXPRIME_2; acc=XXROTATE(acc); acc*=XXPRIME_1; }
            acc+=(uint64_t)tp->len ^ (XXPRIME_5 ^ 3527539UL);
            if(acc==(uint64_t)-1) acc=1546275796;
            tp->hash=acc; tp->hashed=1; return acc; }
        case LY_SET:{
            if(t!=T_frozenset && !mp_is_subtype(t,T_frozenset)) mp_raise_t(E_TypeError,"unhashable type: '%s'",t->name->s);
            SetObj *s=(SetObj*)o; if(s->hashed) return s->hash;
            uint64_t h=0;
            for(int64_t i=0;i<=s->mask;i++) if(s->table[i].key.k!=V_UNDEF) h^=shuffle_bits(s->table[i].hash);
            if((s->fill-s->used)&1) h^=shuffle_bits((uint64_t)-1);
            h^=((uint64_t)s->used+1)*1927868237UL;
            h^=(h>>11)^(h>>25);
            h=h*69069U+907133923UL;
            if(h==(uint64_t)-1) h=590923713UL;
            s->hash=h; s->hashed=1; return h; }
        case LY_COMPLEX:{ ComplexObj *c=(ComplexObj*)o; uint64_t h=hash_double(c->re)+1000003ULL*hash_double(c->im); if(h==(uint64_t)-1) h=(uint64_t)-2; return h; }
        case LY_LIST: case LY_DICT: case LY_BYTEARRAY: mp_raise_t(E_TypeError,"unhashable type: '%s'",t->name->s);
        default: break;
    }
    if(t->flags&TF_DUNDERS){
        Value h=mp_type_lookup_s(t,"__hash__");
        if(h.k==V_NONE) mp_raise_t(E_TypeError,"unhashable type: '%s'",t->name->s);
        if(h.k!=V_UNDEF && !IS(h,T_native)){
            Value r=mp_unbox(mp_call1(h,v));
            if(!IS_INTLIKE(r)) mp_raise_t(E_TypeError,"__hash__ method should return an integer");
            return r.u.i==-1 ? (uint64_t)-2 : (uint64_t)r.u.i;
        }
    }
    uint64_t p=(uint64_t)(uintptr_t)o; p=(p>>4)|(p<<60);
    if(p==(uint64_t)-1) p=(uint64_t)-2;
    return p;
}

/* key equality for dicts and sets: identity, then == */
static int key_eq(Value a, uint64_t ha, Value b, uint64_t hb){
    if(ha!=hb) return 0;
    if(a.k==V_OBJ && b.k==V_OBJ && a.u.o==b.u.o) return 1;
    if(a.k==V_OBJ && b.k==V_OBJ && a.u.o->type->layout==LY_STR && b.u.o->type->layout==LY_STR && !(a.u.o->type->flags&TF_SUBVAL) && !(b.u.o->type->flags&TF_SUBVAL)){
        StrObj *x=AS_STR(a), *y=AS_STR(b); return x->len==y->len && !memcmp(x->s,y->s,(size_t)x->len); }
    return mp_eq(a,b);
}

/* ---------------------------------------------------------------- dict */
Value mp_dict(void){ return v_obj(mp_alloc(T_dict,sizeof(DictObj))); }
static void dict_rebuild(DictObj *d, int64_t need){
    /* compact the entries, then a fresh index of a size that keeps it at most 2/3 full */
    int64_t j=0;
    for(int64_t i=0;i<d->nent;i++) if(d->ent[i].key.k!=V_UNDEF) d->ent[j++]=d->ent[i];
    d->nent=j;
    int64_t want=need>j?need:j;
    if(d->cap<want+1){ d->cap=want<8?8:want*2; d->ent=(DEnt*)xrealloc(d->ent,sizeof(DEnt)*(size_t)d->cap); }
    int64_t isz=8; while(isz*2<=d->cap*3) isz*=2;
    free(d->idx); d->idx=(int64_t*)xmalloc(sizeof(int64_t)*(size_t)isz); d->isize=isz;
    for(int64_t i=0;i<isz;i++) d->idx[i]=-1;
    for(int64_t i=0;i<d->nent;i++){
        uint64_t h=d->ent[i].hash, m=(uint64_t)isz-1, p=h&m, perturb=h;
        while(d->idx[p]!=-1){ perturb>>=5; p=(p*5+1+perturb)&m; }
        d->idx[p]=i;
    }
}
/* the index slot of key (its entry: idx[slot]>=0), or the slot for it */
static int64_t dict_lookup(DictObj *d, Value key, uint64_t h, int *found){
    *found=0;
    if(!d->idx) return -1;
    uint64_t m=(uint64_t)d->isize-1, p=h&m, perturb=h; int64_t freeslot=-1;
    for(;;){
        int64_t ix=d->idx[p];
        if(ix==-1) return freeslot>=0?freeslot:(int64_t)p;
        if(ix==-2){ if(freeslot<0) freeslot=(int64_t)p; }
        else {
            DEnt *e=&d->ent[ix];
            if(e->hash==h && key_eq(e->key,e->hash,key,h)){
                if(!d->idx || d->idx[p]!=ix) return dict_lookup(d,key,h,found);   /* changed by __eq__ */
                *found=1; return (int64_t)p;
            }
        }
        perturb>>=5; p=(p*5+1+perturb)&m;
    }
}
/* a dict key's / set element's hash: a TypeError from hashing becomes CPython 3.14's
   "cannot use 'list' as a dict key (unhashable type: 'list')" */
static uint64_t hash_as(Value key, const char *what){
    if(key.k!=V_OBJ) return mp_hash(key);
    Type *t=key.u.o->type;
    if(t==T_str || t==T_bytes || t==T_frozenset || (t==T_tuple && ((TupleObj*)key.u.o)->hashed) || (!(t->flags&TF_DUNDERS) && t->layout!=LY_TUPLE
       && t->layout!=LY_LIST && t->layout!=LY_DICT && t->layout!=LY_SET && t->layout!=LY_BYTEARRAY)) return mp_hash(key);
    Catch c; uint64_t h=0;
    if(!CATCH_BEGIN(c)){ h=mp_hash(key); CATCH_END(c); return h; }
    Value e=mp_catch_exc(&c);
    if(e.k==V_OBJ && e.u.o->type==E_TypeError){ Value m=mp_tostr(e); mp_raise_t(E_TypeError,"cannot use '%s' as a %s (%s)",t->name->s,what,AS_STR(m)->s); }
    mp_raise(e);
    return 0;
}
int mp_dict_get(DictObj *d, Value key, Value *out){
    uint64_t h=hash_as(key,"dict key"); int found;
    if(!d->used) return 0;
    int64_t slot=dict_lookup(d,key,h,&found);
    if(!found) return 0;
    if(out) *out=d->ent[d->idx[slot]].val;
    return 1;
}
void mp_dict_set(DictObj *d, Value key, Value val){
    uint64_t h=hash_as(key,"dict key"); int found;
    if(!d->idx) dict_rebuild(d,8);
    int64_t slot=dict_lookup(d,key,h,&found);
    if(found){ d->ent[d->idx[slot]].val=val; return; }
    if(d->nent+1>d->cap || (d->nent+1)*3>d->isize*2){ dict_rebuild(d,d->used+1); slot=dict_lookup(d,key,h,&found); }
    d->ent[d->nent].key=key; d->ent[d->nent].val=val; d->ent[d->nent].hash=h;
    d->idx[slot]=d->nent++; d->used++;
}
int mp_dict_del(DictObj *d, Value key){
    uint64_t h=hash_as(key,"dict key"); int found;
    if(!d->used) return 0;
    int64_t slot=dict_lookup(d,key,h,&found);
    if(!found) return 0;
    int64_t ix=d->idx[slot];
    d->ent[ix].key=v_undef(); d->ent[ix].val=v_undef();
    d->idx[slot]=-2; d->used--;
    return 1;
}
int mp_dict_next(DictObj *d, int64_t *pos, Value *key, Value *val){
    while(*pos<d->nent){
        DEnt *e=&d->ent[(*pos)++];
        if(e->key.k!=V_UNDEF){ if(key) *key=e->key; if(val) *val=e->val; return 1; }
    }
    return 0;
}
int mp_dict_get_s(DictObj *d, const char *key, Value *out){ return mp_dict_get(d,mp_str(key),out); }
void mp_dict_set_s(DictObj *d, const char *key, Value val){ mp_dict_set(d,mp_intern(key),val); }

/* ---------------------------------------------------------------- set (CPython's setobject.c) */
#define LINEAR_PROBES 9
#define SET_MINSIZE 8
Value mp_set(Type *t){
    SetObj *s=(SetObj*)mp_alloc(t,sizeof(SetObj));
    s->mask=SET_MINSIZE-1; s->table=(SEnt*)xmalloc(sizeof(SEnt)*SET_MINSIZE); memset(s->table,0,sizeof(SEnt)*SET_MINSIZE);
    return v_obj(s);
}
#define S_EMPTY(e) ((e)->key.k==V_UNDEF && (e)->hash==0)
#define S_DUMMY(e) ((e)->key.k==V_UNDEF && (e)->hash==(uint64_t)-1)
static void set_insert_clean(SEnt *table, uint64_t mask, Value key, uint64_t hash){
    uint64_t perturb=hash, i=hash&mask;
    for(;;){
        SEnt *e=&table[i];
        if(e->key.k==V_UNDEF) goto found;
        if(i+LINEAR_PROBES<=mask){
            for(int j=0;j<LINEAR_PROBES;j++){ e++; if(e->key.k==V_UNDEF) goto found; }
        }
        perturb>>=5; i=(i*5+1+perturb)&mask;
        continue;
      found:
        e->key=key; e->hash=hash; return;
    }
}
static void set_resize(SetObj *s, int64_t minused){
    int64_t newsize=SET_MINSIZE; while(newsize<=minused) newsize<<=1;
    SEnt *old=s->table; int64_t oldmask=s->mask;
    SEnt *nt=(SEnt*)xmalloc(sizeof(SEnt)*(size_t)newsize); memset(nt,0,sizeof(SEnt)*(size_t)newsize);
    for(int64_t i=0;i<=oldmask;i++) if(old[i].key.k!=V_UNDEF) set_insert_clean(nt,(uint64_t)newsize-1,old[i].key,old[i].hash);
    s->table=nt; s->mask=newsize-1; s->fill=s->used; free(old);
}
static void set_add_hash(SetObj *s, Value key, uint64_t hash){
  restart:;
    uint64_t mask=(uint64_t)s->mask, i=hash&mask, perturb=hash;
    SEnt *freeslot=NULL;
    for(;;){
        SEnt *e=&s->table[i];
        int probes= (i+LINEAR_PROBES<=mask) ? LINEAR_PROBES : 0;
        do{
            if(S_EMPTY(e)) goto found_unused_or_dummy;
            if(e->key.k!=V_UNDEF && e->hash==hash){
                SEnt *tbl=s->table; Value sk=e->key;
                if(key_eq(sk,hash,key,hash)) return;
                if(tbl!=s->table || e->key.k==V_UNDEF) goto restart;
                mask=(uint64_t)s->mask;
            } else if(S_DUMMY(e) && !freeslot) freeslot=e;
            e++;
        } while(probes--);
        perturb>>=5; i=(i*5+1+perturb)&mask;
        continue;
      found_unused_or_dummy:
        if(freeslot){ s->used++; freeslot->key=key; freeslot->hash=hash; s->hashed=0; return; }
        s->fill++; s->used++; e->key=key; e->hash=hash; s->hashed=0;
        if((uint64_t)s->fill*5<mask*3) return;
        set_resize(s,s->used>50000?s->used*2:s->used*4);
        return;
    }
}
static SEnt *set_find(SetObj *s, Value key, uint64_t hash){
    uint64_t mask=(uint64_t)s->mask, i=hash&mask, perturb=hash;
    for(;;){
        SEnt *e=&s->table[i];
        int probes= (i+LINEAR_PROBES<=mask) ? LINEAR_PROBES : 0;
        do{
            if(S_EMPTY(e)) return NULL;
            if(e->key.k!=V_UNDEF && e->hash==hash && key_eq(e->key,hash,key,hash)) return e;
            e++;
        } while(probes--);
        perturb>>=5; i=(i*5+1+perturb)&mask;
    }
}
void mp_set_add(SetObj *s, Value v){ set_add_hash(s,v,hash_as(v,"set element")); }
int mp_set_has(SetObj *s, Value v){ return set_find(s,v,hash_as(v,"set element"))!=NULL; }
int mp_set_del(SetObj *s, Value v){
    SEnt *e=set_find(s,v,hash_as(v,"set element"));
    if(!e) return 0;
    e->key=v_undef(); e->hash=(uint64_t)-1; s->used--; s->hashed=0;
    return 1;
}
int mp_set_next(SetObj *s, int64_t *pos, Value *key){
    while(*pos<=s->mask){ SEnt *e=&s->table[(*pos)++]; if(e->key.k!=V_UNDEF){ *key=e->key; return 1; } }
    return 0;
}
Value mp_set_pop(SetObj *s){
    if(!s->used) mp_raise_t(E_KeyError,"pop from an empty set");
    SEnt *e=s->table+(s->finger & s->mask), *limit=s->table+s->mask;
    while(e->key.k==V_UNDEF){ e++; if(e>limit) e=s->table; }
    Value k=e->key;
    e->key=v_undef(); e->hash=(uint64_t)-1; s->used--; s->hashed=0;
    s->finger=(e-s->table)+1;
    return k;
}
/* a copy: a new set the original is merged into (CPython's set_copy) */
void mp_set_merge(SetObj *s, SetObj *o);
Value mp_set_copy(SetObj *s, Type *t){
    Value n=mp_set(t);
    mp_set_merge((SetObj*)n.u.o,s);
    return n;
}
/* add every element of a set (CPython's set_merge) */
void mp_set_merge(SetObj *s, SetObj *o){
    if(o==s || !o->used) return;
    if((s->fill+o->used)*5>=s->mask*3) set_resize(s,(s->used+o->used)*2);
    if(s->fill==0 && s->mask==o->mask && o->fill==o->used){
        memcpy(s->table,o->table,sizeof(SEnt)*(size_t)(o->mask+1)); s->fill=o->fill; s->used=o->used; s->hashed=0; return;
    }
    if(s->fill==0){
        for(int64_t i=0;i<=o->mask;i++) if(o->table[i].key.k!=V_UNDEF){ set_insert_clean(s->table,(uint64_t)s->mask,o->table[i].key,o->table[i].hash); s->fill++; s->used++; }
        s->hashed=0; return;
    }
    for(int64_t i=0;i<=o->mask;i++) if(o->table[i].key.k!=V_UNDEF) set_add_hash(s,o->table[i].key,o->table[i].hash);
}
void mp_set_add_hashed(SetObj *s, Value v, uint64_t h){ set_add_hash(s,v,h); }
int mp_set_has_hashed(SetObj *s, Value v, uint64_t h);
/* after removals (CPython's set_difference_update): more than a quarter dummies -> a new table */
void mp_set_compact(SetObj *s){
    if((uint64_t)(s->fill-s->used)<=(uint64_t)s->mask/4) return;
    set_resize(s,s->used>50000?s->used*2:s->used*4);
}
void mp_set_clear(SetObj *s){
    free(s->table); s->table=(SEnt*)xmalloc(sizeof(SEnt)*SET_MINSIZE); memset(s->table,0,sizeof(SEnt)*SET_MINSIZE);
    s->mask=SET_MINSIZE-1; s->fill=s->used=0; s->hashed=0; s->finger=0;
}
/* set.update(x), set(x), {*x}: a set is merged, anything else added item by item */
void mp_set_update(SetObj *s, Value it){
    if(it.k==V_OBJ && it.u.o->type->layout==LY_SET){ mp_set_merge(s,(SetObj*)it.u.o); return; }
    Value iter=mp_iter(it), x; while(mp_next(iter,&x)) mp_set_add(s,x);
}
/* a -= b (CPython's set_difference_update) */
void mp_set_diff_update(SetObj *s, Value other){
    if(other.k==V_OBJ && other.u.o==(Obj*)s){ mp_set_clear(s); return; }
    if(other.k==V_OBJ && other.u.o->type->layout==LY_SET){
        SetObj *o=(SetObj*)other.u.o;
        if((o->used>>3)>s->used){            /* much bigger: walk the intersection instead */
            Value r=mp_set(T_set); SetObj *rs=(SetObj*)r.u.o; int64_t pos=0; Value k;
            while(mp_set_next(s,&pos,&k)){ uint64_t h=s->table[pos-1].hash; if(mp_set_has_hashed(o,k,h)) mp_set_add_hashed(rs,k,h); }
            o=rs;
        }
        for(int64_t i=0;i<=o->mask;i++) if(o->table[i].key.k!=V_UNDEF){
            SEnt *e=set_find(s,o->table[i].key,o->table[i].hash);
            if(e){ e->key=v_undef(); e->hash=(uint64_t)-1; s->used--; s->hashed=0; }
        }
    } else { Value iter=mp_iter(other), x; while(mp_next(iter,&x)) mp_set_del(s,x); }
    mp_set_compact(s);
}
/* a ^= b (CPython's set_symmetric_difference_update): b's items toggled in a */
void mp_set_xor_update(SetObj *s, Value other){
    if(other.k==V_OBJ && other.u.o==(Obj*)s){ mp_set_clear(s); return; }
    SetObj *o;
    if(other.k==V_OBJ && other.u.o->type->layout==LY_SET) o=(SetObj*)other.u.o;
    else { Value t=mp_set(T_set); mp_set_update((SetObj*)t.u.o,other); o=(SetObj*)t.u.o; }
    for(int64_t i=0;i<=o->mask;i++) if(o->table[i].key.k!=V_UNDEF){
        Value k=o->table[i].key; uint64_t h=o->table[i].hash;
        SEnt *e=set_find(s,k,h);
        if(e){ e->key=v_undef(); e->hash=(uint64_t)-1; s->used--; s->hashed=0; }
        else set_add_hash(s,k,h);
    }
}
int mp_set_has_hashed(SetObj *s, Value v, uint64_t h){ return set_find(s,v,h)!=NULL; }

/* ---------------------------------------------------------------- range, slice, cell, complex */
Value mp_range(int64_t start, int64_t stop, int64_t step){
    RangeObj *r=(RangeObj*)mp_alloc(T_range,sizeof(RangeObj)); r->start=start; r->stop=stop; r->step=step; return v_obj(r);
}
Value mp_slice(Value a, Value b, Value c){
    SliceObj *s=(SliceObj*)mp_alloc(T_slice,sizeof(SliceObj)); s->start=a; s->stop=b; s->step=c; return v_obj(s);
}
Value mp_cell(Value v){ CellObj *c=(CellObj*)mp_alloc(T_cell,sizeof(CellObj)); c->v=v; return v_obj(c); }
Value mp_complex(double re, double im){ ComplexObj *c=(ComplexObj*)mp_alloc(T_complex,sizeof(ComplexObj)); c->re=re; c->im=im; return v_obj(c); }

/* ---------------------------------------------------------------- numbers */
int64_t mp_int_checked(int op, int64_t a, int64_t b){
    int64_t r; int ov;
    switch(op){
        case '+': ov=__builtin_add_overflow(a,b,&r); break;
        case '-': ov=__builtin_sub_overflow(a,b,&r); break;
        default:  ov=__builtin_mul_overflow(a,b,&r); break;
    }
    if(ov) mp_raise_t(E_OverflowError,"integer overflow (ints are 64-bit)");
    return r;
}
/* "d.ddde+XX" one unit more in its last digit (an exact tie rounded down to the even digit may not
   read back while the one above does: 2**-24 is 5.960464477539063e-08) */
static void float_text_up(char *out, size_t n, const char *t){
    char dig[40]; int nd=0, neg=0; const char *p=t;
    if(*p=='-'){ neg=1; p++; }
    for(;*p && *p!='e' && nd<39;p++) if(*p>='0' && *p<='9') dig[nd++]=*p;
    int ex= *p=='e' ? atoi(p+1) : 0, i=nd-1;
    while(i>=0 && dig[i]=='9') dig[i--]='0';
    if(i<0){ dig[0]='1'; ex++; } else dig[i]++;
    dig[nd]=0;
    snprintf(out,n,"%s%c%s%s%se%+03d",neg?"-":"",dig[0],nd>1?".":"",dig+1,"",ex);
}
/* shortest repr that reads back the same, Python's spelling (1e+16, 1.5e-07, inf, nan) */
void mp_float_repr(char *out, size_t n, double f){
    if(isnan(f)){ snprintf(out,n,"nan"); return; }
    if(isinf(f)){ snprintf(out,n,f>0?"inf":"-inf"); return; }
    if(f==0){ snprintf(out,n,signbit(f)?"-0.0":"0.0"); return; }
    char buf[64]; int prec;
    for(prec=1;prec<=17;prec++){ snprintf(buf,sizeof buf,"%.*e",prec-1,f); double y=strtod(buf,NULL); if(y==f) break;
        if(prec<17 && fabs(y)<fabs(f)){ char up[64]; float_text_up(up,sizeof up,buf); if(strtod(up,NULL)==f){ strcpy(buf,up); break; } } }
    /* digits and exponent of the %e form */
    char digits[32]; int nd=0, neg=0; const char *p=buf;
    if(*p=='-'){ neg=1; p++; }
    for(;*p && *p!='e';p++) if(*p>='0' && *p<='9') digits[nd++]=*p;
    digits[nd]=0;
    int exp10=atoi(p+1);
    while(nd>1 && digits[nd-1]=='0') digits[--nd]=0;
    char *o=out; size_t room=n;
    #define PUT(c) do{ if(room>1){ *o++=(c); room--; } }while(0)
    if(neg) PUT('-');
    if(exp10>=16 || exp10<-4){                   /* scientific: d[.ddd]e+XX */
        PUT(digits[0]);
        if(nd>1){ PUT('.'); for(int i=1;i<nd;i++) PUT(digits[i]); }
        char e[16]; snprintf(e,sizeof e,"e%c%02d",exp10<0?'-':'+',exp10<0?-exp10:exp10);
        for(char *q=e;*q;q++) PUT(*q);
    } else if(exp10<0){                          /* 0.000ddd */
        PUT('0'); PUT('.');
        for(int i=0;i<-exp10-1;i++) PUT('0');
        for(int i=0;i<nd;i++) PUT(digits[i]);
    } else {                                     /* ddd.ddd */
        for(int i=0;i<=exp10;i++) PUT(i<nd?digits[i]:'0');
        PUT('.');
        if(nd>exp10+1) for(int i=exp10+1;i<nd;i++) PUT(digits[i]);
        else PUT('0');
    }
    *o=0;
    #undef PUT
}
