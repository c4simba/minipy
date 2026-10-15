/* ========================= Typed compiler, types: function values, constructors =========================
   Calls of function values; constructor calls (generic and abstract
   classes). */

#include "check.h"

/* cls_value(args): the constructor of each class the value may be (compared at run time) */
static Ty *ck_typecall(Ck *c, Expr *e, Ty *t){
    XInfo *xi=xinfo(e);
    if(!t->cls) ck_err(c,e->line,"calling this type value is not supported in compiled code (a class value is needed)");
    AClass *root=t->cls, *cands[64]; int n=0;
    if(!t->tup) cands[n++]=root;
    else for(int i=0;i<c->p->nclasses && n<64;i++){ AClass *k=c->p->classes[i]; if(!k->origin && (k==root || (k->value_used && aot_subclass(k,root)))) cands[n++]=k; }
    if(!n) ck_err(c,e->line,"no class of %s is used as a value here",root->name);
    if(!xi->cxi || xi->ncands<n){ xi->cxi=(XInfo**)xrealloc(xi->cxi,sizeof(XInfo*)*(size_t)n); for(int i=xi->ncands;i<n;i++) xi->cxi[i]=MPY_NEW0(XInfo); }
    xi->cands=(AClass**)xrealloc(xi->cands,sizeof(AClass*)*(size_t)n); memcpy(xi->cands,cands,sizeof(AClass*)*(size_t)n); xi->ncands=n;
    void *keep=e->ty;
    for(int i=0;i<n;i++){ e->ty=xi->cxi[i]; xi->cxi[i]->ty=NULL; ck_ctor(c,e,cands[i]); }    /* (each in an XInfo of its own) */
    e->ty=keep; xi->kind=X_TYPECALL;
    return ty_new(TY_OBJ,NULL,root);
}
Ty *ck_callval(Ck *c, Expr *e, Ty *ft){
    XInfo *xi=xinfo(e);
    ft=ty_find(ft);
    if(ft->k==TY_TYPE) return ck_typecall(c,e,ft);
    if(ft->k==TY_VAR){                                  /* fn(x) of an unknown fn: it takes these arguments */
        for(int i=0;i<e->count;i++) if(e->items[i]->akind){ for(int k=0;k<e->count;k++) ck_expr(c,e->items[k]); return pending(c,e->line,"the called function"); }
        Ty *ps[16]; if(e->count>16) ck_err(c,e->line,"too many arguments");
        for(int i=0;i<e->count;i++) ps[i]=ty_var();
        Ty *nf=ty_func(ps,e->count,ty_var()); unify(c,ft,nf); ft=nf;
    }
    if(ft->k==TY_OBJ && ft->cls && aot_find_method(ft->cls,"__call__")){   /* obj(args): obj.__call__(args) */
        Expr *at=xnew(EXPR_ATTRIBUTE,e->line); at->a=e->a; at->name="__call__"; e->a=at; xi->kind=X_NONE;
        return ck_call_inner(c,e); }
    if(ft->k!=TY_FUNC) ck_err(c,e->line,"%s is not callable",ty_name(ft));
    int star=ft->tup&1, dstar=(ft->tup>>1)&1, nreg=ft->nelems-star-dstar;
    Ty *starel=star?ft->elems[nreg]:NULL, *kwel=dstar?ft->elems[nreg+star]:NULL;
    for(int i=0;i<16;i++){ xi->argmap[i]=-1; xi->argelem[i]=0; }
    xi->nxargs=0; xi->nkwargs=0; xi->splat=0; xi->dsplat=0; xi->emptysplat=0;
    int pi=0;
    #define TOO_MANY() ck_err(c,e->line,"this function takes %d positional argument%s",nreg,nreg==1?"":"s")
    for(int i=0;i<e->count;i++){ Expr *a=e->items[i];
        if(a->akind==0){
            if(pi<nreg-ft->kwo){ int ep=is_endpoint(ft->elems[pi]); c->ep_value+=ep;
                Ty *t=ck_expr_want(c,a,ft->elems[pi]); c->ep_value-=ep;
                no_void(c,t,e->line); expect(c,ft->elems[pi],t,e->line,"an argument"); ep_arg(c,ft->elems[pi],a,e->line); xi->argmap[pi++]=i; }
            else if(star){ Ty *t=ck_expr_want(c,a,starel); no_void(c,t,e->line); expect(c,starel,t,e->line,"an argument"); int_push(&xi->xargs,&xi->nxargs,i); }
            else TOO_MANY();
        } else if(a->akind==3){
            int p=-1; if(ft->names) for(int k=0;k<nreg;k++) if(ft->names[k] && !strcmp(ft->names[k],a->kw)) p=k;
            if(p>=0){                                       /* a parameter by its name */
                if(xi->argmap[p]>=0) ck_err(c,e->line,"this function got multiple values for argument '%s'",a->kw);
                Ty *t=ck_expr_want(c,a,ft->elems[p]); no_void(c,t,e->line); expect(c,ft->elems[p],t,e->line,"an argument"); xi->argmap[p]=i; continue; }
            if(!dstar) ck_err(c,e->line,ft->names?"this function got an unexpected keyword argument '%s'":"keyword argument '%s': a function value takes keywords only through **kwargs (or its own parameter names) in compiled code",a->kw);
            Ty *t=ck_expr_want(c,a,kwel); no_void(c,t,e->line); expect(c,kwel,t,e->line,"a keyword argument"); int_push(&xi->kwargs,&xi->nkwargs,i);
        } else if(a->akind==1){                         /* *seq */
            Ty *t=ty_find(ck_expr(c,a)); no_void(c,t,e->line);
            if(t->k==TY_VAR){ pending(c,e->line,"the unpacked argument"); continue; }
            if(t->k!=TY_LIST && t->k!=TY_TUPLE) ck_err(c,e->line,"*argument must be a list or a tuple, not %s",ty_name(t));
            if(pi<nreg && t->k==TY_TUPLE){
                for(int k=0;k<t->nelems;k++){ if(pi>=nreg) ck_err(c,e->line,"f(*tuple) over both parameters and *args is not supported in compiled code");
                    expect(c,ft->elems[pi],t->elems[k],e->line,"an argument"); xi->argmap[pi]=i; xi->argelem[pi]=k+1; pi++; }
            } else if(pi<nreg){
                if(star) ck_err(c,e->line,"f(*list) over both parameters and *args is not supported in compiled code");
                for(int k=0;pi<nreg;k++){ expect(c,ft->elems[pi],t->elem,e->line,"an argument"); xi->argmap[pi]=i; xi->argelem[pi]=k+1; pi++; }
            } else if(star){
                if(xi->splat) ck_err(c,e->line,"only one *argument per call is supported in compiled code");
                if(t->k==TY_LIST) expect(c,starel,t->elem,e->line,"an argument"); else for(int k=0;k<t->nelems;k++) expect(c,starel,t->elems[k],e->line,"an argument");
                xi->splat=i+1;
            } else if(t->k==TY_LIST && !xi->emptysplat) xi->emptysplat=i+1;    /* nothing left for it: must be empty */
            else if(t->k!=TY_TUPLE || t->nelems) TOO_MANY();
        } else {                                        /* **d */
            Ty *t=ty_find(ck_expr(c,a));
            if(xi->dsplat) ck_err(c,e->line,"only one **argument per call is supported in compiled code");
            if(dstar) expect(c,ty_dict(TY_STR_T,kwel),t,e->line,"the **argument");
            else if(t->k!=TY_DICT && t->k!=TY_VAR) ck_err(c,e->line,"**argument must be a dict, not %s",ty_name(t));   /* must be empty when called */
            xi->dsplat=i+1;
        }
    }
    #undef TOO_MANY
    for(int p=0;p<nreg-ft->ndef;p++) if(xi->argmap[p]<0){                /* (the rest: its defaults) */
        if(ft->names && ft->names[p]) ck_err(c,e->line,"this function is missing argument '%s'",ft->names[p]);
        ck_err(c,e->line,"this function takes %d argument%s",nreg,nreg==1?"":"s"); }
    xi->kind=X_CALLVAL; xi->fn=NULL;
    return ft->elem;
}
int class_has_subclass(Ck *c, AClass *cls){ for(int i=0;i<c->p->nclasses;i++) if(c->p->classes[i]->base==cls) return 1; return 0; }
/* A generic class's constructor call: the class (or an instance of it) whose __init__ takes these argument types */
static int ctor_args_fit(Ck *c, Expr *e, AClass *cand);
static AClass *pick_class_instance(Ck *c, Expr *e, AClass *cls, XInfo *xi){
    if(xi->icls && (xi->icls==cls || xi->icls->origin==cls)){
        if(ctor_args_fit(c,e,xi->icls)) return xi->icls;
        xi->icls=NULL; }                                                    /* (chosen before its types were bound: again) */
    AFunc *init=aot_find_method(cls,"__init__");
    if(!init || init->cls!=cls || class_has_subclass(c,cls)) return cls;
    Ty *at[16]={0}; Stmt *def=init->def; int unknown=0, lam_generic=0;
    for(int i=0, pos=1;i<e->count;i++){ Expr *a=e->items[i]; int p=-1;
        if(a->akind==1||a->akind==2) return cls;
        if(a->akind==3){ for(int k=1;k<init->nparams;k++) if(!strcmp(def->params[k],a->kw)) p=k; }
        else p=pos++;
        if(p<1 || p>=init->nparams || p==init->star || p==init->dstar || p>=16) return cls;
        if(a->kind==EXPR_NAME && def->annotations && p<def->annotation_cap){ int ar=ann_callable_arity(def->annotations[p]); if(ar>=0) builtin_fn_value(c,a,ar); }   /* defaultdict(list) */
        at[p]=ty_find(ck_expr(c,a));
        if(!ty_known(at[p]) || at[p]->k==TY_VOID){ at[p]=NULL; if(a->kind!=EXPR_LAMBDA) unknown=1; }   /* (a lambda's types come from the instance) */
        if(a->kind==EXPR_LAMBDA) lam_generic|=1<<p; }
    if(unknown && !c->strict) return NULL;                                     /* (decided once the arguments are known) */
    if(lam_generic){ int any=0; for(int p=1;p<16;p++) if((lam_generic>>p&1) && lambda_decides(cls->mod,def,p,at,1)) any=1; lam_generic=any; }
    if(lam_generic){ AClass *k=class_instance(c,cls); k->bare=1; xi->icls=k; return k; }   /* what T is comes from a lambda: a copy for this call */
    { int any=0, decides=0; for(int p=0;p<16;p++) if(at[p]){ any=1; Expr *an=def->annotations && p<def->annotation_cap ? def->annotations[p] : NULL;
          if(!an || mentions_typevar(cls->mod,an) || (cls->def && mentions_name_of(an,cls->def->tparams,cls->def->ntparams))) decides=1; }   /* (an untyped one may: through the body) */
      if((!any && !e->count) || (any && !decides && !unknown && cls->def && cls->def->ntparams)){ AClass *k=class_instance(c,cls); k->bare=1; xi->icls=k; return k; }   /* Box(), Queue(maxsize=2): an instance of
                                                                               its own (what it holds: from its uses) */
      if(!any) return c->strict ? cls : NULL; }
    for(int k=-1;k<cls->ninsts;k++){ AClass *cand=k<0?cls:cls->insts[k]; AFunc *ci=aot_find_method(cand,"__init__"); int ok=1;
        if(cand->bare) continue;
        for(int p=1;p<ci->nparams && p<16 && ok;p++) if(at[p]){ Ty *q=ty_find(ci->params[p]->ty); if(q->k!=TY_VAR && !ty_same(q,at[p])) ok=0; }
        if(ok){ xi->icls=cand; return cand; } }
    AClass *k=class_instance(c,cls); xi->icls=k;
    return k;
}
/* whether the positional / keyword arguments of e (known types) fit cand's __init__ */
static int ctor_args_fit(Ck *c, Expr *e, AClass *cand){
    AFunc *ci=aot_find_method(cand,"__init__"); if(!ci || !ci->def) return 1;
    for(int i=0, pos=1;i<e->count;i++){ Expr *a=e->items[i]; int p=-1;
        if(a->akind==1||a->akind==2) return 1;
        if(a->akind==3){ for(int k=1;k<ci->nparams;k++) if(!strcmp(ci->def->params[k],a->kw)) p=k; }
        else p=pos++;
        if(p<1 || p>=ci->nparams || p==ci->star || p==ci->dstar) continue;
        Ty *at=ty_find(xinfo(a)->ty ? xinfo(a)->ty : TY_VOID_T); if(!xinfo(a)->ty || !ty_known(at) || at->k==TY_VOID) continue;
        Ty *q=ty_find(ci->params[p]->ty);
        if(q->k==TY_VAR) continue;
        if(!ty_same(q,at) && !(q->opt && at->k==q->k)) return 0; }
    return 1;
}
/* a copy of generic class cls (its body made again from the source) */
AClass *class_instance(Ck *c, AClass *cls){
    Stmt *copy=stmt_clone(c,cls->mod,cls->def);
    AModule *save=c->mod; c->mod=cls->mod;
    AClass *k=new_class(c,cls->mod,copy);
    k->name=cls->name; k->qualname=cls->qualname; k->outer=cls->outer; k->encl=cls->encl; k->origin=cls;
    char h[700]; snprintf(h,sizeof h,"%s#%d",cls->symname,cls->ninsts+2);
    symtab_add(&cls->mod->syms,h,AS_CLASS,k); k->symname=symtab_find(&cls->mod->syms,h)->name;
    for(int i=0;i<copy->body_count;i++) if(copy->body[i]->kind==STMT_CLASS_DEF) new_inner_class(c,cls->mod,copy->body[i],k,NULL);
    cls->insts=(AClass**)xrealloc(cls->insts,sizeof(AClass*)*(size_t)(cls->ninsts+1)); cls->insts[cls->ninsts++]=k;
    fill_class(c,k);
    c->mod=save; c->changed=1;
    return k;
}
/* the TypeError instantiating cls raises when abstract methods have no implementation, or NULL */
static const char *abstract_message(AClass *cls){
    const char *names[64]; int n=0;
    for(AClass *k=cls;k;k=k->base) for(int i=0;i<k->nmethods;i++){ AFunc *m=k->methods[i];
        if(!m->is_abstract) continue;
        AFunc *r=aot_find_method(cls,m->name); if(!r || !r->is_abstract) continue;
        int dup=0; for(int j=0;j<n;j++) if(!strcmp(names[j],m->name)) dup=1;
        if(!dup && n<64) names[n++]=m->name; }
    if(!n) return NULL;
    for(int a=0;a<n;a++) for(int b=a+1;b<n;b++) if(strcmp(names[a],names[b])>0){ const char *t=names[a]; names[a]=names[b]; names[b]=t; }
    char buf[2048]; int l=snprintf(buf,sizeof buf,"Can't instantiate abstract class %s without an implementation for abstract method%s ",cls->name,n>1?"s":"");
    for(int i=0;i<n && l<(int)sizeof buf-80;i++) l+=snprintf(buf+l,sizeof buf-(size_t)l,"%s'%s'",i?", ":"",names[i]);
    return xstrdup2(buf);
}
Ty *ck_ctor(Ck *c, Expr *e, AClass *cls){
    if(cls->check_failed) ck_err(c,e->line,"class %s has errors (reported above): it cannot be used here",cls->name);   /* (check mode) */
    XInfo *xi=xinfo(e);
    if(cls->builtin && !strcmp(cls->name,"SystemExit") && lookup(c,"__mpy_sysexit_int") && strcmp(c->mod->name,"_mpy_exit") && e->kind==EXPR_CALL && e->count<=1){
        const char *h=NULL;                                                   /* SystemExit(code): its code kept (_mpy_exit.py) */
        if(!e->count || e->items[0]->kind==EXPR_NONE){ h="__mpy_sysexit_none"; e->count=0; }
        else if(!e->items[0]->akind){ Ty *t=ty_find(ck_expr(c,e->items[0]));
            if(t->k==TY_VAR) return pending(c,e->line,"the exit code");
            if((t->k==TY_INT||t->k==TY_BOOL) && !t->opt) h="__mpy_sysexit_int";
            else if(t->k==TY_STR && !t->opt) h="__mpy_sysexit_str"; }
        if(h){ e->a=name_expr(h,e->line); return ck_call_inner(c,e); } }
    if(cls==complex_class && e->count==1 && !e->items[0]->akind && ty_find(ck_expr(c,e->items[0]))->k==TY_STR){   /* complex("1+2j") */
        e->a=name_expr("__mpy_complex_parse",e->line); return ck_call_inner(c,e); }
    if(cls->origin) cls=cls->origin;
    if(cls->generic){ cls=pick_class_instance(c,e,cls,xi); if(!cls) return pending(c,e->line,"the arguments"); }
    AFunc *init=aot_find_method(cls,"__init__");
    if(init && init->pristine){                                             /* __init__(self, x: T): a copy per kind of arguments, one class
                                                                               (constructors call it directly: subclasses' own __init__ do not matter) */
        AFunc *g=pick_instance(c,e,init,xi);
        if(!g){ for(int i=0;i<e->count;i++) ck_expr(c,e->items[i]); return pending(c,e->line,"the arguments"); }
        init=g; }
    xi->kind=X_CTOR; xi->cls=cls; xi->fn=init;
    if(!init && aot_is_exception(cls) && e->count==5 && aot_find_field(cls,"reason")){   /* UnicodeDecodeError(encoding, object, start, end, reason): _oserror.py */
        for(int i=0;i<5;i++) if(e->items[i]->akind) ck_err(c,e->line,"%s() takes positional arguments in compiled code",cls->name);
        AClass *k=cls; int dec=0; for(;k;k=k->base) if(k->builtin && !strcmp(k->name,"UnicodeDecodeError")) dec=1;
        Expr *args[5]; for(int i=0;i<5;i++) args[i]=e->items[i];
        Expr *inner=xnew(EXPR_CALL,e->line); inner->a=e->a; e->count=0;
        e->a=name_expr(dec?"__mpy_decode_error":"__mpy_encode_error",e->line); xpush(e,inner);
        for(int i=0;i<5;i++) xpush(e,args[i]);
        if(!lookup(c,"__mpy_decode_error")) ck_err(c,e->line,"internal error: a UnicodeError without _oserror.py");
        return ck_call_inner(c,e); }
    if(!init && aot_is_exception(cls) && e->count>=2 && e->count<=5 && aot_find_field(cls,"_argrepr") && !aot_find_field(cls,"reason")){   /* OSError(errno, strerror[, filename[, winerror, filename2]]): _oserror.py */
        for(int i=0;i<e->count;i++) if(e->items[i]->akind) ck_err(c,e->line,"%s() takes positional arguments in compiled code",cls->name);
        int exact= cls->builtin && (!strcmp(cls->name,"OSError")||!strcmp(cls->name,"IOError")||!strcmp(cls->name,"EnvironmentError"));
        Expr *a0=e->items[0], *a1=e->items[1], *a2= e->count>=3 ? e->items[2] : xnew(EXPR_NONE,e->line), *a4= e->count==5 ? e->items[4] : xnew(EXPR_NONE,e->line);
        e->count=0;
        if(exact) e->a=name_expr("__mpy_oserror_new",e->line);
        else { Expr *inner=xnew(EXPR_CALL,e->line); inner->a=e->a; e->a=name_expr("__mpy_oserror",e->line); xpush(e,inner); }
        xpush(e,a0); xpush(e,a1); xpush(e,a2); xpush(e,a4);
        if(!lookup(c,"__mpy_oserror")) ck_err(c,e->line,"internal error: OSError(errno, strerror) without _oserror.py");
        return ck_call_inner(c,e); }
    if(!init && aot_is_exception(cls)){                 /* ValueError("message") */
        if(e->count>1) ck_err(c,e->line,"%s() takes at most one argument (the message) in compiled code",cls->name);
        for(int i=0;i<e->count;i++){ if(e->items[i]->akind) ck_err(c,e->line,"%s() takes the message as a positional argument",cls->name); no_void(c,ck_expr(c,e->items[i]),e->line); }
        xi->name="exc";
        return ty_new(TY_OBJ,NULL,cls);
    }
    if(init) ck_args(c,e,init,1,xi);
    else if(e->count) ck_err(c,e->line,"%s() takes no arguments (it has no __init__)",cls->name);
    xi->abstract_msg=abstract_message(cls);
    for(AClass *k=cls;k;k=k->base) if(!k->constructed){ k->constructed=1; c->changed=1; }
    return ty_new(TY_OBJ,NULL,cls);
}
Ty *ck_positional(Ck *c, Expr *e, int min, int max, const char *name){
    if(e->count<min||e->count>max){
        if(min==max) ck_err(c,e->line,"%s() takes %d argument%s",name,min,min==1?"":"s");
        ck_err(c,e->line,"%s() takes %d to %d arguments",name,min,max);
    }
    for(int i=0;i<e->count;i++) if(e->items[i]->akind) ck_err(c,e->line,"%s() takes positional arguments only",name);
    return NULL;
}
Ty *arg(Ck *c, Expr *e, int i){ Ty *t=ck_expr(c,e->items[i]); no_void(c,t,e->line); return t; }

