/* ========================= Interpreter: generators, coroutines =========================
   Generators and coroutines (a frame of their own, suspended at yield),
   async generators and their awaitables. */

#include "run.h"

Value gen_new(Frame *f, FuncObj *fn){
    CodeObj *co=fn->code;
    Type *t= (co->flags&CO_CORO) ? T_coroutine : (co->flags&CO_ASYNCGEN) ? T_asyncgen : T_generator;
    GenObj *g=(GenObj*)mp_alloc(t,sizeof(GenObj));
    g->f=f; g->kind= t==T_coroutine?G_CORO:t==T_asyncgen?G_ASYNCGEN:G_GEN;
    g->name=fn->name; g->qualname=fn->qualname; g->awaiting=v_undef();
    f->gen=(Obj*)g; f->state=FS_NEW;
    return v_obj(g);
}
static Value stop_value(Value e){ TupleObj *a=AS_TUPLE(AS_EXC(e)->args); return a->len?a->items[0]:v_none(); }
/* resume g: sent (resume>0) or an exception thrown in (throwing); done: it returned */
static Value gen_resume(GenObj *g, Value sent, int throwing, int *done){
    const char *what= g->kind==G_CORO?"coroutine":"generator";
    *done=0;
    if(g->running) mp_raise_t(E_ValueError,"%s already executing",what);
    Frame *f=g->f;
    if(!f || f->state==FS_DONE){
        if(throwing) reraise(sent);
        if(g->kind==G_CORO) mp_raise_t(E_RuntimeError,"cannot reuse already awaited coroutine");
        *done=1; return v_none();
    }
    if(f->state==FS_NEW && !throwing && !IS_NONE(sent)) mp_raise_t(E_TypeError,"can't send non-None value to a just-started %s",what);
    if(f->state==FS_NEW && throwing){ f->state=FS_DONE; reraise(sent); }
    g->running=1;
    Catch c; Value r;
    int resume= f->state==FS_SUSPENDED;
    if(!CATCH_BEGIN(c)){ r=eval(f,sent,resume,throwing); CATCH_END(c); }
    else {
        Value e=mp_catch_exc(&c);
        g->running=0; f->state=FS_DONE;
        if(mp_isinstance(e,E_StopIteration)){
            Value n=mp_exc(E_RuntimeError,"%s raised StopIteration",what);
            AS_EXC(n)->cause=e; AS_EXC(n)->context=e; AS_EXC(n)->suppress=1;
            reraise(n);
        }
        if(g->kind==G_ASYNCGEN && mp_isinstance(e,E_StopAsyncIteration)){
            Value n=mp_exc(E_RuntimeError,"async generator raised StopAsyncIteration");
            AS_EXC(n)->cause=e; AS_EXC(n)->context=e; AS_EXC(n)->suppress=1;
            reraise(n);
        }
        reraise(e);
    }
    g->running=0;
    if(f->state==FS_DONE){ *done=1; return r; }
    return r;
}
Value mp_gen_send(Value gv, Value v, int *done){ return gen_resume((GenObj*)gv.u.o,v,0,done); }
Value mp_gen_throw(Value gv, Value e, int *done){
    GenObj *g=(GenObj*)gv.u.o;
    e=instantiate_exc(e);
    Frame *f=g->f;
    if(f && f->state==FS_SUSPENDED && f->yf.k!=V_UNDEF){
        Value inner=f->yf, out; int idone=0;
        Catch c;
        if(!CATCH_BEGIN(c)){
            if(inner.k==V_OBJ && inner.u.o->type->layout==LY_GEN){
                if(mp_isinstance(e,E_GeneratorExit)){ mp_gen_close(inner); CATCH_END(c); f->yf=v_undef(); return gen_resume(g,e,1,done); }
                out=mp_gen_throw(inner,e,&idone);
            } else {
                Value th;
                if(!mp_isinstance(e,E_GeneratorExit) && mp_getattr_opt(inner,mp_intern("throw"),&th)) out=mp_call1(th,e);
                else { CATCH_END(c); f->yf=v_undef(); return gen_resume(g,e,1,done); }
            }
            CATCH_END(c);
        } else {
            Value x=mp_catch_exc(&c);
            f->yf=v_undef();
            if(mp_isinstance(x,E_StopIteration)){ out=stop_value(x); idone=1; }
            else return gen_resume(g,x,1,done);
        }
        if(!idone) return out;
        /* the inner one returned: the yield from goes on with its value */
        f->yf=v_undef();
        f->stack[f->sp-1]=out; f->ip=f->yf_exit; f->nopush=1;
        return gen_resume(g,v_none(),0,done);
    }
    return gen_resume(g,e,1,done);
}
int mp_gen_needs_close(Value v){
    if(!IS(v,T_generator)) return 0;
    GenObj *g=(GenObj*)v.u.o;
    return g->kind==G_GEN && g->f && g->f->state==FS_SUSPENDED && (g->f->nblocks>0 || g->f->yf.k!=V_UNDEF);
}
void mp_gen_close_quietly(Value v){
    Catch c; if(!CATCH_BEGIN(c)){ mp_gen_close(v); CATCH_END(c); } else (void)mp_catch_exc(&c);
}
/* for loops left early (break, return, an exception): the generators they made and own are closed
   (CPython frees them there, running their finally blocks); what that raises is dropped */
