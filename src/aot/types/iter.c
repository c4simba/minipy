/* ========================= Typed compiler, types: iteration =========================
   Element types of iterables, the iterator protocol, comprehensions
   and generator expressions. */

#include "check.h"

Ty *elem_of(Ck *c, Ty *t, int line, const char *what){
    t=ty_find(t);
    if(t->k==TY_VAR) return pending(c,line,what);
    if(t->k==TY_STR) return TY_STR_T;
    if(t->k==TY_BYTES) return TY_INT_T;
    if(t->k==TY_LIST||t->k==TY_SET) return t->elem;
    if(t->k==TY_DICT) return ty_dkey(t);
    if(t->k==TY_FILE) return t->elem?t->elem:TY_STR_T;   /* its lines */
    if(t->k==TY_GEN) return t->elem;
    if(t->k==TY_OBJ){                                    /* for x in obj: obj.__iter__() gives a generator or a list */
        AFunc *m=aot_find_method(t->cls,"__iter__");
        if(m && !m->is_static && m->nparams==1){ Ty *r=ty_find(m->ret);
            if(r->k==TY_VAR) return pending(c,line,what);
            if(r->k==TY_GEN||r->k==TY_LIST) return r->elem;
            ck_err(c,line,"%s.__iter__ must return a generator or a list, not %s",t->cls->name,ty_name(r)); }
    }
    if(t->k==TY_TUPLE && t->names) ck_err(c,line,"%s (a dict literal with values of different types) cannot be iterated in compiled code",ty_name(t));
    if(t->k==TY_TUPLE && t->nelems>0){
        for(int i=1;i<t->nelems;i++) if(!ty_same(t->elems[i],t->elems[0])) ck_err(c,line,"only a tuple of one item type can be iterated, not %s",ty_name(t));
        return t->elems[0];
    }
    if(t->k==TY_TUPLE && !t->names) return ty_var();                    /* () : no items (of any type) */
    ck_err(c,line,"%s is not iterable",ty_name(t));
}
int is_builtin_call(Ck *c, Expr *e, const char *name){
    return e->kind==EXPR_CALL && e->a->kind==EXPR_NAME && !strcmp(e->a->name,name) && !lookup(c,name);
}
/* Types produced by iterating `it` into n targets. range/enumerate/zip/d.items() are understood directly. */
/* An iterable object whose __iter__() gives an iterator (an object with __next__): it
   becomes __mpy_iter(obj.__iter__()), a generator of the __next__() values (_mpy_iter.py),
   which every loop and builtin takes */
