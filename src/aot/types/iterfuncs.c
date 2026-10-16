/* ========================= Typed compiler, types: functions of iterables =========================
   key= functions; map, filter, zip, enumerate, iter, reduce rewritten
   into comprehensions; open(). */

#include "check.h"

static Expr *key_as_lambda(Ck *c, Expr *key);

/* key= / reverse= of sorted, sort, min, max: taken out of the argument list
   (kept in XInfo), the key checked against the element type -> key type */
void take_sort_kwargs(Ck *c, Expr *e, int allow_reverse){
    XInfo *xi=xinfo(e); int n=0;
    for(int i=0;i<e->count;i++){ Expr *a=e->items[i];
        if(a->akind==3 && !strcmp(a->kw,"key")) xi->key=key_as_lambda(c,a);
        else if(a->akind==3 && !strcmp(a->kw,"reverse") && allow_reverse) xi->rev=a;
        else e->items[n++]=a; }
    e->count=n;
}
/* key=len / key=abs / key=str.lower: the same as lambda k: len(k) / ... */
static Expr *key_as_lambda(Ck *c, Expr *key){
    if(key->kind==EXPR_CALL && key->count==1 && !key->items[0]->akind){            /* cmp_to_key(f): lambda k: __ppy_CmpKey(k, f) */
        Expr *fn=key->a; ASym *s=fn->kind==EXPR_NAME ? lookup(c,fn->name) : NULL;
        if((s && s->kind==AS_SYS && !strcmp((const char*)s->p,"functools.cmp_to_key")) || (fn->kind==EXPR_ATTRIBUTE && is_bmod(c,fn->a,"functools") && !strcmp(fn->name,"cmp_to_key"))){
            Expr *call=xnew(EXPR_CALL,key->line); call->a=name_expr("__ppy_CmpKey",key->line);
            xpush(call,name_expr("__key",key->line)); xpush(call,key->items[0]);
            Expr *lam=xnew(EXPR_LAMBDA,key->line); lam->a=call;
            lam->eparams=PPY_NEW_ARR(char*,1); lam->eparams[0]="__key"; lam->neparam=1;
            return lam; } }
    if(key->kind==EXPR_CALL && key->count>=1){                                   /* operator.itemgetter(i, ...), attrgetter('a'), methodcaller('m', ...) */
        Expr *fn=key->a; const char *opn=NULL;
        if(fn->kind==EXPR_NAME){ ASym *s=lookup(c,fn->name); if(s && s->kind==AS_FUNC && !strcmp(((AFunc*)s->p)->mod->name,"operator")) opn=fn->name; }
        else if(fn->kind==EXPR_ATTRIBUTE){ AModule *m=module_expr(c,fn->a); if(m && !strcmp(m->name,"operator")) opn=fn->name; }
        int lits=1; for(int i=0;i<key->count;i++) if(key->items[i]->akind || !(key->items[i]->kind==EXPR_LITERAL && key->items[i]->tok->kind==T_STRING)) lits=0;
        if(opn && (!strcmp(opn,"itemgetter") || ((!strcmp(opn,"attrgetter") || !strcmp(opn,"methodcaller")) && (lits || !strcmp(opn,"methodcaller"))))){
            Expr *body=NULL; int line=key->line;
            if(!strcmp(opn,"methodcaller")){
                Expr *nm=key->items[0]; if(nm->akind || !(nm->kind==EXPR_LITERAL && nm->tok->kind==T_STRING)) ck_err(c,line,"methodcaller(): the name must be a literal in compiled code");
                Expr *m=xnew(EXPR_ATTRIBUTE,line); m->a=name_expr("__key",line); m->name=nm->tok->text;
                body=xnew(EXPR_CALL,line); body->a=m; for(int i=1;i<key->count;i++) xpush(body,key->items[i]); }
            else {
                Expr *parts[16]; int n=key->count>16?16:key->count;
                for(int i=0;i<n;i++){ Expr *it=key->items[i];
                    if(opn[0]=='i'){ Expr *ix=xnew(EXPR_INDEX,line); ix->a=name_expr("__key",line); ix->b=it; parts[i]=ix; }
                    else { Expr *cur=name_expr("__key",line); char *path=xstrdup2(it->tok->text), *save=NULL;   /* 'a.b': __key.a.b */
                        for(char *seg=strtok_r(path,".",&save);seg;seg=strtok_r(NULL,".",&save)){ Expr *at=xnew(EXPR_ATTRIBUTE,line); at->a=cur; at->name=xstrdup2(seg); cur=at; }
                        parts[i]=cur; } }
                if(n==1) body=parts[0];
                else { body=xnew(EXPR_TUPLE,line); for(int i=0;i<n;i++) xpush(body,parts[i]); } }
            Expr *lam=xnew(EXPR_LAMBDA,line); lam->a=body;
            lam->eparams=PPY_NEW_ARR(char*,1); lam->eparams[0]="__key"; lam->neparam=1;
            return lam; } }
    int builtin_name = key->kind==EXPR_NAME && !lookup(c,key->name);
    int str_method = key->kind==EXPR_ATTRIBUTE && key->a->kind==EXPR_NAME && !strcmp(key->a->name,"str") && !lookup(c,"str");
    if(!builtin_name && !str_method) return key;
    Expr *param=xnew(EXPR_NAME,key->line); param->name="__key";
    Expr *call=xnew(EXPR_CALL,key->line);
    if(builtin_name){ call->a=key; xpush(call,param); }
    else { Expr *m=xnew(EXPR_ATTRIBUTE,key->line); m->a=param; m->name=key->name; call->a=m; }
    Expr *lam=xnew(EXPR_LAMBDA,key->line); lam->a=call;
    lam->eparams=PPY_NEW_ARR(char*,1); lam->eparams[0]="__key"; lam->neparam=1;
    return lam;
}
Ty *ck_sort_key(Ck *c, Expr *key, Ty *elem, int line){
    XInfo *ki=xinfo(key); Ty *k;
    if(key->kind==EXPR_LAMBDA){
        if(key->neparam!=1) ck_err(c,line,"a key function takes one argument");
        if(!ki->var) ki->var=scope_hidden(c,key->eparams[0]);
        expect(c,ki->var->ty,elem,line,"the key's parameter");
        int save=c->ncscope;
        cs_push(c,key->eparams[0],ki->var,line);
        k=ck_expr(c,key->a);
        c->ncscope=save;
        ki->kind=X_NONE; ki->ty=k;
    } else if(key->kind==EXPR_NAME && lookup(c,key->name) && lookup(c,key->name)->kind==AS_FUNC && !((AFunc*)lookup(c,key->name)->p)->outer){
        AFunc *fn=(AFunc*)lookup(c,key->name)->p; fn->ncalls++;
        if(fn->nparams!=1 || fn->is_async) ck_err(c,line,"a key function takes one argument");
        expect(c,fn->params[0]->ty,elem,line,"the key's parameter");
        ki->kind=X_FUNC; ki->fn=fn; k=fn->ret; ki->ty=k;
    } else {                                                /* a function value: key=self.score, key=f */
        if(key->kind!=EXPR_NAME && key->kind!=EXPR_ATTRIBUTE) ck_err(c,line,"key= needs a lambda or a function in compiled code");
        Ty *ft=ty_find(ck_expr(c,key));
        if(ft->k==TY_VAR){ Ty *ps[1]={ty_var()}; Ty *nf=ty_func(ps,1,ty_var()); unify(c,ft,nf); ft=nf; }
        if(ft->k!=TY_FUNC || ft->nelems!=1) ck_err(c,line,"key= needs a function of one argument, not %s",ty_name(ft));
        expect(c,ft->elems[0],elem,line,"the key's parameter");
        k=ft->elem;
    }
    Ty *kt=ty_find(k);
    if(kt->k==TY_VAR) return pending(c,line,"the key");
    if(!numeric(kt)&&kt->k!=TY_STR&&kt->k!=TY_TUPLE&&!(kt->k==TY_OBJ && aot_find_method(kt->cls,"__lt__")))
        ck_err(c,line,"sort keys must be numbers, strings, tuples or objects with __lt__, not %s",ty_name(kt));
    return k;
}
/* a generator expression given to a function that consumes all of it: a list comprehension */
void genexp_as_list(Expr *e, int i){ if(i<e->count && e->items[i]->kind==EXPR_COMPREHENSION && e->items[i]->comp_kind=='G') e->items[i]->comp_kind='g'; }
/* a generator value given to such a function: list(generator) */
Ty *gen_as_list(Ck *c, Expr *e, int i){
    if(i>=e->count) return NULL;
    Expr *a=e->items[i];
    Ty *t=ty_find(ck_expr(c,a));
    if((t->k!=TY_GEN && t->k!=TY_BYTES) || a->akind) return t;     /* a generator or bytes: list(...) of it */
    Expr *call=xnew(EXPR_CALL,a->line), *nm=xnew(EXPR_NAME,a->line); nm->name="\001list"; call->a=nm; xpush(call,a);
    e->items[i]=call;
    return ck_expr(c,call);
}
Expr *name_expr(const char *name, int line){ Expr *n=xnew(EXPR_NAME,line); n->name=(char*)name; return n; }
/* map(f, xs[, ys]) -> [f(x) for x in xs] / [f(x, y) for x, y in zip(xs, ys)];
   filter(f, xs) -> [x for x in xs if f(x)]: lambdas are inlined */
