/* ========================= Interpreter: compiler =========================
   The full parser's tree (py_ast.h) -> code objects. Scopes as CPython's
   symtable works them out: a function's names are fast locals, the ones an
   inner function uses become cells (the inner one's free variables), class
   bodies keep theirs in the class namespace, `global` / `nonlocal` as
   declared, comprehensions are functions of their own (`:=` binds in the
   function around them), a method that uses super() gets the class's
   __class__ cell. Code: a stack machine (interp.h's I_* instructions);
   try / with run their cleanup on every way out (return, break, continue
   inline, exceptions through a handler block). */

#include "interp.h"

/* ---------------------------------------------------------------- symbol table */
enum { SK_MODULE, SK_CLASS, SK_FUNC, SK_COMP };
enum { F_BOUND=1, F_PARAM=2, F_GLOBAL=4, F_NONLOCAL=8, F_USED=16, F_ANNOT=32 };
enum { S_LOCAL=1, S_CELL, S_FREE, S_GLOBAL_EXPLICIT, S_GLOBAL_IMPLICIT, S_CLASSFREE };
typedef struct { char *name; int flags, scope; } Sym;
typedef struct Scope {
    int kind; PyNode *node; struct Scope *parent;
    Sym *syms; int nsyms, cap;
    struct Scope **kids; int nkids, kcap, next_kid;
    int gen, coro, has_class_cell, uses_class, uses_super;
    char *name, *qualname;
} Scope;

typedef struct C {
    const char *file;
    jmp_buf jb; char *err; int errline;
    Scope *top;
} C;

MPY_NORETURN static void cfail(C *c, int line, const char *fmt, ...){
    char buf[512]; va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof buf,fmt,ap); va_end(ap);
    c->err=xstrdup2(buf); c->errline=line; longjmp(c->jb,1);
}

static Sym *sym_find(Scope *s, const char *name){ for(int i=0;i<s->nsyms;i++) if(!strcmp(s->syms[i].name,name)) return &s->syms[i]; return NULL; }
static Sym *sym_add(Scope *s, const char *name, int flags){
    Sym *y=sym_find(s,name);
    if(!y){
        if(s->nsyms==s->cap){ s->cap=s->cap?s->cap*2:16; s->syms=(Sym*)xrealloc(s->syms,sizeof(Sym)*(size_t)s->cap); }
        y=&s->syms[s->nsyms++]; y->name=xstrdup2(name); y->flags=0; y->scope=0;
    }
    y->flags|=flags;
    return y;
}
static Scope *scope_new(C *c, int kind, PyNode *node, Scope *parent, const char *name){
    (void)c;
    Scope *s=MPY_NEW0(Scope); s->kind=kind; s->node=node; s->parent=parent; s->name=xstrdup2(name);
    if(parent){
        if(parent->nkids==parent->kcap){ parent->kcap=parent->kcap?parent->kcap*2:8; parent->kids=(Scope**)xrealloc(parent->kids,sizeof(Scope*)*(size_t)parent->kcap); }
        parent->kids[parent->nkids++]=s;
        char q[512];
        if(parent->kind==SK_MODULE) snprintf(q,sizeof q,"%s",name);
        else if(parent->kind==SK_CLASS) snprintf(q,sizeof q,"%s.%s",parent->qualname,name);
        else snprintf(q,sizeof q,"%s.<locals>.%s",parent->qualname,name);
        s->qualname=xstrdup2(q);
    } else s->qualname=xstrdup2(name);
    return s;
}

/* type X[T] = v: the value as `lambda T: v` (n->n[2], made once) */
static PyNode *alias_lambda(PyNode *n){
    if(n->n[2]) return n->n[2];
    PyNode *lam=py_node(PK_Lambda,n->line,n->col), *a=py_node(PK_arguments,n->line,n->col);
    for(int i=0;i<n->L[2].n;i++){ PyNode *arg=py_node(PK_arg,n->line,n->col); arg->id[0]=n->L[2].v[i]->id[0]; py_list_add(&a->L[1],arg); }
    lam->n[0]=a; lam->n[1]=n->n[1];
    return n->n[2]=lam;
}
static void st_expr(C *c, Scope *s, PyNode *n);
static void st_stmts(C *c, Scope *s, PyList *l);
static void st_bind_target(C *c, Scope *s, PyNode *t){
    if(!t) return;
    switch(t->kind){
        case PK_Name: sym_add(s,t->id[0],F_BOUND); break;
        case PK_Tuple: case PK_List: for(int i=0;i<t->L[0].n;i++) st_bind_target(c,s,t->L[0].v[i]); break;
        case PK_Starred: st_bind_target(c,s,t->n[0]); break;
        case PK_Attribute: st_expr(c,s,t->n[0]); break;
        case PK_Subscript: st_expr(c,s,t->n[0]); st_expr(c,s,t->n[1]); break;
        default: st_expr(c,s,t); break;
    }
}
static Scope *comp_scope(C *c, Scope *s, PyNode *n, const char *name){
    st_expr(c,s,n->L[0].v[0]->n[1]);                   /* the first iterable: in the scope around */
    Scope *cs=scope_new(c,SK_COMP,n,s,name);
    sym_add(cs,".0",F_BOUND|F_PARAM);
    for(int i=0;i<n->L[0].n;i++){
        PyNode *g=n->L[0].v[i];
        if(i>0) st_expr(c,cs,g->n[1]);
        st_bind_target(c,cs,g->n[0]);
        for(int j=0;j<g->L[0].n;j++) st_expr(c,cs,g->L[0].v[j]);
        if(g->op) cs->coro=1;
    }
    st_expr(c,cs,n->n[0]);
    if(n->kind==PK_DictComp) st_expr(c,cs,n->n[1]);
    if(n->kind==PK_GeneratorExp) cs->gen=1;
    return cs;
}
static void st_pattern(C *c, Scope *s, PyNode *p){
    if(!p) return;
    switch(p->kind){
        case PK_MatchValue: st_expr(c,s,p->n[0]); break;
        case PK_MatchSequence: for(int i=0;i<p->L[0].n;i++) st_pattern(c,s,p->L[0].v[i]); break;
        case PK_MatchMapping: for(int i=0;i<p->L[0].n;i++) st_expr(c,s,p->L[0].v[i]); for(int i=0;i<p->L[1].n;i++) st_pattern(c,s,p->L[1].v[i]); if(p->id[0]) sym_add(s,p->id[0],F_BOUND); break;
        case PK_MatchClass: st_expr(c,s,p->n[0]); for(int i=0;i<p->L[0].n;i++) st_pattern(c,s,p->L[0].v[i]); for(int i=0;i<p->L[2].n;i++) st_pattern(c,s,p->L[2].v[i]); break;
        case PK_MatchStar: if(p->id[0]) sym_add(s,p->id[0],F_BOUND); break;
        case PK_MatchAs: st_pattern(c,s,p->n[0]); if(p->id[0]) sym_add(s,p->id[0],F_BOUND); break;
        case PK_MatchOr: for(int i=0;i<p->L[0].n;i++) st_pattern(c,s,p->L[0].v[i]); break;
        default: break;
    }
}
static void st_type_params(C *c, Scope *s, PyList *tp){ for(int i=0;i<tp->n;i++) sym_add(s,tp->v[i]->id[0],F_BOUND); (void)c; }
static void st_expr(C *c, Scope *s, PyNode *n){
    if(!n) return;
    switch(n->kind){
        case PK_Name:
            if(n->op==CTX_Load){ sym_add(s,n->id[0],F_USED); if(!strcmp(n->id[0],"super") || !strcmp(n->id[0],"__class__")) s->uses_super=1; }
            else sym_add(s,n->id[0],F_BOUND);
            return;
        case PK_NamedExpr:{
            Scope *t=s; while(t->kind==SK_COMP) t=t->parent;
            if(t->kind==SK_CLASS) cfail(c,n->line,"assignment expression within a comprehension cannot be used in a class body");
            sym_add(t,n->n[0]->id[0],F_BOUND);
            if(t!=s) sym_add(s,n->n[0]->id[0],F_USED);
            st_expr(c,s,n->n[1]);
            return; }
        case PK_Lambda:{
            PyNode *a=n->n[0];
            for(int i=0;i<a->L[4].n;i++) st_expr(c,s,a->L[4].v[i]);
            for(int i=0;i<a->L[3].n;i++) if(a->L[3].v[i]) st_expr(c,s,a->L[3].v[i]);
            Scope *fs=scope_new(c,SK_FUNC,n,s,"<lambda>");
            for(int k=0;k<3;k++) for(int i=0;i<a->L[k].n;i++) sym_add(fs,a->L[k].v[i]->id[0],F_BOUND|F_PARAM);
            if(a->n[0]) sym_add(fs,a->n[0]->id[0],F_BOUND|F_PARAM);
            if(a->n[1]) sym_add(fs,a->n[1]->id[0],F_BOUND|F_PARAM);
            st_expr(c,fs,n->n[1]);
            return; }
        case PK_ListComp: comp_scope(c,s,n,"<listcomp>"); return;
        case PK_SetComp: comp_scope(c,s,n,"<setcomp>"); return;
        case PK_DictComp: comp_scope(c,s,n,"<dictcomp>"); return;
        case PK_GeneratorExp: comp_scope(c,s,n,"<genexpr>"); return;
        case PK_Yield: case PK_YieldFrom: s->gen=1; st_expr(c,s,n->n[0]); return;
        case PK_Await: s->coro=1; st_expr(c,s,n->n[0]); return;
        case PK_Dict: for(int i=0;i<n->L[0].n;i++){ st_expr(c,s,n->L[0].v[i]); st_expr(c,s,n->L[1].v[i]); } return;
        default: break;
    }
    for(int i=0;i<4;i++) if(n->n[i]) st_expr(c,s,n->n[i]);
    for(int k=0;k<5;k++) for(int i=0;i<n->L[k].n;i++){
        PyNode *x=n->L[k].v[i];
        if(!x) continue;
        if(x->kind==PK_keyword){ st_expr(c,s,x->n[0]); continue; }
        if(x->kind==PK_comprehension||x->kind==PK_ident) continue;
        if(n->kind==PK_Compare && k==0) continue;      /* (operator nodes) */
        st_expr(c,s,x);
    }
}
static void st_stmt(C *c, Scope *s, PyNode *n){
    switch(n->kind){
        case PK_FunctionDef: case PK_AsyncFunctionDef:{
            for(int i=0;i<n->L[1].n;i++) st_expr(c,s,n->L[1].v[i]);
            sym_add(s,n->id[0],F_BOUND);
            PyNode *a=n->n[0];
            for(int i=0;i<a->L[4].n;i++) st_expr(c,s,a->L[4].v[i]);
            for(int i=0;i<a->L[3].n;i++) if(a->L[3].v[i]) st_expr(c,s,a->L[3].v[i]);
            Scope *fs=scope_new(c,SK_FUNC,n,s,n->id[0]);
            if(n->kind==PK_AsyncFunctionDef) fs->coro=1;
            st_type_params(c,fs,&n->L[2]);
            for(int k=0;k<3;k++) for(int i=0;i<a->L[k].n;i++) sym_add(fs,a->L[k].v[i]->id[0],F_BOUND|F_PARAM);
            if(a->n[0]) sym_add(fs,a->n[0]->id[0],F_BOUND|F_PARAM);
            if(a->n[1]) sym_add(fs,a->n[1]->id[0],F_BOUND|F_PARAM);
            st_stmts(c,fs,&n->L[0]);
            return; }
        case PK_ClassDef:{
            for(int i=0;i<n->L[1].n;i++) st_expr(c,s,n->L[1].v[i]);
            for(int i=0;i<n->L[3].n;i++) st_expr(c,s,n->L[3].v[i]);
            for(int i=0;i<n->L[4].n;i++) st_expr(c,s,n->L[4].v[i]->n[0]);
            sym_add(s,n->id[0],F_BOUND);
            Scope *cs=scope_new(c,SK_CLASS,n,s,n->id[0]);
            st_type_params(c,cs,&n->L[2]);
            st_stmts(c,cs,&n->L[0]);
            return; }
        case PK_Return: st_expr(c,s,n->n[0]); return;
        case PK_Delete: for(int i=0;i<n->L[0].n;i++) st_bind_target(c,s,n->L[0].v[i]); return;
        case PK_Assign: st_expr(c,s,n->n[0]); for(int i=0;i<n->L[0].n;i++) st_bind_target(c,s,n->L[0].v[i]); return;
        case PK_TypeAlias: sym_add(s,n->n[0]->id[0],F_BOUND); st_expr(c,s,alias_lambda(n)); return;
        case PK_AugAssign:
            if(n->n[0]->kind==PK_Name) sym_add(s,n->n[0]->id[0],F_BOUND|F_USED); else st_bind_target(c,s,n->n[0]);
            st_expr(c,s,n->n[1]); return;
        case PK_AnnAssign:
            if(n->n[0]->kind==PK_Name){ if(n->n[2]) sym_add(s,n->n[0]->id[0],F_BOUND); else sym_add(s,n->n[0]->id[0],F_ANNOT); }
            else st_bind_target(c,s,n->n[0]);
            st_expr(c,s,n->n[2]); return;
        case PK_For: case PK_AsyncFor:
            if(n->kind==PK_AsyncFor) s->coro=1;
            st_expr(c,s,n->n[1]); st_bind_target(c,s,n->n[0]); st_stmts(c,s,&n->L[0]); st_stmts(c,s,&n->L[1]); return;
        case PK_While: case PK_If: st_expr(c,s,n->n[0]); st_stmts(c,s,&n->L[0]); st_stmts(c,s,&n->L[1]); return;
        case PK_With: case PK_AsyncWith:
            if(n->kind==PK_AsyncWith) s->coro=1;
            for(int i=0;i<n->L[3].n;i++){ st_expr(c,s,n->L[3].v[i]->n[0]); st_bind_target(c,s,n->L[3].v[i]->n[1]); }
            st_stmts(c,s,&n->L[0]); return;
        case PK_Match:
            st_expr(c,s,n->n[0]);
            for(int i=0;i<n->L[3].n;i++){ PyNode *mc=n->L[3].v[i]; st_pattern(c,s,mc->n[0]); st_expr(c,s,mc->n[1]); st_stmts(c,s,&mc->L[0]); }
            return;
        case PK_Raise: st_expr(c,s,n->n[0]); st_expr(c,s,n->n[1]); return;
        case PK_Try: case PK_TryStar:
            st_stmts(c,s,&n->L[0]);
            for(int i=0;i<n->L[3].n;i++){ PyNode *h=n->L[3].v[i]; st_expr(c,s,h->n[0]); if(h->id[0]) sym_add(s,h->id[0],F_BOUND); st_stmts(c,s,&h->L[0]); }
            st_stmts(c,s,&n->L[1]); st_stmts(c,s,&n->L[2]); return;
        case PK_Assert: st_expr(c,s,n->n[0]); st_expr(c,s,n->n[1]); return;
        case PK_Import:
            for(int i=0;i<n->L[3].n;i++){ PyNode *al=n->L[3].v[i];
                if(al->id[1]) sym_add(s,al->id[1],F_BOUND);
                else { char top[256]; snprintf(top,sizeof top,"%s",al->id[0]); char *d=strchr(top,'.'); if(d) *d=0; sym_add(s,top,F_BOUND); } }
            return;
        case PK_ImportFrom:
            for(int i=0;i<n->L[3].n;i++){ PyNode *al=n->L[3].v[i]; if(strcmp(al->id[0],"*")) sym_add(s,al->id[1]?al->id[1]:al->id[0],F_BOUND); }
            return;
        case PK_Global: case PK_Nonlocal:
            for(int i=0;i<n->L[3].n;i++){
                const char *nm=n->L[3].v[i]->id[0];
                Sym *y=sym_find(s,nm);
                if(y && (y->flags&(F_BOUND|F_USED)) && !(y->flags&(F_GLOBAL|F_NONLOCAL)))
                    cfail(c,n->line,"name '%s' is %s prior to %s declaration",nm,(y->flags&F_BOUND)?"assigned to":"used",n->kind==PK_Global?"global":"nonlocal");
                if(n->kind==PK_Nonlocal && s->kind==SK_MODULE) cfail(c,n->line,"nonlocal declaration not allowed at module level");
                sym_add(s,nm,n->kind==PK_Global?F_GLOBAL:F_NONLOCAL);
            }
            return;
        case PK_Expr: st_expr(c,s,n->n[0]); return;
        default: return;
    }
}
static void st_stmts(C *c, Scope *s, PyList *l){ for(int i=0;i<l->n;i++) st_stmt(c,s,l->v[i]); }

