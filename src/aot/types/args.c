/* ========================= Typed compiler, types: call arguments =========================
   Defaults, and the arguments of a call matched to the parameters. */

#include "check.h"

/* A constant default (a number, str, bytes, True / False / None) is put in at
   the call; any other is evaluated once, where its def / lambda is, and kept
   in the function object (as CPython does: f(x=[]) shares the list). */
int const_default(Expr *d){
    Expr *e=d;
    if(e->kind==EXPR_UNARY && e->op==T_MINUS && e->a->kind==EXPR_LITERAL) e=e->a;
    return e->kind==EXPR_LITERAL||e->kind==EXPR_TRUE||e->kind==EXPR_FALSE||e->kind==EXPR_NONE||(e->kind==EXPR_TUPLE && !e->count);   /* (() too: args=()) */
}
Ty *ck_default(Ck *c, Expr *d){ return ck_expr(c,d); }
/* raise X from Y: Y an exception (or a class of one, or None) */
void ck_raise_cause(Ck *c, Stmt *s){
    if(!s->expr2) return;
    Expr *y=aot_expr(c->mod->unit,&s->expr2);
    if(y->kind==EXPR_NONE) return;
    if(y->kind==EXPR_NAME){ ASym *x=lookup(c,y->name);
        if(x && x->kind==AS_CLASS){ AClass *cls=(AClass*)x->p; if(!aot_is_exception(cls)) ck_err(c,s->line,"exception causes must derive from BaseException");
            xinfo(y)->kind=X_CTOR; xinfo(y)->cls=cls; xinfo(y)->ty=ty_new(TY_OBJ,NULL,cls); return; } }
    Ty *t=ty_find(ck_expr(c,y)); no_void(c,t,s->line);
    if(t->k!=TY_OBJ || !aot_is_exception(t->cls)) ck_err(c,s->line,"exception causes must derive from BaseException, not %s",ty_name(t));
}
/* the defaults of f, checked where f is defined (they are evaluated there) */
void ck_def_defaults(Ck *c, AFunc *f, int line){
    for(int i=0;i<f->nparams;i++){ Expr *d=f->defaults[i]; if(!d || i==f->star || i==f->dstar) continue;
        if((f->pristine || f->origin) && const_default(d) && d->kind!=EXPR_NONE){   /* (a generic one's: checked at the calls that use it) */
            Ty *t=ty_find(ck_default(c,d)); Ty *pt=ty_find(f->params[i]->ty);
            if(pt->k==TY_VAR || ty_same(t,pt)) expect(c,f->params[i]->ty,t,line,"a default");
            else f->defaults_mismatch|=1u<<(i&31);                  /* (the function object leaves that one out) */
            continue; }
        Ty *t=ck_expr_want(c,d,f->params[i]->ty); no_void(c,t,line);
        char what[160]; snprintf(what,sizeof what,"the default of %s's parameter '%s'",f->name,f->params[i]->name);
        expect(c,f->params[i]->ty,t,line,what); }
}
void int_push(int **v, int *n, int x){ if(!(*n&7)) *v=(int*)xrealloc(*v,sizeof(int)*(size_t)(*n+8)); (*v)[(*n)++]=x; }
/* Match call arguments to fn's parameters (from `skip`) and check their types:
   positional, keyword, extra positionals into *args, extra keywords into
   **kwargs, f(*tuple) / f(*list) spread over parameters, f(*xs) / f(**d)
   feeding *args / **kwargs. */
/* a where a str is wanted: a.__fspath__() when a is an object with __fspath__ (os.PathLike: a
   pathlib path for open(), os.path.join(), shutil.copy() ...); the rewritten one's type in *t */
