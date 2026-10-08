/* ========================= cpython target: scopes =========================
   CPython's symtable rules for every name of a module: a function's names
   are local (assigned, parameters, imports, ...), global (declared, or
   neither assigned here nor in an enclosing function), free (assigned in an
   enclosing function; nonlocal) or cells (local and used by an inner
   function). Class bodies are not enclosing scopes for their methods; a
   method that uses `super` or `__class__` gets the class's `__class__` cell.
   Comprehensions and generator expressions are function scopes (their first
   iterable belongs to the enclosing one); a walrus in them binds in the
   enclosing function. Private names (__x) are mangled with the class name.
   Annotations get their own scope (PEP 649) so that the enclosing names they
   use become cells, as CPython makes them. */

#include "capi.h"
#include <setjmp.h>

static jmp_buf *st_jb; static CSymtable *st_cur;
static void st_fail(int line, const char *msg){
    if(!st_cur->error){ st_cur->error=xstrdup2(msg); st_cur->error_line=line; }
    longjmp(*st_jb,1);
}

char *capi_mangle(const char *priv, const char *name){
    if(!priv || name[0]!='_' || name[1]!='_') return xstrdup2(name);
    size_t n=strlen(name);
    if(n>=4 && name[n-1]=='_' && name[n-2]=='_') return xstrdup2(name);
    if(strchr(name,'.')) return xstrdup2(name);
    while(*priv=='_') priv++;
    if(!*priv) return xstrdup2(name);
    size_t pl=strlen(priv);
    char *r=(char*)xmalloc(pl+n+2); r[0]='_'; memcpy(r+1,priv,pl); memcpy(r+1+pl,name,n+1);
    return r;
}

CSym *capi_lookup(CScope *s, const char *name){
    for(int i=0;i<s->nsyms;i++) if(!strcmp(s->syms[i].name,name)) return &s->syms[i];
    return NULL;
}
static CSym *sym_get(CScope *s, const char *name){
    CSym *y=capi_lookup(s,name); if(y) return y;
    if(s->nsyms==s->capsyms){ s->capsyms=s->capsyms?s->capsyms*2:16; s->syms=(CSym*)xrealloc(s->syms,sizeof(CSym)*(size_t)s->capsyms); }
    y=&s->syms[s->nsyms++]; memset(y,0,sizeof *y); y->name=xstrdup2(name); return y;
}
static void add_flag(CScope *s, const char *raw, int flag, int line){
    char *m=capi_mangle(s->private_name,raw);
    CSym *y=sym_get(s,m); free(m);
    if((flag&DF_GLOBAL) && (y->flags&(DF_LOCAL|DF_PARAM|DF_USE)) && s->kind!=SC_MODULE){
        /* CPython: "name assigned/used before global declaration" is an error */
        char b[300]; snprintf(b,sizeof b,"name '%s' is %s prior to global declaration",y->name,(y->flags&DF_USE)?"used":"assigned to");
        st_fail(line,b);
    }
    if((flag&DF_NONLOCAL) && s->kind==SC_MODULE) st_fail(line,"nonlocal declaration not allowed at module level");
    y->flags|=flag;
}

static CScope *scope_new(CSymtable *st, CScopeKind k, const char *name, PyNode *node, CScope *parent){
    CScope *s=(CScope*)xmalloc(sizeof(CScope)); memset(s,0,sizeof *s);
    s->kind=k; s->name=xstrdup2(name); s->node=node; s->parent=parent;
    s->private_name= k==SC_CLASS ? s->name : parent ? parent->private_name : NULL;
    if(parent){
        if(parent->nkids==parent->capkids){ parent->capkids=parent->capkids?parent->capkids*2:4; parent->kids=(CScope**)xrealloc(parent->kids,sizeof(CScope*)*(size_t)parent->capkids); }
        parent->kids[parent->nkids++]=s;
        /* qualname: f.<locals>.g, C.m; a module's direct children are just their name */
        if(parent->kind==SC_MODULE) s->qualname=xstrdup2(name);
        else {
            const char *sep= parent->kind==SC_CLASS ? "." : ".<locals>.";
            size_t a=strlen(parent->qualname), b=strlen(sep), c=strlen(name);
            s->qualname=(char*)xmalloc(a+b+c+1); memcpy(s->qualname,parent->qualname,a); memcpy(s->qualname+a,sep,b); memcpy(s->qualname+a+b,name,c+1);
        }
    } else s->qualname=xstrdup2(name);
    st->all=(CScope**)xrealloc(st->all,sizeof(CScope*)*(size_t)(st->nall+1));
    s->id=st->nall; st->all[st->nall++]=s;
    return s;
}

