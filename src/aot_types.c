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
#include "compiler.h"
#include "lexer.h"

/* ---------------------------------------------------------------- types */

static int ty_ids;
Ty *TY_INT_T, *TY_BOOL_T, *TY_FLOAT_T, *TY_STR_T, *TY_VOID_T, *TY_BUF_T;

Ty *ty_new(TyKind k, Ty *elem, AClass *cls){ Ty *t=MPY_NEW0(Ty); t->k=k; t->elem=elem; t->cls=cls; t->id=++ty_ids; return t; }
Ty *ty_var(void){ return ty_new(TY_VAR,NULL,NULL); }
Ty *ty_dict(Ty *key, Ty *val){ Ty *t=ty_new(TY_DICT,val,NULL); t->key=key; return t; }
Ty *ty_dkey(Ty *d){ d=ty_find(d); return d->key?d->key:TY_STR_T; }
Ty *ty_tuple(Ty **elems, int n){
    Ty *t=ty_new(TY_TUPLE,NULL,NULL); t->nelems=n; t->elems=MPY_NEW_ARR(Ty*,n>0?n:1);
    for(int i=0;i<n;i++) t->elems[i]=elems[i];
    return t;
}
Ty *ty_find(Ty *t){ while(t && t->k==TY_VAR && t->link) t=t->link; return t; }
int ty_known(Ty *t){
    t=ty_find(t);
    if(!t || t->k==TY_VAR) return 0;
    if(t->k==TY_DICT && !ty_known(ty_dkey(t))) return 0;
    if(t->k==TY_LIST||t->k==TY_DICT||t->k==TY_SET||t->k==TY_TASK) return ty_known(t->elem);
    if(t->k==TY_TUPLE){ for(int i=0;i<t->nelems;i++) if(!ty_known(t->elems[i])) return 0; }
    return 1;
}
int ty_is_ptr(Ty *t){ t=ty_find(t); return t->k==TY_STR||t->k==TY_LIST||t->k==TY_DICT||t->k==TY_SET||t->k==TY_OBJ||t->k==TY_BUF||t->k==TY_TASK||t->k==TY_TUPLE||t->k==TY_FILE; }
int ty_size(Ty *t){ return ty_find(t)->k==TY_FLOAT ? 8 : 4; }
int ty_same(Ty *a, Ty *b){
    a=ty_find(a); b=ty_find(b);
    if(a==b) return 1;
    if(a->k!=b->k) return 0;
    if(a->k==TY_DICT && !ty_same(ty_dkey(a),ty_dkey(b))) return 0;
    if(a->k==TY_LIST||a->k==TY_DICT||a->k==TY_SET||a->k==TY_TASK) return ty_same(a->elem,b->elem);
    if(a->k==TY_OBJ) return a->cls==b->cls;
    if(a->k==TY_TUPLE){ if(a->nelems!=b->nelems) return 0; for(int i=0;i<a->nelems;i++) if(!ty_same(a->elems[i],b->elems[i])) return 0; return 1; }
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
        case TY_LIST: snprintf(b,160,"list[%s]",ty_name(t->elem)); return b;
        case TY_SET: snprintf(b,160,"set[%s]",ty_name(t->elem)); return b;
        case TY_DICT: snprintf(b,160,"dict[%s, %s]",ty_name(ty_dkey(t)),ty_name(t->elem)); return b;
        case TY_OBJ: return t->cls->name;
        case TY_TASK: snprintf(b,160,"Task[%s]",ty_name(t->elem)); return b;
        case TY_TUPLE:{ int n=snprintf(b,160,"tuple[");
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
    struct { const char *name; ASym sym; } cscope[32];   /* comprehension variables in scope */
    int ncscope;
    Expr *coro_ok;          /* the coroutine call being awaited / handed to asyncio */
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

static int occurs(Ty *v, Ty *t){
    t=ty_find(t); if(t==v) return 1;
    if(t->k==TY_TUPLE){ for(int i=0;i<t->nelems;i++) if(occurs(v,t->elems[i])) return 1; return 0; }
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
    if(a->k==TY_LIST||a->k==TY_DICT||a->k==TY_SET||a->k==TY_TASK) return unify(c,a->elem,b->elem);
    if(a->k==TY_OBJ) return a->cls==b->cls;
    if(a->k==TY_TUPLE){ if(a->nelems!=b->nelems) return 0; for(int i=0;i<a->nelems;i++) if(!unify(c,a->elems[i],b->elems[i])) return 0; return 1; }
    return 1;
}
/* May a value of type src be stored where dst is expected? (instances: subclass -> base) */
static int assignable(Ck *c, Ty *dst, Ty *src){
    Ty *d=ty_find(dst), *s=ty_find(src);
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

/* NAME | NAME '[' type (',' type)* ']' | module '.' NAME | None */
static Ty *parse_type(Ck *c, Tok *v, int *i, int end, int line){
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
    while(*i+1<end && v[*i].kind==T_DOT && v[*i+1].kind==T_NAME){       /* module.Class */
        ASym *s=module_sym(m,name);
        if(!s || s->kind!=AS_MODULE) err(c,line,"'%s' is not a module",name);
        m=(AModule*)s->p; name=v[*i+1].text; *i+=2;
    }
    Ty *args[16]={NULL}; int nargs=0;
    if(*i<end && v[*i].kind==T_LB){
        (*i)++;
        for(;;){ if(nargs==16) err(c,line,"too many type arguments"); args[nargs++]=parse_type(c,v,i,end,line);
            if(*i<end && v[*i].kind==T_COMMA){ (*i)++; continue; } break; }
        if(*i>=end || v[*i].kind!=T_RB) err(c,line,"expected ']' in type");
        (*i)++;
    }
    if(!strcmp(name,"int")) return TY_INT_T;
    if(!strcmp(name,"bool")) return TY_BOOL_T;
    if(!strcmp(name,"float")) return TY_FLOAT_T;
    if(!strcmp(name,"str")) return TY_STR_T;
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
            case STMT_RETURN: case STMT_RAISE: case STMT_YIELD: names_expr(aot_expr(u,&s->expr),out); break;
            case STMT_ASSERT: names_expr(aot_expr(u,&s->expr),out); names_expr(aot_expr(u,&s->expr2),out); break;
            case STMT_GLOBAL: if(globals) for(int k=0;k<s->param_count;k++) names_add(globals,s->params[k]); break;
            default: break;
        }
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
static void no_nested_defs(Ck *c, Stmt **b, int n){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_FUNCTION_DEF) err(c,s->line,"nested functions are not supported in compiled code (define '%s' at module level)",s->name);
        if(s->kind==STMT_CLASS_DEF) err(c,s->line,"classes must be defined at module level in compiled code");
        if(s->kind==STMT_IMPORT||s->kind==STMT_FROM_IMPORT) err(c,s->line,"import must be at the top of the module");
        no_nested_defs(c,s->body,s->body_count); no_nested_defs(c,s->orelse,s->orelse_count);
    }
}

/* f: a function object made earlier (module-level functions exist before imports are linked) or NULL */
static AFunc *new_func(Ck *c, AFunc *f, AModule *m, AClass *cls, Stmt *def){
    if(!f) f=MPY_NEW0(AFunc);
    f->name=xstrdup2(def->name); f->mod=m; f->cls=cls; f->def=def; f->body=def->body; f->nbody=def->body_count; f->line=def->line;
    for(int i=0;i<def->decorator_count;i++){
        if(cls && !strcmp(def->decorators[i],"staticmethod")) f->is_static=1;
        else if(cls && !strcmp(def->decorators[i],"property")) f->is_property=1;
        else err(c,def->line,"decorator @%s is not supported in compiled code",def->decorators[i]);
    }
    if(def->star_index>=0||def->dstar_index>=0) err(c,def->line,"*args/**kwargs are not supported in compiled code");
    if(def->param_count>16) err(c,def->line,"too many parameters");
    no_nested_defs(c,def->body,def->body_count);
    f->nparams=def->param_count;
    f->params=MPY_NEW_ARR(AVar*,f->nparams>0?f->nparams:1);
    f->defaults=MPY_NEW_ARR(Expr*,f->nparams>0?f->nparams:1);
    for(int i=0;i<f->nparams;i++){
        Ty *t=annotation(c,def->annotations && i<def->annotation_cap ? def->annotations[i] : NULL);
        if(cls && !f->is_static && i==0){
            Ty *self=ty_new(TY_OBJ,NULL,cls);
            if(t && !ty_same(t,self)) err(c,def->line,"'%s' of %s.%s must be %s",def->params[0],cls->name,f->name,cls->name);
            t=self;
        }
        if(t && ty_find(t)->k==TY_VOID) err(c,def->line,"parameter '%s' cannot be None",def->params[i]);
        f->params[i]=new_local(f,def->params[i],t);
        f->defaults[i]=NULL;
    }
    if(cls && !f->is_static && f->nparams==0) err(c,def->line,"method %s.%s needs a self parameter",cls->name,f->name);
    for(int i=0;i<def->default_count;i++){ int pi=f->nparams-def->default_count+i; if(pi>=0) f->defaults[pi]=aot_expr(m->unit,&def->defaults[i]); }
    f->returns_value=has_return_value(def->body,def->body_count);
    if(def->returns){ f->ret=annotation(c,def->returns); if(ty_find(f->ret)->k==TY_VOID && f->returns_value) err(c,def->line,"%s() is annotated -> None but returns a value",f->name); }
    else f->ret=f->returns_value ? ty_var() : TY_VOID_T;
    if(cls && !strcmp(f->name,"__init__") && ty_find(f->ret)->k!=TY_VOID) err(c,def->line,"__init__ must not return a value");
    f->is_async=def->is_async;
    if(f->is_async && cls && (f->is_static || !strncmp(f->name,"__",2))) err(c,def->line,"%s.%s cannot be async in compiled code",cls->name,f->name);
    Names locals={0}, globals={0};
    names_bound(c,def->body,def->body_count,&locals,&globals);
    f->globals_decl=globals.v; f->nglobals_decl=globals.n;
    for(int i=0;i<globals.n;i++) global_var(c,m,globals.v[i],def->line);
    for(int i=0;i<locals.n;i++){
        int isglobal=0; for(int k=0;k<globals.n;k++) if(!strcmp(globals.v[k],locals.v[i])) isglobal=1;
        if(!isglobal && !symtab_find(&f->locals,locals.v[i])) new_local(f,locals.v[i],NULL);
    }
    free(locals.v);
    add_func(c->p,f);
    return f;
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
            AFunc *f=new_func(c,NULL,cls->mod,cls,s);
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
static void collect_module(Ck *c, AModule *m){
    c->mod=m;
    Stmt **b=m->unit->ast->body; int n=m->unit->ast->body_count;
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_CLASS_DEF){ if(module_sym(m,s->name)) err(c,s->line,"'%s' is defined twice",s->name); symtab_add(&m->syms,s->name,AS_CLASS,new_class(c,m,s)); }
    }
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_FUNCTION_DEF){ if(module_sym(m,s->name)) err(c,s->line,"'%s' is defined twice",s->name); symtab_add(&m->syms,s->name,AS_FUNC,MPY_NEW0(AFunc)); }
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
    for(int i=0;i<nrest;i++){ no_nested_defs(c,rest[i]->body,rest[i]->body_count); no_nested_defs(c,rest[i]->orelse,rest[i]->orelse_count); }
    for(int i=0;i<globals.n;i++) global_var(c,m,globals.v[i],1);
    free(globals.v); free(rest);
}
static void collect_functions(Ck *c, AModule *m){
    c->mod=m;
    Stmt **b=m->unit->ast->body; int n=m->unit->ast->body_count;
    for(int i=0;i<n;i++) if(b[i]->kind==STMT_CLASS_DEF) fill_class(c,(AClass*)module_sym(m,b[i]->name)->p);
    for(int i=0;i<n;i++) if(b[i]->kind==STMT_FUNCTION_DEF) new_func(c,(AFunc*)module_sym(m,b[i]->name)->p,m,NULL,b[i]);
    /* the module body itself */
    AFunc *body=MPY_NEW0(AFunc);
    body->name=xstrdup2(m->name); body->mod=m; body->body=b; body->nbody=n; body->ret=TY_VOID_T; body->line=1;
    add_func(c->p,body);
    m->body=body;
}

