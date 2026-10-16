/* ========================= Typed compiler, types: statements rewritten =========================
   with, async for, augmented assignment items and except* rewritten
   into simpler statements. */

#include "check.h"

static void hidden_reg(Ck *c, const char *name){                 /* a hidden variable rewritten code names */
    if(is_func(c->fn)){ if(!symtab_find(&c->fn->locals,name)){ AVar *v=hidden_local(c->fn,name,NULL); symtab_add(&c->fn->locals,name,AS_VAR,v); } }
    else if(!module_sym(c->mod,name)){ AVar *v=new_global(c,c->mod,name); symtab_add(&c->mod->syms,name,AS_VAR,v); }
}
/* with / async with on an object: CPython's expansion (__enter__; __exit__ with the exception,
   which it may swallow, or with None) -> 0 when the types are not known yet */
int desugar_with(Ck *c, Stmt *s){
    AotUnit *u=c->mod->unit; AWith *w=aot_with(u,s); int line=s->line, any=0;
    for(int i=0;i<w->n;i++){ Ty *t=ty_find(ck_expr(c,w->e[i])); if(t->k==TY_VAR){ pending(c,line,"the context manager"); return 0; } if(t->k==TY_OBJ) any=1; }
    if(!any && !s->is_async){
        for(int i=0;i<w->n;i++) if(s->withtgt && s->withtgt[i]) ck_err(c,line,"`with ... as` takes a name here in compiled code");
        return 1; }
    if(w->n>1){                                                  /* with a, b: with a: with b: */
        Stmt *in=stmt_new(STMT_WITH,NULL,line); in->is_async=s->is_async;
        in->ntargets=s->ntargets-1; in->targets=s->targets+1; in->withas=s->withas+1; in->withtgt=s->withtgt?s->withtgt+1:NULL;
        in->body=s->body; in->body_count=s->body_count; in->body_cap=s->body_cap;
        s->ntargets=1; s->body=NULL; s->body_count=s->body_cap=0; stmt_add_body(s,in);
        s->aux=NULL; w=aot_with(u,s); }
    Ty *t=ty_find(ck_expr(c,w->e[0]));
    const char *en=s->is_async?"__aenter__":"__enter__", *exn=s->is_async?"__aexit__":"__exit__", *aw=s->is_async?"await ":"";
    if(t->k!=TY_OBJ) ck_err(c,line,"'%s' object does not support the %scontext manager protocol",ty_name(t),s->is_async?"asynchronous ":"");
    AFunc *fen=aot_find_method(t->cls,en), *fex=aot_find_method(t->cls,exn);
    if(!fen || !fex) ck_err(c,line,"'%s' object does not support the %scontext manager protocol (missed %s method)",t->cls->name,s->is_async?"asynchronous ":"",!fen?en:exn);
    int k=++c->nhidden; char m[48], v[48], h[48], e[48];
    snprintf(m,sizeof m,"__ppy_with%d_mgr",k); snprintf(v,sizeof v,"__ppy_with%d_value",k); snprintf(h,sizeof h,"__ppy_with%d_hit",k); snprintf(e,sizeof e,"__ppy_with%d_exc",k);
    hidden_reg(c,m); hidden_reg(c,h);
    Expr *tg= s->withtgt && s->withtgt[0] ? s->withtgt[0] : w->as[0] ? name_expr(w->as[0],line) : NULL;
    int env= ty_find(fen->ret)->k!=TY_VOID, exv= ty_find(fex->ret)->k!=TY_VOID;
    if(env) hidden_reg(c,v);
    Txt x={0};
    for(int i=1;i<line;i++) tx(&x,"\n");
    tx(&x,"%s = 0\n",m);
    if(env) tx(&x,"%s = %s%s.%s()\n",v,aw,m,en); else tx(&x,"%s%s.%s()\n",aw,m,en);
    tx(&x,"%s = False\ntry:\n",h);
    if(tg && env) tx(&x,"    __ppy_target = %s\n",v);
    tx(&x,"    pass\nexcept BaseException as %s:\n    %s = True\n",e,h);
    if(exv) tx(&x,"    if not %s%s.%s(type(%s), %s, None):\n        raise\n",aw,m,exn,e,e);
    else tx(&x,"    %s%s.%s(type(%s), %s, None)\n    raise\n",aw,m,exn,e,e);
    tx(&x,"finally:\n    if not %s:\n        %s%s.%s(None, None, None)\n",h,aw,m,exn);
    Stmt *blk=py_front_stmts(u->path,x.s,line); free(x.s);
    if(!blk) ck_err(c,line,"internal error: with");
    blk->body[0]->value=w->e[0];
    Stmt *tr=NULL; for(int i=0;i<blk->body_count;i++) if(blk->body[i]->kind==STMT_TRY) tr=blk->body[i];
    int keep=tg&&env?1:0;
    if(keep) tr->body[0]->targets[0]=tg;
    Stmt **nb=PPY_NEW_ARR(Stmt*,keep+s->body_count+1); int nn=0;
    if(keep) nb[nn++]=tr->body[0];
    for(int i=0;i<s->body_count;i++) nb[nn++]=s->body[i];
    if(!nn) nb[nn++]=stmt_new(STMT_PASS,NULL,line);
    tr->body=nb; tr->body_count=nn; tr->body_cap=nn;
    s->kind=STMT_IF; s->expr=xnew(EXPR_TRUE,line); s->body=blk->body; s->body_count=blk->body_count; s->body_cap=blk->body_cap; s->aux=NULL;
    return 1;
}
/* obj[k] op= v (an object's __getitem__ / __setitem__): t = obj; i = k; t[i] = t[i] op v */
void desugar_aug_item(Ck *c, Stmt *s, AAssign *a){
    int line=s->line; Expr *t=a->target[0];
    static const struct { int aug, op; } ops[]={{T_PLUS_ASSIGN,T_PLUS},{T_MINUS_ASSIGN,T_MINUS},{T_STAR_ASSIGN,T_STAR},{T_SLASH_ASSIGN,T_SLASH},
        {T_FLOOR_DIV_ASSIGN,T_FLOOR_DIV},{T_PERCENT_ASSIGN,T_PERCENT},{T_POWER_ASSIGN,T_POWER},{T_AMP_ASSIGN,T_AMP},{T_PIPE_ASSIGN,T_PIPE},
        {T_CARET_ASSIGN,T_CARET},{T_SHL_ASSIGN,T_SHL},{T_SHR_ASSIGN,T_SHR},{T_AT_ASSIGN,T_AT},{0,0}};
    int op=a->aug; for(int i=0;ops[i].aug;i++) if(ops[i].aug==a->aug) op=ops[i].op;
    const char *ho=hidden_for(c,"augobj"), *hk=hidden_for(c,"augkey"); hidden_reg(c,ho); hidden_reg(c,hk);
    Expr *bin=xnew(EXPR_BINARY,line); bin->op=op; bin->a=ex_index(nx(ho,line),nx(hk,line),line); bin->b=a->value;
    Stmt **b=PPY_NEW_ARR(Stmt*,3);
    b[0]=st_assign(nx(ho,line),t->a,line); b[1]=st_assign(nx(hk,line),t->b,line); b[2]=st_assign(ex_index(nx(ho,line),nx(hk,line),line),bin,line);
    s->kind=STMT_IF; s->expr=xnew(EXPR_TRUE,line); s->body=b; s->body_count=s->body_cap=3; s->orelse_count=0; s->aux=NULL; s->aug=0; s->targets=NULL; s->ntargets=0; s->value=NULL;
}
/* async for x in obj (its __aiter__ / __anext__): a while loop until StopAsyncIteration (else: when it ran out) */
void desugar_async_for(Ck *c, Stmt *s){
    AotUnit *u=c->mod->unit; int line=s->line, k=++c->nhidden;
    char it[48], v[48], d[48]; snprintf(it,sizeof it,"__ppy_afor%d_it",k); snprintf(v,sizeof v,"__ppy_afor%d_value",k); snprintf(d,sizeof d,"__ppy_afor%d_done",k);
    hidden_reg(c,it); hidden_reg(c,v); hidden_reg(c,d);
    Txt x={0}; for(int i=1;i<line;i++) tx(&x,"\n");
    tx(&x,"%s = (0).__aiter__()\n%s = False\nwhile True:\n    try:\n        %s = await %s.__anext__()\n    except StopAsyncIteration:\n        %s = True\n        break\n    __ppy_target = %s\n    pass\nif %s:\n    pass\n",it,d,v,it,d,v,d);
    Stmt *blk=py_front_stmts(u->path,x.s,line); free(x.s);
    if(!blk) ck_err(c,line,"internal error: async for");
    blk->body[0]->value->a->a=aot_expr(u,&s->expr);                 /* (0).__aiter__() -> obj.__aiter__() */
    Stmt *wh=blk->body[2], *ifd=blk->body[3];
    Stmt *as=wh->body[1];
    if(s->param_count==1) as->targets[0]=name_expr(s->params[0],line);
    else { Expr *tu=xnew(EXPR_TUPLE,line); for(int i=0;i<s->param_count;i++) xpush(tu,name_expr(s->params[i],line)); as->targets[0]=tu; }
    Stmt **nb=PPY_NEW_ARR(Stmt*,2+s->body_count); int nn=0;
    nb[nn++]=wh->body[0]; nb[nn++]=as; for(int i=0;i<s->body_count;i++) nb[nn++]=s->body[i];
    wh->body=nb; wh->body_count=nn; wh->body_cap=nn;
    if(s->orelse_count){ ifd->body=s->orelse; ifd->body_count=s->orelse_count; ifd->body_cap=s->orelse_cap; }
    s->kind=STMT_IF; s->expr=xnew(EXPR_TRUE,line); s->body=blk->body; s->body_count=blk->body_count; s->body_cap=blk->body_cap;
    s->orelse=NULL; s->orelse_count=s->orelse_cap=0; s->aux=NULL; s->is_async=0;
}
/* isinstance(x, C) (also first in an `and`), x a variable of a base class of C -> 1 (its name, C);
   -1 when x's type is not known yet; 0 otherwise */
