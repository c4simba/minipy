/* ========================= Interpreter, methods: objects =========================
   Generators and coroutines, object, BaseException and its subclasses,
   property, files. */

#include "methods.h"

static Value m_send(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("send",argc,1,1);
    int done; Value r=mp_gen_send(SELF,argv[1],&done);
    if(done) mp_raise(mp_exc_args(E_StopIteration,IS_NONE(r)?mp_tuple(0,NULL):mp_tuple(1,&r)));
    return r; }
static Value m_throw(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("throw",argc,1,3);
    Value e=argv[1];
    if(argc>2 && !IS_NONE(argv[2]) && IS_TYPE(e)) e=mp_call1(e,argv[2]);
    int done; Value r=mp_gen_throw(SELF,e,&done);
    if(done) mp_raise(mp_exc_args(E_StopIteration,IS_NONE(r)?mp_tuple(0,NULL):mp_tuple(1,&r)));
    return r; }
static Value m_close(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; mp_gen_close(SELF); return v_none(); }
static Value m_gnext(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; Value v; if(!mp_next(SELF,&v)) mp_raise(mp_exc_args(E_StopIteration,mp_tuple(0,NULL))); return v; }
static Value m_self(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; return SELF; }

/* ---------------------------------------------------------------- object, exceptions, property */
static Value o_init(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; return v_none(); }
static Value o_new(int argc, Value *argv, TupleObj *kw){ (void)kw;
    if(argc<1 || !IS_TYPE(argv[0])) mp_raise_t(E_TypeError,"object.__new__(X): X is not a type object");
    Type *t=AS_TYPE(argv[0]);
    if(t->flags&TF_SUBVAL){ const char *nm=t->qualname?t->qualname->s:t->name->s;      /* (a dict / list ... subclass: its own __new__) */
        mp_raise_t(E_TypeError,"object.__new__(%s) is not safe, use %s.__new__()",nm,nm); }
    return mp_instance(t); }
static Value o_eq(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; return argv[0].k==V_OBJ && argv[1].k==V_OBJ && argv[0].u.o==argv[1].u.o ? v_bool(1) : mp_NotImplemented; }
static Value o_ne(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; Value r=mp_compare(OP_Eq,argv[0],argv[1]); return v_bool(!mp_truth(r)); }
static Value o_hash(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; uint64_t p=(uint64_t)(uintptr_t)argv[0].u.o; return v_int((int64_t)((p>>4)|(p<<60))); }
static Value o_repr(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; Type *t=TYPE(argv[0]); Value m=mp_type_lookup_s(t,"__module__");
    return mp_strf("<%s.%s object at %p>",IS_STR(m)?mp_cstr(m):"builtins",t->qualname?t->qualname->s:t->name->s,(void*)argv[0].u.o); }
static Value o_str(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; return mp_repr(argv[0]); }
static Value o_format(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; if(argc>1 && IS_STR(argv[1]) && AS_STR(argv[1])->len) mp_raise_t(E_TypeError,"unsupported format string passed to %s.__format__",mp_type_name(argv[0])); return mp_tostr(argv[0]); }
static Value o_setattr(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__setattr__",argc,2,2);
    InstObj *o=AS_INST(argv[0]);
    if(argv[0].u.o->type->layout==LY_INSTANCE){ mp_dict_set(o->dict,argv[1],argv[2]); return v_none(); }
    mp_setattr(argv[0],argv[1],argv[2]); return v_none(); }
static Value o_getattribute(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__getattribute__",argc,1,1);
    if(!IS_STR(argv[1])) mp_raise_t(E_TypeError,"attribute name must be string, not '%s'",mp_type_name(argv[1]));
    return mp_getattr_generic(argv[0],argv[1]); }
static Value o_delattr(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__delattr__",argc,1,1);
    if(argv[0].u.o->type->layout==LY_INSTANCE){ if(!mp_dict_del(AS_INST(argv[0])->dict,argv[1])) mp_raise_t(E_AttributeError,"'%s' object has no attribute '%s'",mp_type_name(argv[0]),mp_cstr(argv[1])); return v_none(); }
    mp_delattr(argv[0],argv[1]); return v_none(); }
static Value o_init_subclass(int argc, Value *argv, TupleObj *kw){
    const char *n= argc>0 && IS_TYPE(argv[0]) ? AS_TYPE(argv[0])->name->s : "object";
    if(kw && kw->len) mp_raise_t(E_TypeError,"%s.__init_subclass__() takes no keyword arguments",n);
    if(argc>1) mp_raise_t(E_TypeError,"%s.__init_subclass__() takes no arguments (%d given)",n,argc-1);
    return v_none(); }