/* resolution */
/* a free variable of scope s: make every scope up to the one that binds it carry it */
static int resolve_free(C *c, Scope *s, const char *name, int line){
    for(Scope *p=s->parent;p;p=p->parent){
        if(p->kind==SK_MODULE) return 0;
        Sym *y=sym_find(p,name);
        if(p->kind==SK_CLASS){
            if(!strcmp(name,"__class__")){ p->has_class_cell=1; Sym *k=sym_add(p,"__class__",0); k->scope=S_CELL; goto carry; }
            continue;
        }
        if(y && (y->flags&F_GLOBAL)) return 0;
        if(y && (y->flags&F_BOUND) && !(y->flags&F_NONLOCAL)){
            if(y->scope==S_LOCAL || y->scope==0) y->scope=S_CELL;
          carry:
            for(Scope *q=s->parent;q!=p;q=q->parent){ Sym *z=sym_add(q,name,0); if(q->kind==SK_CLASS){ if(!z->scope) z->scope=S_CLASSFREE; } else if(z->scope!=S_CELL) z->scope=S_FREE; }
            return 1;
        }
        if(y && (y->flags&F_NONLOCAL)) continue;    /* resolved there in turn: keep looking up */
    }
    (void)c; (void)line;
    return 0;
}
static void resolve(C *c, Scope *s){
    /* bound names first (so inner scopes can see them), then the free ones */
    for(int i=0;i<s->nsyms;i++){ Sym *y=&s->syms[i];
        if(y->flags&F_GLOBAL) y->scope=S_GLOBAL_EXPLICIT;
        else if(y->flags&F_NONLOCAL) y->scope=0;
        else if(y->flags&F_BOUND) y->scope= s->kind==SK_MODULE ? S_GLOBAL_IMPLICIT : S_LOCAL;
    }
    if(s->uses_super && s->kind!=SK_MODULE && s->kind!=SK_CLASS) sym_add(s,"__class__",F_USED);
    for(int k=0;k<s->nkids;k++) resolve(c,s->kids[k]);
    for(int i=0;i<s->nsyms;i++){ Sym *y=&s->syms[i];
        if(y->flags&F_NONLOCAL){
            if(!resolve_free(c,s,y->name,0)) cfail(c,s->node?s->node->line:0,"no binding for nonlocal '%s' found",y->name);
            y->scope=S_FREE; continue;
        }
        if(y->scope) continue;
        if(s->kind==SK_MODULE){ y->scope=S_GLOBAL_IMPLICIT; continue; }
        if(resolve_free(c,s,y->name,0)) y->scope= s->kind==SK_CLASS ? S_CLASSFREE : S_FREE;
        else y->scope=S_GLOBAL_IMPLICIT;
    }
    if(s->kind==SK_CLASS && s->has_class_cell){ Sym *k=sym_add(s,"__class__",0); k->scope=S_CELL; }
}

/* ---------------------------------------------------------------- code buffers */
typedef struct { int pos, depth; } Label;
enum { FB_FOR, FB_WHILE, FB_TRY, FB_FINALLY, FB_WITH, FB_ASYNC_WITH, FB_HANDLER, FB_ASYNC_FOR };
typedef struct { int kind, lbreak, lcont; PyList *final; const char *name; int depth; } FBlock;
typedef struct G {
    C *c; Scope *sc;
    uint32_t *code; int *lines; int n, cap;
    Value consts; int nconsts;                        /* a list */
    char **names; int nnames, cnames;
    char **vars; int nvars, cvars;
    char **cells; int ncells; char **frees; int nfrees;
    Label *labels; int nlabels, clabels;
    int *jumps; int njumps, cjumps;
    int depth, maxdepth, line, nblocks, maxblocks;
    FBlock fb[64]; int nfb;
    int ret_tmp;
    int star_try;                                     /* try_finally: the handlers are except* ones */
} G;

static int stack_effect(int op, int arg);
static void emit(G *g, int op, int arg){
    if(g->n==g->cap){ g->cap=g->cap?g->cap*2:256; g->code=(uint32_t*)xrealloc(g->code,sizeof(uint32_t)*(size_t)g->cap); g->lines=(int*)xrealloc(g->lines,sizeof(int)*(size_t)g->cap); }
    g->code[g->n]=(uint32_t)op|((uint32_t)arg<<8); g->lines[g->n]=g->line; g->n++;
    g->depth+=stack_effect(op,arg);
    if(g->depth>g->maxdepth) g->maxdepth=g->depth;
    if(g->depth<0) g->depth=0;
}
static int new_label(G *g){
    if(g->nlabels==g->clabels){ g->clabels=g->clabels?g->clabels*2:32; g->labels=(Label*)xrealloc(g->labels,sizeof(Label)*(size_t)g->clabels); }
    g->labels[g->nlabels].pos=-1; g->labels[g->nlabels].depth=-1;
    return g->nlabels++;
}
static void jump_depth(G *g, int l, int d){ if(g->labels[l].depth<d) g->labels[l].depth=d; }
static void emit_jump(G *g, int op, int label){
    if(g->njumps==g->cjumps){ g->cjumps=g->cjumps?g->cjumps*2:64; g->jumps=(int*)xrealloc(g->jumps,sizeof(int)*(size_t)g->cjumps); }
    g->jumps[g->njumps++]=g->n;
    int d=g->depth;
    switch(op){
        case I_JUMP_IF_FALSE: case I_JUMP_IF_TRUE: d=g->depth-1; break;
        case I_FOR_ITER: d=g->depth-1; break;
        case I_SETUP: d=g->depth+1; break;
        case I_SEND: d=g->depth-1; break;
        case I_WITH_ENTER: d=g->depth+1; break;    /* (ctx ->) exit, exc */
        case I_GET_ANEXT: d=g->depth+1; break;
        default: break;
    }
    jump_depth(g,label,d);
    emit(g,op,label);
}
static void place(G *g, int l){
    g->labels[l].pos=g->n;
    if(g->labels[l].depth>=0) g->depth=g->labels[l].depth;
}
static int name_ix(G *g, const char *s){
    for(int i=0;i<g->nnames;i++) if(!strcmp(g->names[i],s)) return i;
    if(g->nnames==g->cnames){ g->cnames=g->cnames?g->cnames*2:16; g->names=(char**)xrealloc(g->names,sizeof(char*)*(size_t)g->cnames); }
    g->names[g->nnames]=xstrdup2(s); return g->nnames++;
}
static int var_ix(G *g, const char *s){
    for(int i=0;i<g->nvars;i++) if(!strcmp(g->vars[i],s)) return i;
    if(g->nvars==g->cvars){ g->cvars=g->cvars?g->cvars*2:16; g->vars=(char**)xrealloc(g->vars,sizeof(char*)*(size_t)g->cvars); }
    g->vars[g->nvars]=xstrdup2(s); return g->nvars++;
}
static int cell_ix(G *g, const char *s){
    for(int i=0;i<g->ncells;i++) if(!strcmp(g->cells[i],s)) return i;
    for(int i=0;i<g->nfrees;i++) if(!strcmp(g->frees[i],s)) return g->ncells+i;
    return -1;
}
static int const_ix(G *g, Value v){
    ListObj *l=AS_LIST(g->consts);
    for(int i=0;i<l->len;i++){
        Value x=l->items[i];
        if(x.k!=v.k) continue;
        if(v.k==V_INT||v.k==V_BOOL){ if(x.u.i==v.u.i) return i; continue; }
        if(v.k==V_NONE) return i;
        if(v.k==V_FLOAT){ if(!memcmp(&x.u.f,&v.u.f,sizeof(double))) return i; continue; }
        if(v.k==V_OBJ && IS(v,T_str) && IS(x,T_str) && !strcmp(AS_STR(x)->s,AS_STR(v)->s) && AS_STR(x)->len==AS_STR(v)->len) return i;
    }
    mp_list_append(g->consts,v);
    return (int)l->len-1;
}
static void load_const(G *g, Value v){ emit(g,I_CONST,const_ix(g,v)); }