int isinstance_test(Ck *c, Expr *e, const char **name, AClass **k, int allow_and){
    if(allow_and && e->kind==EXPR_BOOL && e->op==T_AND) return isinstance_test(c,e->a,name,k,1);
    if(e->kind!=EXPR_CALL || e->a->kind!=EXPR_NAME || strcmp(e->a->name,"isinstance") || lookup(c,"isinstance") || e->count!=2 || e->items[0]->kind!=EXPR_NAME || e->items[0]->akind || e->items[1]->akind) return 0;
    AClass *kc=class_expr(c,e->items[1]); if(!kc) return 0;
    ASym *s=lookup(c,e->items[0]->name); if(!s || s->kind!=AS_VAR) return 0;
    Ty *t=ty_find(((AVar*)s->p)->ty);
    if(t->k==TY_VAR){ pending(c,e->line,"the variable isinstance() tests"); return -1; }
    if(t->k!=TY_OBJ || t->cls==kc || !aot_subclass(kc,t->cls)) return 0;
    *name=e->items[0]->name; *k=kc; return 1;
}
int stores_name(Ck *c, Stmt **b, int n, const char *name){
    Names nb={0}; names_bound(c,b,n,&nb,NULL);
    int r=0; for(int i=0;i<nb.n;i++) if(!strcmp(nb.v[i],name)) r=1;
    free(nb.v); return r;
}
/* try: ... except* T [as e]: ...: one except BaseException whose handler runs the clauses on the
   parts of the group they match (__ppy_EGState of the ExceptionGroup prelude), then raises what is left */