void fspath_arg(Ck *c, Expr *a, Ty *want, Ty **t){
    Ty *w=ty_find(want), *o=ty_find(*t);
    if(w->k!=TY_STR || o->k!=TY_OBJ || !aot_find_method(o->cls,"__fspath__")) return;
    Expr *inner=MPY_NEW0(Expr); *inner=*a; inner->akind=0; inner->kw=NULL;
    Expr *at=xnew(EXPR_ATTRIBUTE,a->line); at->a=inner; at->name="__fspath__";
    int ak=a->akind, line=a->line; char *kw=a->kw;
    memset(a,0,sizeof *a); a->kind=EXPR_CALL; a->line=line; a->a=at; a->akind=ak; a->kw=kw;
    *t=ck_expr(c,a);
}
/* f(msg, a, b) where f's *args is annotated sys._PercentArgs (logging): f(msg % (a, b)), formatted here */
static void percent_args(Ck *c, Expr *e, AFunc *fn, int skip){
    int pm=fn->star-1-skip, npos=0, first=-1;
    for(int i=0;i<e->count;i++){ Expr *a=e->items[i];
        if(a->akind==1) ck_err(c,e->line,"%s(*args): the message's arguments are formatted where it is called; pass them one by one in compiled code",fn->name);
        if(a->akind==0){ if(npos==pm) first=i; npos++; } }
    if(pm<0 || first<0 || npos<=pm+1) return;
    Expr *msg=e->items[first];
    if(msg->kind!=EXPR_LITERAL || msg->tok->kind!=T_STRING || msg->tok->i)
        ck_err(c,e->line,"%s(): a message with arguments must be a literal format string in compiled code (or format it yourself: f\"...\")",fn->name);
    int n=npos-pm-1;
    Expr *tup=xnew(EXPR_TUPLE,e->line); tup->count=n; tup->items=MPY_NEW_ARR(Expr*,n);
    for(int j=0;j<n;j++){ tup->items[j]=e->items[first+1+j]; }
    Expr *m=MPY_NEW0(Expr); *m=*msg; m->akind=0; m->kw=NULL; m->ty=NULL;
    Expr *bin=xnew(EXPR_BINARY,e->line); bin->op=T_PERCENT; bin->a=m; bin->b=tup;
    e->items[first]=bin;
    for(int j=first+1+n;j<e->count;j++) e->items[j-n]=e->items[j];
    e->count-=n;
}
/* parameters whose default is sys._site(...) / sys.exception(): put in at the call (where it is) */
static void site_args(Ck *c, Expr *e, AFunc *fn, int skip, XInfo *xi){
    for(int k=skip;k<fn->nparams;k++){ if(xi->argmap[k]>=0 || k==fn->star || k==fn->dstar) continue;
        Expr *d=fn->def->pdefaults[k], *v=NULL;
        c->site_callee= e->a->kind==EXPR_ATTRIBUTE ? (e->a->kw ? e->a->kw : e->a->name) : e->a->kind==EXPR_NAME ? e->a->name : NULL;
        if(is_sys_call(d,"_site")){ v=site_value(c,d,e->line); c->site_callee=NULL; }
        else if(is_sys_call(d,"exception")){ v=xnew(EXPR_CALL,e->line); lexical_exception(c,v); }
        if(!v) continue;
        v->akind=3; v->kw=fn->def->params[k];
        e->items=(Expr**)xrealloc(e->items,sizeof(Expr*)*(size_t)(e->count+1)); e->items[e->count++]=v;
        xi->argmap[k]=e->count-1; }
}
/* A module variable whose type is not known yet (its assignment is checked
   later in the pass), passed where an instance of a class is wanted: its own
   assignment says its class - a subclass, maybe - not this parameter (unless
   nothing does: then, defaulting, the parameter's). */