static int stack_effect(int op, int arg){
    switch(op){
        case I_NOP: return 0;
        case I_POP: return -1;
        case I_DUP: return 1; case I_DUP2: return 2;
        case I_ROT2: case I_ROT3: case I_ROT4: case I_SWAP: return 0;
        case I_COPY: return 1;
        case I_CONST: case I_NONE: return 1;
        case I_LOAD_FAST: case I_LOAD_DEREF: case I_LOAD_GLOBAL: case I_LOAD_NAME: case I_LOAD_CLASSDEREF: return 1;
        case I_STORE_FAST: case I_STORE_DEREF: case I_STORE_GLOBAL: case I_STORE_NAME: return -1;
        case I_DEL_FAST: case I_DEL_DEREF: case I_DEL_GLOBAL: case I_DEL_NAME: return 0;
        case I_LOAD_ATTR: return 0;
        case I_STORE_ATTR: return -2;
        case I_DEL_ATTR: return -1;
        case I_LOAD_METHOD: return 1;
        case I_CALL_METHOD: return -(arg&0xffff)-1-((arg>>16)&1);
        case I_SUBSCR: return -1;
        case I_STORE_SUBSCR: return -3;
        case I_DEL_SUBSCR: return -2;
        case I_BINOP: case I_INPLACE: case I_COMPARE: return -1;
        case I_UNARY: case I_NOT: case I_TRUTH: return 0;
        case I_JUMP: return 0;
        case I_JUMP_IF_FALSE: case I_JUMP_IF_TRUE: return -1;
        case I_JUMP_IF_FALSE_KEEP: case I_JUMP_IF_TRUE_KEEP: return 0;
        case I_BUILD_TUPLE: case I_BUILD_LIST: case I_BUILD_STRING: return 1-arg;
        case I_BUILD_SET: return 1-(arg&0xFFFF);
        case I_BUILD_DICT: return 1-2*arg;
        case I_BUILD_SLICE: return 1-arg;
        case I_LIST_APPEND: case I_LIST_EXTEND: case I_SET_ADD: case I_SET_UPDATE: case I_DICT_UPDATE: return -1;
        case I_DICT_SET: return -2;
        case I_LIST_TO_TUPLE: return 0;
        case I_FORMAT: return (arg&0x100)?-1:0;
        case I_UNPACK: return arg-1;
        case I_UNPACK_EX: return (arg&0xff)+(arg>>8);
        case I_GET_ITER: return 0;
        case I_FOR_ITER: return 1;
        case I_CALL: return -arg;
        case I_CALL_KW: return -arg-1;
        case I_CALL_EX: return (arg&1)?-2:-1;
        case I_RETURN: return -1;
        case I_MAKE_FUNCTION: return -((arg&1)+((arg>>1)&1)+((arg>>2)&1));
        case I_MAKE_CLASS: return -(arg&0xffff)-1-((arg>>16)&1);
        case I_IMPORT: return 1;
        case I_IMPORT_FROM: return 1;
        case I_IMPORT_STAR: return -1;
        case I_SETUP: case I_POP_BLOCK: return 0;
        case I_RAISE: return -arg;
        case I_RERAISE: return -1;
        case I_PUSH_EXC: case I_POP_EXC: return 0;
        case I_EXC_MATCH: return -1;
        case I_YIELD: return 0;
        case I_GET_YIELD_FROM_ITER: case I_GET_AWAITABLE: return 0;
        case I_SEND: return 0;
        case I_GET_AITER: return 0;
        case I_GET_ANEXT: return 1;
        case I_END_ASYNC_FOR: return -2;
        case I_ASSERT_FAIL: return -arg;
        case I_WITH_ENTER: case I_ASYNC_WITH_ENTER: return 1;
        case I_WITH_EXIT: return arg==0?-1:arg==1?-2:arg==3?-2:0;
        case I_LOAD_CELL: return 1;
        case I_PRINT_EXPR: return -1;
        case I_MATCH_CLASS: return -2;
        case I_MATCH_SEQ: case I_MATCH_MAP: return 1;
        case I_MATCH_KEYS: return 1+arg;
        case I_SETUP_ANNOTATIONS: return 0;
        case I_LOAD_LOCALS: return 1;
        case I_CALL_INTRINSIC: return arg==3?-1:0;
        default: return 0;
    }
}

/* ---------------------------------------------------------------- names */
static Sym *lookup(G *g, const char *name){ return sym_find(g->sc,name); }
static void name_op(G *g, const char *name, int what /* 0 load, 1 store, 2 delete */){
    Scope *s=g->sc; Sym *y=lookup(g,name);
    int scope= y ? y->scope : (s->kind==SK_MODULE ? S_GLOBAL_IMPLICIT : S_GLOBAL_IMPLICIT);
    if(s->kind==SK_MODULE){
        emit(g,what==0?I_LOAD_GLOBAL:what==1?I_STORE_GLOBAL:I_DEL_GLOBAL,name_ix(g,name)); return;
    }
    if(s->kind==SK_CLASS){
        if(scope==S_GLOBAL_EXPLICIT){ emit(g,what==0?I_LOAD_GLOBAL:what==1?I_STORE_GLOBAL:I_DEL_GLOBAL,name_ix(g,name)); return; }
        if((scope==S_CLASSFREE || scope==S_FREE) && what==0 && cell_ix(g,name)>=0){ emit(g,I_LOAD_CLASSDEREF,cell_ix(g,name)); return; }
        if(scope==S_CELL && what==0 && strcmp(name,"__class__")){ emit(g,I_LOAD_CLASSDEREF,cell_ix(g,name)); return; }
        emit(g,what==0?I_LOAD_NAME:what==1?I_STORE_NAME:I_DEL_NAME,name_ix(g,name)); return;
    }
    switch(scope){
        case S_LOCAL: emit(g,what==0?I_LOAD_FAST:what==1?I_STORE_FAST:I_DEL_FAST,var_ix(g,name)); return;
        case S_CELL: case S_FREE: emit(g,what==0?I_LOAD_DEREF:what==1?I_STORE_DEREF:I_DEL_DEREF,cell_ix(g,name)); return;
        default: emit(g,what==0?I_LOAD_GLOBAL:what==1?I_STORE_GLOBAL:I_DEL_GLOBAL,name_ix(g,name)); return;
    }
}

/* ---------------------------------------------------------------- expressions */
static void ex(G *g, PyNode *n);
/* an expression CPython's optimizer turns into a constant: a set display of
   three or more of them is built from a frozenset constant (its own order) */
static int folds_to_const(PyNode *n){
    switch(n->kind){
        case PK_Constant: return n->k->kind!=PC_Ellipsis;
        case PK_UnaryOp: return n->op!=OP_Not && folds_to_const(n->n[0]);
        case PK_BinOp: return n->op!=OP_MatMult && folds_to_const(n->n[0]) && folds_to_const(n->n[1]);
        case PK_Tuple: for(int i=0;i<n->L[0].n;i++) if(!folds_to_const(n->L[0].v[i])) return 0; return 1;
        default: return 0;
    }
}
static void store(G *g, PyNode *t);
static void stmts(G *g, PyList *l);
static Value make_code(C *c, Scope *s);

