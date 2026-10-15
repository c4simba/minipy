/* ========================= Typed compiler, types: generic functions =========================
   Generic functions (TypeVars, protocols, untyped parameters): a copy
   of the function per kind of argument it is called with. */

#include "check.h"

Stmt *stmt_clone(Ck *c, AModule *m, Stmt *s){
    Stmt *t=py_front_copy(m->unit->path,s);
    if(!t) ck_err(c,s->line,"cannot copy %s()",s->name);
    fold_platform(c,&t->body,&t->body_count,&t->body_cap);             /* (as the original was) */
    return t;
}
int has_untyped_params(Stmt *def){
    for(int i=0;i<def->param_count;i++) if(!def->annotations || i>=def->annotation_cap || !def->annotations[i]) return 1;
    return 0;
}
/* an annotation naming a module-level TypeVar (def f(xs: list[T]) -> T): a generic function */
int mentions_name_of(Expr *e, char **names, int n){                    /* a class's own [T] in an annotation */
    if(!e || !n) return 0;
    if(e->kind==EXPR_NAME) return names_has(names,n,e->name);
    if(mentions_name_of(e->a,names,n) || mentions_name_of(e->b,names,n)) return 1;
    for(int i=0;i<e->count;i++) if(mentions_name_of(e->items[i],names,n)) return 1;
    return 0;
}
int mentions_typevar(AModule *m, Expr *e){
    if(!e) return 0;
    if(e->kind==EXPR_NAME){ ASym *s=module_sym(m,e->name); return s && s->kind==AS_TYPEVAR; }
    if(e->kind==EXPR_LITERAL && e->tok->kind==T_STRING){ Expr *x=py_front_expr(m->unit->path,e->tok->text,e->line); return x && mentions_typevar(m,x); }
    if(mentions_typevar(m,e->a) || mentions_typevar(m,e->b)) return 1;
    for(int i=0;i<e->count;i++) if(mentions_typevar(m,e->items[i])) return 1;
    return 0;
}
/* a parameter annotated with a protocol (Iterable[T], Sequence[int], Mapping[K, V] ...): any such
   object; a copy of the function per kind of argument has it as an untyped parameter */
static int protocol_annotation(Expr *a){
    static const char *const names[]={"Iterable","Iterator","Sequence","Collection","Mapping","MutableMapping","MutableSequence","AbstractSet",
        "Reversible","Container","Sized","Hashable","Generator",NULL};
    if(!a) return 0;
    if(a->kind==EXPR_LITERAL && a->tok->kind==T_STRING) return 0;
    Expr *h= a->kind==EXPR_INDEX ? a->a : a;
    const char *n= h->kind==EXPR_NAME ? h->name : h->kind==EXPR_ATTRIBUTE ? h->name : NULL;
    if(!n) return 0;
    for(int i=0;names[i];i++) if(!strcmp(n,names[i])) return 1;
    return 0;
}
int protocol_params(Stmt *def){
    for(int i=0;i<def->param_count;i++) if(def->annotations && i<def->annotation_cap && protocol_annotation(def->annotations[i])) return 1;
    return 0;
}
int typevar_params(AModule *m, Stmt *def){
    for(int i=0;i<def->param_count;i++) if(def->annotations && i<def->annotation_cap && mentions_typevar(m,def->annotations[i])) return 1;
    return 0;
}
/* a method of generic class cls (class C(Generic[T])) with a parameter annotated with another TypeVar
   (def m(self, other: U)): a copy per kind of that argument, as for a method of a plain class */
