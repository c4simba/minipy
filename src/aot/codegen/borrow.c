/* ========================= Typed compiler, code generator: borrowed references =========================
   Reference counts the compiler can drop: pure expressions, loop
   variables that borrow their items. */

#include "gen.h"

/* ---------------------------------------------------------------- reference counts the compiler can drop
   A loop variable normally holds a reference of its own to the current item
   (incref when it takes the item, decref when it moves on). It can simply
   borrow the item from the container when the container keeps every item
   alive for the whole loop: nobody else can reach it (a fresh list, a fresh
   generator), it cannot change (str, tuple), or the loop's body cannot remove
   anything from any container (no user code runs, no pop/remove/del/x[i]=...).
   The variable must also be used only inside the loops that bind it. */

static int fmt_pure(Ty *t){                      /* formatting t runs no user code (__str__/__repr__) */
    t=ty_find(t);
    switch(t->k){
        case TY_OBJ: for(AClass *c=t->cls;c;c=c->base) for(int i=0;i<c->nmethods;i++)
                         if(!strcmp(c->methods[i]->name,"__str__")||!strcmp(c->methods[i]->name,"__repr__")) return 0;
                     return 1;
        case TY_DICT: return fmt_pure(ty_dkey(t)) && fmt_pure(t->elem);
        case TY_LIST: case TY_SET: return fmt_pure(t->elem);
        case TY_TUPLE: for(int i=0;i<t->nelems;i++) if(!fmt_pure(t->elems[i])) return 0; return 1;
        default: return 1;
    }
}
static int iterates_code(Ty *t){ t=ty_find(t); return t->k==TY_GEN||t->k==TY_OBJ; }   /* iterating it runs user code */
static int pure_expr(Expr *e);
static int pure_exprs(Expr **v, int n){ for(int i=0;i<n;i++) if(!pure_expr(v[i])) return 0; return 1; }
static int pure_call(Expr *e){
    XInfo *xi=xinfo(e);
    if(!pure_exprs(e->items,e->count)) return 0;
    const char *n=xi->name?xi->name:"";
    switch(xi->kind){
        case X_BUILTIN:
            if(!strcmp(n,"next")) return 0;
            if(!strcmp(n,"len") && xi->fn) return 0;                     /* __len__ */
            if((!strcmp(n,"str")||!strcmp(n,"repr")||!strcmp(n,"format")) && e->count && !fmt_pure(TY(e->items[0]))) return 0;
            if(xi->key){ if(e->a && xi->key->kind==EXPR_LAMBDA) { if(!pure_expr(xi->key->a)) return 0; } else if(strcmp(n,"reduce")) return 0; }
            if(!strcmp(n,"reduce") && !pure_expr(xi->key)) return 0;
            for(int i=0;i<e->count;i++){ Ty *it=TY(e->items[i]); if(it && it->k==TY_GEN) return 0; }   /* draining a generator runs its code (a format spec: no type) */
            return 1;
        case X_TMETHOD:{
            static const char *removing[]={"pop","remove","clear","discard","update",NULL};
            for(int i=0;removing[i];i++) if(!strcmp(n,removing[i])) return 0;
            if(!strcmp(n,"sort") && xi->key && (xi->key->kind!=EXPR_LAMBDA || !pure_expr(xi->key->a))) return 0;
            if(!strcmp(n,"format")) for(int i=0;i<e->count;i++) if(!fmt_pure(TY(e->items[i]))) return 0;
            return pure_expr(e->a->a); }
        case X_SYS: case X_SYSCALL: return 1;
        case X_BMOD: return pure_expr(e->a);
        case X_CTOR: return !xi->fn && xi->cls && aot_is_exception(xi->cls) && (!e->count || fmt_pure(TY(e->items[0])));
        default: return 0;                                              /* user code */
    }
}
static int pure_expr(Expr *e){
    if(!e) return 1;
    XInfo *xi=(XInfo*)e->ty;
    XKind k=xi?xi->kind:X_NONE;
    switch(e->kind){
        case EXPR_LITERAL: case EXPR_TRUE: case EXPR_FALSE: case EXPR_NONE: case EXPR_NAME: case EXPR_LAMBDA: return 1;
        case EXPR_UNARY: return k!=X_OPMETHOD && pure_expr(e->a);
        case EXPR_BINARY:
            if(k==X_OPMETHOD) return 0;
            if(e->op==T_PERCENT && TY(e)->k==TY_STR){
                if(e->b->kind==EXPR_TUPLE){ for(int i=0;i<e->b->count;i++) if(!fmt_pure(TY(e->b->items[i]))) return 0; }
                else if(!fmt_pure(TY(e->b))) return 0; }
            return pure_expr(e->a) && pure_expr(e->b);
        case EXPR_BOOL: return pure_expr(e->a) && pure_expr(e->b);
        case EXPR_COMPARE: if(xi && xi->cmpfn) for(int i=0;i<e->count;i++) if(xi->cmpfn[i]) return 0;
            return pure_exprs(e->items,e->count);
        case EXPR_TERNARY: return pure_expr(e->a) && pure_expr(e->b) && pure_expr(e->c);
        case EXPR_ATTRIBUTE: return k!=X_PROP && pure_expr(e->a);
        case EXPR_INDEX: return k!=X_OPMETHOD && pure_expr(e->a) && pure_expr(e->b);
        case EXPR_SLICE: return pure_expr(e->a) && pure_expr(e->b) && pure_expr(e->c) && pure_expr(e->d);
        case EXPR_LIST: case EXPR_SET: case EXPR_TUPLE: return pure_exprs(e->items,e->count);
        case EXPR_DICT: return pure_exprs(e->items,e->count) && pure_exprs(e->vals,e->count);
        case EXPR_COMPREHENSION:
            if(e->comp_kind=='G') return pure_expr(e->clauses[0].iter);       /* making the generator runs nothing else */
            for(int i=0;i<e->nclause;i++){ CompClause *cl=&e->clauses[i];
                if(!pure_expr(cl->iter) || iterates_code(TY(cl->iter)) || !pure_exprs(cl->conds,cl->ncond)) return 0; }
            return pure_expr(e->a) && pure_expr(e->b);
        case EXPR_CALL: return pure_call(e);
        default: return 0;
    }
}
static int pure_stmts(AotUnit *u, Stmt **b, int n);
static int pure_target(Expr *t){
    switch(t->kind){
        case EXPR_NAME: return 1;
        case EXPR_ATTRIBUTE: return xinfo(t)->kind!=X_PROP && pure_expr(t->a);   /* a field's old value goes, items stay */
        case EXPR_TUPLE: case EXPR_LIST: for(int i=0;i<t->count;i++) if(!pure_target(t->items[i])) return 0; return 1;
        default: return 0;                                                     /* x[i] = ... replaces an item */
    }
}
static int pure_stmt(AotUnit *u, Stmt *s){
    switch(s->kind){
        case STMT_EXPR:{
            APrint *pr=aot_print(u,s);
            if(pr){ for(int i=0;i<pr->n;i++){ if(!pure_expr(pr->args[i])) return 0;
                        Ty *t=TY(pr->args[i]); if(pr->star[i]){ if(iterates_code(t)) return 0; t=ty_find(t)->k==TY_TUPLE?t:ty_find(t)->elem; }
                        if(!fmt_pure(t)) return 0; }
                    return pure_expr(pr->sep) && pure_expr(pr->end); }
            if(aot_is_annotation_only(u,s)) return 1;
            return pure_expr(s->expr); }
        case STMT_ASSIGN:{ AAssign *a=aot_assign(u,s);
            for(int k=0;k<a->ntarget;k++) if(!pure_target(a->target[k])) return 0;
            if(a->opfn) return 0;
            return pure_expr(a->value); }
        case STMT_IF: case STMT_WHILE: return pure_expr(s->expr) && pure_stmts(u,s->body,s->body_count) && pure_stmts(u,s->orelse,s->orelse_count);
        case STMT_FOR:{ Expr *it=s->expr; XInfo *xi=xinfo(it);
            if(xi->kind==X_BUILTIN && (!strcmp(xi->name,"enumerate")||!strcmp(xi->name,"zip"))){ for(int i=0;i<it->count;i++) if(iterates_code(TY(it->items[i]))) return 0; }
            else if(!(xi->kind==X_TMETHOD && !strcmp(xi->name,"items")) && iterates_code(TY(it))) return 0;
            return pure_expr(it) && pure_stmts(u,s->body,s->body_count) && pure_stmts(u,s->orelse,s->orelse_count); }
        case STMT_RETURN: case STMT_ASSERT: case STMT_RAISE: return pure_expr(s->expr) && pure_expr(s->expr2);
        case STMT_TRY: case STMT_BLOCK: return pure_stmts(u,s->body,s->body_count) && pure_stmts(u,s->orelse,s->orelse_count);
        case STMT_WITH:{ AWith *w=aot_with(u,s); for(int i=0;i<w->n;i++) if(!pure_expr(w->e[i])) return 0; return pure_stmts(u,s->body,s->body_count); }
        case STMT_PASS: case STMT_BREAK: case STMT_CONTINUE: case STMT_GLOBAL: case STMT_NONLOCAL: return 1;
        case STMT_FUNCTION_DEF: return s->expr2==NULL;
        default: return 0;                                                     /* yield (the consumer runs), del, ... */
    }
}
static int pure_stmts(AotUnit *u, Stmt **b, int n){ for(int i=0;i<n;i++) if(!pure_stmt(u,b[i])) return 0; return 1; }

