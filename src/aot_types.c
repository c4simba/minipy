/* ========================= Typed compiler: model + type checker =========================
   Builds the program model (modules, functions, classes, variables) from the
   ASTs and gives every variable, parameter, return value, field and container
   element exactly one static type. Types come from annotations
   (`x: int`, `def f(a: str) -> list[int]`, class-body fields) or are inferred
   by unification from the code: `i = 1` makes i an int, `grid = []` followed
   by `grid.append(row)` makes grid a list of row's type. Assigning a value of
   another type is a compile error. `None` is the zero value of whatever type
   its context has.

   Inference runs whole-program passes until nothing new is learned, then a
   final strict pass reports whatever is still unknown. */

#include "aot_model.h"
#include <stdarg.h>
#include "frontparser.h"
#include "compiler.h"
#include "lexer.h"

/* ---------------------------------------------------------------- types */

static int ty_ids;
Ty *TY_INT_T, *TY_BOOL_T, *TY_FLOAT_T, *TY_STR_T, *TY_VOID_T, *TY_BUF_T;
static Ty *TY_OSTR_T;       /* Optional[str]: a str (None is "") that JSON writes as null when empty */

Ty *ty_new(TyKind k, Ty *elem, AClass *cls){ Ty *t=MPY_NEW0(Ty); t->k=k; t->elem=elem; t->cls=cls; t->id=++ty_ids; return t; }
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
/* element-like parts: list/dict/set/task/generator elements, a function's result */
static int has_elem(Ty *t){ return t->k==TY_LIST||t->k==TY_DICT||t->k==TY_SET||t->k==TY_TASK||t->k==TY_GEN||t->k==TY_FUNC; }
int ty_known(Ty *t){
    t=ty_find(t);
    if(!t || t->k==TY_VAR) return 0;
    if(t->k==TY_DICT && !ty_known(ty_dkey(t))) return 0;
    if(t->k==TY_TUPLE||t->k==TY_FUNC){ for(int i=0;i<t->nelems;i++) if(!ty_known(t->elems[i])) return 0; }
    if(has_elem(t)) return ty_known(t->elem);
    return 1;
}
int ty_is_ptr(Ty *t){ t=ty_find(t); return t->k==TY_STR||t->k==TY_LIST||t->k==TY_DICT||t->k==TY_SET||t->k==TY_OBJ||t->k==TY_BUF||t->k==TY_TASK||t->k==TY_TUPLE||t->k==TY_FILE||t->k==TY_FUNC||t->k==TY_GEN; }
int ty_size(Ty *t){ return ty_find(t)->k==TY_FLOAT ? 8 : 4; }
static int names_eq(Ty *a, Ty *b){
    if(!a->names || !b->names) return a->names==b->names;
    for(int i=0;i<a->nelems;i++) if(strcmp(a->names[i],b->names[i])) return 0;
    return 1;
}
int ty_same(Ty *a, Ty *b){
    a=ty_find(a); b=ty_find(b);
    if(a==b) return 1;
    if(a->k!=b->k) return 0;
    if(a->k==TY_DICT && !ty_same(ty_dkey(a),ty_dkey(b))) return 0;
    if((a->k==TY_LIST||a->k==TY_FUNC) && a->tup!=b->tup) return 0;
    if(a->k==TY_TUPLE||a->k==TY_FUNC){ if(a->nelems!=b->nelems) return 0; for(int i=0;i<a->nelems;i++) if(!ty_same(a->elems[i],b->elems[i])) return 0; if(a->k==TY_TUPLE) return names_eq(a,b); }
    if(has_elem(a)) return ty_same(a->elem,b->elem);
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
        case TY_BUF: return "buffer";
        case TY_FILE: return "file";
        case TY_LIST: snprintf(b,160,t->tup?"tuple[%s, ...]":"list[%s]",ty_name(t->elem)); return b;
        case TY_GEN: snprintf(b,160,"Iterator[%s]",ty_name(t->elem)); return b;
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
AFunc *aot_find_method(AClass *c, const char *name){ for(;c;c=c->base) for(int i=0;i<c->nmethods;i++) if(!strcmp(c->methods[i]->name,name)) return c->methods[i]; return NULL; }
int aot_subclass(AClass *c, AClass *base){ for(;c;c=c->base) if(c==base) return 1; return 0; }

/* ---------------------------------------------------------------- parsing helpers */

static Expr *xnew(ExprKind k, int line){ Expr *e=MPY_NEW0(Expr); e->kind=k; e->line=line; return e; }
static void xpush(Expr *e, Expr *item){
    if(e->count==e->cap){ e->cap=e->cap?e->cap*2:4; e->items=(Expr**)xrealloc(e->items,sizeof(Expr*)*(size_t)e->cap); }
    e->items[e->count++]=item;
}
static Parser parser_at(AotUnit *u, int pos){ Parser p; memset(&p,0,sizeof p); p.tv=u->tv; p.pos=pos; return p; }

MPY_NORETURN static void parse_fail(AotUnit *u, int line, const char *msg){
    fprintf(stderr,"%s:%d: error: %s\n",u->path?u->path:"<source>",line,msg); exit(1);
}

/* expr ("," expr)* up to `end` (or a token that cannot continue a list) */
static Expr *parse_list_until(AotUnit *u, Parser *p, int end){
    Expr *e=parse_expression(p);
    if(p->pos<end && peek(p)->kind==T_COMMA){
        Expr *t=xnew(EXPR_TUPLE,e->line); xpush(t,e);
        while(p->pos<end && match(p,T_COMMA)){
            TokKind k=peek(p)->kind;
            if(p->pos>=end || k==T_NEWLINE||k==T_ASSIGN||k==T_COLON||is_assign_op(k)) break;
            xpush(t,parse_expression(p));
        }
        if(t->count>1) e=t;
    }
    (void)u;
    return e;
}
Expr *aot_expr(AotUnit *u, Expr **slot){
    Expr *r=*slot;
    if(!r || r->kind!=EXPR_TOKEN_RANGE) return r;
    Parser p=parser_at(u,r->start);
    Expr *e=parse_list_until(u,&p,r->end);
    if(p.pos<r->end && peek(&p)->kind!=T_FROM)          /* `raise X from Y`: the cause is ignored */
        parse_fail(u,peek(&p)->line,"unexpected token in expression");
    *slot=e; return e;
}

int aot_is_annotation_only(AotUnit *u, Stmt *s){
    return s->kind==STMT_EXPR && u->tv.v[s->start].kind==T_NAME && u->tv.v[s->start+1].kind==T_COLON;
}

/* targets "=" ... value | target augop value | target ":" annotation ["=" value] */
AAssign *aot_assign(AotUnit *u, Stmt *s){
    if(s->aux) return (AAssign*)s->aux;
    AAssign *a=MPY_NEW0(AAssign); a->ann_start=a->ann_end=-1;
    Parser p=parser_at(u,s->start);
    int end=s->end;
    for(;;){
        Expr *lhs=parse_list_until(u,&p,end);
        TokKind k=peek(&p)->kind;
        if(k==T_COLON){
            int depth=0; p.pos++; a->ann_start=p.pos;
            while(peek(&p)->kind!=T_EOF && !(depth==0 && (peek(&p)->kind==T_ASSIGN||peek(&p)->kind==T_NEWLINE))){
                TokKind t=peek(&p)->kind; if(t==T_LP||t==T_LB||t==T_LC) depth++; else if(t==T_RP||t==T_RB||t==T_RC) depth--; p.pos++; }
            a->ann_end=p.pos;
            a->target[a->ntarget++]=lhs;
            if(match(&p,T_ASSIGN)) a->value=parse_list_until(u,&p,end);
            break;
        }
        if(k==T_ASSIGN){ if(a->ntarget==16) parse_fail(u,s->line,"too many assignment targets"); a->target[a->ntarget++]=lhs; p.pos++; continue; }
        if(k!=T_NEWLINE && is_assign_op(k)){ a->target[a->ntarget++]=lhs; a->aug=k; p.pos++; a->value=parse_list_until(u,&p,end); break; }
        if(a->ntarget==0) parse_fail(u,s->line,"invalid assignment");
        a->value=lhs; break;
    }
    if(peek(&p)->kind!=T_NEWLINE) parse_fail(u,peek(&p)->line,"unexpected token in assignment");
    s->aux=a; return a;
}
AWith *aot_with(AotUnit *u, Stmt *s){
    if(s->aux) return (AWith*)s->aux;
    AWith *w=MPY_NEW0(AWith);
    Parser p=parser_at(u,s->expr->start);
    do{ if(w->n==8) parse_fail(u,s->line,"too many with items");
        w->e[w->n]=parse_expression(&p);
        if(match(&p,T_AS)) w->as[w->n]=need(&p,T_NAME,"with target")->text;
        w->n++; }while(match(&p,T_COMMA));
    s->aux=w; return w;
}
ADel *aot_del(AotUnit *u, Stmt *s){
    if(s->aux) return (ADel*)s->aux;
    ADel *d=MPY_NEW0(ADel);
    Parser p=parser_at(u,s->expr->start);
    do{ if(d->n==16) parse_fail(u,s->line,"too many del targets"); d->t[d->n++]=parse_expression(&p); }while(match(&p,T_COMMA));
    s->aux=d; return d;
}
/* print(a, b, ..., sep=..., end=...) */
APrint *aot_print(AotUnit *u, Stmt *s){
    if(s->kind!=STMT_EXPR || u->tv.v[s->start].kind!=T_PRINT) return NULL;
    if(s->aux) return (APrint*)s->aux;
    APrint *pr=MPY_NEW0(APrint);
    Parser p=parser_at(u,s->start);
    need(&p,T_PRINT,"print"); need(&p,T_LP,"(");
    while(peek(&p)->kind!=T_RP){
        if(peek(&p)->kind==T_NAME && p.tv.v[p.pos+1].kind==T_ASSIGN){
            const char *kw=peek(&p)->text; p.pos+=2;
            Expr *v=parse_expression(&p);
            if(!strcmp(kw,"sep")) pr->sep=v; else if(!strcmp(kw,"end")) pr->end=v;
            else parse_fail(u,s->line,"print() takes only sep= and end= keywords");
        } else {
            if(pr->n==32) parse_fail(u,s->line,"too many print arguments");
            int star=match(&p,T_STAR);
            pr->star[pr->n]=(char)star;
            pr->args[pr->n++]=parse_expression(&p);
        }
        if(!match(&p,T_COMMA)) break;
    }
    need(&p,T_RP,")");
    s->aux=pr; return pr;
}

char *aot_from_import_module(AotUnit *u, Stmt *s){
    Parser p=parser_at(u,s->start);
    need(&p,T_FROM,"from");
    char buf[256]; int bl=snprintf(buf,sizeof buf,"%s",need(&p,T_NAME,"module name")->text);
    while(match(&p,T_DOT)){ Tok *n=need(&p,T_NAME,"module name"); if(bl<250) bl+=snprintf(buf+bl,sizeof(buf)-(size_t)bl,".%s",n->text); }
    return xstrdup2(buf);
}

/* ---------------------------------------------------------------- checker state */

typedef struct Ck {
    AProg *p;
    AModule *mod;           /* module being processed */
    AFunc *fn;              /* function being checked (module bodies included) */
    int changed;            /* some type known before this pass got bound */
    int pass_first;         /* first type id created in this pass */
    int strict;             /* final pass: anything still unknown is an error */
    struct { const char *name; ASym sym; AFunc *fn; } cscope[32];   /* comprehension / except / key variables in scope (of fn) */
    int ncscope;
    Expr *coro_ok;          /* the coroutine call being awaited / handed to asyncio */
    int ep_value;           /* checking an argument for a minipy.Endpoint: an async function may be a value */
    int in_except;          /* inside an except clause (bare raise) */
    Ty **seen; int nseen, cseen;     /* strict pass: every expression type (unconstrained elements -> int) */
    jmp_buf fail;
} Ck;

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
    if(c->ncscope==32) err(c,line,"nested too deeply");
    c->cscope[c->ncscope].name=name; c->cscope[c->ncscope].sym.name=(char*)name;
    c->cscope[c->ncscope].sym.kind=AS_VAR; c->cscope[c->ncscope].sym.p=v; c->cscope[c->ncscope].fn=c->fn; c->ncscope++;
}
/* a hidden variable of the current scope (comprehension / except / key variable) */
static AVar *new_global(Ck *c, AModule *m, const char *name);
static AVar *hidden_local(AFunc *f, const char *name, Ty *ty);
static AVar *scope_hidden(Ck *c, const char *name){ return is_func(c->fn) ? hidden_local(c->fn,name,NULL) : new_global(c,c->mod,name); }

