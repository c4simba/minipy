/* ========================= Typed compiler, code generator: moves =========================
   A local variable's reference handed over on its last use (liveness);
   the parameters a function keeps. */

#include "gen.h"

/* ---- moves: a local variable's reference handed over on its last use ----
   Backward liveness over the function's statements (loops to a fixpoint)
   gives, after each simple statement, the variables still read later. Where
   an owned value is wanted (y = x, return x, xs.append(x), yield x) and x is
   dead after the statement and occurs in it once, x's reference moves: the
   slot is cleared instead of an incref now and a decref later. */

static unsigned **live_slot(LiveTab *t, Stmt *s){
    if(t->n*2>=t->cap){
        Stmt **ok=t->keys; unsigned **ov=t->vals; int oc=t->cap;
        t->cap=t->cap?t->cap*2:64; t->n=0;
        t->keys=(Stmt**)memset(xmalloc(sizeof(Stmt*)*(size_t)t->cap),0,sizeof(Stmt*)*(size_t)t->cap);
        t->vals=(unsigned**)memset(xmalloc(sizeof(unsigned*)*(size_t)t->cap),0,sizeof(unsigned*)*(size_t)t->cap);
        for(int i=0;i<oc;i++) if(ok[i]) *live_slot(t,ok[i])=ov[i];
        free(ok); free(ov);
    }
    unsigned h=(unsigned)(((uintptr_t)s>>4)*2654435761u)&(unsigned)(t->cap-1);
    while(t->keys[h] && t->keys[h]!=s) h=(h+1)&(unsigned)(t->cap-1);
    if(!t->keys[h]){ t->keys[h]=s; t->vals[h]=NULL; t->n++; }
    return &t->vals[h];
}
static unsigned *live_get(LiveTab *t, Stmt *s){
    if(!t || !t->cap) return NULL;
    unsigned h=(unsigned)(((uintptr_t)s>>4)*2654435761u)&(unsigned)(t->cap-1);
    while(t->keys[h]){ if(t->keys[h]==s) return t->vals[h]; h=(h+1)&(unsigned)(t->cap-1); }
    return NULL;
}
typedef struct { AFunc *fn; AotUnit *u; LiveTab *tab; int W; unsigned *brk[64], *cont[64]; int nloop; } Live;
static unsigned *bs_new(Live *L){ unsigned *b=(unsigned*)xmalloc(sizeof(unsigned)*(size_t)L->W); memset(b,0,sizeof(unsigned)*(size_t)L->W); return b; }
static unsigned *bs_dup(Live *L, unsigned *a){ unsigned *b=bs_new(L); memcpy(b,a,sizeof(unsigned)*(size_t)L->W); return b; }
static void bs_or(Live *L, unsigned *a, unsigned *b){ for(int i=0;i<L->W;i++) a[i]|=b[i]; }
static int bs_eq(Live *L, unsigned *a, unsigned *b){ return !memcmp(a,b,sizeof(unsigned)*(size_t)L->W); }
static int live_index(Live *L, AVar *v){ return v && !v->src && !v->global && v->owner==L->fn && v->id>=0 && v->id<L->fn->nvars && L->fn->vars[v->id]==v ? v->id : -1; }
static void live_uses(Live *L, Expr *e, unsigned *set){
    if(!e) return;
    XInfo *xi=(XInfo*)e->ty;
    if(e->kind==EXPR_NAME){ if(xi && xi->kind==X_VAR){ int i=live_index(L,xi->var); if(i>=0) set[i>>5]|=1u<<(i&31); } return; }
    if(e->kind==EXPR_LAMBDA) return;                         /* its own function (captured variables never move) */
    if(e->kind==EXPR_COMPREHENSION){ for(int i=0;i<e->nclause;i++){ live_uses(L,e->clauses[i].iter,set); for(int k=0;k<e->clauses[i].ncond;k++) live_uses(L,e->clauses[i].conds[k],set); } }
    live_uses(L,e->a,set); live_uses(L,e->b,set); live_uses(L,e->c,set); live_uses(L,e->d,set);
    for(int i=0;i<e->count;i++) live_uses(L,e->items[i],set);
    for(int i=0;i<e->vcount;i++) live_uses(L,e->vals[i],set);
    if(e->kind==EXPR_CALL && xi){ live_uses(L,xi->key,set); live_uses(L,xi->rev,set); }
}
static void live_kill(Live *L, AVar *v, unsigned *set){ int i=live_index(L,v); if(i>=0) set[i>>5]&=~(1u<<(i&31)); }
static void live_target(Live *L, Expr *t, unsigned *kill, unsigned *use){   /* names assigned; the rest of a target is read */
    if(t->kind==EXPR_NAME){ XInfo *xi=(XInfo*)t->ty; if(xi && xi->var){ int i=live_index(L,xi->var); if(i>=0) kill[i>>5]|=1u<<(i&31); } return; }
    if(t->kind==EXPR_TUPLE||t->kind==EXPR_LIST){ for(int i=0;i<t->count;i++) live_target(L,t->items[i],kill,use); return; }
    live_uses(L,t->a,use); live_uses(L,t->b,use);
}
static void live_stmts(Live *L, F *f, Stmt **b, int n, unsigned *live);
static void live_stmt(Live *L, F *f, Stmt *s, unsigned *live){
    AotUnit *u=L->u;
    switch(s->kind){
        case STMT_ASSIGN:{
            *live_slot(L->tab,s)=bs_dup(L,live);
            AAssign *a=aot_assign(u,s); if(!a->value) return;
            unsigned *kill=bs_new(L), *use=bs_new(L);
            for(int k=0;k<a->ntarget;k++){ live_target(L,a->target[k],kill,use); if(a->aug) live_uses(L,a->target[k],use); }
            if(!a->aug) for(int i=0;i<L->W;i++) live[i]&=~kill[i];
            live_uses(L,a->value,use); bs_or(L,live,use); free(kill); free(use); return; }
        case STMT_EXPR:{
            *live_slot(L->tab,s)=bs_dup(L,live);
            APrint *pr=aot_print(u,s);
            if(pr){ for(int i=0;i<pr->n;i++) live_uses(L,pr->args[i],live); live_uses(L,pr->sep,live); live_uses(L,pr->end,live); return; }
            if(aot_is_annotation_only(u,s)) return;
            live_uses(L,s->expr,live); return; }
        case STMT_RETURN:
            memset(live,0,sizeof(unsigned)*(size_t)L->W);
            *live_slot(L->tab,s)=bs_dup(L,live);
            live_uses(L,s->expr,live); return;
        case STMT_YIELD: *live_slot(L->tab,s)=bs_dup(L,live); live_uses(L,s->expr,live); return;
        case STMT_RAISE: memset(live,0,sizeof(unsigned)*(size_t)L->W); live_uses(L,s->expr,live); return;
        case STMT_ASSERT: case STMT_DEL:
            if(s->kind==STMT_DEL){ ADel *d=aot_del(u,s); for(int i=0;i<d->n;i++) live_uses(L,d->t[i],live); }
            else { live_uses(L,s->expr,live); live_uses(L,s->expr2,live); }
            return;
        case STMT_IF:{
            unsigned *el=bs_dup(L,live);
            live_stmts(L,f,s->orelse,s->orelse_count,el); live_stmts(L,f,s->body,s->body_count,live);
            bs_or(L,live,el); free(el); live_uses(L,s->expr,live); return; }
        case STMT_WHILE: case STMT_FOR:{
            unsigned *after=bs_dup(L,live), *head=bs_new(L);
            AVar *vars[16]; int nv=0;
            if(s->kind==STMT_FOR) for(int k=0;k<s->param_count && k<16;k++) vars[nv++]=find_var(f,s->params[k],s->line);
            for(int round=0;round<64;round++){
                if(L->nloop<64){ L->brk[L->nloop]=after; L->cont[L->nloop]=head; } L->nloop++;
                unsigned *bd=bs_dup(L,head); live_stmts(L,f,s->body,s->body_count,bd);
                for(int k=0;k<nv;k++) live_kill(L,vars[k],bd);
                unsigned *el=bs_dup(L,after); live_stmts(L,f,s->orelse,s->orelse_count,el);
                L->nloop--;
                bs_or(L,bd,el); if(s->kind==STMT_WHILE) live_uses(L,s->expr,bd);
                int same=bs_eq(L,bd,head); free(el); free(head); head=bd;
                if(same) break;
            }
            memcpy(live,head,sizeof(unsigned)*(size_t)L->W);
            if(s->kind==STMT_FOR) live_uses(L,s->expr,live);
            free(head); free(after); return; }
        case STMT_BREAK: if(L->nloop>0 && L->nloop<=64) memcpy(live,L->brk[L->nloop-1],sizeof(unsigned)*(size_t)L->W); return;
        case STMT_CONTINUE: if(L->nloop>0 && L->nloop<=64) memcpy(live,L->cont[L->nloop-1],sizeof(unsigned)*(size_t)L->W); return;
        case STMT_TRY:{                     /* no moves inside; before it: what the body, the handlers and finally read */
            unsigned *fin=bs_dup(L,live), *all=bs_new(L);
            Stmt *finb=NULL, *elb=NULL;
            for(int i=0;i<s->orelse_count;i++){ Stmt *b=s->orelse[i]; if(b->block_tag==3) finb=b; else if(b->block_tag==2) elb=b; }
            if(finb) live_stmts(L,f,finb->body,finb->body_count,fin);
            bs_or(L,all,fin);
            for(int i=0;i<s->orelse_count;i++){ Stmt *b=s->orelse[i]; if(b->block_tag!=1) continue;
                unsigned *h=bs_dup(L,fin); live_stmts(L,f,b->body,b->body_count,h); bs_or(L,all,h); free(h); }
            unsigned *bd=bs_dup(L,fin);
            if(elb) live_stmts(L,f,elb->body,elb->body_count,bd);
            bs_or(L,bd,all); live_stmts(L,f,s->body,s->body_count,bd); bs_or(L,bd,all);
            memcpy(live,bd,sizeof(unsigned)*(size_t)L->W); free(bd); free(fin); free(all); return; }
        case STMT_WITH:{ AWith *w=aot_with(u,s);
            live_stmts(L,f,s->body,s->body_count,live);
            for(int i=0;i<w->n;i++){ if(w->as[i]) live_kill(L,find_var(f,w->as[i],s->line),live); live_uses(L,w->e[i],live); }
            return; }
        case STMT_FUNCTION_DEF:
            if(is_fn_body(f->fn)) live_kill(L,find_var(f,s->name,s->line),live);
            if(s->expr2) live_uses(L,s->expr2,live);
            return;
        default: return;
    }
}
static void live_stmts(Live *L, F *f, Stmt **b, int n, unsigned *live){ for(int i=n-1;i>=0;i--) live_stmt(L,f,b[i],live); }
void plan_moves(F *f){
    AFunc *fn=f->fn;
    if(!fn->def || !fn->nvars) return;
    Live L; memset(&L,0,sizeof L); L.fn=fn; L.u=fn->mod->unit; L.W=(fn->nvars+31)/32;
    f->live=MPY_NEW0(LiveTab); L.tab=f->live;
    unsigned *live=bs_new(&L);
    live_stmts(&L,f,fn->body,fn->nbody,live);
    free(live);
}
/* occurrences of the movable variables in a statement's expressions (2: several, or evaluated repeatedly) */
static void occ_count(F *f, Expr *e, int repeat){
    if(!e) return;
    XInfo *xi=(XInfo*)e->ty;
    if(e->kind==EXPR_NAME){ if(xi && xi->kind==X_VAR){ AVar *v=xi->var; if(v->owner==f->fn && !v->src && !v->global && v->id>=0 && v->id<f->fn->nvars && f->fn->vars[v->id]==v){
            int c=f->occ[v->id]+(repeat?2:1); f->occ[v->id]=(unsigned char)(c>2?2:c); } } return; }
    if(e->kind==EXPR_LAMBDA) return;
    if(e->kind==EXPR_COMPREHENSION){
        for(int i=0;i<e->nclause;i++){ occ_count(f,e->clauses[i].iter,repeat||(i>0)||e->comp_kind=='G'); for(int k=0;k<e->clauses[i].ncond;k++) occ_count(f,e->clauses[i].conds[k],1); }
        occ_count(f,e->a,1); occ_count(f,e->b,1); return; }
    occ_count(f,e->a,repeat); occ_count(f,e->b,repeat); occ_count(f,e->c,repeat); occ_count(f,e->d,repeat);
    for(int i=0;i<e->count;i++) occ_count(f,e->items[i],repeat);
    for(int i=0;i<e->vcount;i++) occ_count(f,e->vals[i],repeat);
    if(e->kind==EXPR_CALL && xi){ occ_count(f,xi->key,1); occ_count(f,xi->rev,repeat); }
}
static void occ_target(F *f, Expr *t){
    if(t->kind==EXPR_NAME) return;
    if(t->kind==EXPR_TUPLE||t->kind==EXPR_LIST){ for(int i=0;i<t->count;i++) occ_target(f,t->items[i]); return; }
    occ_count(f,t->a,0); occ_count(f,t->b,0);
}
/* entering simple statement s: may its variables move? */
void moves_begin(F *f, Stmt *s){
    f->live_after=NULL;
    if(!f->live || f->ntries) return;
    unsigned *la=live_get(f->live,s); if(!la) return;
    AotUnit *u=f->fn->mod->unit;
    if(!f->occ) f->occ=(unsigned char*)xmalloc((size_t)f->fn->nvars+1);
    memset(f->occ,0,(size_t)f->fn->nvars+1);
    switch(s->kind){
        case STMT_ASSIGN:{ AAssign *a=aot_assign(u,s); if(!a->value) return;
            for(int k=0;k<a->ntarget;k++){ occ_target(f,a->target[k]); if(a->aug) occ_count(f,a->target[k],0); }
            occ_count(f,a->value,0); break; }
        case STMT_EXPR:{ APrint *pr=aot_print(u,s);
            if(pr){ for(int i=0;i<pr->n;i++) occ_count(f,pr->args[i],0); occ_count(f,pr->sep,0); occ_count(f,pr->end,0); }
            else if(!aot_is_annotation_only(u,s)) occ_count(f,s->expr,0);
            break; }
        case STMT_RETURN: case STMT_YIELD: occ_count(f,s->expr,0); break;
        default: return;
    }
    f->live_after=la;
}
/* the owned value of x: its reference itself when this is x's last use */
int try_move(F *f, Expr *e){
    if(!f->live_after || e->kind!=EXPR_NAME) return 0;
    XInfo *xi=(XInfo*)e->ty; if(!xi || xi->kind!=X_VAR) return 0;
    AVar *v=xi->var; AFunc *fn=f->fn;
    if(v->owner!=fn || v->src || v->global || v->cell || v->captured || v->borrowed || (v->id<fn->nparams && !v->consumed) || v->id>=fn->nvars || fn->vars[v->id]!=v) return 0;
    if(!is_ptr(v->ty) || f->occ[v->id]!=1 || (f->live_after[v->id>>5]>>(v->id&31))&1) return 0;
    E(f,"mov eax,[ebp%+d]",v->offset); E(f,"and dword [ebp%+d],0",v->offset);   /* moved */
    return 1;
}
/* ---- parameters a function keeps ----
   A function whose top-level statement stores a parameter away for good
   (self.x = x, items[k] = x, xs.append(x), return x) as the parameter's last
   use takes it owned: callers hand temporaries over (no incref there, no
   decref after the call) and increfs move from the callee to the caller.
   Only for functions every call of which is direct (not values, not virtual,
   not operators, coroutines or generators). */