static Value const_of(G *g, PyNode *n){
    PyConst *k=n->k;
    switch(k->kind){
        case PC_None: return v_none();
        case PC_True: return v_bool(1);
        case PC_False: return v_bool(0);
        case PC_Ellipsis: return mp_Ellipsis;
        case PC_Int:{
            const char *s=k->text; int base=10;
            if(s[0]=='0' && (s[1]=='x'||s[1]=='X')){ base=16; s+=2; } else if(s[0]=='0' && (s[1]=='o'||s[1]=='O')){ base=8; s+=2; } else if(s[0]=='0' && (s[1]=='b'||s[1]=='B')){ base=2; s+=2; }
            uint64_t v=0;
            for(;*s;s++){ int d= *s>='0'&&*s<='9' ? *s-'0' : *s>='a'&&*s<='f' ? *s-'a'+10 : *s>='A'&&*s<='F' ? *s-'A'+10 : 99; if(d>=base) break;
                if(v>(UINT64_MAX-(uint64_t)d)/(uint64_t)base) cfail(g->c,n->line,"integer literal too large: ints are 64-bit");
                v=v*(uint64_t)base+(uint64_t)d; }
            if(v>(uint64_t)INT64_MAX) cfail(g->c,n->line,"integer literal too large: ints are 64-bit");
            return v_int((int64_t)v); }
        case PC_Float: return v_float(strtod(k->text,NULL));
        case PC_Complex: return mp_complex(0,strtod(k->text,NULL));
        case PC_Str:{ SBuf b={0}; for(int i=0;i<k->ulen;i++){ char t[4]; int m=mp_utf8_encode(t,k->u[i]); sb_put(&b,t,m); } Value v=mp_strn(b.s?b.s:"",b.n); free(b.s); return v; }
        case PC_Bytes: return mp_bytes(k->b,k->blen);
    }
    return v_none();
}
static int has_starred(PyList *l){ for(int i=0;i<l->n;i++) if(l->v[i] && l->v[i]->kind==PK_Starred) return 1; return 0; }
/* [a, *b, c] onto the stack as a list */
static void starred_list(G *g, PyList *l){
    emit(g,I_BUILD_LIST,0);
    for(int i=0;i<l->n;i++){ PyNode *x=l->v[i];
        if(x->kind==PK_Starred){ ex(g,x->n[0]); emit(g,I_LIST_EXTEND,1); }
        else { ex(g,x); emit(g,I_LIST_APPEND,1); }
    }
}
static void call(G *g, PyNode *n){
    PyNode *f=n->n[0]; PyList *args=&n->L[0], *kws=&n->L[1];
    int splat=has_starred(args); for(int i=0;i<kws->n;i++) if(!kws->v[i]->id[0]) splat=1;
    if(splat){
        ex(g,f);
        starred_list(g,args); emit(g,I_LIST_TO_TUPLE,0);
        if(kws->n){
            emit(g,I_BUILD_DICT,0);
            for(int i=0;i<kws->n;i++){ PyNode *k=kws->v[i];
                if(k->id[0]){ load_const(g,mp_intern(k->id[0])); ex(g,k->n[0]); emit(g,I_DICT_SET,1); }
                else { ex(g,k->n[0]); emit(g,I_DICT_UPDATE,1|0x100); }
            }
        }
        emit(g,I_CALL_EX,kws->n?1:0);
        return;
    }
    int nargs=args->n+kws->n;
    Value kwn=v_undef();
    if(kws->n){ kwn=mp_tuple(kws->n,NULL); for(int i=0;i<kws->n;i++) AS_TUPLE(kwn)->items[i]=mp_intern(kws->v[i]->id[0]); }
    if(f->kind==PK_Attribute){
        ex(g,f->n[0]); int save=g->line; g->line=f->line; emit(g,I_LOAD_METHOD,name_ix(g,f->id[0])); g->line=save;
        for(int i=0;i<args->n;i++) ex(g,args->v[i]);
        for(int i=0;i<kws->n;i++) ex(g,kws->v[i]->n[0]);
        if(kws->n) load_const(g,kwn);
        g->line=n->line;
        emit(g,I_CALL_METHOD,nargs|(kws->n?1<<16:0));
        return;
    }
    ex(g,f);
    for(int i=0;i<args->n;i++) ex(g,args->v[i]);
    for(int i=0;i<kws->n;i++) ex(g,kws->v[i]->n[0]);
    g->line=n->line;
    if(kws->n){ load_const(g,kwn); emit(g,I_CALL_KW,nargs); }
    else emit(g,I_CALL,nargs);
}
static void closure_of(G *g, Scope *child){
    /* the cells the child's free variables come from */
    int n=0;
    for(int i=0;i<child->nsyms;i++){ Sym *y=&child->syms[i];
        if(y->scope==S_FREE || (child->kind==SK_CLASS && y->scope==S_CLASSFREE)){
            int ix=cell_ix(g,y->name);
            if(ix<0) cfail(g->c,child->node?child->node->line:0,"internal error: no cell for '%s'",y->name);
            emit(g,I_LOAD_CELL,ix); n++;
        }
    }
    if(n) emit(g,I_BUILD_TUPLE,n);
}
static int closure_needed(Scope *child){
    for(int i=0;i<child->nsyms;i++) if(child->syms[i].scope==S_FREE || (child->kind==SK_CLASS && child->syms[i].scope==S_CLASSFREE)) return 1;
    return 0;
}
static Scope *next_kid(G *g){
    Scope *s=g->sc;
    if(s->next_kid>=s->nkids) cfail(g->c,g->line,"internal error: scopes out of step");
    return s->kids[s->next_kid++];
}
/* defaults, keyword-only defaults, closure, code -> a function on the stack (the child scope: the next one) */
static void make_function(G *g, PyNode *args){
    int flags=0;
    if(args && args->L[4].n){ for(int i=0;i<args->L[4].n;i++) ex(g,args->L[4].v[i]); emit(g,I_BUILD_TUPLE,args->L[4].n); flags|=1; }
    if(args){ int nk=0;
        for(int i=0;i<args->L[3].n;i++) if(args->L[3].v[i]){ load_const(g,mp_intern(args->L[2].v[i]->id[0])); ex(g,args->L[3].v[i]); nk++; }
        if(nk){ emit(g,I_BUILD_DICT,nk); flags|=2; } }
    Scope *child=next_kid(g);
    if(closure_needed(child)){ closure_of(g,child); flags|=4; }
    Value code=make_code(g->c,child);
    load_const(g,code);
    emit(g,I_MAKE_FUNCTION,flags);
}
static void yield_from_loop(G *g);
static void comprehension(G *g, PyNode *n){
    ex(g,n->L[0].v[0]->n[1]);
    emit(g,n->L[0].v[0]->op?I_GET_AITER:I_GET_ITER,0);
    Scope *cs=g->sc->kids[g->sc->next_kid];
    make_function(g,NULL);
    emit(g,I_ROT2,0);
    emit(g,I_CALL,1);
    if(cs->coro && n->kind!=PK_GeneratorExp){ emit(g,I_GET_AWAITABLE,0); yield_from_loop(g); }
}
static void yield_from_loop(G *g){
    emit(g,I_NONE,0);
    int l=new_label(g), e=new_label(g);
    place(g,l);
    emit_jump(g,I_SEND,e);
    emit(g,I_YIELD,1);
    emit_jump(g,I_JUMP,l);
    place(g,e);
}
static void fstring_part(G *g, PyNode *v){
    if(v->kind==PK_Constant){ load_const(g,const_of(g,v)); return; }
    ex(g,v->n[0]);
    int conv= v->op<0 ? 0 : v->op;
    if(v->n[1]){ ex(g,v->n[1]); emit(g,I_FORMAT,conv|0x100); }
    else emit(g,I_FORMAT,conv);
}
static void ex(G *g, PyNode *n){
    int save=g->line; if(n->line) g->line=n->line;
    switch(n->kind){
        case PK_Constant: if(n->k->kind==PC_None) emit(g,I_NONE,0); else load_const(g,const_of(g,n)); break;
        case PK_Name: name_op(g,n->id[0],0); break;
        case PK_BinOp: ex(g,n->n[0]); ex(g,n->n[1]); g->line=n->line; emit(g,I_BINOP,n->op); break;
        case PK_UnaryOp: ex(g,n->n[0]); if(n->op==OP_Not) emit(g,I_NOT,0); else emit(g,I_UNARY,n->op); break;
        case PK_BoolOp:{
            int end=new_label(g);
            for(int i=0;i<n->L[0].n;i++){
                ex(g,n->L[0].v[i]);
                if(i<n->L[0].n-1){ emit_jump(g,n->op==OP_And?I_JUMP_IF_FALSE_KEEP:I_JUMP_IF_TRUE_KEEP,end); emit(g,I_POP,0); }
            }
            place(g,end); break; }
        case PK_Compare:{
            ex(g,n->n[0]);
            int nops=n->L[1].n;
            if(nops==1){ ex(g,n->L[1].v[0]); g->line=n->line; emit(g,I_COMPARE,n->L[0].v[0]->op); break; }
            int cleanup=new_label(g), end=new_label(g);
            for(int i=0;i<nops;i++){
                ex(g,n->L[1].v[i]);
                if(i<nops-1){ emit(g,I_DUP,0); emit(g,I_ROT3,0); emit(g,I_COMPARE,n->L[0].v[i]->op); emit_jump(g,I_JUMP_IF_FALSE_KEEP,cleanup); emit(g,I_POP,0); }
                else emit(g,I_COMPARE,n->L[0].v[i]->op);
            }
            emit_jump(g,I_JUMP,end);
            place(g,cleanup); g->depth+=1; emit(g,I_ROT2,0); emit(g,I_POP,0);
            place(g,end); break; }
        case PK_IfExp:{
            int el=new_label(g), end=new_label(g);
            ex(g,n->n[0]); emit_jump(g,I_JUMP_IF_FALSE,el);
            ex(g,n->n[1]); emit_jump(g,I_JUMP,end);
            place(g,el); g->depth--; ex(g,n->n[2]);
            place(g,end); break; }
        case PK_Call: call(g,n); break;
        case PK_Attribute: ex(g,n->n[0]); g->line=n->line; emit(g,I_LOAD_ATTR,name_ix(g,n->id[0])); break;
        case PK_Subscript:
            ex(g,n->n[0]);
            if(n->n[1]->kind==PK_Slice){ PyNode *s=n->n[1];
                if(s->n[0]) ex(g,s->n[0]); else emit(g,I_NONE,0);
                if(s->n[1]) ex(g,s->n[1]); else emit(g,I_NONE,0);
                if(s->n[2]){ ex(g,s->n[2]); emit(g,I_BUILD_SLICE,3); } else emit(g,I_BUILD_SLICE,2);
            } else ex(g,n->n[1]);
            g->line=n->line; emit(g,I_SUBSCR,0); break;
        case PK_Slice:
            if(n->n[0]) ex(g,n->n[0]); else emit(g,I_NONE,0);
            if(n->n[1]) ex(g,n->n[1]); else emit(g,I_NONE,0);
            if(n->n[2]){ ex(g,n->n[2]); emit(g,I_BUILD_SLICE,3); } else emit(g,I_BUILD_SLICE,2);
            break;
        case PK_List:
            if(has_starred(&n->L[0])) starred_list(g,&n->L[0]);
            else { for(int i=0;i<n->L[0].n;i++) ex(g,n->L[0].v[i]); emit(g,I_BUILD_LIST,n->L[0].n); }
            break;
        case PK_Tuple:
            if(has_starred(&n->L[0])){ starred_list(g,&n->L[0]); emit(g,I_LIST_TO_TUPLE,0); }
            else { for(int i=0;i<n->L[0].n;i++) ex(g,n->L[0].v[i]); emit(g,I_BUILD_TUPLE,n->L[0].n); }
            break;
        case PK_Set:
            if(has_starred(&n->L[0])){
                emit(g,I_BUILD_SET,0);
                for(int i=0;i<n->L[0].n;i++){ PyNode *x=n->L[0].v[i]; if(x->kind==PK_Starred){ ex(g,x->n[0]); emit(g,I_SET_UPDATE,1); } else { ex(g,x); emit(g,I_SET_ADD,1); } }
            } else {
                int consts=n->L[0].n>=3;
                for(int i=0;i<n->L[0].n;i++){ ex(g,n->L[0].v[i]); if(!folds_to_const(n->L[0].v[i])) consts=0; }
                emit(g,I_BUILD_SET,n->L[0].n|(consts?0x10000:0));
            }
            break;
        case PK_Dict:{
            int spl=0; for(int i=0;i<n->L[0].n;i++) if(!n->L[0].v[i]) spl=1;
            if(!spl){ for(int i=0;i<n->L[0].n;i++){ ex(g,n->L[0].v[i]); ex(g,n->L[1].v[i]); } emit(g,I_BUILD_DICT,n->L[0].n); break; }
            emit(g,I_BUILD_DICT,0);
            for(int i=0;i<n->L[0].n;i++){
                if(n->L[0].v[i]){ ex(g,n->L[0].v[i]); ex(g,n->L[1].v[i]); emit(g,I_DICT_SET,1); }
                else { ex(g,n->L[1].v[i]); emit(g,I_DICT_UPDATE,1); }
            }
            break; }
        case PK_ListComp: case PK_SetComp: case PK_DictComp: case PK_GeneratorExp: comprehension(g,n); break;
        case PK_Lambda: make_function(g,n->n[0]); break;
        case PK_NamedExpr: ex(g,n->n[1]); emit(g,I_DUP,0); name_op(g,n->n[0]->id[0],1); break;
        case PK_Yield:
            if(g->sc->kind==SK_COMP && g->sc->node->kind!=PK_GeneratorExp) cfail(g->c,n->line,"'yield' inside list comprehension");
            if(n->n[0]) ex(g,n->n[0]); else emit(g,I_NONE,0);
            emit(g,I_YIELD,0); break;
        case PK_YieldFrom:
            if(g->sc->coro) cfail(g->c,n->line,"'yield from' inside async function");
            ex(g,n->n[0]); emit(g,I_GET_YIELD_FROM_ITER,0); yield_from_loop(g); break;
        case PK_Await: ex(g,n->n[0]); emit(g,I_GET_AWAITABLE,0); yield_from_loop(g); break;
        case PK_JoinedStr:
            if(n->L[0].n==1 && n->L[0].v[0]->kind==PK_Constant){ load_const(g,const_of(g,n->L[0].v[0])); break; }
            for(int i=0;i<n->L[0].n;i++) fstring_part(g,n->L[0].v[i]);
            emit(g,I_BUILD_STRING,n->L[0].n);
            break;
        case PK_FormattedValue: fstring_part(g,n); break;
        case PK_Starred: cfail(g->c,n->line,"can't use starred expression here");
        case PK_TemplateStr:                          /* t"...": __mpy_tstr__(str, (value, expression, conversion, spec), ...) */
            name_op(g,"__mpy_tstr__",0);
            for(int i=0;i<n->L[0].n;i++){ PyNode *v=n->L[0].v[i];
                if(v->kind!=PK_Interpolation){ ex(g,v); continue; }
                ex(g,v->n[0]);
                { SBuf b={0}; if(v->k) for(int k=0;k<v->k->ulen;k++){ uint32_t cp=v->k->u[k]; char o[4]; int m=mp_utf8_encode(o,cp); sb_put(&b,o,m); }
                  load_const(g,sb_value(&b)); }
                if(v->op>0){ char cv[2]={(char)v->op,0}; load_const(g,mp_str(cv)); } else emit(g,I_NONE,0);
                if(v->n[1]) ex(g,v->n[1]); else load_const(g,mp_str(""));
                emit(g,I_BUILD_TUPLE,4); }
            g->line=n->line; emit(g,I_CALL,n->L[0].n);
            break;
        default: cfail(g->c,n->line,"unsupported expression %s",py_kind_name(n->kind));
    }
    g->line=save;
}

/* ---------------------------------------------------------------- assignment targets */
static void store(G *g, PyNode *t){
    switch(t->kind){
        case PK_Name: name_op(g,t->id[0],1); break;
        case PK_Attribute: ex(g,t->n[0]); g->line=t->line; emit(g,I_STORE_ATTR,name_ix(g,t->id[0])); break;
        case PK_Subscript:
            ex(g,t->n[0]);
            if(t->n[1]->kind==PK_Slice) ex(g,t->n[1]); else ex(g,t->n[1]);
            g->line=t->line; emit(g,I_STORE_SUBSCR,0); break;
        case PK_Tuple: case PK_List:{
            PyList *l=&t->L[0]; int star=-1;
            for(int i=0;i<l->n;i++) if(l->v[i]->kind==PK_Starred){ if(star>=0) cfail(g->c,t->line,"multiple starred expressions in assignment"); star=i; }
            if(star<0) emit(g,I_UNPACK,l->n);
            else emit(g,I_UNPACK_EX,star|((l->n-star-1)<<8));
            for(int i=0;i<l->n;i++) store(g,l->v[i]->kind==PK_Starred ? l->v[i]->n[0] : l->v[i]);
            break; }
        case PK_Starred: cfail(g->c,t->line,"starred assignment target must be in a list or tuple");
        default: cfail(g->c,t->line,"cannot assign to %s",py_kind_name(t->kind));
    }
}
static void delete_target(G *g, PyNode *t){
    switch(t->kind){
        case PK_Name: name_op(g,t->id[0],2); break;
        case PK_Attribute: ex(g,t->n[0]); emit(g,I_DEL_ATTR,name_ix(g,t->id[0])); break;
        case PK_Subscript: ex(g,t->n[0]); ex(g,t->n[1]); emit(g,I_DEL_SUBSCR,0); break;
        case PK_Tuple: case PK_List: for(int i=0;i<t->L[0].n;i++) delete_target(g,t->L[0].v[i]); break;
        default: cfail(g->c,t->line,"cannot delete %s",py_kind_name(t->kind));
    }
}

