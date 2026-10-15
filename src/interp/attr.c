/* ========================= Interpreter: attributes =========================
   Attribute lookup through the MRO and descriptors (functions bind,
   property, staticmethod, classmethod, super); setting and deleting. */

#include "run.h"

Value mp_type_lookup(Type *t, Value name){
    Value v;
    for(int i=0;i<t->nmro;i++) if(t->mro[i]->dict && mp_dict_get(t->mro[i]->dict,name,&v)) return v;
    return v_undef();
}
Value mp_type_lookup_s(Type *t, const char *name){ return mp_type_lookup(t,mp_intern(name)); }
/* a special method for the interpreter to call: a Python function, or a built-in one a class
   statement put in a class (IntEnum.__str__ = int.__repr__); not the built-in types' own */
Value mp_type_lookup_user(Type *t, const char *name){
    Value v, key=mp_intern(name);
    for(int i=0;i<t->nmro;i++) if(t->mro[i]->dict && mp_dict_get(t->mro[i]->dict,key,&v)){
        if(IS(v,T_native) && !(t->mro[i]->flags&TF_HEAP)) return v_undef();
        return v; }
    return v_undef();
}
static Value bind(Value f, Value self){
    MethodObj *m=(MethodObj*)mp_alloc(T_method,sizeof(MethodObj)); m->func=f; m->self=self; return v_obj(m);
}
static Value bind_native(Value f, Value self){
    NativeObj *n=(NativeObj*)f.u.o, *b=(NativeObj*)mp_alloc(T_native,sizeof(NativeObj));
    b->name=n->name; b->fn=n->fn; b->self=mp_unbox(self); b->is_method=1; return v_obj(b);   /* (an int subclass's methods: of its value) */
}
/* the attribute found on the type of `obj`, as seen through obj */
Value descr_get(Value d, Value obj, Type *t){
    if(d.k!=V_OBJ) return d;
    Type *dt=d.u.o->type;
    if(dt==T_function) return obj.k==V_UNDEF || ((FuncObj*)d.u.o)->builtin ? d : bind(d,obj);
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
DictObj *inst_dict(Value o){
    if(o.k!=V_OBJ) return NULL;
    if(o.u.o->type->flags&TF_SUBVAL) return *mp_sub_slot(o.u.o);
    switch(o.u.o->type->layout){
        case LY_INSTANCE: return ((InstObj*)o.u.o)->dict;
        case LY_EXC: return ((ExcObj*)o.u.o)->dict;
        case LY_MODULE: return ((ModuleObj*)o.u.o)->dict;
        case LY_FUNC: return ((FuncObj*)o.u.o)->dict;
        default: return NULL;
    }
}
static Value special_attr(Value o, const char *n, int *found);
static int getattr_generic(Value o, Value name, Value *out, int with_getattr);
/* function.__annotate__(format): Format.VALUE (1, 2) only, as CPython's own annotate functions */
static Value fn_annotate(int argc, Value *argv, TupleObj *kw){
    (void)kw; if(argc!=2) mp_raise_t(E_TypeError,"__annotate__() takes exactly one argument (%d given)",argc-1);
    int64_t fmt=mp_index(argv[1],"format");
    if(fmt!=1 && fmt!=2) mp_raise_t(E_NotImplementedError,"%lld",(long long)fmt);
    return mp_getattr_s(argv[0],"__annotations__");
}
int mp_getattr_opt(Value o, Value name, Value *out){
    Type *t=TYPE(o);
    if(o.k==V_OBJ && t->layout!=LY_TYPE && (t->flags&TF_HEAP)){
        Value gab=mp_type_lookup_user(t,"__getattribute__");        /* a class's own __getattribute__ (then __getattr__) */
        if(gab.k!=V_UNDEF){
            Catch c; Value r;
            if(!CATCH_BEGIN(c)){ r=mp_call2(gab,o,name); CATCH_END(c); *out=r; return 1; }
            Value e=mp_catch_exc(&c);
            if(!mp_isinstance(e,E_AttributeError)) mp_raise(e);
            Value ga=mp_type_lookup_s(t,"__getattr__");
            if(ga.k==V_UNDEF) mp_raise(e);
            Catch c2;
            if(!CATCH_BEGIN(c2)){ r=mp_call2(ga,o,name); CATCH_END(c2); *out=r; return 1; }
            Value e2=mp_catch_exc(&c2);
            if(mp_isinstance(e2,E_AttributeError)) return 0;
            mp_raise(e2); } }
    return getattr_generic(o,name,out,1);
}
/* object.__getattribute__(o, name): the instance's dict, its class's descriptors; no __getattr__ */
Value mp_getattr_generic(Value o, Value name){
    Value v; int ok=getattr_generic(o,name,&v,0);
    if(ok) return v;
    if(IS(o,T_module)) mp_raise_attr(o,name,"module '%s' has no attribute '%s'",((ModuleObj*)o.u.o)->name->s,mp_cstr(name));
    if(IS_TYPE(o)) mp_raise_attr(o,name,"type object '%s' has no attribute '%s'",AS_TYPE(o)->name->s,mp_cstr(name));
    mp_raise_attr(o,name,"'%s' object has no attribute '%s'",mp_type_name(o),mp_cstr(name));
}
static int getattr_generic(Value o, Value name, Value *out, int with_getattr){
    Type *t=TYPE(o);
    const char *n=mp_cstr(name);
    if(t->layout==LY_TYPE){                        /* an attribute of a class: its MRO, then the metaclass */
        Type *cls=AS_TYPE(o);
        if(!strcmp(n,"__annotations__")){ int found=0; *out=special_attr(o,n,&found); return 1; }   /* its own, evaluated (not inherited) */
        if(!strcmp(n,"__annotate__") && cls->dict){                        /* its own too (3.14): None when it has no annotations */
            Value an; if(mp_dict_get_s(cls->dict,"__annotate__",&an) || mp_dict_get_s(cls->dict,"__annotate_func__",&an)){ *out=an; return 1; }
            if(mp_dict_get_s(cls->dict,"__annotations__",&an) && IS_DICT(an)){
                NativeObj *b=(NativeObj*)mp_alloc(T_native,sizeof(NativeObj)); b->name="__annotate__"; b->fn=fn_annotate; b->self=o; b->is_method=1;
                *out=v_obj(b); return 1; }
            if(cls->flags&TF_HEAP){ *out=v_none(); return 1; } }
        Value md= mp_type_lookup(t,name);                                   /* (the metaclass's: data descriptors first; type's methods last) */
        if(md.k!=V_UNDEF && is_data_descr(md)){ *out=descr_get(md,o,t); return 1; }
        Value d=mp_type_lookup(cls,name);
        if(d.k!=V_UNDEF){ *out=descr_get(d,v_undef(),cls); if(IS(d,T_classmethod)) *out=descr_get(d,v_undef(),cls); return 1; }
        int found=0; Value v=special_attr(o,n,&found);
        if(found){ *out=v; return 1; }
        if(md.k!=V_UNDEF){ *out=descr_get(md,o,t); return 1; }
        if(t!=T_type && (t->flags&TF_HEAP)){
            Value ga=mp_type_lookup_user(t,"__getattr__");
            if(ga.k!=V_UNDEF){
                Catch c; Value r;
                if(!CATCH_BEGIN(c)){ r=mp_call2(ga,o,name); CATCH_END(c); *out=r; return 1; }
                Value e=mp_catch_exc(&c);
                if(mp_isinstance(e,E_AttributeError)) return 0;
                mp_raise(e); } }
        return 0;
    }
    if(t->layout==LY_SUPER){
        SuperObj *s=(SuperObj*)o.u.o;
        Type *start=s->objtype; int i=0;
        while(i<start->nmro && start->mro[i]!=s->start) i++;
        for(i++;i<start->nmro;i++){ Value v; if(start->mro[i]->dict && mp_dict_get(start->mro[i]->dict,name,&v)){ *out=descr_get(v,IS_TYPE(s->obj)&&AS_TYPE(s->obj)==s->objtype?v_undef():s->obj,s->objtype); return 1; } }
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
    if(t->layout==LY_NUM && mp_getattr_opt(mp_unbox(o),name,out)) return 1;     /* (x.real of an int subclass: its value's) */
    if((t->flags&TF_DUNDERS) && with_getattr){
        Value ga=mp_type_lookup_s(t,"__getattr__");
        if(ga.k!=V_UNDEF){
            Catch c; Value r;
            if(!CATCH_BEGIN(c)){ r=mp_call2(ga,o,name); CATCH_END(c); *out=r; return 1; }
            Value e=mp_catch_exc(&c);
            if(mp_isinstance(e,E_AttributeError)) return 0;
            mp_raise(e);
        }
    }
    if(IS(o,T_module) && dict && strcmp(n,"__getattr__")){                    /* a module's __getattr__(name) (PEP 562) */
        Value ga;
        if(mp_dict_get_s(dict,"__getattr__",&ga)){
            Catch c; Value r;
            if(!CATCH_BEGIN(c)){ r=mp_call1(ga,name); CATCH_END(c); *out=r; return 1; }
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
    if(IS(o,T_module)) mp_raise_attr(o,name,"module '%s' has no attribute '%s'",((ModuleObj*)o.u.o)->name->s,mp_cstr(name));
    if(IS_TYPE(o)) mp_raise_attr(o,name,"type object '%s' has no attribute '%s'",AS_TYPE(o)->name->s,mp_cstr(name));
    mp_raise_attr(o,name,"'%s' object has no attribute '%s'",mp_type_name(o),mp_cstr(name));
}
Value mp_getattr_s(Value o, const char *name){ return mp_getattr(o,mp_intern(name)); }
/* attributes every object of a kind has (not in a type's dict) */
static Value special_attr(Value o, const char *n, int *found){
    *found=1;
    Type *t=TYPE(o);
    if(!strcmp(n,"__class__")) return v_obj(t);
    if(!strcmp(n,"__dict__")){ DictObj *d=inst_dict(o); if(t->layout==LY_TYPE) d=AS_TYPE(o)->dict;
        if(!d && t->layout==LY_FUNC){ d=AS_DICT(mp_dict()); ((FuncObj*)o.u.o)->dict=d; }          /* (a function's attributes: made when asked) */
        if(d) return v_obj(d); }
    switch(t->layout){
        case LY_TYPE:{ Type *c=AS_TYPE(o);
            if(!strcmp(n,"__name__")) return v_obj(c->name);
            if(!strcmp(n,"__qualname__")) return v_obj(c->qualname?c->qualname:c->name);
            if(!strcmp(n,"__bases__")) return v_obj(c->bases);
            if(!strcmp(n,"__base__")) return c->base?v_obj(c->base):v_none();
            if(!strcmp(n,"__mro__")){ Value r=mp_tuple(c->nmro,NULL); for(int i=0;i<c->nmro;i++) AS_TUPLE(r)->items[i]=v_obj(c->mro[i]); return r; }
            if(!strcmp(n,"__module__")){ Value m=mp_type_lookup_s(c,"__module__"); return m.k==V_UNDEF?mp_str("builtins"):m; }
            if(!strcmp(n,"__flags__")){                    /* CPython's bits copyreg and inspect look at: heap type (a class statement's), base type, abstract */
                int64_t r=0; if(c->flags&TF_HEAP) r|=1<<9; if(c->flags&TF_BASETYPE) r|=1<<10;
                Value am; if(c->dict && mp_dict_get_s(c->dict,"__abstractmethods__",&am) && mp_truth(am)) r|=1<<20;
                return v_int(r); }
            if(!strcmp(n,"__doc__")) return v_none();
            if(!strcmp(n,"__annotations__")){ Value own, r=mp_dict();          /* its own, evaluated now (CPython 3.14) */
                if(c->dict && !mp_dict_get_s(c->dict,"__annotations__",NULL)){   /* (none: what its __annotate__ gives, as NamedTuple's) */
                    Value an; if((mp_dict_get_s(c->dict,"__annotate__",&an) || mp_dict_get_s(c->dict,"__annotate_func__",&an)) && !IS_NONE(an)){
                        Value d=mp_call1(an,v_int(1)); if(!IS_DICT(d)) mp_raise_t(E_TypeError,"__annotate__ returned non-dict of type '%s'",mp_type_name(d));
                        return d; } }
                if(!c->dict || !mp_dict_get_s(c->dict,"__annotations__",&own) || !IS_DICT(own)) return r;
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
            if(!strcmp(n,"__annotate__")){                                   /* (3.14) format -> the annotations; None: it has none */
                if(!f->code->nann) return v_none();
                NativeObj *b=(NativeObj*)mp_alloc(T_native,sizeof(NativeObj)); b->name="__annotate__"; b->fn=fn_annotate; b->self=o; b->is_method=1;
                return v_obj(b); }
            if(!strcmp(n,"__mpy_annotation_texts__")){ Value d=mp_dict(); CodeObj *co=f->code;   /* (annotationlib: Format.STRING / FORWARDREF) */
                for(int i=0;i<co->nann;i++) mp_dict_set(AS_DICT(d),mp_str(co->annname[i]),mp_str(co->anntext[i]));
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
            if(!strcmp(n,"co_posonlyargcount")) return v_int(c->posonly);
            if(!strcmp(n,"co_kwonlyargcount")) return v_int(c->kwonly);
            if(!strcmp(n,"co_nlocals")) return v_int(c->nlocals);
            if(!strcmp(n,"co_stacksize")) return v_int(c->stacksize);
            if(!strcmp(n,"co_flags")){                     /* CPython's bits: optimized, newlocals, varargs, varkeywords, nested, generator, coroutine, async generator, has docstring */
                int f=c->flags, r=(f&(CO_MODULE|CO_CLASS)) ? 0 : 3;
                if(f&CO_VARARGS) r|=4; if(f&CO_VARKW) r|=8; if(f&CO_NESTED) r|=0x10; if(f&CO_GEN) r|=0x20; if(f&CO_CORO) r|=0x80; if(f&CO_ASYNCGEN) r|=0x200;
                if(c->doc.k!=V_UNDEF && !IS_NONE(c->doc)) r|=0x4000000;
                return v_int(r); }
            if(!strcmp(n,"co_cellvars")){ Value r=mp_tuple(c->ncells,NULL); for(int i=0;i<c->ncells;i++) AS_TUPLE(r)->items[i]=v_obj(c->cellnames[i]); return r; }
            if(!strcmp(n,"co_freevars")){ Value r=mp_tuple(c->nfrees,NULL); for(int i=0;i<c->nfrees;i++) AS_TUPLE(r)->items[i]=v_obj(c->freenames[i]); return r; }
            if(!strcmp(n,"co_names")){ Value r=mp_tuple(c->nnames,NULL); for(int i=0;i<c->nnames;i++) AS_TUPLE(r)->items[i]=v_obj(c->names[i]); return r; }
            if(!strcmp(n,"co_consts")) return mp_tuple(c->nconsts,c->consts);
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
            if((!strcmp(n,"name") && (mp_isinstance(o,E_NameError)||mp_isinstance(o,E_AttributeError)||mp_isinstance(o,E_ImportError)))
               || (!strcmp(n,"obj") && mp_isinstance(o,E_AttributeError)) || ((!strcmp(n,"path")||!strcmp(n,"name_from")) && mp_isinstance(o,E_ImportError))) return v_none();
            if(mp_isinstance(o,E_KeyError) && !strcmp(n,"key")){ TupleObj *a=AS_TUPLE(e->args); return a->len?a->items[0]:v_none(); }
            if(mp_isinstance(o,E_OSError)){ TupleObj *a=AS_TUPLE(e->args);
                if(!strcmp(n,"errno")) return a->len>=2?a->items[0]:v_none();
                if(!strcmp(n,"strerror")) return a->len>=2?a->items[1]:v_none();
                if(!strcmp(n,"filename")||!strcmp(n,"filename2")){ Value fv; if(e->dict && mp_dict_get_s(e->dict,n,&fv)) return fv; return v_none(); } }
            if(mp_isinstance(o,E_ImportError) && (!strcmp(n,"name")||!strcmp(n,"path"))) return v_none();
            if(mp_isinstance(o,E_SyntaxError) && IS_TUPLE(e->args)){ TupleObj *a=AS_TUPLE(e->args);   /* (msg, (filename, lineno, offset, text, end_lineno, end_offset)) */
                if(!strcmp(n,"msg")) return a->len?a->items[0]:v_none();
                static const char *const f[]={"filename","lineno","offset","text","end_lineno","end_offset"};
                for(int i=0;i<6;i++) if(!strcmp(n,f[i])){
                    if(a->len>=2 && IS_TUPLE(a->items[1]) && AS_TUPLE(a->items[1])->len>i) return AS_TUPLE(a->items[1])->items[i];
                    return v_none(); }
                if(!strcmp(n,"print_file_and_line")) return v_none(); }
            if(mp_isinstance(o,E_UnicodeError) && IS_TUPLE(e->args) && AS_TUPLE(e->args)->len==5){ TupleObj *a=AS_TUPLE(e->args);   /* (encoding, object, start, end, reason) */
                static const char *const f[]={"encoding","object","start","end","reason"};
                for(int i=0;i<5;i++) if(!strcmp(n,f[i])) return a->items[i]; }
            break; }
        case LY_GEN:{ GenObj *g=(GenObj*)o.u.o;
            if(!strcmp(n,"__name__")) return v_obj(g->name);
            if(!strcmp(n,"__qualname__")) return v_obj(g->qualname);
            if(!strcmp(n,"gi_running")||!strcmp(n,"cr_running")) return v_bool(g->running);
            if(!strcmp(n,"gi_frame")||!strcmp(n,"cr_frame")) return g->f && g->f->state!=FS_DONE ? v_bool(1) : v_none();
            if(!strcmp(n,"gi_suspended")||!strcmp(n,"cr_suspended")) return v_bool(g->f && g->f->state==FS_SUSPENDED);
            if(!strcmp(n,"gi_code")||!strcmp(n,"cr_code")) return g->f && g->f->code ? v_obj(g->f->code) : v_none();
            break; }
        case LY_PROPERTY:{ PropertyObj *p=(PropertyObj*)o.u.o;
            if(!strcmp(n,"fget")) return p->get.k==V_UNDEF?v_none():p->get;
            if(!strcmp(n,"fset")) return p->set.k==V_UNDEF?v_none():p->set;
            if(!strcmp(n,"fdel")) return p->del.k==V_UNDEF?v_none():p->del;
            if(!strcmp(n,"__doc__")) return p->doc.k==V_UNDEF?v_none():p->doc;
            if(!strcmp(n,"__isabstractmethod__")){ Value fs[3]={p->get,p->set,p->del}, v;
                for(int i=0;i<3;i++) if(fs[i].k==V_OBJ && mp_getattr_opt(fs[i],mp_intern(n),&v) && mp_truth(v)) return v_bool(1);
                return v_bool(0); }
            break; }
        case LY_STATICMETHOD: case LY_CLASSMETHOD:
            if(!strcmp(n,"__func__") || !strcmp(n,"__wrapped__")) return ((BoxObj*)o.u.o)->v;
            if(!strcmp(n,"__isabstractmethod__")){ Value v; return v_bool(mp_getattr_opt(((BoxObj*)o.u.o)->v,mp_intern(n),&v) && mp_truth(v)); }
            break;
        case LY_COMPLEX:{ ComplexObj *c=(ComplexObj*)o.u.o; if(!strcmp(n,"real")) return v_float(c->re); if(!strcmp(n,"imag")) return v_float(c->im); break; }
        case LY_RANGE:{ RangeObj *r=(RangeObj*)o.u.o; if(!strcmp(n,"start")) return v_int(r->start); if(!strcmp(n,"stop")) return v_int(r->stop); if(!strcmp(n,"step")) return v_int(r->step); break; }
        case LY_SLICE:{ SliceObj *s=(SliceObj*)o.u.o; if(!strcmp(n,"start")) return s->start; if(!strcmp(n,"stop")) return s->stop; if(!strcmp(n,"step")) return s->step; break; }
        case LY_FILE:{ FileObj *f=(FileObj*)o.u.o; if(!strcmp(n,"name")) return f->name; if(!strcmp(n,"closed")) return v_bool(f->closed); if(!strcmp(n,"mode")){ char m[3]={(char)f->mode,f->binary?'b':0,0}; return mp_str(m); } break; }
        default: break;
    }
    if(o.k==V_INT || o.k==V_BOOL){ if(!strcmp(n,"real")||!strcmp(n,"numerator")) return v_int(o.u.i); if(!strcmp(n,"imag")) return v_int(0); if(!strcmp(n,"denominator")) return v_int(1); }
    if(o.k==V_FLOAT){ if(!strcmp(n,"real")) return o; if(!strcmp(n,"imag")) return v_float(0); }
    if(!strcmp(n,"__doc__")){ Value d=mp_type_lookup_s(t,"__doc__"); return d.k==V_UNDEF?v_none():d; }   /* every object has one (its class's) */
    *found=0;
    return v_undef();
}
void mp_setattr(Value o, Value name, Value v){
    Type *t=TYPE(o);
    if(t->layout==LY_TYPE){
        Type *c=AS_TYPE(o);
        if(t!=T_type && (t->flags&TF_HEAP) && o.u.o!=mp_raw_obj){            /* a metaclass's __setattr__ */
            Value sa=mp_type_lookup_s(t,"__setattr__");
            if(sa.k!=V_UNDEF && !IS(sa,T_native)){ Value a[3]={o,name,v}; mp_call(sa,3,a,NULL); return; } }
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
        Value sa=mp_type_lookup_user(t,"__setattr__");
        if(sa.k!=V_UNDEF){ Value a[3]={o,name,v}; mp_call(sa,3,a,NULL); return; }
    }
    if(t->flags&TF_SUBVAL){ DictObj **slot=mp_sub_slot(o.u.o); if(!*slot) *slot=AS_DICT(mp_dict()); mp_dict_set(*slot,name,v); return; }
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
        if(mp_str_eq_c(name,"args")){ e->args=IS_TUPLE(v)?v:mp_tuple(1,&v); return; }
        if(!e->dict) e->dict=AS_DICT(mp_dict());
        mp_dict_set(e->dict,name,v); return; }
    if(d.k!=V_UNDEF) mp_raise_t(E_AttributeError,"'%s' object attribute '%s' is read-only",t->name->s,mp_cstr(name));
    mp_raise_t(E_AttributeError,"'%s' object has no attribute '%s' and no __dict__ for setting new attributes",t->name->s,mp_cstr(name));
}
void mp_delattr(Value o, Value name){
    Type *t=TYPE(o);
    if(t->layout==LY_TYPE && t!=T_type && (t->flags&TF_HEAP) && o.u.o!=mp_raw_obj){      /* a metaclass's __delattr__ */
        Value da=mp_type_lookup_s(t,"__delattr__");
        if(da.k!=V_UNDEF && !IS(da,T_native)){ mp_call2(da,o,name); return; } }
    if(t->layout==LY_TYPE){ if(!mp_dict_del(AS_TYPE(o)->dict,name)) mp_raise_t(E_AttributeError,"type object '%s' has no attribute '%s'",AS_TYPE(o)->name->s,mp_cstr(name)); return; }
    Value d=mp_type_lookup(t,name);
    if(IS(d,T_property)){ PropertyObj *p=(PropertyObj*)d.u.o; if(p->del.k==V_UNDEF||IS_NONE(p->del)) mp_raise_t(E_AttributeError,"property '%s' of '%s' object has no deleter",mp_cstr(name),t->name->s); mp_call1(p->del,o); return; }
    if(t->flags&TF_DUNDERS){ Value da=mp_type_lookup_user(t,"__delattr__"); if(da.k!=V_UNDEF){ mp_call2(da,o,name); return; } }
    DictObj *dict=inst_dict(o);
    if(dict && mp_dict_del(dict,name)) return;
    mp_raise_t(E_AttributeError,"'%s' object has no attribute '%s'",t->name->s,mp_cstr(name));
}