static int has_return(Stmt **b, int n){
    for(int i=0;i<n;i++){ Stmt *s=b[i]; if(s->kind==STMT_RETURN) return 1;
        if(s->kind!=STMT_FUNCTION_DEF && (has_return(s->body,s->body_count)||has_return(s->orelse,s->orelse_count))) return 1; }
    return 0;
}
static int consumes_param(Expr *v, AVar *p){ XInfo *xi=v?(XInfo*)v->ty:NULL; return v && v->kind==EXPR_NAME && xi && xi->kind==X_VAR && xi->var==p; }
void plan_consumed(AFunc *fn){
    if(!fn->def || fn->value_used || fn->ndeco || fn->is_async || fn->is_gen || fn->is_property || fn->overridden || fn->outer) return;
    if(fn->cls){
        if(!strncmp(fn->name,"__",2) && strcmp(fn->name,"__init__")) return;   /* operators, __str__, __iter__ ...: called from the runtime */
        if(fn->cls->base && aot_find_method(fn->cls->base,fn->name)) return;    /* overrides: called through the vtable */
    }
    int any=0; for(int i=0;i<fn->nparams;i++){ AVar *v=fn->params[i]; if(is_ptr(v->ty) && !v->cell && !v->captured && !v->nbind && i!=fn->star && i!=fn->dstar) any=1; }
    if(!any) return;
    F tmp; memset(&tmp,0,sizeof tmp); tmp.fn=fn;
    plan_moves(&tmp);
    AotUnit *u=fn->mod->unit;
    for(int k=0;k<fn->nbody;k++){ Stmt *s=fn->body[k];
        unsigned *la=live_get(tmp.live,s); if(!la) continue;
        for(int i=0;i<fn->nparams;i++){ AVar *p=fn->params[i];
            if(!is_ptr(p->ty) || p->cell || p->captured || p->nbind || i==fn->star || i==fn->dstar || (la[i>>5]>>(i&31))&1) continue;
            if(i==0 && fn->cls && !fn->is_static) continue;                       /* self is always lent */
            int keep=0;
            if(s->kind==STMT_ASSIGN){ AAssign *a=aot_assign(u,s);
                if(a->value && !a->aug && a->ntarget==1 && consumes_param(a->value,p) && (a->target[0]->kind==EXPR_ATTRIBUTE||a->target[0]->kind==EXPR_INDEX)) keep=1; }
            else if(s->kind==STMT_EXPR && !aot_print(u,s) && !aot_is_annotation_only(u,s)){ Expr *e=s->expr; XInfo *xi=(XInfo*)e->ty;
                if(e->kind==EXPR_CALL && xi && xi->kind==X_TMETHOD && !strcmp(xi->name,"append") && e->count==1 && consumes_param(e->items[0],p)) keep=1; }
            else if(s->kind==STMT_RETURN && consumes_param(s->expr,p)) keep=1;
            if(!keep) continue;
            tmp.occ=NULL; tmp.live_after=NULL; moves_begin(&tmp,s);              /* it must be its only occurrence there */
            if(tmp.live_after && tmp.occ[i]==1 && !p->consumed)
                p->consumed=has_return(fn->body,k)?1:2;                              /* 2: always handed on before any exit */
            free(tmp.occ);
        }
    }
}
