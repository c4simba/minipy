/* ========================= Interpreter: calls =========================
   Frames, binding arguments to parameters (CPython's messages), calling
   functions, natives and methods. */

#include "run.h"

Frame *frame_new(CodeObj *co, FuncObj *fn, DictObj *globals, DictObj *locals){
    size_t nv=(size_t)(co->nlocals+co->ncells+co->nfrees+co->stacksize);
    size_t sz=sizeof(Frame)+sizeof(Value)*nv+sizeof(Block)*(size_t)co->nblocks;
    Frame *f=(Frame*)xmalloc(sz); memset(f,0,sz);
    f->fast=(Value*)(f+1); f->cells=f->fast+co->nlocals; f->stack=f->cells+co->ncells+co->nfrees; f->blocks=(Block*)(f->stack+co->stacksize);
    f->code=co; f->func=fn; f->globals=globals; f->locals=locals; f->builtins=mp_builtins;
    f->yf=v_undef();
    return f;
}

/* ---------------------------------------------------------------- argument binding */
static const char *fname(FuncObj *fn){ return fn->qualname->s; }
static void missing_error(FuncObj *fn, Value *fast, int from, int to, const char *what){
    CodeObj *co=fn->code; int n=0; for(int i=from;i<to;i++) if(fast[i].k==V_UNDEF) n++;
    SBuf b={0}; int k=0;
    for(int i=from;i<to;i++) if(fast[i].k==V_UNDEF){
        if(k){ if(n==2) sb_puts(&b," and "); else if(k==n-1) sb_puts(&b,", and "); else sb_puts(&b,", "); }
        sb_printf(&b,"'%s'",co->varnames[i]->s); k++;
    }
    Value names=sb_value(&b);
    mp_raise_t(E_TypeError,"%s() missing %d required %s argument%s: %s",fname(fn),n,what,n==1?"":"s",mp_cstr(names));
}
static void bind_args(FuncObj *fn, Value *fast, int argc, Value *argv, TupleObj *kw){
    CodeObj *co=fn->code;
    int npos=co->argc, nkwo=co->kwonly, nkw= kw ? (int)kw->len : 0, npass=argc-nkw;
    int vi= (co->flags&CO_VARARGS) ? npos+nkwo : -1;
    int ki= (co->flags&CO_VARKW) ? npos+nkwo+(vi>=0) : -1;
    int n= npass<npos ? npass : npos;
    for(int i=0;i<n;i++) fast[i]=argv[i];
    if(npass>npos){
        if(vi<0){
            int ndef= fn->defaults ? (int)fn->defaults->len : 0;
            int given=npass; int nkwgiven=0;
            for(int i=0;i<nkw;i++) for(int j=npos;j<npos+nkwo;j++) if(kw->items[i].u.o==(Obj*)co->varnames[j]) nkwgiven++;
            if(ndef) mp_raise_t(E_TypeError,"%s() takes from %d to %d positional arguments but %d %s given",fname(fn),npos-ndef,npos,given,given==1?"was":"were");
            if(nkwgiven) mp_raise_t(E_TypeError,"%s() takes %d positional argument%s but %d positional argument%s (and %d keyword-only argument%s) were given",fname(fn),npos,npos==1?"":"s",given,given==1?"":"s",nkwgiven,nkwgiven==1?"":"s");
            mp_raise_t(E_TypeError,"%s() takes %d positional argument%s but %d %s given",fname(fn),npos,npos==1?"":"s",given,given==1?"was":"were");
        }
        fast[vi]=mp_tuple(npass-npos,argv+npos);
    } else if(vi>=0) fast[vi]=mp_tuple(0,NULL);
    if(ki>=0) fast[ki]=mp_dict();
    for(int i=0;i<nkw;i++){
        Value name=kw->items[i], val=argv[npass+i];
        int j=-1;
        for(int k=co->posonly;k<npos+nkwo;k++) if(co->varnames[k]==AS_STR(name) || !strcmp(co->varnames[k]->s,mp_cstr(name))){ j=k; break; }
        if(j<0){
            if(ki>=0){ mp_dict_set(AS_DICT(fast[ki]),name,val); continue; }
            for(int k=0;k<co->posonly;k++) if(!strcmp(co->varnames[k]->s,mp_cstr(name)))
                mp_raise_t(E_TypeError,"%s() got some positional-only arguments passed as keyword arguments: '%s'",fname(fn),mp_cstr(name));
            mp_raise_t(E_TypeError,"%s() got an unexpected keyword argument '%s'",fname(fn),mp_cstr(name));
        }
        if(fast[j].k!=V_UNDEF) mp_raise_t(E_TypeError,"%s() got multiple values for argument '%s'",fname(fn),mp_cstr(name));
        fast[j]=val;
    }
    if(npass<npos){
        int ndef= fn->defaults ? (int)fn->defaults->len : 0;
        for(int i=npos-ndef;i<npos;i++) if(fast[i].k==V_UNDEF) fast[i]=fn->defaults->items[i-(npos-ndef)];
        for(int i=0;i<npos;i++) if(fast[i].k==V_UNDEF){ missing_error(fn,fast,0,npos,"positional"); }
    }
    if(nkwo){
        for(int i=npos;i<npos+nkwo;i++) if(fast[i].k==V_UNDEF && fn->kwdefaults){ Value d; if(mp_dict_get(fn->kwdefaults,v_obj(co->varnames[i]),&d)) fast[i]=d; }
        for(int i=npos;i<npos+nkwo;i++) if(fast[i].k==V_UNDEF) missing_error(fn,fast,npos,npos+nkwo,"keyword-only");
    }
}