/* Modules the compiler provides itself (no source file). */
static const char *builtin_module(const char *name){
    static const char *mods[]={"sys","asyncio","math","time","random",NULL};
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

static ASym *lookup(Ck *c, const char *name){
    for(int i=c->ncscope-1;i>=0;i--) if(!strcmp(c->cscope[i].name,name)) return &c->cscope[i].sym;
    AFunc *f=c->fn;
    if(f && f->def){
        for(int i=0;i<f->nglobals_decl;i++) if(!strcmp(f->globals_decl[i],name)) return module_sym(c->mod,name);
        ASym *s=symtab_find(&f->locals,name);
        if(s) return s;
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
        if(ty_same(x,y) && (x->k==TY_LIST||x->k==TY_SET||x->k==TY_TUPLE)) return;
        err(c,line,"cannot compare %s with %s",ty_name(x),ty_name(y));
    }
    if(x->k==TY_TUPLE && ty_same(x,y)) return;
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
        xi->cvars=MPY_NEW_ARR(AVar*,2*e->nclause+1);
        for(int i=0;i<e->nclause;i++) for(int k=0;k<e->clauses[i].nvars && k<2;k++){
            const char *nm=e->clauses[i].vars[k];
            xi->cvars[2*i+k]=c->fn&&c->fn->def ? hidden_local(c->fn,nm,NULL) : new_global(c,c->mod,nm);
        }
    }
    int save=c->ncscope; Ty *r;
    for(int i=0;i<e->nclause;i++){
        CompClause *cl=&e->clauses[i];
        if(cl->nvars>2) err(c,e->line,"at most two loop variables are supported");
        Ty *ts[2]; iter_types(c,cl->iter,cl->nvars,ts,e->line);
        for(int k=0;k<cl->nvars;k++){
            if(c->ncscope==32) err(c,e->line,"comprehensions nested too deeply");
            c->cscope[c->ncscope].name=cl->vars[k]; c->cscope[c->ncscope].sym.name=cl->vars[k];
            c->cscope[c->ncscope].sym.kind=AS_VAR; c->cscope[c->ncscope].sym.p=xi->cvars[2*i+k]; c->ncscope++;
            Expr nm; memset(&nm,0,sizeof nm); nm.kind=EXPR_NAME; nm.name=cl->vars[k]; ck_store(c,&nm,ts[k],e->line);
        }
        for(int k=0;k<cl->ncond;k++) no_void(c,ck_expr(c,cl->conds[k]),e->line);
    }
    if(e->comp_kind=='D'){ Ty *k=ck_expr(c,e->a); no_void(c,k,e->line); Ty *v=ck_expr(c,e->b); no_void(c,v,e->line); r=ty_dict(k,v); }
    else { Ty *el=ck_expr(c,e->a); no_void(c,el,e->line); r=ty_new(e->comp_kind=='L'?TY_LIST:TY_SET,el,NULL); }
    c->ncscope=save;
    return r;
}
/* [1, 2.5] is a float list; [Rect(), Circle()] a list of their nearest common base class */
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
            if(s->kind!=AS_VAR) err(c,e->line,"'%s' is a %s, not a value",e->name,s->kind==AS_FUNC?"function":s->kind==AS_CLASS?"class":"module");
            xi->kind=X_VAR; xi->var=(AVar*)s->p;
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
                if(s->kind!=AS_VAR) err(c,e->line,"%s.%s is not a value",m->name,e->name);
                xi->kind=X_VAR; xi->var=(AVar*)s->p; return xi->var->ty;
            }
            Ty *t=ty_find(ck_expr(c,e->a));
            if(t->k==TY_VAR) return pending(c,e->line,"the object");
            if(t->k!=TY_OBJ) err(c,e->line,"%s has no attribute '%s'",ty_name(t),e->name);
            AField *fd=aot_find_field(t->cls,e->name);
            if(fd){ xi->kind=X_FIELD; xi->field=fd; xi->cls=t->cls; return fd->ty; }
            { AFunc *m=aot_find_method(t->cls,e->name);
              if(m && m->is_property){ xi->kind=X_PROP; xi->fn=m; return m->ret; }
              if(m) err(c,e->line,"method %s.%s must be called",t->cls->name,e->name); }
            if(c->strict) err(c,e->line,"%s has no attribute '%s'",t->cls->name,e->name);
            return ty_var(); }
        case EXPR_INDEX:{
            Ty *t=ty_find(ck_expr(c,e->a)), *i=ck_expr(c,e->b);
            if(t->k==TY_VAR) return pending(c,e->line,"the indexed value");
            if(t->k==TY_OBJ){ AFunc *m=op_method_of(c,t,"__getitem__",i,e->line); if(!m) err(c,e->line,"%s has no __getitem__",t->cls->name); xi->kind=X_OPMETHOD; xi->fn=m; return m->ret; }
            if(t->k==TY_STR){ expect(c,TY_INT_T,i,e->line,"a string index"); return TY_STR_T; }
            if(t->k==TY_LIST){ expect(c,TY_INT_T,i,e->line,"a list index"); return t->elem; }
            if(t->k==TY_DICT){ expect(c,ty_dkey(t),i,e->line,"a dictionary key"); return t->elem; }
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
            Ty *v=literal_elem(c,e->vals,e->count), *k=ty_var();
            for(int i=0;i<e->count;i++){ Ty *kt=ck_expr(c,e->items[i]); no_void(c,kt,e->line); expect(c,k,kt,e->line,"a dictionary key"); Ty *t=ck_expr(c,e->vals[i]); no_void(c,t,e->line); expect(c,v,t,e->line,"a dictionary value"); }
            return ty_dict(k,v); }
        case EXPR_COMPREHENSION: return ck_comprehension(c,e);
        case EXPR_TUPLE:{
            Ty *ts[64];
            if(e->count>64) err(c,e->line,"tuple too long");
            for(int i=0;i<e->count;i++){ ts[i]=ck_expr(c,e->items[i]); no_void(c,ts[i],e->line); }
            return ty_tuple(ts,e->count); }
        case EXPR_LAMBDA: err(c,e->line,"lambda is not supported in compiled code (define a function)");
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
/* Match call arguments to fn's parameters (from `skip`) and check their types. */
static void ck_args(Ck *c, Expr *e, AFunc *fn, int skip, XInfo *xi){
    int np=fn->nparams, pi=skip;
    fn->ncalls++;
    for(int i=0;i<16;i++) xi->argmap[i]=-1;
    for(int i=0;i<e->count;i++){
        Expr *a=e->items[i];
        if(a->akind==1||a->akind==2) err(c,e->line,"*args/**kwargs calls are not supported in compiled code");
        int slot;
        if(a->akind==3){
            slot=-1; for(int k=skip;k<np;k++) if(!strcmp(fn->params[k]->name,a->kw)) slot=k;
            if(slot<0) err(c,e->line,"%s() has no parameter '%s'",fn->name,a->kw);
        } else { if(pi>=np) err(c,e->line,"%s() takes %d argument%s",fn->name,np-skip,np-skip==1?"":"s"); slot=pi++; }
        if(xi->argmap[slot]>=0) err(c,e->line,"%s() got two values for '%s'",fn->name,fn->params[slot]->name);
        xi->argmap[slot]=i;
    }
    for(int k=skip;k<np;k++){
        char what[200]; snprintf(what,sizeof what,"parameter '%s' of %s()",fn->params[k]->name,fn->name);
        if(xi->argmap[k]>=0){ Ty *t=ck_expr_want(c,e->items[xi->argmap[k]],fn->params[k]->ty); no_void(c,t,e->line); expect(c,fn->params[k]->ty,t,e->line,what); }
        else if(fn->defaults[k]){ AModule *save=c->mod; c->mod=fn->mod; Ty *t=ck_default(c,fn->defaults[k]); c->mod=save; expect(c,fn->params[k]->ty,t,e->line,what); }
        else err(c,e->line,"%s() is missing argument '%s'",fn->name,fn->params[k]->name);
    }
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
        if(!ki->var){ ki->var=c->fn&&c->fn->def ? hidden_local(c->fn,key->eparams[0],NULL) : new_global(c,c->mod,key->eparams[0]); }
        expect(c,ki->var->ty,elem,line,"the key's parameter");
        int save=c->ncscope;
        if(c->ncscope==32) err(c,line,"nested too deeply");
        c->cscope[c->ncscope].name=key->eparams[0]; c->cscope[c->ncscope].sym.name=key->eparams[0];
        c->cscope[c->ncscope].sym.kind=AS_VAR; c->cscope[c->ncscope].sym.p=ki->var; c->ncscope++;
        k=ck_expr(c,key->a);
        c->ncscope=save;
        ki->kind=X_NONE; ki->ty=k;
    } else if(key->kind==EXPR_NAME){
        ASym *s=lookup(c,key->name);
        if(!s || s->kind!=AS_FUNC) err(c,line,"key= needs a lambda or a function");
        AFunc *fn=(AFunc*)s->p; fn->ncalls++;
        if(fn->nparams!=1 || fn->is_async) err(c,line,"a key function takes one argument");
        expect(c,fn->params[0]->ty,elem,line,"the key's parameter");
        ki->kind=X_FUNC; ki->fn=fn; k=fn->ret; ki->ty=k;
    } else err(c,line,"key= needs a lambda or a function");
    Ty *kt=ty_find(k);
    if(kt->k==TY_VAR) return pending(c,line,"the key");
    if(!numeric(kt)&&kt->k!=TY_STR&&kt->k!=TY_TUPLE) err(c,line,"sort keys must be numbers, strings or tuples, not %s",ty_name(kt));
    return k;
}
static Ty *ck_builtin(Ck *c, Expr *e, const char *name){
    XInfo *xi=xinfo(e); xi->kind=X_BUILTIN; xi->name=name;
    int line=e->line;
    if(!strcmp(name,"sorted")||!strcmp(name,"min")||!strcmp(name,"max")) take_sort_kwargs(c,e,name[0]=='s');
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
            if(!numeric(el)&&el->k!=TY_STR) err(c,line,"%s() needs numbers or strings, not %s",name,ty_name(el));
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
    if(!strcmp(name,"dict")){ ck_positional(c,e,0,0,name); return ty_dict(ty_var(),ty_var()); }
    if(!strcmp(name,"print")) err(c,line,"print() is a statement here");
    if(!strcmp(name,"range")||!strcmp(name,"enumerate")||!strcmp(name,"zip")) err(c,line,"%s() can only be iterated by a for loop or a comprehension in compiled code",name);
    if(!strcmp(name,"super")) err(c,line,"super() can only be used as super().method(...)");
    err(c,line,"name '%s' is not defined",name);
}

