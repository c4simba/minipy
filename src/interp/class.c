/* ========================= Interpreter: classes =========================
   Instances, subclasses of built-in types, the C3 MRO, making classes
   (metaclasses, __init_subclass__, __set_name__). */

#include "run.h"

/* a class with abstract methods left (abc: __abstractmethods__) cannot be instantiated */
static void check_abstract(Type *t){
    Value am=mp_type_lookup_s(t,"__abstractmethods__");
    if(am.k!=V_OBJ || !mp_truth(am)) return;
    Value l=mp_list_of(am); mp_sort(l,v_none(),0);
    SBuf b={0}; ListObj *x=AS_LIST(l);
    for(int64_t i=0;i<x->len;i++){ if(i) sb_puts(&b,", "); sb_putc(&b,'\''); if(IS_STR(x->items[i])) sb_puts(&b,mp_cstr(x->items[i])); sb_putc(&b,'\''); }
    sb_putc(&b,0);
    char msg[1024]; snprintf(msg,sizeof msg,"%s",b.s); free(b.s);
    mp_raise_t(E_TypeError,"Can't instantiate abstract class %s without an implementation for abstract method%s %s",t->name->s,x->len>1?"s":"",msg);
}
Value mp_instance(Type *t){
    if(t->flags&TF_HEAP) check_abstract(t);
    if(t->layout==LY_EXC){ Value e=mp_exc_args(t,mp_tuple(0,NULL)); return e; }
    InstObj *o=(InstObj*)mp_alloc(t,sizeof(InstObj));
    o->dict=AS_DICT(mp_dict());
    return v_obj(o);
}
/* OSError(errno, ...): the subclass for that error number, as CPython's OSError.__new__
   picks it: the host's numbers (KolibriOS: Linux's, which the standard library uses there) */