/* scopes opened by nodes */
typedef struct { PyNode *n; CScope *s; } NodeScope;
static NodeScope *ns_map; static int ns_n, ns_cap;
static void map_node(PyNode *n, CScope *s){
    if(ns_n==ns_cap){ ns_cap=ns_cap?ns_cap*2:64; ns_map=(NodeScope*)xrealloc(ns_map,sizeof(NodeScope)*(size_t)ns_cap); }
    ns_map[ns_n].n=n; ns_map[ns_n].s=s; ns_n++;
}
CScope *capi_scope_of(CSymtable *st, PyNode *n){
    (void)st;
    for(int i=0;i<ns_n;i++) if(ns_map[i].n==n) return ns_map[i].s;
    return NULL;
}

static void v_expr(CSymtable *st, CScope *s, PyNode *e);
static void v_stmts(CSymtable *st, CScope *s, PyList *l);
static void v_exprs(CSymtable *st, CScope *s, PyList *l){ for(int i=0;i<l->n;i++) if(l->v[i]) v_expr(st,s,l->v[i]); }

/* a function scope that annotations / defaults belong to */
static void v_args_defaults(CSymtable *st, CScope *s, PyNode *a){
    v_exprs(st,s,&a->L[4]);                                /* defaults */
    v_exprs(st,s,&a->L[3]);                                /* kw_defaults (may hold NULLs) */
}
static int has_annotations(PyNode *a, PyNode *returns){
    if(returns) return 1;
    for(int k=0;k<3;k++) for(int i=0;i<a->L[k].n;i++) if(a->L[k].v[i]->n[0]) return 1;
    if(a->n[0] && a->n[0]->n[0]) return 1;
    if(a->n[1] && a->n[1]->n[0]) return 1;
    return 0;
}
static void v_annotations(CSymtable *st, CScope *s, PyNode *a, PyNode *returns, PyNode *owner){
    if(!has_annotations(a,returns)) return;
    CScope *an=scope_new(st,SC_ANNOTATION,"__annotate__",owner,s);
    an->private_name=s->private_name;
    for(int k=0;k<3;k++) for(int i=0;i<a->L[k].n;i++) if(a->L[k].v[i]->n[0]) v_expr(st,an,a->L[k].v[i]->n[0]);
    if(a->n[0] && a->n[0]->n[0]) v_expr(st,an,a->n[0]->n[0]);
    if(a->n[1] && a->n[1]->n[0]) v_expr(st,an,a->n[1]->n[0]);
    if(returns) v_expr(st,an,returns);
}
static void def_params(CScope *f, PyNode *a, int line){
    for(int k=0;k<3;k++) for(int i=0;i<a->L[k].n;i++) add_flag(f,a->L[k].v[i]->id[0],DF_PARAM,line);
    if(a->n[0]) add_flag(f,a->n[0]->id[0],DF_PARAM,line);
    if(a->n[1]) add_flag(f,a->n[1]->id[0],DF_PARAM,line);
}

/* the enclosing scope a walrus in a comprehension binds in */
static void walrus_target(CSymtable *st, CScope *s, const char *name, int line){
    (void)st;
    if(!s->is_comprehension){ add_flag(s,name,DF_LOCAL,line); return; }
    CScope *t=s;
    while(t->is_comprehension) t=t->parent;
    if(t->kind==SC_CLASS) st_fail(line,"assignment expression within a comprehension cannot be used in a class body");
    for(CScope *c=s;c!=t;c=c->parent){
        CSym *y=capi_lookup(c,name);
        if(y && (y->flags&DF_COMP_ITER)) st_fail(line,"assignment expression cannot rebind comprehension iteration variable");
        char *m=capi_mangle(c->private_name,name); CSym *yy=sym_get(c,m); free(m);
        yy->flags|= t->kind==SC_MODULE?DF_GLOBAL:DF_NONLOCAL;
    }
    add_flag(t,name,DF_LOCAL,line);
}