/* Methods of str / list / dict / set. */
static Ty *ck_tmethod(Ck *c, Expr *e, Ty *t, const char *m){
    XInfo *xi=xinfo(e); xi->kind=X_TMETHOD; xi->name=m; int line=e->line;
    #define NARGS(lo,hi) ck_positional(c,e,lo,hi,m)
    if(t->k==TY_STR){
        if(!strcmp(m,"upper")||!strcmp(m,"lower")||!strcmp(m,"capitalize")){ NARGS(0,0); return TY_STR_T; }
        if(!strcmp(m,"strip")||!strcmp(m,"lstrip")||!strcmp(m,"rstrip")){ NARGS(0,1); if(e->count) expect(c,TY_STR_T,arg(c,e,0),line,"the characters"); return TY_STR_T; }
        if(!strcmp(m,"startswith")||!strcmp(m,"endswith")){ NARGS(1,1); expect(c,TY_STR_T,arg(c,e,0),line,"the argument"); return TY_BOOL_T; }
        if(!strcmp(m,"find")||!strcmp(m,"count")||!strcmp(m,"index")){ NARGS(1,1); expect(c,TY_STR_T,arg(c,e,0),line,"the argument"); return TY_INT_T; }
        if(!strcmp(m,"replace")){ NARGS(2,2); expect(c,TY_STR_T,arg(c,e,0),line,"the argument"); expect(c,TY_STR_T,arg(c,e,1),line,"the argument"); return TY_STR_T; }
        if(!strcmp(m,"split")){ NARGS(0,1); if(e->count) expect(c,TY_STR_T,arg(c,e,0),line,"the separator"); return ty_new(TY_LIST,TY_STR_T,NULL); }
        if(!strcmp(m,"join")){ NARGS(1,1); Ty *a=ty_find(arg(c,e,0)); if(a->k==TY_VAR) return pending(c,line,"the argument of join()"); if(a->k==TY_STR) return TY_STR_T; if((a->k!=TY_LIST&&a->k!=TY_SET)||!unify(c,a->elem,TY_STR_T)) err(c,line,"join() needs a list of str, not %s",ty_name(a)); return TY_STR_T; }
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
        if(!strcmp(m,"extend")){ NARGS(1,1); Ty *a=ty_find(arg(c,e,0)); if(a->k==TY_VAR) pending(c,line,"the argument"); else if(a->k!=TY_LIST||!unify(c,a->elem,el)) err(c,line,"extend() needs a %s",ty_name(t)); return TY_VOID_T; }
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
        ck_positional(c,e,1,6,"syscall");
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
static Ty *ck_call(Ck *c, Expr *e){
    Ty *t=ck_call_inner(c,e);
    XInfo *xi=xinfo(e);
    if((xi->kind==X_FUNC||xi->kind==X_METHOD||xi->kind==X_STATIC||xi->kind==X_SUPER) && xi->fn && xi->fn->is_async && c->coro_ok!=e)
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
    for(int i=0;i<e->count;i++) if(e->items[i]->akind==1||e->items[i]->akind==2) err(c,e->line,"*args/**kwargs calls are not supported in compiled code");
    if(f->kind==EXPR_NAME){
        ASym *s=lookup(c,f->name);
        if(!s) return ck_builtin(c,e,f->name);
        if(s->kind==AS_FUNC){ AFunc *fn=(AFunc*)s->p; xi->kind=X_FUNC; xi->fn=fn; ck_args(c,e,fn,0,xi); return fn->ret; }
        if(s->kind==AS_CLASS) return ck_ctor(c,e,(AClass*)s->p);
        err(c,e->line,"'%s' is not callable",f->name);
    }
    if(f->kind!=EXPR_ATTRIBUTE) err(c,e->line,"only functions, classes and methods can be called in compiled code");
    const char *m=f->name;
    if(is_sys(c,f->a)) return ck_sys(c,e,m);
    if(is_bmod(c,f->a,"asyncio")) return ck_asyncio(c,e,m);
    if(bmod(c,f->a)) return ck_bmod(c,e,bmod(c,f->a),m);
    AModule *mod=module_expr(c,f->a);
    if(mod){
        ASym *s=module_sym(mod,m);
        if(!s) err(c,e->line,"module %s has no attribute '%s'",mod->name,m);
        if(s->kind==AS_FUNC){ AFunc *fn=(AFunc*)s->p; xi->kind=X_FUNC; xi->fn=fn; ck_args(c,e,fn,0,xi); return fn->ret; }
        if(s->kind==AS_CLASS) return ck_ctor(c,e,(AClass*)s->p);
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
        if(!fn){ if(aot_find_field(t->cls,m)) err(c,e->line,"field %s.%s is not callable",t->cls->name,m); err(c,e->line,"%s has no method '%s'",t->cls->name,m); }
        xi->kind=fn->is_static?X_STATIC:X_METHOD; xi->fn=fn; xi->cls=t->cls;
        ck_args(c,e,fn,fn->is_static?0:1,xi);
        return fn->ret;
    }
    return ck_tmethod(c,e,t,m);
}

/* ---------------------------------------------------------------- statements */

static void ck_stmts(Ck *c, Stmt **b, int n);

/* Store a value of type vt into target t (name, attribute, item, or unpacking). */
static void ck_store(Ck *c, Expr *t, Ty *vt, int line){
    XInfo *xi=xinfo(t);
    no_void(c,vt,line);
    switch(t->kind){
        case EXPR_NAME:{
            ASym *s=lookup(c,t->name);
            if(!s || s->kind!=AS_VAR) err(c,line,"cannot assign to '%s'",t->name);
            xi->kind=X_VAR; xi->var=(AVar*)s->p; xi->ty=xi->var->ty;
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
            if(pr){ for(int i=0;i<pr->n;i++) no_void(c,ck_expr(c,pr->args[i]),s->line);
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
            if(!c->fn->def) err(c,s->line,"return outside a function");
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
        case STMT_IMPORT: case STMT_FROM_IMPORT: case STMT_FUNCTION_DEF: case STMT_CLASS_DEF:
            if(c->fn->def) err(c,s->line,"definitions and imports must be at module level in compiled code");
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
                    if(!v){ v=c->fn&&c->fn->def ? hidden_local(c->fn,b->name,NULL) : new_global(c,c->mod,b->name); b->aux=v; }
                    expect(c,v->ty,ty_new(TY_OBJ,NULL,common),b->line,"the exception variable");
                    if(c->ncscope==32) err(c,b->line,"nested too deeply");
                    c->cscope[c->ncscope].name=b->name; c->cscope[c->ncscope].sym.name=b->name;
                    c->cscope[c->ncscope].sym.kind=AS_VAR; c->cscope[c->ncscope].sym.p=v; c->ncscope++;
                }
                c->in_except++;
                ck_stmts(c,b->body,b->body_count);
                c->in_except--;
                c->ncscope=save;
            }
            return; }
        case STMT_YIELD: err(c,s->line,"generators (yield) are not supported in compiled code");
        case STMT_NONLOCAL: err(c,s->line,"nonlocal is not supported in compiled code");
        default: err(c,s->line,"'%s' is not supported in compiled code",s->name?s->name:stmt_kind_name(s->kind));
    }
}
static void ck_stmts(Ck *c, Stmt **b, int n){ for(int i=0;i<n;i++) ck_stmt(c,b[i]); }

/* ---------------------------------------------------------------- whole program */

static void check_all(Ck *c){
    AProg *p=c->p;
    for(int i=0;i<p->nclasses;i++){ AClass *cls=p->classes[i];           /* field defaults, in the module scope */
        c->mod=cls->mod; c->fn=cls->mod->body;
        for(int k=0;k<cls->nfields;k++){ AField *fd=cls->fields[k];
            if(fd->init){ Ty *t=ck_expr(c,fd->init); no_void(c,t,fd->init->line); char what[160]; snprintf(what,sizeof what,"field %s.%s",cls->name,fd->name); expect(c,fd->ty,t,fd->init->line,what); } }
    }
    for(int i=0;i<p->nfuncs;i++){ AFunc *f=p->funcs[i]; c->fn=f; c->mod=f->mod;
        if(c->strict && f->def && !f->cls && !f->ncalls){         /* a library function this program never calls */
            int unknown=0; for(int k=0;k<f->nparams;k++) if(!ty_known(f->params[k]->ty)) unknown=1;
            if(unknown){ f->unused=1; continue; }
        }
        ck_stmts(c,f->body,f->nbody); }
}

/* Element types nothing constrains (e.g. a list that stays empty) default to int. */
static void default_elems(Ty *t){
    t=ty_find(t);
    if(t->k==TY_DICT && t->key){ Ty *k=ty_find(t->key); if(k->k==TY_VAR) k->link=TY_STR_T; else default_elems(k); }
    if(t->k==TY_LIST||t->k==TY_DICT||t->k==TY_SET){ Ty *e=ty_find(t->elem); if(e->k==TY_VAR) e->link=TY_INT_T; else default_elems(e); }
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
        TY_STR_T=ty_new(TY_STR,NULL,NULL); TY_VOID_T=ty_new(TY_VOID,NULL,NULL); TY_BUF_T=ty_new(TY_BUF,NULL,NULL); }
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
    for(int i=0;i<p->nfuncs;i++){ AFunc *f=p->funcs[i]; c->mod=f->mod; if(f->unused) continue;
        for(int k=0;k<f->nvars;k++) must_know(c,f->vars[k]->ty,f->line,k<f->nparams?"parameter":"variable",f->vars[k]->name);
        if(f->def){ default_elems(f->ret); if(!ty_known(f->ret)) err(c,f->line,"cannot infer the return type of %s(); annotate it (-> type)",f->name); } }
    for(int i=0;i<p->nclasses;i++){ AClass *cls=p->classes[i]; c->mod=cls->mod; if(cls->builtin) continue;
        for(int k=0;k<cls->nfields;k++) must_know(c,cls->fields[k]->ty,cls->def->line,"field",cls->fields[k]->name); }
    for(int i=0;i<p->nclasses;i++) layout_class(p->classes[i]);
    mark_overrides(p);
    return p;
}
