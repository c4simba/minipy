/* ========================= Interpreter, modules: threads =========================
   thread: OS threads under the GIL; _thread: locks, RLocks, thread
   identities. */

#include "modules.h"

typedef struct { Thread th; volatile int done; Value fn, args; } PyThread;
static PyThread *pthreads[MP_MAX_THREADS];
static ppy_lock user_locks[256]; static int lock_used[256];
static void thread_entry(void *p){
    PyThread *pt=(PyThread*)p;
    ppy_lock_acquire(&mp_gil);
    mp_ts=&pt->th;
    int anchor; pt->th.cstack_base=&anchor;
    Catch c;
    if(!CATCH_BEGIN(c)){
        Value a=pt->args;
        if(IS_TUPLE(a)) mp_call(pt->fn,(int)AS_TUPLE(a)->len,AS_TUPLE(a)->items,NULL);
        else mp_call0(pt->fn);
        CATCH_END(c);
    } else {
        Value e=mp_catch_exc(&c);
        if(!mp_isinstance(e,E_SystemExit)) mp_print_exception(e);
    }
    pt->th.roots[0]=v_undef(); pt->th.roots[1]=v_undef();
    pt->done=1;
    mp_thread_remove(&pt->th);
    ppy_lock_release(&mp_gil);
}
static Value th_start(int argc, Value *argv, TupleObj *kw){
    mod_no_kw("start",kw); mod_nargs("start",argc,1,2);
    if(argc==2 && !IS_TUPLE(argv[1])) mp_raise_t(E_TypeError,"thread.start args must be a tuple");
    int h=-1; for(int i=0;i<MP_MAX_THREADS;i++) if(!pthreads[i] || pthreads[i]->done){ h=i; break; }
    if(h<0) mp_raise_t(E_RuntimeError,"too many threads");
    PyThread *pt=(PyThread*)xmalloc(sizeof(PyThread)); memset(pt,0,sizeof *pt);
    pt->fn=argv[0]; pt->args= argc==2 ? argv[1] : v_none();
    pt->th.roots[0]=pt->fn; pt->th.roots[1]=pt->args; pt->th.id=h+1;
    pthreads[h]=pt;
    mp_thread_add(&pt->th);
    if(ppy_thread_spawn(thread_entry,pt)!=0){ mp_thread_remove(&pt->th); pthreads[h]=NULL; free(pt); mp_raise_t(E_RuntimeError,"cannot start a thread"); }
    return v_int(h);
}
static Value th_join(int argc, Value *argv, TupleObj *kw){
    mod_no_kw("join",kw); mod_nargs("join",argc,1,1);
    int64_t h=mp_index(argv[0],"join");
    if(h<0||h>=MP_MAX_THREADS||!pthreads[h]) mp_raise_t(E_ValueError,"no such thread");
    PyThread *pt=pthreads[h];
    Thread *self=mp_ts;
    while(!pt->done){ mp_gil_release(); ppy_thread_sleep_ms(1); mp_gil_acquire(self); }
    return v_none();
}
static Value th_lock(int argc, Value *argv, TupleObj *kw){ (void)argv; mod_no_kw("lock",kw); mod_nargs("lock",argc,0,0);
    for(int i=0;i<256;i++) if(!lock_used[i]){ lock_used[i]=1; ppy_lock_init(&user_locks[i]); return v_int(i); }
    mp_raise_t(E_RuntimeError,"too many locks"); }
static Value th_acquire(int argc, Value *argv, TupleObj *kw){ mod_no_kw("acquire",kw); mod_nargs("acquire",argc,1,1);
    int64_t i=mp_index(argv[0],"acquire"); if(i<0||i>=256||!lock_used[i]) mp_raise_t(E_ValueError,"no such lock");
    if(ppy_lock_try(&user_locks[i])) return v_none();
    Thread *self=mp_ts; mp_gil_release();
    ppy_lock_acquire(&user_locks[i]);
    mp_gil_acquire(self);
    return v_none(); }