static void v_comprehension(CSymtable *st, CScope *s, PyNode *e, const char *name){
    PyList *gens=&e->L[0];
    v_expr(st,s,gens->v[0]->n[1]);                          /* the first iterable: in the enclosing scope */
    CScope *c=scope_new(st,SC_FUNCTION,name,e,s);
    c->is_comprehension=1; c->is_genexp= e->kind==PK_GeneratorExp;
    map_node(e,c);
    for(int i=0;i<gens->n;i++){
        PyNode *g=gens->v[i];
        if(i) v_expr(st,c,g->n[1]);
        if(g->op && !c->is_genexp) s->has_await=1;          /* async for in a list comprehension: its function awaits */
        if(g->op) c->is_coroutine=1;
        v_expr(st,c,g->n[0]);
        v_exprs(st,c,&g->L[0]);
    }
    /* iteration variables */
    for(int i=0;i<c->nsyms;i++) if(c->syms[i].flags&DF_LOCAL) c->syms[i].flags|=DF_COMP_ITER;
    if(e->kind==PK_DictComp){ v_expr(st,c,e->n[0]); v_expr(st,c,e->n[1]); }
    else v_expr(st,c,e->n[0]);
    if(c->has_await && !c->is_genexp) s->has_await=1;
}


static void v_expr(CSymtable *st, CScope *s, PyNode *e){
    if(!e) return;
    switch(e->kind){
        case PK_Name:{
            int flag= e->op==CTX_Load ? DF_USE : DF_LOCAL;
            add_flag(s,e->id[0],flag,e->line);
            if(e->op==CTX_Load && (!strcmp(e->id[0],"super") || !strcmp(e->id[0],"__class__")) && s->kind==SC_FUNCTION)
                add_flag(s,"__class__",DF_USE,e->line);           /* the implicit __class__ cell */
            return;
        }
        case PK_NamedExpr:
            v_expr(st,s,e->n[1]);
            walrus_target(st,s,e->n[0]->id[0],e->line);
            return;
        case PK_Lambda:{
            PyNode *a=e->n[0];
            v_args_defaults(st,s,a);
            CScope *f=scope_new(st,SC_FUNCTION,"<lambda>",e,s);
            f->is_lambda=1; map_node(e,f);
            def_params(f,a,e->line);
            v_expr(st,f,e->n[1]);
            return;
        }
        case PK_ListComp: v_comprehension(st,s,e,"<listcomp>"); return;
        case PK_SetComp: v_comprehension(st,s,e,"<setcomp>"); return;
        case PK_DictComp: v_comprehension(st,s,e,"<dictcomp>"); return;
        case PK_GeneratorExp: v_comprehension(st,s,e,"<genexpr>"); return;
        case PK_Yield: case PK_YieldFrom:
            if(s->kind!=SC_FUNCTION || s->is_comprehension) st_fail(e->line,"'yield' outside function");
            s->is_generator=1; v_expr(st,s,e->n[0]); return;
        case PK_Await:
            s->has_await=1; v_expr(st,s,e->n[0]); return;
        case PK_Call:
            v_expr(st,s,e->n[0]); v_exprs(st,s,&e->L[0]);
            for(int i=0;i<e->L[1].n;i++) v_expr(st,s,e->L[1].v[i]->n[0]);
            if(e->n[0]->kind==PK_Name){
                const char *f=e->n[0]->id[0]; int n=e->L[0].n+e->L[1].n;
                if((!strcmp(f,"locals") && n==0) || (!strcmp(f,"vars") && n==0) || (!strcmp(f,"dir") && n==0) ||
                   ((!strcmp(f,"eval") || !strcmp(f,"exec")) && n<=1)) s->dynamic_locals=1;
            }
            return;
        default: break;
    }
    /* everything else: its children */
    for(int i=0;i<4;i++) if(e->n[i]) v_expr(st,s,e->n[i]);
    for(int k=0;k<5;k++) for(int i=0;i<e->L[k].n;i++){
        PyNode *c=e->L[k].v[i];
        if(!c || c->kind==PK_ident) continue;
        if(c->kind==PK_keyword){ v_expr(st,s,c->n[0]); continue; }
        if(c->kind==PK_comprehension) continue;
        v_expr(st,s,c);
    }
}