static int typevars_in(AModule *m, Expr *e, const char **out, int n);
int foreign_typevar_params(AModule *m, Stmt *def, Stmt *cls){
    for(int i=1;i<def->param_count;i++){
        if(!def->annotations || i>=def->annotation_cap || !def->annotations[i]) continue;
        const char *tv[8]; int n=typevars_in(m,def->annotations[i],tv,0);
        for(int k=0;k<n;k++) if(!names_has(cls->tparams,cls->ntparams,tv[k])) return 1; }
    return 0;
}
/* the TypeVars annotation e names (at most 8) */
static int typevars_in(AModule *m, Expr *e, const char **out, int n){
    if(!e || n>=8) return n;
    if(e->kind==EXPR_NAME){ ASym *s=module_sym(m,e->name); if(s && s->kind==AS_TYPEVAR){ for(int i=0;i<n;i++) if(!strcmp(out[i],e->name)) return n; out[n++]=e->name; } return n; }
    if(e->kind==EXPR_LITERAL && e->tok->kind==T_STRING){ Expr *x=py_front_expr(m->unit->path,e->tok->text,e->line); return x ? typevars_in(m,x,out,n) : n; }
    n=typevars_in(m,e->a,out,n); n=typevars_in(m,e->b,out,n);
    for(int i=0;i<e->count;i++) n=typevars_in(m,e->items[i],out,n);
    return n;
}
/* a lambda given for parameter p (annotated with TypeVars) when the other arguments known (at[q]
   set) do not decide all those TypeVars: what they are comes from the lambda, its own copy */
int lambda_decides(AModule *m, Stmt *def, int p, Ty **at, int first){
    if(!def->annotations || p>=def->annotation_cap) return 0;
    const char *tv[8]; int n=typevars_in(m,def->annotations[p],tv,0);
    for(int i=0;i<n;i++){ int covered=0;
        for(int q=first;q<def->param_count && q<16 && !covered;q++){ if(q==p || !at[q] || q>=def->annotation_cap) continue;
            const char *o[8]; int k=typevars_in(m,def->annotations[q],o,0);
            for(int j=0;j<k;j++) if(!strcmp(o[j],tv[i])) covered=1; }
        if(!covered) return 1; }
    return 0;
}
/* a fresh instance of generic module function fn (one per decorator use) -> its hidden name */
/* A call of a function without (or with generic) parameter types: one instance per kind of
   arguments (CPython runs one function for all of them; a compiled one is made for each) */
/* how many arguments the body calls parameter `name` with (0: no call seen) */
static int expr_call_arity(Expr *e, const char *name){
    if(!e) return 0;
    if(e->kind==EXPR_CALL && e->a && e->a->kind==EXPR_NAME && !strcmp(e->a->name,name)) return e->count;
    int r;
    if((r=expr_call_arity(e->a,name)) || (r=expr_call_arity(e->b,name)) || (r=expr_call_arity(e->c,name)) || (r=expr_call_arity(e->d,name))) return r;
    for(int i=0;i<e->count;i++) if((r=expr_call_arity(e->items[i],name))) return r;
    for(int i=0;i<e->vcount;i++) if((r=expr_call_arity(e->vals[i],name))) return r;
    for(int i=0;i<e->nclause;i++){ CompClause *cl=&e->clauses[i]; if((r=expr_call_arity(cl->iter,name))) return r;
        for(int k=0;k<cl->ncond;k++) if((r=expr_call_arity(cl->conds[k],name))) return r; }
    return 0;
}
static int stmt_call_arity(Stmt **b, int n, const char *name){
    for(int i=0;i<n;i++){ Stmt *s=b[i]; int r;
        if(s->kind==STMT_FUNCTION_DEF || s->kind==STMT_CLASS_DEF) continue;
        if((r=expr_call_arity(s->expr,name)) || (r=expr_call_arity(s->expr2,name)) || (r=expr_call_arity(s->value,name))) return r;
        for(int t=0;t<s->ntargets;t++) if((r=expr_call_arity(s->targets[t],name))) return r;
        if((r=stmt_call_arity(s->body,s->body_count,name)) || (r=stmt_call_arity(s->orelse,s->orelse_count,name))) return r; }
    return 0;
}
/* a subclass of fn's class defines a method of fn's name */
static AFunc *instance_for(Ck *c, AFunc *fn, Ty **at, Ty **fnarg);
static void none_use(Ck *c, AFunc *g, unsigned omit, unsigned fnm, unsigned gv, int line);
int method_overridden(Ck *c, AFunc *fn){
    for(int i=0;i<c->p->nclasses;i++){ AClass *k=c->p->classes[i];
        if(k==fn->cls || !aot_subclass(k,fn->cls)) continue;
        for(int j=0;j<k->nmethods;j++) if(!strcmp(k->methods[j]->name,fn->name)) return 1; }
    return 0;
}
/* a generic function's still open parameter types: copies of what this call gives (not the caller's
   own type objects, which may become optional later on: a default None of the caller's) */