static int iterator_object(Ty *t){
    t=ty_find(t); if(t->k!=TY_OBJ) return 0;
    AFunc *m=aot_find_method(t->cls,"__iter__"); if(!m || m->is_static || m->nparams!=1) return 0;
    Ty *r=ty_find(m->ret);
    return r->k==TY_OBJ && aot_find_method(r->cls,"__next__");
}
void iterator_protocol(Ck *c, Expr *it){
    if(!it || it->akind) return;
    if(enum_members_expr(c,it)) return;                                  /* for m in Color: its members */
    if(it->kind!=EXPR_NAME && it->kind!=EXPR_ATTRIBUTE && it->kind!=EXPR_CALL && it->kind!=EXPR_INDEX) return;   /* (only these can be objects) */
    if(it->kind==EXPR_CALL && it->a->kind==EXPR_NAME && !lookup(c,it->a->name) && strcmp(it->a->name,"open")) return;   /* (a builtin's result; open() gives a file) */
    Ty *t=ty_find(ck_expr(c,it));
    if(!iterator_object(t)){                                             /* __iter__ a generator (or a list): obj.__iter__() */
        AFunc *m= t->k==TY_OBJ ? aot_find_method(t->cls,"__iter__") : NULL;
        if(m && !m->is_static && m->nparams==1){ Ty *r=ty_find(m->ret); if(r->k==TY_GEN || r->k==TY_LIST){
            Expr *orig=xnew(EXPR_NONE,it->line); *orig=*it; int line=it->line; memset(it,0,sizeof *it);
            Expr *meth=xnew(EXPR_ATTRIBUTE,line); meth->a=orig; meth->name="__iter__";
            it->kind=EXPR_CALL; it->line=line; it->a=meth; } }
        return; }
    if(!lookup(c,"__mpy_iter")) ck_err(c,it->line,"internal error: an iterator object without _mpy_iter.py");
    Expr *orig=xnew(EXPR_NONE,it->line); *orig=*it;
    Expr *meth=xnew(EXPR_ATTRIBUTE,it->line); meth->a=orig; meth->name="__iter__";
    Expr *call=xnew(EXPR_CALL,it->line); call->a=meth;
    int line=it->line; memset(it,0,sizeof *it);
    it->kind=EXPR_CALL; it->line=line; it->a=name_expr("__mpy_iter",line); xpush(it,call);
}
void iter_types(Ck *c, Expr *it, int n, Ty **out, int line){
    XInfo *xi=xinfo(it);
    if(is_builtin_call(c,it,"range")){
        if(it->count<1||it->count>3) ck_err(c,line,"range() takes 1 to 3 arguments");
        for(int i=0;i<it->count;i++){ if(it->items[i]->akind) ck_err(c,line,"range() takes positional arguments"); expect(c,TY_INT_T,ck_expr(c,it->items[i]),line,"a range() argument"); }
        xi->kind=X_BUILTIN; xi->name="range"; xi->ty=TY_INT_T;
        if(n!=1) ck_err(c,line,"range() produces one value per iteration");
        out[0]=TY_INT_T; return;
    }
    if(is_builtin_call(c,it,"reversed") && it->count==1 && is_builtin_call(c,it->items[0],"range")){   /* counting down */
        Expr *r=it->items[0];
        if(r->count<1||r->count>2) ck_err(c,line,"reversed(range(...)) takes range(stop) or range(start, stop)");
        for(int i=0;i<r->count;i++) expect(c,TY_INT_T,ck_expr(c,r->items[i]),line,"a range() argument");
        if(n!=1) ck_err(c,line,"range() produces one value per iteration");
        xi->kind=X_BUILTIN; xi->name="reversed_range"; xi->ty=TY_INT_T; out[0]=TY_INT_T; return;
    }
    if(is_builtin_call(c,it,"enumerate")){
        if(it->count<1||it->count>2) ck_err(c,line,"enumerate() takes 1 or 2 arguments");
        if(it->count==2) expect(c,TY_INT_T,ck_expr(c,it->items[1]),line,"enumerate() start");
        if(n!=1 && n!=2) ck_err(c,line,"cannot unpack (index, item) pairs into %d variables",n);
        xi->kind=X_BUILTIN; xi->name="enumerate"; xi->ty=TY_INT_T;
        iterator_protocol(c,it->items[0]);
        Ty *two[2]={TY_INT_T,elem_of(c,ck_expr(c,it->items[0]),line,"the iterable")};
        if(n==1){ out[0]=ty_tuple(two,2); return; }               /* for p in enumerate(xs): (i, x) */
        out[0]=two[0]; out[1]=two[1];
        return;
    }
    if(is_builtin_call(c,it,"zip")){
        if(it->count==1 && it->items[0]->akind==1 && pure_ref(it->items[0])){   /* zip(*t), t a tuple: zip(t[0], t[1] ...) */
            Expr *t=it->items[0]; Ty *tt=ty_find(ck_expr(c,t));
            if(tt->k==TY_VAR){ pending(c,line,"the unpacked argument"); out[0]=ty_var(); for(int i=1;i<n;i++) out[i]=ty_var(); return; }
            if(tt->k==TY_TUPLE && !tt->names){ Expr *base=MPY_NEW0(Expr); *base=*t; base->akind=0; it->count=0;
                for(int i=0;i<tt->nelems;i++){ Expr *ix=xnew(EXPR_INDEX,line); ix->a=base; ix->b=lit_int(line,i); xpush(it,ix); } } }
        if(it->count<1 || it->count>8) ck_err(c,line,"zip() of 1 to 8 iterables in compiled code");
        for(int i=0;i<it->count;i++) if(it->items[i]->akind) ck_err(c,line,"zip() takes positional arguments (strict= is not supported in compiled code)");
        if(n!=1 && n!=it->count) ck_err(c,line,"cannot unpack zip() of %d iterables into %d variables",it->count,n);
        xi->kind=X_BUILTIN; xi->name="zip"; xi->ty=TY_INT_T;
        Ty *ts[8]; for(int i=0;i<it->count;i++){ iterator_protocol(c,it->items[i]); ts[i]=elem_of(c,ck_expr(c,it->items[i]),line,"the iterable"); }
        if(n==1){ out[0]=ty_tuple(ts,it->count); return; }       /* for t in zip(a, b): a tuple */
        for(int i=0;i<n;i++) out[i]=ts[i];
        return;
    }
    if(it->kind==EXPR_CALL && it->a->kind==EXPR_ATTRIBUTE && !strcmp(it->a->name,"items") && it->count==0 && ty_find(ck_expr(c,it->a->a))->k!=TY_OBJ){
        Ty *d=ty_find(ck_expr(c,it->a->a));
        if(d->k==TY_VAR){ out[0]=pending(c,line,"the dictionary"); if(n==2) out[1]=ty_var(); return; }
        if(d->k!=TY_DICT) ck_err(c,line,"items() needs a dict");
        if(n!=1 && n!=2) ck_err(c,line,"cannot unpack (key, value) pairs into %d variables",n);
        xi->kind=X_TMETHOD; xi->name="items"; xi->ty=TY_VOID_T;
        if(n==1){ Ty *two[2]={ty_dkey(d),d->elem}; out[0]=ty_tuple(two,2); return; }   /* for kv in d.items() */
        out[0]=ty_dkey(d); out[1]=d->elem; return;
    }
    iterator_protocol(c,it);
    Ty *t=ty_find(it->kind==EXPR_TUPLE ? ck_tuple_as_list(c,it) : ck_expr(c,it));
    Ty *el=elem_of(c,t,line,"the iterable");
    if(n==1){ out[0]=el; return; }
    Ty *e2=ty_find(el);                               /* for a, b in list_of_lists */
    if(e2->k==TY_VAR){ for(int i=0;i<n;i++) out[i]=pending(c,line,"the loop variables"); return; }
    if(e2->k==TY_TUPLE){
        if(e2->nelems!=n) ck_err(c,line,"cannot unpack %s into %d variables",ty_name(e2),n);
        for(int i=0;i<n;i++) out[i]=e2->elems[i];
        return;
    }
    if(e2->k!=TY_LIST) ck_err(c,line,"cannot unpack %s into %d variables",ty_name(e2),n);
    for(int i=0;i<n;i++) out[i]=e2->elem;
}