void close_loop_gens(Value *v, int n){
    for(int i=0;i<n;i++){ if(!IS(v[i],T_generator)) continue;
        GenObj *g=(GenObj*)v[i].u.o; if(!g->loop_owned || !g->f || g->f->state!=FS_SUSPENDED) continue;
        g->loop_owned=0;
        Catch c; if(!CATCH_BEGIN(c)){ mp_gen_close(v[i]); CATCH_END(c); } else (void)mp_catch_exc(&c); }
}
void mp_gen_close(Value gv){
    GenObj *g=(GenObj*)gv.u.o;
    Frame *f=g->f;
    if(!f || f->state==FS_DONE || f->state==FS_NEW){ if(f) f->state=FS_DONE; return; }
    Catch c; int done=0;
    if(!CATCH_BEGIN(c)){ mp_gen_throw(gv,v_obj(E_GeneratorExit),&done); CATCH_END(c); }
    else {
        Value e=mp_catch_exc(&c);
        if(mp_isinstance(e,E_GeneratorExit) || mp_isinstance(e,E_StopIteration)) return;
        reraise(e);
    }
    if(!done) mp_raise_t(E_RuntimeError,"%s ignored GeneratorExit",g->kind==G_CORO?"coroutine":"generator");
}
/* ---------------------------------------------------------------- async generators */
/* `yield v` in an async generator gives v wrapped (what an await yields passes through as it is);
   __anext__ / asend / athrow / aclose give an awaitable: an IT_NATIVE iterator (src the generator,
   aux the value sent or the exception, i 0 send / 1 throw / 2 close, n 0 new / 1 running / 2 done) */