/* ---------------------------------------------------------------- statements */
static void push_fb(G *g, int kind, int lbreak, int lcont, PyList *final, const char *name){
    if(g->nfb==64) cfail(g->c,g->line,"too many statically nested blocks");
    FBlock *f=&g->fb[g->nfb++]; f->kind=kind; f->lbreak=lbreak; f->lcont=lcont; f->final=final; f->name=name; f->depth=g->depth;
}
static void setup(G *g, int label){ emit_jump(g,I_SETUP,label); g->nblocks++; if(g->nblocks>g->maxblocks) g->maxblocks=g->nblocks; }
static void pop_block(G *g){ emit(g,I_POP_BLOCK,0); g->nblocks--; }
/* leave fblock i (0 = outermost) on the way out: its cleanup */
static void unwind_one(G *g, int i, int for_return){
    FBlock *f=&g->fb[i];
    switch(f->kind){
        case FB_FOR: emit(g,I_POP,1); break;                     /* (1: a generator the loop owns is closed, as CPython frees it) */
        case FB_ASYNC_FOR: emit(g,I_POP,0); break;
        case FB_WHILE: break;
        case FB_TRY: emit(g,I_POP_BLOCK,0); break;
        case FB_FINALLY:{
            emit(g,I_POP_BLOCK,0);
            FBlock save=*f; int nfb=g->nfb;
            g->nfb=i;                                   /* the finally body runs outside its own block */
            int k=g->sc->next_kid; g->sc->next_kid=save.depth;   /* (its scopes again) */
            stmts(g,save.final);
            g->sc->next_kid=k;
            g->nfb=nfb; g->fb[i]=save;
            break; }
        case FB_WITH: emit(g,I_POP_BLOCK,0); emit(g,I_WITH_EXIT,0); break;
        case FB_ASYNC_WITH: emit(g,I_POP_BLOCK,0); emit(g,I_WITH_EXIT,4); emit(g,I_GET_AWAITABLE,0); yield_from_loop(g); emit(g,I_POP,0); break;
        case FB_HANDLER:
            emit(g,I_POP_EXC,0);
            if(f->name){ emit(g,I_NONE,0); name_op(g,f->name,1); name_op(g,f->name,2); }
            break;
    }
    (void)for_return;
}
static void st(G *g, PyNode *n);
static void stmts(G *g, PyList *l){ for(int i=0;i<l->n;i++) st(g,l->v[i]); }

static void try_except(G *g, PyNode *n){
    int handler=new_label(g), end=new_label(g), orelse=new_label(g);
    setup(g,handler);
    push_fb(g,FB_TRY,-1,-1,NULL,NULL);
    stmts(g,&n->L[0]);
    g->nfb--;
    pop_block(g);
    emit_jump(g,I_JUMP,orelse);
    place(g,handler);                             /* [exc] */
    emit(g,I_PUSH_EXC,0);
    for(int i=0;i<n->L[3].n;i++){
        PyNode *h=n->L[3].v[i];
        g->line=h->line;
        int next=new_label(g);
        if(h->n[0]){
            emit(g,I_DUP,0); ex(g,h->n[0]); emit(g,I_EXC_MATCH,0);
            emit_jump(g,I_JUMP_IF_FALSE,next);
        } else if(i<n->L[3].n-1) cfail(g->c,h->line,"default 'except:' must be last");
        if(h->id[0]) name_op(g,h->id[0],1); else emit(g,I_POP,0);
        push_fb(g,FB_HANDLER,-1,-1,NULL,h->id[0]);
        stmts(g,&h->L[0]);
        g->nfb--;
        emit(g,I_POP_EXC,0);
        if(h->id[0]){ emit(g,I_NONE,0); name_op(g,h->id[0],1); name_op(g,h->id[0],2); }
        emit_jump(g,I_JUMP,end);
        place(g,next);
        if(h->n[0]) g->depth=g->labels[handler].depth;
    }
    emit(g,I_RERAISE,0);
    place(g,orelse);
    stmts(g,&n->L[1]);
    place(g,end);
}
/* the scope cursor where the scopes of a try statement's finally body start */
static int count_kids_list(PyList *l);
static int count_kids(PyNode *n){
    if(!n) return 0;
    switch(n->kind){
        case PK_FunctionDef: case PK_AsyncFunctionDef:{ int k=0; for(int i=0;i<n->L[1].n;i++) k+=count_kids(n->L[1].v[i]);
            PyNode *a=n->n[0]; for(int i=0;i<a->L[4].n;i++) k+=count_kids(a->L[4].v[i]); for(int i=0;i<a->L[3].n;i++) k+=count_kids(a->L[3].v[i]); return k+1; }
        case PK_ClassDef:{ int k=0; for(int i=0;i<n->L[1].n;i++) k+=count_kids(n->L[1].v[i]); for(int i=0;i<n->L[3].n;i++) k+=count_kids(n->L[3].v[i]);
            for(int i=0;i<n->L[4].n;i++) k+=count_kids(n->L[4].v[i]->n[0]); return k+1; }
        case PK_Lambda:{ int k=0; PyNode *a=n->n[0]; for(int i=0;i<a->L[4].n;i++) k+=count_kids(a->L[4].v[i]); for(int i=0;i<a->L[3].n;i++) k+=count_kids(a->L[3].v[i]); return k+1; }
        case PK_ListComp: case PK_SetComp: case PK_DictComp: case PK_GeneratorExp: return count_kids(n->L[0].v[0]->n[1])+1;
        case PK_AnnAssign: return count_kids(n->n[2])+count_kids(n->n[0]);
        default: break;
    }
    int k=0;
    for(int i=0;i<4;i++) k+=count_kids(n->n[i]);
    for(int j=0;j<5;j++) for(int i=0;i<n->L[j].n;i++){ PyNode *x=n->L[j].v[i]; if(x && x->kind!=PK_ident && !(n->kind==PK_Compare && j==0)) k+=count_kids(x); }
    return k;
}
static int count_kids_list(PyList *l){ int k=0; for(int i=0;i<l->n;i++) k+=count_kids(l->v[i]); return k; }
/* try: body except* T [as e]: handler ...: each clause takes the part of the group it matches
   (a lone exception is a group of one); what is left, and what handlers raise, is raised after */
static void try_star(G *g, PyNode *n){
    int handler=new_label(g), end=new_label(g), orelse=new_label(g);
    setup(g,handler);
    push_fb(g,FB_TRY,-1,-1,NULL,NULL);
    stmts(g,&n->L[0]);
    g->nfb--;
    pop_block(g);
    emit_jump(g,I_JUMP,orelse);
    place(g,handler);                             /* [exc] */
    emit(g,I_PUSH_EXC,0);
    emit(g,I_CALL_INTRINSIC,1);                   /* [state] */
    for(int i=0;i<n->L[3].n;i++){
        PyNode *h=n->L[3].v[i];
        g->line=h->line;
        int skip=new_label(g), raised=new_label(g), done=new_label(g);
        if(!h->n[0]) cfail(g->c,h->line,"expected one or more exception types");
        emit(g,I_DUP,0); ex(g,h->n[0]); emit(g,I_CALL_INTRINSIC,2);   /* [state, state, matched or None] */
        emit(g,I_ROT2,0); emit(g,I_POP,0);       /* [state, matched or None] */
        emit(g,I_DUP,0); emit(g,I_NONE,0); emit(g,I_COMPARE,OP_Is); emit_jump(g,I_JUMP_IF_TRUE,skip);
        emit(g,I_PUSH_EXC,0);                     /* the matched group: being handled */
        if(h->id[0]) name_op(g,h->id[0],1); else emit(g,I_POP,0);
        setup(g,raised);                          /* what the handler raises */
        push_fb(g,FB_HANDLER,-1,-1,NULL,h->id[0]);
        stmts(g,&h->L[0]);
        g->nfb--;
        pop_block(g);
        emit(g,I_POP_EXC,0);
        if(h->id[0]){ emit(g,I_NONE,0); name_op(g,h->id[0],1); name_op(g,h->id[0],2); }
        emit_jump(g,I_JUMP,done);
        place(g,raised);                          /* [state, x] */
        emit(g,I_POP_EXC,0);
        emit(g,I_CALL_INTRINSIC,3);               /* [state] */
        if(h->id[0]){ emit(g,I_NONE,0); name_op(g,h->id[0],1); name_op(g,h->id[0],2); }
        emit_jump(g,I_JUMP,done);
        place(g,skip);                            /* [state, None] */
        emit(g,I_POP,0);
        place(g,done);
    }
    emit(g,I_CALL_INTRINSIC,4);                   /* raises what is left; [state] */
    emit(g,I_POP,0);
    emit(g,I_POP_EXC,0);
    emit_jump(g,I_JUMP,end);
    place(g,orelse);
    stmts(g,&n->L[1]);
    place(g,end);
}
static void try_finally(G *g, PyNode *n, int has_handlers){
    int handler=new_label(g), end=new_label(g);
    /* where the finally body's scopes start: after the body's, handlers' and else's */
    int fin_kids=g->sc->next_kid+count_kids_list(&n->L[0])+count_kids_list(&n->L[1]);
    for(int i=0;i<n->L[3].n;i++){ fin_kids+=count_kids(n->L[3].v[i]->n[0]); fin_kids+=count_kids_list(&n->L[3].v[i]->L[0]); }
    setup(g,handler);
    push_fb(g,FB_FINALLY,-1,-1,&n->L[2],NULL);
    g->fb[g->nfb-1].depth=fin_kids;
    if(has_handlers){ if(g->star_try){ g->star_try=0; try_star(g,n); } else try_except(g,n); }
    else stmts(g,&n->L[0]);
    g->nfb--;
    pop_block(g);
    stmts(g,&n->L[2]);
    int after=g->sc->next_kid;
    emit_jump(g,I_JUMP,end);
    place(g,handler);                             /* [exc] */
    emit(g,I_PUSH_EXC,0);
    g->sc->next_kid=fin_kids;
    stmts(g,&n->L[2]);
    g->sc->next_kid=after;
    emit(g,I_RERAISE,0);
    place(g,end);
}
static void with_items(G *g, PyNode *n, int item, int async){
    if(item==n->L[3].n){ stmts(g,&n->L[0]); return; }
    PyNode *w=n->L[3].v[item];
    int exc=new_label(g), end=new_label(g);
    ex(g,w->n[0]);
    g->line=n->line;
    if(!async){
        emit_jump(g,I_WITH_ENTER,exc); g->nblocks++; if(g->nblocks>g->maxblocks) g->maxblocks=g->nblocks;
        if(w->n[1]) store(g,w->n[1]); else emit(g,I_POP,0);
    } else {
        emit(g,I_ASYNC_WITH_ENTER,0);              /* [aexit, awaitable] */
        emit(g,I_GET_AWAITABLE,0); yield_from_loop(g);
        if(w->n[1]) store(g,w->n[1]); else emit(g,I_POP,0);
        setup(g,exc);                              /* [aexit]; on exception: [aexit, exc] */
    }
    push_fb(g,async?FB_ASYNC_WITH:FB_WITH,-1,-1,NULL,NULL);
    with_items(g,n,item+1,async);
    g->nfb--;
    pop_block(g);
    if(!async) emit(g,I_WITH_EXIT,0);
    else { emit(g,I_WITH_EXIT,4); emit(g,I_GET_AWAITABLE,0); yield_from_loop(g); emit(g,I_POP,0); }
    emit_jump(g,I_JUMP,end);
    place(g,exc);                                 /* [exit, exc] */
    emit(g,I_PUSH_EXC,0);
    if(!async) emit(g,I_WITH_EXIT,1);
    else { emit(g,I_WITH_EXIT,2); emit(g,I_GET_AWAITABLE,0); yield_from_loop(g); emit(g,I_WITH_EXIT,3); }
    place(g,end);
}

