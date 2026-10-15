/* ========================= Typed compiler, types: annotations =========================
   The built-in exception classes; annotations (int, list[T], Optional[T],
   Callable[...], TypeVars, classes) turned into types. */

#include "check.h"

ASym *module_sym(AModule *m, const char *name){ return symtab_find(&m->syms,name); }

/* ---- built-in exception classes: BaseException (field _msg: str) and its subclasses ---- */
SymTab builtin_syms;
AClass *exc_root;
int aot_is_exception(AClass *c){ return exc_root && aot_subclass(c,exc_root); }
ASym *global_sym(AModule *m, const char *name){ ASym *s=module_sym(m,name); return s?s:symtab_find(&builtin_syms,name); }
void make_builtin_exceptions(AProg *p, AModule *m){
    static const char *tree[][2]={
        {"BaseException",NULL},{"Exception","BaseException"},{"ArithmeticError","Exception"},{"ZeroDivisionError","ArithmeticError"},
        {"LookupError","Exception"},{"IndexError","LookupError"},{"KeyError","LookupError"},{"ValueError","Exception"},
        {"TypeError","Exception"},{"AttributeError","Exception"},{"RuntimeError","Exception"},{"NotImplementedError","RuntimeError"},
        {"AssertionError","Exception"},{"OSError","Exception"},{"MemoryError","Exception"},{"NameError","Exception"},{"ImportError","Exception"},
        {"OverflowError","ArithmeticError"},{"StopIteration","Exception"},{"InvalidStateError","Exception"},
        {"FileNotFoundError","OSError"},{"EOFError","Exception"},{"UnicodeError","ValueError"},{"UnicodeDecodeError","UnicodeError"},{"UnicodeEncodeError","UnicodeError"},{"KeyboardInterrupt","BaseException"},
        {"SystemExit","BaseException"},{"PermissionError","OSError"},{"FileExistsError","OSError"},{"IsADirectoryError","OSError"},
        {"NotADirectoryError","OSError"},{"TimeoutError","OSError"},{"ConnectionError","OSError"},{"UnboundLocalError","NameError"},
        {"RecursionError","RuntimeError"},{"StopAsyncIteration","Exception"},{"BufferError","Exception"},{"EnvironmentError","OSError"},
        {"IOError","OSError"},{"ModuleNotFoundError","ImportError"},{"FrozenInstanceError","AttributeError"},{"GeneratorExit","BaseException"},
        {"BlockingIOError","OSError"},{"ChildProcessError","OSError"},{"BrokenPipeError","ConnectionError"},{"ConnectionAbortedError","ConnectionError"},
        {"ConnectionRefusedError","ConnectionError"},{"ConnectionResetError","ConnectionError"},{"InterruptedError","OSError"},{"ProcessLookupError","OSError"},
        {"Warning","Exception"},{"UserWarning","Warning"},{"DeprecationWarning","Warning"},{"PendingDeprecationWarning","Warning"},
        {"SyntaxWarning","Warning"},{"RuntimeWarning","Warning"},{"FutureWarning","Warning"},{"ImportWarning","Warning"},
        {"UnicodeWarning","Warning"},{"BytesWarning","Warning"},{"ResourceWarning","Warning"},{"EncodingWarning","Warning"},
        {"FloatingPointError","ArithmeticError"},{"ReferenceError","Exception"},{"SystemError","Exception"},{"SyntaxError","Exception"},
        {"IndentationError","SyntaxError"},{"TabError","IndentationError"},{"PythonFinalizationError","RuntimeError"},{NULL,NULL}};
    static Expr none_init; none_init.kind=EXPR_NONE;
    memset(&builtin_syms,0,sizeof builtin_syms); exc_root=NULL;
    for(int i=0;tree[i][0];i++){
        AClass *cls=MPY_NEW0(AClass); cls->name=xstrdup2(tree[i][0]); cls->mod=m; cls->builtin=1; cls->id=p->nclasses;
        cls->qualname=cls->name; cls->symname=cls->name;
        if(tree[i][1]) cls->base=(AClass*)symtab_find(&builtin_syms,tree[i][1])->p;
        else { exc_root=cls; add_field(cls,"_msg",TY_STR_T,NULL);
            Ty *ex=optional(ty_new(TY_OBJ,NULL,cls));
            add_field(cls,"__cause__",ex,NULL); add_field(cls,"__context__",ex,NULL); add_field(cls,"__suppress_context__",ty_copy(TY_BOOL_T),NULL);
            add_field(cls,"_rawrepr",ty_copy(TY_BOOL_T),NULL); }       /* the message is the repr already (KeyError('k'), ValueError(1)) */
        if(p->nclasses==p->ccap){ p->ccap=p->ccap?p->ccap*2:16; p->classes=(AClass**)xrealloc(p->classes,sizeof(AClass*)*(size_t)p->ccap); }
        p->classes[p->nclasses++]=cls;
        symtab_add(&builtin_syms,cls->name,AS_CLASS,cls);
        if(!strcmp(cls->name,"StopIteration")){ Ty *v=ty_var(); v->opt=1; add_field(cls,"value",v,NULL); }   /* a generator's return value (None) */
        if(!strcmp(cls->name,"SystemExit")){ add_field(cls,"code",optional(TY_INT_T),&none_init); add_field(cls,"_msgexit",ty_copy(TY_BOOL_T),NULL); }   /* (_mpy_exit.py) */
        if(!strcmp(cls->name,"SystemExit")){ add_field(cls,"code",optional(TY_INT_T),&none_init); add_field(cls,"_msgexit",ty_copy(TY_BOOL_T),NULL); }   /* (_mpy_exit.py) */
        if(!strcmp(cls->name,"OSError")){                     /* OSError(errno, strerror, filename): _oserror.py */
            add_field(cls,"errno",optional(TY_INT_T),&none_init); add_field(cls,"strerror",optional(TY_STR_T),&none_init);
            add_field(cls,"filename",optional(TY_STR_T),&none_init); add_field(cls,"filename2",optional(TY_STR_T),&none_init);
            add_field(cls,"_argrepr",optional(TY_STR_T),&none_init); }
        if(!strcmp(cls->name,"UnicodeError")) add_field(cls,"_argrepr",optional(TY_STR_T),&none_init);
        if(!strcmp(cls->name,"UnicodeDecodeError")||!strcmp(cls->name,"UnicodeEncodeError")){   /* (encoding, object, start, end, reason): _oserror.py */
            add_field(cls,"encoding",optional(TY_STR_T),&none_init); add_field(cls,"object",optional(cls->name[7]=='D'?TY_BYTES_T:TY_STR_T),&none_init);
            add_field(cls,"start",ty_copy(TY_INT_T),NULL); add_field(cls,"end",ty_copy(TY_INT_T),NULL); add_field(cls,"reason",optional(TY_STR_T),&none_init); }
    }
}
/* abc.ABC: a plain base class (made once a module imports it) */
AClass *abc_class(Ck *c){
    ASym *s=symtab_find(&builtin_syms,"ABC"); if(s) return (AClass*)s->p;
    AProg *p=c->p; AClass *cls=MPY_NEW0(AClass); cls->name=xstrdup2("ABC"); cls->qualname=cls->name; cls->symname=cls->name; cls->mod=p->mods[0]; cls->builtin=1; cls->filled=1; cls->id=p->nclasses;
    if(p->nclasses==p->ccap){ p->ccap=p->ccap?p->ccap*2:16; p->classes=(AClass**)xrealloc(p->classes,sizeof(AClass*)*(size_t)p->ccap); }
    p->classes[p->nclasses++]=cls;
    symtab_add(&builtin_syms,cls->name,AS_CLASS,cls);
    return cls;
}
AModule *find_module(AProg *p, const char *name){
    for(int i=0;i<p->nmods;i++) if(!strcmp(p->mods[i]->name,name)) return p->mods[i];
    const char *al=mpy_stdlib_alias(name);                       /* os.path: posixpath */
    return al ? find_module(p,al) : NULL;
}

