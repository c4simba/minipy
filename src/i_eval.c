/* ========================= Interpreter: running code =========================
   Exceptions (raised with longjmp to the innermost catch point: a frame's
   eval loop or a native that catches), attributes (the MRO, descriptors:
   functions bind, property, staticmethod, classmethod, super), calls (the
   argument binding and messages of CPython), classes (C3 MRO), generators
   and coroutines (a frame of their own, suspended at yield), and the eval
   loop over i_compile.c's instructions. */

#include "interp.h"
#include <math.h>

#define RECURSION_LIMIT 1000
Value mp_iter_kind(int kind, Value src, Value aux, Value aux2);
int64_t mp_slice_indices(Value sl, int64_t n, int64_t *start, int64_t *stop, int64_t *step);
Value mp_set_copy(SetObj *s, Type *t);
void  mp_set_merge(SetObj *s, SetObj *o);
int   mp_native_send(Value it, Value v, Value *out);   /* i_modules.c: 1 yielded, 0 returned (out: the value) */

/* ---------------------------------------------------------------- exceptions */
void mp_catch_push(Catch *c){ c->prev=mp_ts->catch; c->frame=mp_ts->frame; c->depth=mp_ts->depth; mp_ts->catch=c; }
void mp_catch_pop(Catch *c){ mp_ts->catch=c->prev; }
Value mp_catch_exc(Catch *c){ (void)c; Value e=mp_ts->exc; mp_ts->exc=v_undef(); return e; }

static int is_exc_class(Value v){ return IS(v,T_type) && mp_is_subtype(AS_TYPE(v),E_BaseException); }
int mp_isinstance(Value v, Type *t){ return mp_is_subtype(TYPE(v),t); }
int mp_is_subtype(Type *t, Type *base){
    if(t==base) return 1;
    if(base==T_object) return 1;
    for(int i=0;i<t->nmro;i++) if(t->mro[i]==base) return 1;
    return 0;
}
Value mp_exc_args(Type *t, Value args){
    ExcObj *e=(ExcObj*)mp_alloc(t,sizeof(ExcObj));
    e->args=args; e->cause=v_none(); e->context=v_none(); e->tb=v_undef(); e->notes=v_undef();
    return v_obj(e);
}
Value mp_exc(Type *t, const char *fmt, ...){
    char buf[1024]; va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof buf,fmt,ap); va_end(ap);
    Value m=mp_str(buf);
    return mp_exc_args(t,mp_tuple(1,&m));
}
static MPY_NORETURN void do_raise(Value e){
    Thread *ts=mp_ts;
    ts->exc=e;
    Catch *c=ts->catch;
    if(!c){ mp_print_exception(e); mp_flush_stdout(); exit(1); }
    ts->catch=c->prev; ts->frame=c->frame; ts->depth=c->depth;
    longjmp(c->jb,1);
}
static Value instantiate_exc(Value e){
    if(is_exc_class(e)) e=mp_call0(e);
    if(!mp_isinstance(e,E_BaseException)) e=mp_exc(E_TypeError,"exceptions must derive from BaseException");
    return e;
}
MPY_NORETURN void mp_raise(Value e){
    e=instantiate_exc(e);
    Thread *ts=mp_ts;
    if(ts->nhandled>0){                            /* raised while handling one: its __context__ */
        Value cur=ts->handled[ts->nhandled-1];
        ExcObj *x=AS_EXC(e);
        if(cur.k==V_OBJ && cur.u.o!=e.u.o && IS_NONE(x->context)){
            /* no cycles: cut the chain where it reaches e */
            Value o=cur;
            while(o.k==V_OBJ && mp_isinstance(o,E_BaseException)){ ExcObj *y=AS_EXC(o); if(y->context.k==V_OBJ && y->context.u.o==e.u.o){ y->context=v_none(); break; } o=y->context; }
            x->context=cur;
        }
    }
    do_raise(e);
}
static MPY_NORETURN void reraise(Value e){ do_raise(e); }
MPY_NORETURN void mp_raise_t(Type *t, const char *fmt, ...){
    char buf[1024]; va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof buf,fmt,ap); va_end(ap);
    Value m=mp_str(buf);
    mp_raise(mp_exc_args(t,mp_tuple(1,&m)));
}
int mp_exc_matches(Value e, Value spec){
    if(IS(spec,T_tuple)){ TupleObj *t=AS_TUPLE(spec); for(int64_t i=0;i<t->len;i++) if(mp_exc_matches(e,t->items[i])) return 1; return 0; }
    if(!is_exc_class(spec)) mp_raise_t(E_TypeError,"catching classes that do not inherit from BaseException is not allowed");
    return mp_isinstance(e,AS_TYPE(spec));
}
static void tb_add(Value e, Frame *f){
    if(!mp_isinstance(e,E_BaseException)) return;
    ExcObj *x=AS_EXC(e);
    if(x->tb_frame==(void*)f) return;
    x->tb_frame=f;
    if(x->tb.k==V_UNDEF) x->tb=mp_list(0,NULL);
    int ip= f->ip>0 ? f->ip-1 : 0;
    Value ent[3]={mp_str(f->code->file),v_int(f->code->ncode?f->code->lines[ip<f->code->ncode?ip:f->code->ncode-1]:f->code->firstline),v_obj(f->code->name)};
    mp_list_append(x->tb,mp_tuple(3,ent));
}

/* ---------------------------------------------------------------- the source lines of tracebacks */
static DictObj *sources;
void mp_remember_source(const char *file, const char *src){
    static Value keep;
    if(!sources){ keep=mp_dict(); sources=AS_DICT(keep); mp_gc_add_root(&keep); }
    mp_dict_set(sources,mp_str(file),mp_str(src));
}
static void source_line(SBuf *b, const char *file, int line){
    Value s;
    if(!sources || !mp_dict_get(sources,mp_str(file),&s)) return;
    const char *p=mp_cstr(s);
    for(int l=1;l<line && *p;p++) if(*p=='\n') l++;
    while(*p==' '||*p=='\t') p++;
    const char *e=p; while(*e && *e!='\n' && *e!='\r') e++;
    if(e>p){ sb_puts(b,"    "); sb_put(b,p,e-p); sb_putc(b,'\n'); }
}
/* CPython's layout: exception groups print their members inside a frame of | and +--- */
typedef struct { int depth, need_close; } PCtx;
static void emit_lines(SBuf *b, PCtx *c, const char *text, int64_t n, char margin){
    const char *p=text, *end=text+n;
    while(p<end){ const char *q=p; while(q<end && *q!='\n') q++;
        for(int i=0;i<2*c->depth;i++) sb_putc(b,' ');
        if(c->depth){ sb_putc(b,margin); sb_putc(b,' '); }
        sb_put(b,p,q-p); sb_putc(b,'\n'); p= q<end ? q+1 : q; }
}
static void print_one(SBuf *b, Value e);
static void format_exc(SBuf *b, Value e, PCtx *c, int chain_depth){
    ExcObj *x=AS_EXC(e);
    if(chain_depth<16){
        SBuf m={0};
        if(x->cause.k==V_OBJ){ format_exc(b,x->cause,c,chain_depth+1); sb_puts(&m,"\nThe above exception was the direct cause of the following exception:\n\n"); }
        else if(x->context.k==V_OBJ && !x->suppress){ format_exc(b,x->context,c,chain_depth+1); sb_puts(&m,"\nDuring handling of the above exception, another exception occurred:\n\n"); }
        if(m.n) emit_lines(b,c,m.s,m.n,'|');
        free(m.s);
    }
    Value excs=v_undef();
    if(E_BaseExceptionGroup && mp_isinstance(e,E_BaseExceptionGroup)){ Catch k; if(!CATCH_BEGIN(k)){ excs=mp_getattr_s(e,"exceptions"); CATCH_END(k); } else { mp_catch_exc(&k); excs=v_undef(); } }
    if(!IS(excs,T_tuple)){ SBuf one={0}; print_one(&one,e); emit_lines(b,c,one.s?one.s:"",one.n,'|'); free(one.s); return; }
    int top= c->depth==0; if(top) c->depth++;
    SBuf one={0}; print_one(&one,e);
    if(one.n>=35 && !strncmp(one.s,"Traceback (most recent call last):\n",35)){       /* its own heading, then the frames */
        emit_lines(b,c,"Exception Group Traceback (most recent call last):",50,top?'+':'|');
        emit_lines(b,c,one.s+35,one.n-35,'|'); }
    else emit_lines(b,c,one.s?one.s:"",one.n,'|');
    free(one.s);
    TupleObj *t=AS_TUPLE(excs); int64_t n= t->len<=15 ? t->len : 16;
    c->need_close=0;
    for(int64_t i=0;i<n;i++){ int last= i==n-1; if(last) c->need_close=1;
        for(int k=0;k<2*c->depth;k++) sb_putc(b,' ');
        if(i<15) sb_printf(b,"%s+---------------- %lld ----------------\n",i==0?"+-":"  ",(long long)i+1);
        else sb_printf(b,"  +---------------- ... ----------------\n");
        c->depth++;
        if(i<15) format_exc(b,t->items[i],c,chain_depth);
        else { char m[96]; snprintf(m,sizeof m,"and %lld more exception%s",(long long)(t->len-15),t->len-15>1?"s":""); emit_lines(b,c,m,(int64_t)strlen(m),'|'); }
        if(last && c->need_close){ for(int k=0;k<2*c->depth;k++) sb_putc(b,' '); sb_puts(b,"+------------------------------------\n"); c->need_close=0; }
        c->depth--; }
    if(top) c->depth=0;
}
static void print_one(SBuf *b, Value e){
    ExcObj *x=AS_EXC(e);
    if(x->tb.k==V_OBJ && AS_LIST(x->tb)->len){
        sb_puts(b,"Traceback (most recent call last):\n");
        ListObj *l=AS_LIST(x->tb);
        for(int64_t i=l->len-1;i>=0;i--){
            TupleObj *t=AS_TUPLE(l->items[i]);
            sb_printf(b,"  File \"%s\", line %lld, in %s\n",mp_cstr(t->items[0]),(long long)t->items[1].u.i,mp_cstr(t->items[2]));
            source_line(b,mp_cstr(t->items[0]),(int)t->items[1].u.i);
        }
    }
    Type *t=TYPE(e);
    Value mod=mp_type_lookup_s(t,"__module__");
    if(IS_STR(mod) && strcmp(mp_cstr(mod),"builtins") && strcmp(mp_cstr(mod),"__main__")) sb_printf(b,"%s.",mp_cstr(mod));
    sb_puts(b,t->qualname?t->qualname->s:t->name->s);
    Value s=mp_tostr(e);
    if(AS_STR(s)->len){ sb_puts(b,": "); sb_put(b,AS_STR(s)->s,AS_STR(s)->len); }
    sb_putc(b,'\n');
}
void mp_print_exception(Value e){
    SBuf b={0};
    Catch c;
    if(!CATCH_BEGIN(c)){ PCtx pc={0,0}; format_exc(&b,e,&pc,0); CATCH_END(c); }
    else { mp_catch_exc(&c); sb_puts(&b,"(error printing the exception)\n"); }
    mp_flush_stdout();
    mp_write_err(b.s?b.s:"",b.n);
    free(b.s);
}