/* the names of a comprehension clause's target: its own variables too */
static void comp_target_vars(Ck *c, Expr *t, int line){
    if(t->kind==EXPR_NAME){ XInfo *x=xinfo(t); if(!x->var) x->var=scope_hidden(c,t->name); x->own_var=1; cs_push(c,t->name,x->var,line); return; }
    if(t->kind==EXPR_TUPLE||t->kind==EXPR_LIST) for(int i=0;i<t->count;i++) comp_target_vars(c,t->items[i],line);
}
/* The loop variables of a comprehension are its own (hidden locals/globals). */
/* async for: an async generator; for: not one */
void async_iter_ok(Ck *c, Expr *it, int is_async, int line){
    Ty *t=xinfo(it)->ty?ty_find(xinfo(it)->ty):NULL;
    if(!t || t->k==TY_VAR) return;
    int ag= t->k==TY_GEN && t->tup;
    if(is_async && !ag) ck_err(c,line,"'async for' requires an object with __aiter__ method, got %s",ty_name(t));
    if(!is_async && ag) ck_err(c,line,"'async_generator' object is not iterable");
}
/* a copy of expression e (unchecked) in which the name var (when not NULL) is a copy of with */
Expr *expr_subst(Expr *e, const char *var, Expr *with){
    if(!e) return NULL;
    if(var && e->kind==EXPR_NAME && e->name && !strcmp(e->name,var)) return expr_subst(with,NULL,NULL);
    Expr *n=MPY_NEW0(Expr); *n=*e; n->ty=NULL; n->cm_done=0;
    if(var && e->kind==EXPR_LAMBDA) for(int k=0;k<e->neparam;k++) if(!strcmp(e->eparams[k],var)) var=NULL;   /* (bound again there) */
    if(var && e->kind==EXPR_COMPREHENSION) for(int i=0;i<e->nclause;i++) for(int k=0;k<e->clauses[i].nvars;k++) if(!strcmp(e->clauses[i].vars[k],var)) var=NULL;
    n->a=expr_subst(e->a,var,with); n->b=expr_subst(e->b,var,with); n->c=expr_subst(e->c,var,with); n->d=expr_subst(e->d,var,with);
    if(e->items){ n->items=(Expr**)xmalloc(sizeof(Expr*)*(size_t)(e->cap>0?e->cap:1)); for(int i=0;i<e->count;i++) n->items[i]=expr_subst(e->items[i],var,with); }
    if(e->vals){ n->vals=(Expr**)xmalloc(sizeof(Expr*)*(size_t)(e->vcap>0?e->vcap:1)); for(int i=0;i<e->vcount;i++) n->vals[i]=expr_subst(e->vals[i],var,with); }
    if(e->edefaults){ n->edefaults=(Expr**)xmalloc(sizeof(Expr*)*(size_t)(e->neparam>0?e->neparam:1)); for(int i=0;i<e->neparam;i++) n->edefaults[i]=expr_subst(e->edefaults[i],var,with); }
    if(e->clauses){ n->clauses=(CompClause*)xmalloc(sizeof(CompClause)*(size_t)(e->ccap>0?e->ccap:1));
        for(int i=0;i<e->nclause;i++){ CompClause *f=&e->clauses[i], *t=&n->clauses[i]; *t=*f;
            t->iter=expr_subst(f->iter,var,with); t->target=expr_subst(f->target,var,with); t->target2=expr_subst(f->target2,var,with);
            if(f->conds){ t->conds=(Expr**)xmalloc(sizeof(Expr*)*(size_t)(f->ncond>0?f->ncond:1)); for(int k=0;k<f->ncond;k++) t->conds[k]=expr_subst(f->conds[k],var,with); } } }
    return n;
}
int pure_ref(Expr *e){ return e->kind==EXPR_NAME || (e->kind==EXPR_ATTRIBUTE && pure_ref(e->a)); }
/* [f(x) for x in t], t a tuple of several item types: [f(t[0]), f(t[1]), ...] (each its own types); 0: not one */
static int unroll_tuple_comp(Ck *c, Expr *e){
    if((e->comp_kind!='L' && e->comp_kind!='S' && e->comp_kind!='g') || e->nclause!=1) return 0;
    CompClause *cl=&e->clauses[0];
    if(cl->nvars!=1 || cl->target || cl->ncond || cl->is_async || !pure_ref(cl->iter)) return 0;
    Ty *t=ty_find(ck_expr(c,cl->iter));
    if(t->k!=TY_TUPLE || t->names || t->opt || t->nelems<2) return 0;
    int mixed=0; for(int i=1;i<t->nelems;i++) if(!ty_same_exact(ty_find(t->elems[i]),ty_find(t->elems[0]))) mixed=1;
    if(!mixed) return 0;
    Expr *elem=e->a; const char *var=cl->vars[0]; Expr *it=cl->iter; int line=e->line, ck=e->comp_kind;
    Expr **items=(Expr**)xmalloc(sizeof(Expr*)*(size_t)t->nelems);
    for(int i=0;i<t->nelems;i++){ Expr *ix=xnew(EXPR_INDEX,line); ix->a=it; char num[24]; snprintf(num,sizeof num,"%d",i);
        ix->b=py_front_expr(c->mod->unit->path,num,line); items[i]=expr_subst(elem,var,ix); }
    void *keep=e->ty; memset(e,0,sizeof *e); e->kind= ck=='S' ? EXPR_SET : EXPR_LIST; e->line=line; e->ty=keep;
    for(int i=0;i<t->nelems;i++) xpush(e,items[i]);
    free(items);
    return 1;
}
Ty *ck_comprehension(Ck *c, Expr *e){
    XInfo *xi=xinfo(e);
    if(unroll_tuple_comp(c,e)){ xi->kind=X_NONE; return ck_expr(c,e); }
    if(!xi->cvars){
        xi->cvars=(AVar**)memset(xmalloc(sizeof(AVar*)*(size_t)(2*e->nclause+1)),0,sizeof(AVar*)*(size_t)(2*e->nclause+1));
        for(int i=0;i<e->nclause;i++) for(int k=0;k<e->clauses[i].nvars && k<2;k++){
            const char *nm=e->clauses[i].vars[k];
            xi->cvars[2*i+k]=scope_hidden(c,nm);
        }
    }
    int save=c->ncscope; Ty *r;
    for(int i=0;i<e->nclause;i++){
        CompClause *cl=&e->clauses[i];
        if(cl->nvars>2) ck_err(c,e->line,"at most two loop variables are supported");
        Ty *ts[2]; iter_types(c,cl->iter,cl->nvars,ts,e->line);
        async_iter_ok(c,cl->iter,cl->is_async,e->line);
        for(int k=0;k<cl->nvars;k++){
            cs_push(c,cl->vars[k],xi->cvars[2*i+k],e->line);
            Expr nm; memset(&nm,0,sizeof nm); nm.kind=EXPR_NAME; nm.name=cl->vars[k]; ck_store(c,&nm,ts[k],e->line);
        }
        if(cl->target){ comp_target_vars(c,cl->target,e->line); ck_store(c,cl->target,ts[0],e->line); }   /* for (a, b), *c in ... */
        if(cl->target2){ comp_target_vars(c,cl->target2,e->line); ck_store(c,cl->target2,ts[1],e->line); }
        for(int k=0;k<cl->ncond;k++) no_void(c,ck_cond(c,cl->conds[k]),e->line);
    }
    if(e->comp_kind=='D'){ Ty *k=ck_expr(c,e->a); no_void(c,k,e->line); Ty *v=ck_expr(c,e->b); no_void(c,v,e->line); r=ty_dict(k,v); }
    else { Ty *el=ck_expr(c,e->a); no_void(c,el,e->line); r=ty_new(e->comp_kind=='S'?TY_SET:TY_LIST,el,NULL); }
    c->ncscope=save;
    return r;
}
/* range(...) / reversed(range(...)) / enumerate(...) / zip(...) / d.items(): forms only loops understand */
static int loop_form(Ck *c, Expr *it){
    if(is_builtin_call(c,it,"range")||is_builtin_call(c,it,"enumerate")||is_builtin_call(c,it,"zip")) return 1;
    if(is_builtin_call(c,it,"reversed") && it->count==1 && is_builtin_call(c,it->items[0],"range")) return 1;
    return it->kind==EXPR_CALL && it->a->kind==EXPR_ATTRIBUTE && !strcmp(it->a->name,"items") && it->count==0 && ty_find(ck_expr(c,it->a->a))->k!=TY_OBJ;
}
/* A generator expression as a value: a generator function of its own whose
   parameter .0 is the first iterable (evaluated where the expression is). */