static int occurs(Ty *v, Ty *t){
    t=ty_find(t); if(t==v) return 1;
    if(t->k==TY_TUPLE||t->k==TY_FUNC){ for(int i=0;i<t->nelems;i++) if(occurs(v,t->elems[i])) return 1; if(t->k==TY_TUPLE) return 0; }
    if(t->k==TY_DICT && t->key && occurs(v,t->key)) return 1;
    return t->elem ? occurs(v,t->elem) : 0;
}
static void bind(Ck *c, Ty *var, Ty *t){ var->link=t; if(var->id<c->pass_first) c->changed=1; }
static int unify(Ck *c, Ty *a, Ty *b){
    a=ty_find(a); b=ty_find(b);
    if(a==b) return 1;
    if(a->k==TY_VAR && b->k==TY_VAR){ if(a->id<b->id) bind(c,b,a); else bind(c,a,b); return 1; }   /* younger -> older */
    if(a->k==TY_VAR){ if(occurs(a,b)) return 0; bind(c,a,b); return 1; }
    if(b->k==TY_VAR){ if(occurs(b,a)) return 0; bind(c,b,a); return 1; }
    if(a->k!=b->k) return 0;
    if(a->k==TY_DICT && !unify(c,ty_dkey(a),ty_dkey(b))) return 0;
    if((a->k==TY_LIST||a->k==TY_FUNC) && a->tup!=b->tup) return 0;
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
static int assignable(Ck *c, Ty *dst, Ty *src){
    Ty *d=ty_find(dst), *s=ty_find(src);
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
static int is_var(Ty *t){ return ty_find(t)->k==TY_VAR; }

/* ---------------------------------------------------------------- type annotations */

static ASym *module_sym(AModule *m, const char *name){ return symtab_find(&m->syms,name); }

/* ---- built-in exception classes: BaseException (field _msg: str) and its subclasses ---- */
static SymTab builtin_syms;
static AClass *exc_root;
int aot_is_exception(AClass *c){ return exc_root && aot_subclass(c,exc_root); }
static ASym *global_sym(AModule *m, const char *name){ ASym *s=module_sym(m,name); return s?s:symtab_find(&builtin_syms,name); }
static AField *add_field(AClass *cls, const char *name, Ty *ty, Expr *init);
static void make_builtin_exceptions(AProg *p, AModule *m){
    static const char *tree[][2]={
        {"BaseException",NULL},{"Exception","BaseException"},{"ArithmeticError","Exception"},{"ZeroDivisionError","ArithmeticError"},
        {"LookupError","Exception"},{"IndexError","LookupError"},{"KeyError","LookupError"},{"ValueError","Exception"},
        {"TypeError","Exception"},{"AttributeError","Exception"},{"RuntimeError","Exception"},{"NotImplementedError","RuntimeError"},
        {"AssertionError","Exception"},{"OSError","Exception"},{"MemoryError","Exception"},{"NameError","Exception"},
        {"OverflowError","ArithmeticError"},{"StopIteration","Exception"},{"InvalidStateError","Exception"},
        {"FileNotFoundError","OSError"},{NULL,NULL}};
    memset(&builtin_syms,0,sizeof builtin_syms); exc_root=NULL;
    for(int i=0;tree[i][0];i++){
        AClass *cls=MPY_NEW0(AClass); cls->name=xstrdup2(tree[i][0]); cls->mod=m; cls->builtin=1; cls->id=p->nclasses;
        if(tree[i][1]) cls->base=(AClass*)symtab_find(&builtin_syms,tree[i][1])->p;
        else { exc_root=cls; add_field(cls,"_msg",TY_STR_T,NULL); }
        if(p->nclasses==p->ccap){ p->ccap=p->ccap?p->ccap*2:16; p->classes=(AClass**)xrealloc(p->classes,sizeof(AClass*)*(size_t)p->ccap); }
        p->classes[p->nclasses++]=cls;
        symtab_add(&builtin_syms,cls->name,AS_CLASS,cls);
    }
}
static AModule *find_module(AProg *p, const char *name){ for(int i=0;i<p->nmods;i++) if(!strcmp(p->mods[i]->name,name)) return p->mods[i]; return NULL; }

static Ty *optional(Ty *t){ return ty_find(t)->k==TY_STR ? TY_OSTR_T : t; }
/* minipy.Endpoint: Callable[[dict[str, str]], str] that any function converts
   to (an adapter made by the compiler, see ep_adapter) */
static Ty *endpoint_type(void){
    Ty *ps[1]; ps[0]=ty_dict(TY_STR_T,TY_STR_T);
    Ty *t=ty_func(ps,1,TY_STR_T); t->tup=4; return t;
}
static int is_endpoint(Ty *t){ t=ty_find(t); return t->k==TY_FUNC && (t->tup&4); }
static Ty *parse_type1(Ck *c, Tok *v, int *i, int end, int line);
/* type ('|' type)*: T | None is Optional[T] */
static Ty *parse_type(Ck *c, Tok *v, int *i, int end, int line){
    Ty *t=parse_type1(c,v,i,end,line);
    while(*i<end && v[*i].kind==T_PIPE){
        (*i)++;
        Ty *u=parse_type1(c,v,i,end,line);
        if(ty_find(u)->k==TY_VOID) t=optional(t);
        else if(ty_find(t)->k==TY_VOID) t=optional(u);
        else err(c,line,"only T | None is supported in compiled code: one type per value");
    }
    return t;
}
/* NAME | NAME '[' type (',' type)* ']' | module '.' NAME | None */
static Ty *parse_type1(Ck *c, Tok *v, int *i, int end, int line){
    if(*i<end && v[*i].kind==T_NONE){ (*i)++; return TY_VOID_T; }
    if(*i<end && v[*i].kind==T_STRING){                                 /* "Class" (forward reference) */
        TokVec tv=lex(v[(*i)++].text); int n=0, j=0;
        while(n<tv.n && tv.v[n].kind!=T_NEWLINE && tv.v[n].kind!=T_EOF) n++;
        Ty *t=parse_type(c,tv.v,&j,n,line);
        if(j!=n) err(c,line,"unexpected token in type annotation");
        return t;
    }
    if(*i>=end || v[*i].kind!=T_NAME) err(c,line,"expected a type");
    const char *name=v[(*i)++].text;
    AModule *m=c->mod;
    while(*i+1<end && v[*i].kind==T_DOT && v[*i+1].kind==T_NAME){       /* module.Class, typing.List */
        ASym *s=module_sym(m,name);
        if(s && s->kind==AS_SYS && !strcmp((const char*)s->p,"typing")){ name=v[*i+1].text; *i+=2; continue; }
        if(s && s->kind==AS_SYS && !strcmp((const char*)s->p,"minipy") && !strcmp(v[*i+1].text,"Endpoint")){ *i+=2; return endpoint_type(); }
        if(!s || s->kind!=AS_MODULE) err(c,line,"'%s' is not a module",name);
        m=(AModule*)s->p; name=v[*i+1].text; *i+=2;
    }
    if(!strcmp(name,"Callable")){                                       /* Callable[[A, B], R] */
        Ty *ps[16]; int np=0;
        if(*i>=end || v[*i].kind!=T_LB || *i+1>=end || v[*i+1].kind!=T_LB) err(c,line,"Callable needs its parameter and result types: Callable[[int, str], bool]");
        *i+=2;
        while(*i<end && v[*i].kind!=T_RB){ if(np==16) err(c,line,"too many parameters"); ps[np++]=parse_type(c,v,i,end,line);
            if(*i<end && v[*i].kind==T_COMMA) (*i)++; else break; }
        if(*i>=end || v[*i].kind!=T_RB) err(c,line,"expected ']' in Callable");
        (*i)++;
        if(*i>=end || v[*i].kind!=T_COMMA) err(c,line,"Callable needs a result type: Callable[[int], int]");
        (*i)++;
        Ty *r=parse_type(c,v,i,end,line);
        if(*i>=end || v[*i].kind!=T_RB) err(c,line,"expected ']' in Callable");
        (*i)++;
        return ty_func(ps,np,r);
    }
    Ty *args[16]={NULL}; int nargs=0, varlen=0;
    if(*i<end && v[*i].kind==T_LB){
        (*i)++;
        for(;;){ if(nargs==16) err(c,line,"too many type arguments");
            if(*i+2<end && v[*i].kind==T_DOT && v[*i+1].kind==T_DOT && v[*i+2].kind==T_DOT){ *i+=3; varlen=1; break; }   /* tuple[int, ...] */
            args[nargs++]=parse_type(c,v,i,end,line);
            if(*i<end && v[*i].kind==T_COMMA){ (*i)++; continue; } break; }
        if(*i>=end || v[*i].kind!=T_RB) err(c,line,"expected ']' in type");
        (*i)++;
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
    if(!strcmp(name,"Iterator")||!strcmp(name,"Iterable")||!strcmp(name,"Generator")){
        if(nargs<1) err(c,line,"%s needs the item type: %s[int]",name,name);
        if(nargs>1 && !strcmp(name,"Generator")){ for(int k=1;k<nargs;k++) if(ty_find(args[k])->k!=TY_VOID) err(c,line,"only Generator[T, None, None] is supported (no send() / return values)"); }
        else if(nargs>1) err(c,line,"%s takes one type argument",name);
        return ty_new(TY_GEN,args[0],NULL);
    }
    if(varlen){
        if(strcmp(name,"tuple")||nargs!=1) err(c,line,"`...` is only supported as tuple[T, ...]");
        Ty *t=ty_new(TY_LIST,args[0],NULL); t->tup=1; return t;
    }
    if(!strcmp(name,"int")) return TY_INT_T;
    if(!strcmp(name,"bool")) return TY_BOOL_T;
    if(!strcmp(name,"float")) return TY_FLOAT_T;
    if(!strcmp(name,"str")||!strcmp(name,"bytes")||!strcmp(name,"bytearray")) return TY_STR_T;   /* bytes: the same bytes */
    if(!strcmp(name,"buffer")) return TY_BUF_T;
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
        if(nargs==2){ TyKind kk=ty_find(args[0])->k; if(kk!=TY_STR&&kk!=TY_INT&&kk!=TY_BOOL&&kk!=TY_TUPLE&&kk!=TY_OBJ) err(c,line,"dictionary keys must be int, str, tuples or objects"); }
        return ty_dict(nargs?args[0]:ty_var(),nargs?args[1]:ty_var());
    }
    ASym *s=global_sym(m,name);
    if(s && s->kind==AS_CLASS){ if(nargs) err(c,line,"class %s takes no type arguments",name); return ty_new(TY_OBJ,NULL,(AClass*)s->p); }
    if(s && s->kind==AS_SYS && !strcmp((const char*)s->p,"minipy.Endpoint")) return endpoint_type();
    err(c,line,"unknown type '%s'",name);
}
static Ty *annotation_range(Ck *c, int start, int end, int line){
    Tok *v=c->mod->unit->tv.v; int i=start;
    Ty *t=parse_type(c,v,&i,end,line);
    if(i!=end) err(c,line,"unexpected token in type annotation");
    return t;
}
static Ty *annotation(Ck *c, Expr *range){ return range ? annotation_range(c,range->start,range->end,range->line) : NULL; }

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
static void names_expr(Expr *e, Names *out){          /* expressions bind nothing (comprehension variables are their own) */
    (void)e; (void)out;
}
/* Names assigned by statements (not descending into def/class bodies). */
static void names_bound(Ck *c, Stmt **b, int n, Names *out, Names *globals){
    AotUnit *u=c->mod->unit;
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        switch(s->kind){
            case STMT_ASSIGN:{ AAssign *a=aot_assign(u,s); for(int k=0;k<a->ntarget;k++){ names_target(a->target[k],out); names_expr(a->target[k],out); } names_expr(a->value,out); break; }
            case STMT_EXPR:{ APrint *pr=aot_print(u,s);
                if(pr){ for(int k=0;k<pr->n;k++) names_expr(pr->args[k],out); }
                else if(aot_is_annotation_only(u,s)) names_add(out,u->tv.v[s->start].text);
                else names_expr(aot_expr(u,&s->expr),out);
                break; }
            case STMT_FOR: for(int k=0;k<s->param_count;k++) names_add(out,s->params[k]);
                names_expr(aot_expr(u,&s->expr),out);
                names_bound(c,s->body,s->body_count,out,globals); names_bound(c,s->orelse,s->orelse_count,out,globals); break;
            case STMT_IF: case STMT_WHILE: names_expr(aot_expr(u,&s->expr),out);
                names_bound(c,s->body,s->body_count,out,globals); names_bound(c,s->orelse,s->orelse_count,out,globals); break;
            case STMT_WITH:{ AWith *w=aot_with(u,s); for(int k=0;k<w->n;k++){ names_expr(w->e[k],out); if(w->as[k]) names_add(out,w->as[k]); }
                names_bound(c,s->body,s->body_count,out,globals); break; }
            case STMT_TRY: case STMT_BLOCK: names_bound(c,s->body,s->body_count,out,globals); names_bound(c,s->orelse,s->orelse_count,out,globals); break;
            case STMT_RETURN: case STMT_RAISE: names_expr(aot_expr(u,&s->expr),out); break;
            case STMT_ASSERT: names_expr(aot_expr(u,&s->expr),out); names_expr(aot_expr(u,&s->expr2),out); break;
            case STMT_GLOBAL: case STMT_NONLOCAL: if(globals) for(int k=0;k<s->param_count;k++) names_add(globals,s->params[k]); break;
            case STMT_FUNCTION_DEF: names_add(out,s->name); break;
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
        if(s->kind==STMT_CLASS_DEF) err(c,s->line,"classes must be defined at module level in compiled code");
        if(s->kind==STMT_IMPORT||s->kind==STMT_FROM_IMPORT) err(c,s->line,"import must be at the top of the module");
        no_nested_defs(c,s->body,s->body_count,in_func); no_nested_defs(c,s->orelse,s->orelse_count,in_func);
    }
}
/* what decorator i of a def is: 0 a general one (name = d(function)),
   1 @staticmethod, 2 @property, 3 ignored (@wraps(f): only copies names) */
static int deco_kind(AotUnit *u, Stmt *def, int i){
    Expr *d=aot_expr(u,&def->decorator_exprs[i]);
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
            case STMT_IF: case STMT_WHILE: case STMT_TRY: case STMT_BLOCK:
                count_bindings(f,s->body,s->body_count,t,1); count_bindings(f,s->orelse,s->orelse_count,t,1); break;
            default: break;
        }
    }
}
static AFunc *new_func(Ck *c, AFunc *f, AModule *m, AClass *cls, Stmt *def, AFunc *outer);
/* nested defs of f's body (not of their own bodies) */
static void collect_nested(Ck *c, AFunc *f, Stmt **b, int n){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_FUNCTION_DEF){
            if(s->is_async) err(c,s->line,"nested async functions are not supported in compiled code");
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
    {   /* defaults belong to the last parameters before *args / **kwargs, and to keyword-only ones */
        int pos[16], np=0; for(int i=0;i<f->nparams;i++) if(i!=f->star && i!=f->dstar) pos[np++]=i;
        for(int i=0;i<def->default_count;i++){ int k=np-def->default_count+i; if(k>=0) f->defaults[pos[k]]=aot_expr(m->unit,&def->defaults[i]); }
    }
    f->returns_value=has_return_value(def->body,def->body_count);
    f->is_gen=has_yield(def->body,def->body_count);
    if(f->is_gen){
        if(def->is_async) err(c,def->line,"async generators are not supported in compiled code");
        if(f->returns_value) err(c,def->line,"%s(): `return value` in a generator is not supported in compiled code",f->name);
        if(def->returns){ f->ret=annotation(c,def->returns); if(ty_find(f->ret)->k!=TY_GEN) err(c,def->line,"generator %s() must be annotated -> Iterator[T] (or Generator[T, None, None])",f->name); }
        else f->ret=ty_new(TY_GEN,ty_var(),NULL);
        f->yield_ty=ty_find(f->ret)->elem;
    }
    else if(def->returns){ f->ret=annotation(c,def->returns); if(ty_find(f->ret)->k==TY_VOID && f->returns_value) err(c,def->line,"%s() is annotated -> None but returns a value",f->name); }
    else f->ret=f->returns_value ? ty_var() : TY_VOID_T;
    if(cls && !strcmp(f->name,"__init__") && ty_find(f->ret)->k!=TY_VOID) err(c,def->line,"__init__ must not return a value");
    f->is_async=def->is_async;
    if(f->is_async && cls && (f->is_static || !strncmp(f->name,"__",2))) err(c,def->line,"%s.%s cannot be async in compiled code",cls->name,f->name);
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
    f->params=MPY_NEW_ARR(AVar*,f->nparams>0?f->nparams:1);
    f->defaults=MPY_NEW_ARR(Expr*,f->nparams>0?f->nparams:1);
    for(int i=0;i<f->nparams;i++){ if(symtab_find(&f->locals,e->eparams[i])) err(c,e->line,"duplicate lambda parameter '%s'",e->eparams[i]); f->params[i]=new_local(f,e->eparams[i],NULL); }
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
    if(f->star<0 && f->kwonly<nreg) return "keyword-only parameters";
    return NULL;
}
static Ty *fn_type(Ck *c, AFunc *f, int line){
    const char *why=fn_type_problem(f);
    if(why && !(c->ep_value && f->is_async && !f->outer && !f->cls && !strcmp(why,"a coroutine"))) err(c,line,"%s() cannot be used as a value in compiled code (%s)",f->name,why);
    if(!f->fty){ Ty *ps[16]; int n=0;
        for(int i=0;i<f->nparams;i++) ps[n++]=i==f->star||i==f->dstar ? ty_find(f->params[i]->ty)->elem : f->params[i]->ty;
        f->fty=ty_func(ps,n,f->ret); f->fty->tup=(f->star>=0?1:0)|(f->dstar>=0?2:0); }
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
    if(def->decorator_count) err(c,def->line,"class decorators are not supported in compiled code");
    if(p->nclasses==p->ccap){ p->ccap=p->ccap?p->ccap*2:16; p->classes=(AClass**)xrealloc(p->classes,sizeof(AClass*)*(size_t)p->ccap); }
    p->classes[p->nclasses++]=cls;
    return cls;
}
/* Fields and methods; done once every class of the module is known (annotations may name them). */
static void fill_class(Ck *c, AClass *cls){
    Stmt *def=cls->def; AotUnit *u=cls->mod->unit;
    if(def->name2){
        ASym *s=global_sym(cls->mod,def->name2);
        if(!s || s->kind!=AS_CLASS) err(c,def->line,"base class '%s' is not a class",def->name2);
        cls->base=(AClass*)s->p;
        if(aot_subclass(cls->base,cls)) err(c,def->line,"class %s inherits from itself",cls->name);
    }
    for(int i=0;i<def->body_count;i++){ Stmt *s=def->body[i];
        if(s->kind==STMT_FUNCTION_DEF){
            AFunc *f=new_func(c,NULL,cls->mod,cls,s,NULL);
            if(f->ndeco){ char *vn=hidden_name(cls->name,s->name);
                f->decovar=new_global(c,cls->mod,vn+1);          /* Class.name after its decorators */
                symtab_add(&cls->mod->syms,vn,AS_FUNC,f); }
            if(cls->nmethods==cls->mcap){ cls->mcap=cls->mcap?cls->mcap*2:8; cls->methods=(AFunc**)xrealloc(cls->methods,sizeof(AFunc*)*(size_t)cls->mcap); }
            cls->methods[cls->nmethods++]=f;
        } else if(aot_is_annotation_only(u,s)){
            AAssign *a=aot_assign(u,s);
            Ty *t=annotation_range(c,a->ann_start,a->ann_end,s->line);
            add_field(cls,a->target[0]->name,t,NULL);
        } else if(s->kind==STMT_ASSIGN){
            AAssign *a=aot_assign(u,s);
            if(a->ntarget!=1 || a->target[0]->kind!=EXPR_NAME || a->aug) err(c,s->line,"a class body may only declare fields (name: type [= value]) and methods");
            Ty *t=a->ann_start>=0 ? annotation_range(c,a->ann_start,a->ann_end,s->line) : NULL;
            add_field(cls,a->target[0]->name,t,a->value);
        } else if(s->kind==STMT_PASS || (s->kind==STMT_EXPR && aot_expr(u,&s->expr)->kind==EXPR_LITERAL)){
        } else err(c,s->line,"a class body may only declare fields and methods in compiled code");
    }
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
            if(!strcmp(dotted,"ctypes")){
                Parser p=parser_at(u,s->start);
                while(peek(&p)->kind!=T_IMPORT) p.pos++;
                p.pos++;
                if(peek(&p)->kind!=T_STAR) do{ Tok *nm=need(&p,T_NAME,"imported name"); const char *alias=nm->text;
                    if(match(&p,T_AS)) alias=need(&p,T_NAME,"alias")->text;
                    if(ct->n<64){ ct->local[ct->n]=alias; ct->member[ct->n]=nm->text; ct->n++; }
                } while(match(&p,T_COMMA));
            }
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
        {"c_int",CT_INT},{"c_long",CT_INT},{"c_int32",CT_INT},{"c_ssize_t",CT_INT},
        {"c_uint",CT_UINT},{"c_ulong",CT_UINT},{"c_uint32",CT_UINT},{"c_size_t",CT_UINT},
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
            if(c->p->target!=AOT_TARGET_LINUX) err(c,s->line,"ctypes libraries need the linux target");
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
static void collect_module(Ck *c, AModule *m){
    c->mod=m;
    collect_ctypes(c,m);                        /* lib = ctypes.CDLL(...) and its declarations: not variables */
    Stmt **b=m->unit->ast->body; int n=m->unit->ast->body_count;
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_CLASS_DEF){ if(module_sym(m,s->name)) err(c,s->line,"'%s' is defined twice",s->name); symtab_add(&m->syms,s->name,AS_CLASS,new_class(c,m,s)); }
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
/* A copy of a def's statements as the parser left them (token ranges): every
   instance of a generic function gets its own expression trees and types. */
static Expr *range_copy(Expr *e){ if(!e) return NULL; if(e->kind!=EXPR_TOKEN_RANGE) return e; return expr_new_range(e->start,e->end,e->line); }
static Stmt *stmt_clone(Stmt *s){
    Stmt *t=MPY_NEW0(Stmt); *t=*s;
    t->aux=NULL; t->expr=range_copy(s->expr); t->expr2=range_copy(s->expr2);
    t->defaults=s->default_count?MPY_NEW_ARR(Expr*,s->default_count):NULL; t->default_cap=s->default_count;
    for(int i=0;i<s->default_count;i++) t->defaults[i]=range_copy(s->defaults[i]);
    t->decorator_exprs=s->decorator_count?MPY_NEW_ARR(Expr*,s->decorator_count):NULL; t->decorator_cap=s->decorator_count;
    for(int i=0;i<s->decorator_count;i++) t->decorator_exprs[i]=range_copy(s->decorator_exprs[i]);
    t->body=s->body_count?MPY_NEW_ARR(Stmt*,s->body_count):NULL; t->body_cap=s->body_count;
    for(int i=0;i<s->body_count;i++) t->body[i]=stmt_clone(s->body[i]);
    t->orelse=s->orelse_count?MPY_NEW_ARR(Stmt*,s->orelse_count):NULL; t->orelse_cap=s->orelse_count;
    for(int i=0;i<s->orelse_count;i++) t->orelse[i]=stmt_clone(s->orelse[i]);
    return t;
}
static int has_untyped_params(Stmt *def){
    for(int i=0;i<def->param_count;i++) if(!def->annotations || i>=def->annotation_cap || !def->annotations[i]) return 1;
    return 0;
}
/* a fresh instance of generic module function fn (one per decorator use) -> its hidden name */
static const char *instantiate(Ck *c, AFunc *fn){
    Stmt *copy=stmt_clone(fn->pristine);
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
    for(int i=0;i<n;i++) if(b[i]->kind==STMT_CLASS_DEF) fill_class(c,(AClass*)module_sym(m,b[i]->name)->p);
    for(int i=0;i<n;i++) if(b[i]->kind==STMT_FUNCTION_DEF){
        ASym *x=module_sym(m,b[i]->name);
        if(x->kind!=AS_FUNC){ char *h=hidden_name(b[i]->name,NULL); x=module_sym(m,h); free(h); }
        AFunc *fn=(AFunc*)x->p;
        if(!b[i]->decorator_count && has_untyped_params(b[i])) fn->pristine=stmt_clone(b[i]);   /* before anything parses it */
        new_func(c,fn,m,NULL,b[i],NULL); }
    /* the module body itself */
    AFunc *body=MPY_NEW0(AFunc);
    body->name=xstrdup2(m->name); body->mod=m; body->body=b; body->nbody=n; body->ret=TY_VOID_T; body->line=1;
    add_func(c->p,body);
    m->body=body;
}

/* Modules the compiler provides itself (no source file). */
static const char *builtin_module(const char *name){
    static const char *mods[]={"sys","asyncio","math","time","random","typing","functools","ctypes","json","minipy",NULL};
    for(int i=0;mods[i];i++) if(!strcmp(mods[i],name)) return mods[i];
    return NULL;
}

/* import a.b.c / import a.b as x / from a.b import n [as m] / from a import * */
static void link_imports(Ck *c, AModule *m){
    c->mod=m;
    AotUnit *u=m->unit;
    Stmt **b=u->ast->body; int n=u->ast->body_count;
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_IMPORT){
            const char *dotted=s->name2?s->name2:s->name;
            if(!strcmp(dotted,"thread")) err(c,s->line,"the thread module is not supported in compiled code");
            const char *bm=builtin_module(dotted);
            if(bm){ symtab_add(&m->syms,s->name,AS_SYS,(void*)bm); continue; }
            if(s->block_tag){ AModule *t=find_module(c->p,dotted); if(!t) err(c,s->line,"no module named '%s'",dotted); symtab_add(&m->syms,s->name,AS_MODULE,t); }
            else { AModule *t=find_module(c->p,s->name); if(!t) err(c,s->line,"no module named '%s'",s->name); symtab_add(&m->syms,s->name,AS_MODULE,t); }
        } else if(s->kind==STMT_FROM_IMPORT){
            char *dotted=aot_from_import_module(u,s);
            Parser p=parser_at(u,s->start);
            while(peek(&p)->kind!=T_IMPORT) p.pos++;
            p.pos++;
            if(!strcmp(dotted,"typing")||!strcmp(dotted,"collections.abc")||!strcmp(dotted,"__future__")){ free(dotted); continue; }   /* type names are understood directly */
            if(!strcmp(dotted,"functools")){                    /* from functools import reduce */
                do{ Tok *nm=need(&p,T_NAME,"imported name"); const char *alias=nm->text;
                    if(strcmp(nm->text,"reduce") && strcmp(nm->text,"wraps")) err(c,s->line,"functools.%s is not available in compiled code",nm->text);
                    if(match(&p,T_AS)) alias=need(&p,T_NAME,"alias")->text;
                    symtab_add(&m->syms,alias,AS_SYS,(void*)(nm->text[0]=='r'?"functools.reduce":"functools.wraps"));
                } while(match(&p,T_COMMA));
                free(dotted); continue;
            }
            if(!strcmp(dotted,"ctypes")||!strcmp(dotted,"json")||!strcmp(dotted,"minipy")){    /* from ctypes import CDLL, c_int / from json import dumps */
                do{ Tok *nm=need(&p,T_NAME,"imported name"); const char *alias=nm->text;
                    if(match(&p,T_AS)) alias=need(&p,T_NAME,"alias")->text;
                    char full[96]; snprintf(full,sizeof full,"%s.%s",dotted,nm->text);
                    symtab_add(&m->syms,alias,AS_SYS,xstrdup2(full));
                } while(match(&p,T_COMMA));
                free(dotted); continue;
            }
            if(builtin_module(dotted)) err(c,s->line,"use `import %s` and %s.name in compiled code",dotted,dotted);
            if(!strcmp(dotted,"thread")) err(c,s->line,"the thread module is not supported in compiled code");
            AModule *t=find_module(c->p,dotted);
            if(!t) err(c,s->line,"no module named '%s'",dotted);
            if(match(&p,T_STAR)){
                for(int k=0;k<t->syms.n;k++){ ASym *x=&t->syms.v[k]; if(x->name[0]!='_') symtab_add(&m->syms,x->name,x->kind,x->p); }
            } else do{
                Tok *nm=need(&p,T_NAME,"imported name"); const char *alias=nm->text;
                if(match(&p,T_AS)) alias=need(&p,T_NAME,"alias")->text;
                ASym *x=symtab_find(&t->syms,nm->text);
                if(x) symtab_add(&m->syms,alias,x->kind,x->p);
                else {
                    char sub[300]; snprintf(sub,sizeof sub,"%s.%s",dotted,nm->text);
                    AModule *sm=find_module(c->p,sub);
                    if(!sm) err(c,s->line,"cannot import name '%s' from '%s'",nm->text,dotted);
                    symtab_add(&m->syms,alias,AS_MODULE,sm);
                }
            } while(match(&p,T_COMMA));
            free(dotted);
        }
    }
}