static void bind_copies(Ck *c, AFunc *g, Ty **at){
    for(int p=0;p<g->nparams && p<16;p++) if(at[p]){ Ty *q=ty_find(g->params[p]->ty); if(q->k==TY_VAR) unify(c,q,ty_copy(at[p]));
        else if(p==g->dstar && q->k==TY_DICT && ty_find(q->elem)->k==TY_VAR) unify(c,q->elem,ty_copy(at[p]->elem)); }   /* (**kwargs: T) */
}
/* None, or a parameter of the function being checked that is None in it (passed on: f(x, key)) */
static int none_arg(Ck *c, Expr *a){
    if(a->kind==EXPR_NONE) return 1;
    if(a->kind!=EXPR_NAME || !c->fn || !c->fn->def) return 0;
    Stmt *def=c->fn->def;
    for(int q=0;q<def->param_count && q<16;q++) if(!strcmp(def->params[q],a->name)){
        if(c->fn->none_folded>>q&1) return 1;
        if((c->fn->origin || c->fn->pristine) && (!c->fn->cls || c->fn->origin || !method_overridden(c,c->fn)) && def->pdefaults && def->pdefaults[q] && def->pdefaults[q]->kind==EXPR_NONE && q<c->fn->nparams
           && (c->fn->none_omit>>q&1) && !(c->fn->none_given>>q&1) && ty_find(c->fn->params[q]->ty)->k==TY_VAR
           && !stores_name(c,c->fn->body,c->fn->nbody,a->name)) return 1;      /* (a copy's parameter every call leaves out: None) */
        return 0; }
    return 0;
}
/* f(fn, a, b) for def f(fn, *args: sys._PackArgs): f(fn, (a, b)) - a tuple of their own types (first: the
   parameters before the call's first argument: self) */