static Type *T_agwrap;
static Value ag_native;
Value ag_wrap(Value v){ BoxObj *b=(BoxObj*)mp_alloc(T_agwrap,sizeof(BoxObj)); b->v=v; return v_obj(b); }
static MPY_NORETURN void ag_stop(Value v){ mp_raise(mp_exc_args(E_StopIteration,mp_tuple(1,&v))); }
static Value ag_step(int argc, Value *argv, TupleObj *kw){
    (void)argc; (void)kw;
    IterObj *io=(IterObj*)argv[0].u.o; Value sent=argv[1];
    if(io->n==2) mp_raise_t(E_RuntimeError,io->i==0?"cannot reuse already awaited __anext__()/asend()":"cannot reuse already awaited aclose()/athrow()");
    GenObj *g=(GenObj*)io->src.u.o; int done=0; Value r;
    Catch c;
    if(!CATCH_BEGIN(c)){
        if(io->n==0){ io->n=1;
            if(io->i==0) r=gen_resume(g,io->aux,0,&done);
            else if(io->i==2 && (!g->f || g->f->state!=FS_SUSPENDED)){ if(g->f) g->f->state=FS_DONE; done=1; r=v_none(); }
            else r=mp_gen_throw(io->src,io->i==2?v_obj(E_GeneratorExit):io->aux,&done);
        } else r=gen_resume(g,sent,0,&done);
        CATCH_END(c);
    } else {
        Value x=mp_catch_exc(&c); io->n=2;
        if(io->i==2 && (mp_isinstance(x,E_GeneratorExit)||mp_isinstance(x,E_StopAsyncIteration))) ag_stop(v_none());
        reraise(x);
    }
    if(done){ io->n=2; if(io->i==2) ag_stop(v_none()); mp_raise(mp_exc_args(E_StopAsyncIteration,mp_tuple(0,NULL))); }
    if(IS(r,T_agwrap)){ io->n=2; if(io->i==2) mp_raise_t(E_RuntimeError,"async generator ignored GeneratorExit"); ag_stop(((BoxObj*)r.u.o)->v); }
    return r;
}
static Value ag_awaitable(Value gen, int kind, Value aux){
    IterObj *it=(IterObj*)mp_alloc(T_iter,sizeof(IterObj)); it->kind=IT_NATIVE; it->src=gen; it->aux=aux; it->aux2=ag_native; it->i=kind; it->n=0;
    return v_obj(it);
}
static Value ag_anext(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)kw; return ag_awaitable(argv[0],0,v_none()); }
static Value ag_asend(int argc, Value *argv, TupleObj *kw){ (void)kw; if(argc!=2) mp_raise_t(E_TypeError,"asend() takes exactly one argument (%d given)",argc-1); return ag_awaitable(argv[0],0,argv[1]); }
static Value ag_athrow(int argc, Value *argv, TupleObj *kw){ (void)kw; if(argc<2) mp_raise_t(E_TypeError,"athrow expected at least 1 argument, got 0"); return ag_awaitable(argv[0],1,instantiate_exc(argv[1])); }
static Value ag_aclose(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)kw; return ag_awaitable(argv[0],2,v_none()); }
static Value ag_self(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)kw; return argv[0]; }
/* anext(ait, default): the default instead of StopAsyncIteration (an IT_NATIVE over the awaitable) */
static Value anext_default_step(int argc, Value *argv, TupleObj *kw){
    (void)argc; (void)kw;
    IterObj *io=(IterObj*)argv[0].u.o; Value out; int more;
    Catch c;
    if(!CATCH_BEGIN(c)){ more=send_into(io->src,argv[1],&out); CATCH_END(c); }
    else { Value x=mp_catch_exc(&c); if(mp_isinstance(x,E_StopAsyncIteration)) ag_stop(io->aux); reraise(x); }
    if(!more) ag_stop(out);
    return out;
}
Value mp_anext(Value ait, int has_default, Value dflt){
    Value m=mp_type_lookup_s(TYPE(ait),"__anext__");
    if(m.k==V_UNDEF) mp_raise_t(E_TypeError,"'%s' object is not an async iterator",mp_type_name(ait));
    Value aw=mp_call1(m,ait);
    if(!has_default) return aw;
    static Value step; if(step.k!=V_OBJ){ step=mp_native("anext",anext_default_step); mp_gc_add_root(&step); }
    IterObj *it=(IterObj*)mp_alloc(T_iter,sizeof(IterObj)); it->kind=IT_NATIVE; it->src=mp_await_iter(aw); it->aux=dflt; it->aux2=step;
    return v_obj(it);
}
void mp_asyncgen_init(void){
    T_agwrap=AS_TYPE(mp_new_type("async_generator_wrapped_value",T_object,LY_BOX,0));
    ag_native=mp_native("async_generator_asend",ag_step); mp_gc_add_root(&ag_native);
    mp_type_add(T_asyncgen,"__aiter__",ag_self); mp_type_add(T_asyncgen,"__anext__",ag_anext);
    mp_type_add(T_asyncgen,"asend",ag_asend); mp_type_add(T_asyncgen,"athrow",ag_athrow); mp_type_add(T_asyncgen,"aclose",ag_aclose);
}
Value mp_await_iter(Value o){
    if(IS(o,T_coroutine)) return o;
    if(IS(o,T_generator) && ((GenObj*)o.u.o)->f && (((GenObj*)o.u.o)->f->code->flags&CO_COMP)==0 && 0) return o;
    if(IS(o,T_iter) && ((IterObj*)o.u.o)->kind==IT_NATIVE) return o;
    Value m=mp_type_lookup_s(TYPE(o),"__await__");
    if(m.k==V_UNDEF) mp_raise_t(E_TypeError,"'%s' object can't be awaited",mp_type_name(o));
    Value it=mp_call1(m,o);
    if(IS(it,T_coroutine)) mp_raise_t(E_TypeError,"__await__() returned a coroutine");
    if(!(IS(it,T_generator) || IS(it,T_iter) || (TYPE(it)->flags&TF_DUNDERS))) mp_raise_t(E_TypeError,"__await__() returned non-iterator of type '%s'",mp_type_name(it));
    return it;
}
/* send v into an iterator `yield from` / `await` drives: 1 yielded, 0 returned (out: the value) */
int send_into(Value recv, Value v, Value *out){
    if(recv.k==V_OBJ && recv.u.o->type->layout==LY_GEN){ int done; *out=mp_gen_send(recv,v,&done); return !done; }
    if(IS(recv,T_iter) && ((IterObj*)recv.u.o)->kind==IT_NATIVE) return mp_native_send(recv,v,out);
    Catch c; Value r;
    if(!CATCH_BEGIN(c)){
        if(IS_NONE(v)){ int ok=mp_next(recv,&r); CATCH_END(c); if(!ok){ *out=v_none(); return 0; } *out=r; return 1; }
        r=mp_callmethod(recv,"send",1,&v);
        CATCH_END(c);
        *out=r; return 1;
    }
    Value e=mp_catch_exc(&c);
    if(mp_isinstance(e,E_StopIteration)){ *out=stop_value(e); return 0; }
    reraise(e);
}