/* an expression giving a new object nothing else refers to */
static int fresh_expr(Expr *e){
    switch(e->kind){
        case EXPR_COMPREHENSION: case EXPR_LIST: case EXPR_SET: case EXPR_DICT: case EXPR_TUPLE: case EXPR_SLICE: return 1;
        case EXPR_BINARY: return xinfo(e)->kind!=X_OPMETHOD && TY(e)->k==TY_LIST;
        case EXPR_CALL:{ XInfo *xi=xinfo(e); const char *n=xi->name?xi->name:"";
            if(xi->kind==X_BUILTIN){ static const char *fresh[]={"sorted","list","set","reversed","range_list","dict_copy",NULL};
                for(int i=0;fresh[i];i++) if(!strcmp(n,fresh[i])) return 1; return 0; }
            if(xi->kind==X_TMETHOD){ static const char *fresh[]={"split","splitlines","keys","values","items","copy","union","intersection","difference","readlines",NULL};
                for(int i=0;fresh[i];i++) if(!strcmp(n,fresh[i])) return 1; return 0; }
            return (xi->kind==X_FUNC||xi->kind==X_METHOD||xi->kind==X_STATIC||xi->kind==X_CALLNEST||xi->kind==X_SUPER) && xi->fn && xi->fn->is_gen; }
        default: return 0;
    }
}
/* the items of x stay alive for the whole loop, whatever the loop does */
static int src_safe(Expr *x){
    Ty *t=TY(x);
    if(t->k==TY_STR||t->k==TY_BYTES||t->k==TY_TUPLE||t->k==TY_FILE) return 1;              /* static 1-char strings; immutable; a fresh list of lines */
    if(t->k==TY_OBJ) return 0;
    return fresh_expr(x);
}
static int iter_safe(Expr *it){
    XInfo *xi=xinfo(it);
    if(xi->kind==X_BUILTIN && (!strcmp(xi->name,"range")||!strcmp(xi->name,"reversed_range"))) return 1;
    if(xi->kind==X_BUILTIN && (!strcmp(xi->name,"enumerate")||!strcmp(xi->name,"zip"))) return src_safe(it->items[0]) && (xi->name[0]=='e' || src_safe(it->items[1]));
    if(xi->kind==X_TMETHOD && !strcmp(xi->name,"items") && ty_find(xi->ty)->k==TY_VOID) return src_safe(it->a->a);
    return src_safe(it);
}