/* ---------------------------------------------------------------- calls */
Value call_function(FuncObj *fn, int argc, Value *argv, TupleObj *kw, DictObj *locals){
    CodeObj *co=fn->code;
    Frame *f=frame_new(co,fn,fn->globals,locals);
    Catch c;
    if(!CATCH_BEGIN(c)){ bind_args(fn,f->fast,argc,argv,kw); CATCH_END(c); }
    else { Value e=mp_catch_exc(&c); free(f); reraise(e); }
    for(int i=0;i<co->ncells;i++) f->cells[i]=mp_cell(co->cellarg[i]>=0 ? f->fast[co->cellarg[i]] : v_undef());
    for(int i=0;i<co->nfrees;i++) f->cells[co->ncells+i]=fn->closure->items[i];
    if(co->flags&(CO_GEN|CO_CORO|CO_ASYNCGEN)) return gen_new(f,fn);
    Value r=eval(f,v_undef(),0,0);
    int dying=0;                                                     /* a generator in a local dies with the call (CPython closes it now) */
    for(int i=0;i<co->nlocals;i++) if(mp_gen_needs_close(f->fast[i])){ dying=1; f->fast[i]=v_undef(); }
    free(f);
    if(dying){ Value keep[1]={r}; (void)keep; mp_gc_collect(); }
    return r;
}
Value mp_call(Value f, int argc, Value *argv, TupleObj *kw){
    if(f.k==V_OBJ){
        Obj *o=f.u.o; Type *t=o->type;
        switch(t->layout){
            case LY_FUNC: return call_function((FuncObj*)o,argc,argv,kw,NULL);
            case LY_NATIVE:{ NativeObj *n=(NativeObj*)o;
                if(n->self.k==V_UNDEF){
                    if(n->is_method && argc>0 && argv[0].k==V_OBJ && argv[0].u.o->type->layout==LY_NUM){   /* int.bit_length(x) of an int subclass */
                        Value a0[8], *a= argc<=8 ? a0 : (Value*)xmalloc(sizeof(Value)*(size_t)argc);
                        memcpy(a,argv,sizeof(Value)*(size_t)argc); a[0]=mp_unbox(a[0]);
                        Value keep=mp_tuple(argc,a); if(a!=a0) free(a);
                        return n->fn(argc,AS_TUPLE(keep)->items,kw); }
                    return n->fn(argc,argv,kw); }
                Value small[8], *a= argc+1<=8 ? small : (Value*)xmalloc(sizeof(Value)*(size_t)(argc+1));
                a[0]=n->self; memcpy(a+1,argv,sizeof(Value)*(size_t)argc);
                Value keep=mp_tuple(argc+1,a);                 /* (collected: the native may call Python) */
                if(a!=small) free(a);
                return n->fn(argc+1,AS_TUPLE(keep)->items,kw); }
            case LY_METHOD:{ MethodObj *m=(MethodObj*)o;
                Value keep=mp_tuple(argc+1,NULL); AS_TUPLE(keep)->items[0]=m->self; memcpy(AS_TUPLE(keep)->items+1,argv,sizeof(Value)*(size_t)argc);
                if(IS(m->func,T_function)) return call_function(AS_FUNC(m->func),argc+1,AS_TUPLE(keep)->items,kw,NULL);
                return mp_call(m->func,argc+1,AS_TUPLE(keep)->items,kw); }
            case LY_TYPE:{ Type *c=(Type*)o;
                if(t!=T_type && (t->flags&TF_HEAP)){                 /* a class of a metaclass that has __call__ */
                    Value m=mp_type_lookup_user(t,"__call__");
                    if(m.k!=V_UNDEF && !IS(m,T_function)) return mp_call(descr_get(m,f,t),argc,argv,kw);   /* (__call__ = dict: not bound) */
                    if(m.k!=V_UNDEF){
                        Value keep=mp_tuple(argc+1,NULL); AS_TUPLE(keep)->items[0]=f; memcpy(AS_TUPLE(keep)->items+1,argv,sizeof(Value)*(size_t)argc);
                        return mp_call(m,argc+1,AS_TUPLE(keep)->items,kw); } }
                if(c->flags&TF_SUBVAL) return subval_call(c,argc,argv,kw);
                if(c->make) return c->make(c,argc,argv,kw);
                return mp_instance_call(c,argc,argv,kw); }
            case LY_STATICMETHOD: return mp_call(((BoxObj*)o)->v,argc,argv,kw);
            default: break;
        }
        if(t->flags&TF_DUNDERS){
            Value m=mp_type_lookup_s(t,"__call__");
            if(m.k!=V_UNDEF && !IS(m,T_function)) return mp_call(descr_get(m,f,t),argc,argv,kw);
            if(m.k!=V_UNDEF){
                Value keep=mp_tuple(argc+1,NULL); AS_TUPLE(keep)->items[0]=f; memcpy(AS_TUPLE(keep)->items+1,argv,sizeof(Value)*(size_t)argc);
                return mp_call(m,argc+1,AS_TUPLE(keep)->items,kw);
            }
        }
    }
    mp_raise_t(E_TypeError,"'%s' object is not callable",mp_type_name(f));
}
Value mp_call0(Value f){ return mp_call(f,0,NULL,NULL); }
Value mp_call1(Value f, Value a){ return mp_call(f,1,&a,NULL); }
Value mp_call2(Value f, Value a, Value b){ Value v[2]={a,b}; return mp_call(f,2,v,NULL); }
int mp_callmethod_opt(Value o, const char *name, int argc, Value *argv, Value *out){
    Value m;
    if(!mp_getattr_opt(o,mp_intern(name),&m)) return 0;
    *out=mp_call(m,argc,argv,NULL);
    return 1;
}
Value mp_callmethod(Value o, const char *name, int argc, Value *argv){
    Value m=mp_getattr_s(o,name);
    return mp_call(m,argc,argv,NULL);
}
Value mp_native(const char *name, NFn fn){
    NativeObj *n=(NativeObj*)mp_alloc(T_native,sizeof(NativeObj)); n->name=name; n->fn=fn; n->self=v_undef(); return v_obj(n);
}