/* ---- match */
static void pattern(G *g, PyNode *p, int fail);
/* subject on the stack; -> nothing on the stack, bindings made, or a jump to fail (subject popped) */
static void pattern(G *g, PyNode *p, int fail){
    switch(p->kind){
        case PK_MatchAs:
            if(!p->n[0]){ if(p->id[0]) name_op(g,p->id[0],1); else emit(g,I_POP,0); return; }
            emit(g,I_DUP,0); pattern(g,p->n[0],fail);
            if(p->id[0]) name_op(g,p->id[0],1); else emit(g,I_POP,0);
            return;
        case PK_MatchValue:{
            int ok=new_label(g);
            emit(g,I_DUP,0); ex(g,p->n[0]); emit(g,I_COMPARE,OP_Eq);
            emit_jump(g,I_JUMP_IF_TRUE,ok);
            emit(g,I_POP,0); emit_jump(g,I_JUMP,fail);
            place(g,ok); g->depth=g->labels[ok].depth;
            emit(g,I_POP,0); return; }
        case PK_MatchSingleton:{
            int ok=new_label(g);
            emit(g,I_DUP,0);
            PyNode fake; memset(&fake,0,sizeof fake); fake.kind=PK_Constant; fake.k=p->k;
            if(p->k->kind==PC_None) emit(g,I_NONE,0); else load_const(g,const_of(g,&fake));
            emit(g,I_COMPARE,OP_Is);
            emit_jump(g,I_JUMP_IF_TRUE,ok);
            emit(g,I_POP,0); emit_jump(g,I_JUMP,fail);
            place(g,ok); emit(g,I_POP,0); return; }
        case PK_MatchOr:{
            int ok=new_label(g);
            for(int i=0;i<p->L[0].n;i++){
                int next=new_label(g);
                emit(g,I_DUP,0);
                pattern(g,p->L[0].v[i],next);
                emit_jump(g,I_JUMP,ok);
                place(g,next); g->depth=g->labels[ok].depth>=0?g->labels[ok].depth:g->depth;
            }
            emit(g,I_POP,0); emit_jump(g,I_JUMP,fail);
            place(g,ok); emit(g,I_POP,0); return; }
        case PK_MatchSequence:{
            PyList *ps=&p->L[0]; int star=-1;
            for(int i=0;i<ps->n;i++) if(ps->v[i]->kind==PK_MatchStar) star=i;
            /* MATCH_SEQ: [subj] -> [subj, list-or-None] with the length checked */
            int nmin= star<0 ? ps->n : ps->n-1;
            emit(g,I_MATCH_SEQ,nmin|(star<0?0:0x10000));
            int okl=new_label(g);
            emit(g,I_DUP,0); emit(g,I_NONE,0); emit(g,I_COMPARE,OP_IsNot);
            emit_jump(g,I_JUMP_IF_TRUE,okl);
            emit(g,I_POP,0); emit(g,I_POP,0); emit_jump(g,I_JUMP,fail);
            place(g,okl);
            emit(g,I_ROT2,0); emit(g,I_POP,0);                     /* [list] */
            if(star<0) emit(g,I_UNPACK,ps->n);
            else emit(g,I_UNPACK_EX,star|((ps->n-star-1)<<8));
            /* each item: on failure the remaining ones must go too */
            int nitems=ps->n;
            for(int i=0;i<nitems;i++){
                int sub=new_label(g), cont=new_label(g);
                PyNode *q=ps->v[i];
                if(q->kind==PK_MatchStar){ if(q->id[0]) name_op(g,q->id[0],1); else emit(g,I_POP,0); continue; }
                pattern(g,q,sub);
                emit_jump(g,I_JUMP,cont);
                place(g,sub); g->depth-=0;
                for(int k=i+1;k<nitems;k++) emit(g,I_POP,0);
                emit_jump(g,I_JUMP,fail);
                place(g,cont);
            }
            return; }
        case PK_MatchMapping:{
            /* MATCH_MAP: [subj] -> [subj, bool]; MATCH_KEYS: [subj, keys] -> [subj, values-tuple-or-None] */
            int okl=new_label(g);
            emit(g,I_MATCH_MAP,0);
            emit_jump(g,I_JUMP_IF_TRUE,okl);
            emit(g,I_POP,0); emit_jump(g,I_JUMP,fail);
            place(g,okl);
            int nk=p->L[0].n;
            for(int i=0;i<nk;i++) ex(g,p->L[0].v[i]);
            emit(g,I_BUILD_TUPLE,nk);
            emit(g,I_MATCH_KEYS,p->id[0]?1:0);                     /* [subj, keys, values|None (, rest)] */
            int ok2=new_label(g);
            if(p->id[0]){ emit(g,I_ROT2,0); }                    /* [subj, keys, rest, values] -> values on top */
            emit(g,I_DUP,0); emit(g,I_NONE,0); emit(g,I_COMPARE,OP_IsNot);
            emit_jump(g,I_JUMP_IF_TRUE,ok2);
            emit(g,I_POP,0); if(p->id[0]) emit(g,I_POP,0); emit(g,I_POP,0); emit(g,I_POP,0); emit_jump(g,I_JUMP,fail);
            place(g,ok2);
            emit(g,I_UNPACK,nk);
            for(int i=0;i<nk;i++){
                int sub=new_label(g), cont=new_label(g);
                pattern(g,p->L[1].v[i],sub);
                emit_jump(g,I_JUMP,cont);
                place(g,sub);
                for(int k=i+1;k<nk;k++) emit(g,I_POP,0);
                if(p->id[0]) emit(g,I_POP,0);
                emit(g,I_POP,0); emit(g,I_POP,0);
                emit_jump(g,I_JUMP,fail);
                place(g,cont);
            }
            if(p->id[0]) name_op(g,p->id[0],1);
            emit(g,I_POP,0); emit(g,I_POP,0);                     /* keys, subject */
            return; }
        case PK_MatchClass:{
            /* MATCH_CLASS n: [subj, cls, kwnames] -> [subj, attrs-tuple-or-None] */
            int npos=p->L[0].n, nkw=p->L[2].n;
            ex(g,p->n[0]);
            Value kn=mp_tuple(nkw,NULL); for(int i=0;i<nkw;i++) AS_TUPLE(kn)->items[i]=mp_intern(p->L[1].v[i]->id[0]);
            emit(g,I_DUP2,0); emit(g,I_POP,0);                    /* [subj, cls, subj] -> keep subj under */
            emit(g,I_ROT2,0);                                     /* [subj, subj, cls] */
            load_const(g,kn);
            emit(g,I_MATCH_CLASS,npos);                           /* [subj, attrs|None] */
            int okl=new_label(g);
            emit(g,I_DUP,0); emit(g,I_NONE,0); emit(g,I_COMPARE,OP_IsNot);
            emit_jump(g,I_JUMP_IF_TRUE,okl);
            emit(g,I_POP,0); emit(g,I_POP,0); emit_jump(g,I_JUMP,fail);
            place(g,okl);
            int n=npos+nkw;
            emit(g,I_UNPACK,n);
            for(int i=0;i<n;i++){
                int sub=new_label(g), cont=new_label(g);
                pattern(g,i<npos?p->L[0].v[i]:p->L[2].v[i-npos],sub);
                emit_jump(g,I_JUMP,cont);
                place(g,sub);
                for(int k=i+1;k<n;k++) emit(g,I_POP,0);
                emit(g,I_POP,0);
                emit_jump(g,I_JUMP,fail);
                place(g,cont);
            }
            emit(g,I_POP,0);
            return; }
        case PK_MatchStar: if(p->id[0]) name_op(g,p->id[0],1); else emit(g,I_POP,0); return;
        default: cfail(g->c,p->line,"unsupported pattern");
    }
}
static void match_stmt(G *g, PyNode *n){
    int end=new_label(g);
    ex(g,n->n[0]);
    int base=g->depth;
    for(int i=0;i<n->L[3].n;i++){
        PyNode *mc=n->L[3].v[i];
        g->line=mc->line;
        int next=new_label(g);
        int last= i==n->L[3].n-1;
        emit(g,I_DUP,0);
        pattern(g,mc->n[0],next);
        if(mc->n[1]){ ex(g,mc->n[1]); emit_jump(g,I_JUMP_IF_FALSE,next); }
        emit(g,I_POP,0);
        stmts(g,&mc->L[0]);
        emit_jump(g,I_JUMP,end);
        place(g,next); g->depth=base;
        (void)last;
    }
    emit(g,I_POP,0);
    place(g,end); g->depth=base-1;
}