typedef struct { AFunc *fn; AotUnit *u; char *only, *ok; AVar *stack[64]; int nstack; } Plan;
static int plan_index(Plan *pl, AVar *v){ return v && !v->src && !v->global && v->owner==pl->fn && v->id>=0 && v->id<pl->fn->nvars && pl->fn->vars[v->id]==v ? v->id : -1; }
static int plan_active(Plan *pl, AVar *v){ for(int i=0;i<pl->nstack;i++) if(pl->stack[i]==v) return 1; return 0; }
static void plan_use(Plan *pl, AVar *v){ int i=plan_index(pl,v); if(i>=0 && !plan_active(pl,v)) pl->only[i]=0; }
static void plan_store(Plan *pl, AVar *v){ int i=plan_index(pl,v); if(i>=0) pl->only[i]=0; }
static void plan_expr(Plan *pl, Expr *e);
static void plan_target(Plan *pl, Expr *t);
static int xi_of_iter(Expr *it);
/* loop variables vars (n), bound per item; safe: the items stay alive meanwhile; then the body */
static void plan_bind(Plan *pl, AVar **vars, int n, int safe){
    for(int k=0;k<n;k++){ int i=plan_index(pl,vars[k]); if(i<0) continue;
        if(plan_active(pl,vars[k])) pl->only[i]=0;                              /* bound again inside its own loop */
        if(!safe) pl->ok[i]=0; }
    for(int k=0;k<n && pl->nstack<64;k++) pl->stack[pl->nstack++]=vars[k];
}
static void plan_comp(Plan *pl, Expr *e){                                       /* a comprehension's clauses, then its items */
    XInfo *xi=xinfo(e); int save=pl->nstack;
    for(int i=0;i<e->nclause;i++){ CompClause *cl=&e->clauses[i];
        plan_expr(pl,cl->iter);
        int safe=iter_safe(cl->iter);
        if(!safe){ safe=pure_expr(e->a) && pure_expr(e->b);                    /* the rest of the comprehension runs per item */
            for(int k=i;k<e->nclause && safe;k++){ CompClause *c2=&e->clauses[k];
                if(!pure_exprs(c2->conds,c2->ncond) || (k>i && (!pure_expr(c2->iter) || iterates_code(TY(c2->iter))))) safe=0; } }
        if(cl->nvars==1 && xi_of_iter(cl->iter)) plan_store(pl,xi->cvars[2*i]); else plan_bind(pl,&xi->cvars[2*i],cl->nvars,safe);
        if(cl->target) plan_target(pl,cl->target);
        if(cl->target2) plan_target(pl,cl->target2);
        for(int k=0;k<cl->ncond;k++) plan_expr(pl,cl->conds[k]);
    }
    plan_expr(pl,e->a); plan_expr(pl,e->b);
    pl->nstack=save;
}
static void plan_expr(Plan *pl, Expr *e){
    if(!e) return;
    XInfo *xi=(XInfo*)e->ty;
    switch(e->kind){
        case EXPR_NAME: if(xi && xi->kind==X_VAR) plan_use(pl,xi->var); return;
        case EXPR_LAMBDA: return;                                               /* its own function */
        case EXPR_WALRUS: plan_expr(pl,e->a); plan_target(pl,e->b); return;
        case EXPR_COMPREHENSION:
            if(e->comp_kind=='G'){ if(xi->key) plan_expr(pl,e->clauses[0].iter); return; }   /* the rest is the generator's function */
            plan_comp(pl,e); return;
        case EXPR_CALL:
            plan_expr(pl,e->a);
            for(int i=0;i<e->count;i++) plan_expr(pl,e->items[i]);
            if(xi && xi->key && xi->key->kind==EXPR_LAMBDA && xinfo(xi->key)->var){  /* key=lambda k: ...: k takes each item */
                int save=pl->nstack; AVar *kv=xinfo(xi->key)->var;
                plan_bind(pl,&kv,1,pure_expr(xi->key->a));
                plan_expr(pl,xi->key->a); pl->nstack=save; }
            else if(xi && xi->key && xi->kind==X_BUILTIN && xi->name && !strcmp(xi->name,"reduce")){
                int save=pl->nstack;
                plan_store(pl,xi->cvars[0]);                                    /* the accumulator takes results */
                plan_bind(pl,&xi->cvars[1],1,pure_expr(xi->key));               /* the item: one per element */
                plan_expr(pl,xi->key); pl->nstack=save; }
            else if(xi && xi->key) plan_expr(pl,xi->key);
            if(xi && xi->rev) plan_expr(pl,xi->rev);
            return;
        default:
            plan_expr(pl,e->a); plan_expr(pl,e->b); plan_expr(pl,e->c); plan_expr(pl,e->d);
            for(int i=0;i<e->count;i++) plan_expr(pl,e->items[i]);
            for(int i=0;i<e->vcount;i++) plan_expr(pl,e->vals[i]);
            return;
    }
}
static void plan_target(Plan *pl, Expr *t){
    if(t->kind==EXPR_NAME){ XInfo *xi=(XInfo*)t->ty; if(xi && xi->var) plan_store(pl,xi->var); else plan_store(pl,NULL); return; }
    if(t->kind==EXPR_TUPLE||t->kind==EXPR_LIST){ for(int i=0;i<t->count;i++) plan_target(pl,t->items[i]); return; }
    plan_expr(pl,t->a); plan_expr(pl,t->b);
}
static void plan_stmts(Plan *pl, F *f, Stmt **b, int n);
/* 1 when iterating it gives a tuple made per item to a single variable (zip, enumerate, items) */
static int xi_of_iter(Expr *it){ XInfo *xi=xinfo(it);
    return (xi->kind==X_BUILTIN && xi->name && (!strcmp(xi->name,"enumerate")||!strcmp(xi->name,"zip"))) || (xi->kind==X_TMETHOD && xi->name && !strcmp(xi->name,"items")); }