static void bind_pattern(CSymtable *st, CScope *s, PyNode *p){
    if(!p) return;
    switch(p->kind){
        case PK_MatchAs: if(p->id[0]) add_flag(s,p->id[0],DF_LOCAL,p->line); bind_pattern(st,s,p->n[0]); return;
        case PK_MatchStar: if(p->id[0]) add_flag(s,p->id[0],DF_LOCAL,p->line); return;
        case PK_MatchMapping: v_exprs(st,s,&p->L[0]); for(int i=0;i<p->L[1].n;i++) bind_pattern(st,s,p->L[1].v[i]); if(p->id[0]) add_flag(s,p->id[0],DF_LOCAL,p->line); return;
        case PK_MatchClass: v_expr(st,s,p->n[0]); for(int i=0;i<p->L[0].n;i++) bind_pattern(st,s,p->L[0].v[i]); for(int i=0;i<p->L[2].n;i++) bind_pattern(st,s,p->L[2].v[i]); return;
        case PK_MatchSequence: case PK_MatchOr: for(int i=0;i<p->L[0].n;i++) bind_pattern(st,s,p->L[0].v[i]); return;
        case PK_MatchValue: v_expr(st,s,p->n[0]); return;
        default: return;
    }
}

static void v_stmt(CSymtable *st, CScope *s, PyNode *n){
    switch(n->kind){
        case PK_FunctionDef: case PK_AsyncFunctionDef:{
            PyNode *a=n->n[0];
            v_exprs(st,s,&n->L[1]);                         /* decorators */
            v_args_defaults(st,s,a);
            if(n->L[2].n){                                  /* PEP 695 type parameters: CPython compiles these */
                CScope *f=scope_new(st,SC_FUNCTION,n->id[0],n,s);
                map_node(n,f); f->island=1; f->island_reason="type parameters";
                add_flag(s,n->id[0],DF_LOCAL,n->line);
                def_params(f,a,n->line); v_stmts(st,f,&n->L[0]);
                if(n->kind==PK_AsyncFunctionDef) f->is_coroutine=1;
                return;
            }
            v_annotations(st,s,a,n->n[1],n);
            add_flag(s,n->id[0],DF_LOCAL,n->line);
            CScope *f=scope_new(st,SC_FUNCTION,n->id[0],n,s);   /* (qualnames keep the unmangled name) */
            map_node(n,f);
            if(n->kind==PK_AsyncFunctionDef) f->is_coroutine=1;
            def_params(f,a,n->line);
            v_stmts(st,f,&n->L[0]);
            return;
        }
        case PK_ClassDef:{
            v_exprs(st,s,&n->L[1]);                         /* decorators */
            v_exprs(st,s,&n->L[3]);                         /* bases */
            for(int i=0;i<n->L[4].n;i++) v_expr(st,s,n->L[4].v[i]->n[0]);
            add_flag(s,n->id[0],DF_LOCAL,n->line);
            CScope *c=scope_new(st,SC_CLASS,n->id[0],n,s);
            c->private_name=c->name;
            /* mangling uses the class's own (unmangled) name */
            c->private_name=xstrdup2(n->id[0]);
            map_node(n,c);
            if(n->L[2].n){ c->island=1; c->island_reason="type parameters"; }
            v_stmts(st,c,&n->L[0]);
            return;
        }
        case PK_Return:
            if(s->kind!=SC_FUNCTION) st_fail(n->line,"'return' outside function");
            v_expr(st,s,n->n[0]); return;
        case PK_Delete: v_exprs(st,s,&n->L[0]); return;
        case PK_Assign: v_expr(st,s,n->n[0]); v_exprs(st,s,&n->L[0]); return;
        case PK_AugAssign:
            if(n->n[0]->kind==PK_Name) add_flag(s,n->n[0]->id[0],DF_USE,n->line);
            v_expr(st,s,n->n[0]); v_expr(st,s,n->n[1]); return;
        case PK_AnnAssign:{
            PyNode *t=n->n[0];
            if(t->kind==PK_Name){
                char *m=capi_mangle(s->private_name,t->id[0]); CSym *y=capi_lookup(s,m); free(m);
                if(y && (y->flags&(DF_GLOBAL|DF_NONLOCAL)) && n->op) st_fail(n->line,"annotated name can't be global");
            }
            if(n->n[2]) v_expr(st,s,n->n[2]);
            v_expr(st,s,t);
            if(s->kind!=SC_FUNCTION && n->op){               /* collected by the scope's __annotate__ */
                CScope *an=scope_new(st,SC_ANNOTATION,"__annotate__",n,s); an->private_name=s->private_name;
                v_expr(st,an,n->n[1]);
            }
            return; }
        case PK_For: case PK_AsyncFor:
            if(n->kind==PK_AsyncFor) s->has_await=1;
            v_expr(st,s,n->n[1]); v_expr(st,s,n->n[0]); v_stmts(st,s,&n->L[0]); v_stmts(st,s,&n->L[1]); return;
        case PK_While: case PK_If:
            v_expr(st,s,n->n[0]); v_stmts(st,s,&n->L[0]); v_stmts(st,s,&n->L[1]); return;
        case PK_With: case PK_AsyncWith:
            if(n->kind==PK_AsyncWith) s->has_await=1;
            for(int i=0;i<n->L[3].n;i++){ v_expr(st,s,n->L[3].v[i]->n[0]); v_expr(st,s,n->L[3].v[i]->n[1]); }
            v_stmts(st,s,&n->L[0]); return;
        case PK_Match:
            v_expr(st,s,n->n[0]);
            for(int i=0;i<n->L[3].n;i++){ PyNode *c=n->L[3].v[i]; bind_pattern(st,s,c->n[0]); v_expr(st,s,c->n[1]); v_stmts(st,s,&c->L[0]); }
            return;
        case PK_Raise: v_expr(st,s,n->n[0]); v_expr(st,s,n->n[1]); return;
        case PK_Try: case PK_TryStar:
            v_stmts(st,s,&n->L[0]);
            for(int i=0;i<n->L[3].n;i++){ PyNode *h=n->L[3].v[i]; v_expr(st,s,h->n[0]); if(h->id[0]) add_flag(s,h->id[0],DF_LOCAL,h->line); v_stmts(st,s,&h->L[0]); }
            v_stmts(st,s,&n->L[1]); v_stmts(st,s,&n->L[2]); return;
        case PK_Assert: v_expr(st,s,n->n[0]); v_expr(st,s,n->n[1]); return;
        case PK_Import: case PK_ImportFrom:
            for(int i=0;i<n->L[3].n;i++){
                PyNode *al=n->L[3].v[i];
                if(!strcmp(al->id[0],"*")){ if(s->kind!=SC_MODULE) st_fail(n->line,"import * only allowed at module level"); continue; }
                const char *bound=al->id[1];
                char first[512];
                if(!bound){
                    if(n->kind==PK_Import){ const char *d=strchr(al->id[0],'.'); size_t l=d?(size_t)(d-al->id[0]):strlen(al->id[0]); if(l>=sizeof first) l=sizeof first-1; memcpy(first,al->id[0],l); first[l]=0; bound=first; }
                    else bound=al->id[0];
                }
                add_flag(s,bound,DF_LOCAL|DF_IMPORT,n->line);
            }
            return;
        case PK_Global: case PK_Nonlocal:
            for(int i=0;i<n->L[3].n;i++) add_flag(s,n->L[3].v[i]->id[0],n->kind==PK_Global?DF_GLOBAL:DF_NONLOCAL,n->line);
            return;
        case PK_Expr: v_expr(st,s,n->n[0]); return;
        case PK_TypeAlias:
            add_flag(s,n->n[0]->id[0],DF_LOCAL,n->line);
            { CScope *t=scope_new(st,SC_FUNCTION,n->n[0]->id[0],n,s); t->island=1; t->island_reason="type alias"; map_node(n,t); v_expr(st,t,n->n[1]); }
            return;
        default: return;                                   /* pass, break, continue */
    }
}
static void v_stmts(CSymtable *st, CScope *s, PyList *l){ for(int i=0;i<l->n;i++) v_stmt(st,s,l->v[i]); }