/* ---------------------------------------------------------------- expressions */

static Ty *ck_expr(Ck *c, Expr *e);
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

static const char *op_method(TokKind op){
    switch(op){
        case T_PLUS: case T_PLUS_ASSIGN: return "__add__"; case T_MINUS: case T_MINUS_ASSIGN: return "__sub__";
        case T_STAR: case T_STAR_ASSIGN: return "__mul__"; case T_SLASH: case T_SLASH_ASSIGN: return "__truediv__";
        case T_FLOOR_DIV: case T_FLOOR_DIV_ASSIGN: return "__floordiv__"; case T_PERCENT: case T_PERCENT_ASSIGN: return "__mod__";
        case T_POWER: case T_POWER_ASSIGN: return "__pow__"; case T_AMP: case T_AMP_ASSIGN: return "__and__";
        case T_PIPE: case T_PIPE_ASSIGN: return "__or__"; case T_CARET: case T_CARET_ASSIGN: return "__xor__";
        case T_SHL: case T_SHL_ASSIGN: return "__lshift__"; case T_SHR: case T_SHR_ASSIGN: return "__rshift__";
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
    if(opfn && a->k==TY_OBJ){ AFunc *m=op_method_of(c,a,op_method(op),b,line); if(m){ *opfn=m; return m->ret; } }
    int fl=(a->k==TY_FLOAT||b->k==TY_FLOAT);
    switch(op){
        case T_PLUS: case T_PLUS_ASSIGN:
            if(numeric(a)&&numeric(b)) return fl?TY_FLOAT_T:TY_INT_T;
            if(a->k==TY_STR&&b->k==TY_STR) return TY_STR_T;
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
            if(a->k==TY_SET&&b->k==TY_SET&&op!=T_CARET&&op!=T_CARET_ASSIGN){ if(!unify(c,a->elem,b->elem)) break; return a; }
            break;
        case T_SHL: case T_SHR: case T_SHL_ASSIGN: case T_SHR_ASSIGN:
            if((a->k==TY_INT||a->k==TY_BOOL)&&(b->k==TY_INT||b->k==TY_BOOL)) return TY_INT_T;
            break;
        default: break;
    }
    err(c,line,"unsupported operand types %s and %s",ty_name(a),ty_name(b));
}

static int is_none(Expr *e){ return e->kind==EXPR_NONE; }
static void ck_compare_pair_m(Ck *c, int code, Expr *ea, Expr *eb, Ty *a, Ty *b, int line, AFunc **opfn, int *neg);
static void ck_compare_pair(Ck *c, int code, Expr *ea, Expr *eb, Ty *a, Ty *b, int line){ ck_compare_pair_m(c,code,ea,eb,a,b,line,NULL,NULL); }
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
        if(is_none(ea)||is_none(eb)){ unify(c,a,b); return; }       /* x is None: compare with x's zero value */
        a=ty_find(a); b=ty_find(b);
        if(a->k==TY_VAR||b->k==TY_VAR){ pending(c,line,"an operand of 'is'"); return; }
        if(a->k==TY_OBJ&&b->k==TY_OBJ&&(aot_subclass(a->cls,b->cls)||aot_subclass(b->cls,a->cls))) return;
        if(ty_is_ptr(a) && unify(c,a,b)) return;                    /* the same object */
        err(c,line,"'is' compares objects (or with None); got %s and %s",ty_name(a),ty_name(b));
    }
    if(code==CMP_IN||code==CMP_NOTIN){
        Ty *h=ty_find(b);
        if(h->k==TY_VAR){ pending(c,line,"the right operand of 'in'"); return; }
        if(h->k==TY_STR){ expect(c,TY_STR_T,a,line,"the left operand of 'in'"); return; }
        if(h->k==TY_DICT){ expect(c,ty_dkey(h),a,line,"the left operand of 'in'"); return; }
        if(h->k==TY_LIST||h->k==TY_SET){ expect(c,h->elem,a,line,"the left operand of 'in'"); return; }
        err(c,line,"'in' needs a str, list, set or dict on the right, not %s",ty_name(h));
    }
    if(is_none(ea)||is_none(eb)){ unify(c,a,b); return; }
    Ty *x=ty_find(a), *y=ty_find(b);
    if(x->k==TY_VAR||y->k==TY_VAR){ if(!unify(c,x,y)) pending(c,line,"a comparison operand"); return; }
    if(numeric(x)&&numeric(y)) return;
    if(code==CMP_EQ||code==CMP_NE){
        if(x->k==TY_OBJ&&y->k==TY_OBJ&&(aot_subclass(x->cls,y->cls)||aot_subclass(y->cls,x->cls))) return;
        if(x->k==TY_STR&&y->k==TY_STR) return;
        if(ty_same(x,y) && (x->k==TY_LIST||x->k==TY_SET||x->k==TY_TUPLE||x->k==TY_DICT)) return;
        if((x->k==TY_FUNC||x->k==TY_GEN) && unify(c,x,y)) return;      /* identity */
        err(c,line,"cannot compare %s with %s",ty_name(x),ty_name(y));
    }
    if(x->k==TY_TUPLE && !x->names && ty_same(x,y)) return;
    if(x->k==TY_STR&&y->k==TY_STR) return;
    err(c,line,"cannot order %s and %s",ty_name(x),ty_name(y));
}

