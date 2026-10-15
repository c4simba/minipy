/* ========================= Typed compiler, types: statements =========================
   The statements of a function body (ck_stmt), None tests narrowing
   variables, reachability. */

#include "check.h"

static void nn_push(Ck *c, AVar *v);
static int none_test(Ck *c, Expr *e, AVar **v);
static void ck_stmts(Ck *c, Stmt **b, int n);

void ck_stmt(Ck *c, Stmt *s){
    AotUnit *u=c->mod->unit;
    if(s->kind==STMT_MATCH && !desugar_match(c,s)) return;     /* (its subject's type not known yet) */
    if(s->kind==STMT_WITH && !desugar_with(c,s)) return;
    if(s->kind==STMT_TRY && s->star) desugar_trystar(c,s);
    if(s->kind==STMT_EXPR && !s->ann_only){                  /* object.__setattr__(obj, "name", value): obj.name = value (frozen or not) */
        Expr *e=aot_expr(u,&s->expr);
        if(e->kind==EXPR_CALL && e->a->kind==EXPR_ATTRIBUTE && !strcmp(e->a->name,"__setattr__") && e->a->a->kind==EXPR_NAME && !strcmp(e->a->a->name,"object")
           && !lookup(c,"object") && e->count==3 && e->items[1]->kind==EXPR_LITERAL && e->items[1]->tok->kind==T_STRING && e->items[1]->tok->i!=2){
            Expr *t=xnew(EXPR_ATTRIBUTE,e->line); t->a=e->items[0]; t->name=e->items[1]->tok->text; t->akind=7;    /* (7: not a frozen dataclass's check) */
            s->kind=STMT_ASSIGN; s->targets=MPY_NEW_ARR(Expr*,1); s->targets[0]=t; s->ntargets=1; s->value=e->items[2]; s->expr=NULL; s->aux=NULL; } }
    switch(s->kind){
        case STMT_EXPR:{
            APrint *pr=aot_print(u,s);
            if(pr && pr->file && !is_none(pr->file)){                        /* print(..., file=f) */
                Expr *fl=pr->file;
                if(fl->kind==EXPR_ATTRIBUTE && is_sys(c,fl->a) && (!strcmp(fl->name,"stderr")||!strcmp(fl->name,"stdout"))) pr->fd=fl->name[3]=='e'?2:1;
                else {                                                     /* an object: f.write(sep.join([str(a), ...]) + end) */
                    int line=s->line;
                    Expr *lst=xnew(EXPR_LIST,line);
                    for(int i=0;i<pr->n;i++){ if(pr->star[i]) ck_err(c,line,"print(*args, file=...) is not supported in compiled code");
                        Expr *sc=xnew(EXPR_CALL,line); sc->a=name_expr("\001str",line); xpush(sc,pr->args[i]); xpush(lst,sc); }
                    Expr *sep=pr->sep && !is_none(pr->sep) ? pr->sep : py_front_expr(u->path,"' '",line);
                    Expr *end=pr->end && !is_none(pr->end) ? pr->end : py_front_expr(u->path,"'\\n'",line);
                    Expr *join=xnew(EXPR_ATTRIBUTE,line); join->a=sep; join->name="join";
                    Expr *jc=xnew(EXPR_CALL,line); jc->a=join; xpush(jc,lst);
                    Expr *text=xnew(EXPR_BINARY,line); text->op=T_PLUS; text->a=jc; text->b=end;
                    Expr *w=xnew(EXPR_ATTRIBUTE,line); w->a=fl; w->name="write";
                    Expr *call=xnew(EXPR_CALL,line); call->a=w; xpush(call,text);
                    s->expr=call; s->aux=NULL;
                    ck_expr(c,call); return; } }
            if(pr){ for(int i=0;i<pr->n;i++){ Ty *t=ck_expr(c,pr->args[i]); no_void(c,t,s->line); if(pr->star[i]) elem_of(c,t,s->line,"the *argument"); }
                if(pr->flush) no_void(c,ck_expr(c,pr->flush),s->line);
                if(pr->sep) expect(c,TY_STR_T,ck_expr(c,pr->sep),s->line,"sep");
                if(pr->end) expect(c,TY_STR_T,ck_expr(c,pr->end),s->line,"end");
                return; }
            if(aot_is_annotation_only(u,s)){ ck_assign(c,s); return; }
            { Expr *x=aot_expr(u,&s->expr);                                  /* sys.exit(code): raise SystemExit(code) */
              if(x->kind==EXPR_CALL && x->a->kind==EXPR_ATTRIBUTE && !strcmp(x->a->name,"exit") && is_sys(c,x->a->a) && lookup(c,"__mpy_sysexit_int") && x->count<=1 && lookup(c,"SystemExit") && lookup(c,"SystemExit")->kind==AS_CLASS && ((AClass*)lookup(c,"SystemExit")->p)->builtin){
                  x->a=name_expr("SystemExit",x->line); s->kind=STMT_RAISE; s->expr=x; s->expr2=NULL; s->aux=NULL; ck_stmt(c,s); return; } }
            { Expr *x=aot_expr(u,&s->expr); Expr *save=c->discard; c->discard=x; ck_expr(c,x); c->discard=save; }   /* (its value unused) */
            return; }
        case STMT_ASSIGN: ck_assign(c,s); return;
        case STMT_IF: case STMT_WHILE:
            if(s->kind==STMT_IF){ int st=static_isinstance(c,s->expr);       /* isinstance(x, dict) with x a list: the other branch only */
                if(st==-2 && !c->strict){ pending(c,s->line,"the object isinstance() tests"); return; }   /* (its branches may need what it is) */
                if(st<0){ st=static_none_param(c,s->expr);
                    if(st==-2){ pending(c,s->line,"the parameter tested"); return; } }   /* (its branches may need what the calls give it) */
                if(st>=0){ s->expr=xnew(st?EXPR_TRUE:EXPR_FALSE,s->line);
                    if(st) s->orelse_count=0; else { s->body=MPY_NEW_ARR(Stmt*,1); s->body[0]=stmt_new(STMT_PASS,NULL,s->line); s->body_count=s->body_cap=1; } } }
            no_void(c,ck_cond(c,aot_expr(u,&s->expr)),s->line);
            if(s->kind==STMT_IF && s->body_count){ const char *nm; AClass *k; int r=isinstance_test(c,aot_expr(u,&s->expr),&nm,&k,1);
                if(r<0) return;                                  /* (the variable's type is not known yet) */
                if(r && !stores_name(c,s->body,s->body_count,nm)){   /* if isinstance(x, C): x is a C in the body */
                    if(!s->name2 || strncmp(s->name2,".narrow",7)){ char h[64]; named_hidden(c,"narrow",h,sizeof h); s->name2=xstrdup2(h);
                        Stmt *as=st_assign(nx(s->name2,s->line),ex_call("\001downcast",nx(k->symname,s->line),nx(nm,s->line),s->line),s->line);
                        Stmt **nb=MPY_NEW_ARR(Stmt*,s->body_count+1); nb[0]=as; for(int i=0;i<s->body_count;i++) nb[i+1]=s->body[i];
                        s->body=nb; s->body_count++; s->body_cap=s->body_count; }
                    ck_stmt(c,s->body[0]);
                    int save=c->ncscope; cs_push(c,nm,(AVar*)lookup(c,s->name2)->p,s->line);
                    ck_stmts(c,s->body+1,s->body_count-1); c->ncscope=save;
                    ck_stmts(c,s->orelse,s->orelse_count);
                    return; } }
            { AVar *nv=NULL; int nt= s->kind==STMT_IF ? none_test(c,s->expr,&nv) : 0;   /* x is not None: x in the body; x is None: x in the else */
              int save_nn=c->nnn;
              if(nt==2 && !stores_name(c,s->body,s->body_count,nv->name)) nn_push(c,nv);
              ck_stmts(c,s->body,s->body_count); c->nnn=save_nn;
              if(nt==1 && !stores_name(c,s->orelse,s->orelse_count,nv->name)) nn_push(c,nv);
              ck_stmts(c,s->orelse,s->orelse_count); c->nnn=save_nn; }
            return;
        case STMT_FOR:{
            if(s->param_count<1||s->param_count>16) ck_err(c,s->line,"for loops take 1 to 16 variables in compiled code");
            if(s->is_async){ Ty *it=ty_find(ck_expr(c,aot_expr(u,&s->expr)));
                if(it->k==TY_VAR){ pending(c,s->line,"the async iterable"); return; }
                if(it->k==TY_OBJ){ desugar_async_for(c,s); ck_stmt(c,s); return; } }
            Ty *ts[16]; iter_types(c,aot_expr(u,&s->expr),s->param_count,ts,s->line);
            async_iter_ok(c,aot_expr(u,&s->expr),s->is_async,s->line);
            for(int i=0;i<s->param_count;i++){ Expr nm; memset(&nm,0,sizeof nm); nm.kind=EXPR_NAME; nm.name=s->params[i]; ck_store(c,&nm,ts[i],s->line); }
            ck_stmts(c,s->body,s->body_count); ck_stmts(c,s->orelse,s->orelse_count);
            return; }
        case STMT_RETURN:
            if(!is_func(c->fn)) ck_err(c,s->line,"return outside a function");
            if(c->fn->is_gen){ if(!s->expr) return;
                Ty *gr=aot_gen_part(c->fn->ret,1), *t=ck_expr_want(c,aot_expr(u,&s->expr),gr); no_void(c,t,s->line);
                expect(c,gr,t,s->line,"the generator's return value"); return; }
            if(s->expr){ Ty *t=ck_expr_want(c,aot_expr(u,&s->expr),c->fn->ret); no_void(c,t,s->line);
                char what[160]; snprintf(what,sizeof what,"the return value of %s()",c->fn->name);
                expect(c,c->fn->ret,t,s->line,what); }
            else if(c->fn->returns_value){ Ty *v=ty_var(); v->opt=1; unify(c,c->fn->ret,v); }   /* `return`: None */
            return;
        case STMT_DEL:{
            ADel *d=aot_del(u,s);
            for(int i=0;i<d->n;i++){ Expr *t=d->t[i];
                if(t->kind==EXPR_NAME){                         /* del x: x holds no value until assigned again */
                    ASym *x=lookup(c,t->name);
                    if(!x || x->kind!=AS_VAR) ck_err(c,s->line,"cannot delete '%s'",t->name);
                    AVar *v=(AVar*)x->p, *r=var_root(v);
                    if(v->src || r->captured || r->cell) ck_err(c,s->line,"del of '%s', which a nested function uses, is not supported in compiled code",t->name);
                    if(!r->bound){ char h[300]; snprintf(h,sizeof h," %s.bound",r->name);
                        r->bound= r->global ? new_global(c,r->mod,h) : hidden_local(r->owner,h,TY_BOOL_T);
                        r->bound->ty=ty_copy(TY_BOOL_T); }
                    xinfo(t)->kind=X_VAR; xinfo(t)->var=r; continue; }
                if(t->kind==EXPR_SLICE){                        /* del xs[a:b:c] */
                    Ty *o=ty_find(ck_expr(c,t->a)); Expr *bounds[3]={t->b,t->c,t->d};
                    for(int k=0;k<3;k++) if(bounds[k] && bounds[k]->kind!=EXPR_NONE) expect(c,TY_INT_T,ck_expr(c,bounds[k]),s->line,"a slice bound");
                    if(o->k==TY_VAR){ pending(c,s->line,"the list"); continue; }
                    if(o->k==TY_OBJ && aot_find_method(o->cls,"__mpy_delslice__") && d->n==1){   /* del obj[a:b]: obj.__mpy_delslice__(a, b, None) */
                        s->kind=STMT_EXPR; s->expr=slice_call(t,"__mpy_delslice__",s->line); s->aux=NULL; s->ntargets=0; s->targets=NULL;
                        ck_stmt(c,s); return; }
                    if(o->k!=TY_LIST || o->tup) ck_err(c,s->line,"'%s' object does not support item deletion",ty_name(o));
                    xinfo(t)->ty=o; continue; }
                if(t->kind==EXPR_ATTRIBUTE) ck_err(c,s->line,"del obj.attr is not supported in compiled code: an object's fields are fixed");
                if(t->kind!=EXPR_INDEX) ck_err(c,s->line,"cannot delete this in compiled code");
                Ty *o=ty_find(ck_expr(c,t->a)), *k=ck_expr(c,t->b);
                if(o->k==TY_VAR){ pending(c,s->line,"the container"); continue; }
                if(o->k==TY_LIST) expect(c,TY_INT_T,k,s->line,"a list index");
                else if(o->k==TY_DICT) expect(c,ty_dkey(o),k,s->line,"a dictionary key");
                else if(o->k==TY_OBJ && aot_find_method(o->cls,"__delitem__") && d->n==1){      /* del obj[key]: obj.__delitem__(key) */
                    Expr *at=xnew(EXPR_ATTRIBUTE,s->line); at->a=t->a; at->name="__delitem__";
                    Expr *call=xnew(EXPR_CALL,s->line); call->a=at; xpush(call,t->b);
                    s->kind=STMT_EXPR; s->expr=call; s->aux=NULL; s->ntargets=0; s->targets=NULL;
                    ck_stmt(c,s); return; }
                else ck_err(c,s->line,"cannot delete items of %s",ty_name(o));
                xinfo(t)->ty=o; }
            return; }
        case STMT_ASSERT:
            no_void(c,ck_cond(c,aot_expr(u,&s->expr)),s->line);
            if(s->expr2) no_void(c,ck_expr(c,aot_expr(u,&s->expr2)),s->line);
            return;
        case STMT_RAISE:{
            if(!s->expr){ if(!c->in_except) ck_err(c,s->line,"bare raise outside an except clause"); return; }
            Expr *e=aot_expr(u,&s->expr);
            if(e->kind==EXPR_NAME){                         /* raise ValueError */
                ASym *x=lookup(c,e->name);
                if(x && x->kind==AS_CLASS){ AClass *cls=(AClass*)x->p;
                    if(!aot_is_exception(cls)) ck_err(c,s->line,"%s is not an exception class",cls->name);
                    xinfo(e)->kind=X_CTOR; xinfo(e)->cls=cls; xinfo(e)->ty=ty_new(TY_OBJ,NULL,cls); ck_raise_cause(c,s); return; }
            }
            Ty *t=ty_find(ck_expr(c,e)); no_void(c,t,s->line);
            if(t->k==TY_VAR && !c->strict){ ck_raise_cause(c,s); return; }       /* (a call still being decided) */
            if(t->k!=TY_OBJ || !aot_is_exception(t->cls)) ck_err(c,s->line,"raise needs an exception (a subclass of Exception), not %s",ty_name(t));
            ck_raise_cause(c,s);
            return; }
        case STMT_WITH:{
            AWith *w=aot_with(u,s);
            for(int i=0;i<w->n;i++){ Ty *t=ck_expr(c,w->e[i]); if(w->as[i]){ Expr nm; memset(&nm,0,sizeof nm); nm.kind=EXPR_NAME; nm.name=w->as[i]; ck_store(c,&nm,t,s->line); } }
            ck_stmts(c,s->body,s->body_count);
            return; }
        case STMT_PASS: case STMT_BREAK: case STMT_CONTINUE: case STMT_GLOBAL: return;
        case STMT_FUNCTION_DEF:
            if(s->aux) ck_def_defaults(c,(AFunc*)s->aux,s->line);
            else if(!is_func(c->fn)){ ASym *x=module_sym(c->mod,s->name); if(!x || x->kind!=AS_FUNC){ char *h=hidden_name(s->name,NULL); x=module_sym(c->mod,h); free(h); }
                if(x && x->kind==AS_FUNC){ AFunc *mf=(AFunc*)x->p; ck_def_defaults(c,mf,s->line);
                    for(int q=0;q<mf->ninsts;q++) ck_def_defaults(c,mf->insts[q],s->line); } }   /* (a generic function's instances: their own copies) */
            if(!is_func(c->fn)){                             /* module level: decorators applied here */
                char *h=hidden_name(s->name,NULL); ASym *x=module_sym(c->mod,h); free(h);
                if(x){ Ty *t=ck_expr(c,deco_app(c,u,s,x->name,0)); no_void(c,t,s->line);
                    Expr nm; memset(&nm,0,sizeof nm); nm.kind=EXPR_NAME; nm.name=s->name; ck_store(c,&nm,t,s->line); }
            }
            if(is_func(c->fn)){                              /* a nested def: binds its closure, checked in place */
                AFunc *inner=(AFunc*)s->aux;
                ASym *x=symtab_find(&c->fn->locals,s->name);
                if(x && x->kind==AS_VAR){ Expr nm; memset(&nm,0,sizeof nm); nm.kind=EXPR_NAME; nm.name=s->name;
                    Ty *ft;
                    if(inner->ndeco){ char *h=hidden_name(s->name,NULL); ASym *hx=symtab_find(&c->fn->locals,h); free(h);
                        ft=ck_expr(c,deco_app(c,u,s,hx->name,0)); no_void(c,ft,s->line); }
                    else ft=fn_type_problem(inner) ? (inner->fty?inner->fty:(inner->fty=ty_func(NULL,0,inner->ret))) : fn_type(c,inner,s->line);   /* only called directly */
                    ck_store(c,&nm,ft,s->line); }
                else if(!x) ck_err(c,s->line,"internal error: nested def without a variable");
                if(c->strict && !inner->ncalls && !inner->value_used){
                    int unknown=0; for(int k=0;k<inner->nparams;k++) if(!ty_known(inner->params[k]->ty)) unknown=1;
                    if(unknown){ inner->unused=1; return; } }
                AFunc *save=c->fn; c->fn=inner;
                ck_body(c,inner);
                c->fn=save;
            }
            return;
        case STMT_CLASS_DEF:
            {   /* decorated methods: Class.name = decorators(method), when the class statement runs */
                AClass *cls0=(AClass*)s->aux;
                for(int q=-1;cls0 && q<cls0->ninsts;q++){ AClass *cls=q<0?cls0:cls0->insts[q];   /* (and the instances of a generic class) */
                if(cls->encl) ck_class_attrs(c,cls);                   /* a class of a function: its attributes in the function's scope */
                for(int i=0;i<cls->nmethods;i++) ck_def_defaults(c,cls->methods[i],cls->methods[i]->line);   /* evaluated with the class */
                for(int i=0;i<cls->nmethods;i++){ AFunc *m=cls->methods[i]; if(!m->decovar) continue;
                    char *h=hidden_name(cls->symname,m->name); ASym *hx=module_sym(c->mod,h); free(h);
                    Ty *t=ck_expr(c,deco_app(c,u,m->def,hx->name,1)); no_void(c,t,m->line);
                    char what[200]; snprintf(what,sizeof what,"%s.%s after its decorators",cls->name,m->name);
                    expect(c,m->decovar->ty,t,m->line,what); }
                for(int i=0;i<cls->def->body_count;i++) if(cls->def->body[i]->kind==STMT_CLASS_DEF) ck_stmt(c,cls->def->body[i]);   /* its nested classes */
                }
            }
            return;
        case STMT_IMPORT: case STMT_FROM_IMPORT: return;          /* (its names: see link_imports) */
        case STMT_YIELD:{
            AFunc *g=c->fn;
            if(!g->is_gen) ck_err(c,s->line,"'yield' outside a function");
            if(!s->expr) return;                            /* yields None: the zero value */
            Expr *e=aot_expr(u,&s->expr);
            if(s->block_tag==7){
                Ty *ts[1]; iter_types(c,e,1,ts,s->line);
                expect(c,g->yield_ty,ts[0],s->line,"the yielded values");
                Ty *it=xinfo(e)->ty?ty_find(xinfo(e)->ty):TY_VOID_T; if(it->k==TY_GEN) unify(c,aot_gen_part(it,0),aot_gen_part(g->ret,0));   /* send() reaches the inner generator */
                return;
            }
            Ty *t=ck_expr_want(c,e,g->yield_ty); no_void(c,t,s->line);
            expect(c,g->yield_ty,t,s->line,"the yielded value");
            return; }
        case STMT_NONLOCAL:
            if(!is_func(c->fn) || !c->fn->outer) ck_err(c,s->line,"nonlocal outside a nested function");
            for(int k=0;k<s->param_count;k++){ ASym *x=lookup(c,s->params[k]);
                if(!x || x->kind!=AS_VAR || !((AVar*)x->p)->src) ck_err(c,s->line,"no binding for nonlocal '%s' in an enclosing function",s->params[k]); }
            return;
        case STMT_TRY:{
            ck_stmts(c,s->body,s->body_count);
            for(int i=0;i<s->orelse_count;i++){ Stmt *b=s->orelse[i];
                if(b->block_tag!=1){ ck_stmts(c,b->body,b->body_count); continue; }
                AClass *common=exc_root;
                if(b->expr){                                /* except A / except (A, B) [as e] */
                    Expr *te=aot_expr(u,&b->expr);
                    int n=te->kind==EXPR_TUPLE?te->count:1;
                    common=NULL;
                    for(int k=0;k<n;k++){ Expr *x=te->kind==EXPR_TUPLE?te->items[k]:te;
                        AClass *cls=class_expr(c,x);                /* (a module's, a nested one) */
                        if(!cls||!aot_is_exception(cls)) ck_err(c,b->line,"except needs exception classes");
                        xinfo(x)->kind=X_CTOR; xinfo(x)->cls=cls;
                        if(!common) common=cls; else while(common && !aot_subclass(cls,common)) common=common->base; }
                    if(!common) common=exc_root;
                }
                int save=c->ncscope;
                if(b->name){                                /* the `as` variable belongs to this clause */
                    AVar *v=(AVar*)b->aux;
                    if(!v){ v=scope_hidden(c,b->name); b->aux=v; }
                    expect(c,v->ty,ty_new(TY_OBJ,NULL,common),b->line,"the exception variable");
                    cs_push(c,b->name,v,b->line);
                }
                c->in_except++;
                if(c->nhst<64){ c->hst[c->nhst].b=b; c->hst[c->nhst].fn=c->fn; c->hst[c->nhst].ty=ty_new(TY_OBJ,NULL,common); }
                c->nhst++;
                ck_stmts(c,b->body,b->body_count);
                c->nhst--;
                c->in_except--;
                c->ncscope=save;
            }
            return; }
        default: ck_err(c,s->line,"'%s' is not supported in compiled code",s->name?s->name:stmt_kind_name(s->kind));
    }
}
/* if not isinstance(x, C): return / continue / break / raise -> x is a C after it (the if's else
   keeps x as a C in a hidden variable) */
