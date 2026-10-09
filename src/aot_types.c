/* ========================= Typed compiler: model + type checker =========================
   Builds the program model (modules, functions, classes, variables) from the
   ASTs and gives every variable, parameter, return value, field and container
   element exactly one static type. Types come from annotations
   (`x: int`, `def f(a: str) -> list[int]`, class-body fields) or are inferred
   by unification from the code: `i = 1` makes i an int, `grid = []` followed
   by `grid.append(row)` makes grid a list of row's type. Assigning a value of
   another type is a compile error. `None` makes the type it meets optional
   (Ty.opt): whatever can hold None is Optional[T], and the code generator
   checks for None where Python would fail on it.

   Inference runs whole-program passes until nothing new is learned, then a
   final strict pass reports whatever is still unknown. */

#include "aot_model.h"
#include <stdarg.h>
#include "py_front.h"
#include "tokens.h"

/* ---------------------------------------------------------------- types */

static int ty_ids;
Ty *TY_INT_T, *TY_BOOL_T, *TY_FLOAT_T, *TY_STR_T, *TY_VOID_T, *TY_BUF_T, *TY_BYTES_T, *TY_TYPE_T;

Ty *ty_new(TyKind k, Ty *elem, AClass *cls){ Ty *t=MPY_NEW0(Ty); t->k=k; t->elem=elem; t->cls=cls; t->id=++ty_ids;
    if(k==TY_GEN){ t->nelems=2; t->elems=MPY_NEW_ARR(Ty*,2); t->elems[0]=ty_var(); t->elems[1]=TY_VOID_T; }   /* what send() takes, what it returns (None) */
    return t; }
Ty *aot_gen_part(Ty *g, int i){ g=ty_find(g); if(g->k!=TY_GEN || g->nelems!=2) return TY_VOID_T; Ty *t=ty_find(g->elems[i]); return t; }
Ty *ty_var(void){ return ty_new(TY_VAR,NULL,NULL); }
Ty *ty_dict(Ty *key, Ty *val){ Ty *t=ty_new(TY_DICT,val,NULL); t->key=key; return t; }
Ty *ty_dkey(Ty *d){ d=ty_find(d); return d->key?d->key:TY_STR_T; }
Ty *ty_tuple(Ty **elems, int n){
    Ty *t=ty_new(TY_TUPLE,NULL,NULL); t->nelems=n; t->elems=MPY_NEW_ARR(Ty*,n>0?n:1);
    for(int i=0;i<n;i++) t->elems[i]=elems[i];
    return t;
}
Ty *ty_func(Ty **params, int n, Ty *ret){ Ty *t=ty_tuple(params,n); t->k=TY_FUNC; t->elem=ret; return t; }
Ty *ty_find(Ty *t){ while(t && t->k==TY_VAR && t->link) t=t->link; return t; }
int ty_opt(Ty *t){ t=ty_find(t); return t && t->opt; }
/* the shared types (TY_INT_T ...) are never made optional: a variable gets a copy of its own */
static int ty_shared(Ty *t){ return t==TY_INT_T||t==TY_BOOL_T||t==TY_FLOAT_T||t==TY_STR_T||t==TY_BUF_T||t==TY_VOID_T||t==TY_BYTES_T||t==TY_TYPE_T; }
static Ty *ty_copy(Ty *t){ Ty *n=ty_new(t->k,t->elem,t->cls); n->elems=t->elems; n->nelems=t->nelems; n->key=t->key; n->tup=t->tup; n->names=t->names; n->opt=t->opt; n->ndef=t->ndef; return n; }
/* element-like parts: list/dict/set/task/generator elements, a function's result */
static int has_elem(Ty *t){ return t->k==TY_LIST||t->k==TY_DICT||t->k==TY_SET||t->k==TY_TASK||t->k==TY_GEN||t->k==TY_FUNC||t->k==TY_FILE; }
int ty_known(Ty *t){
    t=ty_find(t);
    if(!t || t->k==TY_VAR) return 0;
    if(t->k==TY_DICT && !ty_known(ty_dkey(t))) return 0;
    if(t->k==TY_TUPLE||t->k==TY_FUNC){ for(int i=0;i<t->nelems;i++) if(!ty_known(t->elems[i])) return 0; }
    if(has_elem(t)) return ty_known(t->elem);
    return 1;
}
int ty_is_ptr(Ty *t){ t=ty_find(t); return t->k==TY_STR||t->k==TY_BYTES||t->k==TY_LIST||t->k==TY_DICT||t->k==TY_SET||t->k==TY_OBJ||t->k==TY_BUF||t->k==TY_TASK||t->k==TY_TUPLE||t->k==TY_FILE||t->k==TY_FUNC||t->k==TY_GEN; }
int ty_size(Ty *t){ TyKind k=ty_find(t)->k; return k==TY_FLOAT||k==TY_INT ? 8 : 4; }
static int names_eq(Ty *a, Ty *b){
    if(!a->names || !b->names) return a->names==b->names;
    for(int i=0;i<a->nelems;i++) if(strcmp(a->names[i],b->names[i])) return 0;
    return 1;
}
static int same_rec(Ty *a, Ty *b, int exact);
int ty_same(Ty *a, Ty *b){ return same_rec(a,b,0); }
int ty_same_exact(Ty *a, Ty *b){ return same_rec(a,b,1); }    /* Optional[...] counts too */
static int same_rec(Ty *a, Ty *b, int exact){
    a=ty_find(a); b=ty_find(b);
    if(a==b) return 1;
    if(a->k!=b->k) return 0;
    if(exact && a->opt!=b->opt) return 0;
    if(a->k==TY_DICT && !same_rec(ty_dkey(a),ty_dkey(b),exact)) return 0;
    if((a->k==TY_LIST||a->k==TY_FUNC||a->k==TY_GEN) && a->tup!=b->tup) return 0;
    if(a->k==TY_TUPLE||a->k==TY_FUNC){ if(a->nelems!=b->nelems) return 0; for(int i=0;i<a->nelems;i++) if(!same_rec(a->elems[i],b->elems[i],exact)) return 0; if(a->k==TY_TUPLE) return names_eq(a,b); }
    if(has_elem(a)) return same_rec(a->elem,b->elem,exact);
    if(a->k==TY_OBJ) return a->cls==b->cls;
    return a->k!=TY_VAR;
}
const char *ty_name(Ty *t){
    static char ring[8][160]; static int k;
    char *b=ring[k++&7];
    t=ty_find(t);
    switch(t->k){
        case TY_VAR: return "?";
        case TY_VOID: return "None";
        case TY_INT: return "int";
        case TY_BOOL: return "bool";
        case TY_FLOAT: return "float";
        case TY_STR: return "str";
        case TY_BYTES: return "bytes";
        case TY_TYPE: return "type";
        case TY_BUF: return "buffer";
        case TY_FILE: return "file";
        case TY_LIST: snprintf(b,160,t->tup?"tuple[%s, ...]":"list[%s]",ty_name(t->elem)); return b;
        case TY_GEN: snprintf(b,160,"%sIterator[%s]",t->tup?"Async":"",ty_name(t->elem)); return b;
        case TY_FUNC:{ int n=snprintf(b,160,"Callable[[");
            int nreg=t->nelems-(t->tup&1)-((t->tup>>1)&1);
            for(int i=0;i<t->nelems && n<140;i++) n+=snprintf(b+n,160-(size_t)n,"%s%s%s",i?", ":"",i<nreg?"":i==nreg&&(t->tup&1)?"*":"**",ty_name(t->elems[i]));
            snprintf(b+n,160-(size_t)n,"], %s]",ty_name(t->elem)); return b; }
        case TY_SET: snprintf(b,160,"set[%s]",ty_name(t->elem)); return b;
        case TY_DICT: snprintf(b,160,"dict[%s, %s]",ty_name(ty_dkey(t)),ty_name(t->elem)); return b;
        case TY_OBJ: return t->cls->name;
        case TY_TASK: snprintf(b,160,"Task[%s]",ty_name(t->elem)); return b;
        case TY_TUPLE: if(t->names){ int n=snprintf(b,160,"{");
                for(int i=0;i<t->nelems && n<150;i++) n+=snprintf(b+n,160-(size_t)n,"%s'%s': %s",i?", ":"",t->names[i],ty_name(t->elems[i]));
                snprintf(b+n,160-(size_t)n,"}"); return b; }
            { int n=snprintf(b,160,"tuple[");
            for(int i=0;i<t->nelems && n<150;i++) n+=snprintf(b+n,160-(size_t)n,"%s%s",i?", ":"",ty_name(t->elems[i]));
            snprintf(b+n,160-(size_t)n,"]"); return b; }
    }
    return "?";
}

/* ---------------------------------------------------------------- symbol tables */

ASym *symtab_find(SymTab *t, const char *name){ for(int i=0;i<t->n;i++) if(!strcmp(t->v[i].name,name)) return &t->v[i]; return NULL; }
ASym *symtab_add(SymTab *t, const char *name, SymKind kind, void *p){
    ASym *s=symtab_find(t,name);
    if(!s){ if(t->n==t->cap){ t->cap=t->cap?t->cap*2:16; t->v=(ASym*)xrealloc(t->v,sizeof(ASym)*(size_t)t->cap); } s=&t->v[t->n++]; s->name=xstrdup2(name); }
    s->kind=kind; s->p=p; return s;
}
XInfo *xinfo(Expr *e){ if(!e->ty) e->ty=MPY_NEW0(XInfo); return (XInfo*)e->ty; }

AField *aot_find_field(AClass *c, const char *name){ for(;c;c=c->base) for(int i=0;i<c->nfields;i++) if(!strcmp(c->fields[i]->name,name)) return c->fields[i]; return NULL; }
AField *aot_field_root(AField *fd){ while(fd->over) fd=fd->over; return fd; }
int aot_field_shared(AField *fd){ AField *r=aot_field_root(fd); return fd->cvar && !r->inst_set && !r->overridden; }
AFunc *aot_find_method(AClass *c, const char *name){ for(;c;c=c->base) for(int i=0;i<c->nmethods;i++) if(!strcmp(c->methods[i]->name,name)) return c->methods[i]; return NULL; }
int aot_subclass(AClass *c, AClass *base){ for(;c;c=c->base) if(c==base) return 1; return 0; }

/* ---------------------------------------------------------------- parsing helpers */

static Expr *xnew(ExprKind k, int line){ Expr *e=MPY_NEW0(Expr); e->kind=k; e->line=line; return e; }
static void xpush(Expr *e, Expr *item){
    if(e->count==e->cap){ e->cap=e->cap?e->cap*2:4; e->items=(Expr**)xrealloc(e->items,sizeof(Expr*)*(size_t)e->cap); }
    e->items[e->count++]=item;
}
MPY_NORETURN static void parse_fail(AotUnit *u, int line, const char *msg){
    fprintf(stderr,"%s:%d: error: %s\n",u->path?u->path:"<source>",line,msg); exit(1);
}

/* The statements' parts, as the frontend (py_front.c) spelled them out. */
Expr *aot_expr(AotUnit *u, Expr **slot){ (void)u; return *slot; }

int aot_is_annotation_only(AotUnit *u, Stmt *s){ (void)u; return s->kind==STMT_EXPR && s->ann_only; }

/* targets "=" ... value | target augop value | target ":" annotation ["=" value] */
AAssign *aot_assign(AotUnit *u, Stmt *s){
    if(s->aux) return (AAssign*)s->aux;
    AAssign *a=MPY_NEW0(AAssign);
    if(s->ntargets>16) parse_fail(u,s->line,"too many assignment targets");
    for(int i=0;i<s->ntargets;i++) a->target[a->ntarget++]=s->targets[i];
    a->value=s->value; a->aug=(TokKind)s->aug; a->ann=s->ann;
    s->aux=a; return a;
}
AWith *aot_with(AotUnit *u, Stmt *s){
    if(s->aux) return (AWith*)s->aux;
    AWith *w=MPY_NEW0(AWith);
    if(s->ntargets>8) parse_fail(u,s->line,"too many with items");
    for(int i=0;i<s->ntargets;i++){ w->e[w->n]=s->targets[i]; w->as[w->n]=s->withas[i]; w->n++; }
    s->aux=w; return w;
}
ADel *aot_del(AotUnit *u, Stmt *s){
    if(s->aux) return (ADel*)s->aux;
    ADel *d=MPY_NEW0(ADel);
    if(s->ntargets>16) parse_fail(u,s->line,"too many del targets");
    for(int i=0;i<s->ntargets;i++) d->t[d->n++]=s->targets[i];
    s->aux=d; return d;
}
/* print(a, b, ..., sep=..., end=...) */
APrint *aot_print(AotUnit *u, Stmt *s){
    if(s->kind!=STMT_EXPR || s->ann_only || !s->expr || s->expr->kind!=EXPR_CALL || s->expr->a->kind!=EXPR_NAME || strcmp(s->expr->a->name,"print")) return NULL;
    if(s->aux) return (APrint*)s->aux;
    APrint *pr=MPY_NEW0(APrint);
    Expr *call=s->expr;
    for(int i=0;i<call->count;i++){ Expr *v=call->items[i];
        if(v->akind==3){
            if(!strcmp(v->kw,"sep")) pr->sep=v; else if(!strcmp(v->kw,"end")) pr->end=v;
            else parse_fail(u,s->line,"print() takes only sep= and end= keywords");
        } else if(v->akind==2) parse_fail(u,s->line,"print(**kwargs) is not supported in compiled code");
        else {
            if(pr->n==32) parse_fail(u,s->line,"too many print arguments");
            pr->star[pr->n]=(char)(v->akind==1);
            pr->args[pr->n++]=v;
        }
        v->akind=0;                                         /* (values, as any other expression) */
    }
    s->aux=pr; return pr;
}

/* the module a from-import names: relative ones (from .m import x) resolved against the importing
   module's package; NULL (an error printed) if there is none */
char *aot_from_import_module(AotUnit *u, Stmt *s){
    if(!s->level) return xstrdup2(s->module);
    char base[600]; snprintf(base,sizeof base,"%s",u->name);
    const char *bn=u->path?strrchr(u->path,'/'):NULL; bn=bn?bn+1:u->path;
    int pkg= bn && !strncmp(bn,"__init__.",9);
    for(int i=pkg?1:0;i<s->level;i++){ char *dot=strrchr(base,'.');
        if(!dot || !strcmp(u->name,"__main__")){
            fprintf(stderr,"%s:%d: error: ImportError: attempted relative import %s\n",u->path?u->path:"<source>",s->line,
                    !strchr(u->name,'.') && !pkg ? "with no known parent package" : "beyond top-level package");
            return NULL; }
        *dot=0; }
    if(s->module){ size_t n=strlen(base); snprintf(base+n,sizeof base-n,".%s",s->module); }
    return xstrdup2(base);
}

/* ---------------------------------------------------------------- checker state */

typedef struct Ck {
    AProg *p;
    AModule *mod;           /* module being processed */
    AFunc *fn;              /* function being checked (module bodies included) */
    int changed;            /* some type known before this pass got bound */
    int pass_first;         /* first type id created in this pass */
    int strict;             /* final pass: anything still unknown is an error */
    struct { const char *name; ASym sym; AFunc *fn; } cscope[256];   /* comprehension / except / key variables, class attributes in scope (of fn) */
    int ncscope;
    Expr *coro_ok;          /* the coroutine call being awaited / handed to asyncio */
    int ep_value;           /* checking an argument for a minipy.Endpoint: an async function may be a value */
    int in_except;          /* inside an except clause (bare raise) */
    Ty **seen; int nseen, cseen;     /* strict pass: every expression type (unconstrained elements -> int) */
    jmp_buf fail;
    int reflected;          /* binop_type_m chose the right operand's __r<op>__ */
    AFunc *sig_fn; AClass *sig_cls;          /* the def / class whose annotations are being typed */
    const char **alias_n; Ty **alias_t; int nalias;   /* a generic alias's parameters while it is expanded */
    int dc_conv;            /* dataclasses.asdict / astuple are used: dataclasses get __mp_asdict__ / __mp_astuple__ */
    int nhidden;            /* hidden variables made for rewritten calls */
} Ck;

static Ty *ck_call_inner(Ck *c, Expr *e);
MPY_NORETURN static void err(Ck *c, int line, const char *fmt, ...){
    va_list ap; va_start(ap,fmt);
    AotUnit *u=c->mod?c->mod->unit:NULL;
    fprintf(stderr,"%s:%d: error: ",u&&u->path?u->path:"<source>",line);
    vfprintf(stderr,fmt,ap); fputc('\n',stderr);
    va_end(ap);
    longjmp(c->fail,1);
}

/* a function body (not a module body): def, lambda, generator expression */
static int is_func(AFunc *f){ return f && (f->def||f->lam||f->genexp); }
static void cs_push(Ck *c, const char *name, AVar *v, int line){
    if(c->ncscope==256) err(c,line,"nested too deeply");
    c->cscope[c->ncscope].name=name; c->cscope[c->ncscope].sym.name=(char*)name;
    c->cscope[c->ncscope].sym.kind=AS_VAR; c->cscope[c->ncscope].sym.p=v; c->cscope[c->ncscope].fn=c->fn; c->ncscope++;
}
/* a hidden variable of the current scope (comprehension / except / key variable) */
static AVar *new_global(Ck *c, AModule *m, const char *name);
static AVar *hidden_local(AFunc *f, const char *name, Ty *ty);
static AVar *scope_hidden(Ck *c, const char *name){ return is_func(c->fn) ? hidden_local(c->fn,name,NULL) : new_global(c,c->mod,name); }

static int occurs(Ty *v, Ty *t){
    t=ty_find(t); if(t==v) return 1;
    if(t->k==TY_TUPLE||t->k==TY_FUNC||t->k==TY_GEN){ for(int i=0;i<t->nelems;i++) if(t->elems[i] && occurs(v,t->elems[i])) return 1; if(t->k==TY_TUPLE) return 0; }
    if(t->k==TY_DICT && t->key && occurs(v,t->key)) return 1;
    return t->elem ? occurs(v,t->elem) : 0;
}
/* t (found, not shared) may be None from now on */
static void set_opt(Ck *c, Ty *t){ if(t->opt || ty_shared(t) || t->k==TY_VOID) return; t->opt=1; if(t->id<c->pass_first) c->changed=1; }
static void bind(Ck *c, Ty *var, Ty *t){
    if(var->opt && t->k==TY_VAR) set_opt(c,t);
    var->link=t; if(var->id<c->pass_first) c->changed=1;
}
static int unify(Ck *c, Ty *a, Ty *b){
    a=ty_find(a); b=ty_find(b);
    if(a==b) return 1;
    if(a->k==TY_VAR && b->k==TY_VAR){ if(a->id<b->id) bind(c,b,a); else bind(c,a,b); return 1; }   /* younger -> older */
    if(a->k==TY_VAR || b->k==TY_VAR){
        Ty *v=a->k==TY_VAR?a:b, *t=a->k==TY_VAR?b:a;
        if(occurs(v,t)) return 0;
        if(ty_shared(t)) t=ty_copy(t);                      /* a type of its own: it may become optional */
        if(v->opt) set_opt(c,t);
        bind(c,v,t); return 1;
    }
    if(a->k!=b->k) return 0;
    if(a->opt!=b->opt){ set_opt(c,a); set_opt(c,b); }       /* None flows both ways */
    if(a->k==TY_DICT && !unify(c,ty_dkey(a),ty_dkey(b))) return 0;
    if((a->k==TY_LIST||a->k==TY_FUNC||a->k==TY_GEN) && a->tup!=b->tup) return 0;
    if(a->k==TY_FUNC && a->ndef!=b->ndef){ int m=a->ndef<b->ndef?a->ndef:b->ndef; a->ndef=b->ndef=m; }   /* defaults: the ones both have */
    if(a->k==TY_FUNC && a->kwo!=b->kwo){ int m=a->kwo<b->kwo?a->kwo:b->kwo; a->kwo=b->kwo=m; }   /* keyword-only: the ones both have */
    if(a->k==TY_TYPE && a!=b && !ty_shared(a) && !ty_shared(b)){          /* class values: their common base (or a subclass of it) */
        if(a->cls && b->cls && a->cls!=b->cls){ AClass *cb=a->cls; while(cb && !aot_subclass(b->cls,cb)) cb=cb->base; a->cls=b->cls=cb; a->tup=b->tup=cb?1:0; }
        else if(a->cls && b->cls){ if(a->tup||b->tup) a->tup=b->tup=1; }
        else { a->cls=b->cls=NULL; a->tup=b->tup=0; } }
    if(a->k==TY_FUNC && !a->names && b->names) a->names=b->names;            /* (names: the ones a function gave) */
    if(a->k==TY_FUNC && !b->names && a->names) b->names=a->names;
    if(a->k==TY_GEN && a->nelems==2 && b->nelems==2){                  /* send() values; return values unless one is None */
        if(!unify(c,a->elems[0],b->elems[0])) return 0;
        if(ty_find(a->elems[1])->k!=TY_VOID && ty_find(b->elems[1])->k!=TY_VOID && !unify(c,a->elems[1],b->elems[1])) return 0; }
    if(a->k==TY_TUPLE||a->k==TY_FUNC){ if(a->nelems!=b->nelems) return 0; if(a->k==TY_TUPLE && !names_eq(a,b)) return 0; for(int i=0;i<a->nelems;i++) if(!unify(c,a->elems[i],b->elems[i])) return 0; if(a->k==TY_TUPLE) return 1; }
    if(has_elem(a)) return unify(c,a->elem,b->elem);
    if(a->k==TY_OBJ) return a->cls==b->cls;
    return 1;
}
/* May a value of type src be stored where dst is expected? (instances: subclass -> base) */
/* a function taking *args where a fixed signature is wanted: an adapter packs the extra arguments */
static int func_adaptable(Ck *c, Ty *d, Ty *s){
    int nreg=s->nelems-1-((s->tup>>1)&1);
    if(d->nelems<nreg) return 0;
    for(int i=0;i<nreg;i++) if(!unify(c,d->elems[i],s->elems[i])) return 0;
    for(int i=nreg;i<d->nelems;i++) if(!unify(c,d->elems[i],s->elems[nreg])) return 0;
    return unify(c,d->elem,s->elem);
}
static AClass *complex_class;                                    /* (the cmath prelude's) */
int aot_is_complex(Ty *t){ t=ty_find(t); return complex_class && t->k==TY_OBJ && t->cls==complex_class; }
static int assignable(Ck *c, Ty *dst, Ty *src){
    Ty *d=ty_find(dst), *s=ty_find(src);
    if(aot_is_complex(d) && (s->k==TY_INT||s->k==TY_BOOL||s->k==TY_FLOAT)) return 1;   /* 1 + 2j: converted on the way */
    if(d->k==TY_FUNC && (d->tup&4) && s->k==TY_FUNC && !(s->tup&4)) return 1;   /* a function -> its endpoint adapter */
    if(s->k==TY_FUNC && (s->tup&4) && d->k==TY_FUNC && !(d->tup&4)){ Ty *p=ty_find(d->elems[0]); /* an Endpoint is also a plain Callable */
        return d->nelems==1 && !d->tup && unify(c,p,s->elems[0]) && unify(c,d->elem,s->elem); }
    if(d->k==TY_FUNC && s->k==TY_FUNC && !d->tup && (s->tup&1)) return func_adaptable(c,d,s);
    if(d->k==TY_OBJ && s->k==TY_OBJ) return aot_subclass(s->cls,d->cls);
    if(d->k==TY_FLOAT && (s->k==TY_INT||s->k==TY_BOOL)) return 1;      /* converted on the way */
    if(d->k==TY_INT && s->k==TY_BOOL) return 1;
    return unify(c,d,s);
}
static void expect(Ck *c, Ty *dst, Ty *src, int line, const char *what){
    if(!assignable(c,dst,src)) err(c,line,"type mismatch: %s is %s, cannot take %s",what,ty_name(dst),ty_name(src));
}
static int numeric(Ty *t){ t=ty_find(t); return t->k==TY_INT||t->k==TY_BOOL||t->k==TY_FLOAT; }
/* values sorted(), min() and max() can order (as CPython can): numbers, str,
   tuples, lists of such, objects with __lt__ */
static int orderable(Ty *t){
    t=ty_find(t);
    if(numeric(t)||t->k==TY_STR||t->k==TY_BYTES||t->k==TY_TUPLE||t->k==TY_VAR) return 1;
    if(t->k==TY_LIST) return orderable(t->elem);
    if(t->k==TY_OBJ) return aot_find_method(t->cls,"__lt__")!=NULL;
    return 0;
}
static int is_var(Ty *t){ return ty_find(t)->k==TY_VAR; }

/* ---------------------------------------------------------------- type annotations */

static ASym *module_sym(AModule *m, const char *name){ return symtab_find(&m->syms,name); }

/* ---- built-in exception classes: BaseException (field _msg: str) and its subclasses ---- */
static SymTab builtin_syms;
static AClass *exc_root;
int aot_is_exception(AClass *c){ return exc_root && aot_subclass(c,exc_root); }
static ASym *global_sym(AModule *m, const char *name){ ASym *s=module_sym(m,name); return s?s:symtab_find(&builtin_syms,name); }
static AField *add_field(AClass *cls, const char *name, Ty *ty, Expr *init);
static Ty *optional(Ty *t);
static void make_builtin_exceptions(AProg *p, AModule *m){
    static const char *tree[][2]={
        {"BaseException",NULL},{"Exception","BaseException"},{"ArithmeticError","Exception"},{"ZeroDivisionError","ArithmeticError"},
        {"LookupError","Exception"},{"IndexError","LookupError"},{"KeyError","LookupError"},{"ValueError","Exception"},
        {"TypeError","Exception"},{"AttributeError","Exception"},{"RuntimeError","Exception"},{"NotImplementedError","RuntimeError"},
        {"AssertionError","Exception"},{"OSError","Exception"},{"MemoryError","Exception"},{"NameError","Exception"},{"ImportError","Exception"},
        {"OverflowError","ArithmeticError"},{"StopIteration","Exception"},{"InvalidStateError","Exception"},
        {"FileNotFoundError","OSError"},{"EOFError","Exception"},{"UnicodeError","ValueError"},{"UnicodeDecodeError","UnicodeError"},{"UnicodeEncodeError","UnicodeError"},{"KeyboardInterrupt","BaseException"},
        {"SystemExit","BaseException"},{"PermissionError","OSError"},{"FileExistsError","OSError"},{"IsADirectoryError","OSError"},
        {"NotADirectoryError","OSError"},{"TimeoutError","OSError"},{"ConnectionError","OSError"},{"UnboundLocalError","NameError"},
        {"RecursionError","RuntimeError"},{"StopAsyncIteration","Exception"},{"BufferError","Exception"},{"EnvironmentError","OSError"},
        {"IOError","OSError"},{"ModuleNotFoundError","ImportError"},{"FrozenInstanceError","AttributeError"},{"GeneratorExit","BaseException"},{NULL,NULL}};
    memset(&builtin_syms,0,sizeof builtin_syms); exc_root=NULL;
    for(int i=0;tree[i][0];i++){
        AClass *cls=MPY_NEW0(AClass); cls->name=xstrdup2(tree[i][0]); cls->mod=m; cls->builtin=1; cls->id=p->nclasses;
        cls->qualname=cls->name; cls->symname=cls->name;
        if(tree[i][1]) cls->base=(AClass*)symtab_find(&builtin_syms,tree[i][1])->p;
        else { exc_root=cls; add_field(cls,"_msg",TY_STR_T,NULL);
            Ty *ex=optional(ty_new(TY_OBJ,NULL,cls));
            add_field(cls,"__cause__",ex,NULL); add_field(cls,"__context__",ex,NULL); add_field(cls,"__suppress_context__",ty_copy(TY_BOOL_T),NULL);
            add_field(cls,"_rawrepr",ty_copy(TY_BOOL_T),NULL); }       /* the message is the repr already (KeyError('k'), ValueError(1)) */
        if(p->nclasses==p->ccap){ p->ccap=p->ccap?p->ccap*2:16; p->classes=(AClass**)xrealloc(p->classes,sizeof(AClass*)*(size_t)p->ccap); }
        p->classes[p->nclasses++]=cls;
        symtab_add(&builtin_syms,cls->name,AS_CLASS,cls);
        if(!strcmp(cls->name,"StopIteration")){ Ty *v=ty_var(); v->opt=1; add_field(cls,"value",v,NULL); }   /* a generator's return value (None) */
    }
}
/* abc.ABC: a plain base class (made once a module imports it) */
static AClass *abc_class(Ck *c){
    ASym *s=symtab_find(&builtin_syms,"ABC"); if(s) return (AClass*)s->p;
    AProg *p=c->p; AClass *cls=MPY_NEW0(AClass); cls->name=xstrdup2("ABC"); cls->qualname=cls->name; cls->symname=cls->name; cls->mod=p->mods[0]; cls->builtin=1; cls->filled=1; cls->id=p->nclasses;
    if(p->nclasses==p->ccap){ p->ccap=p->ccap?p->ccap*2:16; p->classes=(AClass**)xrealloc(p->classes,sizeof(AClass*)*(size_t)p->ccap); }
    p->classes[p->nclasses++]=cls;
    symtab_add(&builtin_syms,cls->name,AS_CLASS,cls);
    return cls;
}
static AModule *find_module(AProg *p, const char *name){ for(int i=0;i<p->nmods;i++) if(!strcmp(p->mods[i]->name,name)) return p->mods[i]; return NULL; }

/* Optional[t]: a copy that may be None */
static Ty *optional(Ty *t){ Ty *f=ty_find(t); if(f->k==TY_VAR){ f->opt=1; return f; } Ty *n=ty_copy(f); n->opt=1; return n; }
/* minipy.Endpoint: Callable[[dict[str, str]], str] that any function converts
   to (an adapter made by the compiler, see ep_adapter) */
static Ty *endpoint_type(void){
    Ty *ps[1]; ps[0]=ty_dict(TY_STR_T,TY_STR_T);
    Ty *t=ty_func(ps,1,TY_STR_T); t->tup=4; return t;
}
static int is_endpoint(Ty *t){ t=ty_find(t); return t->k==TY_FUNC && (t->tup&4); }
/* a type annotation (its expression): NAME | NAME '[' type, ... ']' | module '.' NAME | None |
   "Class" (a forward reference) | T | None (Optional[T]) */
/* a type parameter (def f[T], class C[T], a TypeVar) named in an annotation -> its type variable */
static int names_has(char **v, int n, const char *name){ for(int i=0;i<n;i++) if(!strcmp(v[i],name)) return 1; return 0; }
static Ty *tparam_ty(Ck *c, AModule *m, const char *name){
    for(int i=c->nalias-1;i>=0;i--) if(!strcmp(c->alias_n[i],name)) return c->alias_t[i];
    AFunc *f=c->sig_fn ? c->sig_fn : c->fn;
    AClass *cls=c->sig_cls ? c->sig_cls : f ? f->cls : NULL;
    if(cls && cls->def && names_has(cls->def->tparams,cls->def->ntparams,name)){          /* the class's: shared by its methods */
        if(!cls->tpv){ cls->tpv=MPY_NEW_ARR(Ty*,cls->def->ntparams); for(int i=0;i<cls->def->ntparams;i++) cls->tpv[i]=ty_var(); }
        for(int i=0;i<cls->def->ntparams;i++) if(!strcmp(cls->def->tparams[i],name)) return cls->tpv[i]; }
    ASym *s=module_sym(m,name);
    int tv= s && s->kind==AS_TYPEVAR;
    if(f && f->def && (tv || names_has(f->def->tparams,f->def->ntparams,name))){          /* the function's own */
        for(int i=0;i<f->ntp;i++) if(!strcmp(f->tpn[i],name)) return f->tpv[i];
        f->tpn=(const char**)xrealloc(f->tpn,sizeof(char*)*(size_t)(f->ntp+1)); f->tpv=(Ty**)xrealloc(f->tpv,sizeof(Ty*)*(size_t)(f->ntp+1));
        f->tpn[f->ntp]=name; f->tpv[f->ntp]=ty_var(); return f->tpv[f->ntp++]; }
    return tv ? ty_var() : NULL;
}
static ASym *local_class(Ck *c, const char *name);
static AClass *class_instance(Ck *c, AClass *cls);
static int class_has_subclass(Ck *c, AClass *cls);
static Ty *type_of(Ck *c, Expr *e, int line){
    if(e->kind==EXPR_BINARY && e->op==T_PIPE){
        Ty *t=type_of(c,e->a,line), *u=type_of(c,e->b,line);
        if(ty_find(u)->k==TY_VOID) return optional(t);
        if(ty_find(t)->k==TY_VOID) return optional(u);
        err(c,line,"only T | None is supported in compiled code: one type per value");
    }
    if(e->kind==EXPR_NONE) return TY_VOID_T;
    if(e->kind==EXPR_LITERAL && e->tok->kind==T_STRING){
        Expr *x=py_front_expr(c->mod->unit->path,e->tok->text,line);
        if(!x) err(c,line,"unexpected token in type annotation");
        return type_of(c,x,line);
    }
    Expr *base=e, *targ=NULL;
    if(e->kind==EXPR_INDEX){ base=e->a; targ=e->b; }
    const char *name; AModule *m=c->mod;
    if(base->kind==EXPR_ATTRIBUTE){                                      /* module.Class, typing.List */
        Expr *chain[16]; int k=0; Expr *x=base;
        while(x->kind==EXPR_ATTRIBUTE && k<16){ chain[k++]=x; x=x->a; }
        if(x->kind!=EXPR_NAME) err(c,line,"expected a type");
        name=x->name;
        for(int j=k-1;j>=0;j--){ const char *next=chain[j]->name;
            ASym *s=j==k-1 ? local_class(c,name) : NULL; if(!s) s=module_sym(m,name);
            if(s && s->kind==AS_CLASS){ AClass *in=aot_nested_class(c->p,(AClass*)s->p,next);       /* Outer.Inner */
                if(!in) err(c,line,"class %s has no class '%s'",((AClass*)s->p)->name,next);
                if(j) { m=in->mod; name=in->symname; continue; }
                m=in->mod; name=in->symname; break; }
            if(s && s->kind==AS_SYS && (!strcmp((const char*)s->p,"typing")||!strcmp((const char*)s->p,"dataclasses"))){ name=next; continue; }
            if(s && s->kind==AS_SYS && !strcmp((const char*)s->p,"minipy") && !strcmp(next,"Endpoint")){
                if(j || targ) err(c,line,"unexpected token in type annotation");
                return endpoint_type(); }
            if(!s || s->kind!=AS_MODULE) err(c,line,"'%s' is not a module",name);
            m=(AModule*)s->p; name=next;
        }
    } else if(base->kind==EXPR_NAME && strcmp(base->name,"...")) name=base->name;
    else err(c,line,"expected a type");
    if(!strcmp(name,"ClassVar")||!strcmp(name,"Final")||!strcmp(name,"InitVar")||!strcmp(name,"Annotated")){   /* ClassVar[T], Final[T], InitVar[T]: a T */
        if(!targ) return ty_var();
        if(!strcmp(name,"Annotated")) return type_of(c,targ->kind==EXPR_TUPLE&&targ->count?targ->items[0]:targ,line);
        return type_of(c,targ,line); }
    if(!strcmp(name,"Callable") && targ && targ->kind==EXPR_TUPLE && targ->count==2 && targ->items[0]->kind==EXPR_NAME && !strcmp(targ->items[0]->name,"...")){
        Ty *v=ty_var(); return v; }                                    /* Callable[..., R]: the function given (its parameters as it has them) */
    if(!strcmp(name,"Callable")){                                       /* Callable[[A, B], R] */
        if(!targ || (targ->kind!=EXPR_LIST && !(targ->kind==EXPR_TUPLE && targ->count>=1 && targ->items[0]->kind==EXPR_LIST)))
            err(c,line,"Callable needs its parameter and result types: Callable[[int, str], bool]");
        if(targ->kind==EXPR_LIST || targ->count<2) err(c,line,"Callable needs a result type: Callable[[int], int]");
        if(targ->count>2) err(c,line,"expected ']' in Callable");
        Expr *pl=targ->items[0]; Ty *ps[16]; int np=0;
        for(int k=0;k<pl->count;k++){ if(np==16) err(c,line,"too many parameters"); ps[np++]=type_of(c,pl->items[k],line); }
        return ty_func(ps,np,type_of(c,targ->items[1],line));
    }
    Ty *args[16]={NULL}; int nargs=0, varlen=0;
    if(targ){
        Expr **v=targ->kind==EXPR_TUPLE ? targ->items : &targ; int n=targ->kind==EXPR_TUPLE ? targ->count : 1;
        for(int k=0;k<n;k++){
            if(v[k]->kind==EXPR_NAME && !strcmp(v[k]->name,"...")){ if(k!=n-1) err(c,line,"expected ']' in type"); varlen=1; break; }   /* tuple[int, ...] */
            if(nargs==16) err(c,line,"too many type arguments");
            args[nargs++]=type_of(c,v[k],line);
        }
    }
    static const char *aliases[][2]={{"List","list"},{"Dict","dict"},{"Set","set"},{"Tuple","tuple"},{"FrozenSet","set"},{"Sequence","list"},{"MutableSequence","list"},{"Mapping","dict"},{"MutableMapping","dict"},{"AbstractSet","set"},{NULL,NULL}};
    for(int k=0;aliases[k][0];k++) if(!strcmp(name,aliases[k][0])){ name=aliases[k][1]; break; }
    if(!strcmp(name,"Optional")){ if(nargs!=1) err(c,line,"Optional takes one type argument"); return optional(args[0]); }   /* None is the zero value anyway */
    if(!strcmp(name,"Union")){                                       /* Union[T, None] */
        if(nargs==2 && ty_find(args[1])->k==TY_VOID) return optional(args[0]);
        if(nargs==2 && ty_find(args[0])->k==TY_VOID) return optional(args[1]);
        if(nargs==1) return args[0];
        err(c,line,"only Union[T, None] (Optional[T]) is supported in compiled code: one type per value");
    }
    if(!strcmp(name,"AsyncIterator")||!strcmp(name,"AsyncIterable")||!strcmp(name,"AsyncGenerator")){   /* an async generator */
        if(nargs<1) err(c,line,"%s needs the item type: %s[int]",name,name);
        if(nargs>2 || (nargs>1 && strcmp(name,"AsyncGenerator"))) err(c,line,"%s takes %s",name,strcmp(name,"AsyncGenerator")?"one type argument":"two type arguments: AsyncGenerator[yield, send]");
        Ty *g=ty_new(TY_GEN,args[0],NULL); g->tup=1;
        if(nargs>1 && ty_find(args[1])->k!=TY_VOID) g->elems[0]=args[1];
        return g; }
    if(!strcmp(name,"Iterator")||!strcmp(name,"Iterable")||!strcmp(name,"Generator")){
        if(nargs<1) err(c,line,"%s needs the item type: %s[int]",name,name);
        if(nargs>1 && strcmp(name,"Generator")) err(c,line,"%s takes one type argument",name);
        if(nargs>3) err(c,line,"Generator takes three type arguments: Generator[yield, send, return]");
        Ty *g=ty_new(TY_GEN,args[0],NULL);
        if(nargs>1 && ty_find(args[1])->k!=TY_VOID) g->elems[0]=args[1];          /* Generator[Y, S, R] */
        if(nargs>2) g->elems[1]=args[2];
        return g;
    }
    if(varlen){
        if(strcmp(name,"tuple")||nargs!=1) err(c,line,"`...` is only supported as tuple[T, ...]");
        Ty *t=ty_new(TY_LIST,args[0],NULL); t->tup=1; return t;
    }
    if(!strcmp(name,"int")) return ty_copy(TY_INT_T);              /* (copies: an annotated variable may become optional) */
    if(!strcmp(name,"bool")) return ty_copy(TY_BOOL_T);
    if(!strcmp(name,"float")) return ty_copy(TY_FLOAT_T);
    if(!strcmp(name,"str")) return ty_copy(TY_STR_T);
    if(!strcmp(name,"bytes")) return ty_copy(TY_BYTES_T);
    if(!strcmp(name,"type")){                                          /* type[C]: C or a subclass of it */
        if(nargs==1 && ty_find(args[0])->k==TY_OBJ){ Ty *t=ty_new(TY_TYPE,NULL,ty_find(args[0])->cls); t->tup=1; return t; }
        return ty_copy(TY_TYPE_T); }
    if(!strcmp(name,"buffer")) return ty_copy(TY_BUF_T);
    if(!strcmp(name,"list")||!strcmp(name,"set")){
        if(nargs>1) err(c,line,"%s takes one type argument",name);
        return ty_new(name[0]=='l'?TY_LIST:TY_SET,nargs?args[0]:ty_var(),NULL);
    }
    if(!strcmp(name,"tuple")){
        if(!nargs) err(c,line,"tuple needs its item types: tuple[int, str]");
        for(int k=0;k<nargs;k++) if(ty_find(args[k])->k==TY_VOID) err(c,line,"a tuple item cannot be None");
        return ty_tuple(args,nargs);
    }
    if(nargs>2) err(c,line,"too many type arguments");
    if(!strcmp(name,"dict")){
        if(nargs==1) err(c,line,"dict needs two type arguments: dict[K, V]");
        if(nargs==2){ TyKind kk=ty_find(args[0])->k; if(kk!=TY_STR&&kk!=TY_BYTES&&kk!=TY_INT&&kk!=TY_BOOL&&kk!=TY_TUPLE&&kk!=TY_OBJ&&kk!=TY_FLOAT) err(c,line,"dictionary keys must be int, float, str, bytes, tuples or objects"); }
        return ty_dict(nargs?args[0]:ty_var(),nargs?args[1]:ty_var());
    }
    { Ty *tp=tparam_ty(c,m,name); if(tp){ if(nargs) err(c,line,"%s takes no type arguments",name); return tp; } }
    ASym *s=m==c->mod ? local_class(c,name) : NULL;                   /* a class of this function */
    if(!s) s=global_sym(m,name);
    if(s && s->kind==AS_TYPEALIAS){                                    /* type X = ..., type P[T] = tuple[T, T] */
        Stmt *al=(Stmt*)s->p;
        if(nargs && nargs!=al->ntparams) err(c,line,"%s takes %d type argument%s",name,al->ntparams,al->ntparams==1?"":"s");
        int save=c->nalias;
        for(int k=0;k<al->ntparams;k++){ c->alias_n=(const char**)xrealloc(c->alias_n,sizeof(char*)*(size_t)(c->nalias+1)); c->alias_t=(Ty**)xrealloc(c->alias_t,sizeof(Ty*)*(size_t)(c->nalias+1));
            c->alias_n[c->nalias]=al->tparams[k]; c->alias_t[c->nalias]=k<nargs?args[k]:ty_var(); c->nalias++; }
        AModule *savem=c->mod; c->mod=m;
        Ty *t=type_of(c,al->ann,line);
        c->mod=savem; c->nalias=save;
        return t; }
    if(s && s->kind==AS_CLASS){ AClass *k=(AClass*)s->p;
        if(nargs && !(k->def && k->def->ntparams)) err(c,line,"class %s takes no type arguments",name);
        if(nargs && k->generic && !class_has_subclass(c,k)){                   /* Box[int]: the instance for those type arguments */
            if(nargs!=k->def->ntparams) err(c,line,"%s takes %d type argument%s",name,k->def->ntparams,k->def->ntparams==1?"":"s");
            { AFunc *cf=c->sig_fn ? c->sig_fn : c->fn; AClass *cur=c->sig_cls ? c->sig_cls : cf ? cf->cls : NULL;   /* Box[T] in Box's own body: this instance */
              if(cur && (cur==k || cur->origin==k) && cur->tpv){ int own=1;
                  for(int i=0;i<nargs;i++) if(ty_find(args[i])!=ty_find(cur->tpv[i])) own=0;
                  if(own) return ty_new(TY_OBJ,NULL,cur); } }
            AClass *pick=NULL;
            for(int j=-1;j<k->ninsts && !pick;j++){ AClass *cand=j<0?k:k->insts[j];
                if(!cand->tpv){ cand->tpv=MPY_NEW_ARR(Ty*,nargs); for(int i=0;i<nargs;i++) cand->tpv[i]=ty_var(); }
                int ok=1; for(int i=0;i<nargs && ok;i++){ Ty *q=ty_find(cand->tpv[i]), *a=ty_find(args[i]); if(q->k!=TY_VAR && a->k!=TY_VAR && !ty_same(q,a)) ok=0; }
                if(ok) pick=cand; }
            if(!pick){ pick=class_instance(c,k); if(!pick->tpv){ pick->tpv=MPY_NEW_ARR(Ty*,nargs); for(int i=0;i<nargs;i++) pick->tpv[i]=ty_var(); } }
            for(int i=0;i<nargs;i++) unify(c,pick->tpv[i],args[i]);
            return ty_new(TY_OBJ,NULL,pick); }
        return ty_new(TY_OBJ,NULL,k); }
    if(s && s->kind==AS_SYS && !strcmp((const char*)s->p,"minipy.Endpoint")) return endpoint_type();
    err(c,line,"unknown type '%s'",name);
}
static Ty *annotation(Ck *c, Expr *e){ return e ? type_of(c,e,e->line) : NULL; }

/* ---------------------------------------------------------------- names bound in a scope */

typedef struct { char **v; int n, cap; } Names;
static void names_add(Names *s, const char *name){
    for(int i=0;i<s->n;i++) if(!strcmp(s->v[i],name)) return;
    if(s->n==s->cap){ s->cap=s->cap?s->cap*2:8; s->v=(char**)xrealloc(s->v,sizeof(char*)*(size_t)s->cap); }
    s->v[s->n++]=(char*)name;
}
static void names_target(Expr *t, Names *out){
    if(t->kind==EXPR_NAME) names_add(out,t->name);
    else if(t->kind==EXPR_TUPLE||t->kind==EXPR_LIST) for(int i=0;i<t->count;i++) names_target(t->items[i],out);
}
static void names_expr(Expr *e, Names *out){          /* only `name := value` binds (comprehension variables are their own) */
    if(!e || e->kind==EXPR_LAMBDA) return;
    if(e->kind==EXPR_WALRUS) names_add(out,e->name);
    names_expr(e->a,out); names_expr(e->b,out); names_expr(e->c,out); names_expr(e->d,out);
    for(int i=0;i<e->count;i++) names_expr(e->items[i],out);
    for(int i=0;i<e->vcount;i++) names_expr(e->vals[i],out);
    if(e->kind==EXPR_COMPREHENSION) for(int i=0;i<e->nclause;i++){ names_expr(e->clauses[i].iter,out); for(int k=0;k<e->clauses[i].ncond;k++) names_expr(e->clauses[i].conds[k],out); }
}
/* Names assigned by statements (not descending into def/class bodies). */
static void pattern_names(Expr *p, Names *out){              /* the names a match pattern binds */
    if(!p) return;
    if(p->kind!=EXPR_PATTERN){ return; }
    if((p->akind==PAT_AS || p->akind==PAT_STAR || p->akind==PAT_MAP) && p->name) names_add(out,p->name);
    if(p->akind==PAT_AS) pattern_names(p->a,out);
    if(p->akind!=PAT_MAP) for(int i=0;i<p->count;i++) pattern_names(p->items[i],out);
    else for(int i=0;i<p->vcount;i++) pattern_names(p->vals[i],out);
}
static void names_bound(Ck *c, Stmt **b, int n, Names *out, Names *globals){
    AotUnit *u=c->mod->unit;
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        switch(s->kind){
            case STMT_ASSIGN:{ AAssign *a=aot_assign(u,s); for(int k=0;k<a->ntarget;k++){ names_target(a->target[k],out); names_expr(a->target[k],out); } names_expr(a->value,out); break; }
            case STMT_EXPR:{ APrint *pr=aot_print(u,s);
                if(pr){ for(int k=0;k<pr->n;k++) names_expr(pr->args[k],out); }
                else if(aot_is_annotation_only(u,s)) names_add(out,s->targets[0]->name);
                else names_expr(aot_expr(u,&s->expr),out);
                break; }
            case STMT_FOR: for(int k=0;k<s->param_count;k++) names_add(out,s->params[k]);
                names_expr(aot_expr(u,&s->expr),out);
                names_bound(c,s->body,s->body_count,out,globals); names_bound(c,s->orelse,s->orelse_count,out,globals); break;
            case STMT_IF: case STMT_WHILE: names_expr(aot_expr(u,&s->expr),out);
                names_bound(c,s->body,s->body_count,out,globals); names_bound(c,s->orelse,s->orelse_count,out,globals); break;
            case STMT_WITH:{ AWith *w=aot_with(u,s); for(int k=0;k<w->n;k++){ names_expr(w->e[k],out); if(w->as[k]) names_add(out,w->as[k]); if(s->withtgt && s->withtgt[k]) names_target(s->withtgt[k],out); }
                names_bound(c,s->body,s->body_count,out,globals); break; }
            case STMT_TRY: case STMT_BLOCK: names_bound(c,s->body,s->body_count,out,globals); names_bound(c,s->orelse,s->orelse_count,out,globals); break;
            case STMT_RETURN: case STMT_RAISE: names_expr(aot_expr(u,&s->expr),out); break;
            case STMT_ASSERT: names_expr(aot_expr(u,&s->expr),out); names_expr(aot_expr(u,&s->expr2),out); break;
            case STMT_GLOBAL: case STMT_NONLOCAL: if(globals) for(int k=0;k<s->param_count;k++) names_add(globals,s->params[k]); break;
            case STMT_FUNCTION_DEF: names_add(out,s->name); break;
            case STMT_MATCH: names_expr(s->expr,out);
                for(int k=0;k<s->body_count;k++){ Stmt *cs=s->body[k]; pattern_names(cs->expr,out); if(cs->expr2) names_expr(cs->expr2,out); names_bound(c,cs->body,cs->body_count,out,globals); }
                break;
            default: break;
        }
    }
}
static void collect_nonlocals(Stmt **b, int n, Names *out){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_NONLOCAL) for(int k=0;k<s->param_count;k++) names_add(out,s->params[k]);
        if(s->kind==STMT_FUNCTION_DEF||s->kind==STMT_CLASS_DEF) continue;
        collect_nonlocals(s->body,s->body_count,out); collect_nonlocals(s->orelse,s->orelse_count,out);
    }
}
static int has_return_value(Stmt **b, int n){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_RETURN && s->expr) return 1;
        if(s->kind==STMT_FUNCTION_DEF||s->kind==STMT_CLASS_DEF) continue;
        if(has_return_value(s->body,s->body_count)||has_return_value(s->orelse,s->orelse_count)) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- building the model */

static AVar *new_global(Ck *c, AModule *m, const char *name){
    AProg *p=c->p; AVar *v=MPY_NEW0(AVar);
    v->name=xstrdup2(name); v->ty=ty_var(); v->global=1; v->mod=m; v->id=p->nglobals;
    if(p->nglobals==p->gcap){ p->gcap=p->gcap?p->gcap*2:32; p->globals=(AVar**)xrealloc(p->globals,sizeof(AVar*)*(size_t)p->gcap); }
    p->globals[p->nglobals++]=v;
    return v;
}
static AVar *global_var(Ck *c, AModule *m, const char *name, int line){
    ASym *s=module_sym(m,name);
    if(!s) return (AVar*)symtab_add(&m->syms,name,AS_VAR,new_global(c,m,name))->p;
    if(s->kind!=AS_VAR) err(c,line,"'%s' is a %s and cannot be assigned",name,s->kind==AS_FUNC?"function":s->kind==AS_CLASS?"class":"module");
    return (AVar*)s->p;
}
static AVar *hidden_local(AFunc *f, const char *name, Ty *ty){   /* a local slot no name refers to */
    AVar *v=MPY_NEW0(AVar); v->name=xstrdup2(name); v->ty=ty?ty:ty_var(); v->mod=f->mod; v->id=f->nvars;
    v->owner=f; v->bind_top=-1; v->first_use_top=0x7FFFFFFF;
    if(f->nvars==f->vcap){ f->vcap=f->vcap?f->vcap*2:8; f->vars=(AVar**)xrealloc(f->vars,sizeof(AVar*)*(size_t)f->vcap); }
    f->vars[f->nvars++]=v;
    return v;
}
static AVar *new_local(AFunc *f, const char *name, Ty *ty){
    AVar *v=hidden_local(f,name,ty);
    symtab_add(&f->locals,name,AS_VAR,v);
    return v;
}
static void add_func(AProg *p, AFunc *f){
    if(p->nfuncs==p->fcap){ p->fcap=p->fcap?p->fcap*2:32; p->funcs=(AFunc**)xrealloc(p->funcs,sizeof(AFunc*)*(size_t)p->fcap); }
    f->id=p->nfuncs; p->funcs[p->nfuncs++]=f;
}
static void no_nested_defs(Ck *c, Stmt **b, int n, int in_func){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_FUNCTION_DEF){
            if(!in_func) err(c,s->line,"define '%s' at the top level of the module (or inside a function) in compiled code",s->name);
            continue;                                   /* its body is checked by its own new_func */
        }
        if(s->kind==STMT_CLASS_DEF){
            if(!in_func) err(c,s->line,"define class '%s' at the top level of the module (or inside a function) in compiled code",s->name);
            continue; }
        no_nested_defs(c,s->body,s->body_count,in_func); no_nested_defs(c,s->orelse,s->orelse_count,in_func);
    }
}
/* what decorator i of a def is: 0 a general one (name = d(function)),
   1 @staticmethod, 2 @property, 3 ignored (@wraps(f): only copies names) */
static int is_abstract_deco(Expr *d){
    return (d->kind==EXPR_NAME && !strcmp(d->name,"abstractmethod")) || (d->kind==EXPR_ATTRIBUTE && !strcmp(d->name,"abstractmethod") && d->a->kind==EXPR_NAME && !strcmp(d->a->name,"abc"));
}
static int deco_kind(AotUnit *u, Stmt *def, int i){
    Expr *d=aot_expr(u,&def->decorator_exprs[i]);
    if(is_abstract_deco(d)) return 3;                           /* @abstractmethod: see is_abstract */
    if(d->kind==EXPR_NAME && !strcmp(d->name,"staticmethod")) return 1;
    if(d->kind==EXPR_NAME && !strcmp(d->name,"property")) return 2;
    if(d->kind==EXPR_CALL){ Expr *f=d->a;
        if((f->kind==EXPR_NAME && !strcmp(f->name,"wraps")) || (f->kind==EXPR_ATTRIBUTE && !strcmp(f->name,"wraps") && f->a->kind==EXPR_NAME && !strcmp(f->a->name,"functools"))) return 3; }
    return 0;
}
static int general_decos(AotUnit *u, Stmt *def, int in_class){
    int n=0; for(int i=0;i<def->decorator_count;i++){ int k=deco_kind(u,def,i); if(k==0 || (!in_class && k!=3)) n++; } return n;
}
static char *hidden_name(const char *a, const char *b){        /* a name no source can spell */
    char buf[300]; snprintf(buf,sizeof buf," %s%s%s",a,b?".":"",b?b:""); return xstrdup2(buf);
}
static Expr *name_expr(const char *name, int line);
/* name = d1(d2(fnref)): the decorators' expressions applied, innermost first */
static ASym *lookup(Ck *c, const char *name);
static const char *instantiate(Ck *c, AFunc *fn);
static Expr *deco_app(Ck *c, AotUnit *u, Stmt *def, const char *fnref, int in_class){
    if(def->expr2) return def->expr2;
    Expr *cur=name_expr(fnref,def->line);
    for(int i=def->decorator_count-1;i>=0;i--){ int k=deco_kind(u,def,i);
        if(k==3 || (in_class && k)) continue;
        Expr *d=def->decorator_exprs[i];
        Expr *nm=d->kind==EXPR_NAME?d:d->kind==EXPR_CALL&&d->a->kind==EXPR_NAME?d->a:NULL;
        if(nm){ ASym *x=lookup(c,nm->name);                 /* a generic decorator function: an instance of its own */
            if(x && x->kind==AS_FUNC && ((AFunc*)x->p)->pristine) nm->name=(char*)instantiate(c,(AFunc*)x->p); }
        Expr *call=xnew(EXPR_CALL,def->line); call->a=d; xpush(call,cur); cur=call; }
    def->expr2=cur;
    return cur;
}
static int has_yield(Stmt **b, int n){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_YIELD) return 1;
        if(s->kind==STMT_FUNCTION_DEF||s->kind==STMT_CLASS_DEF) continue;
        if(has_yield(s->body,s->body_count)||has_yield(s->orelse,s->orelse_count)) return 1;
    }
    return 0;
}
/* Binding statements of each local: closures copy a variable bound once
   before them; anything else is shared through a cell. */
static void count_bind(AFunc *f, const char *name, int top, int nested){
    ASym *s=symtab_find(&f->locals,name); if(!s || s->kind!=AS_VAR) return;
    AVar *v=(AVar*)s->p; v->nbind++; v->bind_top=(v->nbind==1 && !nested) ? top : -1;
}
static void count_bind_target(AFunc *f, Expr *t, int top, int nested){
    if(t->kind==EXPR_NAME) count_bind(f,t->name,top,nested);
    else if(t->kind==EXPR_TUPLE||t->kind==EXPR_LIST) for(int i=0;i<t->count;i++) count_bind_target(f,t->items[i],top,nested);
}
static void count_bindings(AFunc *f, Stmt **b, int n, int top, int nested){
    AotUnit *u=f->mod->unit;
    for(int i=0;i<n;i++){ Stmt *s=b[i]; int t=nested?top:i;
        switch(s->kind){
            case STMT_ASSIGN:{ AAssign *a=aot_assign(u,s); if(!a->value) break; for(int k=0;k<a->ntarget;k++) count_bind_target(f,a->target[k],t,nested); break; }
            case STMT_FOR: for(int k=0;k<s->param_count;k++) count_bind(f,s->params[k],t,1);
                count_bindings(f,s->body,s->body_count,t,1); count_bindings(f,s->orelse,s->orelse_count,t,1); break;
            case STMT_WITH:{ AWith *w=aot_with(u,s); for(int k=0;k<w->n;k++) if(w->as[k]) count_bind(f,w->as[k],t,nested);
                count_bindings(f,s->body,s->body_count,t,1); break; }
            case STMT_FUNCTION_DEF: count_bind(f,s->name,t,nested); break;
            case STMT_MATCH: for(int k=0;k<s->body_count;k++){ Names pn={0}; pattern_names(s->body[k]->expr,&pn); for(int q=0;q<pn.n;q++) count_bind(f,pn.v[q],t,1); free(pn.v); }
                count_bindings(f,s->body,s->body_count,t,1); break;
            case STMT_IF: case STMT_WHILE: case STMT_TRY: case STMT_BLOCK: case STMT_CASE:
                count_bindings(f,s->body,s->body_count,t,1); count_bindings(f,s->orelse,s->orelse_count,t,1); break;
            default: break;
        }
    }
}
static AFunc *new_func(Ck *c, AFunc *f, AModule *m, AClass *cls, Stmt *def, AFunc *outer);
static AClass *new_inner_class(Ck *c, AModule *m, Stmt *s, AClass *outer, AFunc *encl);
static void fill_class(Ck *c, AClass *cls);
/* nested defs of f's body (not of their own bodies) */
static void collect_nested(Ck *c, AFunc *f, Stmt **b, int n){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_CLASS_DEF){                        /* a class of the function: hoisted, seen by name in its body */
            ASym *x=symtab_find(&f->locals,s->name);
            if(x) err(c,s->line,"'%s' is defined twice in %s()",s->name,f->name);
            AClass *cls=new_inner_class(c,f->mod,s,NULL,f);
            symtab_add(&f->locals,s->name,AS_CLASS,cls);
            continue; }
        if(s->kind==STMT_FUNCTION_DEF){
            AFunc *inner=new_func(c,NULL,f->mod,NULL,s,f);
            s->aux=inner;
            if(inner->ndeco) symtab_add(&f->locals,hidden_name(s->name,NULL),AS_FUNC,inner);   /* the undecorated function */
            continue;
        }
        collect_nested(c,f,s->body,s->body_count); collect_nested(c,f,s->orelse,s->orelse_count);
    }
}
static void set_fn_const(AFunc *f, Stmt **b, int n){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_FUNCTION_DEF){ ASym *x=symtab_find(&f->locals,s->name);
            if(x && x->kind==AS_VAR && ((AVar*)x->p)->nbind==1 && !((AFunc*)s->aux)->ndeco){ ((AVar*)x->p)->fn_const=(AFunc*)s->aux; ((AFunc*)s->aux)->selfvar=(AVar*)x->p; }
            continue; }
        set_fn_const(f,s->body,s->body_count); set_fn_const(f,s->orelse,s->orelse_count);
    }
}

/* f: a function object made earlier (module-level functions exist before imports are linked) or NULL */
static AFunc *new_func(Ck *c, AFunc *f, AModule *m, AClass *cls, Stmt *def, AFunc *outer){
    if(!f) f=MPY_NEW0(AFunc);
    f->name=xstrdup2(def->name); f->mod=m; f->cls=cls; f->def=def; f->body=def->body; f->nbody=def->body_count; f->line=def->line;
    f->outer=outer;
    for(int i=0;i<def->decorator_count;i++){
        if(cls && is_abstract_deco(aot_expr(m->unit,&def->decorator_exprs[i]))) f->is_abstract=1;
        int k=deco_kind(m->unit,def,i);
        if(cls && k==1) f->is_static=1;
        else if(cls && k==2) f->is_property=1;
        else if(k!=3) f->ndeco++;
    }
    if(f->ndeco && (f->is_property || (cls && !strcmp(def->name,"__init__")))) err(c,def->line,"%s.%s: decorators cannot be combined with @property or __init__ in compiled code",cls?cls->name:"",def->name);
    if(def->param_count>16) err(c,def->line,"too many parameters");
    no_nested_defs(c,def->body,def->body_count,1);
    f->nparams=def->param_count;
    f->star=def->star_index; f->dstar=def->dstar_index;
    f->kwonly=def->kwonly_index>=0 ? def->kwonly_index : f->star>=0 ? f->star+1 : f->dstar>=0 ? f->dstar : f->nparams;
    f->params=MPY_NEW_ARR(AVar*,f->nparams>0?f->nparams:1);
    f->defaults=MPY_NEW_ARR(Expr*,f->nparams>0?f->nparams:1);
    AFunc *save_sig=c->sig_fn; c->sig_fn=f; f->cls=cls;
    for(int i=0;i<f->nparams;i++){
        Ty *t=annotation(c,def->annotations && i<def->annotation_cap ? def->annotations[i] : NULL);
        if(i==f->star){ Ty *l=ty_new(TY_LIST,t?t:ty_var(),NULL); l->tup=1; t=l; }          /* *args: a tuple of them */
        else if(i==f->dstar) t=ty_dict(TY_STR_T,t?t:ty_var());                          /* **kwargs: a dict by name */
        if(cls && !f->is_static && i==0 && i!=f->star){
            Ty *self=ty_new(TY_OBJ,NULL,cls);
            if(t && !ty_same(t,self)) err(c,def->line,"'%s' of %s.%s must be %s",def->params[0],cls->name,f->name,cls->name);
            t=self;
        }
        if(t && ty_find(t)->k==TY_VOID) err(c,def->line,"parameter '%s' cannot be None",def->params[i]);
        f->params[i]=new_local(f,def->params[i],t);
        f->defaults[i]=NULL;
    }
    if(cls && !f->is_static && f->nparams==0) err(c,def->line,"method %s.%s needs a self parameter",cls->name,f->name);
    if(def->pdefaults) for(int i=0;i<f->nparams;i++) f->defaults[i]=def->pdefaults[i];
    f->returns_value=has_return_value(def->body,def->body_count);
    f->is_gen=has_yield(def->body,def->body_count) || def->yield_expr;
    if(f->is_gen){
        if(def->returns){ f->ret=annotation(c,def->returns); if(ty_find(f->ret)->k!=TY_GEN) err(c,def->line,"generator %s() must be annotated -> %s",f->name,def->is_async?"AsyncIterator[T] (or AsyncGenerator[T, S])":"Iterator[T] (or Generator[T, S, R])"); }
        else f->ret=ty_new(TY_GEN,ty_var(),NULL);
        if(def->is_async){ Ty *g=ty_find(f->ret); if(!g->tup && def->returns) err(c,def->line,"async generator %s() must be annotated -> AsyncIterator[T]",f->name); g->tup=1; }   /* an async generator (awaits run in it) */
        if(f->returns_value){ Ty *g=ty_find(f->ret);                          /* return v: StopIteration.value, what yield from gives */
            if(!def->returns) g->elems[1]=ty_var();
            else if(ty_find(g->elems[1])->k==TY_VOID) err(c,def->line,"generator %s() returns a value: annotate it -> Generator[T, S, R]",f->name); }
        f->yield_ty=ty_find(f->ret)->elem;
    }
    else if(def->returns){ f->ret=annotation(c,def->returns); if(ty_find(f->ret)->k==TY_VOID && f->returns_value) err(c,def->line,"%s() is annotated -> None but returns a value",f->name); }
    else f->ret=f->returns_value ? ty_var() : TY_VOID_T;
    c->sig_fn=save_sig;
    if(cls && !strcmp(f->name,"__init__") && ty_find(f->ret)->k!=TY_VOID) err(c,def->line,"__init__ must not return a value");
    f->is_async=def->is_async;
    if(f->is_async && cls && (f->is_static || (!strncmp(f->name,"__",2) && strcmp(f->name,"__aenter__") && strcmp(f->name,"__aexit__") && strcmp(f->name,"__anext__")))) err(c,def->line,"%s.%s cannot be async in compiled code",cls->name,f->name);
    Names locals={0}, globals={0}, nonlocals={0}, decl={0};
    names_bound(c,def->body,def->body_count,&locals,&decl);    /* decl: global and nonlocal names */
    collect_nonlocals(def->body,def->body_count,&nonlocals);
    for(int i=0;i<decl.n;i++){ int nl=0; for(int k=0;k<nonlocals.n;k++) if(!strcmp(nonlocals.v[k],decl.v[i])) nl=1; if(!nl) names_add(&globals,decl.v[i]); }
    if(nonlocals.n && !outer) err(c,def->line,"nonlocal outside a nested function");
    f->globals_decl=globals.v; f->nglobals_decl=globals.n;
    for(int i=0;i<globals.n;i++) global_var(c,m,globals.v[i],def->line);
    for(int i=0;i<locals.n;i++){
        int isdecl=0; for(int k=0;k<decl.n;k++) if(!strcmp(decl.v[k],locals.v[i])) isdecl=1;
        if(!isdecl && !symtab_find(&f->locals,locals.v[i])) new_local(f,locals.v[i],NULL);
    }
    free(decl.v); free(nonlocals.v);
    free(locals.v);
    add_func(c->p,f);
    collect_nested(c,f,def->body,def->body_count);
    for(int i=0;i<c->p->nclasses;i++) if(c->p->classes[i]->encl==f) fill_class(c,c->p->classes[i]);   /* its classes */
    count_bindings(f,def->body,def->body_count,0,0);
    set_fn_const(f,def->body,def->body_count);
    return f;
}
/* a lambda expression's function (made once; checked inline where it appears) */
static AFunc *new_lambda(Ck *c, Expr *e){
    AFunc *f=MPY_NEW0(AFunc);
    f->name="<lambda>"; f->mod=c->mod; f->outer=c->fn; f->lam=e; f->line=e->line; f->star=f->dstar=-1;
    if(e->neparam>16) err(c,e->line,"too many parameters");
    f->nparams=e->neparam; f->kwonly=f->nparams;
    if(e->edefaults){ f->star=e->estar; f->dstar=e->edstar; f->kwonly=e->ekwonly; }          /* (lambdas the compiler makes have none) */
    f->params=MPY_NEW_ARR(AVar*,f->nparams>0?f->nparams:1);
    f->defaults=MPY_NEW_ARR(Expr*,f->nparams>0?f->nparams:1);
    for(int i=0;i<f->nparams;i++){ if(symtab_find(&f->locals,e->eparams[i])) err(c,e->line,"duplicate lambda parameter '%s'",e->eparams[i]);
        Ty *t=NULL;
        if(i==f->star){ t=ty_new(TY_LIST,ty_var(),NULL); t->tup=1; } else if(i==f->dstar) t=ty_dict(TY_STR_T,ty_var());
        f->params[i]=new_local(f,e->eparams[i],t); f->defaults[i]=e->edefaults?e->edefaults[i]:NULL; }
    f->ret=ty_var(); f->returns_value=1;
    add_func(c->p,f);
    return f;
}
/* the function's type as a value */
/* Callable[[regular params..., *args item, **kwargs value], ret]: a value of
   it can be called with positionals (extras into *args) and, given
   **kwargs, keywords. Keyword-only parameters cannot be expressed. */
static const char *fn_type_problem(AFunc *f){
    if(f->is_async) return "a coroutine";
    int nreg=f->nparams-(f->star>=0)-(f->dstar>=0);
    if(f->star>=0 && f->star!=nreg) return "keyword-only parameters";
    if(f->dstar>=0 && f->dstar!=f->nparams-1) return "keyword-only parameters";
    return NULL;
}
static Ty *fn_type(Ck *c, AFunc *f, int line){
    const char *why=fn_type_problem(f);
    if(why && !(c->ep_value && f->is_async && !f->outer && !f->cls && !strcmp(why,"a coroutine"))) err(c,line,"%s() cannot be used as a value in compiled code (%s)",f->name,why);
    if(!f->fty){ Ty *ps[16]; int n=0;
        for(int i=0;i<f->nparams;i++) ps[n++]=i==f->star||i==f->dstar ? ty_find(f->params[i]->ty)->elem : f->params[i]->ty;
        f->fty=ty_func(ps,n,f->ret); f->fty->tup=(f->star>=0?1:0)|(f->dstar>=0?2:0);
        int nreg= f->star>=0 ? f->star : f->dstar>=0 ? f->dstar : f->nparams, nd=0;   /* (keyword-only ones are parameters of the value too) */
        while(nd<nreg && f->defaults[nreg-1-nd]) nd++;
        f->fty->ndef=nd;
        if(f->lam){ f->fty->names=MPY_NEW_ARR(char*,n+1); for(int i=0;i<n && i<f->lam->neparam;i++) f->fty->names[i]=f->lam->eparams[i]; }
        if(f->def && !f->lam){ f->fty->names=MPY_NEW_ARR(char*,n+1); for(int i=0;i<n && i<f->def->param_count;i++) f->fty->names[i]=f->def->params[i];
            if(f->star<0 && f->kwonly<nreg) f->fty->kwo=nreg-f->kwonly; } }
    return f->fty;
}

static AField *add_field(AClass *cls, const char *name, Ty *ty, Expr *init){
    AField *fd=MPY_NEW0(AField); fd->name=xstrdup2(name); fd->ty=ty?ty:ty_var(); fd->init=init; fd->mod=cls->mod;
    if(cls->nfields==cls->fcap){ cls->fcap=cls->fcap?cls->fcap*2:8; cls->fields=(AField**)xrealloc(cls->fields,sizeof(AField*)*(size_t)cls->fcap); }
    cls->fields[cls->nfields++]=fd;
    return fd;
}
static AClass *new_class(Ck *c, AModule *m, Stmt *def){
    AProg *p=c->p; AClass *cls=MPY_NEW0(AClass);
    cls->name=xstrdup2(def->name); cls->mod=m; cls->def=def; cls->id=p->nclasses;
    cls->qualname=cls->name; cls->symname=cls->name; def->aux=cls;
    if(p->nclasses==p->ccap){ p->ccap=p->ccap?p->ccap*2:16; p->classes=(AClass**)xrealloc(p->classes,sizeof(AClass*)*(size_t)p->ccap); }
    p->classes[p->nclasses++]=cls;
    return cls;
}
/* a function's qualified name: C.m, f.<locals>.g */
static void fn_qual(AFunc *f, char *out, size_t n){
    if(f->outer){ char o[400]; fn_qual(f->outer,o,sizeof o); snprintf(out,n,"%s.<locals>.%s",o,f->name); }
    else if(f->cls) snprintf(out,n,"%s.%s",f->cls->qualname,f->name);
    else snprintf(out,n,"%s",f->name);
}
/* A class defined in a class body (Outer.Inner) or in a function: made like the module's
   classes, under its qualified name; the class statement evaluates its attributes. */
static AClass *new_inner_class(Ck *c, AModule *m, Stmt *s, AClass *outer, AFunc *encl){
    char q[600];
    if(outer) snprintf(q,sizeof q,"%s.%s",outer->qualname,s->name);
    else { char fq[500]; fn_qual(encl,fq,sizeof fq); snprintf(q,sizeof q,"%s.<locals>.%s",fq,s->name); }
    AClass *cls=new_class(c,m,s);
    cls->qualname=xstrdup2(q); cls->outer=outer; cls->encl=outer?outer->encl:encl;
    char h[640]; snprintf(h,sizeof h,"%s",q);
    for(int k=2;module_sym(m,h);k++) snprintf(h,sizeof h,"%s#%d",q,k);
    symtab_add(&m->syms,h,AS_CLASS,cls); cls->symname=symtab_find(&m->syms,h)->name;
    for(int i=0;i<s->body_count;i++) if(s->body[i]->kind==STMT_CLASS_DEF) new_inner_class(c,m,s->body[i],cls,NULL);
    return cls;
}
AClass *aot_nested_class(AProg *p, AClass *cls, const char *name){     /* Outer.Inner (inherited too) */
    for(;cls;cls=cls->base) for(int i=0;i<p->nclasses;i++){ AClass *k=p->classes[i]; if(k->outer==cls && !strcmp(k->name,name)) return k; }
    return NULL;
}
/* The scopes a function sees classes of by name: its own, the enclosing functions', and for a
   method of a class defined in a function, that function's. */
static AFunc *next_scope(AFunc *e){ return e->outer ? e->outer : e->cls ? e->cls->encl : NULL; }
static ASym *local_class_in(AFunc *e, const char *name){
    for(;e;e=next_scope(e)){ if(!e->def) return NULL; ASym *s=symtab_find(&e->locals,name); if(s && s->kind==AS_CLASS) return s; }
    return NULL;
}
static ASym *local_class(Ck *c, const char *name){
    ASym *s=NULL;
    if(c->sig_fn) s=local_class_in(c->sig_fn,name);
    if(!s && c->sig_cls && c->sig_cls->encl) s=local_class_in(c->sig_cls->encl,name);
    if(!s && c->fn) s=local_class_in(c->fn,name);
    return s;
}
/* ---------------------------------------------------------------- @dataclass */
/* A dataclass's fields (its bases' first) and options. Its methods are written as Python
   source and made like the class's own: __init__ (assigning the fields), __repr__, __eq__,
   the orderings, __hash__; frozen ones raise FrozenInstanceError when a field is assigned. */
typedef struct DcField { const char *name; Expr *ann; const char *defsym; Expr *factory;
    int has_default, init, repr, compare, kw_only, hash, initvar; } DcField;
struct DcInfo { DcField *f; int n, cap; int init, repr, eq, order, frozen, unsafe_hash; Stmt *init_def; };
typedef struct { char *s; size_t n, cap; } Txt;
static void tx(Txt *t, const char *fmt, ...){
    va_list ap; va_start(ap,fmt); char tmp[2048]; int k=vsnprintf(tmp,sizeof tmp,fmt,ap); va_end(ap);
    if(k<0) return; if((size_t)k>=sizeof tmp) k=(int)sizeof tmp-1;
    if(t->n+(size_t)k+1>t->cap){ t->cap=(t->n+(size_t)k+1)*2; t->s=(char*)xrealloc(t->s,t->cap); }
    memcpy(t->s+t->n,tmp,(size_t)k); t->n+=(size_t)k; t->s[t->n]=0;
}
/* x names dataclasses.<what> (`what` imported from dataclasses, or dataclasses.what) */
static int dc_is(AClass *cls, Expr *x, const char *what){
    if(!x) return 0;
    if(x->kind==EXPR_INDEX) x=x->a;
    if(x->kind==EXPR_NAME){ ASym *s=global_sym(cls->mod,x->name); char full[64]; snprintf(full,sizeof full,"dataclasses.%s",what);
        return s && s->kind==AS_SYS && !strcmp((const char*)s->p,full); }
    if(x->kind==EXPR_ATTRIBUTE && x->a->kind==EXPR_NAME && !strcmp(x->name,what)){ ASym *s=global_sym(cls->mod,x->a->name);
        return s && s->kind==AS_SYS && !strcmp((const char*)s->p,"dataclasses"); }
    return 0;
}
static int is_classvar(Expr *a){
    if(a->kind==EXPR_LITERAL && a->tok->kind==T_STRING) return !strncmp(a->tok->text,"ClassVar",8) || !strncmp(a->tok->text,"typing.ClassVar",15);
    if(a->kind==EXPR_INDEX) a=a->a;
    return (a->kind==EXPR_NAME && !strcmp(a->name,"ClassVar")) || (a->kind==EXPR_ATTRIBUTE && !strcmp(a->name,"ClassVar"));
}
static int dc_flag(Ck *c, Expr *v, int line, const char *what){
    if(v->kind==EXPR_TRUE) return 1;
    if(v->kind==EXPR_FALSE) return 0;
    err(c,line,"%s= must be True or False in compiled code",what);
}
static void dc_add(struct DcInfo *dc, DcField *f){
    for(int i=0;i<dc->n;i++) if(!strcmp(dc->f[i].name,f->name)){ dc->f[i]=*f; return; }
    if(dc->n==dc->cap){ dc->cap=dc->cap?dc->cap*2:8; dc->f=(DcField*)xrealloc(dc->f,sizeof(DcField)*(size_t)dc->cap); }
    dc->f[dc->n++]=*f;
}
static AProg *g_dc_prog;
static void dc_conv(AClass *cls, Expr *a, const char *x, int asdict, int depth, Txt *t);
static Stmt *body_def(Stmt *def, const char *name){
    for(int i=0;i<def->body_count;i++) if(def->body[i]->kind==STMT_FUNCTION_DEF && !strcmp(def->body[i]->name,name)) return def->body[i];
    return NULL;
}
/* a method written as source: parsed at the class's line, added to its body */
static Stmt *dc_method(Ck *c, AClass *cls, Txt *t){
    Stmt *def=cls->def; Txt src={0};
    for(int i=1;i<def->line;i++) tx(&src,"\n");
    tx(&src,"%s",t->s);
    Stmt *blk=py_front_stmts(cls->mod->unit->path,src.s,def->line);
    if(!blk || blk->body_count!=1) err(c,def->line,"internal error: a method of dataclass %s",cls->name);
    free(src.s); free(t->s); memset(t,0,sizeof *t);
    stmt_add_body(def,blk->body[0]);
    return blk->body[0];
}
static void dc_transform(Ck *c, AClass *cls, Expr *deco){
    Stmt *def=cls->def; AotUnit *u=cls->mod->unit; int line=def->line; g_dc_prog=c->p;
    struct DcInfo *dc=MPY_NEW0(struct DcInfo); dc->init=dc->repr=dc->eq=1; int kw_only=0;
    if(deco->kind==EXPR_CALL) for(int i=0;i<deco->count;i++){ Expr *a=deco->items[i];
        if(a->akind!=3) err(c,line,"dataclass() takes keyword arguments only");
        if(!strcmp(a->kw,"init")) dc->init=dc_flag(c,a,line,a->kw);
        else if(!strcmp(a->kw,"repr")) dc->repr=dc_flag(c,a,line,a->kw);
        else if(!strcmp(a->kw,"eq")) dc->eq=dc_flag(c,a,line,a->kw);
        else if(!strcmp(a->kw,"order")) dc->order=dc_flag(c,a,line,a->kw);
        else if(!strcmp(a->kw,"frozen")) dc->frozen=dc_flag(c,a,line,a->kw);
        else if(!strcmp(a->kw,"unsafe_hash")) dc->unsafe_hash=dc_flag(c,a,line,a->kw);
        else if(!strcmp(a->kw,"kw_only")) kw_only=dc_flag(c,a,line,a->kw);
        else if(!strcmp(a->kw,"match_args")||!strcmp(a->kw,"slots")||!strcmp(a->kw,"weakref_slot")) dc_flag(c,a,line,a->kw);
        else err(c,line,"dataclass() got an unexpected keyword argument '%s'",a->kw); }
    if(dc->order && !dc->eq) err(c,line,"eq must be true if order is true");
    AClass *bdc=NULL; for(AClass *b=cls->base;b;b=b->base) if(b->dc){ bdc=b; break; }
    if(bdc){ for(int i=0;i<bdc->dc->n;i++) dc_add(dc,&bdc->dc->f[i]);
        if(bdc->dc->frozen && !dc->frozen) err(c,line,"cannot inherit non-frozen dataclass from a frozen one");
        if(!bdc->dc->frozen && dc->frozen) err(c,line,"cannot inherit frozen dataclass from a non-frozen one"); }
    Stmt **keep=MPY_NEW_ARR(Stmt*,def->body_count+1); int nk=0;
    for(int i=0;i<def->body_count;i++){ Stmt *s=def->body[i];
        int annonly=aot_is_annotation_only(u,s);
        AAssign *a= annonly || s->kind==STMT_ASSIGN ? aot_assign(u,s) : NULL;
        if(!a || !a->ann || a->ntarget!=1 || a->target[0]->kind!=EXPR_NAME){ keep[nk++]=s; continue; }
        const char *name=a->target[0]->name; Expr *ann=a->ann;
        if(dc_is(cls,ann,"KW_ONLY")){ kw_only=1; continue; }            /* _: KW_ONLY: the fields after it are keyword-only */
        if(is_classvar(ann)){ keep[nk++]=s; continue; }                  /* a class attribute, not a field */
        DcField f; memset(&f,0,sizeof f); f.name=name; f.ann=ann; f.init=f.repr=f.compare=1; f.hash=-1; f.kw_only=kw_only;
        if(dc_is(cls,ann,"InitVar")){ f.initvar=1; f.repr=f.compare=0; f.ann= ann->kind==EXPR_INDEX ? ann->b : ann; }
        Expr *v=a->value;
        if(v && v->kind==EXPR_CALL && dc_is(cls,v->a,"field")){        /* name: T = field(...) */
            Expr *dflt=NULL;
            for(int k=0;k<v->count;k++){ Expr *x=v->items[k];
                if(x->akind!=3) err(c,s->line,"field() takes keyword arguments only");
                if(!strcmp(x->kw,"default")) dflt=x;
                else if(!strcmp(x->kw,"default_factory")) f.factory=x;
                else if(!strcmp(x->kw,"init")) f.init=dc_flag(c,x,s->line,x->kw);
                else if(!strcmp(x->kw,"repr")) f.repr=dc_flag(c,x,s->line,x->kw);
                else if(!strcmp(x->kw,"compare")) f.compare=dc_flag(c,x,s->line,x->kw);
                else if(!strcmp(x->kw,"kw_only")) f.kw_only=dc_flag(c,x,s->line,x->kw);
                else if(!strcmp(x->kw,"hash")) f.hash= x->kind==EXPR_NONE ? -1 : dc_flag(c,x,s->line,x->kw);
                else if(!strcmp(x->kw,"metadata")) {}
                else err(c,s->line,"field() got an unexpected keyword argument '%s'",x->kw); }
            if(dflt && f.factory) err(c,s->line,"cannot specify both default and default_factory");
            if(f.factory){ Expr *fe=MPY_NEW0(Expr); *fe=*f.factory; fe->kw=NULL; fe->akind=0; f.factory=fe; }
            if(dflt){ Expr *de=MPY_NEW0(Expr); *de=*dflt; de->kw=NULL; de->akind=0; a->value=s->value=de; v=de; }
            else { a->value=s->value=NULL; v=NULL; s->kind=STMT_EXPR; s->ann_only=1; }   /* no class attribute */
        }
        if(v){ f.has_default=1; f.defsym=cls->symname;
            const char *mk= v->kind==EXPR_LIST||(v->kind==EXPR_COMPREHENSION&&v->comp_kind=='L') ? "list" : v->kind==EXPR_DICT||(v->kind==EXPR_COMPREHENSION&&v->comp_kind=='D') ? "dict"
                          : v->kind==EXPR_SET||(v->kind==EXPR_COMPREHENSION&&v->comp_kind=='S') ? "set" : NULL;
            if(mk) err(c,s->line,"mutable default <class '%s'> for field %s is not allowed: use default_factory",mk,name); }
        dc_add(dc,&f);
        if(f.initvar && !f.has_default) continue;                         /* (not an attribute) */
        keep[nk++]=s;
    }
    def->body=keep; def->body_count=nk; def->body_cap=def->body_count+1;
    cls->dc=dc; cls->dc_frozen=dc->frozen;
    { const char *seen=NULL;                                            /* defaults last (keyword-only ones aside) */
      for(int i=0;i<dc->n;i++){ DcField *f=&dc->f[i]; if(!f->init || f->kw_only) continue;
          if(f->has_default || f->factory) seen=f->name;
          else if(seen) err(c,line,"non-default argument '%s' follows default argument '%s'",f->name,seen); } }
    char ref[600]; snprintf(ref,sizeof ref,"%s",cls->outer?cls->qualname:cls->name);   /* the class, as an annotation */
    Txt t={0};
    if(dc->init && !body_def(def,"__init__")){                          /* def __init__(self, x, y=..., *, z=...) */
        int post=0; for(AClass *k=cls;k;k=k->base) if(k->def && body_def(k->def,"__post_init__")) post=1;
        tx(&t,"def __init__(self");
        int star=0;
        for(int pass=0;pass<2;pass++) for(int i=0;i<dc->n;i++){ DcField *f=&dc->f[i];
            if(!f->init || f->kw_only!=pass) continue;
            if(pass && !star){ tx(&t,", *"); star=1; }
            tx(&t,", %s%s",f->name,f->has_default||f->factory?"=None":""); }
        tx(&t,"):\n");
        int body=0;
        for(int i=0;i<dc->n;i++){ DcField *f=&dc->f[i]; if(f->initvar) continue;
            if(f->init) tx(&t,f->factory?"    self.%s = __factory__() if %s is None else %s\n":"    self.%s = %s\n",f->name,f->name,f->name);
            else if(f->factory) tx(&t,"    self.%s = __factory__()\n",f->name);
            else continue;
            body++; }
        if(post){ tx(&t,"    self.__post_init__("); int n=0; for(int i=0;i<dc->n;i++) if(dc->f[i].initvar) tx(&t,"%s%s",n++?", ":"",dc->f[i].name); tx(&t,")\n"); body++; }
        if(!body) tx(&t,"    pass\n");
        Stmt *d=dc_method(c,cls,&t); dc->init_def=d;
        int p=1;                                                         /* the annotations and defaults: the fields' */
        if(!d->pdefaults){ d->pdefaults=MPY_NEW_ARR(Expr*,d->param_count+1); }
        for(int pass=0;pass<2;pass++) for(int i=0;i<dc->n;i++){ DcField *f=&dc->f[i];
            if(!f->init || f->kw_only!=pass) continue;
            Expr *an=f->ann;
            if(f->factory){ Expr *o=xnew(EXPR_BINARY,line); o->op=T_PIPE; o->a=f->ann; o->b=xnew(EXPR_NONE,line); an=o; }
            stmt_set_annotation(d,p,an);
            if(f->has_default){ char h[640]; snprintf(h,sizeof h," %s.%s",f->defsym,f->name); d->pdefaults[p]=name_expr(xstrdup2(h),line); }
            p++; }
        int k=0;                                                         /* __factory__: the field's default_factory */
        for(int i=0;i<dc->n;i++){ DcField *f=&dc->f[i]; if(f->initvar) continue;
            if(!f->init && !f->factory) continue;
            Stmt *st=d->body[k++];
            if(f->factory){ Expr *val=st->value; Expr *call= val->kind==EXPR_TERNARY ? val->b : val; call->a=f->factory; } }
    }
    if(dc->repr && !body_def(def,"__repr__")){                         /* Name(x=1, y='a') */
        tx(&t,"def __repr__(self) -> str:\n    return type(self).__qualname__ + \"(");
        int n=0; for(int i=0;i<dc->n;i++){ DcField *f=&dc->f[i]; if(!f->repr || f->initvar) continue;
            tx(&t,"%s%s=\" + repr(self.%s) + \"",n++?", ":"",f->name,f->name); }
        tx(&t,")\"\n");
        dc_method(c,cls,&t);
    }
    const char *ops[4][2]={{"__lt__","<"},{"__le__","<="},{"__gt__",">"},{"__ge__",">="}};
    #define DC_TUPLE(who,sel) do{ tx(&t,"("); for(int i=0;i<dc->n;i++){ DcField *f=&dc->f[i]; if(!(sel) || f->initvar) continue; tx(&t,"%s.%s, ",who,f->name); } tx(&t,")"); }while(0)
    if(dc->eq && !body_def(def,"__eq__")){                             /* the fields compared as tuples */
        tx(&t,"def __eq__(self, other: \"%s\") -> bool:\n    if type(other) is not type(self):\n        return False\n    return ",ref);
        DC_TUPLE("self",f->compare); tx(&t," == "); DC_TUPLE("other",f->compare); tx(&t,"\n");
        dc_method(c,cls,&t);
    }
    if(dc->order) for(int o=0;o<4;o++){
        if(body_def(def,ops[o][0])) err(c,line,"Cannot overwrite attribute %s in class %s. Consider using functools.total_ordering",ops[o][0],cls->name);
        tx(&t,"def %s(self, other: \"%s\") -> bool:\n    return ",ops[o][0],ref);
        DC_TUPLE("self",f->compare); tx(&t," %s ",ops[o][1]); DC_TUPLE("other",f->compare); tx(&t,"\n");
        dc_method(c,cls,&t);
    }
    if(c->dc_conv){                                                       /* asdict(obj), astuple(obj) */
        tx(&t,"def __mp_asdict__(self):\n    return {");
        for(int i=0;i<dc->n;i++){ DcField *f=&dc->f[i]; if(f->initvar) continue; char x[300]; snprintf(x,sizeof x,"self.%s",f->name);
            tx(&t,"\"%s\": ",f->name); dc_conv(cls,f->ann,x,1,0,&t); tx(&t,", "); }
        tx(&t,"}\n"); dc_method(c,cls,&t);
        tx(&t,"def __mp_astuple__(self):\n    return (");
        for(int i=0;i<dc->n;i++){ DcField *f=&dc->f[i]; if(f->initvar) continue; char x[300]; snprintf(x,sizeof x,"self.%s",f->name);
            dc_conv(cls,f->ann,x,0,0,&t); tx(&t,", "); }
        tx(&t,")\n"); dc_method(c,cls,&t);
    }
    if((dc->unsafe_hash || (dc->eq && dc->frozen)) && !body_def(def,"__hash__")){
        tx(&t,"def __hash__(self) -> int:\n    return hash(");
        DC_TUPLE("self",f->hash<0?f->compare:f->hash); tx(&t,")\n");
        dc_method(c,cls,&t);
    }
    #undef DC_TUPLE
}
/* asdict / astuple of a value of annotation `ann` (source text `x`): dataclasses (and those in
   lists, dicts, tuples, optional values) converted, containers copied */
static int dc_decorated(AClass *k){
    for(;k;k=k->base){ if(k->dc) return 1;
        if(k->def) for(int i=0;i<k->def->decorator_count;i++){ Expr *d=k->def->decorator_exprs[i]; if(dc_is(k,d->kind==EXPR_CALL?d->a:d,"dataclass")) return 1; } }
    return 0;
}
static AClass *ann_class(AClass *cls, Expr *a){
    ASym *s=NULL;
    if(a->kind==EXPR_NAME){ if(cls->encl) s=local_class_in(cls->encl,a->name); if(!s) s=global_sym(cls->mod,a->name); }
    else if(a->kind==EXPR_ATTRIBUTE && a->a->kind==EXPR_NAME){ ASym *o=global_sym(cls->mod,a->a->name);
        if(o && o->kind==AS_MODULE) s=module_sym((AModule*)o->p,a->name);
        else if(o && o->kind==AS_CLASS) return aot_nested_class(g_dc_prog,(AClass*)o->p,a->name); }
    return s && s->kind==AS_CLASS ? (AClass*)s->p : NULL;
}
static void dc_conv(AClass *cls, Expr *a, const char *x, int asdict, int depth, Txt *t){
    if(a->kind==EXPR_LITERAL && a->tok->kind==T_STRING){ Expr *p=py_front_expr(cls->mod->unit->path,a->tok->text,a->line); if(p) a=p; }
    const char *bn= a->kind==EXPR_INDEX&&a->a->kind==EXPR_NAME ? a->a->name : a->kind==EXPR_INDEX&&a->a->kind==EXPR_ATTRIBUTE ? a->a->name : NULL;
    Expr *opt=NULL;
    if(a->kind==EXPR_BINARY && a->op==T_PIPE){ if(a->b->kind==EXPR_NONE) opt=a->a; else if(a->a->kind==EXPR_NONE) opt=a->b; }
    if(bn && !strcmp(bn,"Optional")) opt=a->b;
    if(opt){ tx(t,"(None if %s is None else ",x); dc_conv(cls,opt,x,asdict,depth,t); tx(t,")"); return; }
    if(bn && (!strcmp(bn,"list")||!strcmp(bn,"List")||!strcmp(bn,"Sequence")||!strcmp(bn,"MutableSequence"))){
        char v[32]; snprintf(v,sizeof v,"_e%d",depth); tx(t,"["); dc_conv(cls,a->b,v,asdict,depth+1,t); tx(t," for %s in %s]",v,x); return; }
    if(bn && (!strcmp(bn,"dict")||!strcmp(bn,"Dict")||!strcmp(bn,"Mapping")||!strcmp(bn,"MutableMapping")) && a->b->kind==EXPR_TUPLE && a->b->count==2){
        char k[32], v[32]; snprintf(k,sizeof k,"_k%d",depth); snprintf(v,sizeof v,"_v%d",depth);
        tx(t,"{"); dc_conv(cls,a->b->items[0],k,asdict,depth+1,t); tx(t,": "); dc_conv(cls,a->b->items[1],v,asdict,depth+1,t); tx(t," for %s, %s in %s.items()}",k,v,x); return; }
    if(bn && (!strcmp(bn,"tuple")||!strcmp(bn,"Tuple"))){
        Expr *tp=a->b; int n=tp->kind==EXPR_TUPLE?tp->count:1; Expr **it=tp->kind==EXPR_TUPLE?tp->items:&a->b;
        if(n==2 && it[1]->kind==EXPR_NAME && !strcmp(it[1]->name,"...")){ char v[32]; snprintf(v,sizeof v,"_e%d",depth);
            tx(t,"tuple(["); dc_conv(cls,it[0],v,asdict,depth+1,t); tx(t," for %s in %s])",v,x); return; }
        tx(t,"("); for(int i=0;i<n;i++){ char v[300]; snprintf(v,sizeof v,"%s[%d]",x,i); dc_conv(cls,it[i],v,asdict,depth+1,t); tx(t,", "); } tx(t,")"); return; }
    AClass *k=ann_class(cls,a);
    if(k && dc_decorated(k)){ tx(t,"%s.%s()",x,asdict?"__mp_asdict__":"__mp_astuple__"); return; }
    tx(t,"%s",x);
}
/* @total_ordering: the orderings the class leaves out, from the one it has (functools' rules) */
static void total_ordering(Ck *c, AClass *cls){
    static const char *const ops[4]={"__lt__","__le__","__gt__","__ge__"};
    /* per root: the derived op and its body (r: the root's result) */
    static const char *const conv[4][3][2]={
        {{"__gt__","not r and self != other"},{"__le__","r or self == other"},{"__ge__","not r"}},
        {{"__ge__","not r or self == other"},{"__lt__","r and self != other"},{"__gt__","not r"}},
        {{"__lt__","not r and self != other"},{"__ge__","r or self == other"},{"__le__","not r"}},
        {{"__le__","not r or self == other"},{"__gt__","r and self != other"},{"__lt__","not r"}}};
    int root=-1; Stmt *rd=NULL;
    for(int i=0;i<4 && root<0;i++){ Stmt *d=body_def(cls->def,ops[i]); if(d){ root=i; rd=d; } }
    if(root<0) err(c,cls->def->line,"must define at least one ordering operation: < > <= >=");
    char ref[600]; snprintf(ref,sizeof ref,"%s",cls->outer?cls->qualname:cls->name);
    for(int k=0;k<3;k++){ const char *op=conv[root][k][0]; if(body_def(cls->def,op)) continue;
        Txt t={0}; tx(&t,"def %s(self, other: \"%s\") -> bool:\n    r = self.%s(other)\n    return %s\n",op,ref,ops[root],conv[root][k][1]);
        Stmt *m=dc_method(c,cls,&t);
        if(rd->param_count>1 && rd->annotations && rd->annotation_cap>1 && rd->annotations[1]) stmt_set_annotation(m,1,rd->annotations[1]); }
}
/* the class's decorators: @dataclass (with its options) or none */
static void class_decorators(Ck *c, AClass *cls){
    Stmt *def=cls->def;
    for(int i=0;i<def->decorator_count;i++){ Expr *d=def->decorator_exprs[i];
        if(dc_is(cls,d->kind==EXPR_CALL?d->a:d,"dataclass")){ if(cls->dc) err(c,def->line,"@dataclass given twice"); dc_transform(c,cls,d); continue; }
        if((d->kind==EXPR_NAME && !strcmp(d->name,"total_ordering")) || (d->kind==EXPR_ATTRIBUTE && !strcmp(d->name,"total_ordering") && d->a->kind==EXPR_NAME && !strcmp(d->a->name,"functools"))){
            total_ordering(c,cls); continue; }
        err(c,def->line,"class decorators other than @dataclass and @total_ordering are not supported in compiled code"); }
    if(!cls->dc) for(AClass *b=cls->base;b;b=b->base) if(b->dc_frozen) cls->dc_frozen=1;   /* (a frozen dataclass's __setattr__) */
}
/* ---------------------------------------------------------------- several base classes */
/* A class with several bases keeps the first as its layout parent; the members of the other
   classes of its C3 MRO are copied into it (their methods made again from the source, so that
   self is this class), and super() goes to the next class of the MRO that has the method:
   a copy named __mro_<class>_<method>. */
static AClass *resolve_base(Ck *c, AClass *cls, const char *name, int line){
    ASym *s=cls->encl ? local_class_in(cls->encl,name) : NULL;
    if(!s) s=global_sym(cls->mod,name);
    const char *dot=strchr(name,'.');
    if(dot){ char mn[256]; snprintf(mn,sizeof mn,"%.*s",(int)(dot-name),name);
        ASym *ms=cls->encl ? local_class_in(cls->encl,mn) : NULL; if(!ms) ms=global_sym(cls->mod,mn);
        if(ms && ms->kind==AS_SYS && !strcmp((const char*)ms->p,"abc") && !strcmp(dot+1,"ABC")) return abc_class(c);
        if(ms && ms->kind==AS_CLASS && !strchr(dot+1,'.')){ AClass *k=aot_nested_class(c->p,(AClass*)ms->p,dot+1); if(!k) err(c,line,"base class '%s' is not a class",name); return k; }
        s= ms && ms->kind==AS_MODULE ? module_sym((AModule*)ms->p,dot+1) : NULL; }
    if(!s || s->kind!=AS_CLASS) err(c,line,"base class '%s' is not a class",name);
    return (AClass*)s->p;
}
static void mro_of(AClass *k, AClass ***out, int *n){
    if(k->mro){ *out=k->mro; *n=k->nmro; return; }
    static AClass *buf[64]; int m=0; for(AClass *x=k;x && m<64;x=x->base) buf[m++]=x;
    *out=MPY_NEW_ARR(AClass*,m); memcpy(*out,buf,sizeof(AClass*)*(size_t)m); *n=m;
}
static int c3(Ck *c, AClass *cls, AClass **bases, int nb, int line){
    AClass **seq[17]; int len[17], pos[17], ns=0;
    for(int i=0;i<nb;i++){ mro_of(bases[i],&seq[ns],&len[ns]); pos[ns++]=0; }
    seq[ns]=bases; len[ns]=nb; pos[ns++]=0;
    AClass *out[64]; int n=0; out[n++]=cls;
    for(;;){
        int left=0; for(int i=0;i<ns;i++) if(pos[i]<len[i]) left=1;
        if(!left) break;
        AClass *pick=NULL;
        for(int i=0;i<ns && !pick;i++){ if(pos[i]>=len[i]) continue; AClass *h=seq[i][pos[i]]; int bad=0;
            for(int j=0;j<ns && !bad;j++) for(int q=pos[j]+1;q<len[j];q++) if(seq[j][q]==h){ bad=1; break; }
            if(!bad) pick=h; }
        if(!pick) err(c,line,"Cannot create a consistent method resolution order (MRO) for bases");
        if(n<64) out[n++]=pick;
        for(int i=0;i<ns;i++) if(pos[i]<len[i] && seq[i][pos[i]]==pick) pos[i]++; }
    cls->mro=MPY_NEW_ARR(AClass*,n); memcpy(cls->mro,out,sizeof(AClass*)*(size_t)n); cls->nmro=n;
    return n;
}
typedef struct { AClass *cls; AClass *from; const char *self; Ck *c; int line; } SuperCtx;
static Stmt *body_def(Stmt *def, const char *name);
/* the class after `from` in cls's MRO with method n of its own */
static AClass *mro_next(AClass *cls, AClass *from, const char *n){
    int i=0; while(i<cls->nmro && cls->mro[i]!=from) i++;
    for(i++;i<cls->nmro;i++){ AClass *k=cls->mro[i]; if(k->def && body_def(k->def,n)) return k;
        if(!k->def){ for(int j=0;j<k->nmethods;j++) if(!strcmp(k->methods[j]->name,n)) return k; } }
    return NULL;
}
static void mro_name(char *out, size_t sz, AClass *k, const char *n){ snprintf(out,sz,"__mro_%s_%s",k->symname,n); for(char *p=out;*p;p++) if(*p=='.'||*p=='#'||*p=='<'||*p=='>') *p='_'; }
static void super_rewrite_expr(Expr *e, SuperCtx *sc);
static void super_rewrite_stmts(Stmt **b, int n, SuperCtx *sc);
static void super_rewrite_expr(Expr *e, SuperCtx *sc){
    if(!e) return;
    if(e->kind==EXPR_CALL && e->a && e->a->kind==EXPR_ATTRIBUTE && e->a->a->kind==EXPR_CALL && e->a->a->a->kind==EXPR_NAME && !strcmp(e->a->a->a->name,"super") && !e->a->a->count){
        const char *n=e->a->name; AClass *y=mro_next(sc->cls,sc->from,n);
        if(!y){ if(!strcmp(n,"__init__")){ for(int i=0;i<e->count;i++) super_rewrite_expr(e->items[i],sc); Expr *z=xnew(EXPR_NONE,e->line); *e=*z; return; }
            err(sc->c,e->line,"super(): no class after %s in %s's MRO has '%s'",sc->from->name,sc->cls->name,n); }
        char nm[400]; mro_name(nm,sizeof nm,y,n);
        Expr *at=xnew(EXPR_ATTRIBUTE,e->line); at->a=name_expr(sc->self,e->line); at->name=xstrdup2(nm); e->a=at; }
    else if(e->kind==EXPR_CALL && e->a && e->a->kind==EXPR_ATTRIBUTE && e->a->a->kind==EXPR_NAME && e->count && e->items[0]->kind==EXPR_NAME && !e->items[0]->akind
            && !strcmp(e->items[0]->name,sc->self)){                      /* Mixin.m(self, ...): the copy of Mixin's m */
        ASym *s=global_sym(sc->cls->mod,e->a->a->name); AClass *x= s && s->kind==AS_CLASS ? (AClass*)s->p : NULL;
        int vb=0; for(int i=0;x && i<sc->cls->nvbases;i++) if(sc->cls->vbases[i]==x) vb=1;
        if(vb && x->def && body_def(x->def,e->a->name)){ char nm[400]; mro_name(nm,sizeof nm,x,e->a->name);
            Expr *at=xnew(EXPR_ATTRIBUTE,e->line); at->a=name_expr(sc->self,e->line); at->name=xstrdup2(nm); e->a=at;
            for(int i=1;i<e->count;i++) e->items[i-1]=e->items[i];
            e->count--; } }
    super_rewrite_expr(e->a,sc); super_rewrite_expr(e->b,sc); super_rewrite_expr(e->c,sc); super_rewrite_expr(e->d,sc);
    for(int i=0;i<e->count;i++) super_rewrite_expr(e->items[i],sc);
    for(int i=0;i<e->vcount;i++) super_rewrite_expr(e->vals[i],sc);
    for(int i=0;i<e->nclause;i++){ CompClause *cl=&e->clauses[i]; super_rewrite_expr(cl->iter,sc); for(int k=0;k<cl->ncond;k++) super_rewrite_expr(cl->conds[k],sc); }
    if(e->edefaults) for(int i=0;i<e->neparam;i++) super_rewrite_expr(e->edefaults[i],sc);
}
static void super_rewrite_stmts(Stmt **b, int n, SuperCtx *sc){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_CLASS_DEF) continue;
        super_rewrite_expr(s->expr,sc); super_rewrite_expr(s->expr2,sc); super_rewrite_expr(s->value,sc);
        for(int k=0;k<s->ntargets;k++) super_rewrite_expr(s->targets[k],sc);
        if(s->kind==STMT_FUNCTION_DEF){ AClass *sv=sc->from; super_rewrite_stmts(s->body,s->body_count,sc); sc->from=sv; continue; }
        super_rewrite_stmts(s->body,s->body_count,sc); super_rewrite_stmts(s->orelse,s->orelse_count,sc); }
}
/* class D(A, B, ...): MRO, the members of the classes not on D's layout chain, the MRO copies of methods */
static void several_bases(Ck *c, AClass *cls){
    Stmt *def=cls->def; int line=def->line;
    AClass *bases[16]; int nb=0;
    for(int i=0;i<def->param_count && nb<16;i++){ AClass *b=resolve_base(c,cls,def->params[i],line);
        if(b->def){ AModule *save=c->mod; c->mod=b->mod; fill_class(c,b); c->mod=save; }
        if(aot_is_exception(b) && i) err(c,line,"an exception class with several bases is not supported in compiled code");
        bases[nb++]=b; }
    cls->base=bases[0];                                          /* the layout parent: a base with an __init__ (state), else the first */
    for(int i=0;i<nb;i++) if(aot_find_method(bases[i],"__init__") || aot_is_exception(bases[i])){ cls->base=bases[i]; break; }
    c3(c,cls,bases,nb,line);
    AClass *vb[64]; int nv=0;
    for(int i=1;i<cls->nmro;i++){ AClass *k=cls->mro[i]; if(!aot_subclass(cls->base,k) && nv<64) vb[nv++]=k; }
    cls->vbases=MPY_NEW_ARR(AClass*,nv+1); memcpy(cls->vbases,vb,sizeof(AClass*)*(size_t)nv); cls->nvbases=nv;
    Stmt **add=NULL; int nadd=0, cadd=0;
    #define ADD(st) do{ if(nadd==cadd){ cadd=cadd?cadd*2:16; add=(Stmt**)xrealloc(add,sizeof(Stmt*)*(size_t)cadd); } add[nadd++]=(st); }while(0)
    for(int i=1;i<cls->nmro;i++){ AClass *k=cls->mro[i]; if(!k->def) continue;
        Stmt *copy=py_front_copy(k->mod->unit->path,k->def);
        if(!copy) err(c,line,"cannot copy class %s",k->name);
        int vbase=0; for(int j=0;j<nv;j++) if(vb[j]==k) vbase=1;
        for(int j=0;j<copy->body_count;j++){ Stmt *m=copy->body[j];
            if(m->kind==STMT_FUNCTION_DEF){
                if(m->decorator_count) continue;                            /* (decorated ones: through the class they are in) */
                char nm[400]; mro_name(nm,sizeof nm,k,m->name);
                SuperCtx sc={cls,k,m->param_count?m->params[0]:"self",c,m->line}; super_rewrite_stmts(m->body,m->body_count,&sc);
                const char *orig=m->name; m->name=xstrdup2(nm); ADD(m);
                if(!body_def(def,orig)){                                      /* the visible one: the first in the MRO */
                    int first=1; for(int q=1;q<i;q++){ AClass *e2=cls->mro[q]; if(e2->def && body_def(e2->def,orig)) first=0; }
                    if(first){ Stmt *again=py_front_copy(k->mod->unit->path,k->def); Stmt *vm=body_def(again,orig);
                        super_rewrite_stmts(vm->body,vm->body_count,&sc); ADD(vm); } } }
            else if(vbase && (m->kind==STMT_ASSIGN || (m->kind==STMT_EXPR && m->ann_only))) ADD(m);     /* a mixin's fields / class attributes */
        } }
    { SuperCtx sc={cls,cls,"self",c,line};                                        /* the class's own methods */
      for(int j=0;j<def->body_count;j++){ Stmt *m=def->body[j]; if(m->kind!=STMT_FUNCTION_DEF) continue;
          sc.self=m->param_count?m->params[0]:"self"; super_rewrite_stmts(m->body,m->body_count,&sc); } }
    for(int j=0;j<nadd;j++) stmt_add_body(def,add[j]);
    #undef ADD
}
/* Fields and methods; done once every class of the module is known (annotations may name them). */
static void fill_class(Ck *c, AClass *cls){
    if(cls->filled) return;
    cls->filled=1;
    Stmt *def=cls->def; AotUnit *u=cls->mod->unit;
    AClass *save_cls=c->sig_cls; c->sig_cls=cls;
    if(def->param_count>1) several_bases(c,cls);
    else if(def->name2){
        ASym *s=cls->encl ? local_class_in(cls->encl,def->name2) : NULL;
        if(!s) s=global_sym(cls->mod,def->name2);
        const char *dot=strchr(def->name2,'.');
        if(dot){ char mn[256]; snprintf(mn,sizeof mn,"%.*s",(int)(dot-def->name2),def->name2);   /* class A(module.Base), class A(Outer.Inner) */
            ASym *ms=cls->encl ? local_class_in(cls->encl,mn) : NULL; if(!ms) ms=global_sym(cls->mod,mn);
            static ASym abcs; if(ms && ms->kind==AS_SYS && !strcmp((const char*)ms->p,"abc") && !strcmp(dot+1,"ABC")){ abcs.kind=AS_CLASS; abcs.p=abc_class(c); ms=NULL; s=&abcs; }   /* class C(abc.ABC) */
            else
            if(ms && ms->kind==AS_CLASS && !strchr(dot+1,'.')){ static ASym ns; AClass *k=aot_nested_class(c->p,(AClass*)ms->p,dot+1); ns.kind=AS_CLASS; ns.p=k; s=k?&ns:NULL; }
            else s= ms && ms->kind==AS_MODULE ? module_sym((AModule*)ms->p,dot+1) : NULL; }
        if(!s || s->kind!=AS_CLASS) err(c,def->line,"base class '%s' is not a class",def->name2);
        cls->base=(AClass*)s->p;
        if(aot_subclass(cls->base,cls)) err(c,def->line,"class %s inherits from itself",cls->name);
        if(cls->base->def){ AModule *save=c->mod; c->mod=cls->base->mod; fill_class(c,cls->base); c->mod=save; }   /* (its attributes may be redefined here) */
    }
    class_decorators(c,cls);
    if(def->ntparams) cls->generic=1;
    { Stmt *ini=body_def(def,"__init__"); if(ini){ for(int i=1;i<ini->param_count;i++) if(!ini->annotations || i>=ini->annotation_cap || !ini->annotations[i]) cls->generic=1; } }
    for(int i=0;i<def->body_count;i++){ Stmt *s=def->body[i];
        if(s->kind==STMT_FUNCTION_DEF){
            AFunc *f=new_func(c,NULL,cls->mod,cls,s,NULL);
            if(f->ndeco){ char *vn=hidden_name(cls->symname,s->name);
                f->decovar=new_global(c,cls->mod,vn+1);          /* Class.name after its decorators */
                symtab_add(&cls->mod->syms,vn,AS_FUNC,f); }
            if(cls->nmethods==cls->mcap){ cls->mcap=cls->mcap?cls->mcap*2:8; cls->methods=(AFunc**)xrealloc(cls->methods,sizeof(AFunc*)*(size_t)cls->mcap); }
            cls->methods[cls->nmethods++]=f;
        } else if(aot_is_annotation_only(u,s)){
            AAssign *a=aot_assign(u,s);
            Ty *t=type_of(c,a->ann,s->line);
            add_field(cls,a->target[0]->name,t,NULL);
        } else if(s->kind==STMT_ASSIGN){
            AAssign *a=aot_assign(u,s);
            if(a->ntarget!=1 || a->target[0]->kind!=EXPR_NAME || a->aug) err(c,s->line,"a class body may only declare fields (name: type [= value]) and methods");
            Ty *t=a->ann ? type_of(c,a->ann,s->line) : NULL;
            AField *bf=cls->base ? aot_find_field(cls->base,a->target[0]->name) : NULL, *fd;
            if(bf){ bf=aot_field_root(bf); bf->overridden=1;                  /* redefined in a subclass: the same slot, a value of its own */
                fd=add_field(cls,bf->name,bf->ty,a->value); fd->over=bf;
                if(t) expect(c,bf->ty,t,s->line,"the redefined class attribute"); }
            else fd=add_field(cls,a->target[0]->name,t,a->value);
            char h[600]; snprintf(h,sizeof h," %s.%s",cls->symname,fd->name);     /* its class-level value: evaluated when the class statement runs */
            fd->cvar=new_global(c,cls->mod,h); fd->cvar->ty=fd->ty; symtab_add(&cls->mod->syms,h,AS_VAR,fd->cvar);
        } else if(s->kind==STMT_CLASS_DEF){                    /* a nested class: made with this one */
        } else if(s->kind==STMT_PASS || (s->kind==STMT_EXPR && aot_expr(u,&s->expr)->kind==EXPR_LITERAL)){
        } else err(c,s->line,"a class body may only declare fields and methods in compiled code");
    }
    c->sig_cls=save_cls;
}

/* Module namespace: functions, classes, globals; imports are linked afterwards. */
/* ---------------------------------------------------------------- ctypes */
/* What a module calls ctypes by: `import ctypes [as x]`, `from ctypes import ...`. */
typedef struct { const char *mod; const char *local[64]; const char *member[64]; int n; } CtNames;
static void ct_names(AotUnit *u, CtNames *ct){
    memset(ct,0,sizeof *ct);
    Stmt **b=u->ast->body; int n=u->ast->body_count;
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_IMPORT && !strcmp(s->name2?s->name2:s->name,"ctypes")) ct->mod=s->name;
        if(s->kind==STMT_FROM_IMPORT){
            char *dotted=aot_from_import_module(u,s);
            if(!strcmp(dotted,"ctypes"))
                for(int k=0;k<s->nnames;k++) if(strcmp(s->names[k],"*") && ct->n<64){
                    ct->local[ct->n]=s->asnames[k]?s->asnames[k]:s->names[k]; ct->member[ct->n]=s->names[k]; ct->n++; }
            free(dotted);
        }
    }
}
/* e names a member of ctypes: its name, else NULL */
static const char *ct_member(CtNames *ct, Expr *e){
    if(e->kind==EXPR_ATTRIBUTE && e->a->kind==EXPR_NAME && ct->mod && !strcmp(e->a->name,ct->mod)) return e->name;
    if(e->kind==EXPR_NAME) for(int i=0;i<ct->n;i++) if(!strcmp(ct->local[i],e->name)) return ct->member[i];
    return NULL;
}
static int ct_type(const char *m, CType *out){
    static const struct { const char *name; CType t; } map[]={
        {"c_int",CT_INT},{"c_long",CT_LONG},{"c_int32",CT_INT},{"c_ssize_t",CT_LONG},
        {"c_uint",CT_UINT},{"c_ulong",CT_ULONG},{"c_uint32",CT_UINT},{"c_size_t",CT_ULONG},
        {"c_short",CT_SHORT},{"c_int16",CT_SHORT},{"c_ushort",CT_USHORT},{"c_uint16",CT_USHORT},
        {"c_byte",CT_BYTE},{"c_int8",CT_BYTE},{"c_char",CT_BYTE},{"c_ubyte",CT_UBYTE},{"c_uint8",CT_UBYTE},{"c_bool",CT_BOOL},
        {"c_longlong",CT_LONGLONG},{"c_int64",CT_LONGLONG},{"c_ulonglong",CT_ULONGLONG},{"c_uint64",CT_ULONGLONG},
        {"c_double",CT_DOUBLE},{"c_longdouble",CT_DOUBLE},{"c_float",CT_FLOAT},{"c_char_p",CT_CHARP},{"c_void_p",CT_VOIDP},{NULL,0}};
    for(int i=0;map[i].name;i++) if(!strcmp(map[i].name,m)){ *out=map[i].t; return 1; }
    return 0;
}
static ACLib *clib_get(AProg *p, const char *soname){
    for(int i=0;i<p->nclibs;i++) if(!strcmp(p->clibs[i]->soname,soname)) return p->clibs[i];
    ACLib *l=MPY_NEW0(ACLib); l->soname=xstrdup2(soname); l->id=p->nclibs;
    p->clibs=(ACLib**)xrealloc(p->clibs,sizeof(ACLib*)*(size_t)(p->nclibs+1)); p->clibs[p->nclibs++]=l;
    return l;
}
static ACFunc *cfunc_get(AProg *p, ACLib *lib, const char *sym){
    for(int i=0;i<p->ncfuncs;i++) if(p->cfuncs[i]->lib==lib && !strcmp(p->cfuncs[i]->sym,sym)) return p->cfuncs[i];
    ACFunc *f=MPY_NEW0(ACFunc); f->lib=lib; f->sym=xstrdup2(sym); f->id=p->ncfuncs; f->nargtypes=-1;
    p->cfuncs=(ACFunc**)xrealloc(p->cfuncs,sizeof(ACFunc*)*(size_t)(p->ncfuncs+1)); p->cfuncs[p->ncfuncs++]=f;
    return f;
}
/* A C function named by e (lib.f or an alias of one) in module m, else NULL */
static ACFunc *ct_func_ref(AProg *p, AModule *m, Expr *e){
    if(e->kind==EXPR_NAME){ ASym *s=module_sym(m,e->name); return s && s->kind==AS_CFUNC ? (ACFunc*)s->p : NULL; }
    if(e->kind==EXPR_ATTRIBUTE && e->a->kind==EXPR_NAME){ ASym *s=module_sym(m,e->a->name);
        if(s && s->kind==AS_CLIB) return cfunc_get(p,(ACLib*)s->p,e->name); }
    return NULL;
}
static CType ct_decl_type(Ck *c, CtNames *ct, Expr *e){
    CType t; const char *m;
    if(e->kind==EXPR_NONE) return CT_VOID;
    if(!(m=ct_member(ct,e)) || !ct_type(m,&t)) err(c,e->line,"expected a ctypes type (ctypes.c_int, c_char_p, c_double, ...) or None");
    return t;
}
/* The module-level ctypes statements, taken out of the program:
     lib = ctypes.CDLL("libc.so.6")       (or ctypes.cdll.LoadLibrary(...), CDLL(None))
     f = lib.name                          (an alias)
     lib.name.restype = ctypes.c_char_p    lib.name.argtypes = [ctypes.c_int, ...] */
static void collect_ctypes(Ck *c, AModule *m){
    AotUnit *u=m->unit; CtNames ct; ct_names(u,&ct);
    if(!ct.mod && !ct.n) return;
    Stmt **b=u->ast->body; int n=u->ast->body_count;
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind!=STMT_ASSIGN) continue;
        AAssign *a=aot_assign(u,s);
        if(a->ntarget!=1 || !a->value || a->aug) continue;
        Expr *t=a->target[0], *v=a->value;
        if(t->kind==EXPR_NAME && v->kind==EXPR_CALL){                 /* lib = CDLL(...) */
            const char *mm=ct_member(&ct,v->a);
            int load=v->a->kind==EXPR_ATTRIBUTE && !strcmp(v->a->name,"LoadLibrary") && (mm=ct_member(&ct,v->a->a)) && !strcmp(mm,"cdll");
            if(!load && !(mm && !strcmp(mm,"CDLL"))) continue;
            if(v->count<1 || v->items[0]->akind) err(c,s->line,"CDLL() takes the library's file name");
            Expr *nm=v->items[0]; const char *so="libc.so.6";
            if(nm->kind==EXPR_LITERAL && nm->tok->kind==T_STRING) so=nm->tok->text;
            else if(nm->kind!=EXPR_NONE) err(c,s->line,"the library name must be a string literal (or None: the C library) in compiled code");
            if(c->p->target==AOT_TARGET_KOLIBRI) err(c,s->line,"ctypes libraries need the linux or macos target");
            if(module_sym(m,t->name)) err(c,s->line,"'%s' is defined twice",t->name);
            symtab_add(&m->syms,t->name,AS_CLIB,clib_get(c->p,so));
            s->kind=STMT_PASS; continue;
        }
        if(t->kind==EXPR_NAME && v->kind==EXPR_ATTRIBUTE){            /* f = lib.name */
            ACFunc *f=ct_func_ref(c->p,m,v);
            if(!f) continue;
            if(module_sym(m,t->name)) err(c,s->line,"'%s' is defined twice",t->name);
            symtab_add(&m->syms,t->name,AS_CFUNC,f);
            s->kind=STMT_PASS; continue;
        }
        if(t->kind==EXPR_ATTRIBUTE && (!strcmp(t->name,"restype") || !strcmp(t->name,"argtypes"))){
            ACFunc *f=ct_func_ref(c->p,m,t->a);
            if(!f) continue;
            if(t->name[0]=='r') f->restype=ct_decl_type(c,&ct,v);
            else {
                if(v->kind==EXPR_NONE){ f->nargtypes=-1; s->kind=STMT_PASS; continue; }
                if(v->kind!=EXPR_LIST && v->kind!=EXPR_TUPLE) err(c,s->line,"argtypes must be a list of ctypes types");
                if(v->count>16) err(c,s->line,"at most 16 argtypes");
                f->nargtypes=v->count;
                for(int k=0;k<v->count;k++){ f->argtypes[k]=ct_decl_type(c,&ct,v->items[k]);
                    if(f->argtypes[k]==CT_VOID) err(c,s->line,"None is not an argument type"); }
            }
            s->kind=STMT_PASS; continue;
        }
    }
}
/* typing.X or X (a name of the typing module or a built-in generic) */
static const char *typing_name(Expr *e){
    if(e->kind==EXPR_NAME) return e->name;
    if(e->kind==EXPR_LITERAL && e->tok->kind==T_STRING && e->tok->i!=2) return e->tok->text;    /* "TypeAlias" */
    if(e->kind==EXPR_ATTRIBUTE && e->a->kind==EXPR_NAME && !strcmp(e->a->name,"typing")) return e->name;
    return NULL;
}
/* type aliases (type X = ..., X: TypeAlias = ..., X = list[int]) and TypeVars: types, not variables */
static void collect_type_names(Ck *c, AModule *m){
    static const char *const generic[]={"list","dict","set","tuple","frozenset","type","Optional","Union","Callable","List","Dict","Set","Tuple",
        "FrozenSet","Sequence","MutableSequence","Mapping","MutableMapping","Iterable","Iterator","Generator","AbstractSet",NULL};
    Stmt **b=m->unit->ast->body; int n=m->unit->ast->body_count;
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_PASS && s->is_alias){ if(module_sym(m,s->name)) err(c,s->line,"'%s' is defined twice",s->name); symtab_add(&m->syms,s->name,AS_TYPEALIAS,s); continue; }
        if(s->kind!=STMT_ASSIGN || s->ntargets!=1 || s->targets[0]->kind!=EXPR_NAME || !s->value || s->aug) continue;
        const char *nm=s->targets[0]->name; Expr *v=s->value; const char *an=s->ann?typing_name(s->ann):NULL;
        if(v->kind==EXPR_CALL && typing_name(v->a) && (!strcmp(typing_name(v->a),"TypeVar")||!strcmp(typing_name(v->a),"ParamSpec")||!strcmp(typing_name(v->a),"TypeVarTuple"))){
            symtab_add(&m->syms,nm,AS_TYPEVAR,(void*)nm); s->kind=STMT_PASS; continue; }
        int alias= an && !strcmp(an,"TypeAlias");
        if(!alias && !s->ann && v->kind==EXPR_INDEX && typing_name(v->a)) for(int k=0;generic[k];k++) if(!strcmp(typing_name(v->a),generic[k])) alias=1;
        if(alias){ s->kind=STMT_PASS; s->is_alias=1; s->ann=v; s->name=(char*)nm; symtab_add(&m->syms,nm,AS_TYPEALIAS,s); }
    }
}
/* @lru_cache / @cache on a module function: a wrapper keeps the results in a dict by the
   arguments (least recently used first, as functools' cache) and calls the function itself */
static int memo_deco(Expr *d, int *maxsize){
    Expr *f= d->kind==EXPR_CALL ? d->a : d;
    const char *nm= f->kind==EXPR_NAME ? f->name : f->kind==EXPR_ATTRIBUTE && f->a->kind==EXPR_NAME && !strcmp(f->a->name,"functools") ? f->name : NULL;
    if(!nm || (strcmp(nm,"lru_cache") && strcmp(nm,"cache"))) return 0;
    *maxsize= nm[0]=='c' ? -1 : 128;
    if(d->kind==EXPR_CALL) for(int i=0;i<d->count;i++){ Expr *a=d->items[i];
        if(a->akind==3 && strcmp(a->kw,"maxsize")) continue;           /* (typed=...) */
        if(a->kind==EXPR_NONE) *maxsize=-1;
        else if(a->kind==EXPR_LITERAL && a->tok->kind==T_NUMBER && !a->tok->is_float) *maxsize=(int)a->tok->i;
        else return -1; }
    return 1;
}
static void memoize_defs(Ck *c, AModule *m){
    AotUnit *u=m->unit; Stmt *mod=u->ast;
    for(int i=0;i<mod->body_count;i++){ Stmt *s=mod->body[i]; int maxsize=0, which=-1;
        if(s->kind!=STMT_FUNCTION_DEF) continue;
        for(int k=0;k<s->decorator_count;k++){ int r=memo_deco(aot_expr(u,&s->decorator_exprs[k]),&maxsize); if(r<0) err(c,s->line,"lru_cache(maxsize=...) needs a number or None in compiled code"); if(r) which=k; }
        if(which<0) continue;
        if(s->decorator_count>1) err(c,s->line,"@lru_cache together with other decorators is not supported in compiled code");
        if(s->star_index>=0 || s->dstar_index>=0) err(c,s->line,"@lru_cache on a function with *args / **kwargs is not supported in compiled code");
        s->decorator_count=0;
        if(maxsize==0) continue;                                     /* (no cache) */
        char un[300], ca[300]; snprintf(un,sizeof un,"__mpy_uncached_%s",s->name); snprintf(ca,sizeof ca,"__mpy_cache_%s",s->name);
        Txt t={0}; for(int l=1;l<s->line;l++) tx(&t,"\n");
        tx(&t,"%s = {}\ndef %s(",ca,s->name);
        for(int p=0;p<s->param_count;p++) tx(&t,"%s%s%s",p?", ":"",s->params[p],s->pdefaults&&s->pdefaults[p]?"=None":"");
        tx(&t,"):\n    __mpy_k = ");
        if(!s->param_count) tx(&t,"0"); else { tx(&t,"("); for(int p=0;p<s->param_count;p++) tx(&t,"%s, ",s->params[p]); tx(&t,")"); }
        tx(&t,"\n    if __mpy_k in %s:\n        __mpy_v = %s.pop(__mpy_k)\n        %s[__mpy_k] = __mpy_v\n        return __mpy_v\n",ca,ca,ca);
        tx(&t,"    __mpy_r = %s(",un); for(int p=0;p<s->param_count;p++) tx(&t,"%s%s",p?", ":"",s->params[p]); tx(&t,")\n    %s[__mpy_k] = __mpy_r\n",ca);
        if(maxsize>0) tx(&t,"    if len(%s) > %d:\n        del %s[next(iter(%s))]\n",ca,maxsize,ca,ca);
        tx(&t,"    return __mpy_r\n");
        Stmt *blk=py_front_stmts(u->path,t.s,s->line); free(t.s);
        if(!blk || blk->body_count!=2) err(c,s->line,"internal error: lru_cache");
        Stmt *w=blk->body[1];
        for(int p=0;p<s->param_count;p++){ if(s->annotations && p<s->annotation_cap && s->annotations[p]) stmt_set_annotation(w,p,s->annotations[p]);
            if(s->pdefaults && s->pdefaults[p]){ if(!w->pdefaults) w->pdefaults=MPY_NEW_ARR(Expr*,s->param_count+1); w->pdefaults[p]=s->pdefaults[p]; } }
        w->returns=s->returns;
        s->name=xstrdup2(un);
        Stmt **nb=MPY_NEW_ARR(Stmt*,mod->body_count+3); int nn=0;
        for(int j=0;j<i;j++) nb[nn++]=mod->body[j];
        nb[nn++]=blk->body[0]; nb[nn++]=s; nb[nn++]=w;
        for(int j=i+1;j<mod->body_count;j++) nb[nn++]=mod->body[j];
        mod->body=nb; mod->body_count=nn; mod->body_cap=nn;
        i+=2; }
}
/* @cached_property def x(self): ... -> the method as __mpy_cf_x, a class attribute
   __mpy_cv_x = None and a property x computing it once, keeping it there */
static int is_cached_property(Expr *d){
    return (d->kind==EXPR_NAME && !strcmp(d->name,"cached_property"))
        || (d->kind==EXPR_ATTRIBUTE && d->a->kind==EXPR_NAME && !strcmp(d->a->name,"functools") && !strcmp(d->name,"cached_property"));
}
static void cached_properties(Ck *c, AotUnit *u, Stmt *cls){
    for(int i=0;i<cls->body_count;i++){ Stmt *s=cls->body[i];
        if(s->kind==STMT_CLASS_DEF){ cached_properties(c,u,s); continue; }
        if(s->kind!=STMT_FUNCTION_DEF) continue;
        int which=-1; for(int k=0;k<s->decorator_count;k++) if(is_cached_property(aot_expr(u,&s->decorator_exprs[k]))) which=k;
        if(which<0) continue;
        if(s->decorator_count>1) err(c,s->line,"@cached_property together with other decorators is not supported in compiled code");
        if(s->param_count!=1) err(c,s->line,"a cached_property takes only self");
        s->decorator_count=0;
        Txt t={0}; for(int l=1;l<s->line;l++) tx(&t,"\n");
        tx(&t,"__mpy_cv_%s = None\n@property\ndef %s(self):\n    __mpy_v = self.__mpy_cv_%s\n    if __mpy_v is None:\n"
              "        __mpy_v = self.__mpy_cf_%s()\n        self.__mpy_cv_%s = __mpy_v\n    return __mpy_v\n",s->name,s->name,s->name,s->name,s->name);
        Stmt *blk=py_front_stmts(u->path,t.s,s->line); free(t.s);
        if(!blk || blk->body_count!=2) err(c,s->line,"internal error: cached_property");
        blk->body[1]->returns=s->returns;
        char nm[300]; snprintf(nm,sizeof nm,"__mpy_cf_%s",s->name); s->name=xstrdup2(nm);
        Stmt **nb=MPY_NEW_ARR(Stmt*,cls->body_count+2); int nn=0;
        for(int j=0;j<=i;j++) nb[nn++]=cls->body[j];
        nb[nn++]=blk->body[0]; nb[nn++]=blk->body[1];
        for(int j=i+1;j<cls->body_count;j++) nb[nn++]=cls->body[j];
        cls->body=nb; cls->body_count=nn; cls->body_cap=nn;
        i+=2; }
}
static void collect_module(Ck *c, AModule *m){
    c->mod=m;
    memoize_defs(c,m);
    for(int i=0;i<m->unit->ast->body_count;i++) if(m->unit->ast->body[i]->kind==STMT_CLASS_DEF) cached_properties(c,m->unit,m->unit->ast->body[i]);
    collect_ctypes(c,m);                        /* lib = ctypes.CDLL(...) and its declarations: not variables */
    collect_type_names(c,m);
    Stmt **b=m->unit->ast->body; int n=m->unit->ast->body_count;
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_CLASS_DEF){ if(module_sym(m,s->name)) err(c,s->line,"'%s' is defined twice",s->name); AClass *cls=new_class(c,m,s); symtab_add(&m->syms,s->name,AS_CLASS,cls);
            for(int k=0;k<s->body_count;k++) if(s->body[k]->kind==STMT_CLASS_DEF) new_inner_class(c,m,s->body[k],cls,NULL); }
    }
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_FUNCTION_DEF){ if(module_sym(m,s->name)) err(c,s->line,"'%s' is defined twice",s->name);
            if(general_decos(m->unit,s,0)){ symtab_add(&m->syms,hidden_name(s->name,NULL),AS_FUNC,MPY_NEW0(AFunc)); global_var(c,m,s->name,s->line); }   /* name = decorators(function) */
            else symtab_add(&m->syms,s->name,AS_FUNC,MPY_NEW0(AFunc)); }
    }
    Names globals={0};
    Stmt **rest=MPY_NEW_ARR(Stmt*,n>0?n:1); int nrest=0;
    for(int i=0;i<n;i++) if(b[i]->kind!=STMT_FUNCTION_DEF && b[i]->kind!=STMT_CLASS_DEF){
        Stmt *s=b[i];
        if(s->kind==STMT_IMPORT||s->kind==STMT_FROM_IMPORT) continue;
        if(s->kind==STMT_GLOBAL) continue;
        rest[nrest++]=s;
    }
    names_bound(c,rest,nrest,&globals,NULL);
    for(int i=0;i<nrest;i++){ no_nested_defs(c,rest[i]->body,rest[i]->body_count,0); no_nested_defs(c,rest[i]->orelse,rest[i]->orelse_count,0); }
    for(int i=0;i<globals.n;i++) global_var(c,m,globals.v[i],1);
    free(globals.v); free(rest);
}
/* A fresh copy of a def (made again from the parser's tree): every instance of a
   generic function gets its own expression trees and types. */
static Stmt *stmt_clone(Ck *c, AModule *m, Stmt *s){
    Stmt *t=py_front_copy(m->unit->path,s);
    if(!t) err(c,s->line,"cannot copy %s()",s->name);
    return t;
}
static int has_untyped_params(Stmt *def){
    for(int i=0;i<def->param_count;i++) if(!def->annotations || i>=def->annotation_cap || !def->annotations[i]) return 1;
    return 0;
}
/* a fresh instance of generic module function fn (one per decorator use) -> its hidden name */
static const char *instantiate(Ck *c, AFunc *fn);
static Ty *ck_expr(Ck *c, Expr *e);
/* A call of a function without (or with generic) parameter types: one instance per kind of
   arguments (CPython runs one function for all of them; a compiled one is made for each) */
static AFunc *pick_instance(Ck *c, Expr *e, AFunc *fn, XInfo *xi){
    if(xi->fn && (xi->fn==fn || xi->fn->origin==fn)) return xi->fn;          /* (decided in an earlier pass) */
    Ty *at[16]={0}; Stmt *def=fn->def;
    for(int i=0, pos=0;i<e->count;i++){ Expr *a=e->items[i]; int p=-1;
        if(a->akind==1||a->akind==2) return fn;
        if(a->akind==3){ for(int k=0;k<fn->nparams;k++) if(!strcmp(def->params[k],a->kw)) p=k; }
        else p=pos++;
        if(p<0 || p>=fn->nparams || p==fn->star || p==fn->dstar || p>=16) return fn;
        at[p]=ty_find(ck_expr(c,a));
        if(!ty_known(at[p]) || at[p]->k==TY_VOID) at[p]=NULL;                /* (not known yet: any) */
    }
    { int any=0; for(int p=0;p<16;p++) if(at[p]) any=1; if(!any) return c->strict||!e->count ? fn : NULL; }   /* nothing known yet: wait (in the end: the function itself) */
    for(int k=-1;k<fn->ninsts;k++){ AFunc *cand=k<0?fn:fn->insts[k]; int ok=1;
        for(int p=0;p<fn->nparams && p<16 && ok;p++) if(at[p]){ Ty *q=ty_find(cand->params[p]->ty); if(q->k!=TY_VAR && !ty_same(q,at[p])) ok=0; }
        if(ok){ xi->fn=cand; return cand; } }
    const char *h=instantiate(c,fn);
    AFunc *g=(AFunc*)module_sym(fn->mod,h)->p; g->origin=fn;
    fn->insts=(AFunc**)xrealloc(fn->insts,sizeof(AFunc*)*(size_t)(fn->ninsts+1)); fn->insts[fn->ninsts++]=g;
    c->changed=1; xi->fn=g;
    return g;
}
static const char *instantiate(Ck *c, AFunc *fn){
    Stmt *copy=stmt_clone(c,fn->mod,fn->pristine);
    copy->decorator_count=0;
    AModule *save=c->mod; c->mod=fn->mod;
    AFunc *g=new_func(c,NULL,fn->mod,NULL,copy,NULL);
    c->mod=save;
    char tag[32]; snprintf(tag,sizeof tag,"#%d",++fn->ninst);
    char *h=hidden_name(fn->name,NULL); char *hn=(char*)xmalloc(strlen(h)+strlen(tag)+1); strcpy(hn,h); strcat(hn,tag); free(h);
    symtab_add(&c->mod->syms,hn,AS_FUNC,g);
    return symtab_find(&c->mod->syms,hn)->name;
}
static void collect_functions(Ck *c, AModule *m){
    c->mod=m;
    Stmt **b=m->unit->ast->body; int n=m->unit->ast->body_count;
    for(int i=0;i<c->p->nclasses;i++){ AClass *k=c->p->classes[i]; if(k->mod==m && !k->builtin && !k->encl) fill_class(c,k); }   /* (nested ones too) */
    for(int i=0;i<n;i++) if(b[i]->kind==STMT_FUNCTION_DEF){
        ASym *x=module_sym(m,b[i]->name);
        if(x->kind!=AS_FUNC){ char *h=hidden_name(b[i]->name,NULL); x=module_sym(m,h); free(h); }
        AFunc *fn=(AFunc*)x->p;
        if(!b[i]->decorator_count && (has_untyped_params(b[i]) || b[i]->ntparams)) fn->pristine=b[i];   /* (instances are made from its tree) */
        new_func(c,fn,m,NULL,b[i],NULL); }
    /* the module body itself */
    AFunc *body=MPY_NEW0(AFunc);
    body->name=xstrdup2(m->name); body->mod=m; body->body=b; body->nbody=n; body->ret=TY_VOID_T; body->line=1;
    add_func(c->p,body);
    m->body=body;
}

/* the constants of the string module */
static const char *string_const(const char *n){
    static const char *const t[][2]={{"whitespace"," \t\n\r\x0b\x0c"},{"ascii_lowercase","abcdefghijklmnopqrstuvwxyz"},{"ascii_uppercase","ABCDEFGHIJKLMNOPQRSTUVWXYZ"},
        {"ascii_letters","abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"},{"digits","0123456789"},{"hexdigits","0123456789abcdefABCDEF"},{"octdigits","01234567"},
        {"punctuation","!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"},
        {"printable","0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~ \t\n\r\x0b\x0c"},{NULL,NULL}};
    for(int i=0;t[i][0];i++) if(!strcmp(t[i][0],n)) return t[i][1];
    return NULL;
}
/* Modules the compiler provides itself (no source file). */
static const char *builtin_module(const char *name){
    static const char *mods[]={"sys","asyncio","math","time","random","typing","functools","ctypes","json","minipy","dataclasses","abc","string","thread",NULL};
    for(int i=0;mods[i];i++) if(!strcmp(mods[i],name)) return mods[i];
    return NULL;
}

/* the import statements of a module, wherever they are: the module's top level first */
static void imports_of(Stmt **b, int n, Stmt ***out, int *nout, int *cap, int nested){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_IMPORT||s->kind==STMT_FROM_IMPORT){ if(nested){ if(*nout==*cap){ *cap=*cap?*cap*2:8; *out=(Stmt**)xrealloc(*out,sizeof(Stmt*)*(size_t)*cap); } (*out)[(*nout)++]=s; } continue; }
        imports_of(s->body,s->body_count,out,nout,cap,1); imports_of(s->orelse,s->orelse_count,out,nout,cap,1);
    }
}
/* import a.b.c / import a.b as x / from a.b import n [as m] / from a import * */
static void link_imports(Ck *c, AModule *m){
    c->mod=m;
    AotUnit *u=m->unit;
    Stmt **top=u->ast->body; int ntop=u->ast->body_count;
    Stmt **b=NULL; int n=0, cap=0;
    for(int i=0;i<ntop;i++) if(top[i]->kind==STMT_IMPORT||top[i]->kind==STMT_FROM_IMPORT){ if(n==cap){ cap=cap?cap*2:8; b=(Stmt**)xrealloc(b,sizeof(Stmt*)*(size_t)cap); } b[n++]=top[i]; }
    int ntopimp=n;
    imports_of(top,ntop,&b,&n,&cap,0);                 /* then those inside functions and blocks (their names: the module's) */
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(i>=ntopimp && s->kind==STMT_IMPORT && module_sym(m,s->name)) continue;     /* already known by that name */
        if(s->kind==STMT_IMPORT){
            const char *dotted=s->name2?s->name2:s->name;
            const char *bm=builtin_module(dotted);
            if(bm){ symtab_add(&m->syms,s->name,AS_SYS,(void*)bm); if(!strcmp(bm,"dataclasses")) c->dc_conv=1; continue; }
            if(s->block_tag){ AModule *t=find_module(c->p,dotted); if(!t) err(c,s->line,"no module named '%s'",dotted); symtab_add(&m->syms,s->name,AS_MODULE,t); }
            else { AModule *t=find_module(c->p,s->name); if(!t) err(c,s->line,"no module named '%s'",s->name); symtab_add(&m->syms,s->name,AS_MODULE,t); }
        } else if(s->kind==STMT_FROM_IMPORT){
            char *dotted=aot_from_import_module(u,s);
            if(!dotted) err(c,s->line,"bad relative import");
            if(!strcmp(dotted,"typing")||!strcmp(dotted,"collections.abc")||!strcmp(dotted,"__future__")){ free(dotted); continue; }   /* type names are understood directly */
            if(!strcmp(dotted,"string")){                       /* from string import digits, capwords */
                for(int k=0;k<s->nnames;k++){ const char *nm=s->names[k], *alias=s->asnames[k]?s->asnames[k]:nm;
                    if(!string_const(nm) && strcmp(nm,"capwords")) err(c,s->line,"string.%s is not available in compiled code",nm);
                    char full[64]; snprintf(full,sizeof full,"string.%s",nm); symtab_add(&m->syms,alias,AS_SYS,xstrdup2(full)); }
                free(dotted); continue;
            }
            if(!strcmp(dotted,"abc")){                          /* from abc import ABC, abstractmethod */
                for(int k=0;k<s->nnames;k++){ const char *nm=s->names[k], *alias=s->asnames[k]?s->asnames[k]:nm;
                    if(!strcmp(nm,"ABC")) symtab_add(&m->syms,alias,AS_CLASS,abc_class(c));
                    else if(!strcmp(nm,"abstractmethod")||!strcmp(nm,"ABCMeta")) symtab_add(&m->syms,alias,AS_SYS,(void*)(nm[1]=='b'?"abc.abstractmethod":"abc.ABCMeta"));
                    else err(c,s->line,"abc.%s is not available in compiled code",nm); }
                free(dotted); continue;
            }
            if(!strcmp(dotted,"functools")){                    /* from functools import reduce */
                for(int k=0;k<s->nnames;k++){ const char *nm=s->names[k], *alias=s->asnames[k]?s->asnames[k]:nm;
                    static const char *const ok[]={"reduce","wraps","partial","lru_cache","cache","total_ordering","cmp_to_key","cached_property",NULL}; const char *full=NULL;
                    for(int q=0;ok[q];q++) if(!strcmp(ok[q],nm)){ static char names[8][32]; snprintf(names[q],sizeof names[q],"functools.%s",ok[q]); full=names[q]; }
                    if(!full) err(c,s->line,"functools.%s is not available in compiled code",nm);
                    symtab_add(&m->syms,alias,AS_SYS,(void*)full); }
                free(dotted); continue;
            }
            if(!strcmp(dotted,"ctypes")||!strcmp(dotted,"json")||!strcmp(dotted,"minipy")||!strcmp(dotted,"dataclasses")){    /* from ctypes import CDLL, c_int / from json import dumps */
                for(int k=0;k<s->nnames;k++){ const char *nm=s->names[k], *alias=s->asnames[k]?s->asnames[k]:nm;
                    if(!strcmp(nm,"*")) err(c,s->line,"use `import %s` and %s.name in compiled code",dotted,dotted);
                    if(!strcmp(dotted,"dataclasses") && !strcmp(nm,"FrozenInstanceError")){ symtab_add(&m->syms,alias,AS_CLASS,symtab_find(&builtin_syms,nm)->p); continue; }
                    if(!strcmp(dotted,"dataclasses")){ static const char *ok[]={"dataclass","field","KW_ONLY","InitVar","asdict","astuple","replace","is_dataclass","fields","MISSING",NULL};
                        int known=0; for(int q=0;ok[q];q++) if(!strcmp(ok[q],nm)) known=1;
                        if(!known) err(c,s->line,"dataclasses.%s is not available in compiled code",nm);
                        if(!strcmp(nm,"asdict")||!strcmp(nm,"astuple")) c->dc_conv=1; }
                    char full[96]; snprintf(full,sizeof full,"%s.%s",dotted,nm);
                    symtab_add(&m->syms,alias,AS_SYS,xstrdup2(full)); }
                free(dotted); continue;
            }
            if(builtin_module(dotted)) err(c,s->line,"use `import %s` and %s.name in compiled code",dotted,dotted);
            AModule *t=find_module(c->p,dotted);
            if(!t) err(c,s->line,"no module named '%s'",dotted);
            if(s->nnames==1 && !strcmp(s->names[0],"*")){
                for(int k=0;k<t->syms.n;k++){ ASym *x=&t->syms.v[k]; if(x->name[0]!='_') symtab_add(&m->syms,x->name,x->kind,x->p); }
            } else for(int k=0;k<s->nnames;k++){
                const char *nm=s->names[k], *alias=s->asnames[k]?s->asnames[k]:nm;
                if(i>=ntopimp && module_sym(m,alias)) continue;         /* (an import in a function: the name may be known already) */
                ASym *x=symtab_find(&t->syms,nm);
                if(x) symtab_add(&m->syms,alias,x->kind,x->p);
                else {
                    char sub[300]; snprintf(sub,sizeof sub,"%s.%s",dotted,nm);
                    AModule *sm=find_module(c->p,sub);
                    if(!sm) err(c,s->line,"cannot import name '%s' from '%s'",nm,dotted);
                    symtab_add(&m->syms,alias,AS_MODULE,sm);
                }
            }
            free(dotted);
        }
    }
}

/* ---------------------------------------------------------------- expressions */

static Ty *ck_expr(Ck *c, Expr *e);
static Ty *ck_cond(Ck *c, Expr *e);
/* a generator that may finish where its consumer is: its return value is StopIteration.value
   (one type for the program's StopIterations) -> what yield from gives */
static Ty *stop_value(Ck *c, Ty *g, int line){
    Ty *r=aot_gen_part(g,1);
    if(r->k==TY_VAR || r->k==TY_VOID) return r;
    AClass *si=(AClass*)symtab_find(&builtin_syms,"StopIteration")->p; AField *vf=aot_find_field(si,"value");
    expect(c,vf->ty,r,line,"StopIteration.value (the return values of the generators)");
    return r;
}
static Ty *ck_call(Ck *c, Expr *e);
static Ty *ck_coro(Ck *c, Expr *x);
static Ty *ck_tuple_as_list(Ck *c, Expr *e);
static Expr *key_as_lambda(Ck *c, Expr *key);
static Ty *ck_genexp(Ck *c, Expr *e);
static Ty *ck_reduce(Ck *c, Expr *e);
static void ck_store(Ck *c, Expr *t, Ty *vt, int line);
static int is_builtin_call(Ck *c, Expr *e, const char *name);

static AVar *var_root(AVar *v){ while(v->src) v=v->src; return v; }
/* v, a variable of `owner`, as seen from g (an inner function of owner): one
   captured copy per function in between */
static AVar *capture(AFunc *g, AFunc *owner, AVar *v){
    if(g==owner) return v;
    AVar *src=capture(g->outer,owner,v);
    for(int i=0;i<g->ncaps;i++) if(g->caps[i]->src==src) return g->caps[i];
    AVar *cv=MPY_NEW0(AVar); cv->name=v->name; cv->ty=v->ty; cv->mod=g->mod; cv->owner=g; cv->src=src; cv->bind_top=-1; cv->id=-1;
    if(g->ncaps==g->capcap){ g->capcap=g->capcap?g->capcap*2:4; g->caps=(AVar**)xrealloc(g->caps,sizeof(AVar*)*(size_t)g->capcap); g->capsym=(ASym**)xrealloc(g->capsym,sizeof(ASym*)*(size_t)g->capcap); }
    ASym *sym=MPY_NEW0(ASym); sym->name=cv->name; sym->kind=AS_VAR; sym->p=cv;
    g->caps[g->ncaps]=cv; g->capsym[g->ncaps]=sym; g->ncaps++;
    return cv;
}
static ASym *self_sym(AFunc *f){ if(!f->selfsym){ f->selfsym=MPY_NEW0(ASym); f->selfsym->name=f->name; f->selfsym->kind=AS_FUNC; f->selfsym->p=f; } return f->selfsym; }
/* A name in the current function: its comprehension/except variables, its
   locals, then those of the enclosing functions (captured), then globals. */
static ASym *lookup(Ck *c, const char *name){
    AFunc *f=c->fn;
    for(AFunc *lv=f; lv; lv=lv->outer){
        ASym *s=NULL;
        for(int i=c->ncscope-1;i>=0 && !s;i--) if(c->cscope[i].fn==lv && !strcmp(c->cscope[i].name,name)) s=&c->cscope[i].sym;
        if(!s){
            if(!is_func(lv)) break;
            for(int i=0;i<lv->nglobals_decl;i++) if(!strcmp(lv->globals_decl[i],name)) return module_sym(lv->mod,name);
            s=symtab_find(&lv->locals,name);
        }
        if(!s) continue;
        if(lv==f || s->kind!=AS_VAR) return s;
        AVar *v=(AVar*)s->p;
        if(v->global) return s;
        if(v->fn_const==f) return self_sym(f);          /* a nested function naming itself */
        v->captured=1;
        if(v->first_use_top>lv->top_index) v->first_use_top=lv->top_index;
        AVar *cv=capture(f,lv,v);
        for(int i=0;i<f->ncaps;i++) if(f->caps[i]==cv) return f->capsym[i];
    }
    { AFunc *o=f; while(o && o->outer) o=o->outer;          /* a method of a class defined in a function: that function's classes */
      if(o && o->cls && o->cls->encl) for(AFunc *e=o->cls->encl;e && e->def;e=next_scope(e)){
          ASym *s=symtab_find(&e->locals,name);
          if(s && s->kind==AS_CLASS) return s;
          if(s && s->kind==AS_VAR && !((AVar*)s->p)->global)
              err(c,f->line,"%s.%s uses '%s' of %s(): methods of a class defined in a function cannot use the function's variables in compiled code",o->cls->name,o->name,name,e->name); } }
    return global_sym(c->mod,name);
}
/* An expression naming a module (`m`, `pkg.sub`), or NULL. */
static AModule *module_expr(Ck *c, Expr *e){
    if(e->kind==EXPR_NAME){ ASym *s=lookup(c,e->name); return s && s->kind==AS_MODULE ? (AModule*)s->p : NULL; }
    if(e->kind==EXPR_ATTRIBUTE){
        AModule *m=module_expr(c,e->a); if(!m) return NULL;
        ASym *s=module_sym(m,e->name); if(s) return s->kind==AS_MODULE ? (AModule*)s->p : NULL;
        char sub[300]; snprintf(sub,sizeof sub,"%s.%s",m->name,e->name);
        return find_module(c->p,sub);
    }
    return NULL;
}
/* An expression naming a class (`C`, `Outer.Inner`, `module.C`), or NULL. */
static AClass *class_expr(Ck *c, Expr *e){
    if(e->kind==EXPR_NAME){ ASym *s=lookup(c,e->name); return s && s->kind==AS_CLASS ? (AClass*)s->p : NULL; }
    if(e->kind==EXPR_ATTRIBUTE){
        AModule *m=module_expr(c,e->a);
        if(m){ ASym *s=module_sym(m,e->name); return s && s->kind==AS_CLASS ? (AClass*)s->p : NULL; }
        if(e->a->kind==EXPR_NAME && !strcmp(e->name,"FrozenInstanceError")){ ASym *s=lookup(c,e->a->name);     /* dataclasses.FrozenInstanceError */
            if(s && s->kind==AS_SYS && !strcmp((const char*)s->p,"dataclasses")) return (AClass*)symtab_find(&builtin_syms,"FrozenInstanceError")->p; }
        AClass *o=class_expr(c,e->a); return o ? aot_nested_class(c->p,o,e->name) : NULL;
    }
    return NULL;
}
/* `name` naming a built-in module: its name, else NULL */
static const char *bmod(Ck *c, Expr *e){ if(e->kind!=EXPR_NAME) return NULL; ASym *s=lookup(c,e->name); return s && s->kind==AS_SYS ? (const char*)s->p : NULL; }
static int is_bmod(Ck *c, Expr *e, const char *name){ const char *m=bmod(c,e); return m && !strcmp(m,name); }
static int is_sys(Ck *c, Expr *e){ return is_bmod(c,e,"sys"); }
static void no_void(Ck *c, Ty *t, int line){ if(ty_find(t)->k==TY_VOID) err(c,line,"this call returns nothing (None) and has no value"); }
/* Operand types still unknown: retry in a later pass; an error in the strict pass. */
static Ty *pending(Ck *c, int line, const char *what){
    if(c->strict) err(c,line,"cannot infer the type of %s; add a type annotation",what);
    return ty_var();
}

static int is_aug_op(TokKind op){ return (op>=T_PLUS_ASSIGN && op<=T_SLASH_ASSIGN) || op>=T_PERCENT_ASSIGN; }
static const char *op_method(TokKind op){
    switch(op){
        case T_PLUS: case T_PLUS_ASSIGN: return "__add__"; case T_MINUS: case T_MINUS_ASSIGN: return "__sub__";
        case T_STAR: case T_STAR_ASSIGN: return "__mul__"; case T_SLASH: case T_SLASH_ASSIGN: return "__truediv__";
        case T_FLOOR_DIV: case T_FLOOR_DIV_ASSIGN: return "__floordiv__"; case T_PERCENT: case T_PERCENT_ASSIGN: return "__mod__";
        case T_POWER: case T_POWER_ASSIGN: return "__pow__"; case T_AMP: case T_AMP_ASSIGN: return "__and__";
        case T_PIPE: case T_PIPE_ASSIGN: return "__or__"; case T_CARET: case T_CARET_ASSIGN: return "__xor__";
        case T_SHL: case T_SHL_ASSIGN: return "__lshift__"; case T_SHR: case T_SHR_ASSIGN: return "__rshift__";
        case T_AT: case T_AT_ASSIGN: return "__matmul__";
        default: return NULL;
    }
}
/* a method of obj's class implementing an operator: m(self, arg) -> its result type */
static AFunc *op_method_of(Ck *c, Ty *obj, const char *name, Ty *arg, int line){
    obj=ty_find(obj);
    if(obj->k!=TY_OBJ || !name) return NULL;
    AFunc *m=aot_find_method(obj->cls,name);
    if(!m || m->is_static) return NULL;
    if(m->nparams!=(arg?2:1)) err(c,line,"%s.%s takes %d argument%s",obj->cls->name,name,arg?1:0,arg?"":"s");
    if(arg){ char what[96]; snprintf(what,sizeof what,"the operand of %s",name); expect(c,m->params[1]->ty,arg,line,what); }
    return m;
}
static Ty *binop_type_m(Ck *c, TokKind op, Ty *a, Ty *b, int line, AFunc **opfn);
static Ty *binop_type(Ck *c, TokKind op, Ty *a, Ty *b, int line){ return binop_type_m(c,op,a,b,line,NULL); }
static Ty *binop_type_m(Ck *c, TokKind op, Ty *a, Ty *b, int line, AFunc **opfn){
    a=ty_find(a); b=ty_find(b);
    if(a->k==TY_VOID||b->k==TY_VOID) no_void(c,TY_VOID_T,line);
    if(a->k==TY_VAR||b->k==TY_VAR) return pending(c,line,"an operand");
    if(opfn && a->k==TY_OBJ && op_method(op)){
        const char *nm=op_method(op);
        if(is_aug_op(op)){ char inm[40]; snprintf(inm,sizeof inm,"__i%s",nm+2);           /* x += y: __iadd__ first */
            AFunc *m=op_method_of(c,a,xstrdup2(inm),b,line); if(m){ *opfn=m; return m->ret; } }
        AFunc *m=op_method_of(c,a,nm,b,line); if(m){ *opfn=m; return m->ret; } }
    if(opfn && b->k==TY_OBJ && op_method(op) && !is_aug_op(op)){                         /* 2 * v: v.__rmul__(2) */
        char rnm[40]; snprintf(rnm,sizeof rnm,"__r%s",op_method(op)+2);
        AFunc *m=op_method_of(c,b,xstrdup2(rnm),a,line); if(m){ *opfn=m; c->reflected=1; return m->ret; } }
    int fl=(a->k==TY_FLOAT||b->k==TY_FLOAT);
    switch(op){
        case T_PLUS: case T_PLUS_ASSIGN:
            if(numeric(a)&&numeric(b)) return fl?TY_FLOAT_T:TY_INT_T;
            if(a->k==TY_STR&&b->k==TY_STR) return TY_STR_T;
            if(a->k==TY_BYTES&&b->k==TY_BYTES) return TY_BYTES_T;
            if(a->k==TY_LIST&&b->k==TY_LIST){ if(!unify(c,a->elem,b->elem)) break; return a; }
            break;
        case T_MINUS: case T_MINUS_ASSIGN: case T_FLOOR_DIV: case T_FLOOR_DIV_ASSIGN:
            if(numeric(a)&&numeric(b)) return fl?TY_FLOAT_T:TY_INT_T;
            if(a->k==TY_SET&&b->k==TY_SET&&(op==T_MINUS||op==T_MINUS_ASSIGN)){ if(!unify(c,a->elem,b->elem)) break; return a; }
            break;
        case T_STAR: case T_STAR_ASSIGN:
            if(numeric(a)&&numeric(b)) return fl?TY_FLOAT_T:TY_INT_T;
            if(a->k==TY_STR&&(b->k==TY_INT||b->k==TY_BOOL)) return TY_STR_T;
            if((a->k==TY_INT||a->k==TY_BOOL)&&b->k==TY_STR) return TY_STR_T;
            if(a->k==TY_BYTES&&(b->k==TY_INT||b->k==TY_BOOL)) return TY_BYTES_T;
            if((a->k==TY_INT||a->k==TY_BOOL)&&b->k==TY_BYTES) return TY_BYTES_T;
            if(a->k==TY_LIST&&(b->k==TY_INT||b->k==TY_BOOL)) return a;
            break;
        case T_SLASH: case T_SLASH_ASSIGN:
            if(numeric(a)&&numeric(b)) return TY_FLOAT_T;
            break;
        case T_PERCENT: case T_PERCENT_ASSIGN:
            if(numeric(a)&&numeric(b)) return fl?TY_FLOAT_T:TY_INT_T;
            if(a->k==TY_STR) return TY_STR_T;           /* formatting; the arguments are checked by the caller */
            break;
        case T_POWER: case T_POWER_ASSIGN:
            if(numeric(a)&&numeric(b)) return fl?TY_FLOAT_T:TY_INT_T;
            break;
        case T_AMP: case T_PIPE: case T_CARET: case T_AMP_ASSIGN: case T_PIPE_ASSIGN: case T_CARET_ASSIGN:
            if(a->k==TY_BOOL&&b->k==TY_BOOL) return TY_BOOL_T;
            if((a->k==TY_INT||a->k==TY_BOOL)&&(b->k==TY_INT||b->k==TY_BOOL)) return TY_INT_T;
            if(a->k==TY_SET&&b->k==TY_SET){ if(!unify(c,a->elem,b->elem)) break; return a; }
            break;
        case T_SHL: case T_SHR: case T_SHL_ASSIGN: case T_SHR_ASSIGN:
            if((a->k==TY_INT||a->k==TY_BOOL)&&(b->k==TY_INT||b->k==TY_BOOL)) return TY_INT_T;
            break;
        default: break;
    }
    err(c,line,"unsupported operand types %s and %s",ty_name(a),ty_name(b));
}

static int is_none(Expr *e){ return e->kind==EXPR_NONE; }
int aot_tuple_prefix(Ty *x, Ty *y);
#define tuple_prefix aot_tuple_prefix
static void ck_compare_pair_m(Ck *c, int code, Expr *ea, Expr *eb, Ty *a, Ty *b, int line, AFunc **opfn, int *neg);
static void ck_compare_pair_m(Ck *c, int code, Expr *ea, Expr *eb, Ty *a, Ty *b, int line, AFunc **opfn, int *neg){
    if(opfn && !is_none(ea) && !is_none(eb)){            /* __eq__ __ne__ __lt__ __le__ __gt__ __ge__ __contains__ */
        static const char *names[]={"__lt__","__le__","__gt__","__ge__","__eq__","__ne__"};
        AFunc *m=NULL;
        if(code<=CMP_NE) m=op_method_of(c,a,names[code],b,line);
        if(!m && code==CMP_NE){ m=op_method_of(c,a,"__eq__",b,line); if(m) *neg=1; }
        if(!m && (code==CMP_IN||code==CMP_NOTIN)) m=op_method_of(c,b,"__contains__",a,line);
        if(m){ *opfn=m; return; }
    }
    if(code==CMP_IS||code==CMP_ISNOT){
        if(is_none(ea)||is_none(eb)) return;                        /* x is None: whether x holds None */
        a=ty_find(a); b=ty_find(b);
        if(a->k==TY_VAR||b->k==TY_VAR){ pending(c,line,"an operand of 'is'"); return; }
        if(a->k==TY_OBJ&&b->k==TY_OBJ&&(aot_subclass(a->cls,b->cls)||aot_subclass(b->cls,a->cls))) return;
        if(ty_is_ptr(a) && unify(c,a,b)) return;                    /* the same object */
        if(a->k==TY_TYPE && b->k==TY_TYPE) return;                  /* the same class */
        err(c,line,"'is' compares objects (or with None); got %s and %s",ty_name(a),ty_name(b));
    }
    if(code==CMP_IN||code==CMP_NOTIN){
        Ty *h=ty_find(b);
        if(h->k==TY_VAR){ pending(c,line,"the right operand of 'in'"); return; }
        if(h->k==TY_STR){ expect(c,TY_STR_T,a,line,"the left operand of 'in'"); return; }
        if(h->k==TY_BYTES){ Ty *n=ty_find(a); if(n->k==TY_VAR){ pending(c,line,"the left operand of 'in'"); return; }
            if(n->k!=TY_BYTES&&n->k!=TY_INT&&n->k!=TY_BOOL) err(c,line,"a bytes-like object is required, not '%s'",ty_name(n)); return; }
        if(h->k==TY_DICT){ expect(c,ty_dkey(h),a,line,"the left operand of 'in'"); return; }
        if(h->k==TY_LIST||h->k==TY_SET){ expect(c,h->elem,a,line,"the left operand of 'in'"); return; }
        err(c,line,"'in' needs a str, bytes, list, set or dict on the right, not %s",ty_name(h));
    }
    if(is_none(ea)||is_none(eb)) return;
    Ty *x=ty_find(a), *y=ty_find(b);
    if(x->k==TY_VAR||y->k==TY_VAR){ if(!unify(c,x,y)) pending(c,line,"a comparison operand"); return; }
    if(numeric(x)&&numeric(y)) return;
    if(code==CMP_EQ||code==CMP_NE){
        if(x->k==TY_OBJ&&y->k==TY_OBJ&&(aot_subclass(x->cls,y->cls)||aot_subclass(y->cls,x->cls))) return;
        if(x->k==TY_STR&&y->k==TY_STR) return;
        if(x->k==TY_BYTES&&y->k==TY_BYTES) return;
        if(x->k==TY_TYPE&&y->k==TY_TYPE) return;
        if(ty_same(x,y) && (x->k==TY_LIST||x->k==TY_SET||x->k==TY_TUPLE||x->k==TY_DICT)) return;
        if(tuple_prefix(x,y)) return;                                  /* tuples of different lengths: unequal */
        if((x->k==TY_FUNC||x->k==TY_GEN) && unify(c,x,y)) return;      /* identity */
        err(c,line,"cannot compare %s with %s",ty_name(x),ty_name(y));
    }
    if(x->k==TY_TUPLE && !x->names && (ty_same(x,y) || tuple_prefix(x,y))) return;
    if(x->k==TY_STR&&y->k==TY_STR) return;
    if(x->k==TY_BYTES&&y->k==TY_BYTES) return;
    if((x->k==TY_LIST||x->k==TY_SET) && x->k==y->k && unify(c,x->elem,y->elem)) return;     /* lexicographic / subsets */
    err(c,line,"cannot order %s and %s",ty_name(x),ty_name(y));
}

/* (1, 'a') < (1, 'a', 2.0): tuples of different lengths, the shorter one's item types first in the longer */
int aot_tuple_prefix(Ty *x, Ty *y){
    x=ty_find(x); y=ty_find(y);
    if(x->k!=TY_TUPLE || y->k!=TY_TUPLE || x->names || y->names || x->nelems==y->nelems) return 0;
    int n=x->nelems<y->nelems?x->nelems:y->nelems;
    for(int i=0;i<n;i++) if(!ty_same(x->elems[i],y->elems[i])) return 0;
    return 1;
}
static Ty *elem_of(Ck *c, Ty *t, int line, const char *what){
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
            err(c,line,"%s.__iter__ must return a generator or a list, not %s",t->cls->name,ty_name(r)); }
    }
    if(t->k==TY_TUPLE && t->names) err(c,line,"%s (a dict literal with values of different types) cannot be iterated in compiled code",ty_name(t));
    if(t->k==TY_TUPLE && t->nelems>0){
        for(int i=1;i<t->nelems;i++) if(!ty_same(t->elems[i],t->elems[0])) err(c,line,"only a tuple of one item type can be iterated, not %s",ty_name(t));
        return t->elems[0];
    }
    err(c,line,"%s is not iterable",ty_name(t));
}
static int is_builtin_call(Ck *c, Expr *e, const char *name){
    return e->kind==EXPR_CALL && e->a->kind==EXPR_NAME && !strcmp(e->a->name,name) && !lookup(c,name);
}
/* Types produced by iterating `it` into n targets. range/enumerate/zip/d.items() are understood directly. */
static void iter_types(Ck *c, Expr *it, int n, Ty **out, int line){
    XInfo *xi=xinfo(it);
    if(is_builtin_call(c,it,"range")){
        if(it->count<1||it->count>3) err(c,line,"range() takes 1 to 3 arguments");
        for(int i=0;i<it->count;i++){ if(it->items[i]->akind) err(c,line,"range() takes positional arguments"); expect(c,TY_INT_T,ck_expr(c,it->items[i]),line,"a range() argument"); }
        xi->kind=X_BUILTIN; xi->name="range"; xi->ty=TY_INT_T;
        if(n!=1) err(c,line,"range() produces one value per iteration");
        out[0]=TY_INT_T; return;
    }
    if(is_builtin_call(c,it,"reversed") && it->count==1 && is_builtin_call(c,it->items[0],"range")){   /* counting down */
        Expr *r=it->items[0];
        if(r->count<1||r->count>2) err(c,line,"reversed(range(...)) takes range(stop) or range(start, stop)");
        for(int i=0;i<r->count;i++) expect(c,TY_INT_T,ck_expr(c,r->items[i]),line,"a range() argument");
        if(n!=1) err(c,line,"range() produces one value per iteration");
        xi->kind=X_BUILTIN; xi->name="reversed_range"; xi->ty=TY_INT_T; out[0]=TY_INT_T; return;
    }
    if(is_builtin_call(c,it,"enumerate")){
        if(it->count<1||it->count>2) err(c,line,"enumerate() takes 1 or 2 arguments");
        if(it->count==2) expect(c,TY_INT_T,ck_expr(c,it->items[1]),line,"enumerate() start");
        if(n!=1 && n!=2) err(c,line,"cannot unpack (index, item) pairs into %d variables",n);
        xi->kind=X_BUILTIN; xi->name="enumerate"; xi->ty=TY_INT_T;
        Ty *two[2]={TY_INT_T,elem_of(c,ck_expr(c,it->items[0]),line,"the iterable")};
        if(n==1){ out[0]=ty_tuple(two,2); return; }               /* for p in enumerate(xs): (i, x) */
        out[0]=two[0]; out[1]=two[1];
        return;
    }
    if(is_builtin_call(c,it,"zip")){
        if(it->count<1 || it->count>8) err(c,line,"zip() of 1 to 8 iterables in compiled code");
        for(int i=0;i<it->count;i++) if(it->items[i]->akind) err(c,line,"zip() takes positional arguments (strict= is not supported in compiled code)");
        if(n!=1 && n!=it->count) err(c,line,"cannot unpack zip() of %d iterables into %d variables",it->count,n);
        xi->kind=X_BUILTIN; xi->name="zip"; xi->ty=TY_INT_T;
        Ty *ts[8]; for(int i=0;i<it->count;i++) ts[i]=elem_of(c,ck_expr(c,it->items[i]),line,"the iterable");
        if(n==1){ out[0]=ty_tuple(ts,it->count); return; }       /* for t in zip(a, b): a tuple */
        for(int i=0;i<n;i++) out[i]=ts[i];
        return;
    }
    if(it->kind==EXPR_CALL && it->a->kind==EXPR_ATTRIBUTE && !strcmp(it->a->name,"items") && it->count==0){
        Ty *d=ty_find(ck_expr(c,it->a->a));
        if(d->k==TY_VAR){ out[0]=pending(c,line,"the dictionary"); if(n==2) out[1]=ty_var(); return; }
        if(d->k!=TY_DICT) err(c,line,"items() needs a dict");
        if(n!=1 && n!=2) err(c,line,"cannot unpack (key, value) pairs into %d variables",n);
        xi->kind=X_TMETHOD; xi->name="items"; xi->ty=TY_VOID_T;
        if(n==1){ Ty *two[2]={ty_dkey(d),d->elem}; out[0]=ty_tuple(two,2); return; }   /* for kv in d.items() */
        out[0]=ty_dkey(d); out[1]=d->elem; return;
    }
    Ty *t=ty_find(it->kind==EXPR_TUPLE ? ck_tuple_as_list(c,it) : ck_expr(c,it));
    Ty *el=elem_of(c,t,line,"the iterable");
    if(n==1){ out[0]=el; return; }
    Ty *e2=ty_find(el);                               /* for a, b in list_of_lists */
    if(e2->k==TY_VAR){ for(int i=0;i<n;i++) out[i]=pending(c,line,"the loop variables"); return; }
    if(e2->k==TY_TUPLE){
        if(e2->nelems!=n) err(c,line,"cannot unpack %s into %d variables",ty_name(e2),n);
        for(int i=0;i<n;i++) out[i]=e2->elems[i];
        return;
    }
    if(e2->k!=TY_LIST) err(c,line,"cannot unpack %s into %d variables",ty_name(e2),n);
    for(int i=0;i<n;i++) out[i]=e2->elem;
}

static void ck_store(Ck *c, Expr *t, Ty *vt, int line);
/* the names of a comprehension clause's target: its own variables too */
static void comp_target_vars(Ck *c, Expr *t, int line){
    if(t->kind==EXPR_NAME){ XInfo *x=xinfo(t); if(!x->var) x->var=scope_hidden(c,t->name); x->own_var=1; cs_push(c,t->name,x->var,line); return; }
    if(t->kind==EXPR_TUPLE||t->kind==EXPR_LIST) for(int i=0;i<t->count;i++) comp_target_vars(c,t->items[i],line);
}
/* The loop variables of a comprehension are its own (hidden locals/globals). */
/* async for: an async generator; for: not one */
static void async_iter_ok(Ck *c, Expr *it, int is_async, int line){
    Ty *t=xinfo(it)->ty?ty_find(xinfo(it)->ty):NULL;
    if(!t || t->k==TY_VAR) return;
    int ag= t->k==TY_GEN && t->tup;
    if(is_async && !ag) err(c,line,"'async for' requires an object with __aiter__ method, got %s",ty_name(t));
    if(!is_async && ag) err(c,line,"'async_generator' object is not iterable");
}
static Ty *ck_comprehension(Ck *c, Expr *e){
    XInfo *xi=xinfo(e);
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
        if(cl->nvars>2) err(c,e->line,"at most two loop variables are supported");
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
    return it->kind==EXPR_CALL && it->a->kind==EXPR_ATTRIBUTE && !strcmp(it->a->name,"items") && it->count==0;
}
/* A generator expression as a value: a generator function of its own whose
   parameter .0 is the first iterable (evaluated where the expression is). */
static Ty *ck_genexp(Ck *c, Expr *e){
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
        for(int i=0;i<e->nclause;i++){ if(e->clauses[i].nvars>2) err(c,e->line,"at most two loop variables are supported");
            for(int k=0;k<e->clauses[i].nvars;k++) xi->cvars[2*i+k]=hidden_local(gf,e->clauses[i].vars[k],NULL); }
    }
    gf->value_used=1; gf->ncalls++;
    if(plain){ Ty *t=ck_expr(c,e->clauses[0].iter); no_void(c,t,e->line);
        if(!unify(c,gf->params[0]->ty,t)) err(c,e->line,"cannot iterate %s",ty_name(t)); }
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
    if(!unify(c,gf->yield_ty,el)) err(c,e->line,"generator items of different types (%s and %s)",ty_name(gf->yield_ty),ty_name(el));
    xi->kind=X_NONE;
    return gf->ret;
}
/* [1, 2.5] is a float list; [Rect(), Circle()] a list of their nearest common base class */
/* Could values of types a and b go into one container? (no type variables bound) */
static int could_unify(Ty *a, Ty *b){
    a=ty_find(a); b=ty_find(b);
    if(a==b || a->k==TY_VAR || b->k==TY_VAR) return 1;
    if(numeric(a) && numeric(b)) return 1;
    if(a->k!=b->k) return 0;
    if(a->k==TY_OBJ) return aot_subclass(a->cls,b->cls) || aot_subclass(b->cls,a->cls);
    if(a->k==TY_DICT && !could_unify(ty_dkey(a),ty_dkey(b))) return 0;
    if(a->k==TY_TUPLE){ if(a->nelems!=b->nelems || !names_eq(a,b)) return 0; for(int i=0;i<a->nelems;i++) if(!could_unify(a->elems[i],b->elems[i])) return 0; return 1; }
    if(has_elem(a) && a->k!=TY_FUNC) return could_unify(a->elem,b->elem);
    return 1;
}
/* {"id": 5, "name": "x"}: distinct str-literal keys */
static int record_keys(Expr *e){
    if(e->count<2 || e->count>64) return 0;
    for(int i=0;i<e->count;i++){ Expr *k=e->items[i];
        if(k->akind || k->kind!=EXPR_LITERAL || k->tok->kind!=T_STRING) return 0;
        for(int j=0;j<i;j++) if(!strcmp(e->items[j]->tok->text,k->tok->text)) return 0; }
    return 1;
}
/* such keys and values of types no one type covers -> a record (a tuple
   whose items have the keys as names) */
static Ty *record_of(Expr *e, Ty **vts){
    if(!record_keys(e)) return NULL;
    int conflict=0;
    for(int i=0;i<e->count && !conflict;i++) for(int j=0;j<i;j++) if(!could_unify(vts[i],vts[j])){ conflict=1; break; }
    if(!conflict) return NULL;
    Ty *t=ty_tuple(vts,e->count); t->names=MPY_NEW_ARR(char*,e->count);
    for(int i=0;i<e->count;i++) t->names[i]=e->items[i]->tok->text;
    return t;
}
/* a display with *xs or **d items */
static int has_splat(Expr *e){ for(int i=0;i<e->count;i++) if(e->items[i]->akind==1||e->items[i]->akind==2) return 1; return 0; }
static Ty *literal_elem(Ck *c, Expr **items, int n){
    int fl=0, num=1, obj=n>0; AClass *base=NULL;
    for(int i=0;i<n;i++){ Ty *t=ty_find(ck_expr(c,items[i]));
        if(t->k==TY_FLOAT) fl=1; else if(t->k!=TY_INT&&t->k!=TY_BOOL) num=0;
        if(t->k!=TY_OBJ) obj=0;
        else if(obj){ if(!base) base=t->cls; else { while(base && !aot_subclass(t->cls,base)) base=base->base; if(!base) obj=0; } } }
    if(fl&&num) return TY_FLOAT_T;
    if(obj&&base) return ty_new(TY_OBJ,NULL,base);
    return ty_var();
}
/* (a, b, c) iterated or tested with `in`: a list of one element type */
static Ty *ck_tuple_as_list(Ck *c, Expr *e){
    Ty *el=literal_elem(c,e->items,e->count);
    for(int i=0;i<e->count;i++){ Ty *t=ck_expr(c,e->items[i]); no_void(c,t,e->line); expect(c,el,t,e->line,"an element"); }
    Ty *t=ty_new(TY_LIST,el,NULL);
    xinfo(e)->kind=X_NONE; xinfo(e)->ty=t;
    return t;
}
/* A container literal checked against the type it is going to (list[Base] = [Derived(), ...]). */
static void ck_def_defaults(Ck *c, AFunc *f, int line);
/* a built-in function used as a value (apply(len, s), reduce(max, xs)): lambda x: len(x) */
static int builtin_fn_value(Ck *c, Expr *e, int arity){
    static const char *const one[]={"len","abs","repr","ascii","ord","chr","hex","bin","oct","hash","sorted","sum","any","all","reversed",
        "iter","next","callable","round","min","max","print","id","format",NULL}, *const two[]={"divmod","pow","isinstance","getattr","hasattr",NULL};
    if(e->kind!=EXPR_NAME || lookup(c,e->name) || e->name[0]=='\001') return 0;
    int n=0; for(int i=0;one[i];i++) if(!strcmp(one[i],e->name)) n=1;
    for(int i=0;two[i];i++) if(!strcmp(two[i],e->name)) n=2;
    if(!n) return 0;
    if(arity>0) n=arity;
    char fn[64]; snprintf(fn,sizeof fn,"\001%s",e->name);
    Expr *call=xnew(EXPR_CALL,e->line); call->a=name_expr(xstrdup2(fn),e->line);
    char **ps=MPY_NEW_ARR(char*,n); Expr **ds=MPY_NEW_ARR(Expr*,n);
    for(int i=0;i<n;i++){ char v[16]; snprintf(v,sizeof v,"_a%d",i); ps[i]=xstrdup2(v); xpush(call,name_expr(ps[i],e->line)); }
    void *keep=e->ty; memset(e,0,sizeof *e);
    e->kind=EXPR_LAMBDA; e->line=call->line; e->ty=keep; e->eparams=ps; e->neparam=n; e->edefaults=ds; e->estar=e->edstar=-1; e->ekwonly=n; e->a=call;
    return 1;
}
static Ty *ck_expr_want(Ck *c, Expr *e, Ty *want){
    Ty *w=ty_find(want);
    if(e->kind==EXPR_NAME && w->k==TY_FUNC && builtin_fn_value(c,e,w->nelems)) return ck_expr_want(c,e,want);
    if((e->kind==EXPR_LIST||e->kind==EXPR_SET||e->kind==EXPR_TUPLE||e->kind==EXPR_DICT) && has_splat(e)) return ck_expr(c,e);
    if(e->count && (((e->kind==EXPR_LIST&&w->k==TY_LIST)||(e->kind==EXPR_SET&&w->k==TY_SET)))){
        for(int i=0;i<e->count;i++){ Ty *t=ck_expr(c,e->items[i]); no_void(c,t,e->line); expect(c,w->elem,t,e->line,"an element"); }
        xinfo(e)->kind=X_NONE; xinfo(e)->ty=w; return w;
    }
    if(e->kind==EXPR_TUPLE && w->k==TY_TUPLE && w->nelems==e->count){
        for(int i=0;i<e->count;i++){ Ty *t=ck_expr_want(c,e->items[i],w->elems[i]); no_void(c,t,e->line); expect(c,w->elems[i],t,e->line,"a tuple item"); }
        xinfo(e)->kind=X_NONE; xinfo(e)->ty=w; return w;
    }
    if(e->count && e->kind==EXPR_DICT && w->k==TY_DICT){
        for(int i=0;i<e->count;i++){ expect(c,ty_dkey(w),ck_expr(c,e->items[i]),e->line,"a dictionary key");
            Ty *t=ck_expr(c,e->vals[i]); no_void(c,t,e->line); expect(c,w->elem,t,e->line,"a dictionary value"); }
        xinfo(e)->kind=X_NONE; xinfo(e)->ty=w; return w;
    }
    return ck_expr(c,e);
}

/* An expression only tested for truth (if/while/assert, not, `x if c else y`, comprehension
   filters): `a and b` there tests a, then b, so they may be of different types */
static Ty *ck_cond(Ck *c, Expr *e){
    if(e->kind==EXPR_BOOL){ no_void(c,ck_cond(c,e->a),e->line); no_void(c,ck_cond(c,e->b),e->line); return xinfo(e)->ty=TY_BOOL_T; }
    if(e->kind==EXPR_UNARY && e->op==T_NOT){ no_void(c,ck_cond(c,e->a),e->line); return xinfo(e)->ty=TY_BOOL_T; }
    return ck_expr(c,e);
}
static Ty *ck_expr_inner(Ck *c, Expr *e){
    XInfo *xi=xinfo(e);
    xi->kind=X_NONE;
    switch(e->kind){
        case EXPR_LITERAL:{ Tok *t=e->tok;
            if(t->kind==T_STRING) return t->i==2 ? TY_BYTES_T : TY_STR_T;
            if(t->is_float) return TY_FLOAT_T;
            return TY_INT_T; }
        case EXPR_TRUE: case EXPR_FALSE: return TY_BOOL_T;
        case EXPR_NONE:{ Ty *v=ty_var(); v->opt=1; return v; }    /* makes the type it meets optional */
        case EXPR_NAME:{
            ASym *s=lookup(c,e->name);
            if(!s && !strcmp(e->name,"__name__")){ xi->kind=X_CONST_STR; xi->name=c->mod->name; return TY_STR_T; }
            if(!s && is_builtin_type_name(e->name)){ xi->kind=X_TYPEVAL; xi->name=e->name; return TY_TYPE_T; }   /* int, str, ... */
            if(s && s->kind==AS_SYS && !strncmp((const char*)s->p,"string.",7) && string_const((const char*)s->p+7)){ xi->kind=X_CONST_STR; xi->name=string_const((const char*)s->p+7); return TY_STR_T; }
            if(!s && builtin_fn_value(c,e,0)) return ck_expr(c,e);
            if(!s) err(c,e->line,"name '%s' is not defined",e->name);
            if(s->kind==AS_CLASS){ xi->kind=X_TYPEVAL; xi->cls=(AClass*)s->p; xi->cls->value_used=1; return ty_new(TY_TYPE,NULL,xi->cls); }   /* (the class: a call of it constructs one) */
            if(s->kind==AS_FUNC){ AFunc *fn=(AFunc*)s->p; xi->kind=X_FUNCREF; xi->fn=fn; fn->value_used=1; fn->ncalls++; return fn_type(c,fn,e->line); }
            if(s->kind==AS_CLIB||s->kind==AS_CFUNC) err(c,e->line,"'%s' is a C %s: call its functions",e->name,s->kind==AS_CLIB?"library":"function");
            if(s->kind!=AS_VAR) err(c,e->line,"'%s' is a %s, not a value",e->name,s->kind==AS_CLASS?"class":"module");
            xi->kind=X_VAR; xi->var=(AVar*)s->p;
            { AVar *r=var_root(xi->var); if(r->fn_const){ r->fn_const->value_used=1; r->fn_const->ncalls++; } }
            return xi->var->ty; }
        case EXPR_UNARY:{
            if(e->op==T_NOT){ no_void(c,ck_cond(c,e->a),e->line); return TY_BOOL_T; }
            Ty *t=ty_find(ck_expr(c,e->a)); no_void(c,t,e->line);
            if(t->k==TY_VAR) return pending(c,e->line,"an operand");
            if(e->op==T_TILDE){ if(t->k!=TY_INT&&t->k!=TY_BOOL) err(c,e->line,"~ needs an int"); return TY_INT_T; }
            if(t->k==TY_OBJ){ AFunc *m=op_method_of(c,t,"__neg__",NULL,e->line); if(m){ xi->kind=X_OPMETHOD; xi->fn=m; return m->ret; } }
            if(!numeric(t)) err(c,e->line,"unary - needs a number, not %s",ty_name(t));
            return t->k==TY_FLOAT?TY_FLOAT_T:TY_INT_T; }
        case EXPR_BINARY:{
            Ty *a=ck_expr(c,e->a);
            if(e->op==T_PERCENT && e->a->kind==EXPR_LITERAL && e->a->tok->kind==T_STRING && e->a->tok->i!=2){
                for(int k=0;k<e->a->count;k++) expect(c,TY_INT_T,ck_expr(c,e->a->items[k]),e->line,"a width or precision in a format spec");
                if(e->b->kind==EXPR_TUPLE){ for(int i=0;i<e->b->count;i++) no_void(c,ck_expr(c,e->b->items[i]),e->line); xinfo(e->b)->ty=TY_VOID_T; }
                else no_void(c,ck_expr(c,e->b),e->line);
                return TY_STR_T;
            }
            if(e->op==T_PERCENT && ty_find(a)->k==TY_STR) err(c,e->line,"the format string of %% must be a string literal in compiled code");
            { AFunc *m=NULL; c->reflected=0; Ty *r=binop_type_m(c,e->op,a,ck_expr(c,e->b),e->line,&m);
              if(m){ xi->kind=X_OPMETHOD; xi->fn=m; xi->reflected=c->reflected; }
              if(e->op==T_POWER && ty_find(r)->k==TY_INT && e->b->kind==EXPR_UNARY && e->b->op==T_MINUS
                 && e->b->a->kind==EXPR_LITERAL && !e->b->a->tok->is_float && e->b->a->tok->i>0) return TY_FLOAT_T;   /* 2 ** -1: a float, as in Python */
              return r; } }
        case EXPR_BOOL:{
            Ty *a=ty_find(ck_expr(c,e->a)), *b=ty_find(ck_expr(c,e->b));
            no_void(c,a,e->line); no_void(c,b,e->line);
            if(numeric(a)&&numeric(b)&&a->k!=b->k) return (a->k==TY_FLOAT||b->k==TY_FLOAT)?TY_FLOAT_T:TY_INT_T;
            if(!unify(c,a,b)) err(c,e->line,"'%s' operands must have the same type (%s and %s)",e->op==T_AND?"and":"or",ty_name(a),ty_name(b));
            return a; }
        case EXPR_COMPARE:{
            Ty *prev=ck_expr(c,e->items[0]); no_void(c,prev,e->line);
            for(int i=1;i<e->count;i++){
                int code=e->items[i]->akind;
                Ty *t=(code==CMP_IN||code==CMP_NOTIN) && e->items[i]->kind==EXPR_TUPLE ? ck_tuple_as_list(c,e->items[i]) : ck_expr(c,e->items[i]);
                no_void(c,t,e->line);
                if(!xi->cmpfn) xi->cmpfn=MPY_NEW_ARR(AFunc*,e->count);
                int neg=0; xi->cmpfn[i]=NULL;
                ck_compare_pair_m(c,e->items[i]->akind,e->items[i-1],e->items[i],prev,t,e->line,&xi->cmpfn[i],&neg);
                if(neg) xi->cmpneg|=1<<i; else xi->cmpneg&=~(1<<i);
                prev=t; }
            return TY_BOOL_T; }
        case EXPR_TERNARY:{
            no_void(c,ck_cond(c,e->a),e->line);
            Ty *a=ck_expr(c,e->b), *b=ck_expr(c,e->c);
            if(numeric(a)&&numeric(b)&&ty_find(a)->k!=ty_find(b)->k) return (ty_find(a)->k==TY_FLOAT||ty_find(b)->k==TY_FLOAT)?TY_FLOAT_T:TY_INT_T;
            if(!unify(c,a,b)) err(c,e->line,"both branches of `x if c else y` must have the same type (%s and %s)",ty_name(a),ty_name(b));
            return a; }
        case EXPR_CALL: return ck_call(c,e);
        case EXPR_ATTRIBUTE:{
            if(is_sys(c,e->a)){
                if(!strcmp(e->name,"platform")){ xi->kind=X_CONST_STR; xi->name=c->p->target==AOT_TARGET_KOLIBRI?"kolibrios":c->p->target==AOT_TARGET_MACOS?"darwin":"linux"; return TY_STR_T; }
                err(c,e->line,"sys.%s is not a value",e->name);
            }
            if(is_bmod(c,e->a,"math") && (!strcmp(e->name,"pi")||!strcmp(e->name,"e")||!strcmp(e->name,"tau")||!strcmp(e->name,"inf")||!strcmp(e->name,"nan"))){ xi->kind=X_BMOD; xi->name=e->name; return TY_FLOAT_T; }
            if(is_bmod(c,e->a,"string")){ const char *v=string_const(e->name); if(!v) err(c,e->line,"string.%s is not available in compiled code",e->name);
                xi->kind=X_CONST_STR; xi->name=v; return TY_STR_T; }
            if(bmod(c,e->a)) err(c,e->line,"%s.%s is not a value in compiled code",bmod(c,e->a),e->name);
            if(class_expr(c,e->a)){                          /* Class.CONST: a field's constant default */
                ASym csx={0}, *cs=&csx; csx.kind=AS_CLASS; csx.p=class_expr(c,e->a);
                if(cs && cs->kind==AS_CLASS){ AClass *in=aot_nested_class(c->p,(AClass*)cs->p,e->name);     /* Outer.Inner */
                    if(in){ xi->kind=X_TYPEVAL; xi->cls=in; in->value_used=1; return ty_new(TY_TYPE,NULL,in); } }
                if(cs && cs->kind==AS_CLASS && !aot_find_field((AClass*)cs->p,e->name) && aot_find_method((AClass*)cs->p,e->name)){   /* Class.method: a function taking the object */
                    AFunc *fn=aot_find_method((AClass*)cs->p,e->name);
                    if(fn->decovar) err(c,e->line,"%s.%s is decorated; use it through an object",((AClass*)cs->p)->name,e->name);
                    xi->kind=X_FUNCREF; xi->fn=fn; fn->value_used=1; return fn_type(c,fn,e->line); }
                if(cs && cs->kind==AS_CLASS && !strcmp(e->name,"__mro__")){          /* C.__mro__: its classes, object last */
                    xi->kind=X_BUILTIN; xi->name="class_mro"; xi->cls=(AClass*)cs->p; Ty *r=ty_new(TY_LIST,ty_copy(TY_TYPE_T),NULL); r->tup=1; return r; }
                if(cs && cs->kind==AS_CLASS && strcmp(e->name,"__name__") && strcmp(e->name,"__qualname__")){ AClass *cls=(AClass*)cs->p; AField *fd=aot_find_field(cls,e->name);
                    if(fd && fd->cvar){ xi->kind=X_VAR; xi->var=fd->cvar; return fd->cvar->ty; }      /* Class.name: the class-level value */
                    if(!fd || !fd->init) err(c,e->line,"class %s has no constant '%s'",cls->name,e->name);
                    Expr *v=fd->init; if(v->kind==EXPR_UNARY && v->op==T_MINUS) v=v->a;
                    if(v->kind!=EXPR_LITERAL && v->kind!=EXPR_TRUE && v->kind!=EXPR_FALSE) err(c,e->line,"%s.%s: only constant class attributes can be read through the class",cls->name,e->name);
                    xi->kind=X_CLASSCONST; xi->field=fd; return fd->ty; }
            }
            AModule *m=module_expr(c,e->a);
            if(m){
                ASym *s=module_sym(m,e->name);
                if(!s){ char sub[300]; snprintf(sub,sizeof sub,"%s.%s",m->name,e->name); if(find_module(c->p,sub)) err(c,e->line,"module %s is not a value",sub); err(c,e->line,"module %s has no attribute '%s'",m->name,e->name); }
                if(s->kind==AS_FUNC){ AFunc *fn=(AFunc*)s->p; xi->kind=X_FUNCREF; xi->fn=fn; fn->value_used=1; fn->ncalls++; return fn_type(c,fn,e->line); }
                if(s->kind!=AS_VAR) err(c,e->line,"%s.%s is not a value",m->name,e->name);
                xi->kind=X_VAR; xi->var=(AVar*)s->p; return xi->var->ty;
            }
            Ty *t=ty_find(ck_expr(c,e->a));
            if(t->k==TY_VAR) return pending(c,e->line,"the object");
            if(!strcmp(e->name,"__class__")){ xi->kind=X_BUILTIN; xi->name="type"; return TY_TYPE_T; }      /* x.__class__: type(x) */
            if(t->k==TY_OBJ && aot_is_exception(t->cls) && !strcmp(e->name,"args") && !aot_find_field(t->cls,"args")){   /* e.args: (message,) or () */
                xi->kind=X_BUILTIN; xi->name="exc_args"; Ty *r=ty_new(TY_LIST,TY_STR_T,NULL); r->tup=1; return r; }
            if(t->k==TY_TYPE && (!strcmp(e->name,"__name__")||!strcmp(e->name,"__qualname__"))){ xi->kind=X_BUILTIN; xi->name=e->name[2]=='q'?"type_qualname":"type_name"; return TY_STR_T; }
            if(t->k==TY_FUNC && !strcmp(e->name,"__name__")){       /* fn.__name__: kept in the function value */
                static AField name_field; name_field.name="__name__"; name_field.ty=TY_STR_T; name_field.offset=12;
                xi->kind=X_FIELD; xi->field=&name_field; xi->cls=NULL; return TY_STR_T; }
            if(t->k!=TY_OBJ) err(c,e->line,"%s has no attribute '%s'",ty_name(t),e->name);
            AField *fd=aot_find_field(t->cls,e->name);
            if(fd){ xi->kind=X_FIELD; xi->field=fd; xi->cls=t->cls; return fd->ty; }
            { AFunc *m=aot_find_method(t->cls,e->name);
              if(m && m->is_property){ xi->kind=X_PROP; xi->fn=m; return m->ret; }
              if(m){                                               /* obj.method as a value: bound to obj */
                  if(m->is_static){ xi->kind=X_FUNCREF; xi->fn=m; m->value_used=1; return fn_type(c,m,e->line); }
                  if(m->decovar || m->star>=0 || m->dstar>=0 || m->kwonly<m->nparams || m->is_async) err(c,e->line,"method %s.%s cannot be used as a value in compiled code (call it)",t->cls->name,e->name);
                  Ty *ps[16]; for(int i=1;i<m->nparams;i++) ps[i-1]=m->params[i]->ty;
                  xi->kind=X_BOUND; xi->fn=m; xi->cls=t->cls; m->value_used=1;
                  return ty_func(ps,m->nparams-1,m->ret); } }
            if(c->strict) err(c,e->line,"%s has no attribute '%s'",t->cls->name,e->name);
            return ty_var(); }
        case EXPR_INDEX:{
            if(e->a->kind==EXPR_NAME){ ASym *cs=lookup(c,e->a->name);           /* Box[int]: the class (type arguments only constrain types) */
                if((cs && cs->kind==AS_CLASS) || (!cs && is_builtin_type_name(e->a->name))){ Ty *t=ck_expr(c,e->a); *xi=*xinfo(e->a); return t; } }
            Ty *t=ty_find(ck_expr(c,e->a)), *i=ck_expr(c,e->b);
            if(t->k==TY_VAR) return pending(c,e->line,"the indexed value");
            if(t->k==TY_OBJ){ AFunc *m=op_method_of(c,t,"__getitem__",i,e->line); if(!m) err(c,e->line,"%s has no __getitem__",t->cls->name); xi->kind=X_OPMETHOD; xi->fn=m; return m->ret; }
            if(t->k==TY_STR){ expect(c,TY_INT_T,i,e->line,"a string index"); return TY_STR_T; }
            if(t->k==TY_BYTES){ expect(c,TY_INT_T,i,e->line,"a bytes index"); return TY_INT_T; }
            if(t->k==TY_LIST){ expect(c,TY_INT_T,i,e->line,"a list index"); return t->elem; }
            if(t->k==TY_DICT){ expect(c,ty_dkey(t),i,e->line,"a dictionary key"); return t->elem; }
            if(t->k==TY_TUPLE && t->names){                 /* record["key"] */
                Expr *k=e->b;
                if(k->kind!=EXPR_LITERAL || k->tok->kind!=T_STRING) err(c,e->line,"%s (a dict literal with values of different types) is indexed by a key literal",ty_name(t));
                for(int ix=0;ix<t->nelems;ix++) if(!strcmp(t->names[ix],k->tok->text)){ xi->argmap[0]=ix; xi->argmap[1]=1; return t->elems[ix]; }
                err(c,e->line,"KeyError: %s has no key '%s'",ty_name(t),k->tok->text);
            }
            if(t->k==TY_TUPLE){
                expect(c,TY_INT_T,i,e->line,"a tuple index");
                Expr *k=e->b; int neg=0;
                if(k->kind==EXPR_UNARY && k->op==T_MINUS && k->a->kind==EXPR_LITERAL){ neg=1; k=k->a; }
                if(k->kind==EXPR_LITERAL && k->tok->kind==T_NUMBER && !k->tok->is_float){
                    int ix=(int)(neg?-k->tok->i:k->tok->i); if(ix<0) ix+=t->nelems;
                    if(ix<0||ix>=t->nelems) err(c,e->line,"tuple index out of range");
                    xi->argmap[0]=ix; xi->argmap[1]=1; return t->elems[ix];
                }
                for(int k2=1;k2<t->nelems;k2++) if(!ty_same(t->elems[k2],t->elems[0])) err(c,e->line,"a %s can only be indexed by a constant",ty_name(t));
                xi->argmap[1]=0; return t->nelems?t->elems[0]:TY_INT_T;
            }
            err(c,e->line,"%s cannot be indexed",ty_name(t)); }
        case EXPR_SLICE:{
            Ty *t=ty_find(ck_expr(c,e->a));
            Expr *bounds[3]={e->b,e->c,e->d};
            for(int k=0;k<3;k++) if(bounds[k]) expect(c,TY_INT_T,ck_expr(c,bounds[k]),e->line,"a slice bound");
            if(t->k==TY_VAR) return pending(c,e->line,"the sliced value");
            if(t->k!=TY_STR&&t->k!=TY_LIST&&t->k!=TY_BYTES) err(c,e->line,"%s cannot be sliced",ty_name(t));
            return t; }
        case EXPR_LIST: case EXPR_SET:{
            if(has_splat(e)){                                   /* [*xs, y]: the elements of xs */
                Ty *el=ty_var();
                for(int i=0;i<e->count;i++){ Expr *x=e->items[i]; Ty *t=ck_expr(c,x); no_void(c,t,e->line);
                    expect(c,el,x->akind==1?elem_of(c,t,e->line,"the unpacked value"):t,e->line,"a list element"); }
                return ty_new(e->kind==EXPR_LIST?TY_LIST:TY_SET,el,NULL); }
            Ty *el=literal_elem(c,e->items,e->count);
            for(int i=0;i<e->count;i++){ Ty *t=ck_expr(c,e->items[i]); no_void(c,t,e->line); expect(c,el,t,e->line,"a list element"); }
            return ty_new(e->kind==EXPR_LIST?TY_LIST:TY_SET,el,NULL); }
        case EXPR_DICT:{
            if(has_splat(e)){                                   /* {**d, k: v}: d's keys and values */
                Ty *k=ty_var(), *v=ty_var();
                for(int i=0;i<e->count;i++){ Expr *x=e->items[i];
                    if(x->akind==2){ Ty *d=ty_find(ck_expr(c,x)); if(d->k==TY_VAR){ pending(c,e->line,"the unpacked dict"); continue; }
                        if(d->k!=TY_DICT) err(c,e->line,"'%s' object is not a mapping",ty_name(d));
                        expect(c,k,ty_dkey(d),e->line,"a dictionary key"); expect(c,v,d->elem,e->line,"a dictionary value"); continue; }
                    Ty *kt=ck_expr(c,x); no_void(c,kt,e->line); expect(c,k,kt,e->line,"a dictionary key");
                    Ty *t=ck_expr(c,e->vals[i]); no_void(c,t,e->line); expect(c,v,t,e->line,"a dictionary value"); }
                return ty_dict(k,v); }
            if(e->count>=2 && e->count<=64){ Ty *vts[64];
                for(int i=0;i<e->count;i++){ vts[i]=ck_expr(c,e->vals[i]); no_void(c,vts[i],e->line); }
                Ty *r=record_of(e,vts);
                if(r){ for(int i=0;i<e->count;i++) ck_expr(c,e->items[i]); return r; }
                if(record_keys(e) && !c->strict)                    /* a record or a dict? not before the values are known */
                    for(int i=0;i<e->count;i++) if(ty_find(vts[i])->k==TY_VAR) return pending(c,e->line,"a dictionary value"); }
            Ty *v=literal_elem(c,e->vals,e->count), *k=ty_var();
            for(int i=0;i<e->count;i++){ Ty *kt=ck_expr(c,e->items[i]); no_void(c,kt,e->line); expect(c,k,kt,e->line,"a dictionary key"); Ty *t=ck_expr(c,e->vals[i]); no_void(c,t,e->line); expect(c,v,t,e->line,"a dictionary value"); }
            return ty_dict(k,v); }
        case EXPR_COMPREHENSION: return e->comp_kind=='G' ? ck_genexp(c,e) : ck_comprehension(c,e);
        case EXPR_TUPLE:{
            Ty *ts[64];
            if(has_splat(e)){                                   /* (*xs, y): a tuple[T, ...] */
                Ty *el=ty_var();
                for(int i=0;i<e->count;i++){ Expr *x=e->items[i]; Ty *t=ck_expr(c,x); no_void(c,t,e->line);
                    expect(c,el,x->akind==1?elem_of(c,t,e->line,"the unpacked value"):t,e->line,"a tuple element"); }
                Ty *r=ty_new(TY_LIST,el,NULL); r->tup=1; return r; }
            if(e->count>64) err(c,e->line,"tuple too long");
            for(int i=0;i<e->count;i++){ ts[i]=ck_expr(c,e->items[i]); no_void(c,ts[i],e->line); }
            return ty_tuple(ts,e->count); }
        case EXPR_LAMBDA:{
            AFunc *lf=xi->fn; if(!lf) lf=xi->fn=new_lambda(c,e);
            lf->value_used=1; lf->ncalls++;
            ck_def_defaults(c,lf,e->line);                      /* evaluated here, once */
            AFunc *save=c->fn; c->fn=lf;
            Ty *r=e->a->kind==EXPR_NONE ? TY_VOID_T : ck_expr(c,e->a);     /* lambda: None returns nothing */
            c->fn=save;
            if(!unify(c,lf->ret,r)) err(c,e->line,"the lambda returns %s, expected %s",ty_name(r),ty_name(lf->ret));
            xi->kind=X_FUNCREF; xi->fn=lf;
            return fn_type(c,lf,e->line); }
        case EXPR_YIELD:{                                       /* x = yield v: what send() gives; x = yield from g: what g returns */
            AFunc *g=c->fn;
            if(!g || !g->is_gen) err(c,e->line,"'yield' outside a function");
            if(e->akind==7){
                Ty *ts[1]; iter_types(c,e->a,1,ts,e->line);
                expect(c,g->yield_ty,ts[0],e->line,"the yielded values");
                Ty *it=ty_find(xinfo(e->a)->ty?xinfo(e->a)->ty:TY_VOID_T);
                if(it->k!=TY_GEN) return TY_VOID_T;
                unify(c,aot_gen_part(it,0),aot_gen_part(g->ret,0));
                return aot_gen_part(it,1); }
            if(e->a){ Ty *t=ck_expr_want(c,e->a,g->yield_ty); no_void(c,t,e->line); expect(c,g->yield_ty,t,e->line,"the yielded value"); }
            return aot_gen_part(g->ret,0); }
        case EXPR_WALRUS:{                                      /* name := value: the name's type, the value stored */
            Ty *t=ck_expr(c,e->a); no_void(c,t,e->line);
            ck_store(c,e->b,t,e->line);
            xi->var=xinfo(e->b)->var; return xi->var->ty; }
        case EXPR_AWAIT:{
            if(!c->fn || !c->fn->is_async) err(c,e->line,"'await' outside an async function");
            Expr *x=e->a;
            xi->kind=X_ASYNC;
            if(x->kind==EXPR_CALL && x->a->kind==EXPR_ATTRIBUTE && is_bmod(c,x->a->a,"asyncio")){
                const char *m=x->a->name; XInfo *xx=xinfo(x);
                for(int i=0;i<x->count;i++) if(x->items[i]->akind) err(c,e->line,"asyncio.%s() takes positional arguments",m);
                if(!strcmp(m,"sleep")){
                    if(x->count!=1) err(c,e->line,"asyncio.sleep() takes one argument (seconds)");
                    Ty *t=ck_expr(c,x->items[0]); if(!numeric(t)&&!is_var(t)) err(c,e->line,"asyncio.sleep() takes a number of seconds");
                    xi->name="sleep"; xx->kind=X_ASYNC; xx->name="sleep"; xx->ty=TY_VOID_T; return TY_VOID_T;
                }
                if(!strcmp(m,"gather")){
                    Ty *r=NULL;
                    for(int i=0;i<x->count;i++){ Ty *t=ck_coro(c,x->items[i]);
                        if(!r) r=t; else if(!unify(c,r,t)) err(c,e->line,"asyncio.gather() needs coroutines of one result type (%s and %s)",ty_name(r),ty_name(t)); }
                    xi->name="gather"; xx->kind=X_ASYNC; xx->name="gather";
                    Ty *res=(!r||ty_find(r)->k==TY_VOID) ? TY_VOID_T : ty_new(TY_LIST,r,NULL);
                    xx->ty=res; return res;
                }
                return ck_expr(c,x);                     /* reports what is wrong */
            }
            if(x->kind==EXPR_CALL && ((x->a->kind==EXPR_NAME && (!strcmp(x->a->name,"anext")||!strcmp(x->a->name,"\001anext")) && !lookup(c,"anext"))
               || (x->a->kind==EXPR_ATTRIBUTE && (!strcmp(x->a->name,"__anext__")||!strcmp(x->a->name,"asend")||!strcmp(x->a->name,"athrow")||!strcmp(x->a->name,"aclose"))
                   && ty_find(ck_expr(c,x->a->a))->k==TY_GEN))){              /* await anext(g), await g.asend(v): the generator's step */
                xi->name="plain"; return ck_expr(c,x); }
            if(x->kind==EXPR_CALL){
                Expr *save=c->coro_ok; c->coro_ok=x;
                Ty *t=ck_expr(c,x);
                c->coro_ok=save;
                XInfo *xx=xinfo(x);
                if((xx->kind==X_FUNC||xx->kind==X_METHOD||xx->kind==X_STATIC||xx->kind==X_SUPER||xx->kind==X_CALLNEST) && xx->fn && xx->fn->is_async){ xi->name="call"; return t; }
                Ty *tt=ty_find(t);
                if(tt->k==TY_TASK){ xi->name="task"; return tt->elem; }
                if(tt->k==TY_VAR) return pending(c,e->line,"the awaited value");
                err(c,e->line,"await needs a coroutine call or a task, not %s",ty_name(t));
            }
            Ty *t=ty_find(ck_expr(c,x));
            if(t->k==TY_TASK){ xi->name="task"; return t->elem; }
            if(t->k==TY_VAR) return pending(c,e->line,"the awaited value");
            err(c,e->line,"await needs a coroutine call or a task, not %s",ty_name(t)); }
        default: err(c,e->line,"unsupported expression");
    }
}
static Ty *ck_expr(Ck *c, Expr *e){
    Ty *t=ck_expr_inner(c,e);
    xinfo(e)->ty=t;
    if(c->strict){
        if(c->nseen==c->cseen){ c->cseen=c->cseen?c->cseen*2:256; c->seen=(Ty**)xrealloc(c->seen,sizeof(Ty*)*(size_t)c->cseen); }
        c->seen[c->nseen++]=t;
    }
    return t;
}

/* ---------------------------------------------------------------- calls */

/* A constant default (a number, str, bytes, True / False / None) is put in at
   the call; any other is evaluated once, where its def / lambda is, and kept
   in the function object (as CPython does: f(x=[]) shares the list). */
int const_default(Expr *d){
    Expr *e=d;
    if(e->kind==EXPR_UNARY && e->op==T_MINUS && e->a->kind==EXPR_LITERAL) e=e->a;
    return e->kind==EXPR_LITERAL||e->kind==EXPR_TRUE||e->kind==EXPR_FALSE||e->kind==EXPR_NONE;
}
static Ty *ck_default(Ck *c, Expr *d){ return ck_expr(c,d); }
/* raise X from Y: Y an exception (or a class of one, or None) */
static void ck_raise_cause(Ck *c, Stmt *s){
    if(!s->expr2) return;
    Expr *y=aot_expr(c->mod->unit,&s->expr2);
    if(y->kind==EXPR_NONE) return;
    if(y->kind==EXPR_NAME){ ASym *x=lookup(c,y->name);
        if(x && x->kind==AS_CLASS){ AClass *cls=(AClass*)x->p; if(!aot_is_exception(cls)) err(c,s->line,"exception causes must derive from BaseException");
            xinfo(y)->kind=X_CTOR; xinfo(y)->cls=cls; xinfo(y)->ty=ty_new(TY_OBJ,NULL,cls); return; } }
    Ty *t=ty_find(ck_expr(c,y)); no_void(c,t,s->line);
    if(t->k!=TY_OBJ || !aot_is_exception(t->cls)) err(c,s->line,"exception causes must derive from BaseException, not %s",ty_name(t));
}
/* the defaults of f, checked where f is defined (they are evaluated there) */
static void ck_def_defaults(Ck *c, AFunc *f, int line){
    for(int i=0;i<f->nparams;i++){ Expr *d=f->defaults[i]; if(!d || i==f->star || i==f->dstar) continue;
        Ty *t=ck_expr_want(c,d,f->params[i]->ty); no_void(c,t,line);
        char what[160]; snprintf(what,sizeof what,"the default of %s's parameter '%s'",f->name,f->params[i]->name);
        expect(c,f->params[i]->ty,t,line,what); }
}
static void int_push(int **v, int *n, int x){ if(!(*n&7)) *v=(int*)xrealloc(*v,sizeof(int)*(size_t)(*n+8)); (*v)[(*n)++]=x; }
/* Match call arguments to fn's parameters (from `skip`) and check their types:
   positional, keyword, extra positionals into *args, extra keywords into
   **kwargs, f(*tuple) / f(*list) spread over parameters, f(*xs) / f(**d)
   feeding *args / **kwargs. */
static void ep_arg(Ck *c, Ty *pt, Expr *a, int line);
static void ck_args(Ck *c, Expr *e, AFunc *fn, int skip, XInfo *xi){
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
            if(t->k==TY_TUPLE && t->names) err(c,e->line,"*%s: a dict literal cannot be spread",ty_name(t));
            if(pi<poslimit && t->k==TY_TUPLE){              /* spread a tuple over the parameters */
                for(int k=0;k<t->nelems;k++){
                    if(pi>=poslimit) err(c,e->line,"%s() takes %d positional argument%s",fn->name,poslimit-skip,poslimit-skip==1?"":"s");
                    xi->argmap[pi]=i; xi->argelem[pi]=k+1; pi++; }
                continue;
            }
            if(pi<poslimit && t->k==TY_LIST){              /* the rest of the positional parameters (length checked when called) */
                if(fn->star>=0) err(c,e->line,"f(*list) into both parameters and *%s is not supported in compiled code",fn->params[fn->star]->name);
                for(int k=0;pi<poslimit;k++){ xi->argmap[pi]=i; xi->argelem[pi]=k+1; pi++; }
                continue;
            }
            if(fn->star<0 && t->k==TY_LIST && !xi->emptysplat){ xi->emptysplat=i+1; continue; }   /* nothing left for it: must be empty */
            if(fn->star<0) err(c,e->line,"%s() takes no *args",fn->name);
            if(xi->splat) err(c,e->line,"only one *argument per call is supported in compiled code");
            if(t->k!=TY_LIST && t->k!=TY_TUPLE) err(c,e->line,"*argument must be a list or a tuple, not %s",ty_name(t));
            xi->splat=i+1; continue;
        }
        if(a->akind==2){                                   /* **d */
            if(fn->dstar<0) err(c,e->line,"%s() takes no **kwargs (f(**d) needs a **kwargs parameter in compiled code)",fn->name);
            if(xi->dsplat) err(c,e->line,"only one **argument per call is supported in compiled code");
            xi->dsplat=i+1; continue;
        }
        if(a->akind==3){
            slot=-1; for(int k=skip;k<np;k++) if(k!=fn->star && k!=fn->dstar && !strcmp(fn->params[k]->name,a->kw)) slot=k;
            if(slot<0){ if(fn->dstar>=0){ int_push(&xi->kwargs,&xi->nkwargs,i); continue; } err(c,e->line,"%s() has no parameter '%s'",fn->name,a->kw); }
        } else {
            if(pi>=poslimit){ if(fn->star>=0){ int_push(&xi->xargs,&xi->nxargs,i); continue; }
                err(c,e->line,"%s() takes %d argument%s",fn->name,poslimit-skip,poslimit-skip==1?"":"s"); }
            slot=pi++;
        }
        if(xi->argmap[slot]>=0) err(c,e->line,"%s() got two values for '%s'",fn->name,fn->params[slot]->name);
        xi->argmap[slot]=i;
    }
    for(int k=skip;k<np;k++){
        char what[200]; snprintf(what,sizeof what,"parameter '%s' of %s()",fn->params[k]->name,fn->name);
        Ty *pt=fn->params[k]->ty;
        if(k==fn->star){                                    /* extra positionals: each an item of *args */
            Ty *el=ty_find(pt)->elem;
            for(int j=0;j<xi->nxargs;j++){ Ty *t=ck_expr_want(c,e->items[xi->xargs[j]],el); no_void(c,t,e->line); expect(c,el,t,e->line,what); }
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
            no_void(c,t,e->line); expect(c,pt,t,e->line,what); ep_arg(c,pt,a,e->line); }
        else if(fn->defaults[k]){ if(const_default(fn->defaults[k])){ AModule *save=c->mod; c->mod=fn->mod; Ty *t=ck_default(c,fn->defaults[k]); c->mod=save; expect(c,pt,t,e->line,what); } }
        else err(c,e->line,"%s() is missing argument '%s'",fn->name,fn->params[k]->name);
    }
}
/* f(args) where f is a function value of type ft */
/* ---- minipy.Endpoint: a function made callable with its parameters as strings
   (path and query parameters of an HTTP request). The compiler writes the
   adapter as Python and compiles it with the program:
       def __endpoint__(values: dict[str, str]) -> str:
           if "item_id" not in values: raise ValueError("#endpoint\nmissing\nitem_id\nint\n")
           a0 = int(values["item_id"])          ValueError("#endpoint\nparsing\nitem_id\nint\n<text>") if it is not one
           a1 = <default>                      (optional) / if "q" in values: a1 = values["q"]
           return json.dumps(f(a0, a1), separators=(",", ":"), ensure_ascii=False)
   An async function is called directly: the adapter runs on a task (await = a call). */
typedef struct { char *s; size_t n, cap; } SrcBuf;
static void sb_add(SrcBuf *b, const char *fmt, ...){
    va_list ap; va_start(ap,fmt); char tmp[1024]; int k=vsnprintf(tmp,sizeof tmp,fmt,ap); va_end(ap);
    if(k<0) return; if((size_t)k>=sizeof tmp) k=(int)sizeof tmp-1;
    if(b->n+(size_t)k+1>b->cap){ b->cap=(b->n+(size_t)k+1)*2; b->s=(char*)xrealloc(b->s,b->cap); }
    memcpy(b->s+b->n,tmp,(size_t)k); b->n+=(size_t)k; b->s[b->n]=0;
}
static void lit_source(Ck *c, Expr *d, SrcBuf *b, int line){
    if(d->kind==EXPR_NONE){ sb_add(b,"None"); return; }
    if(d->kind==EXPR_TRUE){ sb_add(b,"True"); return; }
    if(d->kind==EXPR_FALSE){ sb_add(b,"False"); return; }
    if(d->kind==EXPR_UNARY && d->op==T_MINUS && d->a->kind==EXPR_LITERAL && d->a->tok->kind==T_NUMBER){ sb_add(b,"-%s",d->a->tok->text); return; }
    if(d->kind==EXPR_LITERAL && d->tok->kind==T_NUMBER){ sb_add(b,"%s",d->tok->text); return; }
    if(d->kind==EXPR_LITERAL && d->tok->kind==T_STRING){
        sb_add(b,"\"");
        for(const unsigned char *p=(const unsigned char*)d->tok->text;*p;p++){
            if(*p=='"'||*p=='\\') sb_add(b,"\\%c",*p); else if(*p<32) sb_add(b,"\\x%02x",*p); else sb_add(b,"%c",*p); }
        sb_add(b,"\""); return; }
    err(c,line,"an endpoint parameter's default must be a literal");
}
static AFunc *new_func(Ck *c, AFunc *f, AModule *m, AClass *cls, Stmt *def, AFunc *outer);
static AFunc *ep_adapter(Ck *c, AFunc *fn, int line){
    if(fn->ep_adapter) return fn->ep_adapter;
    if(fn->outer || fn->cls) err(c,line,"an endpoint must be a module-level function");
    if(fn->kwonly<fn->nparams || (fn->def && (fn->def->star_index>=0 || fn->def->dstar_index>=0))) err(c,line,"endpoint %s(): only plain parameters (no *args, **kwargs, keyword-only)",fn->name);
    SrcBuf b={0};
    sb_add(&b,"def __endpoint__(values: dict[str, str]) -> str:\n");
    for(int k=0;k<fn->nparams;k++){ const char *nm=fn->params[k]->name; Ty *t=ty_find(fn->params[k]->ty);
        const char *conv=t->k==TY_INT?"int":t->k==TY_FLOAT?"float":t->k==TY_BOOL?"bool":(t->k==TY_STR||t->k==TY_VAR)?"str":NULL;
        if(!conv) err(c,line,"endpoint %s(): parameter '%s' is %s; path and query parameters are int, float, bool or str",fn->name,nm,ty_name(t));
        if(fn->defaults[k]){ sb_add(&b,"    a%d = ",k); lit_source(c,fn->defaults[k],&b,line); sb_add(&b,"\n    if \"%s\" in values:\n",nm); }
        else sb_add(&b,"    if \"%s\" not in values:\n        raise ValueError(\"#endpoint\\nmissing\\n%s\\n%s\\n\")\n    if True:\n",nm,nm,conv);
        sb_add(&b,"        v%d = values[\"%s\"]\n",k,nm);
        if(!strcmp(conv,"str")) sb_add(&b,"        a%d = v%d\n",k,k);
        else if(!strcmp(conv,"bool")){
            sb_add(&b,"        l%d = v%d.lower()\n        if l%d in (\"1\", \"true\", \"t\", \"yes\", \"y\", \"on\"):\n            a%d = True\n",k,k,k,k);
            sb_add(&b,"        elif l%d in (\"0\", \"false\", \"f\", \"no\", \"n\", \"off\"):\n            a%d = False\n",k,k);
            sb_add(&b,"        else:\n            raise ValueError(\"#endpoint\\nparsing\\n%s\\nbool\\n\" + v%d)\n",nm,k);
        } else {
            sb_add(&b,"        try:\n            a%d = %s(v%d)\n        except ValueError:\n            raise ValueError(\"#endpoint\\nparsing\\n%s\\n%s\\n\" + v%d)\n",k,conv,k,nm,conv,k);
        }
    }
    sb_add(&b,"    return __json__.dumps(__target__(");
    for(int k=0;k<fn->nparams;k++) sb_add(&b,"%sa%d",k?", ":"",k);
    sb_add(&b,"), separators=(\",\", \":\"), ensure_ascii=False)\n");
    AotUnit *u=MPY_NEW0(AotUnit);
    u->name=fn->mod->unit->name; u->path=fn->mod->unit->path; u->src=b.s;
    u->ast=py_front(u->path,b.s);
    if(!u->ast) err(c,line,"internal error: the adapter of endpoint %s() does not compile",fn->name);
    AModule *m=MPY_NEW0(AModule); m->unit=u; m->name=fn->mod->name; m->index=fn->mod->index; m->body=fn->mod->body;
    symtab_add(&m->syms,"__target__",AS_FUNC,fn);
    symtab_add(&m->syms,"__json__",AS_SYS,(void*)"json");
    Stmt *def=u->ast->body[0];
    for(int k=0;k<def->body_count;k++) def->body[k]->line=line;      /* errors in it point at the conversion */
    AModule *save=c->mod; AFunc *savef=c->fn;
    c->mod=m;
    AFunc *ad=new_func(c,NULL,m,NULL,def,NULL);
    c->mod=save; c->fn=savef;
    free(ad->name); ad->name=(char*)xmalloc(strlen(fn->name)+16); sprintf(ad->name,"%s<endpoint>",fn->name);
    ad->calls_coroutines=1; ad->ncalls++; ad->value_used=1;
    fn->ncalls++; fn->ep_adapter=ad; c->changed=1;
    return ad;
}
/* an argument passed where minipy.Endpoint is expected: the function itself */
static void ep_arg(Ck *c, Ty *pt, Expr *a, int line){
    if(!is_endpoint(pt) || is_endpoint(xinfo(a)->ty)) return;
    XInfo *x=xinfo(a);
    if(x->kind!=X_FUNCREF || !x->fn) err(c,line,"a minipy.Endpoint is made from a function given by its name");
    ep_adapter(c,x->fn,line);
}
static Ty *ck_ctor(Ck *c, Expr *e, AClass *cls);
/* cls_value(args): the constructor of each class the value may be (compared at run time) */
static Ty *ck_typecall(Ck *c, Expr *e, Ty *t){
    XInfo *xi=xinfo(e);
    if(!t->cls) err(c,e->line,"calling this type value is not supported in compiled code (a class value is needed)");
    AClass *root=t->cls, *cands[64]; int n=0;
    if(!t->tup) cands[n++]=root;
    else for(int i=0;i<c->p->nclasses && n<64;i++){ AClass *k=c->p->classes[i]; if(!k->origin && (k==root || (k->value_used && aot_subclass(k,root)))) cands[n++]=k; }
    if(!n) err(c,e->line,"no class of %s is used as a value here",root->name);
    if(!xi->cxi || xi->ncands<n){ xi->cxi=(XInfo**)xrealloc(xi->cxi,sizeof(XInfo*)*(size_t)n); for(int i=xi->ncands;i<n;i++) xi->cxi[i]=MPY_NEW0(XInfo); }
    xi->cands=(AClass**)xrealloc(xi->cands,sizeof(AClass*)*(size_t)n); memcpy(xi->cands,cands,sizeof(AClass*)*(size_t)n); xi->ncands=n;
    void *keep=e->ty;
    for(int i=0;i<n;i++){ e->ty=xi->cxi[i]; xi->cxi[i]->ty=NULL; ck_ctor(c,e,cands[i]); }    /* (each in an XInfo of its own) */
    e->ty=keep; xi->kind=X_TYPECALL;
    return ty_new(TY_OBJ,NULL,root);
}
static Ty *ck_callval(Ck *c, Expr *e, Ty *ft){
    XInfo *xi=xinfo(e);
    ft=ty_find(ft);
    if(ft->k==TY_TYPE) return ck_typecall(c,e,ft);
    if(ft->k==TY_VAR){                                  /* fn(x) of an unknown fn: it takes these arguments */
        for(int i=0;i<e->count;i++) if(e->items[i]->akind){ for(int k=0;k<e->count;k++) ck_expr(c,e->items[k]); return pending(c,e->line,"the called function"); }
        Ty *ps[16]; if(e->count>16) err(c,e->line,"too many arguments");
        for(int i=0;i<e->count;i++) ps[i]=ty_var();
        Ty *nf=ty_func(ps,e->count,ty_var()); unify(c,ft,nf); ft=nf;
    }
    if(ft->k!=TY_FUNC) err(c,e->line,"%s is not callable",ty_name(ft));
    int star=ft->tup&1, dstar=(ft->tup>>1)&1, nreg=ft->nelems-star-dstar;
    Ty *starel=star?ft->elems[nreg]:NULL, *kwel=dstar?ft->elems[nreg+star]:NULL;
    for(int i=0;i<16;i++){ xi->argmap[i]=-1; xi->argelem[i]=0; }
    xi->nxargs=0; xi->nkwargs=0; xi->splat=0; xi->dsplat=0; xi->emptysplat=0;
    int pi=0;
    #define TOO_MANY() err(c,e->line,"this function takes %d positional argument%s",nreg,nreg==1?"":"s")
    for(int i=0;i<e->count;i++){ Expr *a=e->items[i];
        if(a->akind==0){
            if(pi<nreg-ft->kwo){ int ep=is_endpoint(ft->elems[pi]); c->ep_value+=ep;
                Ty *t=ck_expr_want(c,a,ft->elems[pi]); c->ep_value-=ep;
                no_void(c,t,e->line); expect(c,ft->elems[pi],t,e->line,"an argument"); ep_arg(c,ft->elems[pi],a,e->line); xi->argmap[pi++]=i; }
            else if(star){ Ty *t=ck_expr_want(c,a,starel); no_void(c,t,e->line); expect(c,starel,t,e->line,"an argument"); int_push(&xi->xargs,&xi->nxargs,i); }
            else TOO_MANY();
        } else if(a->akind==3){
            int p=-1; if(ft->names) for(int k=0;k<nreg;k++) if(ft->names[k] && !strcmp(ft->names[k],a->kw)) p=k;
            if(p>=0){                                       /* a parameter by its name */
                if(xi->argmap[p]>=0) err(c,e->line,"this function got multiple values for argument '%s'",a->kw);
                Ty *t=ck_expr_want(c,a,ft->elems[p]); no_void(c,t,e->line); expect(c,ft->elems[p],t,e->line,"an argument"); xi->argmap[p]=i; continue; }
            if(!dstar) err(c,e->line,ft->names?"this function got an unexpected keyword argument '%s'":"keyword argument '%s': a function value takes keywords only through **kwargs (or its own parameter names) in compiled code",a->kw);
            Ty *t=ck_expr_want(c,a,kwel); no_void(c,t,e->line); expect(c,kwel,t,e->line,"a keyword argument"); int_push(&xi->kwargs,&xi->nkwargs,i);
        } else if(a->akind==1){                         /* *seq */
            Ty *t=ty_find(ck_expr(c,a)); no_void(c,t,e->line);
            if(t->k==TY_VAR){ pending(c,e->line,"the unpacked argument"); continue; }
            if(t->k!=TY_LIST && t->k!=TY_TUPLE) err(c,e->line,"*argument must be a list or a tuple, not %s",ty_name(t));
            if(pi<nreg && t->k==TY_TUPLE){
                for(int k=0;k<t->nelems;k++){ if(pi>=nreg) err(c,e->line,"f(*tuple) over both parameters and *args is not supported in compiled code");
                    expect(c,ft->elems[pi],t->elems[k],e->line,"an argument"); xi->argmap[pi]=i; xi->argelem[pi]=k+1; pi++; }
            } else if(pi<nreg){
                if(star) err(c,e->line,"f(*list) over both parameters and *args is not supported in compiled code");
                for(int k=0;pi<nreg;k++){ expect(c,ft->elems[pi],t->elem,e->line,"an argument"); xi->argmap[pi]=i; xi->argelem[pi]=k+1; pi++; }
            } else if(star){
                if(xi->splat) err(c,e->line,"only one *argument per call is supported in compiled code");
                if(t->k==TY_LIST) expect(c,starel,t->elem,e->line,"an argument"); else for(int k=0;k<t->nelems;k++) expect(c,starel,t->elems[k],e->line,"an argument");
                xi->splat=i+1;
            } else if(t->k==TY_LIST && !xi->emptysplat) xi->emptysplat=i+1;    /* nothing left for it: must be empty */
            else if(t->k!=TY_TUPLE || t->nelems) TOO_MANY();
        } else {                                        /* **d */
            Ty *t=ty_find(ck_expr(c,a));
            if(xi->dsplat) err(c,e->line,"only one **argument per call is supported in compiled code");
            if(dstar) expect(c,ty_dict(TY_STR_T,kwel),t,e->line,"the **argument");
            else if(t->k!=TY_DICT && t->k!=TY_VAR) err(c,e->line,"**argument must be a dict, not %s",ty_name(t));   /* must be empty when called */
            xi->dsplat=i+1;
        }
    }
    #undef TOO_MANY
    for(int p=0;p<nreg-ft->ndef;p++) if(xi->argmap[p]<0){                /* (the rest: its defaults) */
        if(ft->names && ft->names[p]) err(c,e->line,"this function is missing argument '%s'",ft->names[p]);
        err(c,e->line,"this function takes %d argument%s",nreg,nreg==1?"":"s"); }
    xi->kind=X_CALLVAL; xi->fn=NULL;
    return ft->elem;
}
static int class_has_subclass(Ck *c, AClass *cls){ for(int i=0;i<c->p->nclasses;i++) if(c->p->classes[i]->base==cls) return 1; return 0; }
/* A generic class's constructor call: the class (or an instance of it) whose __init__ takes these argument types */
static AClass *pick_class_instance(Ck *c, Expr *e, AClass *cls, XInfo *xi){
    if(xi->icls && (xi->icls==cls || xi->icls->origin==cls)) return xi->icls;
    AFunc *init=aot_find_method(cls,"__init__");
    if(!init || init->cls!=cls || class_has_subclass(c,cls)) return cls;
    Ty *at[16]={0}; Stmt *def=init->def; int unknown=0;
    for(int i=0, pos=1;i<e->count;i++){ Expr *a=e->items[i]; int p=-1;
        if(a->akind==1||a->akind==2) return cls;
        if(a->akind==3){ for(int k=1;k<init->nparams;k++) if(!strcmp(def->params[k],a->kw)) p=k; }
        else p=pos++;
        if(p<1 || p>=init->nparams || p==init->star || p==init->dstar || p>=16) return cls;
        at[p]=ty_find(ck_expr(c,a));
        if(!ty_known(at[p]) || at[p]->k==TY_VOID){ at[p]=NULL; if(a->kind!=EXPR_LAMBDA) unknown=1; } }   /* (a lambda's types come from the instance) */
    if(unknown && !c->strict) return NULL;                                     /* (decided once the arguments are known) */
    { int any=0; for(int p=0;p<16;p++) if(at[p]) any=1; if(!any) return c->strict||!e->count ? cls : NULL; }
    for(int k=-1;k<cls->ninsts;k++){ AClass *cand=k<0?cls:cls->insts[k]; AFunc *ci=aot_find_method(cand,"__init__"); int ok=1;
        for(int p=1;p<ci->nparams && p<16 && ok;p++) if(at[p]){ Ty *q=ty_find(ci->params[p]->ty); if(q->k!=TY_VAR && !ty_same(q,at[p])) ok=0; }
        if(ok){ xi->icls=cand; return cand; } }
    AClass *k=class_instance(c,cls); xi->icls=k;
    return k;
}
/* a copy of generic class cls (its body made again from the source) */
static AClass *class_instance(Ck *c, AClass *cls){
    Stmt *copy=stmt_clone(c,cls->mod,cls->def);
    AModule *save=c->mod; c->mod=cls->mod;
    AClass *k=new_class(c,cls->mod,copy);
    k->name=cls->name; k->qualname=cls->qualname; k->outer=cls->outer; k->encl=cls->encl; k->origin=cls;
    char h[700]; snprintf(h,sizeof h,"%s#%d",cls->symname,cls->ninsts+2);
    symtab_add(&cls->mod->syms,h,AS_CLASS,k); k->symname=symtab_find(&cls->mod->syms,h)->name;
    for(int i=0;i<copy->body_count;i++) if(copy->body[i]->kind==STMT_CLASS_DEF) new_inner_class(c,cls->mod,copy->body[i],k,NULL);
    cls->insts=(AClass**)xrealloc(cls->insts,sizeof(AClass*)*(size_t)(cls->ninsts+1)); cls->insts[cls->ninsts++]=k;
    fill_class(c,k);
    c->mod=save; c->changed=1;
    return k;
}
/* the TypeError instantiating cls raises when abstract methods have no implementation, or NULL */
static const char *abstract_message(AClass *cls){
    const char *names[64]; int n=0;
    for(AClass *k=cls;k;k=k->base) for(int i=0;i<k->nmethods;i++){ AFunc *m=k->methods[i];
        if(!m->is_abstract) continue;
        AFunc *r=aot_find_method(cls,m->name); if(!r || !r->is_abstract) continue;
        int dup=0; for(int j=0;j<n;j++) if(!strcmp(names[j],m->name)) dup=1;
        if(!dup && n<64) names[n++]=m->name; }
    if(!n) return NULL;
    for(int a=0;a<n;a++) for(int b=a+1;b<n;b++) if(strcmp(names[a],names[b])>0){ const char *t=names[a]; names[a]=names[b]; names[b]=t; }
    char buf[2048]; int l=snprintf(buf,sizeof buf,"Can't instantiate abstract class %s without an implementation for abstract method%s ",cls->name,n>1?"s":"");
    for(int i=0;i<n && l<(int)sizeof buf-80;i++) l+=snprintf(buf+l,sizeof buf-(size_t)l,"%s'%s'",i?", ":"",names[i]);
    return xstrdup2(buf);
}
static Ty *ck_ctor(Ck *c, Expr *e, AClass *cls){
    XInfo *xi=xinfo(e);
    if(cls==complex_class && e->count==1 && !e->items[0]->akind && ty_find(ck_expr(c,e->items[0]))->k==TY_STR){   /* complex("1+2j") */
        e->a=name_expr("__mpy_complex_parse",e->line); return ck_call_inner(c,e); }
    if(cls->origin) cls=cls->origin;
    if(cls->generic){ cls=pick_class_instance(c,e,cls,xi); if(!cls) return pending(c,e->line,"the arguments"); }
    AFunc *init=aot_find_method(cls,"__init__");
    xi->kind=X_CTOR; xi->cls=cls; xi->fn=init;
    if(!init && aot_is_exception(cls)){                 /* ValueError("message") */
        if(e->count>1) err(c,e->line,"%s() takes at most one argument (the message) in compiled code",cls->name);
        for(int i=0;i<e->count;i++){ if(e->items[i]->akind) err(c,e->line,"%s() takes the message as a positional argument",cls->name); no_void(c,ck_expr(c,e->items[i]),e->line); }
        xi->name="exc";
        return ty_new(TY_OBJ,NULL,cls);
    }
    if(init) ck_args(c,e,init,1,xi);
    else if(e->count) err(c,e->line,"%s() takes no arguments (it has no __init__)",cls->name);
    xi->abstract_msg=abstract_message(cls);
    for(AClass *k=cls;k;k=k->base) if(!k->constructed){ k->constructed=1; c->changed=1; }
    return ty_new(TY_OBJ,NULL,cls);
}
static Ty *ck_positional(Ck *c, Expr *e, int min, int max, const char *name){
    if(e->count<min||e->count>max){
        if(min==max) err(c,e->line,"%s() takes %d argument%s",name,min,min==1?"":"s");
        err(c,e->line,"%s() takes %d to %d arguments",name,min,max);
    }
    for(int i=0;i<e->count;i++) if(e->items[i]->akind) err(c,e->line,"%s() takes positional arguments only",name);
    return NULL;
}
static Ty *arg(Ck *c, Expr *e, int i){ Ty *t=ck_expr(c,e->items[i]); no_void(c,t,e->line); return t; }

/* key= / reverse= of sorted, sort, min, max: taken out of the argument list
   (kept in XInfo), the key checked against the element type -> key type */
static void take_sort_kwargs(Ck *c, Expr *e, int allow_reverse){
    XInfo *xi=xinfo(e); int n=0;
    for(int i=0;i<e->count;i++){ Expr *a=e->items[i];
        if(a->akind==3 && !strcmp(a->kw,"key")) xi->key=key_as_lambda(c,a);
        else if(a->akind==3 && !strcmp(a->kw,"reverse") && allow_reverse) xi->rev=a;
        else e->items[n++]=a; }
    e->count=n;
}
/* key=len / key=abs / key=str.lower: the same as lambda k: len(k) / ... */
static Expr *key_as_lambda(Ck *c, Expr *key){
    if(key->kind==EXPR_CALL && key->count==1 && !key->items[0]->akind){            /* cmp_to_key(f): lambda k: __mpy_CmpKey(k, f) */
        Expr *fn=key->a; ASym *s=fn->kind==EXPR_NAME ? lookup(c,fn->name) : NULL;
        if((s && s->kind==AS_SYS && !strcmp((const char*)s->p,"functools.cmp_to_key")) || (fn->kind==EXPR_ATTRIBUTE && is_bmod(c,fn->a,"functools") && !strcmp(fn->name,"cmp_to_key"))){
            Expr *call=xnew(EXPR_CALL,key->line); call->a=name_expr("__mpy_CmpKey",key->line);
            xpush(call,name_expr("__key",key->line)); xpush(call,key->items[0]);
            Expr *lam=xnew(EXPR_LAMBDA,key->line); lam->a=call;
            lam->eparams=MPY_NEW_ARR(char*,1); lam->eparams[0]="__key"; lam->neparam=1;
            return lam; } }
    int builtin_name = key->kind==EXPR_NAME && !lookup(c,key->name);
    int str_method = key->kind==EXPR_ATTRIBUTE && key->a->kind==EXPR_NAME && !strcmp(key->a->name,"str") && !lookup(c,"str");
    if(!builtin_name && !str_method) return key;
    Expr *param=xnew(EXPR_NAME,key->line); param->name="__key";
    Expr *call=xnew(EXPR_CALL,key->line);
    if(builtin_name){ call->a=key; xpush(call,param); }
    else { Expr *m=xnew(EXPR_ATTRIBUTE,key->line); m->a=param; m->name=key->name; call->a=m; }
    Expr *lam=xnew(EXPR_LAMBDA,key->line); lam->a=call;
    lam->eparams=MPY_NEW_ARR(char*,1); lam->eparams[0]="__key"; lam->neparam=1;
    return lam;
}
static Ty *ck_sort_key(Ck *c, Expr *key, Ty *elem, int line){
    XInfo *ki=xinfo(key); Ty *k;
    if(key->kind==EXPR_LAMBDA){
        if(key->neparam!=1) err(c,line,"a key function takes one argument");
        if(!ki->var) ki->var=scope_hidden(c,key->eparams[0]);
        expect(c,ki->var->ty,elem,line,"the key's parameter");
        int save=c->ncscope;
        cs_push(c,key->eparams[0],ki->var,line);
        k=ck_expr(c,key->a);
        c->ncscope=save;
        ki->kind=X_NONE; ki->ty=k;
    } else if(key->kind==EXPR_NAME && lookup(c,key->name) && lookup(c,key->name)->kind==AS_FUNC && !((AFunc*)lookup(c,key->name)->p)->outer){
        AFunc *fn=(AFunc*)lookup(c,key->name)->p; fn->ncalls++;
        if(fn->nparams!=1 || fn->is_async) err(c,line,"a key function takes one argument");
        expect(c,fn->params[0]->ty,elem,line,"the key's parameter");
        ki->kind=X_FUNC; ki->fn=fn; k=fn->ret; ki->ty=k;
    } else {                                                /* a function value: key=self.score, key=f */
        if(key->kind!=EXPR_NAME && key->kind!=EXPR_ATTRIBUTE) err(c,line,"key= needs a lambda or a function in compiled code");
        Ty *ft=ty_find(ck_expr(c,key));
        if(ft->k==TY_VAR){ Ty *ps[1]={ty_var()}; Ty *nf=ty_func(ps,1,ty_var()); unify(c,ft,nf); ft=nf; }
        if(ft->k!=TY_FUNC || ft->nelems!=1) err(c,line,"key= needs a function of one argument, not %s",ty_name(ft));
        expect(c,ft->elems[0],elem,line,"the key's parameter");
        k=ft->elem;
    }
    Ty *kt=ty_find(k);
    if(kt->k==TY_VAR) return pending(c,line,"the key");
    if(!numeric(kt)&&kt->k!=TY_STR&&kt->k!=TY_TUPLE&&!(kt->k==TY_OBJ && aot_find_method(kt->cls,"__lt__")))
        err(c,line,"sort keys must be numbers, strings, tuples or objects with __lt__, not %s",ty_name(kt));
    return k;
}
/* a generator expression given to a function that consumes all of it: a list comprehension */
static void genexp_as_list(Expr *e, int i){ if(i<e->count && e->items[i]->kind==EXPR_COMPREHENSION && e->items[i]->comp_kind=='G') e->items[i]->comp_kind='g'; }
/* a generator value given to such a function: list(generator) */
static Ty *gen_as_list(Ck *c, Expr *e, int i){
    if(i>=e->count) return NULL;
    Expr *a=e->items[i];
    Ty *t=ty_find(ck_expr(c,a));
    if((t->k!=TY_GEN && t->k!=TY_BYTES) || a->akind) return t;     /* a generator or bytes: list(...) of it */
    Expr *call=xnew(EXPR_CALL,a->line), *nm=xnew(EXPR_NAME,a->line); nm->name="\001list"; call->a=nm; xpush(call,a);
    e->items[i]=call;
    return ck_expr(c,call);
}
static Expr *name_expr(const char *name, int line){ Expr *n=xnew(EXPR_NAME,line); n->name=(char*)name; return n; }
/* map(f, xs[, ys]) -> [f(x) for x in xs] / [f(x, y) for x, y in zip(xs, ys)];
   filter(f, xs) -> [x for x in xs if f(x)]: lambdas are inlined */
static Ty *ck_mapfilter(Ck *c, Expr *e, const char *name){
    int isfilter=name[0]=='f', nit=e->count-1;
    for(int i=0;i<e->count;i++) if(e->items[i]->akind) err(c,e->line,"%s() takes positional arguments",name);
    if(nit<1 || (isfilter && nit!=1)) err(c,e->line,isfilter?"filter() takes a function and an iterable":"map() takes a function and iterables");
    if(nit>8) err(c,e->line,"map() over more than 8 iterables is not supported in compiled code");
    Expr *fn=e->items[0], *body; char **vars=MPY_NEW_ARR(char*,8);
    static const char *hidden[8]={"__m0","__m1","__m2","__m3","__m4","__m5","__m6","__m7"};
    if(fn->kind==EXPR_LAMBDA){
        if(fn->neparam!=nit) err(c,e->line,"the lambda of %s() takes %d argument%s",name,nit,nit==1?"":"s");
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
    e->clauses=MPY_NEW0(CompClause); e->nclause=1; e->ccap=1;
    CompClause *cl=&e->clauses[0]; cl->vars=vars; cl->nvars=nit; cl->iter=it;
    if(nit>2){                                          /* map(f, a, b, c): for (x, y, z) in zip(a, b, c) */
        Expr *tu=xnew(EXPR_TUPLE,e->line); for(int j=0;j<nit;j++) xpush(tu,name_expr(vars[j],e->line));
        char **one=MPY_NEW_ARR(char*,1); one[0]="__mt"; cl->vars=one; cl->nvars=1; cl->target=tu; }
    if(isfilter){ cl->conds=MPY_NEW_ARR(Expr*,1); cl->conds[0]=body; cl->ncond=1; e->a=name_expr(vars[0],e->line); }
    else e->a=body;
    XInfo *xi=xinfo(e); xi->kind=X_NONE;
    return ck_comprehension(c,e);
}
/* e (a call) becomes the comprehension [elem for vars in iter] ('L') or {k: v ...} ('D') */
static void make_comprehension(Expr *e, int kind, Expr *elem, Expr *val, char **vars, int nvars, Expr *iter){
    e->kind=EXPR_COMPREHENSION; e->comp_kind=kind; e->a=elem; e->b=val;
    e->clauses=MPY_NEW0(CompClause); e->nclause=1; e->ccap=1;
    CompClause *cl=&e->clauses[0]; cl->vars=vars; cl->nvars=nvars; cl->iter=iter;
    xinfo(e)->kind=X_NONE;
}
/* zip(a, b) / enumerate(xs[, start]) as a value: [(x, y) for x, y in zip(a, b)] */
static Ty *ck_pairs(Ck *c, Expr *e, const char *name){
    for(int i=0;i<e->count;i++) if(e->items[i]->akind) err(c,e->line,"%s() takes positional arguments",name);
    if(name[0]=='z' && (e->count<1||e->count>8)) err(c,e->line,"zip() of 1 to 8 iterables in compiled code");
    if(name[0]=='e' && (e->count<1||e->count>2)) err(c,e->line,"enumerate() takes 1 or 2 arguments");
    Expr *it=xnew(EXPR_CALL,e->line); it->a=name_expr(name,e->line);
    for(int i=0;i<e->count;i++) xpush(it,e->items[i]);
    char **vars=MPY_NEW_ARR(char*,1); vars[0]="__p0";            /* [t for t in zip(...)]: the tuples */
    make_comprehension(e,'L',name_expr("__p0",e->line),NULL,vars,1,it);
    return ck_comprehension(c,e);
}
/* iter(xs) -> (x for x in xs) */
static Ty *ck_iter(Ck *c, Expr *e){
    ck_positional(c,e,1,1,"iter");
    Expr *it=e->items[0];
    e->kind=EXPR_COMPREHENSION; e->comp_kind='G';
    e->clauses=MPY_NEW0(CompClause); e->nclause=1; e->ccap=1;
    CompClause *cl=&e->clauses[0]; cl->vars=MPY_NEW_ARR(char*,1); cl->vars[0]="__it"; cl->nvars=1; cl->iter=it;
    e->a=name_expr("__it",e->line);
    xinfo(e)->kind=X_NONE;
    return ck_genexp(c,e);
}
/* functools.reduce(f, xs[, init]): f's parameters become hidden variables (acc, item) */
static Ty *ck_reduce(Ck *c, Expr *e){
    XInfo *xi=xinfo(e); xi->kind=X_BUILTIN; xi->name="reduce"; int line=e->line;
    ck_positional(c,e,2,3,"reduce");
    genexp_as_list(e,1);
    Ty *lt=ty_find(gen_as_list(c,e,1));
    if(lt->k==TY_VAR) return pending(c,line,"the iterable of reduce()");
    if(lt->k!=TY_LIST) err(c,line,"reduce() needs a list, not %s",ty_name(lt));
    Ty *el=lt->elem, *acc=el;
    if(e->count==3){ acc=ty_find(ck_expr(c,e->items[2])); no_void(c,acc,line); if(acc->k==TY_INT && ty_find(el)->k==TY_FLOAT) acc=TY_FLOAT_T;
        if(ty_find(el)->k==TY_OBJ && numeric(acc) && assignable(c,el,acc)) acc=el; }      /* sum of complex numbers: 0 is 0j */
    Expr *fn=e->items[0];
    if(!xi->cvars){
        xi->cvars=MPY_NEW_ARR(AVar*,2);
        const char *n0="__r0", *n1="__r1";
        if(fn->kind==EXPR_LAMBDA){ if(fn->neparam!=2) err(c,line,"the function of reduce() takes two arguments"); n0=fn->eparams[0]; n1=fn->eparams[1]; xi->key=fn->a; }
        else { if(fn->kind!=EXPR_NAME && fn->kind!=EXPR_ATTRIBUTE) err(c,line,"reduce() needs a lambda or a function name in compiled code");
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
static void kw_to_pos(Ck *c, Expr *e, const char *m, const char *const *names, int n);
static Ty *ck_call_inner(Ck *c, Expr *e);
static Ty *ck_builtin(Ck *c, Expr *e, const char *name){
    XInfo *xi=xinfo(e); xi->kind=X_BUILTIN; xi->name=name;
    int line=e->line;
    if(!strcmp(name,"anext")||!strcmp(name,"aiter")){            /* anext(g[, default]) (awaited), aiter(g) */
        ck_positional(c,e,1,name[0]=='a'&&name[1]=='n'?2:1,name); Ty *t=ty_find(arg(c,e,0));
        if(t->k==TY_VAR) return pending(c,line,"the async iterator");
        if(t->k!=TY_GEN || !t->tup) err(c,line,"'%s' object is not an async %s",ty_name(t),name[1]=='n'?"iterator":"iterable");
        if(name[1]=='i') return t;
        if(e->count==2){ Ty *d=ck_expr_want(c,e->items[1],t->elem); no_void(c,d,line); expect(c,t->elem,d,line,"the default of anext()"); }
        return t->elem; }
    if(!strcmp(name,"downcast")){                              /* (match: an object known to be a C) */
        Ty *k=ty_find(ck_expr(c,e->items[0])); XInfo *kx=xinfo(e->items[0]); (void)k;
        Ty *v=ty_find(ck_expr(c,e->items[1]));
        if(kx->kind!=X_TYPEVAL || !kx->cls || v->k!=TY_OBJ) err(c,line,"internal error: downcast");
        return ty_new(TY_OBJ,NULL,kx->cls); }
    if(!strcmp(name,"map")||!strcmp(name,"filter")) return ck_mapfilter(c,e,name);
    if(!strcmp(name,"iter")) return ck_iter(c,e);
    if(!strcmp(name,"sum") && e->count==3 && e->items[0]->kind==EXPR_LAMBDA && e->items[0]->neparam==2
       && !strcmp(e->items[0]->eparams[0],"__mpy_s1")) return ck_reduce(c,e);   /* (sum of objects, made a reduce() in an earlier pass) */
    if(!strcmp(name,"sum")||!strcmp(name,"sorted")||!strcmp(name,"min")||!strcmp(name,"max")||!strcmp(name,"set")||!strcmp(name,"list")||!strcmp(name,"any")||!strcmp(name,"all"))
        genexp_as_list(e,0);
    if(!strcmp(name,"next")){ ck_positional(c,e,1,2,name); Ty *t=ty_find(arg(c,e,0));
        if(t->k==TY_VAR) return pending(c,line,"the argument of next()");
        if(t->k!=TY_GEN) err(c,line,"next() needs a generator (or iter(...)), not %s",ty_name(t));
        if(e->count==2){ Ty *d=ck_expr_want(c,e->items[1],t->elem); no_void(c,d,line); expect(c,t->elem,d,line,"the default of next()"); }
        stop_value(c,t,line);
        return t->elem; }
    if(!strcmp(name,"sorted")||!strcmp(name,"min")||!strcmp(name,"max")) take_sort_kwargs(c,e,name[0]=='s');
    if(!strcmp(name,"sum")||!strcmp(name,"sorted")||(e->count==1&&(!strcmp(name,"min")||!strcmp(name,"max")))||!strcmp(name,"set")||!strcmp(name,"reversed")||!strcmp(name,"any")||!strcmp(name,"all"))
        gen_as_list(c,e,0);
    if(xi->rev) expect(c,TY_BOOL_T,ck_expr(c,xi->rev),line,"reverse=");
    if(!strcmp(name,"len")){ ck_positional(c,e,1,1,name); Ty *t=ty_find(arg(c,e,0));
        if(t->k==TY_VAR) return pending(c,line,"the argument of len()");
        if(t->k==TY_STR||t->k==TY_BYTES||t->k==TY_LIST||t->k==TY_DICT||t->k==TY_SET||t->k==TY_BUF||t->k==TY_TUPLE) return TY_INT_T;
        if(t->k==TY_OBJ){ AFunc *m=aot_find_method(t->cls,"__len__"); if(m){ xi->fn=m; return TY_INT_T; } }
        err(c,line,"len() of %s",ty_name(t)); }
    if(!strcmp(name,"str") && e->count>=2){                    /* str(b, encoding[, errors]): b.decode(...) */
        static const char *const n_dec[]={"object","encoding","errors"};
        kw_to_pos(c,e,name,n_dec,3); ck_positional(c,e,2,3,name);
        expect(c,TY_BYTES_T,arg(c,e,0),line,"the argument of str()");
        for(int i=1;i<e->count;i++) if(!is_none(e->items[i])) expect(c,TY_STR_T,arg(c,e,i),line,i==1?"the encoding":"errors");
        xi->name="str_decode"; return TY_STR_T; }
    if(!strcmp(name,"str")||!strcmp(name,"repr")){ ck_positional(c,e,0,1,name); if(e->count) arg(c,e,0); return TY_STR_T; }
    if(!strcmp(name,"bytes")){
        static const char *const n_bytes[]={"source","encoding","errors"};
        kw_to_pos(c,e,name,n_bytes,3); ck_positional(c,e,0,3,name); genexp_as_list(e,0);
        if(!e->count){ xi->name="bytes_empty"; return TY_BYTES_T; }
        Ty *t=ty_find(gen_as_list(c,e,0)); if(t->k==TY_VAR) return pending(c,line,"the argument of bytes()");
        if(t->k==TY_STR){
            if(e->count<2) err(c,line,"TypeError: string argument without an encoding");
            for(int i=1;i<e->count;i++) if(!is_none(e->items[i])) expect(c,TY_STR_T,arg(c,e,i),line,i==1?"the encoding":"errors");
            xi->name="bytes_encode"; return TY_BYTES_T; }
        if(e->count>1) err(c,line,"TypeError: encoding without a string argument");
        if(t->k==TY_INT||t->k==TY_BOOL){ xi->name="bytes_zeros"; return TY_BYTES_T; }
        if(t->k==TY_BYTES){ xi->name="bytes_same"; return TY_BYTES_T; }
        if(t->k==TY_LIST||t->k==TY_SET){ expect(c,TY_INT_T,t->elem,line,"the values of bytes()"); xi->name="bytes_list"; return TY_BYTES_T; }
        err(c,line,"TypeError: cannot convert '%s' object to bytes",ty_name(t)); }
    if(!strcmp(name,"bytes.fromhex")){ ck_positional(c,e,1,1,"fromhex"); expect(c,TY_STR_T,arg(c,e,0),line,"the argument of fromhex()"); return TY_BYTES_T; }
    if(!strcmp(name,"int.from_bytes")){
        static const char *const n_fb[]={"bytes","byteorder","signed"};
        kw_to_pos(c,e,"from_bytes",n_fb,3); ck_positional(c,e,1,3,"from_bytes");
        expect(c,TY_BYTES_T,arg(c,e,0),line,"the bytes");
        if(e->count>1 && !is_none(e->items[1])) expect(c,TY_STR_T,arg(c,e,1),line,"byteorder");
        if(e->count>2) expect(c,TY_BOOL_T,arg(c,e,2),line,"signed");
        return TY_INT_T; }
    if(!strcmp(name,"int")){ ck_positional(c,e,0,1,name); if(e->count){ Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) pending(c,line,"the argument of int()"); else if(!numeric(t)&&t->k!=TY_STR) err(c,line,"int() of %s",ty_name(t)); } return TY_INT_T; }
    if(!strcmp(name,"float")){ ck_positional(c,e,0,1,name); if(e->count){ Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) pending(c,line,"the argument of float()"); else if(!numeric(t)&&t->k!=TY_STR) err(c,line,"float() of %s",ty_name(t)); } return TY_FLOAT_T; }
    if(!strcmp(name,"bool")){ ck_positional(c,e,0,1,name); if(e->count) arg(c,e,0); return TY_BOOL_T; }
    if(!strcmp(name,"abs")){ ck_positional(c,e,1,1,name); Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) return pending(c,line,"the argument of abs()");
        if(t->k==TY_OBJ && aot_find_method(t->cls,"__abs__")){ Expr *at=xnew(EXPR_ATTRIBUTE,line); at->a=e->items[0]; at->name="__abs__"; e->a=at; e->count=0; return ck_call_inner(c,e); }   /* abs(obj): obj.__abs__() */
        if(!numeric(t)) err(c,line,"bad operand type for abs(): '%s'",ty_name(t)); return t->k==TY_FLOAT?TY_FLOAT_T:TY_INT_T; }
    if(!strcmp(name,"min")||!strcmp(name,"max")){
        ck_positional(c,e,1,16,name);
        if(xi->key){ if(e->count!=1) err(c,line,"%s(key=...) takes one list",name);
            Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) return pending(c,line,"the argument");
            if(t->k!=TY_LIST&&t->k!=TY_SET&&t->k!=TY_DICT) err(c,line,"%s() of %s",name,ty_name(t));
            Ty *el=t->k==TY_DICT?ty_dkey(t):t->elem;
            ck_sort_key(c,xi->key,el,line); xi->ty=t; return el; }
        if(e->count==1){ Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) return pending(c,line,"the argument");
            if(t->k==TY_STR){ xi->ty=t; return TY_STR_T; }                     /* its characters */
            if(t->k!=TY_LIST&&t->k!=TY_SET&&t->k!=TY_DICT) err(c,line,"%s() of %s",name,ty_name(t));
            if(t->k==TY_DICT){ Ty *k=ty_find(ty_dkey(t)); if(k->k==TY_VAR) return pending(c,line,"the keys"); if(!numeric(k)&&k->k!=TY_STR) err(c,line,"%s() needs numbers or strings",name); xi->ty=t; return k; }
            Ty *el=ty_find(t->elem); if(el->k==TY_VAR) return pending(c,line,"the elements");
            if(!orderable(el)) err(c,line,"%s() needs values that can be ordered, not %s",name,ty_name(el));
            xi->ty=t; return t->elem; }
        Ty *r=NULL; int fl=0, allnum=1;
        for(int i=0;i<e->count;i++){ Ty *t=ty_find(arg(c,e,i)); if(t->k==TY_VAR) return pending(c,line,"an argument"); if(t->k==TY_FLOAT) fl=1; if(!numeric(t)) allnum=0; if(!r) r=t; else if(!allnum && !unify(c,r,t)) err(c,line,"%s() arguments must have the same type",name); }
        if(allnum) return fl?TY_FLOAT_T:TY_INT_T;
        if(ty_find(r)->k!=TY_STR) err(c,line,"%s() of %s",name,ty_name(r));
        return r; }
    if(!strcmp(name,"divmod")){ ck_positional(c,e,2,2,name); Ty *a=ty_find(arg(c,e,0)), *b=ty_find(arg(c,e,1));
        if(a->k==TY_VAR||b->k==TY_VAR) return pending(c,line,"an argument of divmod()");
        if(!numeric(a)||!numeric(b)) err(c,line,"divmod() needs numbers");
        Ty *r=(a->k==TY_FLOAT||b->k==TY_FLOAT)?TY_FLOAT_T:TY_INT_T; Ty *qr[2]={r,r}; return ty_tuple(qr,2); }
    if(!strcmp(name,"open")){
        int n=0; for(int i=0;i<e->count;i++){ Expr *a=e->items[i]; if(a->akind==3 && (!strcmp(a->kw,"encoding")||!strcmp(a->kw,"newline")||!strcmp(a->kw,"errors"))) continue; e->items[n++]=a; }
        e->count=n;                                      /* encoding= and friends: the bytes are the text */
        ck_positional(c,e,1,2,name); expect(c,TY_STR_T,arg(c,e,0),line,"the path"); if(e->count==2) expect(c,TY_STR_T,arg(c,e,1),line,"the mode");
        Expr *md= e->count==2 ? e->items[1] : NULL;            /* a literal mode with "b": a file of bytes */
        int binary= md && md->kind==EXPR_LITERAL && md->tok->kind==T_STRING && strchr(md->tok->text,'b');
        return ty_new(TY_FILE,binary?TY_BYTES_T:TY_STR_T,NULL); }
    if(!strcmp(name,"any")||!strcmp(name,"all")){ ck_positional(c,e,1,1,name); elem_of(c,arg(c,e,0),line,"the argument"); return TY_BOOL_T; }
    if(!strcmp(name,"reversed")){ ck_positional(c,e,1,1,name); Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) return pending(c,line,"the argument of reversed()");
        if(t->k!=TY_LIST&&t->k!=TY_STR&&t->k!=TY_TUPLE) err(c,line,"reversed() of %s",ty_name(t));     /* (bytes: a list by now) */
        return ty_new(TY_LIST,elem_of(c,t,line,"the argument"),NULL); }
    if(!strcmp(name,"hex")||!strcmp(name,"bin")||!strcmp(name,"oct")){ ck_positional(c,e,1,1,name); expect(c,TY_INT_T,arg(c,e,0),line,"the argument"); return TY_STR_T; }
    if(!strcmp(name,"pow")){ ck_positional(c,e,2,3,name); Ty *a=ty_find(arg(c,e,0)), *b=ty_find(arg(c,e,1));
        if(e->count==3){ expect(c,TY_INT_T,a,line,"pow() base"); expect(c,TY_INT_T,b,line,"pow() exponent"); expect(c,TY_INT_T,arg(c,e,2),line,"pow() modulus"); return TY_INT_T; }
        return binop_type(c,T_POWER,a,b,line); }
    if(!strcmp(name,"format")){ ck_positional(c,e,1,2,name); arg(c,e,0);
        if(e->count==2){ Expr *sp=e->items[1]; if(sp->kind!=EXPR_LITERAL||sp->tok->kind!=T_STRING) err(c,line,"format() needs a literal spec in compiled code"); }
        return TY_STR_T; }
    if(!strcmp(name,"ord")){ ck_positional(c,e,1,1,name); expect(c,TY_STR_T,arg(c,e,0),line,"the argument of ord()"); return TY_INT_T; }
    if(!strcmp(name,"chr")){ ck_positional(c,e,1,1,name); expect(c,TY_INT_T,arg(c,e,0),line,"the argument of chr()"); return TY_STR_T; }
    if(!strcmp(name,"sum") && e->count>=1 && e->count<=2){ Ty *t=ty_find(arg(c,e,0));      /* sum(objects[, start]): start + x0 + x1 ... (their __add__) */
        if(t->k==TY_VAR) return pending(c,line,"the argument of sum()");
        if((t->k==TY_LIST||t->k==TY_SET) && ty_find(t->elem)->k==TY_OBJ){
            Expr *lam=py_front_expr(c->mod->unit->path,"lambda __mpy_s1, __mpy_s2: __mpy_s1 + __mpy_s2",line);
            Expr *start= e->count==2 ? e->items[1] : py_front_expr(c->mod->unit->path,"0",line);
            Expr *xs=e->items[0]; start->kw=NULL; start->akind=0;
            e->count=0; xpush(e,lam); xpush(e,xs); xpush(e,start);
            return ck_reduce(c,e); } }
    if(!strcmp(name,"sum")){ ck_positional(c,e,1,1,name); Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) return pending(c,line,"the argument of sum()");
        if((t->k!=TY_LIST&&t->k!=TY_SET)) err(c,line,"sum() of %s",ty_name(t));
        Ty *el=ty_find(t->elem); if(el->k==TY_VAR) return pending(c,line,"the elements"); if(!numeric(el)) err(c,line,"sum() of %s",ty_name(t));
        return el->k==TY_FLOAT?TY_FLOAT_T:TY_INT_T; }
    if(!strcmp(name,"sorted")){ ck_positional(c,e,1,1,name); Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) return pending(c,line,"the argument of sorted()");
        if(t->k!=TY_LIST&&t->k!=TY_SET&&t->k!=TY_DICT&&t->k!=TY_STR) err(c,line,"sorted() of %s",ty_name(t));
        Ty *el=t->k==TY_DICT?ty_dkey(t):t->k==TY_STR?TY_STR_T:t->elem;
        if(xi->key){ ck_sort_key(c,xi->key,el,line); return ty_new(TY_LIST,el,NULL); }
        Ty *f=ty_find(el); if(f->k==TY_VAR) return pending(c,line,"the elements");
        if(!orderable(f)) err(c,line,"sorted() needs values that can be ordered, not %s",ty_name(f));
        return ty_new(TY_LIST,el,NULL); }
    if(!strcmp(name,"isinstance")||!strcmp(name,"issubclass")){
        ck_positional(c,e,2,2,name);
        Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) pending(c,line,"the object");
        if(name[2]=='s' && t->k!=TY_TYPE) err(c,line,"TypeError: issubclass() arg 1 must be a class");
        Expr *k=e->items[1]; int n=k->kind==EXPR_TUPLE?k->count:1;
        for(int i=0;i<n;i++){ Expr *x=k->kind==EXPR_TUPLE?k->items[i]:k;       /* classes and built-in types, by name */
            ASym *s=x->kind==EXPR_NAME?lookup(c,x->name):NULL;
            if(s && s->kind==AS_CLASS){ xinfo(x)->kind=X_TYPEVAL; xinfo(x)->cls=(AClass*)s->p; xinfo(x)->ty=TY_TYPE_T; continue; }
            if(!s && x->kind==EXPR_NAME && is_builtin_type_name(x->name)){ xinfo(x)->kind=X_TYPEVAL; xinfo(x)->name=x->name; xinfo(x)->ty=TY_TYPE_T; continue; }
            if(!s && x->kind==EXPR_CALL && x->a->kind==EXPR_NAME && !strcmp(x->a->name,"type") && x->count==1 && x->items[0]->kind==EXPR_NONE){   /* type(None) */
                xinfo(x)->kind=X_TYPEVAL; xinfo(x)->name="NoneType"; xinfo(x)->ty=TY_TYPE_T; continue; }
            err(c,line,"%s() needs a class (or a tuple of classes) in compiled code",name); }
        return TY_BOOL_T; }
    if(!strcmp(name,"getattr")||!strcmp(name,"hasattr")){          /* getattr(o, "name"[, default]) / hasattr(o, "name"): by a literal name */
        ck_positional(c,e,2,name[0]=='g'?3:2,name);
        Expr *k=e->items[1];
        if(k->kind!=EXPR_LITERAL || k->tok->kind!=T_STRING || k->tok->i==2) err(c,line,"%s() with a computed name is not supported in compiled code (the attributes are fixed)",name);
        Expr *o=e->items[0]; Ty *t=ty_find(ck_expr(c,o));
        if(t->k==TY_VAR) return pending(c,line,"the object");
        const char *an=k->tok->text;
        int has= t->k==TY_OBJ ? (aot_find_field(t->cls,an) || aot_find_method(t->cls,an))
               : (t->k==TY_TYPE||t->k==TY_FUNC) && (!strcmp(an,"__name__")||!strcmp(an,"__qualname__"));
        if(!has && t->k==TY_OBJ && aot_is_exception(t->cls) && (!strcmp(an,"args"))) has=1;
        if(name[0]=='h'){ e->kind=has?EXPR_TRUE:EXPR_FALSE; e->count=0; xi->kind=X_NONE; return TY_BOOL_T; }
        if(!has && e->count==3){ Expr *d=e->items[2]; *e=*d; return ck_expr(c,e); }          /* not there: the default */
        if(!has) err(c,line,"AttributeError: '%s' object has no attribute '%s'",ty_name(t),an);
        Expr *a=xnew(EXPR_ATTRIBUTE,line); a->a=o; a->name=(char*)an;
        *e=*a; xi->kind=X_NONE; return ck_expr(c,e); }
    if(!strcmp(name,"type")){
        if(e->count==3) err(c,line,"type(name, bases, dict) creates classes while the program runs: not in compiled code");
        ck_positional(c,e,1,1,name); if(is_none(e->items[0])) return TY_TYPE_T;      /* type(None) */
        Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) return pending(c,line,"the argument of type()");
        return TY_TYPE_T; }
    if(!strcmp(name,"input")){ ck_positional(c,e,0,1,name); if(e->count) arg(c,e,0); return TY_STR_T; }
    if(!strcmp(name,"round")){ ck_positional(c,e,1,2,name); Ty *t=arg(c,e,0); if(!numeric(t)&&!is_var(t)) err(c,line,"round() of %s",ty_name(t)); if(e->count==2){ expect(c,TY_INT_T,arg(c,e,1),line,"round() digits"); return TY_FLOAT_T; } return TY_INT_T; }
    if(!strcmp(name,"list")){
        ck_positional(c,e,0,1,name);
        if(!e->count) return ty_new(TY_LIST,ty_var(),NULL);
        Ty *el; Ty *ts[1];
        Expr *it=e->items[0];
        if(is_builtin_call(c,it,"range")){ iter_types(c,it,1,ts,line); el=ts[0]; }
        else el=elem_of(c,arg(c,e,0),line,"the argument of list()");
        return ty_new(TY_LIST,el,NULL); }
    if(!strcmp(name,"set")){ ck_positional(c,e,0,1,name); if(!e->count) return ty_new(TY_SET,ty_var(),NULL); return ty_new(TY_SET,elem_of(c,arg(c,e,0),line,"the argument of set()"),NULL); }
    if(!strcmp(name,"dict")){ ck_positional(c,e,0,1,name);
        if(!e->count) return ty_dict(ty_var(),ty_var());
        genexp_as_list(e,0);
        Ty *t=ty_find(arg(c,e,0));
        if(t->k==TY_VAR) return pending(c,line,"the argument of dict()");
        if(t->k==TY_DICT){ xi->name="dict_copy"; return t; }
        Expr *src=e->items[0];                          /* dict(pairs) -> {k: v for k, v in pairs} */
        char **vars=MPY_NEW_ARR(char*,2); vars[0]="__d0"; vars[1]="__d1";
        make_comprehension(e,'D',name_expr("__d0",line),name_expr("__d1",line),vars,2,src);
        return ck_comprehension(c,e); }
    if(!strcmp(name,"tuple")){                          /* tuple(xs): a tuple[T, ...] */
        ck_positional(c,e,0,1,name); genexp_as_list(e,0);
        Ty *el=ty_var();
        if(e->count){ Ty *ts[1]; Expr *it=e->items[0];
            if(is_builtin_call(c,it,"range")){ iter_types(c,it,1,ts,line); el=ts[0]; } else el=elem_of(c,arg(c,e,0),line,"the argument of tuple()"); }
        xi->name="list"; Ty *r=ty_new(TY_LIST,el,NULL); r->tup=1; return r; }
    if(!strcmp(name,"print")) err(c,line,"print() is a statement here");
    if(!strcmp(name,"enumerate")||!strcmp(name,"zip")) return ck_pairs(c,e,name);
    if(!strcmp(name,"range")){                          /* range(...) as a value (zip(xs, range(n)), len(range(...))): a list */
        ck_positional(c,e,1,3,name); for(int i=0;i<e->count;i++) expect(c,TY_INT_T,arg(c,e,i),line,"a range() argument");
        xi->name="range_list"; return ty_new(TY_LIST,TY_INT_T,NULL); }
    if(!strcmp(name,"hash")){ ck_positional(c,e,1,1,name); Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) return pending(c,line,"the argument of hash()");
        if(t->k==TY_LIST||t->k==TY_DICT||t->k==TY_SET) err(c,line,"unhashable type: '%s'",t->k==TY_LIST?"list":t->k==TY_DICT?"dict":"set");
        return TY_INT_T; }
    if(!strcmp(name,"super")) err(c,line,"super() can only be used as super().method(...)");
    err(c,line,"name '%s' is not defined",name);
}

/* name=value arguments of a built-in method -> their places among the
   positional ones (skipped ones before them become None: the default) */
static void kw_to_pos(Ck *c, Expr *e, const char *m, const char *const *names, int n){
    int has=0;
    for(int i=0;i<e->count;i++) if(e->items[i]->akind==3) has=1;
    if(!has) return;
    Expr **p=MPY_NEW_ARR(Expr*,n); int np=0;
    memset(p,0,sizeof(Expr*)*(size_t)n);
    for(int i=0;i<e->count;i++){ Expr *a=e->items[i];
        if(a->akind==0){ if(np>=n) err(c,e->line,"%s() takes at most %d arguments",m,n); p[np++]=a; continue; }
        if(a->akind!=3) err(c,e->line,"%s() takes no *args or **kwargs in compiled code",m);
        int k=-1; for(int j=0;j<n;j++) if(!strcmp(names[j],a->kw)) k=j;
        if(k<0) err(c,e->line,"%s() got an unexpected keyword argument '%s'",m,a->kw);
        if(p[k]) err(c,e->line,"argument for %s() given by name ('%s') and position",m,a->kw);
        a->akind=0; p[k]=a;
    }
    int last=-1;
    for(int j=0;j<n;j++) if(p[j]) last=j;
    for(int j=0;j<=last;j++) if(!p[j]){ Expr *x=MPY_NEW0(Expr); x->kind=EXPR_NONE; x->line=e->line; p[j]=x; }
    e->items=p; e->count=last+1;
}
/* start / end of find, count, startswith ...: ints (None: the default) */
static void ck_range_args(Ck *c, Expr *e, int first){
    for(int i=first;i<e->count;i++) if(!is_none(e->items[i])) expect(c,TY_INT_T,arg(c,e,i),e->line,i==first?"the start":"the end");
}
/* the built-in types a program can name as values (type objects) */
int is_builtin_type_name(const char *name){
    static const char *const names[]={"int","str","float","bool","bytes","list","dict","set","tuple","object","type","frozenset","bytearray","complex",NULL};
    for(int i=0;names[i];i++) if(!strcmp(name,names[i])) return 1;
    return 0;
}
/* bytes.upper ... -> rt_bytes_case's number; bytes.isalpha ... -> rt_bytes_is's; or -1 */
int bytes_case_mode(const char *m){
    static const char *const names[]={"upper","lower","swapcase","title","capitalize"};
    for(int i=0;i<5;i++) if(!strcmp(m,names[i])) return i;
    return -1;
}
int bytes_is_mode(const char *m){
    static const char *const names[]={"isalpha","isdigit","isalnum","isspace","islower","isupper","istitle","isascii"};
    for(int i=0;i<8;i++) if(!strcmp(m,names[i])) return i;
    return -1;
}
/* str.isalpha ... -> the runtime's number for it (rt_str_is), or -1 */
int str_is_mode(const char *m){
    static const char *const names[]={"isalpha","isdigit","isalnum","isspace","islower","isupper","isdecimal","isascii","isnumeric","istitle","isprintable","isidentifier"};
    for(int i=0;i<12;i++) if(!strcmp(m,names[i])) return i;
    return -1;
}

/* Methods of str / list / dict / set. */
static Ty *ck_tmethod(Ck *c, Expr *e, Ty *t, const char *m){
    XInfo *xi=xinfo(e); xi->kind=X_TMETHOD; xi->name=m; int line=e->line;
    #define NARGS(lo,hi) ck_positional(c,e,lo,hi,m)
    if(t->k==TY_STR){
        static const char *const n_split[]={"sep","maxsplit"}, *const n_lines[]={"keepends"}, *const n_repl[]={"old","new","count"}, *const n_tabs[]={"tabsize"};
        if(!strcmp(m,"split")||!strcmp(m,"rsplit")) kw_to_pos(c,e,m,n_split,2);
        else if(!strcmp(m,"splitlines")) kw_to_pos(c,e,m,n_lines,1);
        else if(!strcmp(m,"replace")) kw_to_pos(c,e,m,n_repl,3);
        else if(!strcmp(m,"expandtabs")) kw_to_pos(c,e,m,n_tabs,1);
        if(!strcmp(m,"upper")||!strcmp(m,"lower")||!strcmp(m,"capitalize")||!strcmp(m,"title")||!strcmp(m,"swapcase")||!strcmp(m,"casefold")){ NARGS(0,0); return TY_STR_T; }
        if(!strcmp(m,"encode")){ static const char *const n_enc[]={"encoding","errors"}; kw_to_pos(c,e,m,n_enc,2); NARGS(0,2);
            for(int i=0;i<e->count;i++) if(!is_none(e->items[i])) expect(c,TY_STR_T,arg(c,e,i),line,i?"errors":"the encoding"); return TY_BYTES_T; }
        if(!strcmp(m,"strip")||!strcmp(m,"lstrip")||!strcmp(m,"rstrip")){ NARGS(0,1); if(e->count && !is_none(e->items[0])) expect(c,TY_STR_T,arg(c,e,0),line,"the characters"); return TY_STR_T; }
        if(!strcmp(m,"startswith")||!strcmp(m,"endswith")){ NARGS(1,3);
            Ty *a=ty_find(arg(c,e,0));
            if(a->k==TY_TUPLE){ for(int i=0;i<a->nelems;i++) if(!unify(c,a->elems[i],TY_STR_T)) err(c,line,"%s() takes a str or a tuple of str, not %s",m,ty_name(a)); }
            else expect(c,TY_STR_T,a,line,"the argument");
            ck_range_args(c,e,1); return TY_BOOL_T; }
        if(!strcmp(m,"find")||!strcmp(m,"rfind")||!strcmp(m,"index")||!strcmp(m,"rindex")||!strcmp(m,"count")){ NARGS(1,3); expect(c,TY_STR_T,arg(c,e,0),line,"the argument"); ck_range_args(c,e,1); return TY_INT_T; }
        if(!strcmp(m,"replace")){ NARGS(2,3); expect(c,TY_STR_T,arg(c,e,0),line,"the argument"); expect(c,TY_STR_T,arg(c,e,1),line,"the argument"); if(e->count==3) expect(c,TY_INT_T,arg(c,e,2),line,"the count"); return TY_STR_T; }
        if(!strcmp(m,"split")||!strcmp(m,"rsplit")){ NARGS(0,2); if(e->count && !is_none(e->items[0])) expect(c,TY_STR_T,arg(c,e,0),line,"the separator"); if(e->count==2) expect(c,TY_INT_T,arg(c,e,1),line,"maxsplit"); return ty_new(TY_LIST,TY_STR_T,NULL); }
        if(!strcmp(m,"join")){ NARGS(1,1); genexp_as_list(e,0); gen_as_list(c,e,0); Ty *a=ty_find(arg(c,e,0)); if(a->k==TY_VAR) return pending(c,line,"the argument of join()"); if(a->k==TY_STR) return TY_STR_T; if((a->k!=TY_LIST&&a->k!=TY_SET)||!unify(c,a->elem,TY_STR_T)) err(c,line,"join() needs a list of str, not %s",ty_name(a)); return TY_STR_T; }
        if(!strcmp(m,"format")){
            Expr *recv=e->a->a;
            if(recv->kind!=EXPR_LITERAL||recv->tok->kind!=T_STRING) err(c,line,"str.format() needs a literal format string in compiled code");
            for(int i=0;i<e->count;i++){ if(e->items[i]->akind==1||e->items[i]->akind==2) err(c,line,"format(*args) is not supported"); no_void(c,ck_expr(c,e->items[i]),line); }
            return TY_STR_T; }
        if(!strcmp(m,"ljust")||!strcmp(m,"rjust")||!strcmp(m,"center")){ NARGS(1,2); expect(c,TY_INT_T,arg(c,e,0),line,"the width"); if(e->count==2) expect(c,TY_STR_T,arg(c,e,1),line,"the fill character"); return TY_STR_T; }
        if(!strcmp(m,"zfill")){ NARGS(1,1); expect(c,TY_INT_T,arg(c,e,0),line,"the width"); return TY_STR_T; }
        if(!strcmp(m,"splitlines")){ NARGS(0,1); if(e->count){ Ty *k=ty_find(arg(c,e,0)); if(k->k!=TY_BOOL&&k->k!=TY_INT) err(c,line,"splitlines() takes a bool, not %s",ty_name(k)); } return ty_new(TY_LIST,TY_STR_T,NULL); }
        if(!strcmp(m,"partition")||!strcmp(m,"rpartition")){ NARGS(1,1); expect(c,TY_STR_T,arg(c,e,0),line,"the separator"); Ty *three[3]={TY_STR_T,TY_STR_T,TY_STR_T}; return ty_tuple(three,3); }
        if(!strcmp(m,"removeprefix")||!strcmp(m,"removesuffix")){ NARGS(1,1); expect(c,TY_STR_T,arg(c,e,0),line,"the argument"); return TY_STR_T; }
        if(!strcmp(m,"expandtabs")){ NARGS(0,1); if(e->count) expect(c,TY_INT_T,arg(c,e,0),line,"the tab size"); return TY_STR_T; }
        if(str_is_mode(m)>=0){ NARGS(0,0); return TY_BOOL_T; }
    } else if(t->k==TY_LIST){
        Ty *el=t->elem;
        if(!strcmp(m,"append")){ NARGS(1,1); expect(c,el,arg(c,e,0),line,"a list element"); return TY_VOID_T; }
        if(!strcmp(m,"pop")){ NARGS(0,1); if(e->count) expect(c,TY_INT_T,arg(c,e,0),line,"pop() index"); return el; }
        if(!strcmp(m,"insert")){ NARGS(2,2); expect(c,TY_INT_T,arg(c,e,0),line,"insert() index"); expect(c,el,arg(c,e,1),line,"a list element"); return TY_VOID_T; }
        if(!strcmp(m,"remove")){ NARGS(1,1); expect(c,el,arg(c,e,0),line,"a list element"); return TY_VOID_T; }
        if(!strcmp(m,"index")||!strcmp(m,"count")){ NARGS(1,1); expect(c,el,arg(c,e,0),line,"a list element"); return TY_INT_T; }
        if(!strcmp(m,"reverse")||!strcmp(m,"clear")){ NARGS(0,0); return TY_VOID_T; }
        if(!strcmp(m,"sort")){ take_sort_kwargs(c,e,1); NARGS(0,0);
            if(xi->rev) expect(c,TY_BOOL_T,ck_expr(c,xi->rev),line,"reverse=");
            if(xi->key){ ck_sort_key(c,xi->key,el,line); return TY_VOID_T; }
            Ty *f=ty_find(el); if(f->k==TY_VAR) pending(c,line,"the elements"); else if(!orderable(f)) err(c,line,"sort() needs values that can be ordered, not %s",ty_name(f)); return TY_VOID_T; }
        if(!strcmp(m,"copy")){ NARGS(0,0); return t; }
        if(!strcmp(m,"extend")){ NARGS(1,1); genexp_as_list(e,0); gen_as_list(c,e,0); Ty *a=ty_find(arg(c,e,0)); if(a->k==TY_VAR) pending(c,line,"the argument"); else if(a->k!=TY_LIST||!unify(c,a->elem,el)) err(c,line,"extend() needs a %s",ty_name(t)); return TY_VOID_T; }
    } else if(t->k==TY_DICT){
        Ty *v=t->elem, *k=ty_dkey(t);
        if(!strcmp(m,"get")){ NARGS(1,2); expect(c,k,arg(c,e,0),line,"a dictionary key"); if(e->count==2) expect(c,v,arg(c,e,1),line,"the default");
            else { Ty *nv=ty_var(); nv->opt=1; unify(c,v,nv); }       /* a missing key: None */
            return v; }
        if(!strcmp(m,"pop")){ NARGS(1,2); expect(c,k,arg(c,e,0),line,"a dictionary key"); if(e->count==2) expect(c,v,arg(c,e,1),line,"the default"); return v; }
        if(!strcmp(m,"setdefault")){ NARGS(2,2); expect(c,k,arg(c,e,0),line,"a dictionary key"); expect(c,v,arg(c,e,1),line,"the default"); return v; }
        if(!strcmp(m,"keys")){ NARGS(0,0); return ty_new(TY_LIST,k,NULL); }
        if(!strcmp(m,"values")){ NARGS(0,0); return ty_new(TY_LIST,v,NULL); }
        if(!strcmp(m,"clear")){ NARGS(0,0); return TY_VOID_T; }
        if(!strcmp(m,"copy")){ NARGS(0,0); return t; }
        if(!strcmp(m,"update")){ NARGS(1,1); expect(c,t,arg(c,e,0),line,"the argument"); return TY_VOID_T; }
        if(!strcmp(m,"items")){ NARGS(0,0); Ty *kv[2]={k,v}; return ty_new(TY_LIST,ty_tuple(kv,2),NULL); }
    } else if(t->k==TY_FILE){
        Ty *el=t->elem?t->elem:TY_STR_T;                       /* str, or bytes ("rb") */
        if(!strcmp(m,"read")||!strcmp(m,"readline")){ NARGS(0,0); return el; }
        if(!strcmp(m,"readlines")){ NARGS(0,0); return ty_new(TY_LIST,el,NULL); }
        if(!strcmp(m,"write")){ NARGS(1,1); expect(c,el,arg(c,e,0),line,"the text"); return TY_INT_T; }
        if(!strcmp(m,"close")){ NARGS(0,0); return TY_VOID_T; }
    } else if(t->k==TY_GEN){                                    /* g.send(v), g.close(), g.throw(exc) */
        if(!strcmp(m,"send")){ NARGS(1,1); Ty *st=aot_gen_part(t,0);
            if(!is_none(e->items[0])){ Ty *a=ck_expr_want(c,e->items[0],st); no_void(c,a,line); expect(c,t->elems[0],a,line,"the value sent"); }
            stop_value(c,t,line); return t->elem; }
        if(!strcmp(m,"__anext__")){ NARGS(0,0); if(!t->tup) err(c,line,"'generator' object has no attribute '__anext__'"); stop_value(c,t,line); return t->elem; }
        if(!strcmp(m,"asend")){ NARGS(1,1); if(!t->tup) err(c,line,"'generator' object has no attribute 'asend'");
            if(!is_none(e->items[0])){ Ty *a=ck_expr_want(c,e->items[0],aot_gen_part(t,0)); no_void(c,a,line); expect(c,t->elems[0],a,line,"the value sent"); }
            return t->elem; }
        if(!strcmp(m,"aclose")){ NARGS(0,0); if(!t->tup) err(c,line,"'generator' object has no attribute 'aclose'"); return TY_VOID_T; }
        if(!strcmp(m,"athrow")){ NARGS(1,1); if(!t->tup) err(c,line,"'generator' object has no attribute 'athrow'"); Ty *a=ty_find(arg(c,e,0));
            if(a->k==TY_VAR) return pending(c,line,"the exception");
            if(a->k!=TY_OBJ || !aot_is_exception(a->cls)) err(c,line,"athrow() takes an exception, not %s",ty_name(a));
            return t->elem; }
        if(t->tup) err(c,line,"'async_generator' object has no attribute '%s'",m);
        if(!strcmp(m,"close")){ NARGS(0,0); return TY_VOID_T; }
        if(!strcmp(m,"throw")){ NARGS(1,1); Ty *a=ty_find(arg(c,e,0));
            if(a->k==TY_VAR) return pending(c,line,"the exception");
            if(a->k!=TY_OBJ || !aot_is_exception(a->cls)) err(c,line,"throw() takes an exception, not %s",ty_name(a));
            stop_value(c,t,line); return t->elem; }
    } else if(t->k==TY_TASK){
        if(!strcmp(m,"done")){ NARGS(0,0); return TY_BOOL_T; }
        if(!strcmp(m,"result")){ NARGS(0,0); return t->elem; }
    } else if(t->k==TY_BYTES){
        static const char *const n_split[]={"sep","maxsplit"}, *const n_lines[]={"keepends"}, *const n_repl[]={"old","new","count"}, *const n_dec[]={"encoding","errors"};
        if(!strcmp(m,"split")||!strcmp(m,"rsplit")) kw_to_pos(c,e,m,n_split,2);
        else if(!strcmp(m,"splitlines")) kw_to_pos(c,e,m,n_lines,1);
        else if(!strcmp(m,"replace")) kw_to_pos(c,e,m,n_repl,3);
        else if(!strcmp(m,"decode")) kw_to_pos(c,e,m,n_dec,2);
        if(!strcmp(m,"decode")){ NARGS(0,2); for(int i=0;i<e->count;i++) if(!is_none(e->items[i])) expect(c,TY_STR_T,arg(c,e,i),line,i?"errors":"the encoding"); return TY_STR_T; }
        if(!strcmp(m,"hex")){ NARGS(0,0); return TY_STR_T; }
        if(bytes_case_mode(m)>=0){ NARGS(0,0); return TY_BYTES_T; }
        if(bytes_is_mode(m)>=0){ NARGS(0,0); return TY_BOOL_T; }
        if(!strcmp(m,"strip")||!strcmp(m,"lstrip")||!strcmp(m,"rstrip")){ NARGS(0,1); if(e->count && !is_none(e->items[0])) expect(c,TY_BYTES_T,arg(c,e,0),line,"the bytes to strip"); return TY_BYTES_T; }
        if(!strcmp(m,"startswith")||!strcmp(m,"endswith")){ NARGS(1,3);
            Ty *a=ty_find(arg(c,e,0));
            if(a->k==TY_TUPLE){ for(int i=0;i<a->nelems;i++) if(!unify(c,a->elems[i],TY_BYTES_T)) err(c,line,"%s first arg must be bytes or a tuple of bytes, not %s",m,ty_name(a)); }
            else expect(c,TY_BYTES_T,a,line,"the argument");
            ck_range_args(c,e,1); return TY_BOOL_T; }
        if(!strcmp(m,"find")||!strcmp(m,"rfind")||!strcmp(m,"index")||!strcmp(m,"rindex")||!strcmp(m,"count")){ NARGS(1,3);
            Ty *a=ty_find(arg(c,e,0)); if(a->k!=TY_BYTES&&a->k!=TY_INT&&a->k!=TY_BOOL) expect(c,TY_BYTES_T,a,line,"the argument");
            ck_range_args(c,e,1); return TY_INT_T; }
        if(!strcmp(m,"replace")){ NARGS(2,3); expect(c,TY_BYTES_T,arg(c,e,0),line,"the argument"); expect(c,TY_BYTES_T,arg(c,e,1),line,"the argument"); if(e->count==3) expect(c,TY_INT_T,arg(c,e,2),line,"the count"); return TY_BYTES_T; }
        if(!strcmp(m,"split")||!strcmp(m,"rsplit")){ NARGS(0,2); if(e->count && !is_none(e->items[0])) expect(c,TY_BYTES_T,arg(c,e,0),line,"the separator"); if(e->count==2) expect(c,TY_INT_T,arg(c,e,1),line,"maxsplit"); return ty_new(TY_LIST,TY_BYTES_T,NULL); }
        if(!strcmp(m,"join")){ NARGS(1,1); genexp_as_list(e,0); gen_as_list(c,e,0); Ty *a=ty_find(arg(c,e,0)); if(a->k==TY_VAR) return pending(c,line,"the argument of join()");
            if((a->k!=TY_LIST&&a->k!=TY_SET)||!unify(c,a->elem,TY_BYTES_T)) err(c,line,"join() needs a list of bytes, not %s",ty_name(a)); return TY_BYTES_T; }
        if(!strcmp(m,"partition")||!strcmp(m,"rpartition")){ NARGS(1,1); expect(c,TY_BYTES_T,arg(c,e,0),line,"the separator"); Ty *three[3]={TY_BYTES_T,TY_BYTES_T,TY_BYTES_T}; return ty_tuple(three,3); }
        if(!strcmp(m,"removeprefix")||!strcmp(m,"removesuffix")){ NARGS(1,1); expect(c,TY_BYTES_T,arg(c,e,0),line,"the argument"); return TY_BYTES_T; }
        if(!strcmp(m,"splitlines")){ NARGS(0,1); if(e->count){ Ty *k=ty_find(arg(c,e,0)); if(k->k!=TY_BOOL&&k->k!=TY_INT) err(c,line,"splitlines() takes a bool, not %s",ty_name(k)); } return ty_new(TY_LIST,TY_BYTES_T,NULL); }
    } else if(t->k==TY_INT||t->k==TY_BOOL){
        if(!strcmp(m,"to_bytes")){ static const char *const n_tb[]={"length","byteorder","signed"}; kw_to_pos(c,e,m,n_tb,3); NARGS(0,3);
            if(e->count>0 && !is_none(e->items[0])) expect(c,TY_INT_T,arg(c,e,0),line,"length");
            if(e->count>1 && !is_none(e->items[1])) expect(c,TY_STR_T,arg(c,e,1),line,"byteorder");
            if(e->count>2) expect(c,TY_BOOL_T,arg(c,e,2),line,"signed");
            return TY_BYTES_T; }
        if(!strcmp(m,"bit_length")||!strcmp(m,"bit_count")){ NARGS(0,0); return TY_INT_T; }
    } else if(t->k==TY_FLOAT){
        if(!strcmp(m,"is_integer")){ NARGS(0,0); return TY_BOOL_T; }
    } else if(t->k==TY_SET){
        Ty *el=t->elem;
        if(!strcmp(m,"add")||!strcmp(m,"remove")||!strcmp(m,"discard")){ NARGS(1,1); expect(c,el,arg(c,e,0),line,"a set element"); return TY_VOID_T; }
        if(!strcmp(m,"clear")){ NARGS(0,0); return TY_VOID_T; }
        if(!strcmp(m,"copy")){ NARGS(0,0); return t; }
        if(!strcmp(m,"pop")){ NARGS(0,0); return el; }
        if(!strcmp(m,"union")||!strcmp(m,"intersection")||!strcmp(m,"difference")||!strcmp(m,"symmetric_difference")){ NARGS(1,1); expect(c,t,arg(c,e,0),line,"the argument"); return t; }
        if(!strcmp(m,"update")||!strcmp(m,"intersection_update")||!strcmp(m,"difference_update")||!strcmp(m,"symmetric_difference_update")){ NARGS(1,1); expect(c,t,arg(c,e,0),line,"the argument"); return TY_VOID_T; }
        if(!strcmp(m,"issubset")||!strcmp(m,"issuperset")||!strcmp(m,"isdisjoint")){ NARGS(1,1); expect(c,t,arg(c,e,0),line,"the argument"); return TY_BOOL_T; }
    }
    #undef NARGS
    err(c,line,"%s has no method '%s'",ty_name(t),m);
}

static Ty *ck_sys(Ck *c, Expr *e, const char *m){
    XInfo *xi=xinfo(e); xi->kind=X_SYS; xi->name=m; int line=e->line;
    if(!strcmp(m,"syscall")){
        xi->kind=X_SYSCALL;
        ck_positional(c,e,1,7,"syscall");
        for(int i=0;i<e->count;i++){ Ty *t=ty_find(arg(c,e,i));
            if(t->k==TY_VAR){ pending(c,line,"a syscall argument"); continue; }
            if(t->k!=TY_INT&&t->k!=TY_BOOL&&t->k!=TY_STR&&t->k!=TY_BYTES&&t->k!=TY_BUF) err(c,line,"syscall() arguments are int, str, bytes or buffer, not %s",ty_name(t)); }
        return ty_new(TY_LIST,TY_INT_T,NULL);
    }
    if(!strcmp(m,"buffer")){ ck_positional(c,e,1,1,m); Ty *t=ty_find(arg(c,e,0)); if(t->k!=TY_INT&&t->k!=TY_STR&&t->k!=TY_BYTES&&t->k!=TY_VAR) err(c,line,"buffer() takes a size, a str or bytes"); return TY_BUF_T; }
    if(!strcmp(m,"poke")){ ck_positional(c,e,4,4,m); expect(c,TY_BUF_T,arg(c,e,0),line,"the buffer"); for(int i=1;i<4;i++) expect(c,TY_INT_T,arg(c,e,i),line,"a poke() argument"); return TY_VOID_T; }
    if(!strcmp(m,"peek")){ ck_positional(c,e,3,3,m); expect(c,TY_BUF_T,arg(c,e,0),line,"the buffer"); for(int i=1;i<3;i++) expect(c,TY_INT_T,arg(c,e,i),line,"a peek() argument"); return TY_INT_T; }
    if(!strcmp(m,"poke_str")){ ck_positional(c,e,3,3,m); expect(c,TY_BUF_T,arg(c,e,0),line,"the buffer"); expect(c,TY_INT_T,arg(c,e,1),line,"the offset"); expect(c,TY_STR_T,arg(c,e,2),line,"the string"); return TY_VOID_T; }
    if(!strcmp(m,"peek_str")){ ck_positional(c,e,3,3,m); expect(c,TY_BUF_T,arg(c,e,0),line,"the buffer"); expect(c,TY_INT_T,arg(c,e,1),line,"the offset"); expect(c,TY_INT_T,arg(c,e,2),line,"the length"); return TY_STR_T; }
    if(!strcmp(m,"addr")){ ck_positional(c,e,1,1,m); expect(c,TY_BUF_T,arg(c,e,0),line,"the buffer"); return TY_INT_T; }
    if(!strcmp(m,"exit")){ ck_positional(c,e,0,1,m); if(e->count) expect(c,TY_INT_T,arg(c,e,0),line,"the exit code"); return TY_VOID_T; }
    /* raw memory at an address a system call gave (nothing is checked) */
    if(!strcmp(m,"peek_at")){ ck_positional(c,e,2,2,m); expect(c,TY_INT_T,arg(c,e,0),line,"the address"); expect(c,TY_INT_T,arg(c,e,1),line,"the size"); return TY_INT_T; }
    if(!strcmp(m,"poke_at")){ ck_positional(c,e,3,3,m); for(int i=0;i<3;i++) expect(c,TY_INT_T,arg(c,e,i),line,"a poke_at() argument"); return TY_VOID_T; }
    if(!strcmp(m,"peek_str_at")||!strcmp(m,"cstr_at")){ ck_positional(c,e,2,2,m); expect(c,TY_INT_T,arg(c,e,0),line,"the address"); expect(c,TY_INT_T,arg(c,e,1),line,"the length"); return TY_STR_T; }
    if(!strcmp(m,"poke_str_at")){ ck_positional(c,e,2,2,m); expect(c,TY_INT_T,arg(c,e,0),line,"the address"); expect(c,TY_STR_T,arg(c,e,1),line,"the string"); return TY_VOID_T; }
    err(c,line,"sys.%s is not available in compiled code",m);
}

static Ty *ck_asyncio(Ck *c, Expr *e, const char *m);
static Ty *ck_bmod(Ck *c, Expr *e, const char *mod, const char *m);
static Ty *ck_call_inner(Ck *c, Expr *e);
/* Calls of async functions are only valid where they are awaited or given to asyncio. */
static Ty *ck_asyncio(Ck *c, Expr *e, const char *m){
    XInfo *xi=xinfo(e); xi->kind=X_ASYNC; xi->name=m; int line=e->line;
    for(int i=0;i<e->count;i++) if(e->items[i]->akind) err(c,line,"asyncio.%s() takes positional arguments",m);
    if(!strcmp(m,"run")){
        if(e->count!=1) err(c,line,"asyncio.run() takes one coroutine");
        if(c->fn && c->fn->is_async) err(c,line,"asyncio.run() cannot be called from a coroutine (await instead)");
        return ck_coro(c,e->items[0]);
    }
    if(!strcmp(m,"create_task")||!strcmp(m,"ensure_future")){
        if(e->count!=1) err(c,line,"asyncio.%s() takes one coroutine",m);
        if(!c->fn || !c->fn->is_async) err(c,line,"asyncio.%s() needs a running event loop: call it from a coroutine",m);
        xi->name="create_task";
        return ty_new(TY_TASK,ck_coro(c,e->items[0]),NULL);
    }
    if(!strcmp(m,"sleep")||!strcmp(m,"gather")) err(c,line,"asyncio.%s() must be awaited",m);
    err(c,line,"asyncio.%s is not available in compiled code",m);
}
/* math / time / random */
static Ty *ck_bmod(Ck *c, Expr *e, const char *mod, const char *m){
    XInfo *xi=xinfo(e); xi->kind=X_BMOD; xi->name=m; int line=e->line;
    if(!strcmp(mod,"functools") && !strcmp(m,"reduce")) return ck_reduce(c,e);
    char what[64]; snprintf(what,sizeof what,"%s.%s",mod,m);
    #define FARG(i) expect(c,TY_FLOAT_T,arg(c,e,i),line,"an argument of " what_)
    #define what_ ""
    if(!strcmp(mod,"math")){
        static const char *f1[]={"sqrt","sin","cos","tan","asin","acos","atan","exp","log10","log2","fabs","degrees","radians",NULL};
        for(int i=0;f1[i];i++) if(!strcmp(m,f1[i])){ ck_positional(c,e,1,1,what); FARG(0); return TY_FLOAT_T; }
        if(!strcmp(m,"log")){ ck_positional(c,e,1,2,what); FARG(0); if(e->count==2) FARG(1); return TY_FLOAT_T; }
        if(!strcmp(m,"atan2")||!strcmp(m,"pow")||!strcmp(m,"hypot")||!strcmp(m,"fmod")||!strcmp(m,"copysign")){ ck_positional(c,e,2,2,what); FARG(0); FARG(1); return TY_FLOAT_T; }
        if(!strcmp(m,"isnan")||!strcmp(m,"isinf")||!strcmp(m,"isfinite")){ ck_positional(c,e,1,1,what); FARG(0); return TY_BOOL_T; }
        if(!strcmp(m,"floor")||!strcmp(m,"ceil")||!strcmp(m,"trunc")){ ck_positional(c,e,1,1,what); FARG(0); return TY_INT_T; }
        if(!strcmp(m,"gcd")){ ck_positional(c,e,2,2,what); expect(c,TY_INT_T,arg(c,e,0),line,"an argument of gcd"); expect(c,TY_INT_T,arg(c,e,1),line,"an argument of gcd"); return TY_INT_T; }
        if(!strcmp(m,"isqrt")){ ck_positional(c,e,1,1,what); expect(c,TY_INT_T,arg(c,e,0),line,"the argument of isqrt"); return TY_INT_T; }
    }
    if(!strcmp(mod,"time")){
        if(!strcmp(m,"time")||!strcmp(m,"monotonic")||!strcmp(m,"perf_counter")){ ck_positional(c,e,0,0,what); return TY_FLOAT_T; }
        if(!strcmp(m,"sleep")){ ck_positional(c,e,1,1,what); FARG(0); return TY_VOID_T; }
    }
    if(!strcmp(mod,"random")){
        if(!strcmp(m,"random")){ ck_positional(c,e,0,0,what); return TY_FLOAT_T; }
        if(!strcmp(m,"seed")){ ck_positional(c,e,1,1,what); expect(c,TY_INT_T,arg(c,e,0),line,"the seed"); return TY_VOID_T; }
        if(!strcmp(m,"randint")){ ck_positional(c,e,2,2,what); expect(c,TY_INT_T,arg(c,e,0),line,"a bound"); expect(c,TY_INT_T,arg(c,e,1),line,"a bound"); return TY_INT_T; }
        if(!strcmp(m,"randrange")){ ck_positional(c,e,1,2,what); for(int i=0;i<e->count;i++) expect(c,TY_INT_T,arg(c,e,i),line,"a bound"); return TY_INT_T; }
        if(!strcmp(m,"uniform")){ ck_positional(c,e,2,2,what); FARG(0); FARG(1); return TY_FLOAT_T; }
        if(!strcmp(m,"choice")){ ck_positional(c,e,1,1,what); Ty *t=ty_find(arg(c,e,0));
            if(t->k==TY_VAR) return pending(c,line,"the argument of choice()");
            if(t->k==TY_STR) return TY_STR_T;
            if(t->k!=TY_LIST) err(c,line,"random.choice() needs a list or a str");
            return t->elem; }
        if(!strcmp(m,"shuffle")){ ck_positional(c,e,1,1,what); Ty *t=ty_find(arg(c,e,0));
            if(t->k!=TY_LIST && t->k!=TY_VAR) err(c,line,"random.shuffle() needs a list"); return TY_VOID_T; }
    }
    #undef FARG
    #undef what_
    err(c,line,"%s.%s is not available in compiled code",mod,m);
}
/* lib.f(args) / f(args): a cdecl call. Without argtypes each argument goes by
   its type: int/bool c_int, float c_double, str and buffers char *, None NULL. */
static Ty *ck_ccall(Ck *c, Expr *e, ACFunc *cf){
    XInfo *xi=xinfo(e); xi->kind=X_CCALL; xi->cfn=cf; int line=e->line;
    if(c->p->target==AOT_TARGET_KOLIBRI) err(c,line,"C functions (ctypes) need the linux or macos target");
    if(e->count>16) err(c,line,"%s(): at most 16 arguments",cf->sym);
    if(cf->nargtypes>=0 && e->count<cf->nargtypes) err(c,line,"%s() takes %d arguments (its argtypes), not %d",cf->sym,cf->nargtypes,e->count);
    for(int i=0;i<e->count;i++){ Expr *a=e->items[i];
        if(a->akind) err(c,line,"%s() takes positional arguments",cf->sym);
        CType k=i<cf->nargtypes?cf->argtypes[i]:CT_DEFAULT;
        Ty *t=ty_find(arg(c,e,i));
        if(a->kind==EXPR_NONE){
            if(k!=CT_DEFAULT && k!=CT_CHARP && k!=CT_VOIDP) err(c,line,"argument %d of %s(): None is a NULL pointer, the argtype is not a pointer",i+1,cf->sym);
            unify(c,t,TY_BUF_T); xi->argmap[i]=CT_VOIDP; continue;
        }
        if(t->k==TY_VAR){ pending(c,line,"an argument of a C function"); continue; }
        int isint=t->k==TY_INT||t->k==TY_BOOL, isptr=t->k==TY_STR||t->k==TY_BYTES||t->k==TY_BUF;
        if(k==CT_DEFAULT){
            if(isint) k=CT_INT; else if(t->k==TY_FLOAT) k=CT_DOUBLE; else if(t->k==TY_STR||t->k==TY_BYTES) k=CT_CHARP; else if(t->k==TY_BUF) k=CT_VOIDP;
            else err(c,line,"argument %d of %s() is %s: C functions take int, float, str, buffers or None",i+1,cf->sym,ty_name(t));
        } else if(k==CT_DOUBLE || k==CT_FLOAT){
            if(!numeric(t)) err(c,line,"argument %d of %s() must be a number (its argtype is a float), not %s",i+1,cf->sym,ty_name(t));
        } else if(k==CT_CHARP){
            if(!isptr && !isint) err(c,line,"argument %d of %s() must be a str or a buffer (c_char_p), not %s",i+1,cf->sym,ty_name(t));
        } else if(k==CT_VOIDP){
            if(!isptr && !isint) err(c,line,"argument %d of %s() must be an address, a str or a buffer (c_void_p), not %s",i+1,cf->sym,ty_name(t));
        } else if(!isint) err(c,line,"argument %d of %s() must be an int, not %s",i+1,cf->sym,ty_name(t));
        xi->argmap[i]=k;
    }
    cf->used=1; cf->lib->used=1;
    switch(cf->restype){
        case CT_DOUBLE: case CT_FLOAT: return TY_FLOAT_T;
        case CT_CHARP: return optional(TY_STR_T);          /* NULL is None */
        case CT_VOID: return TY_VOID_T;
        case CT_BOOL: return TY_BOOL_T;
        default: return TY_INT_T;
    }
}
/* ctypes.create_string_buffer / string_at / addressof / get_errno */
static Ty *ck_ctypes_fn(Ck *c, Expr *e, const char *m){
    static const char *names[]={"ctypes.create_string_buffer","ctypes.string_at","ctypes.addressof","ctypes.get_errno",NULL};
    XInfo *xi=xinfo(e); xi->kind=X_BMOD; xi->name=m; int line=e->line;
    for(int i=0;names[i];i++) if(!strcmp(names[i]+7,m)) xi->name=names[i];
    if(c->p->target==AOT_TARGET_KOLIBRI) err(c,line,"ctypes needs the linux or macos target");
    if(!strcmp(m,"create_string_buffer")){ ck_positional(c,e,1,1,"create_string_buffer"); Ty *t=ty_find(arg(c,e,0));
        if(t->k!=TY_INT&&t->k!=TY_STR&&t->k!=TY_VAR) err(c,line,"create_string_buffer() takes a size or a str");
        return TY_BUF_T; }
    if(!strcmp(m,"string_at")){ ck_positional(c,e,1,2,"string_at"); expect(c,TY_INT_T,arg(c,e,0),line,"the address");
        if(e->count==2) expect(c,TY_INT_T,arg(c,e,1),line,"the size"); return TY_STR_T; }
    if(!strcmp(m,"addressof")){ ck_positional(c,e,1,1,"addressof"); expect(c,TY_BUF_T,arg(c,e,0),line,"the buffer"); return TY_INT_T; }
    if(!strcmp(m,"get_errno")){ ck_positional(c,e,0,0,"get_errno");
        ACFunc *f=cfunc_get(c->p,clib_get(c->p,"libc.so.6"),"__errno_location"); f->restype=CT_VOIDP; f->used=1; f->lib->used=1; xi->cfn=f;
        return TY_INT_T; }
    err(c,line,"ctypes.%s is not available in compiled code",m);
}

/* What json.dumps can write: as CPython's json, plus objects (their fields,
   as a JSON object) and records. */
static void json_ok(Ck *c, Ty *t, int line, int depth){
    t=ty_find(t);
    if(depth>32) return;
    switch(t->k){
        case TY_VAR: if(!depth) pending(c,line,"the value written as JSON"); return;   /* items of an empty container: defaulted */
        case TY_INT: case TY_BOOL: case TY_FLOAT: case TY_STR: case TY_VOID: return;
        case TY_LIST: json_ok(c,t->elem,line,depth+1); return;
        case TY_TUPLE: for(int i=0;i<t->nelems;i++) json_ok(c,t->elems[i],line,depth+1); return;
        case TY_DICT:{ Ty *k=ty_find(ty_dkey(t));
            if(k->k!=TY_STR && k->k!=TY_INT && k->k!=TY_BOOL && k->k!=TY_FLOAT && k->k!=TY_VAR) err(c,line,"TypeError: keys must be str, int, float or bool, not %s (json)",ty_name(k));
            json_ok(c,t->elem,line,depth+1); return; }
        case TY_OBJ: for(AClass *k=t->cls;k;k=k->base) for(int i=0;i<k->nfields;i++) json_ok(c,k->fields[i]->ty,line,depth+1); return;
        default: err(c,line,"TypeError: Object of type %s is not JSON serializable",ty_name(t));
    }
}
/* json.dumps(obj, separators=(item, key), ensure_ascii=bool) */
static Ty *ck_json(Ck *c, Expr *e, const char *m){
    XInfo *xi=xinfo(e); xi->kind=X_BMOD; int line=e->line;
    if(strcmp(m,"dumps")) err(c,line,"json.%s is not available in compiled code (json.dumps is)",m);
    xi->name="json.dumps"; xi->key=NULL; xi->argmap[0]=1;
    int npos=0;
    for(int i=0;i<e->count;i++){ Expr *a=e->items[i];
        if(a->akind==0){ if(npos++) err(c,line,"json.dumps() takes one positional argument"); continue; }
        if(a->akind!=3) err(c,line,"json.dumps(): *args / **kwargs are not supported");
        if(!strcmp(a->kw,"separators")){
            if(a->kind!=EXPR_TUPLE || a->count!=2 || a->items[0]->kind!=EXPR_LITERAL || a->items[0]->tok->kind!=T_STRING || a->items[1]->kind!=EXPR_LITERAL || a->items[1]->tok->kind!=T_STRING)
                err(c,line,"json.dumps(): separators must be a tuple of two string literals");
            xi->key=a; ck_expr(c,a);
        } else if(!strcmp(a->kw,"ensure_ascii")){
            if(a->kind!=EXPR_TRUE && a->kind!=EXPR_FALSE) err(c,line,"json.dumps(): ensure_ascii must be True or False");
            xi->argmap[0]=a->kind==EXPR_TRUE; ck_expr(c,a);
        } else if(!strcmp(a->kw,"indent") || !strcmp(a->kw,"sort_keys")){
            if(a->kind!=EXPR_NONE && a->kind!=EXPR_FALSE) err(c,line,"json.dumps(): %s is not supported in compiled code",a->kw);
            if(a->kind==EXPR_NONE) unify(c,ck_expr(c,a),TY_VOID_T); else ck_expr(c,a);
        } else err(c,line,"json.dumps(): '%s' is not supported in compiled code",a->kw);
    }
    if(npos!=1 || e->items[0]->akind) err(c,line,"json.dumps() takes the value to write first");
    Expr *v=e->items[0];
    Ty *t=ck_expr(c,v);
    if(v->kind==EXPR_NONE) unify(c,t,TY_VOID_T);
    json_ok(c,t,line,0);
    return TY_STR_T;
}
/* minipy.endpoint(f): f as an Endpoint (a function: its adapter) */
static Ty *ck_minipy(Ck *c, Expr *e, const char *m){
    XInfo *xi=xinfo(e); xi->kind=X_BMOD; int line=e->line;
    if(strcmp(m,"endpoint")) err(c,line,"minipy.%s is not available",m);
    xi->name="minipy.endpoint";
    ck_positional(c,e,1,1,"endpoint");
    Ty *want=endpoint_type();
    c->ep_value++; Ty *t=ck_expr(c,e->items[0]); c->ep_value--;
    expect(c,want,t,line,"the argument of minipy.endpoint()");
    ep_arg(c,want,e->items[0],line);
    return want;
}
static Ty *ck_call(Ck *c, Expr *e){
    Ty *t=ck_call_inner(c,e);
    XInfo *xi=xinfo(e);
    if((xi->kind==X_FUNC||xi->kind==X_METHOD||xi->kind==X_STATIC||xi->kind==X_SUPER||xi->kind==X_CALLNEST) && xi->fn && xi->fn->is_async && !xi->fn->is_gen && c->coro_ok!=e && !(c->fn && c->fn->calls_coroutines))
        err(c,e->line,"%s() is a coroutine: await it, or pass it to asyncio.create_task/gather/run",xi->fn->name);
    return t;
}
/* x must be a call of an async function: its result type */
static Ty *ck_coro(Ck *c, Expr *x){
    if(x->kind!=EXPR_CALL) err(c,x->line,"expected a coroutine call (a call of an async function)");
    Expr *save=c->coro_ok; c->coro_ok=x;
    Ty *t=ck_expr(c,x);
    c->coro_ok=save;
    XInfo *xi=xinfo(x);
    if(xi->kind==X_CALLNEST && xi->fn && xi->fn->is_async) err(c,x->line,"a nested coroutine can only be awaited in compiled code (define %s() at module level to make a task of it)",xi->fn->name);
    if(!((xi->kind==X_FUNC||xi->kind==X_METHOD||xi->kind==X_STATIC||xi->kind==X_SUPER) && xi->fn && xi->fn->is_async))
        err(c,x->line,"expected a coroutine call (a call of an async function)");
    return t;
}
/* dataclasses.replace / asdict / astuple / is_dataclass: rewritten into the class's constructor
   and methods (the rewritten call is checked from then on) */
static Ty *ck_call_inner(Ck *c, Expr *e);
static AVar *named_hidden(Ck *c, const char *base, char *out, size_t n){       /* a hidden variable names can refer to */
    snprintf(out,n,".%s%d",base,++c->nhidden);
    if(is_func(c->fn)){ AVar *v=hidden_local(c->fn,out,NULL); symtab_add(&c->fn->locals,out,AS_VAR,v); return v; }
    AVar *v=new_global(c,c->mod,out); symtab_add(&c->mod->syms,out,AS_VAR,v); return v;
}
static AClass *dc_class_of(AClass *k){ for(;k;k=k->base) if(k->dc) return k; return NULL; }
static Ty *ck_dataclasses(Ck *c, Expr *e, const char *m){
    int line=e->line;
    if(!strcmp(m,"field")) err(c,line,"field() belongs in a dataclass's body");
    if(!strcmp(m,"fields")) err(c,line,"dataclasses.fields() is not supported in compiled code");
    if(strcmp(m,"replace") && strcmp(m,"asdict") && strcmp(m,"astuple") && strcmp(m,"is_dataclass")) err(c,line,"dataclasses.%s() is not supported in compiled code",m);
    if(!e->count || e->items[0]->akind) err(c,line,"%s() needs the object first",m);
    Expr *obj=e->items[0];
    Ty *t=ty_find(ck_expr(c,obj));
    if(t->k==TY_VAR) return pending(c,line,"the argument");
    if(!strcmp(m,"is_dataclass")){                                 /* known from the type */
        int yes= t->k==TY_OBJ ? dc_class_of(t->cls)!=NULL : t->k==TY_TYPE && xinfo(obj)->kind==X_TYPEVAL && xinfo(obj)->cls ? dc_class_of(xinfo(obj)->cls)!=NULL : 0;
        Expr *k=xnew(yes?EXPR_TRUE:EXPR_FALSE,line); void *keep=e->ty; *e=*k; e->ty=keep; return TY_BOOL_T; }
    AClass *dcc= t->k==TY_OBJ ? dc_class_of(t->cls) : NULL;
    if(!dcc) err(c,line,"%s() should be called on dataclass instances",m);
    if(!strcmp(m,"asdict")||!strcmp(m,"astuple")){                  /* obj.__mp_asdict__() */
        if(e->count!=1) err(c,line,"%s(): only the object is supported in compiled code",m);
        Expr *at=xnew(EXPR_ATTRIBUTE,line); at->a=obj; at->name=m[2]=='d'?"__mp_asdict__":"__mp_astuple__";
        e->a=at; e->count=0;
        return ck_call_inner(c,e); }
    struct DcInfo *dc=dcc->dc;                                       /* replace(obj, f=v): Class(f=v, g=obj.g, ...) */
    for(int i=1;i<e->count;i++){ Expr *a=e->items[i];
        if(a->akind!=3) err(c,line,"replace() takes the changes as keyword arguments");
        for(int k=0;k<dc->n;k++) if(!strcmp(dc->f[k].name,a->kw) && !dc->f[k].init)
            err(c,line,"field %s is declared with init=False, it cannot be specified with replace()",a->kw); }
    Expr *src=obj; int pure= obj->kind==EXPR_NAME || (obj->kind==EXPR_ATTRIBUTE && obj->a->kind==EXPR_NAME);
    char h[64]; if(!pure) named_hidden(c,"replace",h,sizeof h);
    Expr *call=xnew(EXPR_CALL,line); call->a=name_expr(t->cls->symname,line);
    int first=1;
    for(int k=0;k<dc->n;k++){ DcField *f=&dc->f[k]; if(!f->init) continue;
        Expr *v=NULL; for(int i=1;i<e->count;i++) if(!strcmp(e->items[i]->kw,f->name)) v=e->items[i];
        if(!v){
            if(f->initvar) err(c,line,"InitVar '%s' must be specified with replace()",f->name);
            Expr *o;
            if(pure) o=src;
            else if(first){ o=xnew(EXPR_WALRUS,line); o->name=xstrdup2(h); o->a=src; o->b=name_expr(xstrdup2(h),line); first=0; }
            else o=name_expr(xstrdup2(h),line);
            Expr *at=xnew(EXPR_ATTRIBUTE,line); at->a=o; at->name=(char*)f->name;
            v=at; }
        Expr *arg=MPY_NEW0(Expr); *arg=*v; arg->akind=3; arg->kw=(char*)f->name;
        xpush(call,arg); }
    for(int i=1;i<e->count;i++){ int known=0; for(int k=0;k<dc->n;k++) if(!strcmp(dc->f[k].name,e->items[i]->kw)) known=1;
        if(!known) err(c,line,"%s.__init__() got an unexpected keyword argument '%s'",t->cls->name,e->items[i]->kw); }
    { void *keep=e->ty; *e=*call; e->ty=keep; }
    return ck_call_inner(c,e);
}
/* minipy's thread module: start(fn[, args]) runs fn(*args) as a task; join, lock, acquire, release, sleep */
static Ty *ck_thread(Ck *c, Expr *e, const char *m){
    XInfo *xi=xinfo(e); int line=e->line;
    xi->kind=X_BUILTIN;
    for(int i=0;i<e->count;i++) if(e->items[i]->akind) err(c,line,"thread.%s() takes positional arguments",m);
    if(!strcmp(m,"start")){
        if(e->count<1 || e->count>2) err(c,line,"thread.start() takes a function and a tuple of its arguments");
        if(!xi->key){ Expr *call=xnew(EXPR_CALL,line); call->a=e->items[0];
            if(e->count==2){ Expr *t=e->items[1]; if(t->kind!=EXPR_TUPLE) err(c,line,"thread.start() needs its arguments as a tuple literal in compiled code");
                for(int i=0;i<t->count;i++) xpush(call,t->items[i]); }
            xi->key=call; }
        Ty *r=ck_expr(c,xi->key); XInfo *cx=xinfo(xi->key);
        if(!((cx->kind==X_FUNC||cx->kind==X_METHOD||cx->kind==X_STATIC) && cx->fn) || cx->fn->is_async || cx->fn->is_gen)
            err(c,line,"thread.start() needs a function defined with def (at module level) in compiled code");
        xi->name="thread.start"; return ty_new(TY_TASK,r,NULL); }
    if(!strcmp(m,"join")){ ck_positional(c,e,1,1,"join"); Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) return pending(c,line,"the thread");
        if(t->k!=TY_TASK) err(c,line,"thread.join() takes what thread.start() gave, not %s",ty_name(t)); xi->name="thread.join"; return TY_VOID_T; }
    if(!strcmp(m,"lock")){ ck_positional(c,e,0,0,"lock"); xi->name="thread.lock"; return TY_INT_T; }
    if(!strcmp(m,"acquire")||!strcmp(m,"release")){ ck_positional(c,e,1,1,m); expect(c,TY_INT_T,arg(c,e,0),line,"a lock"); xi->name=m[0]=='a'?"thread.acquire":"thread.release"; return TY_VOID_T; }
    if(!strcmp(m,"sleep")){ ck_positional(c,e,1,1,"sleep"); Ty *t=arg(c,e,0); if(!numeric(t)) err(c,line,"thread.sleep() takes a number of seconds"); xi->name="thread.sleep"; return TY_VOID_T; }
    err(c,line,"thread.%s is not available in compiled code",m);
}
/* string.capwords(s, sep=None): (sep or ' ').join(w.capitalize() for w in s.split(sep)) */
static Ty *ck_capwords(Ck *c, Expr *e){
    if(e->count<1 || e->count>2) err(c,e->line,"capwords() takes 1 or 2 arguments");
    for(int i=0;i<e->count;i++) if(e->items[i]->akind && !(e->items[i]->akind==3 && !strcmp(e->items[i]->kw,"sep"))) err(c,e->line,"capwords() takes s and sep");
    Expr *sep= e->count==2 && e->items[1]->kind!=EXPR_NONE ? e->items[1] : NULL;
    Expr *r=py_front_expr(c->mod->unit->path,sep?"(0).join([__mpy_w.capitalize() for __mpy_w in (0).split(0)])":"\" \".join([__mpy_w.capitalize() for __mpy_w in (0).split()])",e->line);
    if(!r) err(c,e->line,"internal error: capwords");
    Expr *it=r->items[0]->clauses[0].iter;
    it->a->a=e->items[0]; if(sep){ Expr *s2=MPY_NEW0(Expr); *s2=*sep; s2->kw=NULL; s2->akind=0; it->items[0]=s2; r->a->a=sep; sep->kw=NULL; sep->akind=0; }
    void *keep=e->ty; *e=*r; e->ty=keep;
    return ck_expr(c,e);
}
static void eg_class_arg(Ck *c, Expr *e);
/* functools.partial(f, a, k=b): lambda x: f(a, x, k=b), its fixed arguments (and f) evaluated now as defaults */
static Ty *ck_partial(Ck *c, Expr *e){
    int line=e->line;
    if(!e->count || e->items[0]->akind) err(c,line,"partial() needs the function first");
    Expr *fx=e->items[0]; Ty *ft=ty_find(ck_expr(c,fx));
    if(ft->k==TY_VAR) return pending(c,line,"the function of partial()");
    if(ft->k!=TY_FUNC || ft->tup) err(c,line,"partial() needs a function with fixed parameters, not %s",ty_name(ft));
    AFunc *fn= xinfo(fx)->kind==X_FUNCREF ? xinfo(fx)->fn : NULL;
    int np=ft->nelems; Expr *bound[16]={0}; int pos=0;
    if(np>16) err(c,line,"too many parameters");
    for(int i=1;i<e->count;i++){ Expr *a=e->items[i];
        if(a->akind==3){ int p=-1; if(fn && fn->def) for(int k=0;k<fn->nparams;k++) if(!strcmp(fn->def->params[k],a->kw)) p=k;
            if(p<0) err(c,line,"partial(): keyword argument '%s' needs a function defined with that parameter",a->kw);
            Expr *v=MPY_NEW0(Expr); *v=*a; v->kw=NULL; v->akind=0; bound[p]=v; }
        else if(a->akind) err(c,line,"partial(*args) is not supported in compiled code");
        else { while(pos<np && bound[pos]) pos++; if(pos>=np) err(c,line,"partial(): too many arguments"); bound[pos++]=a; } }
    Expr *call=xnew(EXPR_CALL,line); call->a=name_expr("__mpy_pf",line);
    char **ps=MPY_NEW_ARR(char*,np+2); Expr **ds=MPY_NEW_ARR(Expr*,np+2); int n=0;
    for(int i=0;i<np;i++) if(!bound[i]){ char v[24]; snprintf(v,sizeof v,"__mpy_p%d",i); ps[n++]=xstrdup2(v); }
    int nfree=n;
    ps[n]=xstrdup2("__mpy_pf"); ds[n++]=fx;
    for(int i=0;i<np;i++){ char v[24];
        if(bound[i]){ snprintf(v,sizeof v,"__mpy_b%d",i); ps[n]=xstrdup2(v); ds[n++]=bound[i]; }
        else snprintf(v,sizeof v,"__mpy_p%d",i);
        xpush(call,name_expr(xstrdup2(v),line)); }
    (void)nfree;
    void *keep=e->ty; memset(e,0,sizeof *e);
    e->kind=EXPR_LAMBDA; e->line=line; e->ty=keep; e->eparams=ps; e->neparam=n; e->edefaults=ds; e->estar=e->edstar=-1; e->ekwonly=n; e->a=call;
    return ck_expr(c,e);
}
static Ty *ck_call_inner(Ck *c, Expr *e){
    Expr *f=e->a; XInfo *xi=xinfo(e);
    eg_class_arg(c,e);
    if(f->kind==EXPR_NAME){
        if(f->name[0]=='\001') return ck_builtin(c,e,f->name+1);         /* made by the compiler: always the builtin */
        ASym *s=lookup(c,f->name);
        if(!s) return ck_builtin(c,e,f->name);
        if(s->kind==AS_FUNC){ AFunc *fn=(AFunc*)s->p;
            if(fn->outer){ xi->kind=X_CALLNEST; xi->fn=fn; xi->var=NULL; ck_args(c,e,fn,0,xi); return fn->ret; }   /* a nested function calling itself */
            if(fn->origin) fn=fn->origin;                          /* (an instance naming itself) */
            if(fn->pristine && !fn->cls){ fn=pick_instance(c,e,fn,xi); if(!fn) return pending(c,e->line,"the arguments"); }
            xi->kind=X_FUNC; xi->fn=fn; ck_args(c,e,fn,0,xi); return fn->ret; }
        if(s->kind==AS_CLASS) return ck_ctor(c,e,(AClass*)s->p);
        if(s->kind==AS_SYS && !strcmp((const char*)s->p,"functools.reduce")) return ck_reduce(c,e);
        if(s->kind==AS_SYS && !strcmp((const char*)s->p,"functools.partial")) return ck_partial(c,e);
        if(s->kind==AS_SYS && !strcmp((const char*)s->p,"string.capwords")) return ck_capwords(c,e);
        if(s->kind==AS_SYS && !strncmp((const char*)s->p,"ctypes.",7)) return ck_ctypes_fn(c,e,(const char*)s->p+7);
        if(s->kind==AS_SYS && !strncmp((const char*)s->p,"json.",5)) return ck_json(c,e,(const char*)s->p+5);
        if(s->kind==AS_SYS && !strncmp((const char*)s->p,"minipy.",7)) return ck_minipy(c,e,(const char*)s->p+7);
        if(s->kind==AS_SYS && !strncmp((const char*)s->p,"dataclasses.",12)) return ck_dataclasses(c,e,(const char*)s->p+12);
        if(s->kind==AS_CFUNC) return ck_ccall(c,e,(ACFunc*)s->p);
        if(s->kind==AS_VAR){
            AVar *v=(AVar*)s->p, *r=var_root(v);
            if(r->fn_const){                                       /* a nested def called by its name */
                XInfo *fx=xinfo(f); fx->kind=X_VAR; fx->var=v; fx->ty=v->ty;
                xi->kind=X_CALLNEST; xi->fn=r->fn_const; xi->var=v; ck_args(c,e,r->fn_const,0,xi); return r->fn_const->ret; }
            return ck_callval(c,e,ck_expr(c,f));
        }
        err(c,e->line,"'%s' is not callable",f->name);
    }
    if(f->kind==EXPR_INDEX && f->a->kind==EXPR_NAME){ ASym *s=lookup(c,f->a->name);   /* Box[int](...): Box(...) */
        if(s && s->kind==AS_CLASS) return ck_ctor(c,e,(AClass*)s->p); }
    if(f->kind!=EXPR_ATTRIBUTE) return ck_callval(c,e,ck_expr(c,f));      /* (lambda x: ...)(1), fs[i](x), make()(x) */
    const char *m=f->name;
    if(is_sys(c,f->a)) return ck_sys(c,e,m);
    if(is_bmod(c,f->a,"asyncio")) return ck_asyncio(c,e,m);
    if(is_bmod(c,f->a,"ctypes")) return ck_ctypes_fn(c,e,m);
    if(is_bmod(c,f->a,"json")) return ck_json(c,e,m);
    if(is_bmod(c,f->a,"minipy")) return ck_minipy(c,e,m);
    if(is_bmod(c,f->a,"dataclasses")) return ck_dataclasses(c,e,m);
    if(is_bmod(c,f->a,"functools") && !strcmp(m,"partial")) return ck_partial(c,e);
    if(is_bmod(c,f->a,"string") && !strcmp(m,"capwords")) return ck_capwords(c,e);
    if(is_bmod(c,f->a,"thread")) return ck_thread(c,e,m);
    if(bmod(c,f->a)) return ck_bmod(c,e,bmod(c,f->a),m);
    if(f->a->kind==EXPR_NAME && !lookup(c,f->a->name)){             /* bytes.fromhex(...), int.from_bytes(...) */
        static const char *const cm[]={"bytes.fromhex","int.from_bytes",NULL};
        char q[64]; snprintf(q,sizeof q,"%s.%s",f->a->name,m);
        for(int i=0;cm[i];i++) if(!strcmp(q,cm[i])) return ck_builtin(c,e,cm[i]);
    }
    if(f->a->kind==EXPR_NAME){ ASym *s=lookup(c,f->a->name);       /* lib.f(...) */
        if(s && s->kind==AS_CLIB) return ck_ccall(c,e,cfunc_get(c->p,(ACLib*)s->p,m)); }
    if(f->a->kind==EXPR_ATTRIBUTE){ AModule *lm=module_expr(c,f->a->a);   /* module.lib.f(...) */
        ASym *s=lm?module_sym(lm,f->a->name):NULL;
        if(s && s->kind==AS_CLIB) return ck_ccall(c,e,cfunc_get(c->p,(ACLib*)s->p,m)); }
    AModule *mod=module_expr(c,f->a);
    if(mod){
        ASym *s=module_sym(mod,m);
        if(!s) err(c,e->line,"module %s has no attribute '%s'",mod->name,m);
        if(s->kind==AS_CFUNC) return ck_ccall(c,e,(ACFunc*)s->p);
        if(s->kind==AS_FUNC){ AFunc *fn=(AFunc*)s->p; xi->kind=X_FUNC; xi->fn=fn; ck_args(c,e,fn,0,xi); return fn->ret; }
        if(s->kind==AS_CLASS) return ck_ctor(c,e,(AClass*)s->p);
        if(s->kind==AS_VAR) return ck_callval(c,e,ck_expr(c,f));
        err(c,e->line,"%s.%s is not callable",mod->name,m);
    }
    if(class_expr(c,f->a)){
        ASym sx={0}, *s=&sx; sx.kind=AS_CLASS; sx.p=class_expr(c,f->a);
        if(s && s->kind==AS_CLASS){                       /* Class.method(obj, ...) / static method */
            AClass *cls=(AClass*)s->p; AFunc *fn=aot_find_method(cls,m);
            { AClass *in=aot_nested_class(c->p,cls,m); if(in && !fn) return ck_ctor(c,e,in); }        /* Outer.Inner(...) */
            if(!fn) err(c,e->line,"class %s has no method '%s'",cls->name,m);
            xi->kind=X_STATIC; xi->fn=fn; xi->cls=cls; ck_args(c,e,fn,0,xi); return fn->ret;
        }
    }
    if(f->a->kind==EXPR_CALL && f->a->a->kind==EXPR_NAME && !strcmp(f->a->a->name,"super") && !lookup(c,"super")){
        AFunc *cur=c->fn;
        if(!cur || !cur->cls || cur->is_static) err(c,e->line,"super() is only available in methods");
        if(f->a->count) err(c,e->line,"use super() without arguments");
        if(!cur->cls->base) err(c,e->line,"class %s has no base class",cur->cls->name);
        AFunc *fn=aot_find_method(cur->cls->base,m);
        if(!fn && !strcmp(m,"__init__") && aot_is_exception(cur->cls->base)){
            if(e->count>1) err(c,e->line,"%s.__init__() takes at most one argument (the message) in compiled code",cur->cls->base->name);
            for(int i=0;i<e->count;i++) no_void(c,ck_expr(c,e->items[i]),e->line);
            xi->kind=X_SUPER; xi->fn=NULL; xi->name="exc_init"; xi->cls=cur->cls->base; return TY_VOID_T;
        }
        if(!fn) err(c,e->line,"base classes of %s have no method '%s'",cur->cls->name,m);
        xi->kind=X_SUPER; xi->fn=fn; xi->cls=cur->cls->base; ck_args(c,e,fn,fn->is_static?0:1,xi); return fn->ret;
    }
    Ty *t=ty_find(ck_expr(c,f->a));
    if(t->k==TY_VAR){ for(int i=0;i<e->count;i++) ck_expr(c,e->items[i]); return pending(c,e->line,"the object whose method is called"); }
    if(t->k==TY_OBJ){
        AFunc *fn=aot_find_method(t->cls,m);
        if(!fn){ AField *fd=aot_find_field(t->cls,m); TyKind fk=fd?ty_find(fd->ty)->k:TY_VOID;
            if(fd && (fk==TY_FUNC||fk==TY_VAR)) return ck_callval(c,e,ck_expr(c,f));        /* self.callback(x) */
            if(fd) err(c,e->line,"field %s.%s is not callable",t->cls->name,m);
            if(!c->strict){ for(int i=0;i<e->count;i++) ck_expr(c,e->items[i]); return pending(c,e->line,"the field"); }   /* (a class instance whose __init__ is not checked yet) */
            err(c,e->line,"%s has no method '%s'",t->cls->name,m); }
        if(fn->decovar){                                        /* through Class.name (decorated): name(obj, args) */
            Ty *ft=ty_find(fn->decovar->ty);
            if(ft->k==TY_VAR){ for(int i=0;i<e->count;i++) ck_expr(c,e->items[i]); return pending(c,e->line,"the decorated method"); }
            if(ft->k!=TY_FUNC || ft->nelems-(ft->tup&1)-((ft->tup>>1)&1)<1) err(c,e->line,"%s.%s is not a method after its decorators (%s)",t->cls->name,m,ty_name(ft));
            expect(c,ft->elems[0],t,e->line,"self");
            Ty view=*ft; view.elems=ft->elems+1; view.nelems--; view.link=NULL;
            Ty *r=ck_callval(c,e,&view);
            xi->kind=X_CALLDECO; xi->var=fn->decovar; xi->fn=fn;
            return r;
        }
        xi->kind=fn->is_static?X_STATIC:X_METHOD; xi->fn=fn; xi->cls=t->cls;
        ck_args(c,e,fn,fn->is_static?0:1,xi);
        return fn->ret;
    }
    return ck_tmethod(c,e,t,m);
}

/* ---------------------------------------------------------------- statements */

static void ck_stmts(Ck *c, Stmt **b, int n);
static void ck_body(Ck *c, AFunc *f);

/* Store a value of type vt into target t (name, attribute, item, or unpacking). */
static void ck_store(Ck *c, Expr *t, Ty *vt, int line){
    XInfo *xi=xinfo(t);
    no_void(c,vt,line);
    switch(t->kind){
        case EXPR_NAME:{
            if(xi->own_var){ xi->kind=X_VAR; xi->ty=xi->var->ty; char what[160]; snprintf(what,sizeof what,"variable '%s'",t->name); expect(c,xi->var->ty,vt,line,what); return; }
            ASym *s=lookup(c,t->name);
            if(!s || s->kind!=AS_VAR) err(c,line,"cannot assign to '%s'",t->name);
            if(!strcmp(t->name,"_") && xi->kind!=X_VAR){             /* _ takes throwaway values: one of another type is dropped */
                Ty *have=ty_find(((AVar*)s->p)->ty), *got=ty_find(vt);
                if(xi->kind==X_NONE && have->k!=TY_VAR && got->k!=TY_VAR && !ty_same(have,got) && !(numeric(have)&&numeric(got))){ xi->kind=X_NONE; xi->var=NULL; xi->name="_discard"; return; }
            }
            if(xi->name && !strcmp(xi->name,"_discard")) return;
            xi->kind=X_VAR; xi->var=(AVar*)s->p; xi->ty=xi->var->ty;
            if(xi->var->src) var_root(xi->var)->nonlocal_set=1;     /* nonlocal x; x = ...: shared through a cell */
            char what[160]; snprintf(what,sizeof what,"variable '%s'",t->name);
            expect(c,xi->var->ty,vt,line,what);
            return; }
        case EXPR_ATTRIBUTE:{
            AModule *m=module_expr(c,t->a);
            if(m){ ASym *s=module_sym(m,t->name); if(!s||s->kind!=AS_VAR) err(c,line,"cannot assign %s.%s",m->name,t->name);
                xi->kind=X_VAR; xi->var=(AVar*)s->p; xi->ty=xi->var->ty; expect(c,xi->var->ty,vt,line,"the module variable"); return; }
            if(class_expr(c,t->a)){ ASym csx={0}, *cs=&csx; csx.kind=AS_CLASS; csx.p=class_expr(c,t->a);   /* Class.name = value: its class-level value */
                if(cs && cs->kind==AS_CLASS){ AClass *cls=(AClass*)cs->p; AField *fd=aot_find_field(cls,t->name);
                    if(!fd){ if(aot_find_method(cls,t->name)) err(c,line,"cannot replace method %s.%s in compiled code",cls->name,t->name);
                        fd=add_field(cls,t->name,NULL,NULL);                     /* a new class attribute */
                        char h[600]; snprintf(h,sizeof h," %s.%s",cls->symname,fd->name); fd->cvar=new_global(c,cls->mod,h); fd->cvar->ty=fd->ty; symtab_add(&cls->mod->syms,h,AS_VAR,fd->cvar); }
                    if(!fd->cvar) err(c,line,"%s.%s is an instance attribute; assign it through an object",cls->name,t->name);
                    int own=0; for(int i=0;i<cls->nfields;i++) if(cls->fields[i]==fd) own=1;
                    if(!own) err(c,line,"%s.%s is defined by a base class: assigning it through %s is not supported in compiled code",cls->name,t->name,cls->name);
                    xi->kind=X_VAR; xi->var=fd->cvar; xi->ty=fd->ty;
                    char what[160]; snprintf(what,sizeof what,"class attribute %s.%s",cls->name,t->name);
                    expect(c,fd->ty,vt,line,what); return; } }
            Ty *o=ty_find(ck_expr(c,t->a));
            if(o->k==TY_VAR){ pending(c,line,"the object"); return; }
            if(o->k!=TY_OBJ) err(c,line,"cannot set attribute '%s' of %s",t->name,ty_name(o));
            AField *fd=aot_find_field(o->cls,t->name);
            if(!fd){
                if(aot_find_method(o->cls,t->name)) err(c,line,"'%s' is a method of %s",t->name,o->cls->name);
                fd=add_field(o->cls,t->name,NULL,NULL);
            }
            xi->kind=X_FIELD; xi->field=fd; xi->cls=o->cls; xi->ty=fd->ty; aot_field_root(fd)->inst_set=1;
            if(o->cls->dc_frozen && t->akind!=7){                   /* a frozen dataclass: FrozenInstanceError (but in its __init__) */
                AFunc *cf=c->fn; int init= cf && cf->cls && cf->cls->dc && cf->def==cf->cls->dc->init_def && aot_subclass(o->cls,cf->cls);
                if(!init){ xi->name="frozen"; xi->fn=NULL; } }
            char what[160]; snprintf(what,sizeof what,"field %s.%s",o->cls->name,t->name);
            expect(c,fd->ty,vt,line,what);
            return; }
        case EXPR_SLICE:{                                       /* xs[a:b] = items */
            Ty *o=ty_find(ck_expr(c,t->a)); Expr *bounds[3]={t->b,t->c,t->d};
            for(int k=0;k<3;k++) if(bounds[k] && bounds[k]->kind!=EXPR_NONE) expect(c,TY_INT_T,ck_expr(c,bounds[k]),line,"a slice bound");
            if(o->k==TY_VAR){ pending(c,line,"the list"); return; }
            if(o->k!=TY_LIST || o->tup) err(c,line,"'%s' object does not support item assignment",ty_name(o));
            Ty *v=ty_find(vt); if(v->k==TY_VAR){ pending(c,line,"the assigned value"); return; }
            Ty *el= v->k==TY_STR ? TY_STR_T : (v->k==TY_LIST||v->k==TY_SET||v->k==TY_GEN) ? v->elem : NULL;
            if(!el) err(c,line,"can only assign an iterable to a slice in compiled code (a list, a set, a str or a generator), not %s",ty_name(v));
            expect(c,o->elem,el,line,"an item of the slice");
            xi->ty=o; return; }
        case EXPR_INDEX:{
            Ty *o=ty_find(ck_expr(c,t->a)), *i=ck_expr(c,t->b);
            if(o->k==TY_VAR){ pending(c,line,"the indexed value"); return; }
            if(o->k==TY_OBJ){
                AFunc *m=aot_find_method(o->cls,"__setitem__");
                if(!m || m->nparams!=3) err(c,line,"%s has no __setitem__(self, key, value)",o->cls->name);
                expect(c,m->params[1]->ty,i,line,"the key of __setitem__"); expect(c,m->params[2]->ty,vt,line,"the value of __setitem__");
                xi->kind=X_OPMETHOD; xi->fn=m; xi->ty=m->params[2]->ty;
                AFunc *g=aot_find_method(o->cls,"__getitem__"); xi->field=NULL; xi->var=NULL; (void)g;
                return; }
            if(o->k==TY_LIST){ expect(c,TY_INT_T,i,line,"a list index"); expect(c,o->elem,vt,line,"a list element"); xi->ty=o->elem; return; }
            if(o->k==TY_DICT){ expect(c,ty_dkey(o),i,line,"a dictionary key"); expect(c,o->elem,vt,line,"a dictionary value"); xi->ty=o->elem; return; }
            if(o->k==TY_STR) err(c,line,"strings are immutable");
            err(c,line,"%s does not support item assignment",ty_name(o)); }
        case EXPR_TUPLE: case EXPR_LIST:{
            Ty *v=ty_find(vt);
            if(v->k==TY_VAR){ pending(c,line,"the unpacked value"); return; }
            int star=-1; for(int i=0;i<t->count;i++) if(t->items[i]->akind==1) star=i;
            if(star>=0){                                        /* a, *b, c = ...: b gets a list of the rest */
                xi->ty=vt;
                if(v->k==TY_TUPLE){
                    if(v->names) err(c,line,"%s (a dict literal with values of different types) cannot be unpacked in compiled code",ty_name(v));
                    if(v->nelems<t->count-1) err(c,line,"ValueError: not enough values to unpack (expected at least %d, got %d)",t->count-1,v->nelems);
                    int rest=v->nelems-(t->count-1);
                    for(int i=0;i<star;i++) ck_store(c,t->items[i],v->elems[i],line);
                    for(int i=star+1;i<t->count;i++) ck_store(c,t->items[i],v->elems[v->nelems-(t->count-i)],line);
                    Ty *el=rest?v->elems[star]:ty_var();
                    for(int i=star+1;i<star+rest;i++) if(!unify(c,el,v->elems[i])) err(c,line,"the starred target would hold values of different types (%s)",ty_name(v));
                    ck_store(c,t->items[star],ty_new(TY_LIST,el,NULL),line);
                    return; }
                Ty *el= v->k==TY_STR ? TY_STR_T : v->k==TY_LIST ? v->elem : NULL;
                if(!el) err(c,line,"only a list, a tuple or a str can be unpacked, not %s",ty_name(v));
                for(int i=0;i<t->count;i++) ck_store(c,t->items[i],i==star?ty_new(TY_LIST,el,NULL):el,line);
                return; }
            if(v->k==TY_TUPLE){
                if(v->names) err(c,line,"%s (a dict literal with values of different types) cannot be unpacked in compiled code",ty_name(v));
                if(v->nelems!=t->count) err(c,line,"cannot unpack %s into %d targets",ty_name(v),t->count);
                xi->ty=vt;
                for(int i=0;i<t->count;i++) ck_store(c,t->items[i],v->elems[i],line);
                return;
            }
            if(v->k!=TY_LIST && v->k!=TY_STR) err(c,line,"only a list, a tuple or a str can be unpacked, not %s",ty_name(v));
            xi->ty=vt;
            for(int i=0;i<t->count;i++) ck_store(c,t->items[i],v->k==TY_STR?TY_STR_T:v->elem,line);
            return; }
        default: err(c,line,"cannot assign to this expression");
    }
}
/* Current type of an assignment target (for augmented assignment). */
static Ty *ck_target_value(Ck *c, Expr *t, int line){
    if(t->kind==EXPR_NAME||t->kind==EXPR_ATTRIBUTE||t->kind==EXPR_INDEX) return ck_expr(c,t);
    err(c,line,"invalid target of augmented assignment");
}

static void ck_assign(Ck *c, Stmt *s){
    AotUnit *u=c->mod->unit;
    AAssign *a=aot_assign(u,s);
    if(a->ann && !a->annot) a->annot=type_of(c,a->ann,s->line);
    if(a->annot){
        if(a->ntarget!=1||(a->target[0]->kind!=EXPR_NAME&&a->target[0]->kind!=EXPR_ATTRIBUTE)) err(c,s->line,"only a name or an attribute can be annotated");
        if(ty_find(a->annot)->k==TY_VOID) err(c,s->line,"a variable cannot be annotated as None");
        ck_store(c,a->target[0],a->annot,s->line);
    }
    if(!a->value) return;
    if(a->aug){
        Expr *t=a->target[0];
        Ty *cur=ck_target_value(c,t,s->line), *v=ck_expr(c,a->value);
        no_void(c,v,s->line);
        if(a->aug==T_PERCENT_ASSIGN && ty_find(cur)->k==TY_STR) err(c,s->line,"%%= formatting is not supported in compiled code");
        Ty *r=binop_type_m(c,a->aug,cur,v,s->line,&a->opfn);
        if(numeric(cur)&&numeric(r)&&ty_find(cur)->k!=ty_find(r)->k&&!(ty_find(cur)->k==TY_INT&&ty_find(r)->k==TY_INT))
            err(c,s->line,"type mismatch: the target is %s, the result is %s",ty_name(cur),ty_name(r));
        else if(!ty_same(cur,r) && !unify(c,cur,r)) err(c,s->line,"type mismatch: the target is %s, the result is %s",ty_name(cur),ty_name(r));
        if(t->kind==EXPR_INDEX||t->kind==EXPR_ATTRIBUTE||t->kind==EXPR_NAME) ck_store(c,t,cur,s->line);
        return;
    }
    int parallel=a->value->kind==EXPR_TUPLE;
    for(int k=0;k<a->ntarget && parallel;k++){ Expr *t=a->target[k]; if((t->kind!=EXPR_TUPLE&&t->kind!=EXPR_LIST)||t->count!=a->value->count) parallel=0; }
    if(parallel){                                         /* a, b = x, y: no tuple is built */
        for(int i=0;i<a->value->count;i++){
            Ty *v=ck_expr(c,a->value->items[i]);
            for(int k=0;k<a->ntarget;k++) ck_store(c,a->target[k]->items[i],v,s->line);
        }
        xinfo(a->value)->ty=TY_VOID_T;
        return;
    }
    Ty *want=a->annot;
    if(!want && a->ntarget==1 && a->target[0]->kind==EXPR_NAME){ ASym *t=lookup(c,a->target[0]->name); if(t && t->kind==AS_VAR) want=((AVar*)t->p)->ty; }
    Ty *v=want ? ck_expr_want(c,a->value,want) : ck_expr(c,a->value);
    for(int k=0;k<a->ntarget;k++) ck_store(c,a->target[k],v,s->line);
}

/* ---------------------------------------------------------------- match */
/* A match statement becomes ifs once its subject's type is known: each pattern tests (and
   binds) parts of the subject, kept in hidden variables; a pattern no value of that type can
   match is left out (CPython would just not match it). */
typedef struct { Stmt **v; int n, cap; } SL;
static void sl_add(SL *l, Stmt *s){ if(l->n==l->cap){ l->cap=l->cap?l->cap*2:8; l->v=(Stmt**)xrealloc(l->v,sizeof(Stmt*)*(size_t)l->cap); } l->v[l->n++]=s; }
static void sl_cat(SL *l, SL *m){ for(int i=0;i<m->n;i++) sl_add(l,m->v[i]); }
static Expr *nx(const char *name, int line){ return name_expr(xstrdup2(name),line); }
static Stmt *st_assign(Expr *t, Expr *v, int line){ Stmt *s=stmt_new(STMT_ASSIGN,NULL,line); s->targets=MPY_NEW_ARR(Expr*,1); s->targets[0]=t; s->ntargets=1; s->value=v; return s; }
static Stmt *st_if(Expr *cond, SL *body, int line){ Stmt *s=stmt_new(STMT_IF,NULL,line); s->expr=cond; for(int i=0;i<body->n;i++) stmt_add_body(s,body->v[i]); if(!body->n) stmt_add_body(s,stmt_new(STMT_PASS,NULL,line)); return s; }
static Expr *ex_cmp(Expr *a, int code, Expr *b, int line){ Expr *e=xnew(EXPR_COMPARE,line); xpush(e,a); b->akind=code; xpush(e,b); return e; }
static Expr *ex_call(const char *fn, Expr *a, Expr *b, int line){ Expr *e=xnew(EXPR_CALL,line); e->a=nx(fn,line); if(a) xpush(e,a); if(b) xpush(e,b); return e; }
static Expr *ex_index(Expr *o, Expr *i, int line){ Expr *e=xnew(EXPR_INDEX,line); e->a=o; e->b=i; return e; }
static Expr *ex_int(Ck *c, long long v, int line){ char t[32]; snprintf(t,sizeof t,"%lld",v); return py_front_expr(c->mod->unit->path,t,line); }
static Expr *ex_not(Expr *a, int line){ Expr *e=xnew(EXPR_UNARY,line); e->op=T_NOT; e->a=a; return e; }
static const char *hidden_for(Ck *c, const char *base){ char h[64]; named_hidden(c,base,h,sizeof h); return xstrdup2(h); }
/* class C's positional sub-patterns name these attributes (__match_args__; a dataclass's fields) */
static const char *match_arg(Ck *c, AClass *k, int i, int line){
    for(AClass *b=k;b;b=b->base){
        AField *fd=NULL; for(int j=0;j<b->nfields;j++) if(!strcmp(b->fields[j]->name,"__match_args__")) fd=b->fields[j];
        if(fd && fd->init){ Expr *t=fd->init;
            if(t->kind!=EXPR_TUPLE) err(c,line,"%s.__match_args__ must be a tuple of strings",k->name);
            if(i>=t->count) err(c,line,"%s() accepts %d positional sub-pattern%s (%d given)",k->name,t->count,t->count==1?"":"s",i+1);
            if(t->items[i]->kind!=EXPR_LITERAL || t->items[i]->tok->kind!=T_STRING) err(c,line,"%s.__match_args__ must be a tuple of strings",k->name);
            return t->items[i]->tok->text; }
        if(b->dc){ int n=0; for(int j=0;j<b->dc->n;j++){ DcField *f=&b->dc->f[j]; if(!f->init || f->kw_only || f->initvar) continue; if(n++==i) return f->name; }
            err(c,line,"%s() accepts %d positional sub-pattern%s (%d given)",k->name,n,n==1?"":"s",i+1); }
    }
    err(c,line,"%s() accepts 0 positional sub-patterns (%d given)",k->name,i+1);
}
static int match_pat(Ck *c, const char *xv, Ty *T, Expr *p, SL *inner, SL *out);
/* the patterns of a sequence pattern from item i on (xv's items), then inner */
static int match_items(Ck *c, const char *xv, Ty *T, Expr *p, int i, int n, int star, SL *inner, SL *out, int line){
    if(i==n){ sl_cat(out,inner); return 1; }
    Expr *it=p->items[i]; Ty *base=ty_find(T);
    if(it->akind==PAT_STAR){                                    /* *rest: a list of the middle items */
        if(it->name){ Expr *sl=xnew(EXPR_SLICE,line); sl->a=nx(xv,line);
            if(base->k==TY_TUPLE){
                Expr *tl=xnew(EXPR_LIST,line); for(int k=i;k<base->nelems-(n-1-i);k++) xpush(tl,ex_index(nx(xv,line),ex_int(c,k,line),line));
                sl_add(out,st_assign(nx(it->name,line),tl,line)); }
            else { sl->b=ex_int(c,i,line); sl->c=xnew(EXPR_BINARY,line); sl->c->op=T_MINUS; sl->c->a=ex_call("\001len",nx(xv,line),NULL,line); sl->c->b=ex_int(c,n-1-i,line);
                Expr *v= base->tup ? ex_call("\001list",sl,NULL,line) : sl;
                sl_add(out,st_assign(nx(it->name,line),v,line)); } }
        return match_items(c,xv,T,p,i+1,n,star,inner,out,line); }
    Ty *et; Expr *idx;
    if(base->k==TY_TUPLE){ int k= star<0||i<star ? i : base->nelems-(n-i); et=base->elems[k]; idx=ex_int(c,k,line); }
    else { et=base->elem; idx= star<0||i<star ? ex_int(c,i,line) : ex_int(c,i-n,line); }
    const char *h=hidden_for(c,"item");
    SL rest={0}; if(!match_items(c,xv,T,p,i+1,n,star,inner,&rest,line)) return 0;
    SL here={0}; if(!match_pat(c,h,et,it,&rest,&here)) return 0;
    sl_add(out,st_assign(nx(h,line),ex_index(nx(xv,line),idx,line),line));
    sl_cat(out,&here);
    return 1;
}
static int match_pat(Ck *c, const char *xv, Ty *T, Expr *p, SL *inner, SL *out){
    int line=p->line; T=ty_find(T); int opt=ty_opt(T);
    switch(p->akind){
        case PAT_AS:{
            SL in2={0}; if(p->name) sl_add(&in2,st_assign(nx(p->name,line),nx(xv,line),line)); sl_cat(&in2,inner);
            if(!p->a){ sl_cat(out,&in2); return 1; }
            return match_pat(c,xv,T,p->a,&in2,out); }
        case PAT_VALUE:{
            Ty *tv=ty_find(ck_expr(c,p->a));
            if(tv->k==TY_VAR){ pending(c,line,"a pattern's value"); return 0; }
            int ok=(numeric(T)&&numeric(tv)) || (T->k==tv->k && T->k!=TY_OBJ) || (T->k==TY_OBJ && tv->k==TY_OBJ && (aot_subclass(T->cls,tv->cls)||aot_subclass(tv->cls,T->cls)));
            if(!ok) return 0;
            sl_add(out,st_if(ex_cmp(nx(xv,line),CMP_EQ,p->a,line),inner,line)); return 1; }
        case PAT_SINGLETON:
            if(p->a->kind==EXPR_NONE){ if(!opt && T->k!=TY_VOID) return 0;
                sl_add(out,st_if(ex_cmp(nx(xv,line),CMP_IS,xnew(EXPR_NONE,line),line),inner,line)); return 1; }
            if(T->k!=TY_BOOL) return 0;
            sl_add(out,st_if(ex_cmp(nx(xv,line),CMP_EQ,xnew(p->a->kind,line),line),inner,line)); return 1;
        case PAT_OR:{
            const char *hf=hidden_for(c,"alt"); int any=0;
            SL alts={0}; sl_add(&alts,st_assign(nx(hf,line),xnew(EXPR_FALSE,line),line));
            for(int i=0;i<p->count;i++){ SL set={0}, one={0}; sl_add(&set,st_assign(nx(hf,line),xnew(EXPR_TRUE,line),line));
                if(!match_pat(c,xv,T,p->items[i],&set,&one)) continue;
                any=1; if(i==0) sl_cat(&alts,&one); else sl_add(&alts,st_if(ex_not(nx(hf,line),line),&one,line)); }
            if(!any) return 0;
            sl_cat(out,&alts); sl_add(out,st_if(nx(hf,line),inner,line)); return 1; }
        case PAT_SEQ:{
            if(T->k!=TY_LIST && !(T->k==TY_TUPLE && !T->names)) return 0;
            int n=p->count, star=-1; for(int i=0;i<n;i++) if(p->items[i]->akind==PAT_STAR) star=i;
            SL body={0};
            if(T->k==TY_TUPLE){ if(star<0 ? T->nelems!=n : T->nelems<n-1) return 0;
                if(!match_items(c,xv,T,p,0,n,star,inner,&body,line)) return 0;
                if(opt) sl_add(out,st_if(ex_cmp(nx(xv,line),CMP_ISNOT,xnew(EXPR_NONE,line),line),&body,line)); else sl_cat(out,&body);
                return 1; }
            if(!match_items(c,xv,T,p,0,n,star,inner,&body,line)) return 0;
            Expr *cond=ex_cmp(ex_call("\001len",nx(xv,line),NULL,line),star<0?CMP_EQ:CMP_GE,ex_int(c,star<0?n:n-1,line),line);
            if(opt){ Expr *b=xnew(EXPR_BOOL,line); b->op=T_AND; b->a=ex_cmp(nx(xv,line),CMP_ISNOT,xnew(EXPR_NONE,line),line); b->b=cond; cond=b; }
            sl_add(out,st_if(cond,&body,line)); return 1; }
        case PAT_MAP:{
            if(T->k!=TY_DICT) return 0;
            SL cur={0}; sl_cat(&cur,inner);
            const char **keys=MPY_NEW_ARR(const char*,p->count+1);
            for(int i=0;i<p->count;i++) keys[i]=hidden_for(c,"key");
            if(p->name){ SL r={0}; sl_add(&r,st_assign(nx(p->name,line),ex_call("\001dict",nx(xv,line),NULL,line),line));
                for(int i=0;i<p->count;i++){ Stmt *d=stmt_new(STMT_DEL,NULL,line); d->targets=MPY_NEW_ARR(Expr*,1); d->targets[0]=ex_index(nx(p->name,line),nx(keys[i],line),line); d->ntargets=1; sl_add(&r,d); }
                sl_cat(&r,&cur); cur=r; }
            for(int i=p->count-1;i>=0;i--){ const char *hv=hidden_for(c,"value");
                SL here={0}; if(!match_pat(c,hv,T->elem,p->vals[i],&cur,&here)) return 0;
                SL got={0}; sl_add(&got,st_assign(nx(hv,line),ex_index(nx(xv,line),nx(keys[i],line),line),line)); sl_cat(&got,&here);
                SL step={0}; sl_add(&step,st_assign(nx(keys[i],line),p->items[i],line));
                sl_add(&step,st_if(ex_cmp(nx(keys[i],line),CMP_IN,nx(xv,line),line),&got,line));
                cur=step; }
            if(opt) sl_add(out,st_if(ex_cmp(nx(xv,line),CMP_ISNOT,xnew(EXPR_NONE,line),line),&cur,line)); else sl_cat(out,&cur);
            return 1; }
        case PAT_CLASS:{
            AClass *k=class_expr(c,p->a); const char *subj=xv; SL pre={0}; Expr *cond=NULL;
            if(!k){
                if(p->a->kind!=EXPR_NAME || !is_builtin_type_name(p->a->name)) err(c,line,"%s is not a class",p->a->kind==EXPR_NAME?p->a->name:"the pattern");
                const char *bn=p->a->name;
                int ok= (!strcmp(bn,"int")&&(T->k==TY_INT||T->k==TY_BOOL)) || (!strcmp(bn,"bool")&&T->k==TY_BOOL) || (!strcmp(bn,"float")&&T->k==TY_FLOAT)
                     || (!strcmp(bn,"str")&&T->k==TY_STR) || (!strcmp(bn,"bytes")&&T->k==TY_BYTES) || (!strcmp(bn,"list")&&T->k==TY_LIST&&!T->tup)
                     || (!strcmp(bn,"tuple")&&(T->k==TY_TUPLE||(T->k==TY_LIST&&T->tup))) || (!strcmp(bn,"dict")&&T->k==TY_DICT) || (!strcmp(bn,"set")&&T->k==TY_SET) || !strcmp(bn,"object");
                if(!ok) return 0;
                for(int i=0;i<p->count;i++) if(p->items[i]->kw) err(c,line,"%s() patterns take no keyword sub-patterns in compiled code",bn);
                if(p->count>1) err(c,line,"%s() accepts 1 positional sub-pattern (%d given)",bn,p->count);
                SL body={0}; if(p->count){ if(!match_pat(c,xv,T,p->items[0],inner,&body)) return 0; } else sl_cat(&body,inner);
                if(opt) sl_add(out,st_if(ex_cmp(nx(xv,line),CMP_ISNOT,xnew(EXPR_NONE,line),line),&body,line)); else sl_cat(out,&body);
                return 1; }
            if(T->k!=TY_OBJ) return 0;
            if(aot_subclass(T->cls,k)){ if(opt) cond=ex_cmp(nx(xv,line),CMP_ISNOT,xnew(EXPR_NONE,line),line); }
            else if(aot_subclass(k,T->cls)){                       /* isinstance(x, C): x as a C from there on */
                Expr *kx=nx(k->symname,line);
                cond=ex_call("\001isinstance",nx(xv,line),kx,line);
                subj=hidden_for(c,"as");
                sl_add(&pre,st_assign(nx(subj,line),ex_call("\001downcast",nx(k->symname,line),nx(xv,line),line),line)); }
            else return 0;
            SL cur={0}; sl_cat(&cur,inner);
            for(int i=p->count-1;i>=0;i--){ Expr *sp=p->items[i]; int pos=0; for(int j=0;j<i;j++) if(!p->items[j]->kw) pos++;
                const char *an= sp->kw ? sp->kw : match_arg(c,k,pos,line);
                AField *fd=aot_find_field(k,an); AFunc *pm=fd?NULL:aot_find_method(k,an);
                if(!fd && !(pm && pm->is_property)) err(c,line,"%s has no attribute '%s'",k->name,an);
                const char *ha=hidden_for(c,"attr");
                SL here={0}; if(!match_pat(c,ha,fd?fd->ty:pm->ret,sp,&cur,&here)) return 0;
                Expr *at=xnew(EXPR_ATTRIBUTE,line); at->a=nx(subj,line); at->name=(char*)an;
                SL step={0}; sl_add(&step,st_assign(nx(ha,line),at,line)); sl_cat(&step,&here); cur=step; }
            SL body={0}; sl_cat(&body,&pre); sl_cat(&body,&cur);
            if(cond) sl_add(out,st_if(cond,&body,line)); else sl_cat(out,&body);
            return 1; }
        default: err(c,line,"unsupported pattern");
    }
}
/* match subject: case p [if guard]: body ... -> ifs (in place; the statement is checked as that) */
static int desugar_match(Ck *c, Stmt *s){
    Expr *subject=s->expr; int line=s->line;
    Ty *T=ty_find(ck_expr(c,subject));
    if(!ty_known(T)){ pending(c,line,"the match subject"); return 0; }
    const char *m=hidden_for(c,"subject"), *done=hidden_for(c,"matched");
    SL all={0};
    sl_add(&all,st_assign(nx(m,line),subject,line));
    sl_add(&all,st_assign(nx(done,line),xnew(EXPR_FALSE,line),line));
    int first=1;
    for(int i=0;i<s->body_count;i++){ Stmt *cs=s->body[i];
        SL inner={0}, body={0};
        sl_add(&body,st_assign(nx(done,cs->line),xnew(EXPR_TRUE,cs->line),cs->line));
        for(int k=0;k<cs->body_count;k++) sl_add(&body,cs->body[k]);
        if(cs->expr2) sl_add(&inner,st_if(cs->expr2,&body,cs->line)); else sl_cat(&inner,&body);
        SL got={0};
        if(!match_pat(c,m,T,cs->expr,&inner,&got)) continue;            /* (no value of this type matches it) */
        if(first) sl_cat(&all,&got); else sl_add(&all,st_if(ex_not(nx(done,cs->line),cs->line),&got,cs->line));
        first=0; }
    s->kind=STMT_IF; s->expr=xnew(EXPR_TRUE,line); s->body=all.v; s->body_count=all.n; s->body_cap=all.cap; s->aux=NULL;
    return 1;
}
/* ---------------------------------------------------------------- with */
static void hidden_reg(Ck *c, const char *name){                 /* a hidden variable rewritten code names */
    if(is_func(c->fn)){ if(!symtab_find(&c->fn->locals,name)){ AVar *v=hidden_local(c->fn,name,NULL); symtab_add(&c->fn->locals,name,AS_VAR,v); } }
    else if(!module_sym(c->mod,name)){ AVar *v=new_global(c,c->mod,name); symtab_add(&c->mod->syms,name,AS_VAR,v); }
}
/* with / async with on an object: CPython's expansion (__enter__; __exit__ with the exception,
   which it may swallow, or with None) -> 0 when the types are not known yet */
static int desugar_with(Ck *c, Stmt *s){
    AotUnit *u=c->mod->unit; AWith *w=aot_with(u,s); int line=s->line, any=0;
    for(int i=0;i<w->n;i++){ Ty *t=ty_find(ck_expr(c,w->e[i])); if(t->k==TY_VAR){ pending(c,line,"the context manager"); return 0; } if(t->k==TY_OBJ) any=1; }
    if(!any && !s->is_async){
        for(int i=0;i<w->n;i++) if(s->withtgt && s->withtgt[i]) err(c,line,"`with ... as` takes a name here in compiled code");
        return 1; }
    if(w->n>1){                                                  /* with a, b: with a: with b: */
        Stmt *in=stmt_new(STMT_WITH,NULL,line); in->is_async=s->is_async;
        in->ntargets=s->ntargets-1; in->targets=s->targets+1; in->withas=s->withas+1; in->withtgt=s->withtgt?s->withtgt+1:NULL;
        in->body=s->body; in->body_count=s->body_count; in->body_cap=s->body_cap;
        s->ntargets=1; s->body=NULL; s->body_count=s->body_cap=0; stmt_add_body(s,in);
        s->aux=NULL; w=aot_with(u,s); }
    Ty *t=ty_find(ck_expr(c,w->e[0]));
    const char *en=s->is_async?"__aenter__":"__enter__", *exn=s->is_async?"__aexit__":"__exit__", *aw=s->is_async?"await ":"";
    if(t->k!=TY_OBJ) err(c,line,"'%s' object does not support the %scontext manager protocol",ty_name(t),s->is_async?"asynchronous ":"");
    AFunc *fen=aot_find_method(t->cls,en), *fex=aot_find_method(t->cls,exn);
    if(!fen || !fex) err(c,line,"'%s' object does not support the %scontext manager protocol (missed %s method)",t->cls->name,s->is_async?"asynchronous ":"",!fen?en:exn);
    int k=++c->nhidden; char m[48], v[48], h[48], e[48];
    snprintf(m,sizeof m,"__mpy_with%d_mgr",k); snprintf(v,sizeof v,"__mpy_with%d_value",k); snprintf(h,sizeof h,"__mpy_with%d_hit",k); snprintf(e,sizeof e,"__mpy_with%d_exc",k);
    hidden_reg(c,m); hidden_reg(c,h);
    Expr *tg= s->withtgt && s->withtgt[0] ? s->withtgt[0] : w->as[0] ? name_expr(w->as[0],line) : NULL;
    int env= ty_find(fen->ret)->k!=TY_VOID, exv= ty_find(fex->ret)->k!=TY_VOID;
    if(env) hidden_reg(c,v);
    Txt x={0};
    for(int i=1;i<line;i++) tx(&x,"\n");
    tx(&x,"%s = 0\n",m);
    if(env) tx(&x,"%s = %s%s.%s()\n",v,aw,m,en); else tx(&x,"%s%s.%s()\n",aw,m,en);
    tx(&x,"%s = False\ntry:\n",h);
    if(tg && env) tx(&x,"    __mpy_target = %s\n",v);
    tx(&x,"    pass\nexcept BaseException as %s:\n    %s = True\n",e,h);
    if(exv) tx(&x,"    if not %s%s.%s(type(%s), %s, None):\n        raise\n",aw,m,exn,e,e);
    else tx(&x,"    %s%s.%s(type(%s), %s, None)\n    raise\n",aw,m,exn,e,e);
    tx(&x,"finally:\n    if not %s:\n        %s%s.%s(None, None, None)\n",h,aw,m,exn);
    Stmt *blk=py_front_stmts(u->path,x.s,line); free(x.s);
    if(!blk) err(c,line,"internal error: with");
    blk->body[0]->value=w->e[0];
    Stmt *tr=NULL; for(int i=0;i<blk->body_count;i++) if(blk->body[i]->kind==STMT_TRY) tr=blk->body[i];
    int keep=tg&&env?1:0;
    if(keep) tr->body[0]->targets[0]=tg;
    Stmt **nb=MPY_NEW_ARR(Stmt*,keep+s->body_count+1); int nn=0;
    if(keep) nb[nn++]=tr->body[0];
    for(int i=0;i<s->body_count;i++) nb[nn++]=s->body[i];
    if(!nn) nb[nn++]=stmt_new(STMT_PASS,NULL,line);
    tr->body=nb; tr->body_count=nn; tr->body_cap=nn;
    s->kind=STMT_IF; s->expr=xnew(EXPR_TRUE,line); s->body=blk->body; s->body_count=blk->body_count; s->body_cap=blk->body_cap; s->aux=NULL;
    return 1;
}
/* async for x in obj (its __aiter__ / __anext__): a while loop until StopAsyncIteration (else: when it ran out) */
static void desugar_async_for(Ck *c, Stmt *s){
    AotUnit *u=c->mod->unit; int line=s->line, k=++c->nhidden;
    char it[48], v[48], d[48]; snprintf(it,sizeof it,"__mpy_afor%d_it",k); snprintf(v,sizeof v,"__mpy_afor%d_value",k); snprintf(d,sizeof d,"__mpy_afor%d_done",k);
    hidden_reg(c,it); hidden_reg(c,v); hidden_reg(c,d);
    Txt x={0}; for(int i=1;i<line;i++) tx(&x,"\n");
    tx(&x,"%s = (0).__aiter__()\n%s = False\nwhile True:\n    try:\n        %s = await %s.__anext__()\n    except StopAsyncIteration:\n        %s = True\n        break\n    __mpy_target = %s\n    pass\nif %s:\n    pass\n",it,d,v,it,d,v,d);
    Stmt *blk=py_front_stmts(u->path,x.s,line); free(x.s);
    if(!blk) err(c,line,"internal error: async for");
    blk->body[0]->value->a->a=aot_expr(u,&s->expr);                 /* (0).__aiter__() -> obj.__aiter__() */
    Stmt *wh=blk->body[2], *ifd=blk->body[3];
    Stmt *as=wh->body[1];
    if(s->param_count==1) as->targets[0]=name_expr(s->params[0],line);
    else { Expr *tu=xnew(EXPR_TUPLE,line); for(int i=0;i<s->param_count;i++) xpush(tu,name_expr(s->params[i],line)); as->targets[0]=tu; }
    Stmt **nb=MPY_NEW_ARR(Stmt*,2+s->body_count); int nn=0;
    nb[nn++]=wh->body[0]; nb[nn++]=as; for(int i=0;i<s->body_count;i++) nb[nn++]=s->body[i];
    wh->body=nb; wh->body_count=nn; wh->body_cap=nn;
    if(s->orelse_count){ ifd->body=s->orelse; ifd->body_count=s->orelse_count; ifd->body_cap=s->orelse_cap; }
    s->kind=STMT_IF; s->expr=xnew(EXPR_TRUE,line); s->body=blk->body; s->body_count=blk->body_count; s->body_cap=blk->body_cap;
    s->orelse=NULL; s->orelse_count=s->orelse_cap=0; s->aux=NULL; s->is_async=0;
}
/* isinstance(x, C) (also first in an `and`), x a variable of a base class of C -> 1 (its name, C);
   -1 when x's type is not known yet; 0 otherwise */
static int isinstance_test(Ck *c, Expr *e, const char **name, AClass **k, int allow_and){
    if(allow_and && e->kind==EXPR_BOOL && e->op==T_AND) return isinstance_test(c,e->a,name,k,1);
    if(e->kind!=EXPR_CALL || e->a->kind!=EXPR_NAME || strcmp(e->a->name,"isinstance") || lookup(c,"isinstance") || e->count!=2 || e->items[0]->kind!=EXPR_NAME || e->items[0]->akind || e->items[1]->akind) return 0;
    AClass *kc=class_expr(c,e->items[1]); if(!kc) return 0;
    ASym *s=lookup(c,e->items[0]->name); if(!s || s->kind!=AS_VAR) return 0;
    Ty *t=ty_find(((AVar*)s->p)->ty);
    if(t->k==TY_VAR){ pending(c,e->line,"the variable isinstance() tests"); return -1; }
    if(t->k!=TY_OBJ || t->cls==kc || !aot_subclass(kc,t->cls)) return 0;
    *name=e->items[0]->name; *k=kc; return 1;
}
static int stores_name(Ck *c, Stmt **b, int n, const char *name){
    Names nb={0}; names_bound(c,b,n,&nb,NULL);
    int r=0; for(int i=0;i<nb.n;i++) if(!strcmp(nb.v[i],name)) r=1;
    free(nb.v); return r;
}
/* try: ... except* T [as e]: ...: one except BaseException whose handler runs the clauses on the
   parts of the group they match (__mpy_EGState of the ExceptionGroup prelude), then raises what is left */
static void desugar_trystar(Ck *c, Stmt *s){
    AotUnit *u=c->mod->unit; int line=s->line, k=++c->nhidden;
    char ex[48], st[48], m[48], x[48];
    snprintf(ex,sizeof ex,"__mpy_eg%d_exc",k); snprintf(st,sizeof st,"__mpy_eg%d_state",k); snprintf(m,sizeof m,"__mpy_eg%d_match",k); snprintf(x,sizeof x,"__mpy_eg%d_raised",k);
    hidden_reg(c,st); hidden_reg(c,m);
    Stmt *hs[32]; int nh=0; Stmt *els=NULL, *fin=NULL;
    for(int i=0;i<s->orelse_count;i++){ Stmt *b=s->orelse[i];
        if(b->block_tag==1){ if(nh==32) err(c,b->line,"too many except* clauses"); if(!b->expr) err(c,b->line,"expected one or more exception types"); hs[nh++]=b; }
        else if(b->block_tag==2) els=b; else if(b->block_tag==3) fin=b; }
    Txt t={0}; for(int i=1;i<line;i++) tx(&t,"\n");
    tx(&t,"try:\n    pass\nexcept BaseException as %s:\n    %s = __mpy_EGState(%s)\n",ex,st,ex);
    for(int i=0;i<nh;i++){ char nm[64]; if(hs[i]->name) snprintf(nm,sizeof nm,"%s",hs[i]->name); else snprintf(nm,sizeof nm,"__mpy_eg%d_group%d",k,i);
        tx(&t,"    %s = %s.match(lambda __mpy_x: isinstance(__mpy_x, 0))\n    if %s is not None:\n        try:\n            raise %s\n        except BaseExceptionGroup as %s:\n"
              "            try:\n                pass\n            except BaseException as %s:\n                %s.handler_raised(%s)\n",m,st,m,m,nm,x,st,x); }
    tx(&t,"    %s.end()\n",st);
    Stmt *blk=py_front_stmts(u->path,t.s,line); free(t.s);
    if(!blk) err(c,line,"internal error: except*");
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
static void eg_class_arg(Ck *c, Expr *e){
    Expr *f=e->a;
    if(f->kind!=EXPR_ATTRIBUTE || (strcmp(f->name,"split") && strcmp(f->name,"subgroup")) || e->count!=1 || e->items[0]->akind) return;
    Expr *a=e->items[0];
    int cls= class_expr(c,a)!=NULL || (a->kind==EXPR_NAME && is_builtin_type_name(a->name));
    if(a->kind==EXPR_TUPLE){ cls=a->count>0; for(int i=0;i<a->count;i++) if(!class_expr(c,a->items[i])) cls=0; }
    if(!cls) return;
    Ty *o=ty_find(ck_expr(c,f->a)); ASym *g=symtab_find(&builtin_syms,"BaseExceptionGroup");
    if(o->k!=TY_OBJ || !g || g->kind!=AS_CLASS || !aot_subclass(o->cls,(AClass*)g->p)) return;
    Expr *call=xnew(EXPR_CALL,e->line); call->a=name_expr("isinstance",e->line); xpush(call,name_expr("__mpy_x",e->line)); xpush(call,a);
    Expr *lam=xnew(EXPR_LAMBDA,e->line); lam->eparams=MPY_NEW_ARR(char*,1); lam->eparams[0]="__mpy_x"; lam->neparam=1; lam->edefaults=MPY_NEW_ARR(Expr*,1);
    lam->estar=lam->edstar=-1; lam->ekwonly=1; lam->a=call;
    e->items[0]=lam;
}
static void ck_class_attrs(Ck *c, AClass *cls);
static void ck_stmt(Ck *c, Stmt *s){
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
            if(pr){ for(int i=0;i<pr->n;i++){ Ty *t=ck_expr(c,pr->args[i]); no_void(c,t,s->line); if(pr->star[i]) elem_of(c,t,s->line,"the *argument"); }
                if(pr->sep) expect(c,TY_STR_T,ck_expr(c,pr->sep),s->line,"sep");
                if(pr->end) expect(c,TY_STR_T,ck_expr(c,pr->end),s->line,"end");
                return; }
            if(aot_is_annotation_only(u,s)){ ck_assign(c,s); return; }
            ck_expr(c,aot_expr(u,&s->expr));
            return; }
        case STMT_ASSIGN: ck_assign(c,s); return;
        case STMT_IF: case STMT_WHILE:
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
            ck_stmts(c,s->body,s->body_count); ck_stmts(c,s->orelse,s->orelse_count);
            return;
        case STMT_FOR:{
            if(s->param_count<1||s->param_count>16) err(c,s->line,"for loops take 1 to 16 variables in compiled code");
            if(s->is_async){ Ty *it=ty_find(ck_expr(c,aot_expr(u,&s->expr)));
                if(it->k==TY_VAR){ pending(c,s->line,"the async iterable"); return; }
                if(it->k==TY_OBJ){ desugar_async_for(c,s); ck_stmt(c,s); return; } }
            Ty *ts[16]; iter_types(c,aot_expr(u,&s->expr),s->param_count,ts,s->line);
            async_iter_ok(c,aot_expr(u,&s->expr),s->is_async,s->line);
            for(int i=0;i<s->param_count;i++){ Expr nm; memset(&nm,0,sizeof nm); nm.kind=EXPR_NAME; nm.name=s->params[i]; ck_store(c,&nm,ts[i],s->line); }
            ck_stmts(c,s->body,s->body_count); ck_stmts(c,s->orelse,s->orelse_count);
            return; }
        case STMT_RETURN:
            if(!is_func(c->fn)) err(c,s->line,"return outside a function");
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
                    if(!x || x->kind!=AS_VAR) err(c,s->line,"cannot delete '%s'",t->name);
                    AVar *v=(AVar*)x->p, *r=var_root(v);
                    if(v->src || r->captured || r->cell) err(c,s->line,"del of '%s', which a nested function uses, is not supported in compiled code",t->name);
                    if(!r->bound){ char h[300]; snprintf(h,sizeof h," %s.bound",r->name);
                        r->bound= r->global ? new_global(c,r->mod,h) : hidden_local(r->owner,h,TY_BOOL_T);
                        r->bound->ty=ty_copy(TY_BOOL_T); }
                    xinfo(t)->kind=X_VAR; xinfo(t)->var=r; continue; }
                if(t->kind==EXPR_SLICE){                        /* del xs[a:b:c] */
                    Ty *o=ty_find(ck_expr(c,t->a)); Expr *bounds[3]={t->b,t->c,t->d};
                    for(int k=0;k<3;k++) if(bounds[k] && bounds[k]->kind!=EXPR_NONE) expect(c,TY_INT_T,ck_expr(c,bounds[k]),s->line,"a slice bound");
                    if(o->k==TY_VAR){ pending(c,s->line,"the list"); continue; }
                    if(o->k!=TY_LIST || o->tup) err(c,s->line,"'%s' object does not support item deletion",ty_name(o));
                    xinfo(t)->ty=o; continue; }
                if(t->kind==EXPR_ATTRIBUTE) err(c,s->line,"del obj.attr is not supported in compiled code: an object's fields are fixed");
                if(t->kind!=EXPR_INDEX) err(c,s->line,"cannot delete this in compiled code");
                Ty *o=ty_find(ck_expr(c,t->a)), *k=ck_expr(c,t->b);
                if(o->k==TY_VAR){ pending(c,s->line,"the container"); continue; }
                if(o->k==TY_LIST) expect(c,TY_INT_T,k,s->line,"a list index");
                else if(o->k==TY_DICT) expect(c,ty_dkey(o),k,s->line,"a dictionary key");
                else err(c,s->line,"cannot delete items of %s",ty_name(o));
                xinfo(t)->ty=o; }
            return; }
        case STMT_ASSERT:
            no_void(c,ck_cond(c,aot_expr(u,&s->expr)),s->line);
            if(s->expr2) no_void(c,ck_expr(c,aot_expr(u,&s->expr2)),s->line);
            return;
        case STMT_RAISE:{
            if(!s->expr){ if(!c->in_except) err(c,s->line,"bare raise outside an except clause"); return; }
            Expr *e=aot_expr(u,&s->expr);
            if(e->kind==EXPR_NAME){                         /* raise ValueError */
                ASym *x=lookup(c,e->name);
                if(x && x->kind==AS_CLASS){ AClass *cls=(AClass*)x->p;
                    if(!aot_is_exception(cls)) err(c,s->line,"%s is not an exception class",cls->name);
                    xinfo(e)->kind=X_CTOR; xinfo(e)->cls=cls; xinfo(e)->ty=ty_new(TY_OBJ,NULL,cls); ck_raise_cause(c,s); return; }
            }
            Ty *t=ty_find(ck_expr(c,e)); no_void(c,t,s->line);
            if(t->k!=TY_OBJ || !aot_is_exception(t->cls)) err(c,s->line,"raise needs an exception (a subclass of Exception), not %s",ty_name(t));
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
                if(x && x->kind==AS_FUNC) ck_def_defaults(c,(AFunc*)x->p,s->line); }
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
                else if(!x) err(c,s->line,"internal error: nested def without a variable");
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
            if(!g->is_gen) err(c,s->line,"'yield' outside a function");
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
            if(!is_func(c->fn) || !c->fn->outer) err(c,s->line,"nonlocal outside a nested function");
            for(int k=0;k<s->param_count;k++){ ASym *x=lookup(c,s->params[k]);
                if(!x || x->kind!=AS_VAR || !((AVar*)x->p)->src) err(c,s->line,"no binding for nonlocal '%s' in an enclosing function",s->params[k]); }
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
                        if(!cls||!aot_is_exception(cls)) err(c,b->line,"except needs exception classes");
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
                ck_stmts(c,b->body,b->body_count);
                c->in_except--;
                c->ncscope=save;
            }
            return; }
        default: err(c,s->line,"'%s' is not supported in compiled code",s->name?s->name:stmt_kind_name(s->kind));
    }
}
/* if not isinstance(x, C): return / continue / break / raise -> x is a C after it (the if's else
   keeps x as a C in a hidden variable) */
static int exits(Stmt **b, int n){ if(!n) return 0; Stmt *l=b[n-1]; return l->kind==STMT_RETURN||l->kind==STMT_RAISE||l->kind==STMT_CONTINUE||l->kind==STMT_BREAK; }
static void ck_stmts_top(Ck *c, Stmt **b, int n, AFunc *top){
    int save=c->ncscope; AotUnit *u=c->mod->unit;
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(top) top->top_index=i;
        ck_stmt(c,s);
        const char *nm; AClass *k; Expr *e;
        if(s->kind!=STMT_IF || !(e=aot_expr(u,&s->expr)) || e->kind!=EXPR_UNARY || e->op!=T_NOT || !exits(s->body,s->body_count)) continue;
        if(s->orelse_count && !(s->name2 && !strncmp(s->name2,".guard",6))) continue;
        if(isinstance_test(c,e->a,&nm,&k,0)<=0 || stores_name(c,b+i+1,n-i-1,nm)) continue;
        if(!s->name2){ char h[64]; named_hidden(c,"guard",h,sizeof h); s->name2=xstrdup2(h);
            stmt_add_orelse(s,st_assign(nx(s->name2,s->line),ex_call("\001downcast",nx(k->symname,s->line),nx(nm,s->line),s->line),s->line));
            ck_stmt(c,s->orelse[0]); }
        cs_push(c,nm,(AVar*)lookup(c,s->name2)->p,s->line);
    }
    c->ncscope=save;
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
static int falls_through(Stmt **b, int n){
    if(!n) return 1;
    Stmt *s=b[n-1];
    switch(s->kind){
        case STMT_RETURN: case STMT_RAISE: return 0;
        case STMT_IF: return falls_through(s->body,s->body_count) || !s->orelse_count || falls_through(s->orelse,s->orelse_count);
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
static void ck_body(Ck *c, AFunc *f){
    ck_stmts_top(c,f->body,f->nbody,f);
    if(f->def && f->returns_value && !f->is_gen && !f->is_abstract && falls_through(f->body,f->nbody)){ Ty *v=ty_var(); v->opt=1; unify(c,f->ret,v); }   /* off the end: None */
}

/* ---------------------------------------------------------------- whole program */

/* the values of a class's attributes, in order: each sees the ones before by name */
static void ck_class_attrs(Ck *c, AClass *cls){
    int save=c->ncscope;
    for(int k=0;k<cls->nfields;k++){ AField *fd=cls->fields[k];
        if(fd->init){ Ty *t=ck_expr_want(c,fd->init,fd->ty); no_void(c,t,fd->init->line); char what[160]; snprintf(what,sizeof what,"field %s.%s",cls->name,fd->name); expect(c,fd->ty,t,fd->init->line,what); }
        if(fd->cvar) cs_push(c,fd->name,fd->cvar,fd->init?fd->init->line:1); }
    c->ncscope=save;
}
static void check_all(Ck *c){
    AProg *p=c->p;
    for(int i=0;i<p->nclasses;i++){ AClass *cls=p->classes[i];           /* class attributes, in the module scope */
        if(cls->encl) continue;                                     /* (a function's: where its class statement is) */
        c->mod=cls->mod; c->fn=cls->mod->body; ck_class_attrs(c,cls);
    }
    for(int i=0;i<p->nfuncs;i++){ AFunc *f=p->funcs[i]; c->fn=f; c->mod=f->mod;
        if(f->outer || f->unused) continue;                     /* nested functions are checked where they are defined */
        if(f->cls && f->cls->mixin && !f->cls->constructed) continue;   /* (a mixin's own methods: its copies are checked) */
        if(c->strict && f->def && !f->cls && !f->ncalls){         /* a library function this program never calls */
            int unknown=0; for(int k=0;k<f->nparams;k++) if(!ty_known(f->params[k]->ty)) unknown=1;
            if(unknown){ f->unused=1; continue; }
        }
        ck_body(c,f); }
}

/* Element types nothing constrains (e.g. a list that stays empty) default to int. */
static Ty *default_of(Ty *v, Ty *t){ if(!v->opt) return t; Ty *n=ty_copy(t); n->opt=1; return n; }   /* (None alone: Optional[...]) */
static void default_elems(Ty *t){
    t=ty_find(t);
    if(t->k==TY_DICT && t->key){ Ty *k=ty_find(t->key); if(k->k==TY_VAR) k->link=default_of(k,TY_STR_T); else default_elems(k); }
    if(t->k==TY_LIST||t->k==TY_DICT||t->k==TY_SET||t->k==TY_GEN){ Ty *e=ty_find(t->elem); if(e->k==TY_VAR) e->link=default_of(e,TY_INT_T); else default_elems(e); }
    if(t->k==TY_TUPLE) for(int i=0;i<t->nelems;i++) default_elems(t->elems[i]);
    if(t->k==TY_GEN && t->nelems==2){ Ty *s=ty_find(t->elems[0]); if(s->k==TY_VAR) s->link=default_of(s,TY_INT_T); else default_elems(s);   /* nothing sent: int */
        Ty *r=ty_find(t->elems[1]); if(r->k==TY_VAR) r->link=TY_VOID_T; }
}
static void must_know(Ck *c, Ty *t, int line, const char *what, const char *name){
    { Ty *v=ty_find(t); if(v->k==TY_VAR && v->opt) v->link=default_of(v,TY_INT_T); }   /* only ever None */
    default_elems(t);
    if(!ty_known(t)) err(c,line,"cannot infer the type of %s '%s'; add a type annotation",what,name);
}

static void layout_class(AClass *cls){
    if(cls->size) return;
    int off=12;                                               /* refcount, destroy, vtable */
    if(cls->base){ layout_class(cls->base); off=cls->base->size; }
    for(int i=0;i<cls->nfields;i++){ AField *fd=cls->fields[i];
        if(fd->over){ fd->offset=aot_field_root(fd)->offset; continue; }
        fd->offset=off; off+=ty_size(fd->ty); }
    for(int i=0;i<cls->nfields;i++){ AField *fd=cls->fields[i];      /* obj.name: its own value once set, the class's until then */
        if(!fd->over && fd->cvar && fd->inst_set && !fd->overridden){ fd->setoff=off; off+=4; } }
    cls->size=off;
    int n=cls->base?cls->base->nvt:0;
    cls->vt=MPY_NEW_ARR(AFunc*,n+cls->nmethods+1);
    for(int i=0;i<n;i++) cls->vt[i]=cls->base->vt[i];
    for(int i=0;i<cls->nmethods;i++){ AFunc *m=cls->methods[i];
        if(m->is_static) continue;
        int slot=-1; for(int k=0;k<n;k++) if(!strcmp(cls->vt[k]->name,m->name)) slot=k;
        if(slot>=0){ cls->vt[slot]->overridden=1; m->vslot=slot; cls->vt[slot]=m; }
        else { m->vslot=n; cls->vt[n++]=m; }
    }
    cls->nvt=n;
}
/* A method overridden anywhere below needs a virtual call. */
static void mark_overrides(AProg *p){
    for(int i=0;i<p->nclasses;i++){ AClass *cls=p->classes[i];
        for(int k=0;k<cls->nmethods;k++){ AFunc *m=cls->methods[k]; if(m->is_static) continue;
            for(AClass *b=cls->base;b;b=b->base){ AFunc *bm=NULL; for(int j=0;j<b->nmethods;j++) if(!strcmp(b->methods[j]->name,m->name)) bm=b->methods[j];
                if(bm) bm->overridden=1; } } }
}

AProg *aot_check(AotUnit **units, int nunits, AotTarget target){
    if(!TY_INT_T){ TY_INT_T=ty_new(TY_INT,NULL,NULL); TY_BOOL_T=ty_new(TY_BOOL,NULL,NULL); TY_FLOAT_T=ty_new(TY_FLOAT,NULL,NULL);
        TY_STR_T=ty_new(TY_STR,NULL,NULL); TY_VOID_T=ty_new(TY_VOID,NULL,NULL); TY_BUF_T=ty_new(TY_BUF,NULL,NULL); TY_BYTES_T=ty_new(TY_BYTES,NULL,NULL); TY_TYPE_T=ty_new(TY_TYPE,NULL,NULL); }
    AProg *p=MPY_NEW0(AProg); p->target=target;
    Ck ck; memset(&ck,0,sizeof ck); ck.p=p; Ck *c=&ck;
    if(setjmp(ck.fail)) return NULL;
    p->nmods=nunits; p->mods=MPY_NEW_ARR(AModule*,nunits);
    for(int i=0;i<nunits;i++){ AModule *m=MPY_NEW0(AModule); m->unit=units[i]; m->name=units[i]->name; m->index=i; p->mods[i]=m; }
    make_builtin_exceptions(p,p->mods[0]);
    for(int i=0;i<nunits;i++) if(units[i]->ast) collect_module(c,p->mods[i]);
    for(int i=0;i<nunits;i++) if(units[i]->ast && !strcmp(units[i]->name,"__mpy_eg")){    /* the ExceptionGroup prelude's classes: built-in names */
        static const char *const eg[]={"BaseExceptionGroup","ExceptionGroup","__mpy_EGState",NULL};
        for(int k=0;eg[k];k++){ ASym *s=module_sym(p->mods[i],eg[k]); if(s) symtab_add(&builtin_syms,eg[k],s->kind,s->p); } }
    for(int i=0;i<nunits;i++) if(units[i]->ast && !strcmp(units[i]->name,"__mpy_cmpkey")){ ASym *s=module_sym(p->mods[i],"__mpy_CmpKey"); if(s) symtab_add(&builtin_syms,"__mpy_CmpKey",s->kind,s->p); }
    for(int i=0;i<nunits;i++) if(units[i]->ast && !strcmp(units[i]->name,"cmath") && units[i]->path && !strcmp(units[i]->path,"<cmath>")){   /* complex: a built-in name */
        ASym *s=module_sym(p->mods[i],"complex"); if(s){ symtab_add(&builtin_syms,"complex",s->kind,s->p); complex_class=(AClass*)s->p; }
        s=module_sym(p->mods[i],"__mpy_complex_parse"); if(s) symtab_add(&builtin_syms,"__mpy_complex_parse",s->kind,s->p); }
    for(int i=nunits-1;i>=0;i--) if(units[i]->ast) link_imports(c,p->mods[i]);
    for(int i=0;i<nunits;i++) if(units[i]->ast) collect_functions(c,p->mods[i]);
    for(int i=0;i<p->nclasses;i++){ AClass *m=p->classes[i]; int vb=0, lay=0;      /* a class only used as a mixin: its methods live on as copies */
        for(int j=0;j<p->nclasses;j++){ AClass *k=p->classes[j]; if(k->base==m) lay=1; for(int v=0;v<k->nvbases;v++) if(k->vbases[v]==m) vb=1; }
        if(vb && !lay) m->mixin=1; }
    for(int pass=0;pass<100;pass++){
        c->changed=0; c->pass_first=ty_ids+1;
        check_all(c);
        if(!c->changed) break;
    }
    c->strict=1; c->pass_first=ty_ids+1;
    check_all(c);
    for(int i=0;i<p->nfuncs;i++){ AFunc *f=p->funcs[i]; if(f->cls && f->cls->mixin && !f->cls->constructed) f->unused=1; }
    for(int i=0;i<c->nseen;i++) default_elems(c->seen[i]);
    free(c->seen);
    for(int i=0;i<p->nglobals;i++){ AVar *v=p->globals[i]; c->mod=v->mod; must_know(c,v->ty,1,"variable",v->name); }
    for(int i=0;i<p->nfuncs;i++){ AFunc *f=p->funcs[i];               /* inside a function nothing uses */
        for(AFunc *o=f->outer;o;o=o->outer) if(o->unused) f->unused=1; }
    for(int i=0;i<p->nfuncs;i++){ AFunc *f=p->funcs[i]; c->mod=f->mod; if(f->unused) continue;
        for(int k=0;k<f->nvars;k++) must_know(c,f->vars[k]->ty,f->line,k<f->nparams?"parameter":"variable",f->vars[k]->name);
        if(is_func(f)){ { Ty *v=ty_find(f->ret); if(v->k==TY_VAR && v->opt) v->link=default_of(v,TY_INT_T); }
            default_elems(f->ret); if(!ty_known(f->ret)) err(c,f->line,"cannot infer the return type of %s(); annotate it (-> type)",f->name); } }
    for(int i=0;i<p->nfuncs;i++){ AFunc *f=p->funcs[i];               /* closures: copy the value, or share a cell */
        for(int k=0;k<f->nvars;k++){ AVar *v=f->vars[k]; if(!v->captured) continue;
            int param=k<f->nparams;
            int byval=!v->nonlocal_set && ((param && v->nbind==0) || (!param && v->nbind==1 && v->bind_top>=0 && v->bind_top<v->first_use_top));
            v->cell=!byval; } }
    for(int i=0;i<p->nclasses;i++){ AClass *cls=p->classes[i]; c->mod=cls->mod; if(cls->builtin) continue;
        for(int k=0;k<cls->nfields;k++) must_know(c,cls->fields[k]->ty,cls->def->line,"field",cls->fields[k]->name); }
    for(int i=0;i<p->nclasses;i++) layout_class(p->classes[i]);
    mark_overrides(p);
    return p;
}