void pack_args(Ck *c, Expr *e, AFunc *fn, int first){
    XInfo *xi=xinfo(e);
    if(!fn->packargs || xi->packed) return;
    xi->packed=1;
    int want=fn->packargs-1-first, npos=0;
    { int np=0, splat=-1, extra=0; for(int i=0;i<e->count;i++){ Expr *a=e->items[i];      /* f(fn, *t): t is the tuple */
          if(a->akind==1){ if(np==want && splat<0) splat=i; else extra=1; }
          else if(!a->akind){ if(np>=want) extra=1; np++; } }
      if(splat>=0 && !extra){ e->items[splat]->akind=0; return; } }
    for(int i=0;i<e->count;i++){ if(e->items[i]->akind==1) ck_err(c,e->line,"%s(*args): pass the arguments one by one in compiled code",fn->name);
        if(!e->items[i]->akind) npos++; }
    if(want<0) return;
    Expr *tup=xnew(EXPR_TUPLE,e->line);
    if(npos<=want){                                                      /* none: () */
        if(npos<want) return;
        Expr **items=(Expr**)xmalloc(sizeof(Expr*)*(size_t)(e->count+1)); int n=0, pos=0, put=0;
        for(int i=0;i<e->count;i++){ Expr *a=e->items[i];
            if(a->akind && !put){ items[n++]=tup; put=1; }
            if(!a->akind) pos++;
            items[n++]=a; }
        if(!put) items[n++]=tup;
        free(e->items); e->items=items; e->count=n; e->cap=n; return; }
    Expr **items=(Expr**)xmalloc(sizeof(Expr*)*(size_t)(e->count+1)); int n=0, pos=0;
    for(int i=0;i<e->count;i++){ Expr *a=e->items[i];
        if(!a->akind){ if(pos==want) items[n++]=tup; if(pos>=want){ xpush(tup,a); pos++; continue; } pos++; }
        items[n++]=a; }
    free(e->items); e->items=items; e->count=n; e->cap=e->count+1;
}
/* a parameter (not assigned) of a copy made for a literal None (f(None)): None passed on */
static int none_param_ref(Ck *c, Expr *a){
    AFunc *f=c->fn;
    if(a->kind!=EXPR_NAME || !f || !f->made_for_none || !f->def) return 0;
    for(int p=0;p<f->nparams && p<16;p++) if((f->made_for_none>>p&1) && !strcmp(f->def->params[p],a->name)) return !stores_name(c,f->body,f->nbody,a->name);
    return 0;
}
/* [], {}, set(), list(), dict(), tuple(), frozenset(): a container literal with nothing in it */
static int empty_container(Expr *a){
    if((a->kind==EXPR_LIST||a->kind==EXPR_DICT||a->kind==EXPR_SET||a->kind==EXPR_TUPLE) && a->count==0) return 1;
    if(a->kind==EXPR_CALL && a->count==0 && a->a->kind==EXPR_NAME){ const char *n=a->a->name; if(n[0]=='\001') n++;
        return !strcmp(n,"set")||!strcmp(n,"list")||!strcmp(n,"dict")||!strcmp(n,"tuple")||!strcmp(n,"frozenset"); }
    return 0;
}
/* a type whose unknown parts are all None-only variables ((1, None), [None]) */
static int only_none_unknown(Ty *t){
    t=ty_find(t);
    if(t->k==TY_VAR) return t->opt;
    if(t->k==TY_TUPLE){ for(int i=0;i<t->nelems;i++) if(!only_none_unknown(t->elems[i])) return 0; return 1; }
    if(t->k==TY_LIST||t->k==TY_SET) return only_none_unknown(t->elem);
    if(t->k==TY_DICT) return only_none_unknown(ty_dkey(t)) && only_none_unknown(t->elem);
    return ty_known(t);
}
/* ... those variables: Optional[int] (as they become in the end) */
static void default_none_vars(Ty *t){
    t=ty_find(t);
    if(t->k==TY_TUPLE){ for(int i=0;i<t->nelems;i++){ Ty *e=ty_find(t->elems[i]); if(e->k==TY_VAR && e->opt) e->link=default_of(e,TY_INT_T); else default_none_vars(e); } }
    else if(t->k==TY_LIST||t->k==TY_SET||t->k==TY_DICT){ Ty *e=ty_find(t->elem); if(e->k==TY_VAR && e->opt) e->link=default_of(e,TY_INT_T); else default_none_vars(e); }
}
/* a copy picked in an earlier pass for a call whose argument has become optional since (a default None of the
   caller's): its parameter of a TypeVar becomes optional too (None may come) */