Ty *ck_genexp(Ck *c, Expr *e){
    XInfo *xi=xinfo(e);
    AFunc *gf=xi->fn;
    int plain=!loop_form(c,e->clauses[0].iter);
    if(!gf){
        gf=MPY_NEW0(AFunc); gf->name="<genexpr>"; gf->mod=c->mod; gf->outer=c->fn; gf->genexp=e; gf->line=e->line;
        gf->star=gf->dstar=-1; gf->is_gen=1;
        gf->nparams=plain?1:0; gf->kwonly=gf->nparams;
        gf->params=MPY_NEW_ARR(AVar*,1); gf->defaults=MPY_NEW_ARR(Expr*,1);
        if(plain){ gf->params[0]=new_local(gf,".0",NULL); Expr *nm=xnew(EXPR_NAME,e->line); nm->name=".0"; xi->key=nm; }
        gf->yield_ty=ty_var(); gf->ret=ty_new(TY_GEN,gf->yield_ty,NULL);
        add_func(c->p,gf); xi->fn=gf;
        xi->cvars=(AVar**)memset(xmalloc(sizeof(AVar*)*(size_t)(2*e->nclause+1)),0,sizeof(AVar*)*(size_t)(2*e->nclause+1));
        for(int i=0;i<e->nclause;i++){ if(e->clauses[i].nvars>2) ck_err(c,e->line,"at most two loop variables are supported");
            for(int k=0;k<e->clauses[i].nvars;k++) xi->cvars[2*i+k]=hidden_local(gf,e->clauses[i].vars[k],NULL); }
    }
    gf->value_used=1; gf->ncalls++;
    if(plain){ Ty *t=ck_expr(c,e->clauses[0].iter); no_void(c,t,e->line);
        if(!unify(c,gf->params[0]->ty,t)) ck_err(c,e->line,"cannot iterate %s",ty_name(t)); }
    AFunc *save=c->fn; int savecs=c->ncscope;
    c->fn=gf;
    for(int i=0;i<e->nclause;i++){
        CompClause *cl=&e->clauses[i];
        Ty *ts[2]; iter_types(c,(i==0&&plain)?xi->key:cl->iter,cl->nvars,ts,e->line);
        for(int k=0;k<cl->nvars;k++){
            cs_push(c,cl->vars[k],xi->cvars[2*i+k],e->line);
            Expr nm; memset(&nm,0,sizeof nm); nm.kind=EXPR_NAME; nm.name=cl->vars[k]; ck_store(c,&nm,ts[k],e->line);
        }
        if(cl->target){ comp_target_vars(c,cl->target,e->line); ck_store(c,cl->target,ts[0],e->line); }   /* for (a, b), *c in ... */
        if(cl->target2){ comp_target_vars(c,cl->target2,e->line); ck_store(c,cl->target2,ts[1],e->line); }
        for(int k=0;k<cl->ncond;k++) no_void(c,ck_cond(c,cl->conds[k]),e->line);
    }
    Ty *el=ck_expr(c,e->a); no_void(c,el,e->line);
    c->ncscope=savecs; c->fn=save;
    if(!unify(c,gf->yield_ty,el)) ck_err(c,e->line,"generator items of different types (%s and %s)",ty_name(gf->yield_ty),ty_name(el));
    xi->kind=X_NONE;
    return gf->ret;
}