static Ty *elem_of(Ck *c, Ty *t, int line, const char *what){
    t=ty_find(t);
    if(t->k==TY_VAR) return pending(c,line,what);
    if(t->k==TY_STR) return TY_STR_T;
    if(t->k==TY_LIST||t->k==TY_SET) return t->elem;
    if(t->k==TY_DICT) return ty_dkey(t);
    if(t->k==TY_FILE) return TY_STR_T;                   /* its lines */
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
        if(n!=2) err(c,line,"enumerate() produces (index, item) pairs: use `for i, x in enumerate(...)`");
        xi->kind=X_BUILTIN; xi->name="enumerate"; xi->ty=TY_INT_T;
        out[0]=TY_INT_T; out[1]=elem_of(c,ck_expr(c,it->items[0]),line,"the iterable");
        return;
    }
    if(is_builtin_call(c,it,"zip")){
        if(it->count!=2||n!=2) err(c,line,"zip() is supported with two iterables: `for a, b in zip(x, y)`");
        xi->kind=X_BUILTIN; xi->name="zip"; xi->ty=TY_INT_T;
        out[0]=elem_of(c,ck_expr(c,it->items[0]),line,"the iterable");
        out[1]=elem_of(c,ck_expr(c,it->items[1]),line,"the iterable");
        return;
    }
    if(it->kind==EXPR_CALL && it->a->kind==EXPR_ATTRIBUTE && !strcmp(it->a->name,"items") && it->count==0){
        Ty *d=ty_find(ck_expr(c,it->a->a));
        if(d->k==TY_VAR){ out[0]=pending(c,line,"the dictionary"); if(n==2) out[1]=ty_var(); return; }
        if(d->k!=TY_DICT) err(c,line,"items() needs a dict");
        if(n!=2) err(c,line,"items() produces (key, value) pairs: use `for k, v in d.items()`");
        xi->kind=X_TMETHOD; xi->name="items"; xi->ty=TY_VOID_T;
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
/* The loop variables of a comprehension are its own (hidden locals/globals). */
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
        for(int k=0;k<cl->nvars;k++){
            cs_push(c,cl->vars[k],xi->cvars[2*i+k],e->line);
            Expr nm; memset(&nm,0,sizeof nm); nm.kind=EXPR_NAME; nm.name=cl->vars[k]; ck_store(c,&nm,ts[k],e->line);
        }
        for(int k=0;k<cl->ncond;k++) no_void(c,ck_expr(c,cl->conds[k]),e->line);
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
        for(int k=0;k<cl->ncond;k++) no_void(c,ck_expr(c,cl->conds[k]),e->line);
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
static Ty *ck_expr_want(Ck *c, Expr *e, Ty *want){
    Ty *w=ty_find(want);
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

static Ty *ck_expr_inner(Ck *c, Expr *e){
    XInfo *xi=xinfo(e);
    xi->kind=X_NONE;
    switch(e->kind){
        case EXPR_LITERAL:{ Tok *t=e->tok;
            if(t->kind==T_STRING) return TY_STR_T;
            if(t->is_float) return TY_FLOAT_T;
            if(t->i>0xFFFFFFFFLL||t->i<-0x80000000LL) err(c,e->line,"integer %s does not fit in 32 bits",t->text);
            return TY_INT_T; }
        case EXPR_TRUE: case EXPR_FALSE: return TY_BOOL_T;
        case EXPR_NONE: return ty_var();                 /* the zero value of whatever type it meets */
        case EXPR_NAME:{
            ASym *s=lookup(c,e->name);
            if(!s && !strcmp(e->name,"__name__")){ xi->kind=X_CONST_STR; xi->name=c->mod->name; return TY_STR_T; }
            if(!s) err(c,e->line,"name '%s' is not defined",e->name);
            if(s->kind==AS_FUNC){ AFunc *fn=(AFunc*)s->p; xi->kind=X_FUNCREF; xi->fn=fn; fn->value_used=1; fn->ncalls++; return fn_type(c,fn,e->line); }
            if(s->kind==AS_CLIB||s->kind==AS_CFUNC) err(c,e->line,"'%s' is a C %s: call its functions",e->name,s->kind==AS_CLIB?"library":"function");
            if(s->kind!=AS_VAR) err(c,e->line,"'%s' is a %s, not a value",e->name,s->kind==AS_CLASS?"class":"module");
            xi->kind=X_VAR; xi->var=(AVar*)s->p;
            { AVar *r=var_root(xi->var); if(r->fn_const){ r->fn_const->value_used=1; r->fn_const->ncalls++; } }
            return xi->var->ty; }
        case EXPR_UNARY:{
            Ty *t=ty_find(ck_expr(c,e->a)); no_void(c,t,e->line);
            if(e->op==T_NOT) return TY_BOOL_T;
            if(t->k==TY_VAR) return pending(c,e->line,"an operand");
            if(e->op==T_TILDE){ if(t->k!=TY_INT&&t->k!=TY_BOOL) err(c,e->line,"~ needs an int"); return TY_INT_T; }
            if(t->k==TY_OBJ){ AFunc *m=op_method_of(c,t,"__neg__",NULL,e->line); if(m){ xi->kind=X_OPMETHOD; xi->fn=m; return m->ret; } }
            if(!numeric(t)) err(c,e->line,"unary - needs a number, not %s",ty_name(t));
            return t->k==TY_FLOAT?TY_FLOAT_T:TY_INT_T; }
        case EXPR_BINARY:{
            Ty *a=ck_expr(c,e->a);
            if(e->op==T_PERCENT && e->a->kind==EXPR_LITERAL && e->a->tok->kind==T_STRING){
                if(e->b->kind==EXPR_TUPLE){ for(int i=0;i<e->b->count;i++) no_void(c,ck_expr(c,e->b->items[i]),e->line); xinfo(e->b)->ty=TY_VOID_T; }
                else no_void(c,ck_expr(c,e->b),e->line);
                return TY_STR_T;
            }
            if(e->op==T_PERCENT && ty_find(a)->k==TY_STR) err(c,e->line,"the format string of %% must be a string literal in compiled code");
            { AFunc *m=NULL; Ty *r=binop_type_m(c,e->op,a,ck_expr(c,e->b),e->line,&m);
              if(m){ xi->kind=X_OPMETHOD; xi->fn=m; }
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
                if(xi->cmpfn[i] && e->count>2) err(c,e->line,"chained comparisons of objects are not supported in compiled code");
                prev=t; }
            return TY_BOOL_T; }
        case EXPR_TERNARY:{
            no_void(c,ck_expr(c,e->a),e->line);
            Ty *a=ck_expr(c,e->b), *b=ck_expr(c,e->c);
            if(numeric(a)&&numeric(b)&&ty_find(a)->k!=ty_find(b)->k) return (ty_find(a)->k==TY_FLOAT||ty_find(b)->k==TY_FLOAT)?TY_FLOAT_T:TY_INT_T;
            if(!unify(c,a,b)) err(c,e->line,"both branches of `x if c else y` must have the same type (%s and %s)",ty_name(a),ty_name(b));
            return a; }
        case EXPR_CALL: return ck_call(c,e);
        case EXPR_ATTRIBUTE:{
            if(is_sys(c,e->a)){
                if(!strcmp(e->name,"platform")){ xi->kind=X_CONST_STR; xi->name=c->p->target==AOT_TARGET_KOLIBRI?"kolibrios":"linux"; return TY_STR_T; }
                err(c,e->line,"sys.%s is not a value",e->name);
            }
            if(is_bmod(c,e->a,"math") && (!strcmp(e->name,"pi")||!strcmp(e->name,"e")||!strcmp(e->name,"tau"))){ xi->kind=X_BMOD; xi->name=e->name; return TY_FLOAT_T; }
            if(bmod(c,e->a)) err(c,e->line,"%s.%s is not a value in compiled code",bmod(c,e->a),e->name);
            if(e->a->kind==EXPR_NAME){                        /* Class.CONST: a field's constant default */
                ASym *cs=lookup(c,e->a->name);
                if(cs && cs->kind==AS_CLASS && !aot_find_field((AClass*)cs->p,e->name) && aot_find_method((AClass*)cs->p,e->name)){   /* Class.method: a function taking the object */
                    AFunc *fn=aot_find_method((AClass*)cs->p,e->name);
                    if(fn->decovar) err(c,e->line,"%s.%s is decorated; use it through an object",((AClass*)cs->p)->name,e->name);
                    xi->kind=X_FUNCREF; xi->fn=fn; fn->value_used=1; return fn_type(c,fn,e->line); }
                if(cs && cs->kind==AS_CLASS){ AClass *cls=(AClass*)cs->p; AField *fd=aot_find_field(cls,e->name);
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
            Ty *t=ty_find(ck_expr(c,e->a)), *i=ck_expr(c,e->b);
            if(t->k==TY_VAR) return pending(c,e->line,"the indexed value");
            if(t->k==TY_OBJ){ AFunc *m=op_method_of(c,t,"__getitem__",i,e->line); if(!m) err(c,e->line,"%s has no __getitem__",t->cls->name); xi->kind=X_OPMETHOD; xi->fn=m; return m->ret; }
            if(t->k==TY_STR){ expect(c,TY_INT_T,i,e->line,"a string index"); return TY_STR_T; }
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
            if(t->k!=TY_STR&&t->k!=TY_LIST) err(c,e->line,"%s cannot be sliced",ty_name(t));
            return t; }
        case EXPR_LIST: case EXPR_SET:{
            Ty *el=literal_elem(c,e->items,e->count);
            for(int i=0;i<e->count;i++){ Ty *t=ck_expr(c,e->items[i]); no_void(c,t,e->line); expect(c,el,t,e->line,"a list element"); }
            return ty_new(e->kind==EXPR_LIST?TY_LIST:TY_SET,el,NULL); }
        case EXPR_DICT:{
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
            if(e->count>64) err(c,e->line,"tuple too long");
            for(int i=0;i<e->count;i++){ ts[i]=ck_expr(c,e->items[i]); no_void(c,ts[i],e->line); }
            return ty_tuple(ts,e->count); }
        case EXPR_LAMBDA:{
            AFunc *lf=xi->fn; if(!lf) lf=xi->fn=new_lambda(c,e);
            lf->value_used=1; lf->ncalls++;
            AFunc *save=c->fn; c->fn=lf;
            Ty *r=e->a->kind==EXPR_NONE ? TY_VOID_T : ck_expr(c,e->a);     /* lambda: None returns nothing */
            c->fn=save;
            if(!unify(c,lf->ret,r)) err(c,e->line,"the lambda returns %s, expected %s",ty_name(r),ty_name(lf->ret));
            xi->kind=X_FUNCREF; xi->fn=lf;
            return fn_type(c,lf,e->line); }
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
            if(x->kind==EXPR_CALL){
                Expr *save=c->coro_ok; c->coro_ok=x;
                Ty *t=ck_expr(c,x);
                c->coro_ok=save;
                XInfo *xx=xinfo(x);
                if((xx->kind==X_FUNC||xx->kind==X_METHOD||xx->kind==X_STATIC||xx->kind==X_SUPER) && xx->fn && xx->fn->is_async){ xi->name="call"; return t; }
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

/* Literal default values only: they are evaluated at the call site. */
static Ty *ck_default(Ck *c, Expr *d){
    Expr *e=d;
    if(e->kind==EXPR_UNARY && e->op==T_MINUS && e->a->kind==EXPR_LITERAL) e=e->a;
    if(!(e->kind==EXPR_LITERAL||e->kind==EXPR_TRUE||e->kind==EXPR_FALSE||e->kind==EXPR_NONE||
         (e->kind==EXPR_LIST&&e->count==0)||(e->kind==EXPR_DICT&&e->count==0)))
        err(c,d->line,"default values must be literals in compiled code");
    return ck_expr(c,d);
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
        else if(fn->defaults[k]){ AModule *save=c->mod; c->mod=fn->mod; Ty *t=ck_default(c,fn->defaults[k]); c->mod=save; expect(c,pt,t,e->line,what); }
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
    u->tv=lex(b.s); SymTable st; memset(&st,0,sizeof st); u->ast=build_ast_and_symbols(&u->tv,&st);
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
static Ty *ck_callval(Ck *c, Expr *e, Ty *ft){
    XInfo *xi=xinfo(e);
    ft=ty_find(ft);
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
            if(pi<nreg){ int ep=is_endpoint(ft->elems[pi]); c->ep_value+=ep;
                Ty *t=ck_expr_want(c,a,ft->elems[pi]); c->ep_value-=ep;
                no_void(c,t,e->line); expect(c,ft->elems[pi],t,e->line,"an argument"); ep_arg(c,ft->elems[pi],a,e->line); xi->argmap[pi++]=i; }
            else if(star){ Ty *t=ck_expr_want(c,a,starel); no_void(c,t,e->line); expect(c,starel,t,e->line,"an argument"); int_push(&xi->xargs,&xi->nxargs,i); }
            else TOO_MANY();
        } else if(a->akind==3){
            if(!dstar) err(c,e->line,"keyword argument '%s': a function value takes keywords only through **kwargs in compiled code",a->kw);
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
    if(pi<nreg) err(c,e->line,"this function takes %d argument%s",nreg,nreg==1?"":"s");
    xi->kind=X_CALLVAL; xi->fn=NULL;
    return ft->elem;
}
static Ty *ck_ctor(Ck *c, Expr *e, AClass *cls){
    XInfo *xi=xinfo(e);
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
    if(!numeric(kt)&&kt->k!=TY_STR&&kt->k!=TY_TUPLE) err(c,line,"sort keys must be numbers, strings or tuples, not %s",ty_name(kt));
    return k;
}
/* a generator expression given to a function that consumes all of it: a list comprehension */
static void genexp_as_list(Expr *e, int i){ if(i<e->count && e->items[i]->kind==EXPR_COMPREHENSION && e->items[i]->comp_kind=='G') e->items[i]->comp_kind='g'; }
/* a generator value given to such a function: list(generator) */
static Ty *gen_as_list(Ck *c, Expr *e, int i){
    if(i>=e->count) return NULL;
    Expr *a=e->items[i];
    Ty *t=ty_find(ck_expr(c,a));
    if(t->k!=TY_GEN || a->akind) return t;
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
    if(nit>2) err(c,e->line,"map() over more than two iterables is not supported in compiled code");
    Expr *fn=e->items[0], *body; char **vars=MPY_NEW_ARR(char*,2);
    static const char *hidden[2]={"__m0","__m1"};
    if(fn->kind==EXPR_LAMBDA){
        if(fn->neparam!=nit) err(c,e->line,"the lambda of %s() takes %d argument%s",name,nit,nit==1?"":"s");
        for(int k=0;k<nit;k++) vars[k]=fn->eparams[k];
        body=fn->a;
    } else if(isfilter && fn->kind==EXPR_NONE){ vars[0]=(char*)hidden[0]; body=name_expr(hidden[0],e->line); }
    else {
        Expr *k=nit==1 ? key_as_lambda(c,fn) : fn;
        if(k->kind==EXPR_LAMBDA){ vars[0]=k->eparams[0]; body=k->a; }
        else {
            if(fn->kind!=EXPR_NAME && fn->kind!=EXPR_ATTRIBUTE) err(c,e->line,"%s() needs a lambda or a function name in compiled code",name);
            body=xnew(EXPR_CALL,e->line); body->a=fn;
            for(int j=0;j<nit;j++){ vars[j]=(char*)hidden[j]; xpush(body,name_expr(hidden[j],e->line)); }
        }
    }
    Expr *it=e->items[1];
    if(nit==2){ it=xnew(EXPR_CALL,e->line); it->a=name_expr("zip",e->line); xpush(it,e->items[1]); xpush(it,e->items[2]); }
    e->kind=EXPR_COMPREHENSION; e->comp_kind='L';
    e->clauses=MPY_NEW0(CompClause); e->nclause=1; e->ccap=1;
    CompClause *cl=&e->clauses[0]; cl->vars=vars; cl->nvars=nit; cl->iter=it;
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
    if(name[0]=='z' && e->count!=2) err(c,e->line,"zip() of two iterables is supported in compiled code");
    if(name[0]=='e' && (e->count<1||e->count>2)) err(c,e->line,"enumerate() takes 1 or 2 arguments");
    Expr *it=xnew(EXPR_CALL,e->line); it->a=name_expr(name,e->line);
    for(int i=0;i<e->count;i++) xpush(it,e->items[i]);
    char **vars=MPY_NEW_ARR(char*,2); vars[0]="__p0"; vars[1]="__p1";
    Expr *tu=xnew(EXPR_TUPLE,e->line); xpush(tu,name_expr("__p0",e->line)); xpush(tu,name_expr("__p1",e->line));
    make_comprehension(e,'L',tu,NULL,vars,2,it);
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
    if(e->count==3){ acc=ty_find(ck_expr(c,e->items[2])); no_void(c,acc,line); if(acc->k==TY_INT && ty_find(el)->k==TY_FLOAT) acc=TY_FLOAT_T; }
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
static Ty *ck_builtin(Ck *c, Expr *e, const char *name){
    XInfo *xi=xinfo(e); xi->kind=X_BUILTIN; xi->name=name;
    int line=e->line;
    if(!strcmp(name,"map")||!strcmp(name,"filter")) return ck_mapfilter(c,e,name);
    if(!strcmp(name,"iter")) return ck_iter(c,e);
    if(!strcmp(name,"sum")||!strcmp(name,"sorted")||!strcmp(name,"min")||!strcmp(name,"max")||!strcmp(name,"set")||!strcmp(name,"list")||!strcmp(name,"any")||!strcmp(name,"all"))
        genexp_as_list(e,0);
    if(!strcmp(name,"next")){ ck_positional(c,e,1,2,name); Ty *t=ty_find(arg(c,e,0));
        if(t->k==TY_VAR) return pending(c,line,"the argument of next()");
        if(t->k!=TY_GEN) err(c,line,"next() needs a generator (or iter(...)), not %s",ty_name(t));
        if(e->count==2){ Ty *d=ck_expr_want(c,e->items[1],t->elem); no_void(c,d,line); expect(c,t->elem,d,line,"the default of next()"); }
        return t->elem; }
    if(!strcmp(name,"sorted")||!strcmp(name,"min")||!strcmp(name,"max")) take_sort_kwargs(c,e,name[0]=='s');
    if(!strcmp(name,"sum")||!strcmp(name,"sorted")||(e->count==1&&(!strcmp(name,"min")||!strcmp(name,"max")))||!strcmp(name,"set"))
        gen_as_list(c,e,0);
    if(xi->rev) expect(c,TY_BOOL_T,ck_expr(c,xi->rev),line,"reverse=");
    if(!strcmp(name,"len")){ ck_positional(c,e,1,1,name); Ty *t=ty_find(arg(c,e,0));
        if(t->k==TY_VAR) return pending(c,line,"the argument of len()");
        if(t->k==TY_STR||t->k==TY_LIST||t->k==TY_DICT||t->k==TY_SET||t->k==TY_BUF||t->k==TY_TUPLE) return TY_INT_T;
        if(t->k==TY_OBJ){ AFunc *m=aot_find_method(t->cls,"__len__"); if(m){ xi->fn=m; return TY_INT_T; } }
        err(c,line,"len() of %s",ty_name(t)); }
    if(!strcmp(name,"str")||!strcmp(name,"repr")){ ck_positional(c,e,0,1,name); if(e->count) arg(c,e,0); return TY_STR_T; }
    if(!strcmp(name,"int")){ ck_positional(c,e,0,1,name); if(e->count){ Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) pending(c,line,"the argument of int()"); else if(!numeric(t)&&t->k!=TY_STR) err(c,line,"int() of %s",ty_name(t)); } return TY_INT_T; }
    if(!strcmp(name,"float")){ ck_positional(c,e,0,1,name); if(e->count){ Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) pending(c,line,"the argument of float()"); else if(!numeric(t)&&t->k!=TY_STR) err(c,line,"float() of %s",ty_name(t)); } return TY_FLOAT_T; }
    if(!strcmp(name,"bool")){ ck_positional(c,e,0,1,name); if(e->count) arg(c,e,0); return TY_BOOL_T; }
    if(!strcmp(name,"abs")){ ck_positional(c,e,1,1,name); Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) return pending(c,line,"the argument of abs()"); if(!numeric(t)) err(c,line,"abs() of %s",ty_name(t)); return t->k==TY_FLOAT?TY_FLOAT_T:TY_INT_T; }
    if(!strcmp(name,"min")||!strcmp(name,"max")){
        ck_positional(c,e,1,16,name);
        if(xi->key){ if(e->count!=1) err(c,line,"%s(key=...) takes one list",name);
            Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) return pending(c,line,"the argument");
            if(t->k!=TY_LIST&&t->k!=TY_SET&&t->k!=TY_DICT) err(c,line,"%s() of %s",name,ty_name(t));
            Ty *el=t->k==TY_DICT?ty_dkey(t):t->elem;
            ck_sort_key(c,xi->key,el,line); xi->ty=t; return el; }
        if(e->count==1){ Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) return pending(c,line,"the argument"); if(t->k!=TY_LIST&&t->k!=TY_SET&&t->k!=TY_DICT) err(c,line,"%s() of %s",name,ty_name(t));
            if(t->k==TY_DICT){ Ty *k=ty_find(ty_dkey(t)); if(k->k==TY_VAR) return pending(c,line,"the keys"); if(!numeric(k)&&k->k!=TY_STR) err(c,line,"%s() needs numbers or strings",name); xi->ty=t; return k; }
            Ty *el=ty_find(t->elem); if(el->k==TY_VAR) return pending(c,line,"the elements");
            if(!numeric(el)&&el->k!=TY_STR&&el->k!=TY_TUPLE) err(c,line,"%s() needs numbers, strings or tuples, not %s",name,ty_name(el));
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
        return ty_new(TY_FILE,NULL,NULL); }
    if(!strcmp(name,"any")||!strcmp(name,"all")){ ck_positional(c,e,1,1,name); elem_of(c,arg(c,e,0),line,"the argument"); return TY_BOOL_T; }
    if(!strcmp(name,"reversed")){ ck_positional(c,e,1,1,name); Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) return pending(c,line,"the argument of reversed()");
        if(t->k!=TY_LIST&&t->k!=TY_STR&&t->k!=TY_TUPLE) err(c,line,"reversed() of %s",ty_name(t));
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
    if(!strcmp(name,"sum")){ ck_positional(c,e,1,1,name); Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) return pending(c,line,"the argument of sum()");
        if((t->k!=TY_LIST&&t->k!=TY_SET)) err(c,line,"sum() of %s",ty_name(t));
        Ty *el=ty_find(t->elem); if(el->k==TY_VAR) return pending(c,line,"the elements"); if(!numeric(el)) err(c,line,"sum() of %s",ty_name(t));
        return el->k==TY_FLOAT?TY_FLOAT_T:TY_INT_T; }
    if(!strcmp(name,"sorted")){ ck_positional(c,e,1,1,name); Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) return pending(c,line,"the argument of sorted()");
        if(t->k!=TY_LIST&&t->k!=TY_SET&&t->k!=TY_DICT&&t->k!=TY_STR) err(c,line,"sorted() of %s",ty_name(t));
        Ty *el=t->k==TY_DICT?ty_dkey(t):t->k==TY_STR?TY_STR_T:t->elem;
        if(xi->key){ ck_sort_key(c,xi->key,el,line); return ty_new(TY_LIST,el,NULL); }
        Ty *f=ty_find(el); if(f->k==TY_VAR) return pending(c,line,"the elements");
        if(!numeric(f)&&f->k!=TY_STR&&f->k!=TY_TUPLE) err(c,line,"sorted() needs numbers, strings or tuples, not %s",ty_name(f));
        return ty_new(TY_LIST,el,NULL); }
    if(!strcmp(name,"isinstance")){
        ck_positional(c,e,2,2,name);
        Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) pending(c,line,"the object"); else if(t->k!=TY_OBJ) err(c,line,"isinstance() checks class instances");
        Expr *k=e->items[1]; int n=k->kind==EXPR_TUPLE?k->count:1;
        for(int i=0;i<n;i++){ Expr *x=k->kind==EXPR_TUPLE?k->items[i]:k;
            ASym *s=x->kind==EXPR_NAME?lookup(c,x->name):NULL;
            if(!s||s->kind!=AS_CLASS) err(c,line,"isinstance() needs a class (or a tuple of classes)");
            xinfo(x)->kind=X_CTOR; xinfo(x)->cls=(AClass*)s->p; }
        return TY_BOOL_T; }
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
    if(!strcmp(name,"super")) err(c,line,"super() can only be used as super().method(...)");
    err(c,line,"name '%s' is not defined",name);
}

/* Methods of str / list / dict / set. */
static Ty *ck_tmethod(Ck *c, Expr *e, Ty *t, const char *m){
    XInfo *xi=xinfo(e); xi->kind=X_TMETHOD; xi->name=m; int line=e->line;
    #define NARGS(lo,hi) ck_positional(c,e,lo,hi,m)
    if(t->k==TY_STR){
        if(!strcmp(m,"upper")||!strcmp(m,"lower")||!strcmp(m,"capitalize")){ NARGS(0,0); return TY_STR_T; }
        if(!strcmp(m,"encode")||!strcmp(m,"decode")){ NARGS(0,2); for(int i=0;i<e->count;i++) expect(c,TY_STR_T,arg(c,e,i),line,"the encoding"); return TY_STR_T; }   /* UTF-8 either way */
        if(!strcmp(m,"strip")||!strcmp(m,"lstrip")||!strcmp(m,"rstrip")){ NARGS(0,1); if(e->count) expect(c,TY_STR_T,arg(c,e,0),line,"the characters"); return TY_STR_T; }
        if(!strcmp(m,"startswith")||!strcmp(m,"endswith")){ NARGS(1,1); expect(c,TY_STR_T,arg(c,e,0),line,"the argument"); return TY_BOOL_T; }
        if(!strcmp(m,"find")||!strcmp(m,"count")||!strcmp(m,"index")){ NARGS(1,1); expect(c,TY_STR_T,arg(c,e,0),line,"the argument"); return TY_INT_T; }
        if(!strcmp(m,"replace")){ NARGS(2,2); expect(c,TY_STR_T,arg(c,e,0),line,"the argument"); expect(c,TY_STR_T,arg(c,e,1),line,"the argument"); return TY_STR_T; }
        if(!strcmp(m,"split")){ NARGS(0,1); if(e->count) expect(c,TY_STR_T,arg(c,e,0),line,"the separator"); return ty_new(TY_LIST,TY_STR_T,NULL); }
        if(!strcmp(m,"join")){ NARGS(1,1); genexp_as_list(e,0); gen_as_list(c,e,0); Ty *a=ty_find(arg(c,e,0)); if(a->k==TY_VAR) return pending(c,line,"the argument of join()"); if(a->k==TY_STR) return TY_STR_T; if((a->k!=TY_LIST&&a->k!=TY_SET)||!unify(c,a->elem,TY_STR_T)) err(c,line,"join() needs a list of str, not %s",ty_name(a)); return TY_STR_T; }
        if(!strcmp(m,"format")){
            Expr *recv=e->a->a;
            if(recv->kind!=EXPR_LITERAL||recv->tok->kind!=T_STRING) err(c,line,"str.format() needs a literal format string in compiled code");
            for(int i=0;i<e->count;i++){ if(e->items[i]->akind==1||e->items[i]->akind==2) err(c,line,"format(*args) is not supported"); no_void(c,ck_expr(c,e->items[i]),line); }
            return TY_STR_T; }
        if(!strcmp(m,"ljust")||!strcmp(m,"rjust")||!strcmp(m,"center")){ NARGS(1,2); expect(c,TY_INT_T,arg(c,e,0),line,"the width"); if(e->count==2) expect(c,TY_STR_T,arg(c,e,1),line,"the fill character"); return TY_STR_T; }
        if(!strcmp(m,"zfill")){ NARGS(1,1); expect(c,TY_INT_T,arg(c,e,0),line,"the width"); return TY_STR_T; }
        if(!strcmp(m,"rfind")||!strcmp(m,"rindex")){ NARGS(1,1); expect(c,TY_STR_T,arg(c,e,0),line,"the argument"); return TY_INT_T; }
        if(!strcmp(m,"title")||!strcmp(m,"swapcase")){ NARGS(0,0); return TY_STR_T; }
        if(!strcmp(m,"splitlines")){ NARGS(0,0); return ty_new(TY_LIST,TY_STR_T,NULL); }
        if(!strcmp(m,"isdecimal")||!strcmp(m,"isnumeric")){ NARGS(0,0); xi->name="isdigit"; return TY_BOOL_T; }
        if(!strcmp(m,"isdigit")||!strcmp(m,"isalpha")||!strcmp(m,"isspace")||!strcmp(m,"isupper")||!strcmp(m,"islower")||!strcmp(m,"isalnum")){ NARGS(0,0); return TY_BOOL_T; }
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
            Ty *f=ty_find(el); if(f->k==TY_VAR) pending(c,line,"the elements"); else if(!numeric(f)&&f->k!=TY_STR&&f->k!=TY_TUPLE) err(c,line,"sort() needs numbers, strings or tuples"); return TY_VOID_T; }
        if(!strcmp(m,"copy")){ NARGS(0,0); return t; }
        if(!strcmp(m,"extend")){ NARGS(1,1); genexp_as_list(e,0); gen_as_list(c,e,0); Ty *a=ty_find(arg(c,e,0)); if(a->k==TY_VAR) pending(c,line,"the argument"); else if(a->k!=TY_LIST||!unify(c,a->elem,el)) err(c,line,"extend() needs a %s",ty_name(t)); return TY_VOID_T; }
    } else if(t->k==TY_DICT){
        Ty *v=t->elem, *k=ty_dkey(t);
        if(!strcmp(m,"get")){ NARGS(1,2); expect(c,k,arg(c,e,0),line,"a dictionary key"); if(e->count==2) expect(c,v,arg(c,e,1),line,"the default"); return v; }
        if(!strcmp(m,"pop")){ NARGS(1,2); expect(c,k,arg(c,e,0),line,"a dictionary key"); if(e->count==2) expect(c,v,arg(c,e,1),line,"the default"); return v; }
        if(!strcmp(m,"setdefault")){ NARGS(2,2); expect(c,k,arg(c,e,0),line,"a dictionary key"); expect(c,v,arg(c,e,1),line,"the default"); return v; }
        if(!strcmp(m,"keys")){ NARGS(0,0); return ty_new(TY_LIST,k,NULL); }
        if(!strcmp(m,"values")){ NARGS(0,0); return ty_new(TY_LIST,v,NULL); }
        if(!strcmp(m,"clear")){ NARGS(0,0); return TY_VOID_T; }
        if(!strcmp(m,"copy")){ NARGS(0,0); return t; }
        if(!strcmp(m,"update")){ NARGS(1,1); expect(c,t,arg(c,e,0),line,"the argument"); return TY_VOID_T; }
        if(!strcmp(m,"items")){ NARGS(0,0); Ty *kv[2]={k,v}; return ty_new(TY_LIST,ty_tuple(kv,2),NULL); }
    } else if(t->k==TY_FILE){
        if(!strcmp(m,"read")||!strcmp(m,"readline")){ NARGS(0,0); return TY_STR_T; }
        if(!strcmp(m,"readlines")){ NARGS(0,0); return ty_new(TY_LIST,TY_STR_T,NULL); }
        if(!strcmp(m,"write")){ NARGS(1,1); expect(c,TY_STR_T,arg(c,e,0),line,"the text"); return TY_INT_T; }
        if(!strcmp(m,"close")){ NARGS(0,0); return TY_VOID_T; }
    } else if(t->k==TY_TASK){
        if(!strcmp(m,"done")){ NARGS(0,0); return TY_BOOL_T; }
        if(!strcmp(m,"result")){ NARGS(0,0); return t->elem; }
    } else if(t->k==TY_SET){
        Ty *el=t->elem;
        if(!strcmp(m,"add")||!strcmp(m,"remove")||!strcmp(m,"discard")){ NARGS(1,1); expect(c,el,arg(c,e,0),line,"a set element"); return TY_VOID_T; }
        if(!strcmp(m,"clear")){ NARGS(0,0); return TY_VOID_T; }
        if(!strcmp(m,"copy")){ NARGS(0,0); return t; }
        if(!strcmp(m,"union")||!strcmp(m,"intersection")||!strcmp(m,"difference")){ NARGS(1,1); expect(c,t,arg(c,e,0),line,"the argument"); return t; }
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
            if(t->k!=TY_INT&&t->k!=TY_BOOL&&t->k!=TY_STR&&t->k!=TY_BUF) err(c,line,"syscall() arguments are int, str or buffer, not %s",ty_name(t)); }
        return ty_new(TY_LIST,TY_INT_T,NULL);
    }
    if(!strcmp(m,"buffer")){ ck_positional(c,e,1,1,m); Ty *t=ty_find(arg(c,e,0)); if(t->k!=TY_INT&&t->k!=TY_STR&&t->k!=TY_VAR) err(c,line,"buffer() takes a size or a str"); return TY_BUF_T; }
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
        if(!strcmp(m,"atan2")||!strcmp(m,"pow")||!strcmp(m,"hypot")||!strcmp(m,"fmod")){ ck_positional(c,e,2,2,what); FARG(0); FARG(1); return TY_FLOAT_T; }
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
    if(c->p->target!=AOT_TARGET_LINUX) err(c,line,"C functions (ctypes) need the linux target");
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
        int isint=t->k==TY_INT||t->k==TY_BOOL, isptr=t->k==TY_STR||t->k==TY_BUF;
        if(k==CT_DEFAULT){
            if(isint) k=CT_INT; else if(t->k==TY_FLOAT) k=CT_DOUBLE; else if(t->k==TY_STR) k=CT_CHARP; else if(t->k==TY_BUF) k=CT_VOIDP;
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
        case CT_CHARP: return TY_OSTR_T;                   /* NULL is None */
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
    if(c->p->target!=AOT_TARGET_LINUX) err(c,line,"ctypes needs the linux target");
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
    if((xi->kind==X_FUNC||xi->kind==X_METHOD||xi->kind==X_STATIC||xi->kind==X_SUPER) && xi->fn && xi->fn->is_async && c->coro_ok!=e && !(c->fn && c->fn->calls_coroutines))
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
    if(!((xi->kind==X_FUNC||xi->kind==X_METHOD||xi->kind==X_STATIC||xi->kind==X_SUPER) && xi->fn && xi->fn->is_async))
        err(c,x->line,"expected a coroutine call (a call of an async function)");
    return t;
}
static Ty *ck_call_inner(Ck *c, Expr *e){
    Expr *f=e->a; XInfo *xi=xinfo(e);
    if(f->kind==EXPR_NAME){
        if(f->name[0]=='\001') return ck_builtin(c,e,f->name+1);         /* made by the compiler: always the builtin */
        ASym *s=lookup(c,f->name);
        if(!s) return ck_builtin(c,e,f->name);
        if(s->kind==AS_FUNC){ AFunc *fn=(AFunc*)s->p;
            if(fn->outer){ xi->kind=X_CALLNEST; xi->fn=fn; xi->var=NULL; ck_args(c,e,fn,0,xi); return fn->ret; }   /* a nested function calling itself */
            xi->kind=X_FUNC; xi->fn=fn; ck_args(c,e,fn,0,xi); return fn->ret; }
        if(s->kind==AS_CLASS) return ck_ctor(c,e,(AClass*)s->p);
        if(s->kind==AS_SYS && !strcmp((const char*)s->p,"functools.reduce")) return ck_reduce(c,e);
        if(s->kind==AS_SYS && !strncmp((const char*)s->p,"ctypes.",7)) return ck_ctypes_fn(c,e,(const char*)s->p+7);
        if(s->kind==AS_SYS && !strncmp((const char*)s->p,"json.",5)) return ck_json(c,e,(const char*)s->p+5);
        if(s->kind==AS_SYS && !strncmp((const char*)s->p,"minipy.",7)) return ck_minipy(c,e,(const char*)s->p+7);
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
    if(f->kind!=EXPR_ATTRIBUTE) return ck_callval(c,e,ck_expr(c,f));      /* (lambda x: ...)(1), fs[i](x), make()(x) */
    const char *m=f->name;
    if(is_sys(c,f->a)) return ck_sys(c,e,m);
    if(is_bmod(c,f->a,"asyncio")) return ck_asyncio(c,e,m);
    if(is_bmod(c,f->a,"ctypes")) return ck_ctypes_fn(c,e,m);
    if(is_bmod(c,f->a,"json")) return ck_json(c,e,m);
    if(is_bmod(c,f->a,"minipy")) return ck_minipy(c,e,m);
    if(bmod(c,f->a)) return ck_bmod(c,e,bmod(c,f->a),m);
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
    if(f->a->kind==EXPR_NAME){
        ASym *s=lookup(c,f->a->name);
        if(s && s->kind==AS_CLASS){                       /* Class.method(obj, ...) / static method */
            AClass *cls=(AClass*)s->p; AFunc *fn=aot_find_method(cls,m);
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
            if(fd) err(c,e->line,"field %s.%s is not callable",t->cls->name,m); err(c,e->line,"%s has no method '%s'",t->cls->name,m); }
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
            ASym *s=lookup(c,t->name);
            if(!s || s->kind!=AS_VAR) err(c,line,"cannot assign to '%s'",t->name);
            xi->kind=X_VAR; xi->var=(AVar*)s->p; xi->ty=xi->var->ty;
            if(xi->var->src) var_root(xi->var)->nonlocal_set=1;     /* nonlocal x; x = ...: shared through a cell */
            char what[160]; snprintf(what,sizeof what,"variable '%s'",t->name);
            expect(c,xi->var->ty,vt,line,what);
            return; }
        case EXPR_ATTRIBUTE:{
            AModule *m=module_expr(c,t->a);
            if(m){ ASym *s=module_sym(m,t->name); if(!s||s->kind!=AS_VAR) err(c,line,"cannot assign %s.%s",m->name,t->name);
                xi->kind=X_VAR; xi->var=(AVar*)s->p; xi->ty=xi->var->ty; expect(c,xi->var->ty,vt,line,"the module variable"); return; }
            Ty *o=ty_find(ck_expr(c,t->a));
            if(o->k==TY_VAR){ pending(c,line,"the object"); return; }
            if(o->k!=TY_OBJ) err(c,line,"cannot set attribute '%s' of %s",t->name,ty_name(o));
            AField *fd=aot_find_field(o->cls,t->name);
            if(!fd){
                if(aot_find_method(o->cls,t->name)) err(c,line,"'%s' is a method of %s",t->name,o->cls->name);
                fd=add_field(o->cls,t->name,NULL,NULL);
            }
            xi->kind=X_FIELD; xi->field=fd; xi->cls=o->cls; xi->ty=fd->ty;
            char what[160]; snprintf(what,sizeof what,"field %s.%s",o->cls->name,t->name);
            expect(c,fd->ty,vt,line,what);
            return; }
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
            if(v->k==TY_TUPLE){
                if(v->names) err(c,line,"%s (a dict literal with values of different types) cannot be unpacked in compiled code",ty_name(v));
                if(v->nelems!=t->count) err(c,line,"cannot unpack %s into %d targets",ty_name(v),t->count);
                xi->ty=vt;
                for(int i=0;i<t->count;i++) ck_store(c,t->items[i],v->elems[i],line);
                return;
            }
            if(v->k!=TY_LIST) err(c,line,"only a list or a tuple can be unpacked, not %s",ty_name(v));
            xi->ty=vt;
            for(int i=0;i<t->count;i++) ck_store(c,t->items[i],v->elem,line);
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
    if(a->ann_start>=0 && !a->annot) a->annot=annotation_range(c,a->ann_start,a->ann_end,s->line);
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

static void ck_stmt(Ck *c, Stmt *s){
    AotUnit *u=c->mod->unit;
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
            no_void(c,ck_expr(c,aot_expr(u,&s->expr)),s->line);
            ck_stmts(c,s->body,s->body_count); ck_stmts(c,s->orelse,s->orelse_count);
            return;
        case STMT_FOR:{
            if(s->param_count<1||s->param_count>16) err(c,s->line,"for loops take 1 to 16 variables in compiled code");
            Ty *ts[16]; iter_types(c,aot_expr(u,&s->expr),s->param_count,ts,s->line);
            for(int i=0;i<s->param_count;i++){ Expr nm; memset(&nm,0,sizeof nm); nm.kind=EXPR_NAME; nm.name=s->params[i]; ck_store(c,&nm,ts[i],s->line); }
            ck_stmts(c,s->body,s->body_count); ck_stmts(c,s->orelse,s->orelse_count);
            return; }
        case STMT_RETURN:
            if(!is_func(c->fn)) err(c,s->line,"return outside a function");
            if(c->fn->is_gen){ if(s->expr) err(c,s->line,"`return value` in a generator is not supported in compiled code"); return; }
            if(s->expr){ Ty *t=ck_expr_want(c,aot_expr(u,&s->expr),c->fn->ret); no_void(c,t,s->line);
                char what[160]; snprintf(what,sizeof what,"the return value of %s()",c->fn->name);
                expect(c,c->fn->ret,t,s->line,what); }
            return;
        case STMT_DEL:{
            ADel *d=aot_del(u,s);
            for(int i=0;i<d->n;i++){ Expr *t=d->t[i];
                if(t->kind!=EXPR_INDEX) err(c,s->line,"only `del list[i]` and `del dict[key]` are supported in compiled code");
                Ty *o=ty_find(ck_expr(c,t->a)), *k=ck_expr(c,t->b);
                if(o->k==TY_VAR){ pending(c,s->line,"the container"); continue; }
                if(o->k==TY_LIST) expect(c,TY_INT_T,k,s->line,"a list index");
                else if(o->k==TY_DICT) expect(c,ty_dkey(o),k,s->line,"a dictionary key");
                else err(c,s->line,"cannot delete items of %s",ty_name(o));
                xinfo(t)->ty=o; }
            return; }
        case STMT_ASSERT:
            no_void(c,ck_expr(c,aot_expr(u,&s->expr)),s->line);
            if(s->expr2) no_void(c,ck_expr(c,aot_expr(u,&s->expr2)),s->line);
            return;
        case STMT_RAISE:{
            if(!s->expr){ if(!c->in_except) err(c,s->line,"bare raise outside an except clause"); return; }
            Expr *e=aot_expr(u,&s->expr);
            if(e->kind==EXPR_NAME){                         /* raise ValueError */
                ASym *x=lookup(c,e->name);
                if(x && x->kind==AS_CLASS){ AClass *cls=(AClass*)x->p;
                    if(!aot_is_exception(cls)) err(c,s->line,"%s is not an exception class",cls->name);
                    xinfo(e)->kind=X_CTOR; xinfo(e)->cls=cls; xinfo(e)->ty=ty_new(TY_OBJ,NULL,cls); return; }
            }
            Ty *t=ty_find(ck_expr(c,e)); no_void(c,t,s->line);
            if(t->k!=TY_OBJ || !aot_is_exception(t->cls)) err(c,s->line,"raise needs an exception (a subclass of Exception), not %s",ty_name(t));
            return; }
        case STMT_WITH:{
            AWith *w=aot_with(u,s);
            for(int i=0;i<w->n;i++){ Ty *t=ck_expr(c,w->e[i]); if(w->as[i]){ Expr nm; memset(&nm,0,sizeof nm); nm.kind=EXPR_NAME; nm.name=w->as[i]; ck_store(c,&nm,t,s->line); } }
            ck_stmts(c,s->body,s->body_count);
            return; }
        case STMT_PASS: case STMT_BREAK: case STMT_CONTINUE: case STMT_GLOBAL: return;
        case STMT_FUNCTION_DEF:
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
            if(c->fn->def) err(c,s->line,"definitions and imports must be at module level in compiled code");
            {   /* decorated methods: Class.name = decorators(method), when the class statement runs */
                ASym *x=module_sym(c->mod,s->name); AClass *cls=x&&x->kind==AS_CLASS?(AClass*)x->p:NULL;
                for(int i=0;cls && i<cls->nmethods;i++){ AFunc *m=cls->methods[i]; if(!m->decovar) continue;
                    char *h=hidden_name(cls->name,m->name); ASym *hx=module_sym(c->mod,h); free(h);
                    Ty *t=ck_expr(c,deco_app(c,u,m->def,hx->name,1)); no_void(c,t,m->line);
                    char what[200]; snprintf(what,sizeof what,"%s.%s after its decorators",cls->name,m->name);
                    expect(c,m->decovar->ty,t,m->line,what); }
            }
            return;
        case STMT_IMPORT: case STMT_FROM_IMPORT:
            if(c->fn->def) err(c,s->line,"definitions and imports must be at module level in compiled code");
            return;
        case STMT_YIELD:{
            AFunc *g=c->fn;
            if(!g->is_gen) err(c,s->line,"'yield' outside a function");
            if(s->expr && s->expr->kind==EXPR_TOKEN_RANGE && u->tv.v[s->expr->start].kind==T_FROM){ s->expr->start++; s->block_tag=7; }   /* yield from */
            if(!s->expr) return;                            /* yields None: the zero value */
            Expr *e=aot_expr(u,&s->expr);
            if(s->block_tag==7){
                Ty *ts[1]; iter_types(c,e,1,ts,s->line);
                expect(c,g->yield_ty,ts[0],s->line,"the yielded values");
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
                        ASym *sy=x->kind==EXPR_NAME?lookup(c,x->name):NULL;
                        if(!sy||sy->kind!=AS_CLASS||!aot_is_exception((AClass*)sy->p)) err(c,b->line,"except needs exception classes");
                        AClass *cls=(AClass*)sy->p; xinfo(x)->kind=X_CTOR; xinfo(x)->cls=cls;
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
static void ck_stmts(Ck *c, Stmt **b, int n){ for(int i=0;i<n;i++) ck_stmt(c,b[i]); }
static void ck_body(Ck *c, AFunc *f){ for(int i=0;i<f->nbody;i++){ f->top_index=i; ck_stmt(c,f->body[i]); } }

/* ---------------------------------------------------------------- whole program */

static void check_all(Ck *c){
    AProg *p=c->p;
    for(int i=0;i<p->nclasses;i++){ AClass *cls=p->classes[i];           /* field defaults, in the module scope */
        c->mod=cls->mod; c->fn=cls->mod->body;
        for(int k=0;k<cls->nfields;k++){ AField *fd=cls->fields[k];
            if(fd->init){ Ty *t=ck_expr(c,fd->init); no_void(c,t,fd->init->line); char what[160]; snprintf(what,sizeof what,"field %s.%s",cls->name,fd->name); expect(c,fd->ty,t,fd->init->line,what); } }
    }
    for(int i=0;i<p->nfuncs;i++){ AFunc *f=p->funcs[i]; c->fn=f; c->mod=f->mod;
        if(f->outer) continue;                                  /* nested functions are checked where they are defined */
        if(c->strict && f->def && !f->cls && !f->ncalls){         /* a library function this program never calls */
            int unknown=0; for(int k=0;k<f->nparams;k++) if(!ty_known(f->params[k]->ty)) unknown=1;
            if(unknown){ f->unused=1; continue; }
        }
        ck_body(c,f); }
}

/* Element types nothing constrains (e.g. a list that stays empty) default to int. */
static void default_elems(Ty *t){
    t=ty_find(t);
    if(t->k==TY_DICT && t->key){ Ty *k=ty_find(t->key); if(k->k==TY_VAR) k->link=TY_STR_T; else default_elems(k); }
    if(t->k==TY_LIST||t->k==TY_DICT||t->k==TY_SET||t->k==TY_GEN){ Ty *e=ty_find(t->elem); if(e->k==TY_VAR) e->link=TY_INT_T; else default_elems(e); }
    if(t->k==TY_TUPLE) for(int i=0;i<t->nelems;i++) default_elems(t->elems[i]);
}
static void must_know(Ck *c, Ty *t, int line, const char *what, const char *name){
    default_elems(t);
    if(!ty_known(t)) err(c,line,"cannot infer the type of %s '%s'; add a type annotation",what,name);
}

static void layout_class(AClass *cls){
    if(cls->size) return;
    int off=12;                                               /* refcount, destroy, vtable */
    if(cls->base){ layout_class(cls->base); off=cls->base->size; }
    for(int i=0;i<cls->nfields;i++){ cls->fields[i]->offset=off; off+=ty_size(cls->fields[i]->ty); }
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
        TY_STR_T=ty_new(TY_STR,NULL,NULL); TY_VOID_T=ty_new(TY_VOID,NULL,NULL); TY_BUF_T=ty_new(TY_BUF,NULL,NULL);
        TY_OSTR_T=ty_new(TY_STR,NULL,NULL); TY_OSTR_T->tup=1; }
    AProg *p=MPY_NEW0(AProg); p->target=target;
    Ck ck; memset(&ck,0,sizeof ck); ck.p=p; Ck *c=&ck;
    if(setjmp(ck.fail)) return NULL;
    p->nmods=nunits; p->mods=MPY_NEW_ARR(AModule*,nunits);
    for(int i=0;i<nunits;i++){ AModule *m=MPY_NEW0(AModule); m->unit=units[i]; m->name=units[i]->name; m->index=i; p->mods[i]=m; }
    make_builtin_exceptions(p,p->mods[0]);
    for(int i=0;i<nunits;i++) if(units[i]->ast) collect_module(c,p->mods[i]);
    for(int i=nunits-1;i>=0;i--) if(units[i]->ast) link_imports(c,p->mods[i]);
    for(int i=0;i<nunits;i++) if(units[i]->ast) collect_functions(c,p->mods[i]);
    for(int pass=0;pass<100;pass++){
        c->changed=0; c->pass_first=ty_ids+1;
        check_all(c);
        if(!c->changed) break;
    }
    c->strict=1; c->pass_first=ty_ids+1;
    check_all(c);
    for(int i=0;i<c->nseen;i++) default_elems(c->seen[i]);
    free(c->seen);
    for(int i=0;i<p->nglobals;i++){ AVar *v=p->globals[i]; c->mod=v->mod; must_know(c,v->ty,1,"variable",v->name); }
    for(int i=0;i<p->nfuncs;i++){ AFunc *f=p->funcs[i];               /* inside a function nothing uses */
        for(AFunc *o=f->outer;o;o=o->outer) if(o->unused) f->unused=1; }
    for(int i=0;i<p->nfuncs;i++){ AFunc *f=p->funcs[i]; c->mod=f->mod; if(f->unused) continue;
        for(int k=0;k<f->nvars;k++) must_know(c,f->vars[k]->ty,f->line,k<f->nparams?"parameter":"variable",f->vars[k]->name);
        if(is_func(f)){ default_elems(f->ret); if(!ty_known(f->ret)) err(c,f->line,"cannot infer the return type of %s(); annotate it (-> type)",f->name); } }
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