static Value th_release(int argc, Value *argv, TupleObj *kw){ mod_no_kw("release",kw); mod_nargs("release",argc,1,1);
    int64_t i=mp_index(argv[0],"release"); if(i<0||i>=256||!lock_used[i]) mp_raise_t(E_ValueError,"no such lock");
    ppy_lock_release(&user_locks[i]); return v_none(); }
/* ---------------------------------------------------------------- _thread: locks any thread may release, threads (threading.py's) */
typedef struct { Obj h; volatile int locked; } LockObj;
static Type *T_lock;
static double mono_now(void){
#if defined(PPY_KOLIBRI)
    uint32_t in[7]={26,10,0,0,0,0,0}, out[6]={0}; ppy_platform_syscall(in,out); return ((double)out[1]*4294967296.0+(double)out[0])/1e9;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (double)ts.tv_sec+ts.tv_nsec/1e9;
#endif
}
/* takes the lock (a flag the GIL guards): waiting with the GIL released; 0 when timeout runs out */
static int lock_take(LockObj *l, int blocking, double timeout){
    if(!l->locked){ l->locked=1; return 1; }
    if(!blocking) return 0;
    double deadline= timeout>=0 ? mono_now()+timeout : 0;
    Thread *self=mp_ts; int spins=0;
    for(;;){
        mp_gil_release();
        if(spins<20){ ppy_thread_yield(); spins++; } else ppy_thread_sleep_ms(1);
        mp_gil_acquire(self);
        if(!l->locked){ l->locked=1; return 1; }
        if(timeout>=0 && mono_now()>=deadline) return 0;
    }
}
static Value lk_acquire(int argc, Value *argv, TupleObj *kw){
    int np=mod_npos(argc,kw);
    Value b= np>1 ? argv[1] : mod_kwarg(argc,argv,kw,"blocking",v_bool(1));
    Value t= np>2 ? argv[2] : mod_kwarg(argc,argv,kw,"timeout",v_int(-1));
    int blocking=mp_truth(b); double timeout= IS_INTLIKE(t) ? (double)t.u.i : farg(t);
    if(!blocking && timeout!=-1) mp_raise_t(E_ValueError,"can't specify a timeout for a non-blocking call");
    if(timeout<0 && timeout!=-1) mp_raise_t(E_ValueError,"timeout value must be a non-negative number");
    return v_bool(lock_take((LockObj*)argv[0].u.o,blocking,timeout));
}
static Value lk_release(int argc, Value *argv, TupleObj *kw){ mod_no_kw("release",kw); mod_nargs("release",argc,1,1);
    LockObj *l=(LockObj*)argv[0].u.o; if(!l->locked) mp_raise_t(E_RuntimeError,"release unlocked lock"); l->locked=0; return v_none(); }
static Value lk_locked(int argc, Value *argv, TupleObj *kw){ mod_no_kw("locked",kw); mod_nargs("locked",argc,1,1); return v_bool(((LockObj*)argv[0].u.o)->locked); }
static Value lk_enter(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; lock_take((LockObj*)argv[0].u.o,1,-1); return v_bool(1); }
static Value lk_exit(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; ((LockObj*)argv[0].u.o)->locked=0; return v_none(); }
static Value lk_repr(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; char b[96];
    snprintf(b,sizeof b,"<%s _thread.lock object at %p>",((LockObj*)argv[0].u.o)->locked?"locked":"unlocked",(void*)argv[0].u.o); return mp_str(b); }
/* _thread.RLock: the holder may take it again (count), only it may release it */
typedef struct { Obj h; volatile int locked; int64_t owner, count; } RLockObj;
static Type *T_rlock;
static Value rl_acquire(int argc, Value *argv, TupleObj *kw){
    RLockObj *l=(RLockObj*)argv[0].u.o; int64_t me=mp_ts->id;
    if(l->count && l->owner==me){ l->count++; return v_bool(1); }
    Value r=lk_acquire(argc,argv,kw);
    if(mp_truth(r)){ l->owner=me; l->count=1; }
    return r; }