static void plan_stmt(Plan *pl, F *f, Stmt *s){
    AotUnit *u=pl->u;
    switch(s->kind){
        case STMT_EXPR:{ APrint *pr=aot_print(u,s);
            if(pr){ for(int i=0;i<pr->n;i++) plan_expr(pl,pr->args[i]); plan_expr(pl,pr->sep); plan_expr(pl,pr->end); return; }
            if(aot_is_annotation_only(u,s)){ AAssign *a=aot_assign(u,s); for(int k=0;k<a->ntarget;k++) plan_target(pl,a->target[k]); return; }
            plan_expr(pl,s->expr); return; }
        case STMT_ASSIGN:{ AAssign *a=aot_assign(u,s);
            for(int k=0;k<a->ntarget;k++){ plan_target(pl,a->target[k]); if(a->aug) plan_expr(pl,a->target[k]); }
            plan_expr(pl,a->value); return; }
        case STMT_FOR:{
            Expr *it=s->expr; plan_expr(pl,it);
            AVar *vars[16]; int n=s->param_count<16?s->param_count:16;
            for(int k=0;k<n;k++) vars[k]=find_var(f,s->params[k],s->line);
            int safe=iter_safe(it) || (pure_stmts(u,s->body,s->body_count) && pure_stmts(u,s->orelse,s->orelse_count));
            int save=pl->nstack;
            if(n==1 && ((xi_of_iter(it)==1))) plan_store(pl,vars[0]);              /* for t in zip(...): a new tuple, owned */
            else plan_bind(pl,vars,n,safe);
            plan_stmts(pl,f,s->body,s->body_count); plan_stmts(pl,f,s->orelse,s->orelse_count);
            pl->nstack=save; return; }
        case STMT_WITH:{ AWith *w=aot_with(u,s);
            for(int i=0;i<w->n;i++){ plan_expr(pl,w->e[i]); if(w->as[i]) plan_store(pl,find_var(f,w->as[i],s->line)); }
            plan_stmts(pl,f,s->body,s->body_count); return; }
        case STMT_FUNCTION_DEF:
            if(s->expr2) plan_expr(pl,s->expr2);
            if(is_fn_body(f->fn)) plan_store(pl,find_var(f,s->name,s->line));
            return;
        case STMT_TRY:
            plan_stmts(pl,f,s->body,s->body_count);
            for(int i=0;i<s->orelse_count;i++){ Stmt *b=s->orelse[i]; if(b->name && b->aux) plan_store(pl,(AVar*)b->aux); plan_stmts(pl,f,b->body,b->body_count); }
            return;
        case STMT_YIELD: case STMT_RETURN: case STMT_RAISE: case STMT_IF: case STMT_WHILE: case STMT_ASSERT: case STMT_DEL: case STMT_BLOCK:
            if(s->expr) plan_expr(pl,s->expr);
            if(s->kind==STMT_DEL){ ADel *d=aot_del(u,s); for(int i=0;i<d->n;i++) plan_expr(pl,d->t[i]); }
            if(s->expr2) plan_expr(pl,s->expr2);
            plan_stmts(pl,f,s->body,s->body_count); plan_stmts(pl,f,s->orelse,s->orelse_count); return;
        default: return;
    }
}
static void plan_stmts(Plan *pl, F *f, Stmt **b, int n){ for(int i=0;i<n;i++) plan_stmt(pl,f,b[i]); }
/* decide which of fn's loop variables borrow their items */
void plan_borrowed(F *f){
    AFunc *fn=f->fn; if(!fn->nvars) return;
    Plan pl; memset(&pl,0,sizeof pl); pl.fn=fn; pl.u=fn->mod->unit;
    pl.only=(char*)xmalloc((size_t)fn->nvars); pl.ok=(char*)xmalloc((size_t)fn->nvars);
    for(int i=0;i<fn->nvars;i++){ AVar *v=fn->vars[i]; Ty *t=ty_find(v->ty);
        pl.only[i]=pl.ok[i]=(i>=fn->nparams && is_ptr(t) && t->k!=TY_FUNC && !v->cell && !v->captured && !v->bound); }
    if(fn->lam) plan_expr(&pl,fn->lam->a);
    else if(fn->genexp){                                                         /* its clauses: the body yields (anything may run) */
        Expr *e=fn->genexp; XInfo *xi=xinfo(e); int save=pl.nstack;
        for(int i=0;i<e->nclause;i++){ CompClause *cl=&e->clauses[i];
            Expr *it=(i==0&&xi->key)?xi->key:cl->iter; plan_expr(&pl,it);
            if(cl->nvars==1 && xi_of_iter(it)) plan_store(&pl,xi->cvars[2*i]); else plan_bind(&pl,&xi->cvars[2*i],cl->nvars,iter_safe(it));
            if(e->clauses[i].target) plan_target(&pl,e->clauses[i].target);
            if(e->clauses[i].target2) plan_target(&pl,e->clauses[i].target2);
            for(int k=0;k<cl->ncond;k++) plan_expr(&pl,cl->conds[k]); }
        plan_expr(&pl,e->a); pl.nstack=save;
    }
    else plan_stmts(&pl,f,fn->body,fn->nbody);
    for(int i=0;i<fn->nvars;i++) fn->vars[i]->borrowed=pl.only[i] && pl.ok[i];
    free(pl.only); free(pl.ok);
}