static char *ann_text(PyNode *a);
static void st(G *g, PyNode *n){
    g->line=n->line;
    switch(n->kind){
        case PK_Expr:
            if(n->n[0]->kind==PK_Constant) break;     /* (docstrings and other bare constants) */
            ex(g,n->n[0]); emit(g,I_POP,0); break;
        case PK_Assign:
            ex(g,n->n[0]);
            for(int i=0;i<n->L[0].n;i++){ if(i<n->L[0].n-1) emit(g,I_DUP,0); store(g,n->L[0].v[i]); }
            break;
        case PK_AugAssign:{
            PyNode *t=n->n[0];
            if(t->kind==PK_Name){ name_op(g,t->id[0],0); ex(g,n->n[1]); g->line=n->line; emit(g,I_INPLACE,n->op); name_op(g,t->id[0],1); }
            else if(t->kind==PK_Attribute){
                ex(g,t->n[0]); emit(g,I_DUP,0); emit(g,I_LOAD_ATTR,name_ix(g,t->id[0]));
                ex(g,n->n[1]); g->line=n->line; emit(g,I_INPLACE,n->op);
                emit(g,I_ROT2,0); emit(g,I_STORE_ATTR,name_ix(g,t->id[0]));
            } else if(t->kind==PK_Subscript){
                ex(g,t->n[0]); ex(g,t->n[1]); emit(g,I_DUP2,0); emit(g,I_SUBSCR,0);
                ex(g,n->n[1]); g->line=n->line; emit(g,I_INPLACE,n->op);
                emit(g,I_ROT3,0); emit(g,I_STORE_SUBSCR,0);
            } else cfail(g->c,n->line,"illegal expression for augmented assignment");
            break; }
        case PK_AnnAssign:
            if(n->n[2]){ ex(g,n->n[2]); store(g,n->n[0]); }
            if(n->n[0]->kind==PK_Name && n->op && (g->sc->kind==SK_MODULE || g->sc->kind==SK_CLASS)){
                char *t=ann_text(n->n[1]);
                load_const(g,mp_str(t)); free(t);
                emit(g,g->sc->kind==SK_MODULE?I_LOAD_GLOBAL:I_LOAD_NAME,name_ix(g,"__annotations__"));
                load_const(g,mp_intern(n->n[0]->id[0]));
                emit(g,I_STORE_SUBSCR,0);
            }
            break;
        case PK_Delete: for(int i=0;i<n->L[0].n;i++) delete_target(g,n->L[0].v[i]); break;
        case PK_Pass: break;
        case PK_If:{
            int el=new_label(g), end=new_label(g);
            ex(g,n->n[0]); emit_jump(g,I_JUMP_IF_FALSE,el);
            stmts(g,&n->L[0]);
            if(n->L[1].n){ emit_jump(g,I_JUMP,end); place(g,el); stmts(g,&n->L[1]); place(g,end); }
            else place(g,el);
            break; }
        case PK_While:{
            int top=new_label(g), exit_=new_label(g), brk=new_label(g);
            place(g,top);
            int always= n->n[0]->kind==PK_Constant && n->n[0]->k->kind==PC_True;
            if(!always){ ex(g,n->n[0]); emit_jump(g,I_JUMP_IF_FALSE,exit_); }
            push_fb(g,FB_WHILE,brk,top,NULL,NULL);
            stmts(g,&n->L[0]);
            g->nfb--;
            emit_jump(g,I_JUMP,top);
            place(g,exit_);
            stmts(g,&n->L[1]);
            place(g,brk);
            break; }
        case PK_For:{
            int top=new_label(g), exit_=new_label(g), brk=new_label(g);
            ex(g,n->n[1]); emit(g,I_GET_ITER,n->n[1]->kind==PK_Call);     /* (1: a generator made here belongs to the loop) */
            place(g,top);
            g->line=n->line;
            emit_jump(g,I_FOR_ITER,exit_);
            store(g,n->n[0]);
            push_fb(g,FB_FOR,brk,top,NULL,NULL);
            stmts(g,&n->L[0]);
            g->nfb--;
            emit_jump(g,I_JUMP,top);
            place(g,exit_);
            stmts(g,&n->L[1]);
            place(g,brk);
            break; }
        case PK_AsyncFor:{
            int top=new_label(g), done=new_label(g), brk=new_label(g), after=new_label(g);
            ex(g,n->n[1]); emit(g,I_GET_AITER,0);
            place(g,top);
            g->line=n->line;
            setup(g,done);                             /* StopAsyncIteration: the end */
            emit(g,I_GET_ANEXT,0); emit(g,I_GET_AWAITABLE,0); yield_from_loop(g);
            pop_block(g);
            store(g,n->n[0]);
            push_fb(g,FB_ASYNC_FOR,brk,top,NULL,NULL);
            stmts(g,&n->L[0]);
            g->nfb--;
            emit_jump(g,I_JUMP,top);
            place(g,done);                             /* [aiter, exc] */
            emit(g,I_END_ASYNC_FOR,0);
            stmts(g,&n->L[1]);
            emit_jump(g,I_JUMP,after);
            place(g,brk);
            place(g,after);
            break; }
        case PK_Break: case PK_Continue:{
            int i=g->nfb-1;
            while(i>=0 && g->fb[i].kind!=FB_FOR && g->fb[i].kind!=FB_WHILE && g->fb[i].kind!=FB_ASYNC_FOR) i--;
            if(i<0) cfail(g->c,n->line,n->kind==PK_Break?"'break' outside loop":"'continue' not properly in loop");
            int save=g->depth;
            for(int k=g->nfb-1;k>i;k--) unwind_one(g,k,0);
            if(n->kind==PK_Break){ if(g->fb[i].kind!=FB_WHILE) emit(g,I_POP,g->fb[i].kind==FB_FOR); emit_jump(g,I_JUMP,g->fb[i].lbreak); }
            else emit_jump(g,I_JUMP,g->fb[i].lcont);
            g->depth=save;
            break; }
        case PK_Return:{
            if(g->sc->kind!=SK_FUNC && g->sc->kind!=SK_COMP) cfail(g->c,n->line,"'return' outside function");
            int save=g->depth;
            if(n->n[0]) ex(g,n->n[0]); else emit(g,I_NONE,0);
            if(g->nfb){
                int tmp=var_ix(g,".ret");
                emit(g,I_STORE_FAST,tmp);
                for(int k=g->nfb-1;k>=0;k--) unwind_one(g,k,1);
                emit(g,I_LOAD_FAST,tmp);
            }
            emit(g,I_RETURN,0);
            g->depth=save;
            break; }
        case PK_Raise:
            if(!n->n[0]) emit(g,I_RAISE,0);
            else { ex(g,n->n[0]); if(n->n[1]){ ex(g,n->n[1]); emit(g,I_RAISE,2); } else emit(g,I_RAISE,1); }
            break;
        case PK_Try:
            if(n->L[2].n) try_finally(g,n,n->L[3].n>0);
            else try_except(g,n);
            break;
        case PK_TryStar:
            if(n->L[2].n){ g->star_try=1; try_finally(g,n,1); break; }
            try_star(g,n);
            break;
        case PK_Assert:{
            int ok=new_label(g);
            ex(g,n->n[0]); emit_jump(g,I_JUMP_IF_TRUE,ok);
            if(n->n[1]){ ex(g,n->n[1]); emit(g,I_ASSERT_FAIL,1); } else emit(g,I_ASSERT_FAIL,0);
            place(g,ok); break; }
        case PK_With: with_items(g,n,0,0); break;
        case PK_AsyncWith: with_items(g,n,0,1); break;
        case PK_Match: match_stmt(g,n); break;
        case PK_Import:
            for(int i=0;i<n->L[3].n;i++){ PyNode *al=n->L[3].v[i];
                Value spec[3]={mp_str(al->id[0]),v_int(0),v_bool(al->id[1]!=NULL)};
                emit(g,I_IMPORT,const_ix(g,mp_tuple(3,spec)));
                if(al->id[1]) name_op(g,al->id[1],1);
                else { char top[256]; snprintf(top,sizeof top,"%s",al->id[0]); char *d=strchr(top,'.'); if(d) *d=0; name_op(g,top,1); }
            }
            break;
        case PK_ImportFrom:{
            Value spec[3]={mp_str(n->id[0]?n->id[0]:""),v_int(n->op),v_bool(1)};
            emit(g,I_IMPORT,const_ix(g,mp_tuple(3,spec)));
            if(n->L[3].n==1 && !strcmp(n->L[3].v[0]->id[0],"*")){
                if(g->sc->kind!=SK_MODULE) cfail(g->c,n->line,"import * only allowed at module level");
                emit(g,I_IMPORT_STAR,0); break; }
            for(int i=0;i<n->L[3].n;i++){ PyNode *al=n->L[3].v[i];
                emit(g,I_IMPORT_FROM,name_ix(g,al->id[0]));
                name_op(g,al->id[1]?al->id[1]:al->id[0],1);
            }
            emit(g,I_POP,0);
            break; }
        case PK_Global: case PK_Nonlocal: break;
        case PK_FunctionDef: case PK_AsyncFunctionDef:{
            for(int i=0;i<n->L[1].n;i++) ex(g,n->L[1].v[i]);
            g->line=n->line;
            make_function(g,n->n[0]);
            for(int i=0;i<n->L[1].n;i++) emit(g,I_CALL,1);
            name_op(g,n->id[0],1);
            break; }
        case PK_ClassDef:{
            for(int i=0;i<n->L[1].n;i++) ex(g,n->L[1].v[i]);
            g->line=n->line;
            load_const(g,mp_str(n->id[0]));
            for(int i=0;i<n->L[3].n;i++) ex(g,n->L[3].v[i]);
            int kw=0;
            if(n->L[4].n){ for(int i=0;i<n->L[4].n;i++){ PyNode *k=n->L[4].v[i]; if(!k->id[0]) cfail(g->c,n->line,"**kwargs in a class statement is not supported"); load_const(g,mp_intern(k->id[0])); ex(g,k->n[0]); } emit(g,I_BUILD_DICT,n->L[4].n); kw=1; }
            g->line=n->line;
            make_function(g,NULL);                     /* the body: [name, bases..., (keywords), body] */
            emit(g,I_MAKE_CLASS,n->L[3].n|(kw<<16));
            for(int i=0;i<n->L[1].n;i++) emit(g,I_CALL,1);
            name_op(g,n->id[0],1);
            break; }
        case PK_TypeAlias:{                   /* name = __typealias__("name", lambda T...: value, ("T", ...)): lazily, as CPython */
            name_op(g,"__typealias__",0);
            load_const(g,mp_str(n->n[0]->id[0]));
            ex(g,alias_lambda(n));
            for(int i=0;i<n->L[2].n;i++) load_const(g,mp_str(n->L[2].v[i]->id[0]));
            emit(g,I_BUILD_TUPLE,n->L[2].n);
            emit(g,I_CALL,3);
            name_op(g,n->n[0]->id[0],1); break; }
        default: cfail(g->c,n->line,"unsupported statement %s",py_kind_name(n->kind));
    }
}

/* ---------------------------------------------------------------- annotations as text */
static void ann_into(SBuf *b, PyNode *a){
    if(!a){ sb_puts(b,"None"); return; }
    switch(a->kind){
        case PK_Name: sb_puts(b,a->id[0]); return;
        case PK_Attribute: ann_into(b,a->n[0]); sb_putc(b,'.'); sb_puts(b,a->id[0]); return;
        case PK_Subscript: ann_into(b,a->n[0]); sb_putc(b,'['); ann_into(b,a->n[1]); sb_putc(b,']'); return;
        case PK_Tuple: for(int i=0;i<a->L[0].n;i++){ if(i) sb_puts(b,", "); ann_into(b,a->L[0].v[i]); } return;
        case PK_List: sb_putc(b,'['); for(int i=0;i<a->L[0].n;i++){ if(i) sb_puts(b,", "); ann_into(b,a->L[0].v[i]); } sb_putc(b,']'); return;
        case PK_BinOp: ann_into(b,a->n[0]); sb_puts(b," | "); ann_into(b,a->n[1]); return;
        case PK_Constant:
            if(a->k->kind==PC_None){ sb_puts(b,"None"); return; }
            if(a->k->kind==PC_Ellipsis){ sb_puts(b,"..."); return; }
            if(a->k->kind==PC_Str){ sb_putc(b,'\''); for(int i=0;i<a->k->ulen;i++){ char t[4]; int m=mp_utf8_encode(t,a->k->u[i]); sb_put(b,t,m); } sb_putc(b,'\''); return; }
            sb_puts(b,a->k->text?a->k->text:"?"); return;
        default: sb_puts(b,"..."); return;
    }
}
static char *ann_text(PyNode *a){ SBuf b={0}; ann_into(&b,a); if(!b.s) return xstrdup2(""); return b.s; }
/* the type an annotation names for minipy.endpoint: its first name other than Optional / Union / typing */
static const char *first_type_name(PyNode *a){
    if(!a) return NULL;
    switch(a->kind){
        case PK_Name: return strcmp(a->id[0],"Optional") && strcmp(a->id[0],"Union") && strcmp(a->id[0],"typing") ? a->id[0] : NULL;
        case PK_Attribute:{ const char *r=first_type_name(a->n[0]); if(r) return r; return strcmp(a->id[0],"Optional") && strcmp(a->id[0],"Union") ? a->id[0] : NULL; }
        case PK_Subscript: case PK_BinOp:{ const char *r=first_type_name(a->n[0]); return r?r:first_type_name(a->n[1]); }
        case PK_Tuple: for(int i=0;i<a->L[0].n;i++){ const char *r=first_type_name(a->L[0].v[i]); if(r) return r; } return NULL;
        default: return NULL;
    }
}