/* ---------------------------------------------------------------- attributes */
Value mp_type_lookup(Type *t, Value name){
    Value v;
    for(int i=0;i<t->nmro;i++) if(t->mro[i]->dict && mp_dict_get(t->mro[i]->dict,name,&v)) return v;
    return v_undef();
}
Value mp_type_lookup_s(Type *t, const char *name){ return mp_type_lookup(t,mp_intern(name)); }
static Value bind(Value f, Value self){
    MethodObj *m=(MethodObj*)mp_alloc(T_method,sizeof(MethodObj)); m->func=f; m->self=self; return v_obj(m);
}
static Value bind_native(Value f, Value self){
    NativeObj *n=(NativeObj*)f.u.o, *b=(NativeObj*)mp_alloc(T_native,sizeof(NativeObj));
    b->name=n->name; b->fn=n->fn; b->self=self; b->is_method=1; return v_obj(b);
}
/* the attribute found on the type of `obj`, as seen through obj */
static Value descr_get(Value d, Value obj, Type *t){
    if(d.k!=V_OBJ) return d;
    Type *dt=d.u.o->type;
    if(dt==T_function) return obj.k==V_UNDEF ? d : bind(d,obj);
    if(dt==T_native && ((NativeObj*)d.u.o)->is_method && ((NativeObj*)d.u.o)->self.k==V_UNDEF) return obj.k==V_UNDEF ? d : bind_native(d,obj);
    if(dt==T_staticmethod) return ((BoxObj*)d.u.o)->v;
    if(dt==T_classmethod){ Value f=((BoxObj*)d.u.o)->v; return IS(f,T_native) ? bind_native(f,v_obj(t)) : bind(f,v_obj(t)); }
    if(dt==T_property){ if(obj.k==V_UNDEF) return d; PropertyObj *p=(PropertyObj*)d.u.o;
        if(p->get.k==V_UNDEF || IS_NONE(p->get)) mp_raise_t(E_AttributeError,"property of '%s' object has no getter",t->name->s);
        return mp_call1(p->get,obj); }
    if(dt->flags&TF_DUNDERS){
        Value g=mp_type_lookup_s(dt,"__get__");
        if(g.k!=V_UNDEF){ Value a[3]={d,obj.k==V_UNDEF?v_none():obj,v_obj(t)}; return mp_call(g,3,a,NULL); }
    }
    return d;
}
static int is_data_descr(Value d){
    if(d.k!=V_OBJ) return 0;
    Type *dt=d.u.o->type;
    if(dt==T_property) return 1;
    if(dt->flags&TF_DUNDERS) return mp_type_lookup_s(dt,"__set__").k!=V_UNDEF || mp_type_lookup_s(dt,"__delete__").k!=V_UNDEF;
    return 0;
}
static DictObj *inst_dict(Value o){
    if(o.k!=V_OBJ) return NULL;
    switch(o.u.o->type->layout){
        case LY_INSTANCE: return ((InstObj*)o.u.o)->dict;
        case LY_EXC: return ((ExcObj*)o.u.o)->dict;
        case LY_MODULE: return ((ModuleObj*)o.u.o)->dict;
        case LY_FUNC: return ((FuncObj*)o.u.o)->dict;
        default: return NULL;
    }
}
static Value special_attr(Value o, const char *n, int *found);
int mp_getattr_opt(Value o, Value name, Value *out){
    Type *t=TYPE(o);
    const char *n=mp_cstr(name);
    if(t->layout==LY_TYPE){                        /* an attribute of a class: its MRO, then the metaclass */
        Type *cls=AS_TYPE(o);
        if(!strcmp(n,"__annotations__")){ int found=0; *out=special_attr(o,n,&found); return 1; }   /* its own, evaluated (not inherited) */
        Value d=mp_type_lookup(cls,name);
        if(d.k!=V_UNDEF){ *out=descr_get(d,v_undef(),cls); if(IS(d,T_classmethod)) *out=descr_get(d,v_undef(),cls); return 1; }
        int found=0; Value v=special_attr(o,n,&found);
        if(found){ *out=v; return 1; }
        return 0;
    }
    if(t->layout==LY_SUPER){
        SuperObj *s=(SuperObj*)o.u.o;
        Type *start=s->objtype; int i=0;
        while(i<start->nmro && start->mro[i]!=s->start) i++;
        for(i++;i<start->nmro;i++){ Value v; if(start->mro[i]->dict && mp_dict_get(start->mro[i]->dict,name,&v)){ *out=descr_get(v,IS(s->obj,T_type)&&AS_TYPE(s->obj)==s->objtype?v_undef():s->obj,s->objtype); return 1; } }
        if(!strcmp(n,"__class__")){ *out=v_obj(T_super); return 1; }
        return 0;
    }
    Value d=mp_type_lookup(t,name);
    if(d.k!=V_UNDEF && is_data_descr(d)){ *out=descr_get(d,o,t); return 1; }
    DictObj *dict=inst_dict(o);
    if(dict && mp_dict_get(dict,name,out)) return 1;
    if(d.k!=V_UNDEF){ *out=descr_get(d,o,t); return 1; }
    int found=0; Value v=special_attr(o,n,&found);
    if(found){ *out=v; return 1; }
    if(t->flags&TF_DUNDERS){
        Value ga=mp_type_lookup_s(t,"__getattr__");
        if(ga.k!=V_UNDEF){
            Catch c; Value r;
            if(!CATCH_BEGIN(c)){ r=mp_call2(ga,o,name); CATCH_END(c); *out=r; return 1; }
            Value e=mp_catch_exc(&c);
            if(mp_isinstance(e,E_AttributeError)) return 0;
            mp_raise(e);
        }
    }
    return 0;
}
Value mp_getattr(Value o, Value name){
    Value v;
    if(mp_getattr_opt(o,name,&v)) return v;
    if(IS(o,T_module)) mp_raise_t(E_AttributeError,"module '%s' has no attribute '%s'",((ModuleObj*)o.u.o)->name->s,mp_cstr(name));
    if(IS(o,T_type)) mp_raise_t(E_AttributeError,"type object '%s' has no attribute '%s'",AS_TYPE(o)->name->s,mp_cstr(name));
    mp_raise_t(E_AttributeError,"'%s' object has no attribute '%s'",mp_type_name(o),mp_cstr(name));
}
Value mp_getattr_s(Value o, const char *name){ return mp_getattr(o,mp_intern(name)); }
/* attributes every object of a kind has (not in a type's dict) */
static Value special_attr(Value o, const char *n, int *found){
    *found=1;
    Type *t=TYPE(o);
    if(!strcmp(n,"__class__")) return v_obj(t);
    if(!strcmp(n,"__dict__")){ DictObj *d=inst_dict(o); if(t->layout==LY_TYPE) d=AS_TYPE(o)->dict; if(d) return v_obj(d); }
    switch(t->layout){
        case LY_TYPE:{ Type *c=AS_TYPE(o);
            if(!strcmp(n,"__name__")) return v_obj(c->name);
            if(!strcmp(n,"__qualname__")) return v_obj(c->qualname?c->qualname:c->name);
            if(!strcmp(n,"__bases__")) return v_obj(c->bases);
            if(!strcmp(n,"__base__")) return c->base?v_obj(c->base):v_none();
            if(!strcmp(n,"__mro__")){ Value r=mp_tuple(c->nmro,NULL); for(int i=0;i<c->nmro;i++) AS_TUPLE(r)->items[i]=v_obj(c->mro[i]); return r; }
            if(!strcmp(n,"__module__")){ Value m=mp_type_lookup_s(c,"__module__"); return m.k==V_UNDEF?mp_str("builtins"):m; }
            if(!strcmp(n,"__doc__")) return v_none();
            if(!strcmp(n,"__annotations__")){ Value own, r=mp_dict();          /* its own, evaluated now (CPython 3.14) */
                if(!c->dict || !mp_dict_get_s(c->dict,"__annotations__",&own) || !IS(own,T_dict)) return r;
                Value modname, mod; DictObj *g=NULL;
                if(mp_dict_get_s(c->dict,"__module__",&modname) && IS_STR(modname) && mp_dict_get(mp_modules,modname,&mod)) g=IS(mod,T_module)?((ModuleObj*)mod.u.o)->dict:NULL;
                int64_t pos=0; Value k,v;
                while(mp_dict_next(AS_DICT(own),&pos,&k,&v)) mp_dict_set(AS_DICT(r),k,IS_STR(v)&&g?mp_eval_annotation(mp_cstr(v),g,c->dict):v);
                return r; }
            break; }
        case LY_FUNC:{ FuncObj *f=AS_FUNC(o);
            if(!strcmp(n,"__name__")) return v_obj(f->name);
            if(!strcmp(n,"__qualname__")) return v_obj(f->qualname);
            if(!strcmp(n,"__doc__")) return f->doc;
            if(!strcmp(n,"__module__")) return f->module;
            if(!strcmp(n,"__code__")) return v_obj(f->code);
            if(!strcmp(n,"__defaults__")) return f->defaults?v_obj(f->defaults):v_none();
            if(!strcmp(n,"__kwdefaults__")) return f->kwdefaults?v_obj(f->kwdefaults):v_none();
            if(!strcmp(n,"__globals__")) return v_obj(f->globals);
            if(!strcmp(n,"__closure__")) return f->closure?v_obj(f->closure):v_none();
            if(!strcmp(n,"__minipy_types__")){ Value d=mp_dict(); CodeObj *co=f->code;     /* (minipy.endpoint) the type each parameter names */
                if(co->annots) for(int i=0;i<co->argc+co->kwonly;i++) if(co->annots[i]) mp_dict_set(AS_DICT(d),v_obj(co->varnames[i]),mp_str(co->annots[i]));
                return d; }
            if(!strcmp(n,"__annotations__")){ Value d=mp_dict(); CodeObj *co=f->code;
                for(int i=0;i<co->nann;i++) mp_dict_set(AS_DICT(d),mp_str(co->annname[i]),mp_eval_annotation(co->anntext[i],f->globals,NULL));
                return d; }
            break; }
        case LY_NATIVE: if(!strcmp(n,"__name__")||!strcmp(n,"__qualname__")) return mp_str(((NativeObj*)o.u.o)->name);
            if(!strcmp(n,"__self__")) return ((NativeObj*)o.u.o)->self.k==V_UNDEF?v_none():((NativeObj*)o.u.o)->self;
            if(!strcmp(n,"__doc__")) return v_none();
            break;
        case LY_METHOD:{ MethodObj *m=(MethodObj*)o.u.o;
            if(!strcmp(n,"__self__")) return m->self;
            if(!strcmp(n,"__func__")) return m->func;
            Value v; if(mp_getattr_opt(m->func,mp_intern(n),&v)) return v;
            break; }
        case LY_MODULE: if(!strcmp(n,"__name__")) return v_obj(((ModuleObj*)o.u.o)->name); break;
        case LY_CODE:{ CodeObj *c=(CodeObj*)o.u.o;
            if(!strcmp(n,"co_name")) return v_obj(c->name);
            if(!strcmp(n,"co_qualname")) return v_obj(c->qualname);
            if(!strcmp(n,"co_filename")) return mp_str(c->file);
            if(!strcmp(n,"co_firstlineno")) return v_int(c->firstline);
            if(!strcmp(n,"co_argcount")) return v_int(c->argc);
            if(!strcmp(n,"co_varnames")){ Value r=mp_tuple(c->nlocals,NULL); for(int i=0;i<c->nlocals;i++) AS_TUPLE(r)->items[i]=v_obj(c->varnames[i]); return r; }
            break; }
        case LY_EXC:{ ExcObj *e=AS_EXC(o);
            if(!strcmp(n,"args")) return e->args;
            if(!strcmp(n,"__cause__")) return e->cause;
            if(!strcmp(n,"__context__")) return e->context;
            if(!strcmp(n,"__suppress_context__")) return v_bool(e->suppress);
            if(!strcmp(n,"__traceback__")) return e->tb.k==V_UNDEF?v_none():e->tb;
            if(!strcmp(n,"__notes__") && e->notes.k!=V_UNDEF) return e->notes;
            if(mp_isinstance(o,E_StopIteration) && !strcmp(n,"value")){ TupleObj *a=AS_TUPLE(e->args); return a->len?a->items[0]:v_none(); }
            if(mp_isinstance(o,E_SystemExit) && !strcmp(n,"code")){ TupleObj *a=AS_TUPLE(e->args); return a->len==0?v_none():a->len==1?a->items[0]:e->args; }
            if(mp_isinstance(o,E_KeyError) && !strcmp(n,"key")){ TupleObj *a=AS_TUPLE(e->args); return a->len?a->items[0]:v_none(); }
            if(mp_isinstance(o,E_OSError)){ TupleObj *a=AS_TUPLE(e->args);
                if(!strcmp(n,"errno")) return a->len>=2?a->items[0]:v_none();
                if(!strcmp(n,"strerror")) return a->len>=2?a->items[1]:v_none();
                if(!strcmp(n,"filename")) return a->len>=3?a->items[2]:v_none(); }
            if(mp_isinstance(o,E_ImportError) && (!strcmp(n,"name")||!strcmp(n,"path"))) return v_none();
            break; }
        case LY_GEN:{ GenObj *g=(GenObj*)o.u.o;
            if(!strcmp(n,"__name__")) return v_obj(g->name);
            if(!strcmp(n,"__qualname__")) return v_obj(g->qualname);
            if(!strcmp(n,"gi_running")||!strcmp(n,"cr_running")) return v_bool(g->running);
            if(!strcmp(n,"gi_frame")||!strcmp(n,"cr_frame")) return g->f && g->f->state!=FS_DONE ? v_bool(1) : v_none();
            break; }
        case LY_PROPERTY:{ PropertyObj *p=(PropertyObj*)o.u.o;
            if(!strcmp(n,"fget")) return p->get.k==V_UNDEF?v_none():p->get;
            if(!strcmp(n,"fset")) return p->set.k==V_UNDEF?v_none():p->set;
            if(!strcmp(n,"fdel")) return p->del.k==V_UNDEF?v_none():p->del;
            if(!strcmp(n,"__doc__")) return p->doc.k==V_UNDEF?v_none():p->doc;
            break; }
        case LY_STATICMETHOD: case LY_CLASSMETHOD: if(!strcmp(n,"__func__")) return ((BoxObj*)o.u.o)->v; break;
        case LY_COMPLEX:{ ComplexObj *c=(ComplexObj*)o.u.o; if(!strcmp(n,"real")) return v_float(c->re); if(!strcmp(n,"imag")) return v_float(c->im); break; }
        case LY_RANGE:{ RangeObj *r=(RangeObj*)o.u.o; if(!strcmp(n,"start")) return v_int(r->start); if(!strcmp(n,"stop")) return v_int(r->stop); if(!strcmp(n,"step")) return v_int(r->step); break; }
        case LY_SLICE:{ SliceObj *s=(SliceObj*)o.u.o; if(!strcmp(n,"start")) return s->start; if(!strcmp(n,"stop")) return s->stop; if(!strcmp(n,"step")) return s->step; break; }
        case LY_FILE:{ FileObj *f=(FileObj*)o.u.o; if(!strcmp(n,"name")) return f->name; if(!strcmp(n,"closed")) return v_bool(f->closed); if(!strcmp(n,"mode")){ char m[3]={(char)f->mode,f->binary?'b':0,0}; return mp_str(m); } break; }
        default: break;
    }
    if(o.k==V_INT || o.k==V_BOOL){ if(!strcmp(n,"real")||!strcmp(n,"numerator")) return v_int(o.u.i); if(!strcmp(n,"imag")) return v_int(0); if(!strcmp(n,"denominator")) return v_int(1); }
    if(o.k==V_FLOAT){ if(!strcmp(n,"real")) return o; if(!strcmp(n,"imag")) return v_float(0); }
    *found=0;
    return v_undef();
}
void mp_setattr(Value o, Value name, Value v){
    Type *t=TYPE(o);
    if(t->layout==LY_TYPE){
        Type *c=AS_TYPE(o);
        if(c->flags&TF_BUILTIN) mp_raise_t(E_TypeError,"cannot set '%s' attribute of immutable type '%s'",mp_cstr(name),c->name->s);
        mp_dict_set(c->dict,name,v); return;
    }
    Value d=mp_type_lookup(t,name);
    if(d.k==V_OBJ){
        if(IS(d,T_property)){ PropertyObj *p=(PropertyObj*)d.u.o;
            if(p->set.k==V_UNDEF || IS_NONE(p->set)) mp_raise_t(E_AttributeError,"property '%s' of '%s' object has no setter",mp_cstr(name),t->name->s);
            mp_call2(p->set,o,v); return; }
        if(d.u.o->type->flags&TF_DUNDERS){ Value s=mp_type_lookup_s(d.u.o->type,"__set__"); if(s.k!=V_UNDEF){ Value a[3]={d,o,v}; mp_call(s,3,a,NULL); return; } }
    }
    if(t->flags&TF_DUNDERS){
        Value sa=mp_type_lookup_s(t,"__setattr__");
        if(sa.k!=V_UNDEF && !IS(sa,T_native)){ Value a[3]={o,name,v}; mp_call(sa,3,a,NULL); return; }
    }
    DictObj *dict=inst_dict(o);
    if(dict){ mp_dict_set(dict,name,v); return; }
    if(t->layout==LY_FUNC){ FuncObj *f=AS_FUNC(o);
        if(mp_str_eq_c(name,"__name__")){ f->name=AS_STR(v); return; }
        if(mp_str_eq_c(name,"__qualname__")){ f->qualname=AS_STR(v); return; }
        if(mp_str_eq_c(name,"__doc__")){ f->doc=v; return; }
        if(mp_str_eq_c(name,"__module__")){ f->module=v; return; }
        if(!f->dict) f->dict=AS_DICT(mp_dict());
        mp_dict_set(f->dict,name,v); return; }
    if(t->layout==LY_EXC){ ExcObj *e=AS_EXC(o);
        if(mp_str_eq_c(name,"__cause__")){ e->cause=v; e->suppress=1; return; }
        if(mp_str_eq_c(name,"__context__")){ e->context=v; return; }
        if(mp_str_eq_c(name,"__traceback__")){ e->tb=v; return; }
        if(mp_str_eq_c(name,"args")){ e->args=IS(v,T_tuple)?v:mp_tuple(1,&v); return; }
        if(!e->dict) e->dict=AS_DICT(mp_dict());
        mp_dict_set(e->dict,name,v); return; }
    if(d.k!=V_UNDEF) mp_raise_t(E_AttributeError,"'%s' object attribute '%s' is read-only",t->name->s,mp_cstr(name));
    mp_raise_t(E_AttributeError,"'%s' object has no attribute '%s' and no __dict__ for setting new attributes",t->name->s,mp_cstr(name));
}
void mp_delattr(Value o, Value name){
    Type *t=TYPE(o);
    if(t->layout==LY_TYPE){ if(!mp_dict_del(AS_TYPE(o)->dict,name)) mp_raise_t(E_AttributeError,"type object '%s' has no attribute '%s'",AS_TYPE(o)->name->s,mp_cstr(name)); return; }
    Value d=mp_type_lookup(t,name);
    if(IS(d,T_property)){ PropertyObj *p=(PropertyObj*)d.u.o; if(p->del.k==V_UNDEF||IS_NONE(p->del)) mp_raise_t(E_AttributeError,"property '%s' of '%s' object has no deleter",mp_cstr(name),t->name->s); mp_call1(p->del,o); return; }
    if(t->flags&TF_DUNDERS){ Value da=mp_type_lookup_s(t,"__delattr__"); if(da.k!=V_UNDEF && !IS(da,T_native)){ mp_call2(da,o,name); return; } }
    DictObj *dict=inst_dict(o);
    if(dict && mp_dict_del(dict,name)) return;
    mp_raise_t(E_AttributeError,"'%s' object has no attribute '%s'",t->name->s,mp_cstr(name));
}