/* object.__reduce_ex__ / __reduce__ / __getstate__: copyreg's _object_* functions (what pickle and copy use) */
static Value copyreg_call(const char *fn, int argc, Value *argv){
    Value nm=mp_str("copyreg"), mod=mp_builtin_import(1,&nm,NULL);
    return mp_call(mp_getattr_s(mod,fn),argc,argv,NULL);
}
static Value o_reduce_ex(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__reduce_ex__",argc,1,1); return copyreg_call("_object_reduce_ex",2,argv); }
static Value o_reduce(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__reduce__",argc,0,0); return copyreg_call("_object_reduce",1,argv); }
static Value o_getstate(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__getstate__",argc,0,0); return copyreg_call("_object_getstate",1,argv); }
static Value o_lt(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; (void)argv; return mp_NotImplemented; }
static Value e_init(int argc, Value *argv, TupleObj *kw){ ExcObj *x=AS_EXC(argv[0]); int np=argc-1-(kw?(int)kw->len:0);
    if(kw && kw->len){                                                  /* NameError(name=), AttributeError(name=, obj=), ImportError(name=, path=) */
        const char *ok1= mp_isinstance(argv[0],E_NameError)||mp_isinstance(argv[0],E_AttributeError)||mp_isinstance(argv[0],E_ImportError) ? "name" : NULL;
        const char *ok2= mp_isinstance(argv[0],E_AttributeError) ? "obj" : mp_isinstance(argv[0],E_ImportError) ? "path" : NULL;
        if(!x->dict) x->dict=AS_DICT(mp_dict());
        for(int64_t i=0;i<kw->len;i++){ const char *k=mp_cstr(kw->items[i]);
            if(!(ok1 && !strcmp(k,ok1)) && !(ok2 && !strcmp(k,ok2))){
                if(!ok1) mp_raise_t(E_TypeError,"%s() takes no keyword arguments",TYPE(argv[0])->name->s);
                mp_raise_t(E_TypeError,"%s() got an unexpected keyword argument '%s'",TYPE(argv[0])->name->s,k); }
            mp_dict_set(x->dict,kw->items[i],argv[1+np+i]); } }
    if(np>=3 && np<=5 && mp_isinstance(argv[0],E_OSError) && IS_INTLIKE(argv[1])){   /* OSError(errno, strerror, filename[, winerror, filename2]) */
        x->args=mp_tuple(2,argv+1);
        if(!x->dict) x->dict=AS_DICT(mp_dict());
        mp_dict_set_s(x->dict,"filename",argv[3]);
        if(np==5) mp_dict_set_s(x->dict,"filename2",argv[5]);
        return v_none(); }
    x->args=mp_tuple(np,argv+1); return v_none(); }
static Value e_str(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; return mp_exc_str(argv[0]); }
static Value e_repr(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; ExcObj *e=AS_EXC(argv[0]); SBuf b={0};
    sb_puts(&b,TYPE(argv[0])->name->s);
    Value r=mp_repr(e->args); StrObj *s=AS_STR(r);
    if(AS_TUPLE(e->args)->len==1){ sb_putc(&b,'('); Value a=mp_repr(AS_TUPLE(e->args)->items[0]); sb_put(&b,AS_STR(a)->s,AS_STR(a)->len); sb_putc(&b,')'); }
    else sb_put(&b,s->s,s->len);
    return sb_value(&b); }
static Value e_with_tb(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("with_traceback",argc,1,1); AS_EXC(argv[0])->tb=argv[1]; return argv[0]; }
static Value e_add_note(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("add_note",argc,1,1); ExcObj *e=AS_EXC(argv[0]);
    if(!IS_STR(argv[1])) mp_raise_t(E_TypeError,"note must be a str, not '%s'",mp_type_name(argv[1]));
    if(e->notes.k==V_UNDEF) e->notes=mp_list(0,NULL); mp_list_append(e->notes,argv[1]); return v_none(); }
static Value p_copy_with(Value p, int slot, Value f){           /* p.getter(f) / .setter(f) / .deleter(f): a copy with f */
    PropertyObj *o=(PropertyObj*)p.u.o, *nn=(PropertyObj*)mp_alloc(T_property,sizeof(PropertyObj));
    nn->get=o->get; nn->set=o->set; nn->del=o->del; nn->doc=o->doc;
    if(slot==0) nn->get=f; else if(slot==1) nn->set=f; else nn->del=f;
    return v_obj(nn);
}
static Value p_getter(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("getter",argc,1,1); return p_copy_with(argv[0],0,argv[1]); }
static Value p_setter(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("setter",argc,1,1); return p_copy_with(argv[0],1,argv[1]); }
static Value p_deleter(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("deleter",argc,1,1); return p_copy_with(argv[0],2,argv[1]); }
static Value p_get(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; PropertyObj *p=(PropertyObj*)argv[0].u.o; if(IS_NONE(argv[1])) return argv[0]; return mp_call1(p->get,argv[1]); }
/* property.__init__ / staticmethod.__init__ / classmethod.__init__ (of a subclass: super().__init__(...)) */
static Value p_init(int argc, Value *argv, TupleObj *kw){
    PropertyObj *p=(PropertyObj*)argv[0].u.o; int np=npos_of(argc,kw);
    p->get= np>1 ? argv[1] : kwarg(argc,argv,kw,"fget",v_none());
    p->set= np>2 ? argv[2] : kwarg(argc,argv,kw,"fset",v_none());
    p->del= np>3 ? argv[3] : kwarg(argc,argv,kw,"fdel",v_none());
    p->doc= np>4 ? argv[4] : kwarg(argc,argv,kw,"doc",v_none());
    if(IS_NONE(p->doc) && !IS_NONE(p->get)){ Value d; if(mp_getattr_opt(p->get,mp_intern("__doc__"),&d)) p->doc=d; }
    return v_none(); }
static Value t_subclasses(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__subclasses__",argc,0,0);
    if(!IS_TYPE(argv[0])) mp_raise_t(E_TypeError,"descriptor '__subclasses__' requires a 'type' object");
    Value s=AS_TYPE(argv[0])->subs; return s.k==V_OBJ ? mp_list(AS_LIST(s)->len,AS_LIST(s)->items) : mp_list(0,NULL); }
static Value o_subclasshook(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; (void)argv; return mp_NotImplemented; }
static Value f_call(int argc, Value *argv, TupleObj *kw){ if(argc<1) mp_raise_t(E_TypeError,"__call__ needs its object"); return mp_call(argv[0],argc-1,argv+1,kw); }
static Value box_init(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__init__",argc,1,1); ((BoxObj*)argv[0].u.o)->v=argv[1]; return v_none(); }
static Value p_set(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; PropertyObj *p=(PropertyObj*)argv[0].u.o; mp_call2(p->set,argv[1],argv[2]); return v_none(); }
/* the built-in types' own dunders (what super().__getitem__(k) ... of a subclass reach): the operation
   with the subclass's methods skipped for self (mp_raw_obj) */
enum { RW_GETITEM, RW_SETITEM, RW_DELITEM, RW_CONTAINS, RW_LEN, RW_ITER, RW_REPR, RW_EQ, RW_NE, RW_HASH, RW_STR };
static Value raw_op(int what, Value self, Value a, Value b){
    Obj *save=mp_raw_obj; Value r=v_none();
    if(self.k==V_OBJ) mp_raw_obj=self.u.o;
    Catch c;
    if(!CATCH_BEGIN(c)){
        switch(what){
            case RW_GETITEM: r=mp_getitem(self,a); break;
            case RW_SETITEM: mp_setitem(self,a,b); break;
            case RW_DELITEM: mp_delitem(self,a); break;
            case RW_CONTAINS: r=v_bool(mp_contains(self,a)); break;
            case RW_LEN: r=v_int(mp_len(self)); break;
            case RW_ITER: r=mp_iter(self); break;
            case RW_REPR: r=mp_repr(self); break;
            case RW_STR: r= self.k==V_OBJ && self.u.o->type->layout==LY_STR ? mp_strn(AS_STR(self)->s,AS_STR(self)->len) : mp_repr(self); break;
            case RW_EQ: case RW_NE:{ Value x=mp_unbox(self), y=mp_unbox(a);
                int same= x.k==V_OBJ && y.k==V_OBJ ? x.u.o->type->layout==y.u.o->type->layout : (x.k!=V_OBJ)==(y.k!=V_OBJ);
                r= same ? v_bool(mp_builtin_eq(x,y)==(what==RW_EQ)) : mp_NotImplemented; break; }
            case RW_HASH: r=v_int((int64_t)mp_hash(mp_unbox(self))); break;
        }
        CATCH_END(c);
    } else { mp_raw_obj=save; mp_raise(mp_catch_exc(&c)); }
    mp_raw_obj=save;
    return r;
}
static Value d_getitem(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__getitem__",argc,1,1); return raw_op(RW_GETITEM,argv[0],argv[1],v_none()); }
static Value d_setitem(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__setitem__",argc,2,2); return raw_op(RW_SETITEM,argv[0],argv[1],argv[2]); }
static Value d_delitem(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__delitem__",argc,1,1); return raw_op(RW_DELITEM,argv[0],argv[1],v_none()); }
static Value d_contains(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__contains__",argc,1,1); return raw_op(RW_CONTAINS,argv[0],argv[1],v_none()); }
static Value d_len(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__len__",argc,0,0); return raw_op(RW_LEN,argv[0],v_none(),v_none()); }
static Value d_iter(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__iter__",argc,0,0); return raw_op(RW_ITER,argv[0],v_none(),v_none()); }
static Value d_repr(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__repr__",argc,0,0); return raw_op(RW_REPR,argv[0],v_none(),v_none()); }
static Value d_str(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__str__",argc,0,0); return raw_op(RW_STR,argv[0],v_none(),v_none()); }
static Value d_eq(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__eq__",argc,1,1); return raw_op(RW_EQ,argv[0],argv[1],v_none()); }
static Value d_ne(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__ne__",argc,1,1); return raw_op(RW_NE,argv[0],argv[1],v_none()); }
static Value d_hash(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__hash__",argc,0,0); return raw_op(RW_HASH,argv[0],v_none(),v_none()); }
/* list / tuple / str / bytes + and *: of plain copies (a subclass's own dunders not asked) */
static Value plain_seq(Value v){
    if(v.k!=V_OBJ || !(v.u.o->type->flags&TF_HEAP)) return v;
    switch(v.u.o->type->layout){
        case LY_LIST: return mp_list(AS_LIST(v)->len,AS_LIST(v)->items);
        case LY_TUPLE: return mp_tuple(AS_TUPLE(v)->len,AS_TUPLE(v)->items);
        case LY_STR: return mp_strn(AS_STR(v)->s,AS_STR(v)->len);
        case LY_BYTES: return mp_bytes(AS_BYTES(v)->s,AS_BYTES(v)->len);
        default: return v;
    }
}
static Value s_add(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__add__",argc,1,1);
    Layout ls=argv[0].u.o->type->layout;
    const char *nm= ls==LY_LIST?"list":ls==LY_TUPLE?"tuple":"str";
    if(!(argv[1].k==V_OBJ && (argv[1].u.o->type->layout==ls || (ls==LY_BYTES && (IS(argv[1],T_buffer) || IS_BA(argv[1])))))){
        if(ls==LY_BYTES) mp_raise_t(E_TypeError,"can't concat %s to bytes",mp_type_name(argv[1]));
        mp_raise_t(E_TypeError,"can only concatenate %s (not \"%s\") to %s",nm,mp_type_name(argv[1]),nm); }
    return mp_binop(OP_Add,plain_seq(argv[0]),plain_seq(argv[1])); }
static Value s_mul(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__mul__",argc,1,1);
    int64_t n=mp_index(argv[1],"__mul__"); return mp_binop(OP_Mult,plain_seq(argv[0]),v_int(n)); }
static Value s_iadd(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__iadd__",argc,1,1);
    Value l=mp_list_of(argv[1]); for(int64_t i=0;i<AS_LIST(l)->len;i++) mp_list_append(argv[0],AS_LIST(l)->items[i]);
    return argv[0]; }
static Value s_reversed(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__reversed__",argc,0,0); return mp_reversed_builtin(argv[0]); }
static Value it_next(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__next__",argc,0,0);
    Value v; if(mp_next(argv[0],&v)) return v;
    mp_raise(mp_exc_args(E_StopIteration,mp_tuple(0,NULL))); }
/* int / float subclasses: the arithmetic of their values (NotImplemented for other kinds of operands) */
static Value num_dunder(int op, int reflected, int argc, Value *argv){
    nargs("operator",argc,1,1);
    Value x=mp_unbox(argv[0]), y=mp_unbox(argv[1]);
    int numy= y.k==V_INT || y.k==V_BOOL || y.k==V_FLOAT;
    if(!numy) return mp_NotImplemented;
    return reflected ? mp_binop(op,y,x) : mp_binop(op,x,y);
}
#define NUMD(name,op,refl) static Value name(int argc, Value *argv, TupleObj *kw){ (void)kw; return num_dunder(op,refl,argc,argv); }
NUMD(n_add,OP_Add,0) NUMD(n_radd,OP_Add,1) NUMD(n_sub,OP_Sub,0) NUMD(n_rsub,OP_Sub,1) NUMD(n_mul,OP_Mult,0) NUMD(n_rmul,OP_Mult,1)
NUMD(n_truediv,OP_Div,0) NUMD(n_rtruediv,OP_Div,1) NUMD(n_floordiv,OP_FloorDiv,0) NUMD(n_rfloordiv,OP_FloorDiv,1)
NUMD(n_mod,OP_Mod,0) NUMD(n_rmod,OP_Mod,1) NUMD(n_pow,OP_Pow,0) NUMD(n_rpow,OP_Pow,1)
NUMD(n_and,OP_BitAnd,0) NUMD(n_rand,OP_BitAnd,1) NUMD(n_or,OP_BitOr,0) NUMD(n_ror,OP_BitOr,1) NUMD(n_xor,OP_BitXor,0) NUMD(n_rxor,OP_BitXor,1)
NUMD(n_lshift,OP_LShift,0) NUMD(n_rlshift,OP_LShift,1) NUMD(n_rshift,OP_RShift,0) NUMD(n_rrshift,OP_RShift,1)
static Value n_cmp(int op, int argc, Value *argv){ nargs("comparison",argc,1,1); Value x=mp_unbox(argv[0]), y=mp_unbox(argv[1]);
    if(!(y.k==V_INT || y.k==V_BOOL || y.k==V_FLOAT)) return mp_NotImplemented; return mp_compare(op,x,y); }
static Value n_lt(int argc, Value *argv, TupleObj *kw){ (void)kw; return n_cmp(OP_Lt,argc,argv); }
static Value n_le(int argc, Value *argv, TupleObj *kw){ (void)kw; return n_cmp(OP_LtE,argc,argv); }
static Value n_gt(int argc, Value *argv, TupleObj *kw){ (void)kw; return n_cmp(OP_Gt,argc,argv); }
static Value n_ge(int argc, Value *argv, TupleObj *kw){ (void)kw; return n_cmp(OP_GtE,argc,argv); }
static Value n_neg(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__neg__",argc,0,0); return mp_unary(OP_USub,mp_unbox(argv[0])); }
static Value n_pos(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__pos__",argc,0,0); return mp_unary(OP_UAdd,mp_unbox(argv[0])); }
static Value n_invert(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__invert__",argc,0,0); return mp_unary(OP_Invert,mp_unbox(argv[0])); }
static Value n_abs(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__abs__",argc,0,0); Value x=mp_unbox(argv[0]);
    if(x.k==V_FLOAT) return v_float(fabs(x.u.f)); return x.u.i<0 ? mp_unary(OP_USub,x) : v_int(x.u.i); }
static Value n_int(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__int__",argc,0,0); Value x=mp_unbox(argv[0]);
    if(x.k==V_FLOAT) return T_int->make(T_int,1,&x,NULL); return v_int(x.u.i); }
static Value n_float(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__float__",argc,0,0); return v_float(mp_float_of(mp_unbox(argv[0]))); }
static Value n_bool(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__bool__",argc,0,0); return v_bool(mp_truth(mp_unbox(argv[0]))); }
static Value n_format(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__format__",argc,1,1); return mp_format(mp_unbox(argv[0]),argv[1]); }
/* type.__setattr__ / type.__delattr__ (a metaclass's super().__setattr__): the class's own dict */
static Value t_setattr(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__setattr__",argc,2,2);
    if(!IS_TYPE(argv[0])) mp_raise_t(E_TypeError,"descriptor '__setattr__' requires a 'type' object");
    Obj *save=mp_raw_obj; mp_raw_obj=argv[0].u.o; Catch c;
    if(!CATCH_BEGIN(c)){ mp_setattr(argv[0],argv[1],argv[2]); CATCH_END(c); } else { mp_raw_obj=save; mp_raise(mp_catch_exc(&c)); }
    mp_raw_obj=save; return v_none(); }
static Value t_delattr(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__delattr__",argc,1,1);
    if(!IS_TYPE(argv[0])) mp_raise_t(E_TypeError,"descriptor '__delattr__' requires a 'type' object");
    Obj *save=mp_raw_obj; mp_raw_obj=argv[0].u.o; Catch c;
    if(!CATCH_BEGIN(c)){ mp_delattr(argv[0],argv[1]); CATCH_END(c); } else { mp_raw_obj=save; mp_raise(mp_catch_exc(&c)); }
    mp_raw_obj=save; return v_none(); }
/* the descriptor protocol of functions, staticmethod and classmethod objects: f.__get__(obj, type) */
static Value fn_get(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__get__",argc,1,2);
    if(IS_NONE(argv[1])) return argv[0];
    MethodObj *m=(MethodObj*)mp_alloc(T_method,sizeof(MethodObj)); m->func=argv[0]; m->self=argv[1]; return v_obj(m); }
static Value sm_get(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__get__",argc,1,2); return ((BoxObj*)argv[0].u.o)->v; }
static Value cm_get(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__get__",argc,1,2);
    Value owner= argc>2 && !IS_NONE(argv[2]) ? argv[2] : v_obj(TYPE(argv[1]));
    MethodObj *m=(MethodObj*)mp_alloc(T_method,sizeof(MethodObj)); m->func=((BoxObj*)argv[0].u.o)->v; m->self=owner; return v_obj(m); }
/* list.__init__(self[, iterable]) / dict.__init__(self, ...) / set.__init__(self[, iterable]): (re)fill self */
static Value m_list_init(int argc, Value *argv, TupleObj *kw){ if(kw && kw->len) mp_raise_t(E_TypeError,"list() takes no keyword arguments");
    if(argc>2) mp_raise_t(E_TypeError,"list expected at most 1 argument, got %d",argc-1);
    ListObj *l=AS_LIST(argv[0]); l->len=0;
    if(argc==2){ Value it=mp_iter(argv[1]), x; while(mp_next(it,&x)) mp_list_append(argv[0],x); }
    return v_none(); }
static Value m_dict_init(int argc, Value *argv, TupleObj *kw){
    if(argc-(kw?(int)kw->len:0)>2) mp_raise_t(E_TypeError,"dict expected at most 1 argument, got %d",argc-1-(kw?(int)kw->len:0));
    Value keep=mp_tuple(argc-1,argv+1);
    Value plain=T_dict->make(T_dict,argc-1,AS_TUPLE(keep)->items,kw);
    int64_t pos=0; Value k, v; while(mp_dict_next(AS_DICT(plain),&pos,&k,&v)) mp_dict_set(AS_DICT(argv[0]),k,v);
    return v_none(); }
static Value m_set_init(int argc, Value *argv, TupleObj *kw){ if(kw && kw->len) mp_raise_t(E_TypeError,"set() takes no keyword arguments");
    if(argc>2) mp_raise_t(E_TypeError,"set expected at most 1 argument, got %d",argc-1);
    SetObj *s=(SetObj*)argv[0].u.o;
    if(argc==2){ Value plain=T_set->make(T_set,1,argv+1,NULL); mp_set_merge(s,(SetObj*)plain.u.o); }
    return v_none(); }
static Value t_mro(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; Type *t=AS_TYPE(argv[0]); Value l=mp_list(0,NULL); for(int i=0;i<t->nmro;i++) mp_list_append(l,v_obj(t->mro[i])); return l; }
static Value t_or(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; Value u[2]={argv[0],argv[1]}; return mp_tuple(2,u); }   /* int | str: a tuple isinstance understands */

/* ---------------------------------------------------------------- files */
static FileObj *F(Value v){ FileObj *f=(FileObj*)v.u.o; return f; }
static void need_open(FileObj *f){ if(f->closed) mp_raise_t(E_ValueError,"I/O operation on closed file."); }

static Value m_read(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("read",argc,0,1); FileObj *f=F(SELF); need_open(f);
    if(f->mode!='r') mp_raise_t(E_OSError,"not readable");
    int64_t n= argc>1 && !IS_NONE(argv[1]) ? mp_index(argv[1],"size") : -1;
    int64_t rest=f->len-f->pos;
    if(f->binary){ if(n<0||n>rest) n=rest; Value r=mp_bytes(f->buf+f->pos,n); f->pos+=n; return r; }
    if(n<0 || n>=rest){ Value r=mp_strn(f->buf+f->pos,rest); f->pos=f->len; return r; }
    int64_t p=f->pos; for(int64_t i=0;i<n && p<f->len;i++) mp_utf8_decode(f->buf,f->len,&p);
    Value r=mp_strn(f->buf+f->pos,p-f->pos); f->pos=p; return r; }
int mp_file_readline(Value fv, Value *out){
    FileObj *f=F(fv); need_open(f);
    if(f->pos>=f->len) return 0;
    int64_t e=f->pos; while(e<f->len && f->buf[e]!='\n') e++;
    if(e<f->len) e++;
    *out= f->binary ? mp_bytes(f->buf+f->pos,e-f->pos) : mp_strn(f->buf+f->pos,e-f->pos);
    f->pos=e; return 1;
}
static Value m_readline(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; Value v; if(!mp_file_readline(SELF,&v)) return F(SELF)->binary?mp_bytes("",0):mp_str(""); return v; }
static Value m_readlines(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; Value l=mp_list(0,NULL), v; while(mp_file_readline(SELF,&v)) mp_list_append(l,v); return l; }
static Value m_write(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("write",argc,1,1); FileObj *f=F(SELF); need_open(f);
    if(f->mode=='r') mp_raise_t(E_OSError,"not writable");
    const char *s; int64_t n;
    if(f->binary){ char one; bytes_arg(argv[1],0,&s,&n,&one); }
    else { if(!IS_STR(argv[1])) mp_raise_t(E_TypeError,"write() argument must be str, not %s",mp_type_name(argv[1])); s=AS_STR(argv[1])->s; n=AS_STR(argv[1])->len; }
    if(f->len+n+1>f->cap){ f->cap=(f->len+n+1)*2; f->buf=(char*)xrealloc(f->buf,(size_t)f->cap); }
    memcpy(f->buf+f->len,s,(size_t)n); f->len+=n;
    return v_int(f->binary ? n : AS_STR(argv[1])->cplen); }
static Value m_writelines(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("writelines",argc,1,1); Value it=mp_iter(argv[1]), x; while(mp_next(it,&x)){ Value a[2]={SELF,x}; m_write(2,a,NULL); } return v_none(); }
static Value m_fclose(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; FileObj *f=F(SELF); if(!f->closed){ mp_file_flush(f); f->closed=1; } return v_none(); }
static Value m_fflush(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; need_open(F(SELF)); return v_none(); }
static Value m_fexit(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; m_fclose(1,argv,NULL); return v_bool(0); }
static Value m_fnext(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; Value v; if(!mp_file_readline(SELF,&v)) mp_raise(mp_exc_args(E_StopIteration,mp_tuple(0,NULL))); return v; }
static Value m_seek(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("seek",argc,1,2); FileObj *f=F(SELF); int64_t p=mp_index(argv[1],"offset"), w= argc>2 ? mp_index(argv[2],"whence") : 0;
    if(w==1) p+=f->pos; else if(w==2) p+=f->len; if(p<0) p=0; if(p>f->len) p=f->len; f->pos=p; return v_int(p); }
static Value m_tell(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; return v_int(F(SELF)->pos); }
/* the methods of generators and coroutines, the dunders of the built-in types (for
   super()), object, BaseException, property, type and files (mp_methods_init) */
void mp_object_methods_init(void){
    Type *t;
    for(int k=0;k<2;k++){ t= k==0 ? T_generator : T_coroutine;
        mp_type_add(t,"send",m_send); mp_type_add(t,"throw",m_throw); mp_type_add(t,"close",m_close); }
    mp_asyncgen_init();
    mp_type_add(T_generator,"__next__",m_gnext); mp_type_add(T_generator,"__iter__",m_self);
    mp_type_add(T_coroutine,"__await__",m_self);
    {                                                       /* the built-in value types' __new__ (super().__new__(cls, ...)) and */
        Type *vt[]={T_str,T_bytes,T_tuple,T_list,T_dict,T_set,T_frozenset,T_int,T_float,NULL};   /* list/dict/set.__init__ */
        for(int k=0;vt[k];k++){ Value nw=mp_native("__new__",mp_builtin_new); BoxObj *sm=(BoxObj*)mp_alloc(T_staticmethod,sizeof(BoxObj)); sm->v=nw;
            mp_dict_set(vt[k]->dict,mp_intern("__new__"),v_obj(sm)); }
        mp_type_add(T_list,"__init__",m_list_init); mp_type_add(T_dict,"__init__",m_dict_init); mp_type_add(T_set,"__init__",m_set_init);
        mp_type_add(T_type,"__setattr__",t_setattr); mp_type_add(T_type,"__delattr__",t_delattr);
        mp_type_add(T_function,"__get__",fn_get); mp_type_add(T_staticmethod,"__get__",sm_get); mp_type_add(T_classmethod,"__get__",cm_get);
        Type *ct[]={T_str,T_bytes,T_tuple,T_list,T_dict,T_set,T_frozenset,T_bytearray,NULL};       /* their own dunders, for super() */
        for(int k=0;ct[k];k++){ Type *x=ct[k];
            mp_type_add(x,"__len__",d_len); mp_type_add(x,"__iter__",d_iter); mp_type_add(x,"__contains__",d_contains);
            mp_type_add(x,"__repr__",d_repr); mp_type_add(x,"__eq__",d_eq); mp_type_add(x,"__ne__",d_ne);
            if(x!=T_set && x!=T_frozenset) mp_type_add(x,"__getitem__",d_getitem);
            if(x==T_list || x==T_dict || x==T_bytearray){ mp_type_add(x,"__setitem__",d_setitem); mp_type_add(x,"__delitem__",d_delitem); }
            if(x==T_str || x==T_bytes || x==T_tuple || x==T_frozenset) mp_type_add(x,"__hash__",d_hash); }
        mp_type_add(T_str,"__str__",d_str);
        mp_dict_set_s(T_list->dict,"__hash__",v_none()); mp_dict_set_s(T_dict->dict,"__hash__",v_none()); mp_dict_set_s(T_set->dict,"__hash__",v_none());
        Type *sq[]={T_str,T_bytes,T_tuple,T_list,NULL};
        for(int k=0;sq[k];k++){ mp_type_add(sq[k],"__add__",s_add); mp_type_add(sq[k],"__mul__",s_mul); mp_type_add(sq[k],"__rmul__",s_mul); }
        mp_type_add(T_list,"__iadd__",s_iadd);
        Type *rv[]={T_list,T_dict,T_range,T_dict_keys,T_dict_values,T_dict_items,NULL};
        for(int k=0;rv[k];k++) mp_type_add(rv[k],"__reversed__",s_reversed);
        Type *vw[]={T_range,T_dict_keys,T_dict_values,T_dict_items,NULL};
        for(int k=0;vw[k];k++){ mp_type_add(vw[k],"__len__",d_len); mp_type_add(vw[k],"__iter__",d_iter); mp_type_add(vw[k],"__contains__",d_contains); }
        mp_type_add(T_range,"__getitem__",d_getitem);
        mp_type_add(T_iter,"__iter__",m_self); mp_type_add(T_iter,"__next__",it_next);
        Type *nt[]={T_int,T_float,NULL};
        for(int k=0;nt[k];k++){ Type *x=nt[k];
            mp_type_add(x,"__add__",n_add); mp_type_add(x,"__radd__",n_radd); mp_type_add(x,"__sub__",n_sub); mp_type_add(x,"__rsub__",n_rsub);
            mp_type_add(x,"__mul__",n_mul); mp_type_add(x,"__rmul__",n_rmul); mp_type_add(x,"__truediv__",n_truediv); mp_type_add(x,"__rtruediv__",n_rtruediv);
            mp_type_add(x,"__floordiv__",n_floordiv); mp_type_add(x,"__rfloordiv__",n_rfloordiv); mp_type_add(x,"__mod__",n_mod); mp_type_add(x,"__rmod__",n_rmod);
            mp_type_add(x,"__pow__",n_pow); mp_type_add(x,"__rpow__",n_rpow);
            mp_type_add(x,"__lt__",n_lt); mp_type_add(x,"__le__",n_le); mp_type_add(x,"__gt__",n_gt); mp_type_add(x,"__ge__",n_ge);
            mp_type_add(x,"__eq__",d_eq); mp_type_add(x,"__ne__",d_ne); mp_type_add(x,"__hash__",d_hash); mp_type_add(x,"__repr__",d_repr);
            mp_type_add(x,"__neg__",n_neg); mp_type_add(x,"__pos__",n_pos); mp_type_add(x,"__abs__",n_abs); mp_type_add(x,"__bool__",n_bool);
            mp_type_add(x,"__int__",n_int); mp_type_add(x,"__float__",n_float); mp_type_add(x,"__format__",n_format); }
        mp_type_add(T_int,"__and__",n_and); mp_type_add(T_int,"__rand__",n_rand); mp_type_add(T_int,"__or__",n_or); mp_type_add(T_int,"__ror__",n_ror);
        mp_type_add(T_int,"__xor__",n_xor); mp_type_add(T_int,"__rxor__",n_rxor); mp_type_add(T_int,"__lshift__",n_lshift); mp_type_add(T_int,"__rlshift__",n_rlshift);
        mp_type_add(T_int,"__rshift__",n_rshift); mp_type_add(T_int,"__rrshift__",n_rrshift); mp_type_add(T_int,"__invert__",n_invert); mp_type_add(T_int,"__index__",n_int);
    }
    t=T_object;
    mp_type_add(t,"__init__",o_init); mp_type_add(t,"__eq__",o_eq); mp_type_add(t,"__ne__",o_ne); mp_type_add(t,"__hash__",o_hash);
    mp_type_add(t,"__repr__",o_repr); mp_type_add(t,"__str__",o_str); mp_type_add(t,"__format__",o_format); mp_type_add(t,"__setattr__",o_setattr);
    mp_type_add(t,"__getattribute__",o_getattribute); mp_type_add(t,"__delattr__",o_delattr);
    mp_type_add(t,"__reduce_ex__",o_reduce_ex); mp_type_add(t,"__reduce__",o_reduce); mp_type_add(t,"__getstate__",o_getstate);
    mp_type_add(t,"__lt__",o_lt); mp_type_add(t,"__le__",o_lt); mp_type_add(t,"__gt__",o_lt); mp_type_add(t,"__ge__",o_lt);
    { Value nw=mp_native("__new__",o_new); BoxObj *sm=(BoxObj*)mp_alloc(T_staticmethod,sizeof(BoxObj)); sm->v=nw; mp_dict_set(t->dict,mp_intern("__new__"),v_obj(sm)); }
    { Value sh=mp_native("__subclasshook__",o_subclasshook); BoxObj *cm=(BoxObj*)mp_alloc(T_classmethod,sizeof(BoxObj)); cm->v=sh; mp_dict_set(t->dict,mp_intern("__subclasshook__"),v_obj(cm)); }
    mp_type_add(T_function,"__call__",f_call); mp_type_add(T_native,"__call__",f_call); mp_type_add(T_method,"__call__",f_call);
    { Value is=mp_native("__init_subclass__",o_init_subclass); BoxObj *cm=(BoxObj*)mp_alloc(T_classmethod,sizeof(BoxObj)); cm->v=is; mp_dict_set(t->dict,mp_intern("__init_subclass__"),v_obj(cm)); }
    t=E_BaseException;
    mp_type_add(t,"__init__",e_init); mp_type_add(t,"__str__",e_str); mp_type_add(t,"__repr__",e_repr); mp_type_add(t,"with_traceback",e_with_tb); mp_type_add(t,"add_note",e_add_note);
    mp_type_add(T_property,"__init__",p_init); mp_type_add(T_staticmethod,"__init__",box_init); mp_type_add(T_classmethod,"__init__",box_init);
    t=T_property; mp_type_add(t,"getter",p_getter); mp_type_add(t,"setter",p_setter); mp_type_add(t,"deleter",p_deleter); mp_type_add(t,"__get__",p_get); mp_type_add(t,"__set__",p_set);
    mp_type_add(T_type,"__subclasses__",t_subclasses);
    mp_type_add(T_type,"mro",t_mro); mp_type_add(T_type,"__or__",t_or); mp_type_add(T_type,"__ror__",t_or);
    { Value tn=mp_native("__new__",mp_type_new_m); mp_dict_set_s(T_type->dict,"__new__",tn);       /* (static: super().__new__(mcls, ...)) */
      mp_type_add(T_type,"__init__",mp_type_init_m); mp_type_add(T_type,"__call__",mp_type_call_m); }
    t=T_file;
    mp_type_add(t,"read",m_read); mp_type_add(t,"readline",m_readline); mp_type_add(t,"readlines",m_readlines); mp_type_add(t,"write",m_write); mp_type_add(t,"writelines",m_writelines);
    mp_type_add(t,"close",m_fclose); mp_type_add(t,"flush",m_fflush); mp_type_add(t,"__enter__",m_self); mp_type_add(t,"__exit__",m_fexit);
    mp_type_add(t,"__iter__",m_self); mp_type_add(t,"__next__",m_fnext); mp_type_add(t,"seek",m_seek); mp_type_add(t,"tell",m_tell);
}