/* ---------------------------------------------------------------- code objects */
static void comp_body(G *g, PyNode *n, int gi, int depth_items){
    PyList *gens=&n->L[0];
    PyNode *gen=gens->v[gi];
    int top=new_label(g), exit_=new_label(g);
    if(gi==0) emit(g,I_LOAD_FAST,var_ix(g,".0"));
    else { ex(g,gen->n[1]); emit(g,gen->op?I_GET_AITER:I_GET_ITER,0); }
    place(g,top);
    int done=-1;
    if(gen->op){ done=new_label(g); setup(g,done); emit(g,I_GET_ANEXT,0); emit(g,I_GET_AWAITABLE,0); yield_from_loop(g); pop_block(g); }
    else emit_jump(g,I_FOR_ITER,exit_);
    store(g,gen->n[0]);
    for(int i=0;i<gen->L[0].n;i++){ ex(g,gen->L[0].v[i]); emit_jump(g,I_JUMP_IF_FALSE,top); }
    if(gi+1<gens->n) comp_body(g,n,gi+1,depth_items+1);
    else switch(n->kind){
        case PK_ListComp: ex(g,n->n[0]); emit(g,I_LIST_APPEND,depth_items+2); break;
        case PK_SetComp: ex(g,n->n[0]); emit(g,I_SET_ADD,depth_items+2); break;
        case PK_DictComp: ex(g,n->n[0]); ex(g,n->n[1]); emit(g,I_DICT_SET,depth_items+2); break;
        default: ex(g,n->n[0]); emit(g,I_YIELD,0); emit(g,I_POP,0); break;
    }
    emit_jump(g,I_JUMP,top);
    if(gen->op){ place(g,done); emit(g,I_END_ASYNC_FOR,0); }
    else place(g,exit_);
}
static Value make_code(C *c, Scope *s){
    G G0; memset(&G0,0,sizeof G0); G *g=&G0;
    g->c=c; g->sc=s; g->consts=mp_list(0,NULL);
    s->next_kid=0;
    PyNode *n=s->node;
    CodeObj *co=(CodeObj*)mp_alloc(T_code,sizeof(CodeObj));
    Value cov=v_obj(co);
    Value keep_list=g->consts;
    /* the parameters */
    PyNode *args= (s->kind==SK_FUNC && n) ? n->n[0] : NULL;
    int flags=0;
    if(args){
        for(int i=0;i<args->L[0].n;i++) var_ix(g,args->L[0].v[i]->id[0]);
        for(int i=0;i<args->L[1].n;i++) var_ix(g,args->L[1].v[i]->id[0]);
        for(int i=0;i<args->L[2].n;i++) var_ix(g,args->L[2].v[i]->id[0]);
        if(args->n[0]){ var_ix(g,args->n[0]->id[0]); flags|=CO_VARARGS; }
        if(args->n[1]){ var_ix(g,args->n[1]->id[0]); flags|=CO_VARKW; }
        co->argc=args->L[0].n+args->L[1].n; co->posonly=args->L[0].n; co->kwonly=args->L[2].n;
    } else if(s->kind==SK_COMP){ var_ix(g,".0"); co->argc=1; }
    /* cells and free variables */
    for(int i=0;i<s->nsyms;i++){ Sym *y=&s->syms[i];
        if(y->scope==S_CELL){ g->cells=(char**)xrealloc(g->cells,sizeof(char*)*(size_t)(g->ncells+1)); g->cells[g->ncells++]=y->name; }
    }
    for(int i=0;i<s->nsyms;i++){ Sym *y=&s->syms[i];
        if(y->scope==S_FREE || (s->kind==SK_CLASS && y->scope==S_CLASSFREE)){ g->frees=(char**)xrealloc(g->frees,sizeof(char*)*(size_t)(g->nfrees+1)); g->frees[g->nfrees++]=y->name; }
    }
    if(s->gen && s->coro) flags|=CO_ASYNCGEN; else if(s->gen) flags|=CO_GEN; else if(s->coro) flags|=CO_CORO;
    if(s->kind==SK_CLASS) flags|=CO_CLASS;
    if(s->kind==SK_MODULE) flags|=CO_MODULE;
    if(s->kind==SK_COMP) flags|=CO_COMP;
    co->doc=v_none();
    g->line= n ? n->line : 1;
    /* the body */
    if(s->kind==SK_MODULE || (s->kind==SK_FUNC && n->kind!=PK_Lambda) || s->kind==SK_CLASS){
        PyList *body=&n->L[0];
        int has_doc= body->n && body->v[0]->kind==PK_Expr && body->v[0]->n[0]->kind==PK_Constant && body->v[0]->n[0]->k->kind==PC_Str;
        if(has_doc) co->doc=const_of(g,body->v[0]->n[0]);
        if(s->kind==SK_CLASS){
            emit(g,I_LOAD_GLOBAL,name_ix(g,"__name__")); emit(g,I_STORE_NAME,name_ix(g,"__module__"));
            load_const(g,mp_str(s->qualname)); emit(g,I_STORE_NAME,name_ix(g,"__qualname__"));
            if(has_doc){ load_const(g,co->doc); emit(g,I_STORE_NAME,name_ix(g,"__doc__")); }
        }
        if(s->kind==SK_MODULE && has_doc){ load_const(g,co->doc); emit(g,I_STORE_GLOBAL,name_ix(g,"__doc__")); }
        int anns=0;
        for(int i=0;i<body->n;i++) if(body->v[i]->kind==PK_AnnAssign && body->v[i]->n[0]->kind==PK_Name && body->v[i]->op) anns=1;
        if(anns && (s->kind==SK_MODULE || s->kind==SK_CLASS)) emit(g,I_SETUP_ANNOTATIONS,0);
        stmts(g,body);
        if(s->kind==SK_CLASS && s->has_class_cell){ emit(g,I_LOAD_CELL,cell_ix(g,"__class__")); emit(g,I_RETURN,0); }
        else { emit(g,I_NONE,0); emit(g,I_RETURN,0); }
    } else if(s->kind==SK_FUNC){                      /* lambda */
        ex(g,n->n[1]); emit(g,I_RETURN,0);
    } else {                                          /* comprehension */
        int k=n->kind;
        if(k==PK_ListComp) emit(g,I_BUILD_LIST,0);
        else if(k==PK_SetComp) emit(g,I_BUILD_SET,0);
        else if(k==PK_DictComp) emit(g,I_BUILD_DICT,0);
        comp_body(g,n,0,0);
        if(k==PK_GeneratorExp) emit(g,I_NONE,0);
        emit(g,I_RETURN,0);
    }
    /* labels -> positions */
    for(int i=0;i<g->njumps;i++){ int at=g->jumps[i]; int l=(int)(g->code[at]>>8); g->code[at]=(g->code[at]&0xff)|((uint32_t)g->labels[l].pos<<8); }
    co->code=g->code; co->lines=g->lines; co->ncode=g->n;
    ListObj *cl=AS_LIST(keep_list);
    co->nconsts=(int)cl->len; co->consts=(Value*)xmalloc(sizeof(Value)*(size_t)(cl->len+1)); memcpy(co->consts,cl->items,sizeof(Value)*(size_t)cl->len);
    co->nnames=g->nnames; co->names=(StrObj**)xmalloc(sizeof(StrObj*)*(size_t)(g->nnames+1));
    for(int i=0;i<g->nnames;i++) co->names[i]=AS_STR(mp_intern(g->names[i]));
    co->nlocals=g->nvars; co->varnames=(StrObj**)xmalloc(sizeof(StrObj*)*(size_t)(g->nvars+1));
    for(int i=0;i<g->nvars;i++) co->varnames[i]=AS_STR(mp_intern(g->vars[i]));
    co->ncells=g->ncells; co->cellnames=(StrObj**)xmalloc(sizeof(StrObj*)*(size_t)(g->ncells+1));
    co->cellarg=(int*)xmalloc(sizeof(int)*(size_t)(g->ncells+1));
    for(int i=0;i<g->ncells;i++){ co->cellnames[i]=AS_STR(mp_intern(g->cells[i])); co->cellarg[i]=-1;
        int np=co->argc+co->kwonly+((flags&CO_VARARGS)?1:0)+((flags&CO_VARKW)?1:0);
        for(int k=0;k<np;k++) if(!strcmp(g->vars[k],g->cells[i])) co->cellarg[i]=k; }
    co->nfrees=g->nfrees; co->freenames=(StrObj**)xmalloc(sizeof(StrObj*)*(size_t)(g->nfrees+1));
    for(int i=0;i<g->nfrees;i++) co->freenames[i]=AS_STR(mp_intern(g->frees[i]));
    co->flags=flags; co->stacksize=g->maxdepth+8; co->nblocks=g->maxblocks+1;
    co->name=AS_STR(mp_intern(s->kind==SK_MODULE?"<module>":s->name));
    co->qualname=AS_STR(mp_str(s->qualname));
    co->file=c->file; co->firstline= n ? n->line : 1;
    if(args){
        int np=co->argc+co->kwonly+2;
        co->annots=(char**)xmalloc(sizeof(char*)*(size_t)np); memset(co->annots,0,sizeof(char*)*(size_t)np);
        int k=0;
        for(int j=0;j<3;j++) for(int i=0;i<args->L[j].n;i++,k++){ const char *t=first_type_name(args->L[j].v[i]->n[0]); co->annots[k]=t?xstrdup2(t):NULL; }
        PyNode *order[300]; int no=0;                  /* __annotations__: posonly, regular, *args, keyword-only, **kwargs, return */
        for(int i=0;i<args->L[0].n && no<290;i++) order[no++]=args->L[0].v[i];
        for(int i=0;i<args->L[1].n && no<290;i++) order[no++]=args->L[1].v[i];
        if(args->n[0]) order[no++]=args->n[0];
        for(int i=0;i<args->L[2].n && no<290;i++) order[no++]=args->L[2].v[i];
        if(args->n[1]) order[no++]=args->n[1];
        int nr=n && (n->kind==PK_FunctionDef||n->kind==PK_AsyncFunctionDef) && n->n[1];
        co->annname=(char**)xmalloc(sizeof(char*)*(size_t)(no+2)); co->anntext=(char**)xmalloc(sizeof(char*)*(size_t)(no+2));
        for(int i=0;i<no;i++) if(order[i]->n[0]){ co->annname[co->nann]=xstrdup2(order[i]->id[0]); co->anntext[co->nann++]=ann_text(order[i]->n[0]); }
        if(nr){ co->annname[co->nann]=xstrdup2("return"); co->anntext[co->nann++]=ann_text(n->n[1]); }
    }
    free(g->names); free(g->vars); free(g->cells); free(g->frees); free(g->labels); free(g->jumps);
    (void)keep_list;
    return cov;
}

CodeObj *mp_compile(PyNode *mod, const char *file, const char *modname, char **error, int *error_line){
    C c; memset(&c,0,sizeof c); c.file=xstrdup2(file);
    (void)modname;
    if(setjmp(c.jb)){ *error=c.err; *error_line=c.errline; return NULL; }
    Scope *top=scope_new(&c,SK_MODULE,mod,NULL,"<module>");
    c.top=top;
    st_stmts(&c,top,&mod->L[0]);
    resolve(&c,top);
    Value code=make_code(&c,top);
    return (CodeObj*)code.u.o;
}

/* --dump-bytecode */
static const char *op_names[I__COUNT]={
    "NOP","POP","DUP","DUP2","ROT2","ROT3","ROT4","CONST","NONE","LOAD_FAST","STORE_FAST","DEL_FAST","LOAD_DEREF","STORE_DEREF","DEL_DEREF",
    "LOAD_GLOBAL","STORE_GLOBAL","DEL_GLOBAL","LOAD_NAME","STORE_NAME","DEL_NAME","LOAD_CLASSDEREF","LOAD_ATTR","STORE_ATTR","DEL_ATTR",
    "LOAD_METHOD","CALL_METHOD","SUBSCR","STORE_SUBSCR","DEL_SUBSCR","BINOP","INPLACE","UNARY","COMPARE","NOT","TRUTH","JUMP","JUMP_IF_FALSE",
    "JUMP_IF_TRUE","JUMP_IF_FALSE_KEEP","JUMP_IF_TRUE_KEEP","JUMP_IF_NOT_EXC","BUILD_TUPLE","BUILD_LIST","BUILD_SET","BUILD_DICT","BUILD_SLICE",
    "BUILD_STRING","LIST_APPEND","LIST_EXTEND","SET_ADD","SET_UPDATE","DICT_SET","DICT_UPDATE","LIST_TO_TUPLE","FORMAT","FORMAT_SPEC","UNPACK",
    "UNPACK_EX","GET_ITER","FOR_ITER","CALL","CALL_KW","CALL_EX","RETURN","MAKE_FUNCTION","MAKE_CLASS","IMPORT","IMPORT_FROM","IMPORT_STAR",
    "SETUP","POP_BLOCK","RAISE","RERAISE","PUSH_EXC","POP_EXC","EXC_MATCH","EXC_MATCH_STAR","YIELD","GET_YIELD_FROM_ITER","GET_AWAITABLE",
    "SEND","GET_AITER","GET_ANEXT","END_ASYNC_FOR","ASSERT_FAIL","LOAD_BUILD_CLASS","WITH_ENTER","WITH_EXIT","ASYNC_WITH_ENTER","RETURN_GEN",
    "PRINT_EXPR","CHECK_EXC_GROUP","MATCH_CLASS","MATCH_SEQ","MATCH_MAP","MATCH_KEYS","COPY","SWAP","LOAD_LOCALS","SETUP_ANNOTATIONS",
    "DEL_SUBSCR_SLICE","CALL_INTRINSIC","LOAD_CELL"};
void mp_dump_code(CodeObj *co){
    printf("code %s (%s:%d) args=%d kwonly=%d locals=%d cells=%d frees=%d stack=%d flags=%d\n",co->qualname->s,co->file,co->firstline,co->argc,co->kwonly,co->nlocals,co->ncells,co->nfrees,co->stacksize,co->flags);
    for(int i=0;i<co->ncode;i++){
        int op=(int)(co->code[i]&0xff), arg=(int)(co->code[i]>>8);
        printf("  %4d %4d %-20s %d",i,co->lines[i],op<I__COUNT&&op_names[op]?op_names[op]:"?",arg);
        if(op==I_CONST){ Value r=mp_repr(co->consts[arg]); printf("  (%s)",mp_cstr(r)); }
        else if(op==I_LOAD_GLOBAL||op==I_STORE_GLOBAL||op==I_LOAD_NAME||op==I_STORE_NAME||op==I_LOAD_ATTR||op==I_STORE_ATTR||op==I_LOAD_METHOD||op==I_IMPORT_FROM) printf("  (%s)",co->names[arg]->s);
        else if(op==I_LOAD_FAST||op==I_STORE_FAST) printf("  (%s)",co->varnames[arg]->s);
        printf("\n");
    }
    for(int i=0;i<co->nconsts;i++) if(IS(co->consts[i],T_code)) mp_dump_code((CodeObj*)co->consts[i].u.o);
}