static void widen_picked(Ck *c, Expr *e, AFunc *fn, AFunc *g){
    int first= fn->cls && !fn->is_static ? 1 : 0; Stmt *def=fn->def;
    if(!def) return;
    for(int i=0, pos=first;i<e->count;i++){ Expr *a=e->items[i]; int p=-1;
        if(a->akind==1||a->akind==2) return;
        if(a->akind==3){ for(int k=0;k<fn->nparams;k++) if(k!=fn->dstar && !strcmp(def->params[k],a->kw)) p=k; }
        else { p=pos; if(fn->star<0 || pos<fn->star) pos++; }
        if(p<0 || p>=g->nparams || p>=def->param_count || p==fn->star || p==fn->dstar || p>=16) continue;
        if(!def->annotations || p>=def->annotation_cap || !def->annotations[p] || !mentions_typevar(fn->mod,def->annotations[p])) continue;
        Ty *t=xinfo(a)->ty; if(!t) continue; t=ty_find(t);
        Ty *q=ty_find(g->params[p]->ty);
        if(t->opt && t->k!=TY_VAR && t->k!=TY_VOID && !q->opt && q->k==t->k){ set_opt(c,q); c->changed=1; } }
}
AFunc *pick_instance(Ck *c, Expr *e, AFunc *fn, XInfo *xi){
    if(xi->fn && (xi->fn==fn || xi->fn->origin==fn)){ widen_picked(c,e,fn,xi->fn); return xi->fn; }   /* (decided in an earlier pass) */
    Ty *at[16]={0}, *fnarg[16]={0}; Stmt *def=fn->def; int unknown=0, lam_generic=0, first= fn->cls && !fn->is_static ? 1 : 0;   /* (a method: after self) */
    pack_args(c,e,fn,first);
    unsigned given=0, fnm=0, omit=0, nonelit=0;
    for(int i=0, pos=first;i<e->count;i++){ Expr *a=e->items[i]; int p=-1;
        if(a->akind==1||a->akind==2) return fn;
        if(a->akind==3){ for(int k=0;k<fn->nparams;k++) if(k!=fn->dstar && !strcmp(def->params[k],a->kw)) p=k;
            if(p<0 && fn->dstar>=0 && fn->dstar<16){                        /* name=value for **kwargs: its values' type decides */
                Ty *vt=ty_find(ck_expr(c,a));
                if(!ty_known(vt)){ unknown=1; continue; }
                Ty *prev=at[fn->dstar];
                if(prev && !ty_same(prev->elem,vt)) return fn;
                at[fn->dstar]=ty_dict(TY_STR_T,vt); given|=1u<<fn->dstar; continue; } }
        else { p=pos; if(fn->star<0 || pos<fn->star) pos++; }               /* (positional ones past *args: its items) */
        if(p==fn->star && fn->star>=0){ ck_expr(c,a); continue; }           /* (*args: one item type, not part of the choice) */
        if(p<0 || p>=fn->nparams || p==fn->dstar || p>=16) return fn;
        if(fn->defaults[p] && fn->defaults[p]->kind==EXPR_NONE && none_arg(c,a)){ ck_expr(c,a); continue; }   /* None given: as if left out */
        given|=1u<<p;
        if(a->kind==EXPR_NAME && !lookup(c,a->name) && def){                /* accumulate(xs, max): max as a function of the arguments the body gives it */
            int ar=stmt_call_arity(def->body,def->body_count,def->params[p]); if(ar>0) builtin_fn_value(c,a,ar); }
        at[p]=ty_find(ck_expr(c,a));
        if(c->defaulting && !ty_known(at[p]) && empty_container(a)){ default_elems(at[p]); at[p]=ty_find(at[p]); }   /* (set(), [] that nothing else decides) */
        if(c->defaulting && !ty_known(at[p]) && only_none_unknown(at[p])){ default_none_vars(at[p]); at[p]=ty_find(at[p]); }   /* ((1, None): its None an Optional[int]) */
        if(p<16 && (a->kind==EXPR_NONE || none_param_ref(c,a))) nonelit|=1u<<p;   /* (None, or a parameter of a copy made for None) */
        if(fn->defaults[p] && fn->defaults[p]->kind==EXPR_NONE && (at[p]->k==TY_FUNC || a->kind==EXPR_LAMBDA)) fnm|=1u<<p;   /* (key=f where key=None) */
        if(!ty_known(at[p]) || at[p]->k==TY_VOID){ if(a->kind!=EXPR_LAMBDA && at[p]->k!=TY_FUNC) unknown=1; if(at[p]->k==TY_FUNC) fnarg[p]=at[p]; at[p]=NULL; }   /* (not known yet: any; a function's
                                                                                   parameters may be what this call decides) */
        if(a->kind==EXPR_LAMBDA && def) lam_generic|=1<<p;
    }
    for(int p=first;p<fn->nparams && p<16;p++){                              /* a constant default left to it: its type (f(x: T = 0)) */
        Expr *d=fn->defaults[p];
        if(!(given>>p&1) && p!=fn->star && p!=fn->dstar && d && d->kind==EXPR_NONE) omit|=1u<<p;
        if((given>>p&1) || p==fn->star || p==fn->dstar || !d || !const_default(d) || d->kind==EXPR_NONE) continue;
        AModule *save=c->mod; c->mod=fn->mod; at[p]=ty_find(ck_default(c,d)); c->mod=save; }
    if(lam_generic){ int any=0; for(int p=0;p<16;p++) if((lam_generic>>p&1) && lambda_decides(fn->mod,def,p,at,first)) any=1; lam_generic=any; }
    if(unknown && !c->strict && !(nonelit && c->defaulting)) return NULL;                                     /* (decided once the arguments are known) */
    if(lam_generic && !unknown){                                           /* f(buf, lambda b: (...)) for f(b: bytes, g: Callable[[bytes], T]) -> T: */
        const char *h=instantiate(c,fn);                                   /* what T is comes from the lambda: a copy for this call */
        AFunc *g=(AFunc*)module_sym(fn->mod,h)->p; g->origin=fn;
        fn->insts=(AFunc**)xrealloc(fn->insts,sizeof(AFunc*)*(size_t)(fn->ninsts+1)); fn->insts[fn->ninsts++]=g;
        g->none_omit=omit; g->none_fn=fnm; g->none_given=given;
        c->changed=1; xi->fn=g;
        return g; }
    { int any=0; for(int p=0;p<16;p++) if(at[p]) any=1;
      if(!any && !(nonelit && c->defaulting) && !((fn->none_omit&fnm)||(fn->none_fn&omit)||(fn->none_folded&given))){ if(c->strict||!e->count||!unknown){ fn->direct=1; none_use(c,fn,omit,fnm,given,e->line); return fn; } return NULL; } }   /* nothing known yet: wait (in the end, or with only lambdas: the function itself) */
    c->none_lit=nonelit;
    AFunc *g=instance_for_none(c,fn,at,fnarg,omit,fnm,given,e->line);
    c->none_lit=0;
    xi->fn=g;
    return g;
}
/* an instance's calls leave out parameters omit (default None) and give functions to fnm */
static void none_use(Ck *c, AFunc *g, unsigned omit, unsigned fnm, unsigned gv, int line){
    if(!g->none_calls++ || (omit&~g->none_omit) || (fnm&~g->none_fn)) c->changed=1;   /* (`p is None` tests waiting for them) */
    g->none_omit|=omit; g->none_fn|=fnm; g->none_given|=gv; (void)line;
}
/* the instance of generic fn for arguments of types at[param] (NULL: any; fnarg: a function not fully known) */
static AFunc *instance_for(Ck *c, AFunc *fn, Ty **at, Ty **fnarg){ return instance_for_none(c,fn,at,fnarg,0,0,0,0); }
/* ... where the call leaves out the parameters omit (a default None) and gives functions to fnm: not the
   instance of calls that do otherwise (`key is None` is decided in each) */