static Type *oserror_subtype(int64_t no){
#if defined(PPY_KOLIBRI)
    static const struct { int no; const char *name; } map[]={
        {11,"BlockingIOError"},{114,"BlockingIOError"},{115,"BlockingIOError"},{10,"ChildProcessError"},{32,"BrokenPipeError"},{108,"BrokenPipeError"},
        {103,"ConnectionAbortedError"},{111,"ConnectionRefusedError"},{104,"ConnectionResetError"},{17,"FileExistsError"},{2,"FileNotFoundError"},
        {21,"IsADirectoryError"},{20,"NotADirectoryError"},{4,"InterruptedError"},{13,"PermissionError"},{1,"PermissionError"},{3,"ProcessLookupError"},
        {110,"TimeoutError"},{0,NULL}};
#else
    static const struct { int no; const char *name; } map[]={
        {EAGAIN,"BlockingIOError"},{EALREADY,"BlockingIOError"},{EINPROGRESS,"BlockingIOError"},{ECHILD,"ChildProcessError"},{EPIPE,"BrokenPipeError"},
#ifdef ESHUTDOWN
        {ESHUTDOWN,"BrokenPipeError"},
#endif
#ifdef ENOTCAPABLE
        {ENOTCAPABLE,"PermissionError"},
#endif
        {ECONNABORTED,"ConnectionAbortedError"},{ECONNREFUSED,"ConnectionRefusedError"},{ECONNRESET,"ConnectionResetError"},{EEXIST,"FileExistsError"},
        {ENOENT,"FileNotFoundError"},{EISDIR,"IsADirectoryError"},{ENOTDIR,"NotADirectoryError"},{EINTR,"InterruptedError"},{EACCES,"PermissionError"},
        {EPERM,"PermissionError"},{ESRCH,"ProcessLookupError"},{ETIMEDOUT,"TimeoutError"},{0,NULL}};
#endif
    for(int i=0;map[i].name;i++) if(map[i].no==no){ Value t; if(mp_dict_get_s(mp_builtins,map[i].name,&t) && IS(t,T_type)) return AS_TYPE(t); }
    return NULL;
}
Type *mp_value_base(Type *t){
    for(int i=0;i<t->nmro;i++){ Type *m=t->mro[i];
        if(m==T_str||m==T_bytes||m==T_tuple||m==T_list||m==T_dict||m==T_set||m==T_frozenset||m==T_int||m==T_float
           ||m==T_property||m==T_staticmethod||m==T_classmethod||m==T_bytearray) return m; }
    return NULL;
}
static int user_fn(Value m){ return m.k!=V_UNDEF && !IS(m,T_native) && !(IS(m,T_staticmethod) && IS(((BoxObj*)m.u.o)->v,T_native)); }
/* a new object of t (a subclass of a built-in value type, or one): from args, or empty (list, dict, set: their __init__ fills them) */
static Value subval_new(Type *t, int argc, Value *argv, TupleObj *kw, int fill){
    Type *b=mp_value_base(t);
    if(!b) mp_raise_t(E_TypeError,"%s is not a subtype of a built-in type",t->name->s);
    int mut= b==T_list||b==T_dict||b==T_set||b==T_property||b==T_staticmethod||b==T_classmethod||b==T_bytearray;
    Value plain= mut && !fill ? b->make(b,0,NULL,NULL) : b->make(b,argc,argv,kw);
    if(t==b) return plain;
    return mp_sub_value(t,mp_unbox(plain));
}
Value mp_builtin_new(int argc, Value *argv, TupleObj *kw){
    if(argc<1 || !IS_TYPE(argv[0])) mp_raise_t(E_TypeError,"__new__(X): X is not a type object");
    return subval_new(AS_TYPE(argv[0]),argc-1,argv+1,kw,0);
}
/* calling a subclass of a built-in value type: __new__ (its own, or the built-in's), then __init__ */
Value subval_call(Type *c, int argc, Value *argv, TupleObj *kw){
    Value nw=mp_type_lookup_s(c,"__new__"), init=mp_type_lookup_s(c,"__init__"), self;
    int uinit=user_fn(init);
    if(user_fn(nw)){
        Value keep=mp_tuple(argc+1,NULL); AS_TUPLE(keep)->items[0]=v_obj(c); memcpy(AS_TUPLE(keep)->items+1,argv,sizeof(Value)*(size_t)argc);
        Value fn= IS(nw,T_staticmethod) ? ((BoxObj*)nw.u.o)->v : nw;
        self=mp_call(fn,argc+1,AS_TUPLE(keep)->items,kw);
    } else self=subval_new(c,argc,argv,kw,!uinit);
    if(uinit && mp_isinstance(self,c)){
        Value keep=mp_tuple(argc+1,NULL); AS_TUPLE(keep)->items[0]=self; memcpy(AS_TUPLE(keep)->items+1,argv,sizeof(Value)*(size_t)argc);
        Value r=mp_call(init,argc+1,AS_TUPLE(keep)->items,kw);
        if(!IS_NONE(r)) mp_raise_t(E_TypeError,"__init__() should return None, not '%s'",mp_type_name(r));
    }
    return self;
}
Value mp_instance_call(Type *t, int argc, Value *argv, TupleObj *kw){
    if(mp_is_subtype(t,E_BaseExceptionGroup)) mp_excgroup(NULL);     /* (its methods, the first time) */
    if(t==E_OSError && argc>=2 && IS_INTLIKE(argv[0])){ Type *sub=oserror_subtype(argv[0].u.i); if(sub) t=sub; }
    Value nw=mp_type_lookup_s(t,"__new__");
    Value self;
    if(nw.k!=V_UNDEF && !IS(nw,T_native)){
        Value keep=mp_tuple(argc+1,NULL); AS_TUPLE(keep)->items[0]=v_obj(t); memcpy(AS_TUPLE(keep)->items+1,argv,sizeof(Value)*(size_t)argc);
        Value fn= IS(nw,T_staticmethod) ? ((BoxObj*)nw.u.o)->v : nw;
        self=mp_call(fn,argc+1,AS_TUPLE(keep)->items,kw);
        if(!mp_isinstance(self,t)) return self;
    } else {
        int np=argc-(kw?(int)kw->len:0);
        self=mp_instance(t);
        if(t->layout==LY_EXC) AS_EXC(self)->args=mp_tuple(np,argv);           /* (BaseException.__init__ sets them again) */
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
    free(t->mro); t->mro=(Type**)xrealloc(out,sizeof(Type*)*(size_t)n); t->nmro=n;
    free(seqs[nb]); free(seqs); free(lens); free(pos);
}
void mp_type_finish(Type *t){ compute_mro(t); }
/* base->subs gets t (__subclasses__); the built-in types made before list: when list is there */
static Type *pend_base[64], *pend_sub[64]; static int npend;
static void note_subclass(Type *base, Type *t){
    if(!T_list){ if(npend<64){ pend_base[npend]=base; pend_sub[npend]=t; npend++; } return; }
    for(int i=0;i<npend;i++){ Type *b=pend_base[i]; if(b->subs.k!=V_OBJ) b->subs=mp_list(0,NULL); mp_list_append(b->subs,v_obj(pend_sub[i])); }
    npend=0;
    if(base->subs.k!=V_OBJ) base->subs=mp_list(0,NULL); mp_list_append(base->subs,v_obj(t));
}
Value mp_make_class(Value name, Value bases, DictObj *ns){ return mp_make_class_kw(T_type,name,bases,ns,0,NULL,NULL); }
Value mp_make_class_kw(Type *meta, Value name, Value bases, DictObj *ns, int nkw, Value *kwv, TupleObj *kwn){
    TupleObj *b=AS_TUPLE(bases);
    for(int64_t i=0;i<b->len;i++){
        if(!IS_TYPE(b->items[i])) mp_raise_t(E_TypeError,"bases must be types");
        Type *x=AS_TYPE(b->items[i]);
        if(!(x->flags&TF_BASETYPE)) mp_raise_t(E_TypeError,"type '%s' is not an acceptable base type",x->name->s);
    }
    if(b->len==0){ Value o=v_obj(T_object); bases=mp_tuple(1,&o); b=AS_TUPLE(bases); }
    Type *t=(Type*)mp_alloc(meta,sizeof(Type));
    t->name=AS_STR(name); t->bases=b; t->dict=ns; t->flags=TF_BASETYPE|TF_HEAP|TF_DUNDERS;
    { static const char *const implicit[]={"__init_subclass__","__class_getitem__"};   /* plain functions: classmethods (as CPython) */
      for(int i=0;i<2;i++){ Value f; if(mp_dict_get_s(ns,implicit[i],&f) && IS(f,T_function)){
          BoxObj *cm=(BoxObj*)mp_alloc(T_classmethod,sizeof(BoxObj)); cm->v=f; mp_dict_set(ns,mp_intern(implicit[i]),v_obj(cm)); } } }
    /* the layout: the most derived non-object one among the bases */
    Type *solid=T_object;
    for(int64_t i=0;i<b->len;i++){
        Type *x=AS_TYPE(b->items[i]), *s=x;
        while(s && s->layout==LY_INSTANCE && s!=T_object && (s->flags&TF_HEAP)) s=s->base;
        if(!s) s=T_object;
        if(s->layout==LY_EXC) s=E_BaseException;              /* (the exceptions share one layout: class E(ValueError, IndexError)) */
        if(s!=T_object){
            if(solid==T_object || mp_is_subtype(s,solid)) solid=s;
            else if(!mp_is_subtype(solid,s)) mp_raise_t(E_TypeError,"multiple bases have instance lay-out conflict");
        }
    }
    t->base=AS_TYPE(b->items[0]);
    for(int64_t i=0;i<b->len;i++) note_subclass(AS_TYPE(b->items[i]),t);
    t->layout= solid==T_object ? LY_INSTANCE : solid->layout;
    if(t->layout!=LY_INSTANCE && t->layout!=LY_EXC) t->make=solid->make;
    compute_mro(t);
    { Type *vb=mp_value_base(t);                             /* class S(str), class D(dict), class I(int) ... */
      if(vb){ t->flags|=TF_SUBVAL; t->layout= vb==T_int||vb==T_float ? LY_NUM : vb->layout; t->make=vb->make; } }
    Value q;
    if(mp_dict_get_s(ns,"__qualname__",&q) && IS_STR(q)){ t->qualname=AS_STR(q); mp_dict_del(ns,mp_intern("__qualname__")); }
    else t->qualname=t->name;
    /* __init_subclass__ of the bases, __set_name__ of the attributes */
    { Value snap=mp_list(0,NULL);                            /* (a copy: __set_name__ may add attributes, as an enum's members) */
      int64_t pos=0; Value k, v;
      while(mp_dict_next(ns,&pos,&k,&v)) if(v.k==V_OBJ && (v.u.o->type->flags&TF_DUNDERS)){ mp_list_append(snap,k); mp_list_append(snap,v); }
      for(int64_t i=0;i+1<AS_LIST(snap)->len;i+=2){ Value sk=AS_LIST(snap)->items[i], sv=AS_LIST(snap)->items[i+1];
          Value sn=mp_type_lookup_s(sv.u.o->type,"__set_name__");
          if(sn.k!=V_UNDEF){ Value a[3]={sv,v_obj(t),sk}; mp_call(sn,3,a,NULL); } } }
    for(int i=1;i<t->nmro;i++){                              /* super(t, t).__init_subclass__(**kwargs) */
        Value isc; if(mp_tdict(t->mro[i]) && mp_dict_get_s(t->mro[i]->dict,"__init_subclass__",&isc)){
            Value keep=mp_tuple(nkw,kwv);                        /* (kept from the collector) */
            mp_call(descr_get(isc,v_obj(t),t),nkw,AS_TUPLE(keep)->items,kwn);
            break;
        }
    }
    return v_obj(t);
}
Value mp_new_type(const char *name, Type *base, Layout layout, int flags){
    Type *t=(Type*)mp_alloc(T_type,sizeof(Type));
    t->name=AS_STR(mp_intern(name)); t->qualname=t->name; t->base=base; t->layout=layout; t->flags=flags|TF_BUILTIN;
    t->dict=AS_DICT(mp_dict());
    if(base){ if(base->make && layout==base->layout) t->make=base->make; note_subclass(base,t); }
    /* (one base: the MRO is it and the base's; __bases__ is made when it is asked for - mp_type_bases) */
    t->nmro= base ? base->nmro+1 : 1; t->mro=(Type**)xmalloc(sizeof(Type*)*(size_t)t->nmro);
    t->mro[0]=t; for(int i=1;i<t->nmro;i++) t->mro[i]=base->mro[i-1];
    return v_obj(t);
}
/* methods of built-in types: (name, function) here until the type's dict is first read, in runs
   of one type each (mp_type_add is called type after type) */
typedef struct { const char *name; NFn fn; } LazyMethod;
typedef struct { Type *t; int first, n; } LazyRun;
static LazyMethod *lazy; static int nlazy, clazy;
static LazyRun *runs; static int nruns, cruns;
static int has_key(DictObj *d, const char *name){       /* (no str made for the question) */
    int64_t pos=0; Value k;
    while(mp_dict_next(d,&pos,&k,NULL)) if(IS_STR(k) && !strcmp(AS_STR(k)->s,name)) return 1;
    return 0;
}
void mp_type_add(Type *t, const char *name, NFn fn){
    if(t->dict && has_key(t->dict,name)){                /* (one set before: replaced now, as it always was) */
        Value n=mp_native(name,fn); ((NativeObj*)n.u.o)->is_method=1;
        mp_dict_set(t->dict,mp_intern(name),n); return; }
    if(nlazy==clazy){ clazy=clazy?clazy+clazy/2:640; lazy=(LazyMethod*)xrealloc(lazy,sizeof(LazyMethod)*(size_t)clazy); }   /* (640: about what the built-in types have) */
    if(!nruns || runs[nruns-1].t!=t || runs[nruns-1].first+runs[nruns-1].n!=nlazy){
        if(nruns==cruns){ cruns=cruns?cruns*2:64; runs=(LazyRun*)xrealloc(runs,sizeof(LazyRun)*(size_t)cruns); }
        runs[nruns].t=t; runs[nruns].first=nlazy; runs[nruns].n=0; nruns++; }
    lazy[nlazy].name=name; lazy[nlazy].fn=fn; nlazy++; runs[nruns-1].n++;
    t->flags|=TF_LAZY;
}
TupleObj *mp_type_bases(Type *t){
    if(!t->bases){ Value b=v_obj(t->base); t->bases=AS_TUPLE(t->base ? mp_tuple(1,&b) : mp_tuple(0,NULL)); }
    return t->bases;
}
void mp_type_fill(Type *t){
    if(!t->dict) t->dict=AS_DICT(mp_dict());             /* (an exception class has none until it is needed) */
    if(!(t->flags&TF_LAZY)) return;
    t->flags&=~TF_LAZY;
    int n=0;
    for(int r=0;r<nruns;r++) if(runs[r].t==t) n+=runs[r].n;
    LazyMethod *mine=(LazyMethod*)xmalloc(sizeof(LazyMethod)*(size_t)(n+1)); n=0;
    for(int r=0;r<nruns;r++) if(runs[r].t==t){ memcpy(mine+n,lazy+runs[r].first,sizeof(LazyMethod)*(size_t)runs[r].n); n+=runs[r].n; runs[r].t=NULL; }
                                                         /* (out of the table first: what runs meanwhile may fill another type) */
    for(int i=0;i<n;i++){
        Value k=mp_intern(mine[i].name);
        if(mp_dict_get(t->dict,k,NULL)) continue;        /* (what was set in the dict after it: that stays) */
        Value f=mp_native(mine[i].name,mine[i].fn); ((NativeObj*)f.u.o)->is_method=1;
        mp_dict_set(t->dict,k,f);
    }
    free(mine);
}