/* Optional[t]: a copy that may be None */
Ty *optional(Ty *t){ Ty *f=ty_find(t); if(f->k==TY_VAR){ f->opt=1; return f; } Ty *n=ty_copy(f); n->opt=1; return n; }
/* minipy.Endpoint: Callable[[dict[str, str]], str] that any function converts
   to (an adapter made by the compiler, see ep_adapter) */
Ty *endpoint_type(void){
    Ty *ps[1]; ps[0]=ty_dict(TY_STR_T,TY_STR_T);
    Ty *t=ty_func(ps,1,TY_STR_T); t->tup=4; return t;
}
int is_endpoint(Ty *t){ t=ty_find(t); return t->k==TY_FUNC && (t->tup&4); }
/* a type annotation (its expression): NAME | NAME '[' type, ... ']' | module '.' NAME | None |
   "Class" (a forward reference) | T | None (Optional[T]) */
/* a type parameter (def f[T], class C[T], a TypeVar) named in an annotation -> its type variable */
int names_has(char **v, int n, const char *name){ for(int i=0;i<n;i++) if(!strcmp(v[i],name)) return 1; return 0; }
static Ty *tparam_ty(Ck *c, AModule *m, const char *name){
    for(int i=c->nalias-1;i>=0;i--) if(!strcmp(c->alias_n[i],name)) return c->alias_t[i];
    AFunc *f=c->sig_fn ? c->sig_fn : c->fn;
    AClass *cls=c->sig_cls ? c->sig_cls : f ? f->cls : NULL;
    if(cls && cls->def && names_has(cls->def->tparams,cls->def->ntparams,name)){          /* the class's: shared by its methods */
        if(!cls->tpv){ cls->tpv=MPY_NEW_ARR(Ty*,cls->def->ntparams); for(int i=0;i<cls->def->ntparams;i++) cls->tpv[i]=ty_var(); }
        for(int i=0;i<cls->def->ntparams;i++) if(!strcmp(cls->def->tparams[i],name)) return cls->tpv[i]; }
    ASym *s=module_sym(m,name);
    int tv= s && s->kind==AS_TYPEVAR;
    if(f && f->def && (tv || names_has(f->def->tparams,f->def->ntparams,name))){          /* the function's own */
        for(int i=0;i<f->ntp;i++) if(!strcmp(f->tpn[i],name)) return f->tpv[i];
        f->tpn=(const char**)xrealloc(f->tpn,sizeof(char*)*(size_t)(f->ntp+1)); f->tpv=(Ty**)xrealloc(f->tpv,sizeof(Ty*)*(size_t)(f->ntp+1));
        f->tpn[f->ntp]=name; f->tpv[f->ntp]=ty_var(); return f->tpv[f->ntp++]; }
    return tv ? ty_var() : NULL;
}
Ty *type_of(Ck *c, Expr *e, int line){
    if(e->kind==EXPR_BINARY && e->op==T_PIPE){
        Ty *t=type_of(c,e->a,line), *u=type_of(c,e->b,line);
        if(ty_find(u)->k==TY_VOID) return optional(t);
        if(ty_find(t)->k==TY_VOID) return optional(u);
        ck_err(c,line,"only T | None is supported in compiled code: one type per value");
    }
    if(e->kind==EXPR_NONE) return TY_VOID_T;
    if(e->kind==EXPR_LITERAL && e->tok->kind==T_STRING){
        Expr *x=py_front_expr(c->mod->unit->path,e->tok->text,line);
        if(!x) ck_err(c,line,"unexpected token in type annotation");
        return type_of(c,x,line);
    }
    Expr *base=e, *targ=NULL;
    if(e->kind==EXPR_INDEX){ base=e->a; targ=e->b; }
    const char *name; AModule *m=c->mod;
    if(base->kind==EXPR_ATTRIBUTE && !targ && !strcmp(base->name,"_PercentArgs") && base->a->kind==EXPR_NAME && !strcmp(base->a->name,"sys"))
        return ty_copy(TY_STR_T);                                      /* *args: sys._PercentArgs (formatted into the message at the call) */
    if(base->kind==EXPR_ATTRIBUTE){                                      /* module.Class, typing.List */
        Expr *chain[16]; int k=0; Expr *x=base;
        while(x->kind==EXPR_ATTRIBUTE && k<16){ chain[k++]=x; x=x->a; }
        if(x->kind!=EXPR_NAME) ck_err(c,line,"expected a type");
        name=x->name;
        for(int j=k-1;j>=0;j--){ const char *next=chain[j]->name;
            ASym *s=j==k-1 ? local_class(c,name) : NULL; if(!s) s=module_sym(m,name);
            if(!s && j<k-1){ char full[512]; snprintf(full,sizeof full,"%s.%s",m->name,name); AModule *sub=find_module(c->p,full);   /* package.module */
                if(sub){ m=sub; name=next; continue; } }
            if(s && s->kind==AS_CLASS){ AClass *in=aot_nested_class(c->p,(AClass*)s->p,next);       /* Outer.Inner */
                if(!in) ck_err(c,line,"class %s has no class '%s'",((AClass*)s->p)->name,next);
                if(j) { m=in->mod; name=in->symname; continue; }
                m=in->mod; name=in->symname; break; }
            if(s && s->kind==AS_SYS && (!strcmp((const char*)s->p,"typing")||!strcmp((const char*)s->p,"dataclasses"))){ name=next; continue; }
            if(s && s->kind==AS_SYS && !strcmp((const char*)s->p,"minipy") && !strcmp(next,"Endpoint")){
                if(j || targ) ck_err(c,line,"unexpected token in type annotation");
                return endpoint_type(); }
            if(!s || s->kind!=AS_MODULE) ck_err(c,line,"'%s' is not a module",name);
            m=(AModule*)s->p; name=next;
        }
    } else if(base->kind==EXPR_NAME && strcmp(base->name,"...")) name=base->name;
    else ck_err(c,line,"expected a type");
    if(!strcmp(name,"ClassVar")||!strcmp(name,"Final")||!strcmp(name,"InitVar")||!strcmp(name,"Annotated")){   /* ClassVar[T], Final[T], InitVar[T]: a T */
        if(!targ) return ty_var();
        if(!strcmp(name,"Annotated")) return type_of(c,targ->kind==EXPR_TUPLE&&targ->count?targ->items[0]:targ,line);
        return type_of(c,targ,line); }
    if(!strcmp(name,"Callable") && targ && targ->kind==EXPR_TUPLE && targ->count==2 && targ->items[0]->kind==EXPR_NAME && !strcmp(targ->items[0]->name,"...")){
        Ty *v=ty_var(); return v; }                                    /* Callable[..., R]: the function given (its parameters as it has them) */
    if(!strcmp(name,"Callable")){                                       /* Callable[[A, B], R] */
        if(!targ || (targ->kind!=EXPR_LIST && !(targ->kind==EXPR_TUPLE && targ->count>=1 && targ->items[0]->kind==EXPR_LIST)))
            ck_err(c,line,"Callable needs its parameter and result types: Callable[[int, str], bool]");
        if(targ->kind==EXPR_LIST || targ->count<2) ck_err(c,line,"Callable needs a result type: Callable[[int], int]");
        if(targ->count>2) ck_err(c,line,"expected ']' in Callable");
        Expr *pl=targ->items[0]; Ty *ps[16]; int np=0;
        for(int k=0;k<pl->count;k++){ if(np==16) ck_err(c,line,"too many parameters"); ps[np++]=type_of(c,pl->items[k],line); }
        return ty_func(ps,np,type_of(c,targ->items[1],line));
    }
    Ty *args[16]={NULL}; int nargs=0, varlen=0;
    if(targ){
        Expr **v=targ->kind==EXPR_TUPLE ? targ->items : &targ; int n=targ->kind==EXPR_TUPLE ? targ->count : 1;
        for(int k=0;k<n;k++){
            if(v[k]->kind==EXPR_NAME && !strcmp(v[k]->name,"...")){ if(k!=n-1) ck_err(c,line,"expected ']' in type"); varlen=1; break; }   /* tuple[int, ...] */
            if(nargs==16) ck_err(c,line,"too many type arguments");
            args[nargs++]=type_of(c,v[k],line);
        }
    }
    static const char *aliases[][2]={{"List","list"},{"Dict","dict"},{"Set","set"},{"Tuple","tuple"},{"FrozenSet","frozenset"},{"Sequence","list"},{"MutableSequence","list"},{"Mapping","dict"},{"MutableMapping","dict"},{"AbstractSet","set"},{NULL,NULL}};
    for(int k=0;aliases[k][0];k++) if(!strcmp(name,aliases[k][0])){ name=aliases[k][1]; break; }
    if(!strcmp(name,"Optional")){ if(nargs!=1) ck_err(c,line,"Optional takes one type argument"); return optional(args[0]); }   /* None is the zero value anyway */
    if(!strcmp(name,"Union")){                                       /* Union[T, None] */
        if(nargs==2 && ty_find(args[1])->k==TY_VOID) return optional(args[0]);
        if(nargs==2 && ty_find(args[0])->k==TY_VOID) return optional(args[1]);
        if(nargs==1) return args[0];
        ck_err(c,line,"only Union[T, None] (Optional[T]) is supported in compiled code: one type per value");
    }
    if(!strcmp(name,"AsyncIterator")||!strcmp(name,"AsyncIterable")||!strcmp(name,"AsyncGenerator")){   /* an async generator */
        if(nargs<1) ck_err(c,line,"%s needs the item type: %s[int]",name,name);
        if(nargs>2 || (nargs>1 && strcmp(name,"AsyncGenerator"))) ck_err(c,line,"%s takes %s",name,strcmp(name,"AsyncGenerator")?"one type argument":"two type arguments: AsyncGenerator[yield, send]");
        Ty *g=ty_new(TY_GEN,args[0],NULL); g->tup=1;
        if(nargs>1 && ty_find(args[1])->k!=TY_VOID) g->elems[0]=args[1];
        return g; }
    if(!strcmp(name,"Iterator")||!strcmp(name,"Iterable")||!strcmp(name,"Generator")){
        if(nargs<1) ck_err(c,line,"%s needs the item type: %s[int]",name,name);
        if(nargs>1 && strcmp(name,"Generator")) ck_err(c,line,"%s takes one type argument",name);
        if(nargs>3) ck_err(c,line,"Generator takes three type arguments: Generator[yield, send, return]");
        Ty *g=ty_new(TY_GEN,args[0],NULL);
        if(nargs>1 && ty_find(args[1])->k!=TY_VOID) g->elems[0]=args[1];          /* Generator[Y, S, R] */
        if(nargs>2) g->elems[1]=args[2];
        return g;
    }
    if(varlen){
        if(strcmp(name,"tuple")||nargs!=1) ck_err(c,line,"`...` is only supported as tuple[T, ...]");
        Ty *t=ty_new(TY_LIST,args[0],NULL); t->tup=1; return t;
    }
    if(!strcmp(name,"int")) return ty_copy(TY_INT_T);              /* (copies: an annotated variable may become optional) */
    if(!strcmp(name,"bool")) return ty_copy(TY_BOOL_T);
    if(!strcmp(name,"float")) return ty_copy(TY_FLOAT_T);
    if(!strcmp(name,"str")) return ty_copy(TY_STR_T);
    if(!strcmp(name,"bytes")) return ty_copy(TY_BYTES_T);
    if(!strcmp(name,"type")){                                          /* type[C]: C or a subclass of it */
        if(nargs==1 && ty_find(args[0])->k==TY_OBJ){ Ty *t=ty_new(TY_TYPE,NULL,ty_find(args[0])->cls); t->tup=1; return t; }
        return ty_copy(TY_TYPE_T); }
    if(!strcmp(name,"buffer")) return ty_copy(TY_BUF_T);
    if(!strcmp(name,"list")||!strcmp(name,"set")||!strcmp(name,"frozenset")){
        if(nargs>1) ck_err(c,line,"%s takes one type argument",name);
        Ty *t=ty_new(name[0]=='l'?TY_LIST:TY_SET,nargs?args[0]:ty_var(),NULL);
        if(name[0]=='f') t->tup=1;                                     /* (a frozenset: a set that prints as one) */
        return t;
    }
    if(!strcmp(name,"tuple")){
        if(!nargs) ck_err(c,line,"tuple needs its item types: tuple[int, str]");
        for(int k=0;k<nargs;k++) if(ty_find(args[k])->k==TY_VOID) ck_err(c,line,"a tuple item cannot be None");
        return ty_tuple(args,nargs);
    }
    if(nargs>2) ck_err(c,line,"too many type arguments");
    if(!strcmp(name,"dict")){
        if(nargs==1) ck_err(c,line,"dict needs two type arguments: dict[K, V]");
        if(nargs==2){ TyKind kk=ty_find(args[0])->k; if(kk!=TY_STR&&kk!=TY_BYTES&&kk!=TY_INT&&kk!=TY_BOOL&&kk!=TY_TUPLE&&kk!=TY_OBJ&&kk!=TY_FLOAT&&kk!=TY_VAR) ck_err(c,line,"dictionary keys must be int, float, str, bytes, tuples or objects"); }
        return ty_dict(nargs?args[0]:ty_var(),nargs?args[1]:ty_var());
    }
    { Ty *tp=tparam_ty(c,m,name); if(tp){ if(nargs) ck_err(c,line,"%s takes no type arguments",name); return tp; } }
    ASym *s=m==c->mod ? local_class(c,name) : NULL;                   /* a class of this function */
    if(!s) s=global_sym(m,name);
    if(s && s->kind==AS_TYPEALIAS){                                    /* type X = ..., type P[T] = tuple[T, T] */
        Stmt *al=(Stmt*)s->p;
        if(nargs && nargs!=al->ntparams) ck_err(c,line,"%s takes %d type argument%s",name,al->ntparams,al->ntparams==1?"":"s");
        int save=c->nalias;
        for(int k=0;k<al->ntparams;k++){ c->alias_n=(const char**)xrealloc(c->alias_n,sizeof(char*)*(size_t)(c->nalias+1)); c->alias_t=(Ty**)xrealloc(c->alias_t,sizeof(Ty*)*(size_t)(c->nalias+1));
            c->alias_n[c->nalias]=al->tparams[k]; c->alias_t[c->nalias]=k<nargs?args[k]:ty_var(); c->nalias++; }
        AModule *savem=c->mod; c->mod=m;
        Ty *t=type_of(c,al->ann,line);
        c->mod=savem; c->nalias=save;
        return t; }
    if(s && s->kind==AS_CLASS){ AClass *k=(AClass*)s->p;
        if(nargs && !(k->def && k->def->ntparams)) ck_err(c,line,"class %s takes no type arguments",name);
        if(nargs && k->generic && !class_has_subclass(c,k)){                   /* Box[int]: the instance for those type arguments */
            if(nargs!=k->def->ntparams) ck_err(c,line,"%s takes %d type argument%s",name,k->def->ntparams,k->def->ntparams==1?"":"s");
            { AFunc *cf=c->sig_fn ? c->sig_fn : c->fn; AClass *cur=c->sig_cls ? c->sig_cls : cf ? cf->cls : NULL;   /* Box[T] in Box's own body: this instance */
              if(cur && (cur==k || cur->origin==k) && cur->tpv){ int own=1;
                  for(int i=0;i<nargs;i++) if(ty_find(args[i])!=ty_find(cur->tpv[i])) own=0;
                  if(own) return ty_new(TY_OBJ,NULL,cur); } }
            AClass *pick=NULL;
            for(int j=-1;j<k->ninsts && !pick;j++){ AClass *cand=j<0?k:k->insts[j];
                if(!cand->tpv){ cand->tpv=MPY_NEW_ARR(Ty*,nargs); for(int i=0;i<nargs;i++) cand->tpv[i]=ty_var(); }
                int ok=1; for(int i=0;i<nargs && ok;i++){ Ty *q=ty_find(cand->tpv[i]), *a=ty_find(args[i]); if(q->k!=TY_VAR && a->k!=TY_VAR && !ty_same(q,a)) ok=0; }
                if(ok) pick=cand; }
            if(!pick){ pick=class_instance(c,k); if(!pick->tpv){ pick->tpv=MPY_NEW_ARR(Ty*,nargs); for(int i=0;i<nargs;i++) pick->tpv[i]=ty_var(); } }
            for(int i=0;i<nargs;i++) unify(c,pick->tpv[i],args[i]);
            return ty_new(TY_OBJ,NULL,pick); }
        return ty_new(TY_OBJ,NULL,k); }
    if(s && s->kind==AS_SYS && !strcmp((const char*)s->p,"minipy.Endpoint")) return endpoint_type();
    ck_err(c,line,"unknown type '%s'",name);
}
Ty *annotation(Ck *c, Expr *e){ return e ? type_of(c,e,e->line) : NULL; }