/* parameter p of generic fn is of a TypeVar (its copies: a type per kind of argument) */
static int generic_param(AFunc *fn, int p){
    Stmt *d=fn->pristine?fn->pristine:fn->def;
    return d && p<d->param_count && d->annotations && p<d->annotation_cap && d->annotations[p] && mentions_typevar(fn->mod,d->annotations[p]);
}
AFunc *instance_for_none(Ck *c, AFunc *fn, Ty **at, Ty **fnarg, unsigned omit, unsigned fnm, unsigned gv, int line){
    for(int k=-1;k<fn->ninsts;k++){ AFunc *cand=k<0?fn:fn->insts[k]; int ok=1;
        if((cand->none_omit&fnm) || (cand->none_fn&omit) || (cand->none_folded&gv)) continue;   /* (a copy where it is always None) */
        for(int p=0;p<fn->nparams && p<16 && ok;p++){ Ty *q=ty_find(cand->params[p]->ty);
            if(at[p] && p==fn->dstar && q->k==TY_DICT){ Ty *qe=ty_find(q->elem); if(qe->k!=TY_VAR && !ty_same(qe,at[p]->elem)) ok=0; }   /* (**kwargs: T) */
            else if(!at[p] && ((c->none_lit>>p&1) || ((omit>>p&1) && generic_param(fn,p))) && !q->opt && (q->k==TY_LIST||q->k==TY_DICT||q->k==TY_SET||q->k==TY_TUPLE||q->k==TY_STR||q->k==TY_BYTES
                    ||q->k==TY_INT||q->k==TY_FLOAT||q->k==TY_BOOL||q->k==TY_OBJ)) ok=0;    /* (None given or left out: not a copy for values that are never None) */
            else if(at[p]){ if(q->k!=TY_VAR && !ty_same(q,at[p])) ok=0;
                if(q->k==TY_SET && at[p]->k==TY_SET && (q->tup!=0)!=(at[p]->tup!=0)) ok=0; }   /* (a frozenset's copy: isinstance() decided otherwise) */
            else if(fnarg && fnarg[p] && q->k!=TY_VAR && q->k!=TY_FUNC) ok=0; }   /* (a function not fully known yet: not where another kind went) */
        if(ok){ if(cand==fn) fn->direct=1; none_use(c,cand,omit,fnm,gv,line); bind_copies(c,cand,at); return cand; } }
    const char *h=instantiate(c,fn);
    AFunc *g=(AFunc*)module_sym(fn->mod,h)->p; g->origin=fn;
    fn->insts=(AFunc**)xrealloc(fn->insts,sizeof(AFunc*)*(size_t)(fn->ninsts+1)); fn->insts[fn->ninsts++]=g;
    c->changed=1;
    g->made_for_none=c->none_lit;
    none_use(c,g,omit,fnm,gv,line);
    bind_copies(c,g,at);
    return g;
}
const char *instantiate(Ck *c, AFunc *fn){
    Stmt *copy=stmt_clone(c,fn->mod,fn->pristine);
    if(fn->is_clsm && copy->decorator_count) classmethod_def(fn->cls,copy);   /* (the copy is of the source: its cls parameter goes too) */
    copy->decorator_count=0;
    for(int i=0;i<copy->param_count;i++) if(copy->annotations && i<copy->annotation_cap && protocol_annotation(copy->annotations[i])) copy->annotations[i]=NULL;   /* Iterable[T]: what the call gives */
    AModule *save=c->mod; c->mod=fn->mod; c->mk_static=fn->is_static;
    AFunc *g=new_func(c,NULL,fn->mod,fn->cls,copy,NULL);                   /* (a method's copy: of its class, called directly) */
    c->mod=save; c->mk_static=0;
    char tag[32]; snprintf(tag,sizeof tag,"#%d",++fn->ninst);
    char *h=hidden_name(fn->name,NULL); char *hn=(char*)xmalloc(strlen(h)+strlen(tag)+1); strcpy(hn,h); strcat(hn,tag); free(h);
    symtab_add(&fn->mod->syms,hn,AS_FUNC,g);                    /* (in the function's module: it may be called from another) */
    return symtab_find(&fn->mod->syms,hn)->name;
}