static int exits(Stmt **b, int n){ if(!n) return 0; Stmt *l=b[n-1]; return l->kind==STMT_RETURN||l->kind==STMT_RAISE||l->kind==STMT_CONTINUE||l->kind==STMT_BREAK; }
/* `x is None` (1) / `x is not None` (2) with x a variable -> it in *v, else 0 */
static int none_test(Ck *c, Expr *e, AVar **v){
    if(!e || e->kind!=EXPR_COMPARE || e->count!=2) return 0;
    int code=e->items[1]->akind; Expr *a=e->items[0], *b=e->items[1];
    if((code!=CMP_IS && code!=CMP_ISNOT) || a->kind!=EXPR_NAME || b->kind!=EXPR_NONE) return 0;
    ASym *s=lookup(c,a->name); if(!s || s->kind!=AS_VAR) return 0;
    *v=var_root((AVar*)s->p); return code==CMP_IS ? 1 : 2;
}
static void nn_push(Ck *c, AVar *v){ if(c->nnn<64) c->nnv[c->nnn++]=v; }
int nn_has(Ck *c, AVar *v){ v=var_root(v); for(int i=c->nnn-1;i>=0;i--) if(c->nnv[i]==v) return 1; return 0; }
void nn_drop(Ck *c, AVar *v){ v=var_root(v); for(int i=0;i<c->nnn;i++) if(c->nnv[i]==v) c->nnv[i]=NULL; }
static void ck_stmts_top(Ck *c, Stmt **b, int n, AFunc *top){
    int save=c->ncscope, save_nn=c->nnn; AotUnit *u=c->mod->unit;
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(top) top->top_index=i;
        ck_stmt(c,s);
        if(s->kind==STMT_IF && i+1<n){ Expr *ce=aot_expr(u,&s->expr);       /* if <folded True>: return ...: the rest is never run (nor checked) */
            int dead= ce->kind==EXPR_TRUE ? s->body_count && !falls_through(s->body,s->body_count)
                    : ce->kind==EXPR_FALSE ? s->orelse_count && !falls_through(s->orelse,s->orelse_count) : 0;
            if(dead){ for(int j=i+1;j<n;j++) if(b[j]->kind!=STMT_PASS) b[j]=stmt_new(STMT_PASS,NULL,b[j]->line);
                continue; } }
        { AVar *nv; if(s->kind==STMT_IF && !s->orelse_count && none_test(c,s->expr,&nv)==1 && exits(s->body,s->body_count)) nn_push(c,nv); }   /* if x is None: return -> x after it */
        const char *nm; AClass *k; Expr *e;
        if(s->kind!=STMT_IF || !(e=aot_expr(u,&s->expr)) || e->kind!=EXPR_UNARY || e->op!=T_NOT || !exits(s->body,s->body_count)) continue;
        if(s->orelse_count && !(s->name2 && !strncmp(s->name2,".guard",6))) continue;
        if(isinstance_test(c,e->a,&nm,&k,0)<=0 || stores_name(c,b+i+1,n-i-1,nm)) continue;
        if(!s->name2){ char h[64]; named_hidden(c,"guard",h,sizeof h); s->name2=xstrdup2(h);
            stmt_add_orelse(s,st_assign(nx(s->name2,s->line),ex_call("\001downcast",nx(k->symname,s->line),nx(nm,s->line),s->line),s->line));
            ck_stmt(c,s->orelse[0]); }
        cs_push(c,nm,(AVar*)lookup(c,s->name2)->p,s->line);
    }
    c->ncscope=save; c->nnn=save_nn;
}
static void ck_stmts(Ck *c, Stmt **b, int n){ ck_stmts_top(c,b,n,NULL); }
/* may control run past the end of these statements? (no: a return / raise every way) */
static int is_true_const(Expr *e){ return e && (e->kind==EXPR_TRUE || (e->kind==EXPR_LITERAL && e->tok->kind==T_NUMBER && !e->tok->is_float && e->tok->i)); }
static int has_break(Stmt **b, int n){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_BREAK) return 1;
        if(s->kind==STMT_FOR||s->kind==STMT_WHILE||s->kind==STMT_FUNCTION_DEF||s->kind==STMT_CLASS_DEF) continue;
        if(has_break(s->body,s->body_count)||has_break(s->orelse,s->orelse_count)) return 1; }
    return 0;
}
int falls_through(Stmt **b, int n){
    if(!n) return 1;
    Stmt *s=b[n-1];
    switch(s->kind){
        case STMT_RETURN: case STMT_RAISE: return 0;
        case STMT_IF:
            if(s->expr && s->expr->kind==EXPR_TRUE) return falls_through(s->body,s->body_count);       /* (a test decided when compiling) */
            if(s->expr && s->expr->kind==EXPR_FALSE) return falls_through(s->orelse,s->orelse_count);
            return falls_through(s->body,s->body_count) || !s->orelse_count || falls_through(s->orelse,s->orelse_count);
        case STMT_WHILE: return !(is_true_const(s->expr) && !has_break(s->body,s->body_count));
        case STMT_WITH: return falls_through(s->body,s->body_count);
        case STMT_TRY:{
            for(int i=0;i<s->orelse_count;i++){ Stmt *x=s->orelse[i]; if(x->block_tag==3 && !falls_through(x->body,x->body_count)) return 0; }   /* finally */
            Stmt *els=NULL; for(int i=0;i<s->orelse_count;i++) if(s->orelse[i]->block_tag==2) els=s->orelse[i];
            if(els ? falls_through(els->body,els->body_count) : falls_through(s->body,s->body_count)) return 1;
            for(int i=0;i<s->orelse_count;i++){ Stmt *x=s->orelse[i]; if(x->block_tag==1 && falls_through(x->body,x->body_count)) return 1; }
            return 0; }
        default: return 1;
    }
}
void ck_body(Ck *c, AFunc *f){
    ck_stmts_top(c,f->body,f->nbody,f);
    if(f->def && f->returns_value && !f->is_gen && !f->is_abstract && falls_through(f->body,f->nbody)){ Ty *v=ty_var(); v->opt=1; unify(c,f->ret,v); }   /* off the end: None */
}