void desugar_trystar(Ck *c, Stmt *s){
    AotUnit *u=c->mod->unit; int line=s->line, k=++c->nhidden;
    char ex[48], st[48], m[48], x[48];
    snprintf(ex,sizeof ex,"__ppy_eg%d_exc",k); snprintf(st,sizeof st,"__ppy_eg%d_state",k); snprintf(m,sizeof m,"__ppy_eg%d_match",k); snprintf(x,sizeof x,"__ppy_eg%d_raised",k);
    hidden_reg(c,st); hidden_reg(c,m);
    Stmt *hs[32]; int nh=0; Stmt *els=NULL, *fin=NULL;
    for(int i=0;i<s->orelse_count;i++){ Stmt *b=s->orelse[i];
        if(b->block_tag==1){ if(nh==32) ck_err(c,b->line,"too many except* clauses"); if(!b->expr) ck_err(c,b->line,"expected one or more exception types"); hs[nh++]=b; }
        else if(b->block_tag==2) els=b; else if(b->block_tag==3) fin=b; }
    Txt t={0}; for(int i=1;i<line;i++) tx(&t,"\n");
    tx(&t,"try:\n    pass\nexcept BaseException as %s:\n    %s = __ppy_EGState(%s)\n",ex,st,ex);
    for(int i=0;i<nh;i++){ char nm[64]; if(hs[i]->name) snprintf(nm,sizeof nm,"%s",hs[i]->name); else snprintf(nm,sizeof nm,"__ppy_eg%d_group%d",k,i);
        tx(&t,"    %s = %s.match(lambda __ppy_x: isinstance(__ppy_x, 0))\n    if %s is not None:\n        try:\n            raise %s\n        except BaseExceptionGroup as %s:\n"
              "            try:\n                pass\n            except BaseException as %s:\n                %s.handler_raised(%s)\n",m,st,m,m,nm,x,st,x); }
    tx(&t,"    %s.end()\n",st);
    Stmt *blk=py_front_stmts(u->path,t.s,line); free(t.s);
    if(!blk) ck_err(c,line,"internal error: except*");
    Stmt *tr=blk->body[0], *h=tr->orelse[0];
    tr->body=s->body; tr->body_count=s->body_count; tr->body_cap=s->body_cap;
    for(int i=0;i<nh;i++){
        Stmt *as=h->body[1+2*i], *iff=h->body[2+2*i];
        Expr *lam=as->value->items[0]; Expr *call=lam->a; call->items[1]=aot_expr(u,&hs[i]->expr);
        Stmt *inner=iff->body[0]->orelse[0]->body[0];
        inner->body=hs[i]->body; inner->body_count=hs[i]->body_count; inner->body_cap=hs[i]->body_cap;
        if(!inner->body_count){ inner->body=NULL; inner->body_cap=0; stmt_add_body(inner,stmt_new(STMT_PASS,NULL,line)); } }
    if(els) stmt_add_orelse(tr,els);
    if(fin) stmt_add_orelse(tr,fin);
    *s=*tr;                                                          /* (the try in place) */
}
/* eg.split(ValueError), eg.subgroup((A, B)): with a class -> lambda e: isinstance(e, ...) */
void eg_class_arg(Ck *c, Expr *e){
    Expr *f=e->a;
    if(f->kind!=EXPR_ATTRIBUTE || (strcmp(f->name,"split") && strcmp(f->name,"subgroup")) || e->count!=1 || e->items[0]->akind) return;
    Expr *a=e->items[0];
    int cls= class_expr(c,a)!=NULL || (a->kind==EXPR_NAME && is_builtin_type_name(a->name));
    if(a->kind==EXPR_TUPLE){ cls=a->count>0; for(int i=0;i<a->count;i++) if(!class_expr(c,a->items[i])) cls=0; }
    if(!cls) return;
    Ty *o=ty_find(ck_expr(c,f->a)); ASym *g=symtab_find(&builtin_syms,"BaseExceptionGroup");
    if(o->k!=TY_OBJ || !g || g->kind!=AS_CLASS || !aot_subclass(o->cls,(AClass*)g->p)) return;
    Expr *call=xnew(EXPR_CALL,e->line); call->a=name_expr("isinstance",e->line); xpush(call,name_expr("__ppy_x",e->line)); xpush(call,a);
    Expr *lam=xnew(EXPR_LAMBDA,e->line); lam->eparams=PPY_NEW_ARR(char*,1); lam->eparams[0]="__ppy_x"; lam->neparam=1; lam->edefaults=PPY_NEW_ARR(Expr*,1);
    lam->estar=lam->edstar=-1; lam->ekwonly=1; lam->a=call;
    e->items[0]=lam;
}