static Value rl_release(int argc, Value *argv, TupleObj *kw){ mod_no_kw("release",kw); mod_nargs("release",argc,1,1);
    RLockObj *l=(RLockObj*)argv[0].u.o;
    if(!l->count || l->owner!=mp_ts->id) mp_raise_t(E_RuntimeError,"cannot release un-acquired lock");
    if(--l->count==0){ l->owner=-1; l->locked=0; }
    return v_none(); }
static Value rl_enter(int argc, Value *argv, TupleObj *kw){ (void)kw; Value a[1]={argv[0]}; (void)argc; return rl_acquire(1,a,NULL); }
static Value rl_exit(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; Value a[1]={argv[0]}; return rl_release(1,a,NULL); }
static Value rl_is_owned(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; RLockObj *l=(RLockObj*)argv[0].u.o; return v_bool(l->count && l->owner==mp_ts->id); }
static Value rl_count(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; RLockObj *l=(RLockObj*)argv[0].u.o; return v_int(l->owner==mp_ts->id ? l->count : 0); }
static Value rl_release_save(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; RLockObj *l=(RLockObj*)argv[0].u.o;
    if(!l->count) mp_raise_t(E_RuntimeError,"cannot release un-acquired lock");
    Value st[2]={v_int(l->count),v_int(l->owner)}; l->count=0; l->owner=-1; l->locked=0; return mp_tuple(2,st); }
static Value rl_acquire_restore(int argc, Value *argv, TupleObj *kw){ (void)kw; mod_nargs("_acquire_restore",argc,1,1); RLockObj *l=(RLockObj*)argv[0].u.o;
    Value a[1]={argv[0]}; lk_acquire(1,a,NULL);
    if(IS_TUPLE(argv[1]) && AS_TUPLE(argv[1])->len==2){ l->count=AS_TUPLE(argv[1])->items[0].u.i; l->owner=AS_TUPLE(argv[1])->items[1].u.i; }
    return v_none(); }
static Value rl_repr(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; RLockObj *l=(RLockObj*)argv[0].u.o; char b[160];
    snprintf(b,sizeof b,"<%s _thread.RLock object owner=%lld count=%lld at %p>",l->locked?"locked":"unlocked",(long long)(l->count?l->owner:0),(long long)l->count,(void*)l); return mp_str(b); }
static Value make_rlock(Type *t, int argc, Value *argv, TupleObj *kw){ (void)t; (void)argc; (void)argv; (void)kw;
    RLockObj *l=(RLockObj*)mp_alloc(T_rlock,sizeof(RLockObj)); l->locked=0; l->owner=-1; l->count=0; return v_obj(l); }
static Value tl_allocate(int argc, Value *argv, TupleObj *kw){ (void)argv; mod_no_kw("allocate_lock",kw); mod_nargs("allocate_lock",argc,0,0);
    LockObj *l=(LockObj*)mp_alloc(T_lock,sizeof(LockObj)); l->locked=0; return v_obj(l); }