/* ---------------------------------------------------------------- frames */
static Frame *frame_new(CodeObj *co, FuncObj *fn, DictObj *globals, DictObj *locals){
    size_t nv=(size_t)(co->nlocals+co->ncells+co->nfrees+co->stacksize);
    size_t sz=sizeof(Frame)+sizeof(Value)*nv+sizeof(Block)*(size_t)co->nblocks;
    Frame *f=(Frame*)xmalloc(sz); memset(f,0,sz);
    f->fast=(Value*)(f+1); f->cells=f->fast+co->nlocals; f->stack=f->cells+co->ncells+co->nfrees; f->blocks=(Block*)(f->stack+co->stacksize);
    f->code=co; f->func=fn; f->globals=globals; f->locals=locals; f->builtins=mp_builtins;
    f->yf=v_undef();
    return f;
}
static Value eval(Frame *f, Value sent, int resume, int throwing);
Value mp_instance_call(Type *t, int argc, Value *argv, TupleObj *kw);

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
static Value gen_new(Frame *f, FuncObj *fn);
static Value call_function(FuncObj *fn, int argc, Value *argv, TupleObj *kw, DictObj *locals){
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
                if(n->self.k==V_UNDEF) return n->fn(argc,argv,kw);
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
                if(c->make) return c->make(c,argc,argv,kw);
                return mp_instance_call(c,argc,argv,kw); }
            case LY_STATICMETHOD: return mp_call(((BoxObj*)o)->v,argc,argv,kw);
            default: break;
        }
        if(t->flags&TF_DUNDERS){
            Value m=mp_type_lookup_s(t,"__call__");
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

/* ---------------------------------------------------------------- classes */
Value mp_instance(Type *t){
    if(t->layout==LY_EXC){ Value e=mp_exc_args(t,mp_tuple(0,NULL)); return e; }
    InstObj *o=(InstObj*)mp_alloc(t,sizeof(InstObj));
    o->dict=AS_DICT(mp_dict());
    return v_obj(o);
}
Value mp_instance_call(Type *t, int argc, Value *argv, TupleObj *kw){
    if(mp_is_subtype(t,E_BaseExceptionGroup)) mp_excgroup(NULL);     /* (its methods, the first time) */
    Value nw=mp_type_lookup_s(t,"__new__");
    Value self;
    if(nw.k!=V_UNDEF && !IS(nw,T_native)){
        Value keep=mp_tuple(argc+1,NULL); AS_TUPLE(keep)->items[0]=v_obj(t); memcpy(AS_TUPLE(keep)->items+1,argv,sizeof(Value)*(size_t)argc);
        Value fn= IS(nw,T_staticmethod) ? ((BoxObj*)nw.u.o)->v : nw;
        self=mp_call(fn,argc+1,AS_TUPLE(keep)->items,kw);
        if(!mp_isinstance(self,t)) return self;
    } else {
        self=mp_instance(t);
        if(t->layout==LY_EXC){ int np=argc-(kw?(int)kw->len:0); AS_EXC(self)->args=mp_tuple(np,argv); }
    }
    Value init=mp_type_lookup_s(t,"__init__");
    if(init.k!=V_UNDEF && !IS(init,T_native)){
        Value keep=mp_tuple(argc+1,NULL); AS_TUPLE(keep)->items[0]=self; memcpy(AS_TUPLE(keep)->items+1,argv,sizeof(Value)*(size_t)argc);
        Value r=mp_call(init,argc+1,AS_TUPLE(keep)->items,kw);
        if(!IS_NONE(r)) mp_raise_t(E_TypeError,"__init__() should return None, not '%s'",mp_type_name(r));
    } else if(init.k!=V_UNDEF && IS(init,T_native)){
        Value keep=mp_tuple(argc+1,NULL); AS_TUPLE(keep)->items[0]=self; memcpy(AS_TUPLE(keep)->items+1,argv,sizeof(Value)*(size_t)argc);
        mp_call(init,argc+1,AS_TUPLE(keep)->items,kw);
    } else if(argc && t->layout!=LY_EXC && (nw.k==V_UNDEF || IS(nw,T_native))) mp_raise_t(E_TypeError,"%s() takes no arguments",t->name->s);
    return self;
}
/* C3 linearization */
static int in_tail(Type **l, int n, Type *t){ for(int i=1;i<n;i++) if(l[i]==t) return 1; return 0; }
static void compute_mro(Type *t){
    TupleObj *b=t->bases; int nb=(int)b->len;
    Type ***seqs=(Type***)xmalloc(sizeof(Type**)*(size_t)(nb+1)); int *lens=(int*)xmalloc(sizeof(int)*(size_t)(nb+1)), *pos=(int*)xmalloc(sizeof(int)*(size_t)(nb+1));
    for(int i=0;i<nb;i++){ Type *x=AS_TYPE(b->items[i]); seqs[i]=x->mro; lens[i]=x->nmro; pos[i]=0; }
    seqs[nb]=(Type**)xmalloc(sizeof(Type*)*(size_t)(nb>0?nb:1)); for(int i=0;i<nb;i++) seqs[nb][i]=AS_TYPE(b->items[i]); lens[nb]=nb; pos[nb]=0;
    int cap=16, n=0; Type **out=(Type**)xmalloc(sizeof(Type*)*(size_t)cap);
    out[n++]=t;
    for(;;){
        int any=0;
        for(int i=0;i<=nb;i++) if(pos[i]<lens[i]) any=1;
        if(!any) break;
        Type *cand=NULL;
        for(int i=0;i<=nb && !cand;i++){
            if(pos[i]>=lens[i]) continue;
            Type *c=seqs[i][pos[i]]; int bad=0;
            for(int j=0;j<=nb;j++) if(pos[j]<lens[j] && in_tail(seqs[j]+pos[j],lens[j]-pos[j],c)) bad=1;
            if(!bad) cand=c;
        }
        if(!cand){ free(out); free(seqs[nb]); free(seqs); free(lens); free(pos); mp_raise_t(E_TypeError,"Cannot create a consistent method resolution order (MRO) for bases"); }
        if(n==cap){ cap*=2; out=(Type**)xrealloc(out,sizeof(Type*)*(size_t)cap); }
        out[n++]=cand;
        for(int i=0;i<=nb;i++) if(pos[i]<lens[i] && seqs[i][pos[i]]==cand) pos[i]++;
    }
    free(t->mro); t->mro=out; t->nmro=n;
    free(seqs[nb]); free(seqs); free(lens); free(pos);
}
void mp_type_finish(Type *t){ compute_mro(t); }
Value mp_make_class(Value name, Value bases, DictObj *ns){
    TupleObj *b=AS_TUPLE(bases);
    for(int64_t i=0;i<b->len;i++){
        if(!IS(b->items[i],T_type)) mp_raise_t(E_TypeError,"bases must be types");
        Type *x=AS_TYPE(b->items[i]);
        if(!(x->flags&TF_BASETYPE)) mp_raise_t(E_TypeError,"type '%s' is not an acceptable base type",x->name->s);
    }
    if(b->len==0){ Value o=v_obj(T_object); bases=mp_tuple(1,&o); b=AS_TUPLE(bases); }
    Type *t=(Type*)mp_alloc(T_type,sizeof(Type));
    t->name=AS_STR(name); t->bases=b; t->dict=ns; t->flags=TF_BASETYPE|TF_HEAP|TF_DUNDERS;
    /* the layout: the most derived non-object one among the bases */
    Type *solid=T_object;
    for(int64_t i=0;i<b->len;i++){
        Type *x=AS_TYPE(b->items[i]), *s=x;
        while(s && s->layout==LY_INSTANCE && s!=T_object && (s->flags&TF_HEAP)) s=s->base;
        if(!s) s=T_object;
        if(s!=T_object){
            if(solid==T_object || mp_is_subtype(s,solid)) solid=s;
            else if(!mp_is_subtype(solid,s)) mp_raise_t(E_TypeError,"multiple bases have instance lay-out conflict");
        }
    }
    t->base=AS_TYPE(b->items[0]);
    t->layout= solid==T_object ? LY_INSTANCE : solid->layout;
    if(t->layout!=LY_INSTANCE && t->layout!=LY_EXC) t->make=solid->make;
    compute_mro(t);
    Value q;
    if(mp_dict_get_s(ns,"__qualname__",&q) && IS_STR(q)){ t->qualname=AS_STR(q); mp_dict_del(ns,mp_intern("__qualname__")); }
    else t->qualname=t->name;
    /* __init_subclass__ of the bases, __set_name__ of the attributes */
    int64_t pos=0; Value k, v;
    while(mp_dict_next(ns,&pos,&k,&v)){
        if(v.k==V_OBJ && (v.u.o->type->flags&TF_DUNDERS)){
            Value sn=mp_type_lookup_s(v.u.o->type,"__set_name__");
            if(sn.k!=V_UNDEF){ Value a[3]={v,v_obj(t),k}; mp_call(sn,3,a,NULL); }
        }
    }
    for(int i=1;i<t->nmro;i++){
        Value isc; if(t->mro[i]->dict && mp_dict_get_s(t->mro[i]->dict,"__init_subclass__",&isc)){
            Value f= IS(isc,T_classmethod) ? ((BoxObj*)isc.u.o)->v : isc;
            if(IS(f,T_function)) mp_call1(f,v_obj(t));
            break;
        }
    }
    return v_obj(t);
}
Value mp_new_type(const char *name, Type *base, Layout layout, int flags){
    Type *t=(Type*)mp_alloc(T_type,sizeof(Type));
    t->name=AS_STR(mp_intern(name)); t->qualname=t->name; t->base=base; t->layout=layout; t->flags=flags|TF_BUILTIN;
    t->dict=AS_DICT(mp_dict());
    if(base){ Value bv=v_obj(base); t->bases=AS_TUPLE(mp_tuple(1,&bv)); if(base->make && layout==base->layout) t->make=base->make; }
    else t->bases=AS_TUPLE(mp_tuple(0,NULL));
    compute_mro(t);
    return v_obj(t);
}
void mp_type_add(Type *t, const char *name, NFn fn){
    Value n=mp_native(name,fn); ((NativeObj*)n.u.o)->is_method=1;
    mp_dict_set(t->dict,mp_intern(name),n);
}

/* ---------------------------------------------------------------- generators, coroutines */
static Value gen_new(Frame *f, FuncObj *fn){
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
static void close_loop_gens(Value *v, int n){
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
static int send_into(Value recv, Value v, Value *out);
static Value ag_wrap(Value v){ BoxObj *b=(BoxObj*)mp_alloc(T_agwrap,sizeof(BoxObj)); b->v=v; return v_obj(b); }
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
static int send_into(Value recv, Value v, Value *out){
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

/* ---------------------------------------------------------------- the eval loop */
#define PUSH(v) do{ Value pv_=(v); f->stack[f->sp++]=pv_; }while(0)
#define POP()   (f->stack[--f->sp])
#define TOP()   (f->stack[f->sp-1])
#define SECOND() (f->stack[f->sp-2])
static MPY_NORETURN void unbound_local(CodeObj *co, int i){ mp_raise_t(E_UnboundLocalError,"cannot access local variable '%s' where it is not associated with a value",co->varnames[i]->s); }
static Value load_global(Frame *f, StrObj *name){
    Value v;
    if(mp_dict_get(f->globals,v_obj(name),&v)) return v;
    if(mp_dict_get(f->builtins,v_obj(name),&v)) return v;
    mp_raise_t(E_NameError,"name '%s' is not defined",name->s);
}
static const char *cellname(CodeObj *co, int i){ return i<co->ncells ? co->cellnames[i]->s : co->freenames[i-co->ncells]->s; }
static Value unpack_list(Value seq, int64_t want, int ex){
    if(IS(seq,T_tuple) || IS(seq,T_list)) return seq;
    Value l=mp_list(0,NULL), it=mp_iter(seq), x;
    while(mp_next(it,&x)){ mp_list_append(l,x); if(!ex && AS_LIST(l)->len>want) break; }
    return l;
}
static int64_t seq_len(Value s){ return IS(s,T_tuple) ? AS_TUPLE(s)->len : AS_LIST(s)->len; }
static Value *seq_items(Value s){ return IS(s,T_tuple) ? AS_TUPLE(s)->items : AS_LIST(s)->items; }
static TupleObj *kwnames_of(DictObj *d, Value *vals, int *n){
    int64_t pos=0; Value k, v; int i=0;
    Value names=mp_tuple(d->used,NULL);
    while(mp_dict_next(d,&pos,&k,&v)){
        if(!IS_STR(k)) mp_raise_t(E_TypeError,"keywords must be strings");
        AS_TUPLE(names)->items[i]=k; vals[i]=v; i++;
    }
    *n=i;
    return AS_TUPLE(names);
}
static Value call_ex(Value fn, Value args, Value kwargs){
    TupleObj *a=AS_TUPLE(args);
    if(kwargs.k==V_UNDEF || AS_DICT(kwargs)->used==0) return mp_call(fn,(int)a->len,a->items,NULL);
    DictObj *d=AS_DICT(kwargs);
    Value all=mp_tuple(a->len+d->used,NULL);
    memcpy(AS_TUPLE(all)->items,a->items,sizeof(Value)*(size_t)a->len);
    int nk; TupleObj *kn=kwnames_of(d,AS_TUPLE(all)->items+a->len,&nk);
    return mp_call(fn,(int)(a->len+nk),AS_TUPLE(all)->items,kn);
}
static Value make_function(Frame *f, int flags){
    Value code=POP();
    FuncObj *fn=(FuncObj*)mp_alloc(T_function,sizeof(FuncObj));
    Value fv=v_obj(fn);
    if(flags&4) fn->closure=AS_TUPLE(POP());
    if(flags&2) fn->kwdefaults=AS_DICT(POP());
    if(flags&1) fn->defaults=AS_TUPLE(POP());
    CodeObj *co=(CodeObj*)code.u.o;
    fn->code=co; fn->globals=f->globals; fn->name=co->name; fn->qualname=co->qualname; fn->doc=co->doc;
    Value m; fn->module= mp_dict_get_s(f->globals,"__name__",&m) ? m : v_none();
    return fv;
}
static Value build_class(Frame *f, int arg){
    Value body=POP();
    int nb=arg&0xffff, haskw=(arg>>16)&1;
    Value kw= haskw ? POP() : v_undef();
    Value bases=mp_tuple(nb,f->stack+f->sp-nb); f->sp-=nb;
    Value name=POP();
    if(haskw){
        int64_t pos=0; Value k, v;
        while(mp_dict_next(AS_DICT(kw),&pos,&k,&v)){
            if(mp_str_eq_c(k,"metaclass")){
                if(IS(v,T_type) && (AS_TYPE(v)==T_type)) continue;
                mp_raise_t(E_TypeError,"metaclasses are not supported");
            }
        }
    }
    Value ns=mp_dict();
    FuncObj *bf=AS_FUNC(body);
    Value cell=call_function(bf,0,NULL,NULL,AS_DICT(ns));
    Value cls=mp_make_class(name,bases,AS_DICT(ns));
    if(haskw){
        /* class keywords other than metaclass go to __init_subclass__ (handled there): ignore */
    }
    if(IS(cell,T_cell)) ((CellObj*)cell.u.o)->v=cls;
    return cls;
}
/* a native the eval loop calls for `with`: the special method of the type, bound */
static void special_pair(Value o, const char *en, const char *ex, const char *proto, Value *enter, Value *exitf){
    Value m=mp_type_lookup_s(TYPE(o),en), x=mp_type_lookup_s(TYPE(o),ex);
    const char *miss = x.k==V_UNDEF ? ex : m.k==V_UNDEF ? en : NULL;
    if(miss) mp_raise_t(E_TypeError,"'%s' object does not support the %s protocol (missed %s method)",mp_type_name(o),proto,miss);
    *enter=descr_get(m,o,TYPE(o)); *exitf=descr_get(x,o,TYPE(o));
}
static Value exc_type_of(Value e){ return v_obj(TYPE(e)); }
static Value exc_tb_of(Value e){ ExcObj *x=AS_EXC(e); return x->tb.k==V_UNDEF?v_none():x->tb; }
static int match_seq_ok(Value v){
    Type *t=TYPE(v);
    if(t->layout==LY_LIST || t->layout==LY_TUPLE || t->layout==LY_RANGE) return 1;
    if(t->layout==LY_STR || t->layout==LY_BYTES || t->layout==LY_DICT) return 0;
    if(t->flags&TF_DUNDERS) return mp_type_lookup_s(t,"__getitem__").k!=V_UNDEF && mp_type_lookup_s(t,"__len__").k!=V_UNDEF && mp_type_lookup_s(t,"keys").k==V_UNDEF;
    return 0;
}

static Value eval(Frame *f, Value sent, int resume, int throwing){
    Thread *ts=mp_ts;
    CodeObj *co=f->code;
    f->back=ts->frame; ts->frame=f;
    if(++ts->depth>RECURSION_LIMIT){ ts->depth--; ts->frame=f->back; mp_raise_t(E_RecursionError,"maximum recursion depth exceeded"); }
    f->state=FS_RUNNING;
    f->hbase=ts->nhandled;
    if(f->nsaved){ for(int i=0;i<f->nsaved;i++) ts->handled[ts->nhandled++]=f->saved_exc[i]; f->nsaved=0; }
    if(resume && !throwing && !f->nopush) PUSH(sent);
    f->nopush=0;
    Catch c;
    int ticks=0;
    volatile int pending_throw=throwing;
  arm:
    mp_catch_push(&c);
    if(setjmp(c.jb)){
        /* an exception reached this frame */
        Value e=ts->exc; ts->exc=v_undef();
        ts->frame=f;
        tb_add(e,f);
        if(f->nblocks>0){
            Block b=f->blocks[--f->nblocks];
            if(f->sp>b.sp) close_loop_gens(f->stack+b.sp,f->sp-b.sp);
            f->sp=b.sp; ts->nhandled=b.hdepth;
            PUSH(e); f->ip=b.handler;
            goto arm;
        }
        ts->nhandled=f->hbase; ts->depth--; ts->frame=f->back; f->back=NULL; f->state=FS_DONE;
        if(f->sp>0){ int n=f->sp; f->sp=0; close_loop_gens(f->stack,n); }
        reraise(e);
    }
    if(pending_throw){ pending_throw=0; Value e=sent; mp_raise(e); }
    for(;;){
        if(++ticks>=64){
            ticks=0;
            mp_gc_maybe();
            if(mp_nthreads>1){ Thread *self=ts; mp_gil_release(); mpy_thread_yield(); mp_gil_acquire(self); }
        }
        uint32_t ins=co->code[f->ip++];
        int op=(int)(ins&0xff), arg=(int)(ins>>8);
        switch(op){
            case I_NOP: break;
            case I_POP: f->sp--; if(arg) close_loop_gens(f->stack+f->sp,1); break;
            case I_DUP: PUSH(TOP()); break;
            case I_DUP2:{ Value a=SECOND(), b=TOP(); PUSH(a); PUSH(b); break; }
            case I_ROT2:{ Value t=TOP(); TOP()=SECOND(); SECOND()=t; break; }
            case I_ROT3:{ Value t=f->stack[f->sp-1]; f->stack[f->sp-1]=f->stack[f->sp-2]; f->stack[f->sp-2]=f->stack[f->sp-3]; f->stack[f->sp-3]=t; break; }
            case I_ROT4:{ Value t=f->stack[f->sp-1]; f->stack[f->sp-1]=f->stack[f->sp-2]; f->stack[f->sp-2]=f->stack[f->sp-3]; f->stack[f->sp-3]=f->stack[f->sp-4]; f->stack[f->sp-4]=t; break; }
            case I_CONST: PUSH(co->consts[arg]); break;
            case I_NONE: PUSH(v_none()); break;
            case I_LOAD_FAST:{ Value v=f->fast[arg]; if(v.k==V_UNDEF) unbound_local(co,arg); PUSH(v); break; }
            case I_STORE_FAST: f->fast[arg]=POP(); break;
            case I_DEL_FAST: if(f->fast[arg].k==V_UNDEF) unbound_local(co,arg); f->fast[arg]=v_undef(); break;
            case I_LOAD_DEREF:{ Value v=((CellObj*)f->cells[arg].u.o)->v;
                if(v.k==V_UNDEF){ if(arg<co->ncells) mp_raise_t(E_UnboundLocalError,"cannot access local variable '%s' where it is not associated with a value",cellname(co,arg));
                    mp_raise_t(E_NameError,"cannot access free variable '%s' where it is not associated with a value in enclosing scope",cellname(co,arg)); }
                PUSH(v); break; }
            case I_STORE_DEREF: ((CellObj*)f->cells[arg].u.o)->v=POP(); break;
            case I_DEL_DEREF: ((CellObj*)f->cells[arg].u.o)->v=v_undef(); break;
            case I_LOAD_CELL: PUSH(f->cells[arg]); break;
            case I_LOAD_CLASSDEREF:{ Value v; const char *n=cellname(co,arg);
                if(f->locals && mp_dict_get(f->locals,mp_intern(n),&v)){ PUSH(v); break; }
                v=((CellObj*)f->cells[arg].u.o)->v;
                if(v.k==V_UNDEF) mp_raise_t(E_NameError,"cannot access free variable '%s' where it is not associated with a value in enclosing scope",n);
                PUSH(v); break; }
            case I_LOAD_GLOBAL: PUSH(load_global(f,co->names[arg])); break;
            case I_STORE_GLOBAL: mp_dict_set(f->globals,v_obj(co->names[arg]),POP()); break;
            case I_DEL_GLOBAL: if(!mp_dict_del(f->globals,v_obj(co->names[arg]))) mp_raise_t(E_NameError,"name '%s' is not defined",co->names[arg]->s); break;
            case I_LOAD_NAME:{ Value v; Value nm=v_obj(co->names[arg]);
                if(f->locals && mp_dict_get(f->locals,nm,&v)){ PUSH(v); break; }
                PUSH(load_global(f,co->names[arg])); break; }
            case I_STORE_NAME: mp_dict_set(f->locals?f->locals:f->globals,v_obj(co->names[arg]),POP()); break;
            case I_DEL_NAME: if(!mp_dict_del(f->locals?f->locals:f->globals,v_obj(co->names[arg]))) mp_raise_t(E_NameError,"name '%s' is not defined",co->names[arg]->s); break;
            case I_LOAD_ATTR: TOP()=mp_getattr(TOP(),v_obj(co->names[arg])); break;
            case I_STORE_ATTR:{ Value o=POP(), v=POP(); mp_setattr(o,v_obj(co->names[arg]),v); break; }
            case I_DEL_ATTR:{ Value o=POP(); mp_delattr(o,v_obj(co->names[arg])); break; }
            case I_LOAD_METHOD:{
                Value o=TOP(); Value name=v_obj(co->names[arg]); Type *t=TYPE(o);
                if(t->layout!=LY_TYPE && t->layout!=LY_SUPER && t->layout!=LY_MODULE){
                    Value d=mp_type_lookup(t,name);
                    if(d.k==V_OBJ && (IS(d,T_function) || (IS(d,T_native) && ((NativeObj*)d.u.o)->is_method && ((NativeObj*)d.u.o)->self.k==V_UNDEF))){
                        DictObj *dict=inst_dict(o); Value shadow;
                        if(!dict || !mp_dict_get(dict,name,&shadow)){ TOP()=d; PUSH(o); break; }
                    }
                }
                TOP()=v_undef(); PUSH(mp_getattr(o,name));
                break; }
            case I_CALL_METHOD:{
                int n=arg&0xffff, haskw=(arg>>16)&1;
                TupleObj *kw= haskw ? AS_TUPLE(POP()) : NULL;
                Value *args=f->stack+f->sp-n;
                Value m=f->stack[f->sp-n-2], self=f->stack[f->sp-n-1];
                Value r;
                if(m.k==V_UNDEF) r=mp_call(self,n,args,kw);
                else if(IS(m,T_function)) r=call_function(AS_FUNC(m),n+1,args-1,kw,NULL);
                else r=((NativeObj*)m.u.o)->fn(n+1,args-1,kw);
                f->sp-=n+2; PUSH(r); break; }
            case I_SUBSCR:{ Value k=POP(); TOP()=mp_getitem(TOP(),k); break; }
            case I_STORE_SUBSCR:{ Value k=POP(), o=POP(), v=POP(); mp_setitem(o,k,v); break; }
            case I_DEL_SUBSCR:{ Value k=POP(), o=POP(); mp_delitem(o,k); break; }
            case I_BINOP:{ Value b=POP(), a=TOP();
                if(a.k==V_INT && b.k==V_INT){
                    int64_t r;
                    if(arg==OP_Add && !__builtin_add_overflow(a.u.i,b.u.i,&r)){ TOP()=v_int(r); break; }
                    if(arg==OP_Sub && !__builtin_sub_overflow(a.u.i,b.u.i,&r)){ TOP()=v_int(r); break; }
                }
                TOP()=mp_binop(arg,a,b); break; }
            case I_INPLACE:{ Value b=POP(), a=TOP();
                if(a.k==V_INT && b.k==V_INT){
                    int64_t r;
                    if(arg==OP_Add && !__builtin_add_overflow(a.u.i,b.u.i,&r)){ TOP()=v_int(r); break; }
                    if(arg==OP_Sub && !__builtin_sub_overflow(a.u.i,b.u.i,&r)){ TOP()=v_int(r); break; }
                }
                TOP()=mp_inplace(arg,a,b); break; }
            case I_UNARY: TOP()=mp_unary(arg,TOP()); break;
            case I_NOT: TOP()=v_bool(!mp_truth(TOP())); break;
            case I_TRUTH: TOP()=v_bool(mp_truth(TOP())); break;
            case I_COMPARE:{ Value b=POP(), a=TOP();
                if(a.k==V_INT && b.k==V_INT){
                    int64_t x=a.u.i, y=b.u.i; int r=-1;
                    switch(arg){ case OP_Lt: r=x<y; break; case OP_LtE: r=x<=y; break; case OP_Gt: r=x>y; break; case OP_GtE: r=x>=y; break; case OP_Eq: r=x==y; break; case OP_NotEq: r=x!=y; break; default: break; }
                    if(r>=0){ TOP()=v_bool(r); break; }
                }
                TOP()=mp_compare(arg,a,b); break; }
            case I_JUMP: f->ip=arg; break;
            case I_JUMP_IF_FALSE:{ Value v=POP(); if(v.k==V_BOOL ? !v.u.i : !mp_truth(v)) f->ip=arg; break; }
            case I_JUMP_IF_TRUE:{ Value v=POP(); if(v.k==V_BOOL ? v.u.i : mp_truth(v)) f->ip=arg; break; }
            case I_JUMP_IF_FALSE_KEEP: if(!mp_truth(TOP())) f->ip=arg; break;
            case I_JUMP_IF_TRUE_KEEP: if(mp_truth(TOP())) f->ip=arg; break;
            case I_BUILD_TUPLE:{ Value t=mp_tuple(arg,f->stack+f->sp-arg); f->sp-=arg; PUSH(t); break; }
            case I_BUILD_LIST:{ Value l=mp_list(arg,f->stack+f->sp-arg); f->sp-=arg; PUSH(l); break; }
            case I_BUILD_SET:{ int n=arg&0xFFFF; Value s=mp_set(T_set); for(int i=0;i<n;i++) mp_set_add((SetObj*)s.u.o,f->stack[f->sp-n+i]); f->sp-=n;
                if(arg&0x10000){ Value r=mp_set(T_set); mp_set_merge((SetObj*)r.u.o,(SetObj*)s.u.o); s=r; }   /* constants: CPython builds a frozenset and merges it */
                PUSH(s); break; }
            case I_BUILD_DICT:{ Value d=mp_dict(); int base=f->sp-2*arg; for(int i=0;i<arg;i++) mp_dict_set(AS_DICT(d),f->stack[base+2*i],f->stack[base+2*i+1]); f->sp=base; PUSH(d); break; }
            case I_BUILD_SLICE:{ Value c3= arg==3 ? POP() : v_none(); Value b=POP(), a=POP(); PUSH(mp_slice(a,b,c3)); break; }
            case I_BUILD_STRING:{
                SBuf b={0};
                for(int i=0;i<arg;i++){ Value s=f->stack[f->sp-arg+i]; sb_put(&b,AS_STR(s)->s,AS_STR(s)->len); }
                f->sp-=arg; PUSH(sb_value(&b)); break; }
            case I_LIST_APPEND:{ Value v=POP(); mp_list_append(f->stack[f->sp-arg],v); break; }
            case I_LIST_EXTEND:{ Value it=POP(); Value l=f->stack[f->sp-arg];
                if(IS(it,T_list)||IS(it,T_tuple)){ int64_t n=seq_len(it); Value *items=seq_items(it); for(int64_t i=0;i<n;i++) mp_list_append(l,items[i]); }
                else { Catch cc; Value x, iter;
                    if(!CATCH_BEGIN(cc)){ iter=mp_iter(it); CATCH_END(cc); }
                    else { Value e=mp_catch_exc(&cc); if(mp_isinstance(e,E_TypeError) && !(TYPE(it)->flags&TF_DUNDERS)) mp_raise_t(E_TypeError,"Value after * must be an iterable, not %s",mp_type_name(it)); reraise(e); }
                    while(mp_next(iter,&x)) mp_list_append(l,x); }
                break; }
            case I_SET_ADD:{ Value v=POP(); mp_set_add((SetObj*)f->stack[f->sp-arg].u.o,v); break; }
            case I_SET_UPDATE:{ Value it=POP(), x; SetObj *s=(SetObj*)f->stack[f->sp-arg].u.o;
                if(it.k==V_OBJ && it.u.o->type->layout==LY_SET){ mp_set_merge(s,(SetObj*)it.u.o); break; }    /* CPython's set_update */
                Value iter=mp_iter(it); while(mp_next(iter,&x)) mp_set_add(s,x); break; }
            case I_DICT_SET:{ Value v=POP(), k=POP(); mp_dict_set(AS_DICT(f->stack[f->sp-arg]),k,v); break; }
            case I_DICT_UPDATE:{ Value m=POP(); DictObj *d=AS_DICT(f->stack[f->sp-(arg&0xff)]);
                if(IS(m,T_dict) || (m.k==V_OBJ && m.u.o->type->layout==LY_DICT)){ int64_t pos=0; Value k, v;
                    while(mp_dict_next(AS_DICT(m),&pos,&k,&v)){
                        if((arg&0x100) && mp_dict_get(d,k,NULL)){ Value fn=f->stack[f->sp-(arg&0xff)-2]; mp_raise_t(E_TypeError,"%s got multiple values for keyword argument '%s'",IS(fn,T_function)?AS_FUNC(fn)->qualname->s:"function",IS_STR(k)?mp_cstr(k):"?"); }
                        mp_dict_set(d,k,v); } }
                else {
                    Value keys;
                    if(!mp_getattr_opt(m,mp_intern("keys"),&keys)) mp_raise_t(E_TypeError,"'%s' object is not a mapping",mp_type_name(m));
                    Value it=mp_iter(mp_call0(keys)), k;
                    while(mp_next(it,&k)) mp_dict_set(d,k,mp_getitem(m,k));
                }
                break; }
            case I_LIST_TO_TUPLE:{ Value l=TOP(); TOP()=mp_tuple(AS_LIST(l)->len,AS_LIST(l)->items); break; }
            case I_FORMAT:{
                Value spec= (arg&0x100) ? POP() : v_undef();
                Value v=TOP(); int conv=arg&0xff;
                if(conv=='r') v=mp_repr(v);
                else if(conv=='s') v=mp_tostr(v);
                else if(conv=='a'){ Value r=mp_repr(v); StrObj *s=AS_STR(r); SBuf b={0}; int64_t p=0;
                    while(p<s->len){ int64_t st=p; uint32_t cp=(uint32_t)mp_utf8_decode(s->s,s->len,&p); if(cp<0x80) sb_put(&b,s->s+st,p-st); else if(cp<0x100) sb_printf(&b,"\\x%02x",cp); else if(cp<0x10000) sb_printf(&b,"\\u%04x",cp); else sb_printf(&b,"\\U%08x",cp); }
                    v=sb_value(&b); }
                if(spec.k==V_UNDEF && IS(v,T_str)){ TOP()=v; break; }
                TOP()=mp_format(v,spec.k==V_UNDEF?mp_str(""):spec); break; }
            case I_UNPACK:{
                Value seq=POP(), orig=seq;
                if(IS_STR(seq) && !(TYPE(seq)->flags&TF_DUNDERS)) seq=mp_list_of(seq);
                Value l=unpack_list(seq,arg,0);
                int64_t n=seq_len(l);
                if(n<arg) mp_raise_t(E_ValueError,"not enough values to unpack (expected %d, got %lld)",arg,(long long)n);
                if(n>arg){ if(IS(orig,T_tuple)||IS(orig,T_list)||IS(orig,T_dict)) mp_raise_t(E_ValueError,"too many values to unpack (expected %d, got %lld)",arg,(long long)n); mp_raise_t(E_ValueError,"too many values to unpack (expected %d)",arg); }
                Value *items=seq_items(l);
                for(int i=arg-1;i>=0;i--) PUSH(items[i]);
                break; }
            case I_UNPACK_EX:{
                int before=arg&0xff, after=arg>>8;
                Value seq=POP();
                Value l=mp_list_of(seq);
                int64_t n=AS_LIST(l)->len;
                if(n<before+after) mp_raise_t(E_ValueError,"not enough values to unpack (expected at least %d, got %lld)",before+after,(long long)n);
                Value *items=AS_LIST(l)->items;
                for(int i=0;i<after;i++) PUSH(items[n-1-i]);
                PUSH(mp_list(n-before-after,items+before));
                for(int i=before-1;i>=0;i--) PUSH(AS_LIST(l)->items[i]);
                break; }
            case I_GET_ITER: TOP()=mp_iter(TOP());
                if(arg && IS(TOP(),T_generator) && ((GenObj*)TOP().u.o)->f && ((GenObj*)TOP().u.o)->f->state==FS_NEW) ((GenObj*)TOP().u.o)->loop_owned=1;
                break;
            case I_FOR_ITER:{
                Value it=TOP(), v;
                if(IS(it,T_iter)){
                    IterObj *io=(IterObj*)it.u.o;
                    if(io->kind==IT_RANGE){ if(io->n<=0){ f->sp--; f->ip=arg; break; } PUSH(v_int(io->i)); io->i+=((RangeObj*)io->src.u.o)->step; io->n--; break; }
                    if(io->kind==IT_SEQ && IS(io->src,T_list)){ ListObj *l=AS_LIST(io->src); if(io->i>=l->len){ f->sp--; f->ip=arg; break; } PUSH(l->items[io->i++]); break; }
                }
                if(mp_next(it,&v)) PUSH(v);
                else { f->sp--; f->ip=arg; }
                break; }
            case I_CALL:{
                Value *args=f->stack+f->sp-arg; Value fn=args[-1], r;
                if(IS(fn,T_function)) r=call_function(AS_FUNC(fn),arg,args,NULL,NULL);
                else r=mp_call(fn,arg,args,NULL);
                f->sp-=arg+1; PUSH(r); break; }
            case I_CALL_KW:{
                TupleObj *kw=AS_TUPLE(POP());
                Value *args=f->stack+f->sp-arg; Value fn=args[-1];
                Value r=mp_call(fn,arg,args,kw);
                f->sp-=arg+1; PUSH(r); break; }
            case I_CALL_EX:{
                Value kw= (arg&1) ? POP() : v_undef();
                Value args=POP(); Value fn=TOP();
                TOP()=call_ex(fn,args,kw); break; }
            case I_RETURN:{
                Value r=POP();
                ts->catch=c.prev;
                ts->nhandled=f->hbase; ts->depth--; ts->frame=f->back; f->back=NULL; f->state=FS_DONE;
                return r; }
            case I_MAKE_FUNCTION: PUSH(make_function(f,arg)); break;
            case I_MAKE_CLASS: PUSH(build_class(f,arg)); break;
            case I_IMPORT:{
                TupleObj *spec=AS_TUPLE(co->consts[arg]);
                PUSH(mp_import(mp_cstr(spec->items[0]),f->globals,(int)spec->items[1].u.i,(int)spec->items[2].u.i));
                break; }
            case I_IMPORT_FROM: PUSH(mp_import_from(TOP(),v_obj(co->names[arg]))); break;
            case I_IMPORT_STAR:{
                Value m=POP(); DictObj *src=inst_dict(m); DictObj *dst=f->locals?f->locals:f->globals;
                Value all;
                if(src && mp_dict_get_s(src,"__all__",&all)){ Value it=mp_iter(all), k; while(mp_next(it,&k)) mp_dict_set(dst,k,mp_getattr(m,k)); }
                else if(src){ int64_t pos=0; Value k, v; while(mp_dict_next(src,&pos,&k,&v)) if(IS_STR(k) && mp_cstr(k)[0]!='_') mp_dict_set(dst,k,v); }
                break; }
            case I_SETUP:{ Block *b=&f->blocks[f->nblocks++]; b->handler=arg; b->sp=f->sp; b->hdepth=ts->nhandled; break; }
            case I_POP_BLOCK: f->nblocks--; break;
            case I_RAISE:{
                if(arg==0){
                    if(ts->nhandled<=0) mp_raise_t(E_RuntimeError,"No active exception to reraise");
                    reraise(ts->handled[ts->nhandled-1]);
                }
                Value cause= arg==2 ? POP() : v_undef();
                Value e=instantiate_exc(POP());
                if(arg==2){
                    if(!IS_NONE(cause)){ cause=instantiate_exc(cause); }
                    AS_EXC(e)->cause=cause; AS_EXC(e)->suppress=1;
                }
                AS_EXC(e)->tb_frame=NULL;
                mp_raise(e); }
            case I_RERAISE:{ Value e=POP(); if(ts->nhandled>0) ts->nhandled--; reraise(e); }
            case I_PUSH_EXC: if(ts->nhandled<64) ts->handled[ts->nhandled++]=TOP(); break;
            case I_CALL_INTRINSIC:{                       /* except*: 1 begin, 2 match, 3 a handler raised, 4 end */
                static const char *const steps[]={NULL,"_begin","_match","_raised","_end"};
                Value fn=mp_excgroup(steps[arg]);
                if(arg==1){ TOP()=mp_call1(fn,TOP()); break; }
                if(arg==2){ Value t=POP(); Value m=mp_call2(fn,TOP(),t); PUSH(m); break; }
                if(arg==3){ Value x=POP(); mp_call2(fn,TOP(),x); break; }
                { Value r=mp_call1(fn,TOP()); if(!IS_NONE(r)){ f->sp--; if(ts->nhandled>f->hbase) ts->nhandled--; reraise(r); } }
                break; }
            case I_POP_EXC: if(ts->nhandled>f->hbase) ts->nhandled--; break;
            case I_EXC_MATCH:{ Value spec=POP(), e=POP(); PUSH(v_bool(mp_exc_matches(e,spec))); break; }
            case I_YIELD:{
                Value v=POP();
                if(!arg && (co->flags&CO_ASYNCGEN)) v=ag_wrap(v);     /* (a value of the async generator, not an await's) */
                /* the exceptions it is handling stay with it */
                int k=ts->nhandled-f->hbase; if(k>8) k=8;
                for(int i=0;i<k;i++) f->saved_exc[i]=ts->handled[f->hbase+i];
                f->nsaved=k;
                ts->catch=c.prev;
                ts->nhandled=f->hbase; ts->depth--; ts->frame=f->back; f->back=NULL; f->state=FS_SUSPENDED;
                return v; }
            case I_GET_YIELD_FROM_ITER:{ Value v=TOP();
                if(IS(v,T_coroutine) && !(co->flags&CO_CORO)) mp_raise_t(E_TypeError,"cannot 'yield from' a coroutine object in a non-coroutine generator");
                if(!IS(v,T_generator) && !IS(v,T_coroutine)) TOP()=mp_iter(v);
                break; }
            case I_GET_AWAITABLE: TOP()=mp_await_iter(TOP()); break;
            case I_SEND:{
                Value v=POP(), recv=TOP(), out;
                if(send_into(recv,v,&out)){ PUSH(out); f->yf=recv; f->yf_exit=arg; }
                else { f->yf=v_undef(); TOP()=out; f->ip=arg; }
                break; }
            case I_GET_AITER:{ Value o=TOP(); Value m=mp_type_lookup_s(TYPE(o),"__aiter__");
                if(m.k==V_UNDEF) mp_raise_t(E_TypeError,"'async for' requires an object with __aiter__ method, got %s",mp_type_name(o));
                TOP()=mp_call1(m,o); break; }
            case I_GET_ANEXT:{ Value o=TOP(); Value m=mp_type_lookup_s(TYPE(o),"__anext__");
                if(m.k==V_UNDEF) mp_raise_t(E_TypeError,"'async for' received an object from __aiter__ that does not implement __anext__: %s",mp_type_name(o));
                PUSH(mp_call1(m,o)); break; }
            case I_END_ASYNC_FOR:{ Value e=POP(); if(!mp_isinstance(e,E_StopAsyncIteration)) reraise(e); f->sp--; break; }
            case I_ASSERT_FAIL:{ Value m= arg ? POP() : v_undef(); mp_raise(m.k==V_UNDEF ? mp_exc_args(E_AssertionError,mp_tuple(0,NULL)) : mp_exc_args(E_AssertionError,mp_tuple(1,&m))); }
            case I_WITH_ENTER:{
                Value ctx=POP();
                Value enter,exitf; special_pair(ctx,"__enter__","__exit__","context manager",&enter,&exitf);
                PUSH(exitf);
                Block *b=&f->blocks[f->nblocks++]; b->handler=arg; b->sp=f->sp; b->hdepth=ts->nhandled;
                PUSH(mp_call0(enter));
                break; }
            case I_ASYNC_WITH_ENTER:{
                Value ctx=POP();
                Value enter,exitf; special_pair(ctx,"__aenter__","__aexit__","asynchronous context manager",&enter,&exitf);
                PUSH(exitf); PUSH(mp_call0(enter));
                break; }
            case I_WITH_EXIT:
                if(arg==0){ Value ex=POP(); Value a[3]={v_none(),v_none(),v_none()}; mp_call(ex,3,a,NULL); }
                else if(arg==1){ Value e=POP(), ex=POP(); Value a[3]={exc_type_of(e),e,exc_tb_of(e)};
                    Value r=mp_call(ex,3,a,NULL);
                    if(ts->nhandled>0) ts->nhandled--;
                    if(!mp_truth(r)) reraise(e); }
                else if(arg==2){ Value e=POP(), ex=POP(); Value a[3]={exc_type_of(e),e,exc_tb_of(e)}; PUSH(e); PUSH(mp_call(ex,3,a,NULL)); }
                else if(arg==3){ Value r=POP(), e=POP(); if(ts->nhandled>0) ts->nhandled--; if(!mp_truth(r)) reraise(e); }
                else { Value ex=POP(); Value a[3]={v_none(),v_none(),v_none()}; PUSH(mp_call(ex,3,a,NULL)); }
                break;
            case I_SETUP_ANNOTATIONS:{ DictObj *d=f->locals?f->locals:f->globals; if(!mp_dict_get_s(d,"__annotations__",NULL)) mp_dict_set_s(d,"__annotations__",mp_dict()); break; }
            case I_MATCH_SEQ:{
                Value s=TOP(); int nmin=arg&0xffff, star=(arg>>16)&1;
                if(!match_seq_ok(s)){ PUSH(v_none()); break; }
                int64_t n=mp_len(s);
                if(star ? n<nmin : n!=nmin){ PUSH(v_none()); break; }
                PUSH(mp_list_of(s)); break; }
            case I_MATCH_MAP:{ Value s=TOP(); Type *t=TYPE(s);
                int ok= t->layout==LY_DICT || ((t->flags&TF_DUNDERS) && mp_type_lookup_s(t,"keys").k!=V_UNDEF && mp_type_lookup_s(t,"__getitem__").k!=V_UNDEF);
                PUSH(v_bool(ok)); break; }
            case I_MATCH_KEYS:{
                Value keys=TOP(), s=SECOND(); TupleObj *k=AS_TUPLE(keys);
                Value vals=mp_tuple(k->len,NULL); int ok=1;
                for(int64_t i=0;i<k->len && ok;i++){
                    if(IS(s,T_dict) || TYPE(s)->layout==LY_DICT){ Value v; if(mp_dict_get(AS_DICT(s),k->items[i],&v)) AS_TUPLE(vals)->items[i]=v; else ok=0; }
                    else { Value get=mp_getattr_s(s,"get"); Value miss=mp_list(0,NULL); Value a[2]={k->items[i],miss}; Value v=mp_call(get,2,a,NULL); if(v.k==V_OBJ && v.u.o==miss.u.o) ok=0; else AS_TUPLE(vals)->items[i]=v; }
                }
                PUSH(ok?vals:v_none());
                if(arg){
                    if(!ok) PUSH(v_none());
                    else { Value rest=mp_dict(); Value it=mp_iter(s), x;
                        while(mp_next(it,&x)){ int skip=0; for(int64_t i=0;i<k->len;i++) if(mp_eq(x,k->items[i])) skip=1; if(!skip) mp_dict_set(AS_DICT(rest),x,mp_getitem(s,x)); }
                        PUSH(rest); }
                }
                break; }
            case I_MATCH_CLASS:{
                Value kwn=POP(), cls=POP(), s=POP();
                if(!IS(cls,T_type)) mp_raise_t(E_TypeError,"called match pattern must be a class");
                Type *t=AS_TYPE(cls);
                if(!mp_isinstance(s,t)){ PUSH(v_none()); break; }
                TupleObj *kn=AS_TUPLE(kwn); int npos=arg;
                Value out=mp_tuple(npos+kn->len,NULL); int ok=1;
                if(npos){
                    Value ma=mp_type_lookup_s(t,"__match_args__");
                    int self_match= t==T_int||t==T_float||t==T_str||t==T_bool||t==T_bytes||t==T_list||t==T_tuple||t==T_dict||t==T_set||t==T_frozenset;
                    if(ma.k==V_UNDEF && self_match){ if(npos>1) mp_raise_t(E_TypeError,"%s() accepts 1 positional sub-pattern (%d given)",t->name->s,npos); AS_TUPLE(out)->items[0]=s; }
                    else {
                        if(ma.k==V_UNDEF || !IS(ma,T_tuple)) mp_raise_t(E_TypeError,"%s() accepts 0 positional sub-patterns (%d given)",t->name->s,npos);
                        if(AS_TUPLE(ma)->len<npos) mp_raise_t(E_TypeError,"%s() accepts %lld positional sub-patterns (%d given)",t->name->s,(long long)AS_TUPLE(ma)->len,npos);
                        for(int i=0;i<npos && ok;i++){ Value v; if(mp_getattr_opt(s,AS_TUPLE(ma)->items[i],&v)) AS_TUPLE(out)->items[i]=v; else ok=0; }
                    }
                }
                for(int64_t i=0;i<kn->len && ok;i++){ Value v; if(mp_getattr_opt(s,kn->items[i],&v)) AS_TUPLE(out)->items[npos+i]=v; else ok=0; }
                PUSH(ok?out:v_none());
                break; }
            default: mp_raise_t(E_SystemError,"bad instruction %d",op);
        }
    }
}

Value mp_run_code(CodeObj *co, DictObj *globals, DictObj *locals){
    Frame *f=frame_new(co,NULL,globals,locals);
    for(int i=0;i<co->ncells;i++) f->cells[i]=mp_cell(v_undef());
    Catch c; Value r;
    if(!CATCH_BEGIN(c)){ r=eval(f,v_undef(),0,0); CATCH_END(c); }
    else { Value e=mp_catch_exc(&c); free(f); reraise(e); }
    free(f);
    return r;
}