static int global_not_yet(Ck *c, Expr *a, Ty *t, Ty *pt){
    if(c->defaulting || c->strict || a->kind!=EXPR_NAME) return 0;
    XInfo *xi=xinfo(a);
    return xi->kind==X_VAR && xi->var->global && ty_find(t)->k==TY_VAR && ty_find(pt)->k==TY_OBJ;
}
void ck_args(Ck *c, Expr *e, AFunc *fn, int skip, XInfo *xi){
    if(fn->check_failed) ck_err(c,e->line,"%s() has errors (reported above): it cannot be called here",fn->name);   /* (check mode) */
    pack_args(c,e,fn,skip);
    if(fn->star>=0 && fn->def && fn->def->annotations && fn->star<fn->def->annotation_cap){ Expr *an=fn->def->annotations[fn->star];
        if(an && an->kind==EXPR_ATTRIBUTE && !strcmp(an->name,"_PercentArgs") && an->a->kind==EXPR_NAME && !strcmp(an->a->name,"sys")) percent_args(c,e,fn,skip); }
    int np=fn->nparams, pi=skip;
    int poslimit=fn->star>=0?fn->star:fn->kwonly<np?fn->kwonly:fn->dstar>=0?fn->dstar:np;
    fn->ncalls++;
    for(int i=0;i<16;i++){ xi->argmap[i]=-1; xi->argelem[i]=0; }
    xi->nxargs=0; xi->nkwargs=0; xi->splat=0; xi->dsplat=0; xi->emptysplat=0;
    for(int i=0;i<e->count;i++){
        Expr *a=e->items[i];
        int slot;
        if(a->akind==1){                                   /* *xs */
            Ty *t=ty_find(ck_expr(c,a));
            if(t->k==TY_VAR){ pending(c,e->line,"the unpacked argument"); continue; }
            if(t->k==TY_TUPLE && t->names) ck_err(c,e->line,"*%s: a dict literal cannot be spread",ty_name(t));
            if(pi<poslimit && t->k==TY_TUPLE){              /* spread a tuple over the parameters */
                for(int k=0;k<t->nelems;k++){
                    if(pi>=poslimit) ck_err(c,e->line,"%s() takes %d positional argument%s",fn->name,poslimit-skip,poslimit-skip==1?"":"s");
                    xi->argmap[pi]=i; xi->argelem[pi]=k+1; pi++; }
                continue;
            }
            if(pi<poslimit && t->k==TY_LIST){              /* the rest of the positional parameters (length checked when called) */
                if(fn->star>=0) ck_err(c,e->line,"f(*list) into both parameters and *%s is not supported in compiled code",fn->params[fn->star]->name);
                for(int k=0;pi<poslimit;k++){ xi->argmap[pi]=i; xi->argelem[pi]=k+1; pi++; }
                continue;
            }
            if(fn->star<0 && t->k==TY_LIST && !xi->emptysplat){ xi->emptysplat=i+1; continue; }   /* nothing left for it: must be empty */
            if(fn->star<0) ck_err(c,e->line,"%s() takes no *args",fn->name);
            if(xi->splat) ck_err(c,e->line,"only one *argument per call is supported in compiled code");
            if(t->k!=TY_LIST && t->k!=TY_TUPLE) ck_err(c,e->line,"*argument must be a list or a tuple, not %s",ty_name(t));
            xi->splat=i+1; continue;
        }
        if(a->akind==2){                                   /* **d */
            if(fn->dstar<0) ck_err(c,e->line,"%s() takes no **kwargs (f(**d) needs a **kwargs parameter in compiled code)",fn->name);
            if(xi->dsplat) ck_err(c,e->line,"only one **argument per call is supported in compiled code");
            xi->dsplat=i+1; continue;
        }
        if(a->akind==3){
            slot=-1; for(int k=skip;k<np;k++) if(k!=fn->star && k!=fn->dstar && !strcmp(fn->params[k]->name,a->kw)) slot=k;
            if(slot<0){ if(fn->dstar>=0){ int_push(&xi->kwargs,&xi->nkwargs,i); continue; } ck_err(c,e->line,"%s() has no parameter '%s'",fn->name,a->kw); }
        } else {
            if(pi>=poslimit){ if(fn->star>=0){ int_push(&xi->xargs,&xi->nxargs,i); continue; }
                ck_err(c,e->line,"%s() takes %d argument%s",fn->name,poslimit-skip,poslimit-skip==1?"":"s"); }
            slot=pi++;
        }
        if(xi->argmap[slot]>=0) ck_err(c,e->line,"%s() got two values for '%s'",fn->name,fn->params[slot]->name);
        xi->argmap[slot]=i;
    }
    if(fn->def && fn->def->pdefaults) site_args(c,e,fn,skip,xi);
    for(int k=skip;k<np;k++){
        char what[200]; snprintf(what,sizeof what,"parameter '%s' of %s()",fn->params[k]->name,fn->name);
        Ty *pt=fn->params[k]->ty;
        if(k==fn->star){                                    /* extra positionals: each an item of *args */
            Ty *el=ty_find(pt)->elem;
            for(int j=0;j<xi->nxargs;j++){ Ty *t=ck_expr_want(c,e->items[xi->xargs[j]],el); fspath_arg(c,e->items[xi->xargs[j]],el,&t); no_void(c,t,e->line); expect(c,el,t,e->line,what); }
            if(xi->splat){ Ty *t=ty_find(xinfo(e->items[xi->splat-1])->ty);
                if(t->k==TY_LIST) expect(c,el,t->elem,e->line,what);
                else for(int j=0;j<t->nelems;j++) expect(c,el,t->elems[j],e->line,what); }
            continue;
        }
        if(k==fn->dstar){
            Ty *el=ty_find(pt)->elem;
            for(int j=0;j<xi->nkwargs;j++){ Ty *t=ck_expr_want(c,e->items[xi->kwargs[j]],el); no_void(c,t,e->line); expect(c,el,t,e->line,what); }
            if(xi->dsplat) expect(c,pt,ck_expr(c,e->items[xi->dsplat-1]),e->line,what);
            continue;
        }
        if(xi->argmap[k]>=0 && xi->argelem[k]){                /* item of f(*seq) */
            Ty *t=ty_find(xinfo(e->items[xi->argmap[k]])->ty);
            expect(c,pt,t->k==TY_TUPLE?t->elems[xi->argelem[k]-1]:t->elem,e->line,what);
        }
        else if(xi->argmap[k]>=0){ Expr *a=e->items[xi->argmap[k]]; int ep=is_endpoint(pt); c->ep_value+=ep;
            Ty *t=ck_expr_want(c,a,pt); c->ep_value-=ep;
            fspath_arg(c,a,pt,&t);
            if(int_enum_ty(t) && (ty_find(pt)->k==TY_INT || ty_find(pt)->k==TY_FLOAT)) t=as_enum_value(c,a);   /* f(HTTPStatus.OK) for f(code: int) */
            no_void(c,t,e->line); if(!global_not_yet(c,a,t,pt)) expect(c,pt,t,e->line,what); ep_arg(c,pt,a,e->line); }
        else if(fn->defaults[k]){ if(const_default(fn->defaults[k])){ AModule *save=c->mod; c->mod=fn->mod; Ty *t=ck_expr_want(c,fn->defaults[k],pt); c->mod=save; expect(c,pt,t,e->line,what); } }
        else ck_err(c,e->line,"%s() is missing argument '%s'",fn->name,fn->params[k]->name);
    }
}
/* f(args) where f is a function value of type ft */