Ty *ck_mapfilter(Ck *c, Expr *e, const char *name){
    int isfilter=name[0]=='f', nit=e->count-1;
    for(int i=0;i<e->count;i++) if(e->items[i]->akind) ck_err(c,e->line,"%s() takes positional arguments",name);
    if(nit<1 || (isfilter && nit!=1)) ck_err(c,e->line,isfilter?"filter() takes a function and an iterable":"map() takes a function and iterables");
    if(nit>8) ck_err(c,e->line,"map() over more than 8 iterables is not supported in compiled code");
    Expr *fn=e->items[0], *body; char **vars=PPY_NEW_ARR(char*,8);
    static const char *hidden[8]={"__m0","__m1","__m2","__m3","__m4","__m5","__m6","__m7"};
    if(fn->kind==EXPR_LAMBDA){
        if(fn->neparam!=nit) ck_err(c,e->line,"the lambda of %s() takes %d argument%s",name,nit,nit==1?"":"s");
        for(int k=0;k<nit;k++) vars[k]=fn->eparams[k];
        body=fn->a;
    } else if(isfilter && fn->kind==EXPR_NONE){ vars[0]=(char*)hidden[0]; body=name_expr(hidden[0],e->line); }
    else {
        Expr *k=nit==1 ? key_as_lambda(c,fn) : fn;
        if(k->kind==EXPR_LAMBDA){ vars[0]=k->eparams[0]; body=k->a; }
        else {
            body=xnew(EXPR_CALL,e->line); body->a=fn;                 /* (any function value: called for each item) */
            for(int j=0;j<nit;j++){ vars[j]=(char*)hidden[j]; xpush(body,name_expr(hidden[j],e->line)); }
        }
    }
    Expr *it=e->items[1];
    if(nit>=2){ it=xnew(EXPR_CALL,e->line); it->a=name_expr("zip",e->line); for(int j=1;j<=nit;j++) xpush(it,e->items[j]); }
    e->kind=EXPR_COMPREHENSION; e->comp_kind='L';
    e->clauses=PPY_NEW0(CompClause); e->nclause=1; e->ccap=1;
    CompClause *cl=&e->clauses[0]; cl->vars=vars; cl->nvars=nit; cl->iter=it;
    if(nit>2){                                          /* map(f, a, b, c): for (x, y, z) in zip(a, b, c) */
        Expr *tu=xnew(EXPR_TUPLE,e->line); for(int j=0;j<nit;j++) xpush(tu,name_expr(vars[j],e->line));
        char **one=PPY_NEW_ARR(char*,1); one[0]="__mt"; cl->vars=one; cl->nvars=1; cl->target=tu; }
    if(isfilter){ cl->conds=PPY_NEW_ARR(Expr*,1); cl->conds[0]=body; cl->ncond=1; e->a=name_expr(vars[0],e->line); }
    else e->a=body;
    XInfo *xi=xinfo(e); xi->kind=X_NONE;
    return ck_comprehension(c,e);
}
/* e (a call) becomes the comprehension [elem for vars in iter] ('L') or {k: v ...} ('D') */
void make_comprehension(Expr *e, int kind, Expr *elem, Expr *val, char **vars, int nvars, Expr *iter){
    e->kind=EXPR_COMPREHENSION; e->comp_kind=kind; e->a=elem; e->b=val;
    e->clauses=PPY_NEW0(CompClause); e->nclause=1; e->ccap=1;
    CompClause *cl=&e->clauses[0]; cl->vars=vars; cl->nvars=nvars; cl->iter=iter;
    xinfo(e)->kind=X_NONE;
}
/* zip(a, b) / enumerate(xs[, start]) as a value: [(x, y) for x, y in zip(a, b)] */
Ty *ck_pairs(Ck *c, Expr *e, const char *name){
    for(int i=0;i<e->count;i++) if(e->items[i]->akind) ck_err(c,e->line,"%s() takes positional arguments",name);
    if(name[0]=='z' && (e->count<1||e->count>8)) ck_err(c,e->line,"zip() of 1 to 8 iterables in compiled code");
    if(name[0]=='e' && (e->count<1||e->count>2)) ck_err(c,e->line,"enumerate() takes 1 or 2 arguments");
    Expr *it=xnew(EXPR_CALL,e->line); it->a=name_expr(name,e->line);
    for(int i=0;i<e->count;i++) xpush(it,e->items[i]);
    char **vars=PPY_NEW_ARR(char*,1); vars[0]="__p0";            /* [t for t in zip(...)]: the tuples */
    make_comprehension(e,'L',name_expr("__p0",e->line),NULL,vars,1,it);
    return ck_comprehension(c,e);
}
/* iter(xs) -> (x for x in xs) */
Ty *ck_iter(Ck *c, Expr *e){
    ck_positional(c,e,1,1,"iter");
    Expr *it=e->items[0];
    e->kind=EXPR_COMPREHENSION; e->comp_kind='G';
    e->clauses=PPY_NEW0(CompClause); e->nclause=1; e->ccap=1;
    CompClause *cl=&e->clauses[0]; cl->vars=PPY_NEW_ARR(char*,1); cl->vars[0]="__it"; cl->nvars=1; cl->iter=it;
    e->a=name_expr("__it",e->line);
    xinfo(e)->kind=X_NONE;
    return ck_genexp(c,e);
}
/* functools.reduce(f, xs[, init]): f's parameters become hidden variables (acc, item) */
Ty *ck_reduce(Ck *c, Expr *e){
    XInfo *xi=xinfo(e); xi->kind=X_BUILTIN; xi->name="reduce"; int line=e->line;
    ck_positional(c,e,2,3,"reduce");
    genexp_as_list(e,1);
    Ty *lt=ty_find(gen_as_list(c,e,1));
    if(lt->k==TY_VAR) return pending(c,line,"the iterable of reduce()");
    if(lt->k!=TY_LIST) ck_err(c,line,"reduce() needs a list, not %s",ty_name(lt));
    Ty *el=lt->elem, *acc=el;
    if(e->count==3){ acc=ty_find(ck_expr(c,e->items[2])); no_void(c,acc,line); if(acc->k==TY_INT && ty_find(el)->k==TY_FLOAT) acc=TY_FLOAT_T;
        if(ty_find(el)->k==TY_OBJ && numeric(acc) && assignable(c,el,acc)) acc=el; }      /* sum of complex numbers: 0 is 0j */
    Expr *fn=e->items[0];
    if(!xi->cvars){
        xi->cvars=PPY_NEW_ARR(AVar*,2);
        const char *n0="__r0", *n1="__r1";
        if(fn->kind==EXPR_LAMBDA){ if(fn->neparam!=2) ck_err(c,line,"the function of reduce() takes two arguments"); n0=fn->eparams[0]; n1=fn->eparams[1]; xi->key=fn->a; }
        else { if(fn->kind!=EXPR_NAME && fn->kind!=EXPR_ATTRIBUTE) ck_err(c,line,"reduce() needs a lambda or a function name in compiled code");
            Expr *call=xnew(EXPR_CALL,line); call->a=fn; xpush(call,name_expr(n0,line)); xpush(call,name_expr(n1,line)); xi->key=call; }
        xi->cvars[0]=scope_hidden(c,n0); xi->cvars[1]=scope_hidden(c,n1);
        xi->cvars[0]->name=xstrdup2(n0); xi->cvars[1]->name=xstrdup2(n1);
    }
    expect(c,xi->cvars[0]->ty,acc,line,"the accumulator of reduce()");
    expect(c,xi->cvars[1]->ty,el,line,"the item of reduce()");
    int save=c->ncscope;
    cs_push(c,xi->cvars[0]->name,xi->cvars[0],line); cs_push(c,xi->cvars[1]->name,xi->cvars[1],line);
    Ty *r=ck_expr(c,xi->key); no_void(c,r,line);
    c->ncscope=save;
    expect(c,xi->cvars[0]->ty,r,line,"the result of the reduce() function");
    return xi->cvars[0]->ty;
}
/* open(path, mode, ...): io's text file, or its binary one for a literal mode with 'b' (io.py) */
Ty *ck_open(Ck *c, Expr *e){
    Expr *md=NULL;
    for(int i=0;i<e->count;i++){ Expr *a=e->items[i]; if((!a->akind && i==1) || (a->akind==3 && !strcmp(a->kw,"mode"))) md=a; }
    int binary= md && md->kind==EXPR_LITERAL && md->tok->kind==T_STRING && md->tok->i!=2 && strchr(md->tok->text,'b');
    e->a=name_expr(binary?"__ppy_open_binary":"__ppy_open_text",e->line);
    xinfo(e)->kind=X_NONE;
    return ck_call_inner(c,e);
}
/* f(obj[, more]) for an object with method m: obj.m(more) (int(x) -> x.__int__() ...); NULL if not */