static Value make_lock_type(Type *t, int argc, Value *argv, TupleObj *kw){ (void)t; return tl_allocate(argc,argv,kw); }
static Value tl_start(int argc, Value *argv, TupleObj *kw){
    mod_no_kw("start_new_thread",kw); mod_nargs("start_new_thread",argc,2,3);
    if(!IS_TUPLE(argv[1])) mp_raise_t(E_TypeError,"2nd arg must be a tuple");
    Value fn=argv[0], args=argv[1];
    if(argc==3 && !IS_NONE(argv[2])){
        if(!IS_DICT(argv[2])) mp_raise_t(E_TypeError,"optional 3rd arg must be a dictionary");
        if(AS_DICT(argv[2])->used){                                   /* (keywords: functools.partial does the call) */
            Value ft=mp_str("functools"); Value fm=mp_builtin_import(1,&ft,NULL);
            Value pa[1]={fn}; Value names=mp_tuple(AS_DICT(argv[2])->used,NULL); Value *vals=(Value*)xmalloc(sizeof(Value)*(size_t)(AS_DICT(argv[2])->used+1));
            int64_t pos=0, i=0; Value k, v; vals[0]=fn;
            while(mp_dict_next(AS_DICT(argv[2]),&pos,&k,&v)){ AS_TUPLE(names)->items[i]=k; vals[1+i]=v; i++; }
            (void)pa; fn=mp_call(mp_getattr_s(fm,"partial"),(int)(1+i),vals,AS_TUPLE(names)); free(vals); } }
    Value a[2]={fn,args};
    Value h=th_start(2,a,NULL);
    return v_int(h.u.i+1);                                           /* (the thread's id) */
}
static Value tl_get_ident(int argc, Value *argv, TupleObj *kw){ (void)argv; (void)argc; (void)kw; return v_int(mp_ts->id); }
static Value tl_count(int argc, Value *argv, TupleObj *kw){ (void)argv; (void)argc; (void)kw;
    int n=0; for(int i=0;i<MP_MAX_THREADS;i++) if(pthreads[i] && !pthreads[i]->done) n++; return v_int(n); }
static Value tl_stack_size(int argc, Value *argv, TupleObj *kw){ (void)argv; (void)argc; (void)kw; return v_int(0); }
Value make__thread(void){
    Value m=register_module("_thread");
    if(!T_lock){ T_lock=AS_TYPE(mp_new_type("lock",T_object,LY_OBJECT,0)); T_lock->make=make_lock_type;
        mp_type_add(T_lock,"acquire",lk_acquire); mp_type_add(T_lock,"release",lk_release); mp_type_add(T_lock,"locked",lk_locked);
        mp_type_add(T_lock,"__enter__",lk_enter); mp_type_add(T_lock,"__exit__",lk_exit); mp_type_add(T_lock,"__repr__",lk_repr);
        mp_type_add(T_lock,"acquire_lock",lk_acquire); mp_type_add(T_lock,"release_lock",lk_release); mp_type_add(T_lock,"locked_lock",lk_locked); }
    set_fn(m,"allocate_lock",tl_allocate); set_fn(m,"allocate",tl_allocate); set_fn(m,"start_new_thread",tl_start); set_fn(m,"start_new",tl_start);
    set_fn(m,"get_ident",tl_get_ident); set_fn(m,"get_native_id",tl_get_ident); set_fn(m,"_count",tl_count); set_fn(m,"stack_size",tl_stack_size);
    if(!T_rlock){ T_rlock=AS_TYPE(mp_new_type("RLock",T_object,LY_OBJECT,0)); T_rlock->make=make_rlock;
        mp_type_add(T_rlock,"acquire",rl_acquire); mp_type_add(T_rlock,"release",rl_release); mp_type_add(T_rlock,"__enter__",rl_enter);
        mp_type_add(T_rlock,"__exit__",rl_exit); mp_type_add(T_rlock,"_is_owned",rl_is_owned); mp_type_add(T_rlock,"_recursion_count",rl_count);
        mp_type_add(T_rlock,"_release_save",rl_release_save); mp_type_add(T_rlock,"_acquire_restore",rl_acquire_restore);
        mp_type_add(T_rlock,"locked",lk_locked); mp_type_add(T_rlock,"__repr__",rl_repr); }
    mp_dict_set_s(mdict(m),"RLock",v_obj(T_rlock));
    mp_dict_set_s(mdict(m),"LockType",v_obj(T_lock));
    mp_dict_set_s(mdict(m),"error",v_obj(E_RuntimeError));
    mp_dict_set_s(mdict(m),"TIMEOUT_MAX",v_float(4294967.0));
    return m;
}
Value make_thread(void){
    Value m=register_module("thread");
    set_fn(m,"start",th_start); set_fn(m,"join",th_join); set_fn(m,"sleep",ti_sleep); set_fn(m,"lock",th_lock);
    set_fn(m,"acquire",th_acquire); set_fn(m,"release",th_release); set_fn(m,"current",tl_get_ident);
    return m;
}