/* ---------------------------------------------------------------- resolution */
typedef struct { char **v; int n, cap; } Names;
static int names_has(Names *ns, const char *s){ for(int i=0;i<ns->n;i++) if(!strcmp(ns->v[i],s)) return 1; return 0; }
static void names_add(Names *ns, const char *s){ if(names_has(ns,s)) return; if(ns->n==ns->cap){ ns->cap=ns->cap?ns->cap*2:16; ns->v=(char**)xrealloc(ns->v,sizeof(char*)*(size_t)ns->cap); } ns->v[ns->n++]=(char*)s; }
static int cmpstr(const void *a, const void *b){ return strcmp(*(char*const*)a,*(char*const*)b); }

/* bound: names bound in enclosing function scopes. Returns the names free in s (for the parent). */
static void analyze(CSymtable *st, CScope *s, Names *bound, Names *free_out){
    Names newbound; memset(&newbound,0,sizeof newbound);
    for(int i=0;i<bound->n;i++) names_add(&newbound,bound->v[i]);
    for(int i=0;i<s->nsyms;i++){
        CSym *y=&s->syms[i]; int f=y->flags;
        if(s->kind==SC_MODULE){ y->res= (f&DF_GLOBAL) ? R_GLOBAL_EXPLICIT : R_GLOBAL_IMPLICIT; continue; }
        if(f&DF_GLOBAL){ if(f&DF_NONLOCAL) st_fail(s->node?s->node->line:0,"name is nonlocal and global"); y->res=R_GLOBAL_EXPLICIT; continue; }
        if(f&DF_NONLOCAL){
            if(!names_has(bound,y->name)){ char b[300]; snprintf(b,sizeof b,"no binding for nonlocal '%s' found",y->name); st_fail(s->node?s->node->line:0,b); }
            y->res=R_FREE; continue;
        }
        if(f&(DF_LOCAL|DF_PARAM)){ y->res=R_LOCAL; continue; }
        if(names_has(bound,y->name)){ y->res=R_FREE; continue; }
        y->res=R_GLOBAL_IMPLICIT;
    }
    if(s->kind==SC_FUNCTION || s->kind==SC_ANNOTATION){
        for(int i=0;i<s->nsyms;i++) if(s->syms[i].res==R_LOCAL) names_add(&newbound,s->syms[i].name);
    }
    if(s->kind==SC_CLASS) names_add(&newbound,"__class__");   /* the implicit cell, for its methods */
    if(s->kind==SC_MODULE) newbound.n=0;
    /* children */
    Names childfree; memset(&childfree,0,sizeof childfree);
    for(int k=0;k<s->nkids;k++) analyze(st,s->kids[k],&newbound,&childfree);
    for(int i=0;i<childfree.n;i++){
        const char *nm=childfree.v[i];
        if(s->kind==SC_CLASS && !strcmp(nm,"__class__")){ s->needs_class_cell=1; continue; }
        CSym *y=capi_lookup(s,nm);
        if(y && y->res==R_LOCAL && s->kind!=SC_CLASS){ y->res=R_CELL; continue; }
        if(y && y->res==R_CELL) continue;
        if(s->kind==SC_MODULE) continue;
        /* passes through this scope to an inner one */
        if(!y){ y=sym_get(s,nm); y->flags|=DF_FREE_PASS; y->res=R_FREE; }
        else if(y->res==R_GLOBAL_IMPLICIT || y->res==R_NONE){ y->res=R_FREE; y->flags|=DF_FREE_PASS; }
        else if(y->res==R_LOCAL && s->kind==SC_CLASS){ y->flags|=DF_FREE_PASS; }   /* class-local name, also a free var of the class scope */
    }
    /* what s needs from outside */
    Names fr, cl; memset(&fr,0,sizeof fr); memset(&cl,0,sizeof cl);
    for(int i=0;i<s->nsyms;i++){
        CSym *y=&s->syms[i];
        if(y->res==R_FREE || (s->kind==SC_CLASS && (y->flags&DF_FREE_PASS))){ names_add(&fr,y->name); names_add(free_out,y->name); }
        if(y->res==R_CELL) names_add(&cl,y->name);
    }
    if(s->needs_class_cell) names_add(&cl,"__class__");
    qsort(fr.v,(size_t)fr.n,sizeof(char*),cmpstr); qsort(cl.v,(size_t)cl.n,sizeof(char*),cmpstr);
    s->freevars=fr.v; s->nfree=fr.n; s->cellvars=cl.v; s->ncell=cl.n;
    free(newbound.v); free(childfree.v);
}

CRes capi_resolve(CScope *s, const char *name){
    CSym *y=capi_lookup(s,name);
    if(!y) return s->kind==SC_MODULE ? R_GLOBAL_IMPLICIT : R_GLOBAL_IMPLICIT;
    return y->res;
}

/* islands: what CPython compiles for us */
static void decide_islands(CScope *s){
    if(s->kind==SC_FUNCTION){
        if(s->dynamic_locals && !s->island){ s->island=1; s->island_reason="uses locals()/vars()/eval/exec/dir()"; }
    }
    for(int k=0;k<s->nkids;k++) decide_islands(s->kids[k]);
}

int capi_symtable(CSymtable *st, PyNode *mod, const char *modname){
    memset(st,0,sizeof *st);
    ns_n=0;
    jmp_buf jb; st_jb=&jb; st_cur=st;
    if(setjmp(jb)) return 1;
    CScope *m=scope_new(st,SC_MODULE,modname,mod,NULL);
    map_node(mod,m);
    st->top=m;
    v_stmts(st,m,&mod->L[0]);
    Names none; memset(&none,0,sizeof none); Names fr; memset(&fr,0,sizeof fr);
    analyze(st,m,&none,&fr);
    decide_islands(m);
    return 0;
}
