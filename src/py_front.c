/* ========================= Full parser -> compiler frontend =========================
   See py_front.h. The Expr shapes are the ones the compiler always had:
   operators by token kind (T_PLUS ...), comparisons as an operand list
   whose operands carry their comparison code, call arguments in source
   order with their kind (positional, *, **, keyword), comprehension clauses
   with variable names; an f-string becomes the concatenation the compiler
   formats ("" + str(x) + ("%spec" % (y)) + ...). */

#include "py_front.h"
#include "tokens.h"
#include <setjmp.h>
#include <stdarg.h>

typedef struct { const char *path; jmp_buf jb; int hidden; Stmt *def; } Cv;    /* hidden: names made for desugaring (.t1, ...); def: the function being made */

MPY_NORETURN static void fail(Cv *c, int line, const char *fmt, ...){
    va_list ap; va_start(ap,fmt);
    fprintf(stderr,"%s:%d: error: ",c->path?c->path:"<source>",line);
    vfprintf(stderr,fmt,ap); fputc('\n',stderr);
    va_end(ap);
    longjmp(c->jb,1);
}

/* sys._compiled (1), not sys._compiled (-1), else 0 */
static int compiled_test(PyNode *t){
    int neg=0;
    if(t->kind==PK_UnaryOp && t->op==OP_Not){ neg=1; t=t->n[0]; }
    if(t->kind==PK_Attribute && !strcmp(t->id[0],"_compiled") && t->n[0]->kind==PK_Name && !strcmp(t->n[0]->id[0],"sys")) return neg?-1:1;
    return 0;
}
static Expr *enew(ExprKind k, int line){ Expr *e=MPY_NEW0(Expr); e->kind=k; e->line=line; return e; }
static void push(Expr ***arr, int *cnt, int *cap, Expr *v){
    if(*cnt==*cap){ *cap=*cap?*cap*2:4; *arr=(Expr**)xrealloc(*arr,sizeof(Expr*)*(size_t)*cap); }
    (*arr)[(*cnt)++]=v;
}
static void item(Expr *e, Expr *v){ push(&e->items,&e->count,&e->cap,v); }
static Tok *tok(TokKind k, const char *text, size_t len, int line){
    Tok *t=MPY_NEW0(Tok); t->kind=k; t->line=line;
    t->text=(char*)xmalloc(len+1); memcpy(t->text,text,len); t->text[len]=0; t->len=(int64_t)len;
    return t;
}
static Expr *str_lit(const char *s, size_t len, int line){ Expr *e=enew(EXPR_LITERAL,line); e->tok=tok(T_STRING,s,len,line); return e; }
static Expr *name_x(const char *name, int line){ Expr *e=enew(EXPR_NAME,line); e->name=xstrdup2(name); return e; }
static Expr *call1(const char *fn, Expr *arg, int line){ Expr *e=enew(EXPR_CALL,line); e->a=name_x(fn,line); arg->akind=0; item(e,arg); return e; }

/* ---------------------------------------------------------------- text */
typedef struct { char *s; size_t n, cap; } Buf;
static void bput(Buf *b, const char *s, size_t n){
    if(b->n+n+1>b->cap){ b->cap=(b->n+n+1)*2; b->s=(char*)xrealloc(b->s,b->cap); }
    memcpy(b->s+b->n,s,n); b->n+=n; b->s[b->n]=0;
}
static void bcp(Buf *b, uint32_t cp){                     /* UTF-8 (lone surrogates too, as 3 bytes) */
    char o[4]; size_t n;
    if(cp<0x80){ o[0]=(char)cp; n=1; }
    else if(cp<0x800){ o[0]=(char)(0xC0|(cp>>6)); o[1]=(char)(0x80|(cp&0x3F)); n=2; }
    else if(cp<0x10000){ o[0]=(char)(0xE0|(cp>>12)); o[1]=(char)(0x80|((cp>>6)&0x3F)); o[2]=(char)(0x80|(cp&0x3F)); n=3; }
    else { o[0]=(char)(0xF0|(cp>>18)); o[1]=(char)(0x80|((cp>>12)&0x3F)); o[2]=(char)(0x80|((cp>>6)&0x3F)); o[3]=(char)(0x80|(cp&0x3F)); n=4; }
    bput(b,o,n);
}
static void const_text(Buf *b, PyConst *k){
    if(k->kind==PC_Str) for(int i=0;i<k->ulen;i++) bcp(b,k->u[i]);
    else bput(b,(const char*)k->b,(size_t)k->blen);        /* bytes: the same bytes as a str */
}

/* ---------------------------------------------------------------- expressions */
static Expr *ex(Cv *c, PyNode *n);

static int tok_op(int op){
    switch(op){
        case OP_Add: return T_PLUS; case OP_Sub: return T_MINUS; case OP_Mult: return T_STAR; case OP_Div: return T_SLASH;
        case OP_FloorDiv: return T_FLOOR_DIV; case OP_Mod: return T_PERCENT; case OP_Pow: return T_POWER;
        case OP_LShift: return T_SHL; case OP_RShift: return T_SHR; case OP_BitOr: return T_PIPE; case OP_BitXor: return T_CARET;
        case OP_BitAnd: return T_AMP; case OP_MatMult: return T_AT; default: return 0;
    }
}
static int aug_op(int op){
    switch(op){
        case OP_Add: return T_PLUS_ASSIGN; case OP_Sub: return T_MINUS_ASSIGN; case OP_Mult: return T_STAR_ASSIGN; case OP_Div: return T_SLASH_ASSIGN;
        case OP_FloorDiv: return T_FLOOR_DIV_ASSIGN; case OP_Mod: return T_PERCENT_ASSIGN; case OP_Pow: return T_POWER_ASSIGN;
        case OP_LShift: return T_SHL_ASSIGN; case OP_RShift: return T_SHR_ASSIGN; case OP_BitOr: return T_PIPE_ASSIGN; case OP_BitXor: return T_CARET_ASSIGN;
        case OP_BitAnd: return T_AMP_ASSIGN; case OP_MatMult: return T_AT_ASSIGN; default: return 0;
    }
}
static int cmp_code(int op){
    switch(op){
        case OP_Lt: return CMP_LT; case OP_LtE: return CMP_LE; case OP_Gt: return CMP_GT; case OP_GtE: return CMP_GE;
        case OP_Eq: return CMP_EQ; case OP_NotEq: return CMP_NE; case OP_In: return CMP_IN; case OP_NotIn: return CMP_NOTIN;
        case OP_Is: return CMP_IS; default: return CMP_ISNOT;
    }
}

static Expr *constant(Cv *c, PyNode *n){
    PyConst *k=n->k; int line=n->line;
    switch(k->kind){
        case PC_None: return enew(EXPR_NONE,line);
        case PC_True: return enew(EXPR_TRUE,line);
        case PC_False: return enew(EXPR_FALSE,line);
        case PC_Int:{
            Expr *e=enew(EXPR_LITERAL,line); e->tok=tok(T_NUMBER,k->text,strlen(k->text),line);
            const char *s=k->text; int base=10;
            if(s[0]=='0' && (s[1]=='x'||s[1]=='X')){ base=16; s+=2; } else if(s[0]=='0' && (s[1]=='o'||s[1]=='O')){ base=8; s+=2; } else if(s[0]=='0' && (s[1]=='b'||s[1]=='B')){ base=2; s+=2; }
            uint64_t v=0;
            for(;*s;s++){ int d= *s>='0'&&*s<='9' ? *s-'0' : *s>='a'&&*s<='f' ? *s-'a'+10 : *s>='A'&&*s<='F' ? *s-'A'+10 : 99; if(d>=base) break;
                if(v>(UINT64_MAX-(uint64_t)d)/(uint64_t)base) fail(c,line,"integer literal too large: ints are 64-bit");
                v=v*(uint64_t)base+(uint64_t)d; }
            if(v>(uint64_t)INT64_MAX) fail(c,line,"integer literal too large: ints are 64-bit");
            e->tok->i=(int64_t)v;
            return e; }
        case PC_Float:{
            Expr *e=enew(EXPR_LITERAL,line); e->tok=tok(T_NUMBER,k->text,strlen(k->text),line);
            e->tok->f=strtod(k->text,NULL); e->tok->is_float=1;
            return e; }
        case PC_Str: case PC_Bytes:{
            Buf b={0}; bput(&b,"",0); const_text(&b,k);
            Expr *e=str_lit(b.s,b.n,line); free(b.s);
            if(k->kind==PC_Bytes) e->tok->i=2;                      /* b"...": bytes */
            return e; }
        case PC_Ellipsis: fail(c,line,"`...` is only supported as a statement and in tuple[T, ...] in compiled code");
        case PC_Complex:{                                       /* 2j: complex(0.0, 2.0) */
            Expr *e=enew(EXPR_CALL,line); e->a=name_x("complex",line);
            Expr *z=enew(EXPR_LITERAL,line); z->tok=tok(T_NUMBER,"0.0",3,line); z->tok->is_float=1; z->tok->f=0.0; item(e,z);
            Expr *im=enew(EXPR_LITERAL,line); im->tok=tok(T_NUMBER,k->text,strlen(k->text),line); im->tok->is_float=1; im->tok->f=strtod(k->text,NULL); item(e,im);
            return e; }
        default: fail(c,line,"unsupported constant in compiled code");
    }
}

/* f"...": "" + str(x) + ("%<printf spec>\0%<spec>" % (y)) + "..." - the pieces the compiler formats */
static Expr *fstring(Cv *c, PyNode *n){
    int line=n->line, emitted=0;
    Expr *acc=NULL;
    Buf lit={0}; bput(&lit,"",0);
    #define ADD(piece) do{ Expr *p_=(piece); if(!acc) acc=p_; else { Expr *b_=enew(EXPR_BINARY,line); b_->op=T_PLUS; b_->a=acc; b_->b=p_; acc=b_; } }while(0)
    #define FLUSH() do{ ADD(str_lit(lit.s,lit.n,line)); emitted=1; lit.n=0; lit.s[0]=0; }while(0)
    for(int i=0;i<n->L[0].n;i++){
        PyNode *v=n->L[0].v[i];
        if(v->kind==PK_Constant){ const_text(&lit,v->k); continue; }
        if(v->kind!=PK_FormattedValue) fail(c,v->line,"unsupported f-string part");
        if(!emitted || lit.n>0) FLUSH();
        Expr *x=ex(c,v->n[0]);
        int conv=v->op;                                     /* -1, 's', 'r', 'a' */
        if(conv=='r' || conv=='a') x=call1(conv=='r' ? "\001repr" : "\001ascii",x,line);  /* (\001: the builtin, whatever the module calls repr / str) */
        if(v->n[1]){                                        /* a format spec */
            Buf sp={0}; bput(&sp,"",0); Expr *nested[2]; int nn=0;
            for(int j=0;j<v->n[1]->L[0].n;j++){ PyNode *q=v->n[1]->L[0].v[j];
                if(q->kind==PK_FormattedValue && !q->n[1] && q->op<0 && nn<2){ nested[nn++]=ex(c,q->n[0]); bput(&sp,"\001",1); continue; }   /* {w}: its value, at \001 */
                if(q->kind!=PK_Constant) fail(c,q->line,"a replacement field inside a format spec is only supported as the width or precision in compiled code");
                const_text(&sp,q->k); }
            if(sp.n>60) fail(c,line,"format spec too long");
            char pf[80]; size_t pn=sp.n; memcpy(pf,sp.s,pn+1);
            if(pn>0 && (pf[0]=='<'||pf[0]=='>')){            /* the interpreter's printf form: "<5" left ("-5"), ">5" right */
                int left=pf[0]=='<';
                memmove(pf,pf+1,pn); pn--;
                if(left){ memmove(pf+1,pf,pn+1); pf[0]='-'; pn++; }
            }
            if(pn>0 && !((pf[pn-1]>='a'&&pf[pn-1]<='z')||(pf[pn-1]>='A'&&pf[pn-1]<='Z'))){ pf[pn++]='s'; pf[pn]=0; }
            /* "%<printf spec>", a NUL, "%<Python spec>" (i=1 marks it): one literal carries both */
            char fb[160]; int fl=snprintf(fb,80,"%%%s",pf);
            fl+=1+snprintf(fb+fl+1,sizeof(fb)-(size_t)fl-1,"%%%s",sp.s);
            free(sp.s);
            Expr *f=str_lit(fb,(size_t)fl,line); f->tok->i=1;
            for(int k=0;k<nn;k++) item(f,nested[k]);            /* the {w} / {p} of the spec, in order */
            Expr *m=enew(EXPR_BINARY,line); m->op=T_PERCENT; m->a=f; m->b=x;
            ADD(m);
        } else ADD(conv=='r'||conv=='a' ? x : call1("\001str",x,line));
        emitted=1;
    }
    if(!emitted || lit.n>0) FLUSH();
    free(lit.s);
    #undef FLUSH
    #undef ADD
    return acc;
}

/* comprehension variables: a name or a tuple of names */
static Expr *target(Cv *c, PyNode *n);
static void comp_vars(Cv *c, CompClause *cl, PyNode *t){
    #define ADDVAR(nm) do{ cl->vars=(char**)xrealloc(cl->vars,sizeof(char*)*(size_t)(cl->nvars+1)); cl->vars[cl->nvars++]=xstrdup2(nm); }while(0)
    if(t->kind==PK_Name){ ADDVAR(t->id[0]); return; }
    if((t->kind==PK_Tuple || t->kind==PK_List) && t->L[0].n==2 && t->L[0].v[0]->kind!=PK_Starred && t->L[0].v[1]->kind!=PK_Starred){
        for(int k=0;k<2;k++){ PyNode *v=t->L[0].v[k];                 /* i, (a, b): i, and a hidden one unpacked */
            if(v->kind==PK_Name){ ADDVAR(v->id[0]); continue; }
            char h[32]; snprintf(h,sizeof h,".t%d",++c->hidden); ADDVAR(h);
            if(k) cl->target2=target(c,v); else cl->target=target(c,v); }
        return; }
    char hid[32]; snprintf(hid,sizeof hid,".t%d",++c->hidden);      /* anything else: a hidden variable, unpacked into the target */
    ADDVAR(hid); cl->target=target(c,t);
    #undef ADDVAR
}
static Expr *comprehension(Cv *c, PyNode *n, int kind){
    Expr *e=enew(EXPR_COMPREHENSION,n->line); e->comp_kind=kind;
    e->a=ex(c,n->n[0]);
    if(kind=='D') e->b=ex(c,n->n[1]);
    for(int i=0;i<n->L[0].n;i++){ PyNode *g=n->L[0].v[i];
        if(e->nclause==e->ccap){ e->ccap=e->ccap?e->ccap*2:4; e->clauses=(CompClause*)xrealloc(e->clauses,sizeof(CompClause)*(size_t)e->ccap); }
        CompClause *cl=&e->clauses[e->nclause++]; memset(cl,0,sizeof *cl); cl->is_async=g->op;
        comp_vars(c,cl,g->n[0]);
        cl->iter=ex(c,g->n[1]);
        for(int j=0;j<g->L[0].n;j++){ cl->conds=(Expr**)xrealloc(cl->conds,sizeof(Expr*)*(size_t)(cl->ncond+1)); cl->conds[cl->ncond++]=ex(c,g->L[0].v[j]); }
    }
    return e;
}

static int before(PyNode *a, PyNode *b){ return a->line<b->line || (a->line==b->line && a->col<b->col); }
static Expr *call(Cv *c, PyNode *n){
    Expr *e=enew(EXPR_CALL,n->line); e->a=ex(c,n->n[0]);
    PyList *args=&n->L[0], *kws=&n->L[1];
    int i=0, j=0;
    while(i<args->n || j<kws->n){                          /* in source order */
        int pos= j>=kws->n || (i<args->n && before(args->v[i],kws->v[j]));
        Expr *x;
        if(pos){ PyNode *a=args->v[i++];
            if(a->kind==PK_Starred){ x=ex(c,a->n[0]); x->akind=1; }
            else { x=ex(c,a); x->akind=0; }
        } else { PyNode *k=kws->v[j++];
            x=ex(c,k->n[0]);
            if(k->id[0]){ x->akind=3; x->kw=xstrdup2(k->id[0]); } else x->akind=2;
        }
        item(e,x);
    }
    return e;
}

static Expr *seq(Cv *c, PyNode *n, ExprKind k){
    Expr *e=enew(k,n->line);
    for(int i=0;i<n->L[0].n;i++){ PyNode *v=n->L[0].v[i];
        if(v->kind==PK_Starred){ Expr *x=ex(c,v->n[0]); x->akind=1; item(e,x); continue; }    /* [*xs, y]: akind 1 */
        item(e,ex(c,v)); }
    return e;
}

static Expr *ex(Cv *c, PyNode *n){
    int line=n->line;
    switch(n->kind){
        case PK_Constant: return constant(c,n);
        case PK_Name: return name_x(n->id[0],line);
        case PK_JoinedStr: return fstring(c,n);
        case PK_BinOp:{
            int op=tok_op(n->op);
            if(!op) fail(c,line,"the @ operator is not supported in compiled code");
            Expr *e=enew(EXPR_BINARY,line); e->op=(TokKind)op; e->a=ex(c,n->n[0]); e->b=ex(c,n->n[1]); return e; }
        case PK_UnaryOp:{
            Expr *e=enew(EXPR_UNARY,line); e->op= n->op==OP_UAdd?T_PLUS : n->op==OP_USub?T_MINUS : n->op==OP_Invert?T_TILDE : T_NOT; e->a=ex(c,n->n[0]); return e; }
        case PK_BoolOp:{
            Expr *a=ex(c,n->L[0].v[0]);
            for(int i=1;i<n->L[0].n;i++){ Expr *e=enew(EXPR_BOOL,line); e->op= n->op==OP_And?T_AND:T_OR; e->a=a; e->b=ex(c,n->L[0].v[i]); a=e; }
            return a; }
        case PK_Compare:{
            Expr *e=enew(EXPR_COMPARE,line);
            item(e,ex(c,n->n[0]));
            for(int i=0;i<n->L[1].n;i++){ Expr *r=ex(c,n->L[1].v[i]); r->akind=cmp_code(n->L[0].v[i]->op); item(e,r); }   /* (L[0]: the operators) */
            return e; }
        case PK_IfExp:{ Expr *e=enew(EXPR_TERNARY,line); e->a=ex(c,n->n[0]); e->b=ex(c,n->n[1]); e->c=ex(c,n->n[2]); return e; }
        case PK_Call: return call(c,n);
        case PK_Attribute:{ Expr *e=enew(EXPR_ATTRIBUTE,line); e->a=ex(c,n->n[0]); e->name=xstrdup2(n->id[0]); return e; }
        case PK_Subscript:{
            PyNode *s=n->n[1];
            if(s->kind==PK_Slice){ Expr *e=enew(EXPR_SLICE,line); e->a=ex(c,n->n[0]);
                e->b=s->n[0]?ex(c,s->n[0]):NULL; e->c=s->n[1]?ex(c,s->n[1]):NULL; e->d=s->n[2]?ex(c,s->n[2]):NULL; return e; }
            Expr *e=enew(EXPR_INDEX,line); e->a=ex(c,n->n[0]); e->b=ex(c,s); return e; }
        case PK_List: return seq(c,n,EXPR_LIST);
        case PK_Tuple: return seq(c,n,EXPR_TUPLE);
        case PK_Set: return seq(c,n,EXPR_SET);
        case PK_Dict:{
            Expr *e=enew(EXPR_DICT,line);
            for(int i=0;i<n->L[0].n;i++){
                if(!n->L[0].v[i]){ Expr *d=ex(c,n->L[1].v[i]); d->akind=2; item(e,d); push(&e->vals,&e->vcount,&e->vcap,enew(EXPR_NONE,line)); continue; }   /* {**d}: the key is d, akind 2 */
                item(e,ex(c,n->L[0].v[i])); push(&e->vals,&e->vcount,&e->vcap,ex(c,n->L[1].v[i])); }
            return e; }
        case PK_ListComp: return comprehension(c,n,'L');
        case PK_SetComp: return comprehension(c,n,'S');
        case PK_DictComp: return comprehension(c,n,'D');
        case PK_GeneratorExp: return comprehension(c,n,'G');
        case PK_Lambda:{
            Expr *e=enew(EXPR_LAMBDA,line); PyNode *a=n->n[0];
            int npos=a->L[0].n+a->L[1].n, nd=a->L[4].n, total=npos+(a->n[0]?1:0)+a->L[2].n+(a->n[1]?1:0);
            e->edefaults=MPY_NEW_ARR(Expr*,total>0?total:1); e->estar=e->edstar=-1;
            #define LPARAM(nm,dflt) do{ e->eparams=(char**)xrealloc(e->eparams,sizeof(char*)*(size_t)(e->neparam+1)); e->edefaults[e->neparam]=(dflt)?ex(c,dflt):NULL; e->eparams[e->neparam++]=xstrdup2(nm); }while(0)
            for(int i=0;i<npos;i++){ PyNode *p= i<a->L[0].n ? a->L[0].v[i] : a->L[1].v[i-a->L[0].n]; LPARAM(p->id[0], i>=npos-nd ? a->L[4].v[i-(npos-nd)] : NULL); }
            if(a->n[0]){ e->estar=e->neparam; LPARAM(a->n[0]->id[0],NULL); }
            e->ekwonly=e->neparam;
            for(int i=0;i<a->L[2].n;i++) LPARAM(a->L[2].v[i]->id[0],a->L[3].v[i]);
            if(a->n[1]){ e->edstar=e->neparam; LPARAM(a->n[1]->id[0],NULL); }
            #undef LPARAM
            e->a=ex(c,n->n[1]);
            return e; }
        case PK_Await:{ Expr *e=enew(EXPR_AWAIT,line); e->a=ex(c,n->n[0]); return e; }
        case PK_Yield: case PK_YieldFrom:{                       /* x = yield v: what send() gives */
            if(!c->def) fail(c,line,"'yield' outside function");
            Expr *e=enew(EXPR_YIELD,line); if(n->n[0]) e->a=ex(c,n->n[0]); if(n->kind==PK_YieldFrom) e->akind=7;
            c->def->yield_expr=1; return e; }
        case PK_NamedExpr:{ Expr *e=enew(EXPR_WALRUS,line); e->name=xstrdup2(n->n[0]->id[0]); e->b=name_x(e->name,line); e->a=ex(c,n->n[1]); return e; }
        case PK_Starred: fail(c,line,"*unpacking here is not supported in compiled code");
        case PK_TemplateStr: fail(c,line,"template strings are not supported in compiled code");
        case PK_Slice: fail(c,line,"a slice inside a tuple index is not supported in compiled code");
        default: fail(c,line,"unsupported expression (%s) in compiled code",py_kind_name(n->kind));
    }
}

/* a type annotation: as an expression, with `...` (tuple[T, ...]) as the name "..." */
static Expr *ann(Cv *c, PyNode *n){
    if(!n) return NULL;
    if(n->kind==PK_Constant && n->k->kind==PC_Ellipsis) return name_x("...",n->line);
    if(n->kind==PK_Subscript){ Expr *e=enew(EXPR_INDEX,n->line); e->a=ann(c,n->n[0]); e->b=ann(c,n->n[1]); return e; }
    if(n->kind==PK_Tuple || n->kind==PK_List){ Expr *e=enew(n->kind==PK_Tuple?EXPR_TUPLE:EXPR_LIST,n->line); for(int i=0;i<n->L[0].n;i++) item(e,ann(c,n->L[0].v[i])); return e; }
    if(n->kind==PK_BinOp && n->op==OP_BitOr){ Expr *e=enew(EXPR_BINARY,n->line); e->op=T_PIPE; e->a=ann(c,n->n[0]); e->b=ann(c,n->n[1]); return e; }
    return ex(c,n);
}

/* ---------------------------------------------------------------- statements */
static void block(Cv *c, PyList *l, Stmt *into, int orelse);

static Stmt *snew(StmtKind k, const char *name, PyNode *n){ Stmt *s=stmt_new(k,name,n->line); s->py=n; return s; }
static void add_target(Stmt *s, Expr *t){ s->targets=(Expr**)xrealloc(s->targets,sizeof(Expr*)*(size_t)(s->ntargets+1)); s->targets[s->ntargets++]=t; }
static const char *first_name(PyNode *n){        /* the leftmost name of an expression (an assignment's, a decorator's) */
    while(n){
        switch(n->kind){
            case PK_Name: return n->id[0];
            case PK_Attribute: case PK_Subscript: case PK_Call: case PK_Starred: n=n->n[0]; break;
            case PK_Tuple: case PK_List: n=n->L[0].n?n->L[0].v[0]:NULL; break;
            default: return NULL;
        }
    }
    return NULL;
}
/* an assignment target: a name, attribute, item, slice, or a tuple / list of them */
static Expr *target(Cv *c, PyNode *n){
    switch(n->kind){
        case PK_Name: case PK_Attribute: case PK_Subscript: return ex(c,n);
        case PK_Tuple: case PK_List:{
            Expr *e=enew(n->kind==PK_Tuple?EXPR_TUPLE:EXPR_LIST,n->line);
            for(int i=0;i<n->L[0].n;i++){ PyNode *v=n->L[0].v[i];
                if(v->kind==PK_Starred){ Expr *x=target(c,v->n[0]); x->akind=1; item(e,x); continue; }   /* a, *b = ...: akind 1 */
                item(e,target(c,v)); }
            return e; }
        default: fail(c,n->line,"cannot assign to this in compiled code");
    }
}

/* [T, U] of a def, class or type alias: their names */
static void type_params(Stmt *s, PyNode *n){
    for(int i=0;i<n->L[2].n;i++){ s->tparams=(char**)xrealloc(s->tparams,sizeof(char*)*(size_t)(s->ntparams+1)); s->tparams[s->ntparams++]=xstrdup2(n->L[2].v[i]->id[0]); }
}
static Stmt *def(Cv *c, PyNode *n){
    Stmt *s=snew(STMT_FUNCTION_DEF,n->id[0],n);
    s->is_async= n->kind==PK_AsyncFunctionDef;
    type_params(s,n);
    for(int i=0;i<n->L[1].n;i++){ PyNode *d=n->L[1].v[i]; const char *nm=first_name(d);
        if(!nm) fail(c,d->line,"unsupported decorator in compiled code");
        stmt_add_decorator(s,nm,ex(c,d)); }
    PyNode *a=n->n[0];
    int npos=a->L[0].n+a->L[1].n, total=npos+(a->n[0]?1:0)+a->L[2].n+(a->n[1]?1:0);
    s->pdefaults=MPY_NEW_ARR(Expr*,total>0?total:1);
    for(int i=0;i<total;i++) s->pdefaults[i]=NULL;
    #define PARAM(argnode,dflt) do{ PyNode *an_=(argnode); int ix_=s->param_count; name_add_unique(&s->params,&s->param_count,&s->param_cap,an_->id[0]); \
        if(s->param_count==ix_) fail(c,an_->line,"duplicate parameter '%s'",an_->id[0]); \
        if(an_->n[0]) stmt_set_annotation(s,ix_,ann(c,an_->n[0])); if(dflt) s->pdefaults[ix_]=ex(c,dflt); }while(0)
    int nd=a->L[4].n;
    for(int i=0;i<npos;i++){ PyNode *p= i<a->L[0].n ? a->L[0].v[i] : a->L[1].v[i-a->L[0].n];
        PARAM(p, i>=npos-nd ? a->L[4].v[i-(npos-nd)] : NULL); }
    if(a->n[0]){ s->star_index=s->param_count; PARAM(a->n[0],NULL); }
    else if(a->L[2].n) s->kwonly_index=s->param_count;      /* a bare `*` */
    for(int i=0;i<a->L[2].n;i++) PARAM(a->L[2].v[i],a->L[3].v[i]);
    if(a->n[1]){ s->dstar_index=s->param_count; PARAM(a->n[1],NULL); }
    #undef PARAM
    s->returns=ann(c,n->n[1]);
    Stmt *save=c->def; c->def=s;
    block(c,&n->L[0],s,0);
    c->def=save;
    return s;
}

static Stmt *klass(Cv *c, PyNode *n){
    Stmt *s=snew(STMT_CLASS_DEF,n->id[0],n);
    type_params(s,n);
    for(int i=0;i<n->L[1].n;i++){ PyNode *d=n->L[1].v[i]; const char *nm=first_name(d);
        if(!nm) fail(c,d->line,"unsupported decorator in compiled code");
        stmt_add_decorator(s,nm,ex(c,d)); }
    if(n->L[4].n) fail(c,n->line,"class keywords (metaclass=...) are not supported in compiled code");
    int nb=0;
    for(int i=0;i<n->L[3].n;i++){ PyNode *b=n->L[3].v[i];
        if(b->kind==PK_Subscript && b->n[0]->kind==PK_Name && (!strcmp(b->n[0]->id[0],"Generic")||!strcmp(b->n[0]->id[0],"Protocol"))){   /* typing: Generic[T]: T is the class's */
            PyNode *a=b->n[1]; int na= a->kind==PK_Tuple ? a->L[0].n : 1;
            for(int k=0;k<na;k++){ PyNode *x= a->kind==PK_Tuple ? a->L[0].v[k] : a;
                if(x->kind==PK_Name){ s->tparams=(char**)xrealloc(s->tparams,sizeof(char*)*(size_t)(s->ntparams+1)); s->tparams[s->ntparams++]=xstrdup2(x->id[0]); } }
            continue; }
        if(b->kind==PK_Name && (!strcmp(b->id[0],"Generic")||!strcmp(b->id[0],"Protocol")||!strcmp(b->id[0],"object"))) continue;
        char *bn;
        if(b->kind==PK_Name) bn=xstrdup2(b->id[0]);
        else if(b->kind==PK_Attribute){                                  /* module.Base, package.module.Base */
            char buf[512]; buf[0]=0; PyNode *x=b; const char *parts[16]; int np=0;
            while(x->kind==PK_Attribute && np<16){ parts[np++]=x->id[0]; x=x->n[0]; }
            if(x->kind!=PK_Name || np==16) fail(c,b->line,"the base class must be a name in compiled code");
            snprintf(buf,sizeof buf,"%s",x->id[0]);
            for(int k=np-1;k>=0;k--){ size_t l=strlen(buf); snprintf(buf+l,sizeof buf-l,".%s",parts[k]); }
            bn=xstrdup2(buf); }
        else fail(c,b->line,"the base class must be a name in compiled code");
        if(!nb++) s->name2=bn;
        name_add_unique(&s->params,&s->param_count,&s->param_cap,bn); }   /* (all of them: several bases) */
    block(c,&n->L[0],s,0);
    return s;
}

static char *dotted(PyNode *alias){ return xstrdup2(alias->id[0]); }

/* one statement -> into's body (or orelse): an import of several modules is several */
/* a match statement's pattern */
static Expr *pattern(Cv *c, PyNode *n){
    int line=n->line; Expr *e=enew(EXPR_PATTERN,line);
    switch(n->kind){
        case PK_MatchValue: e->akind=PAT_VALUE; e->a=ex(c,n->n[0]); return e;
        case PK_MatchSingleton: e->akind=PAT_SINGLETON;
            e->a=enew(n->k->kind==PC_True?EXPR_TRUE:n->k->kind==PC_False?EXPR_FALSE:EXPR_NONE,line); return e;
        case PK_MatchAs: e->akind=PAT_AS; if(n->n[0]) e->a=pattern(c,n->n[0]); if(n->id[0]) e->name=xstrdup2(n->id[0]); return e;
        case PK_MatchOr: e->akind=PAT_OR; for(int i=0;i<n->L[0].n;i++) item(e,pattern(c,n->L[0].v[i])); return e;
        case PK_MatchSequence: e->akind=PAT_SEQ; for(int i=0;i<n->L[0].n;i++) item(e,pattern(c,n->L[0].v[i])); return e;
        case PK_MatchStar: e->akind=PAT_STAR; if(n->id[0]) e->name=xstrdup2(n->id[0]); return e;
        case PK_MatchMapping: e->akind=PAT_MAP;
            for(int i=0;i<n->L[0].n;i++){ item(e,ex(c,n->L[0].v[i])); push(&e->vals,&e->vcount,&e->vcap,pattern(c,n->L[1].v[i])); }
            if(n->id[0]) e->name=xstrdup2(n->id[0]); return e;
        case PK_MatchClass: e->akind=PAT_CLASS; e->a=ex(c,n->n[0]);
            for(int i=0;i<n->L[0].n;i++) item(e,pattern(c,n->L[0].v[i]));
            for(int i=0;i<n->L[2].n;i++){ Expr *p=pattern(c,n->L[2].v[i]); p->kw=xstrdup2(n->L[1].v[i]->id[0]); item(e,p); }
            return e;
        default: fail(c,line,"unsupported pattern (%s)",py_kind_name(n->kind));
    }
}
static void stmt(Cv *c, PyNode *n, Stmt *into, int orelse){
    #define OUT(st) do{ Stmt *o_=(st); if(orelse) stmt_add_orelse(into,o_); else stmt_add_body(into,o_); }while(0)
    int line=n->line;
    switch(n->kind){
        case PK_FunctionDef: case PK_AsyncFunctionDef: OUT(def(c,n)); return;
        case PK_ClassDef: OUT(klass(c,n)); return;
        case PK_Return:{ Stmt *s=snew(STMT_RETURN,NULL,n); if(n->n[0]) s->expr=ex(c,n->n[0]); OUT(s); return; }
        case PK_Delete:{ Stmt *s=snew(STMT_DEL,NULL,n); for(int i=0;i<n->L[0].n;i++) add_target(s,target(c,n->L[0].v[i])); OUT(s); return; }
        case PK_Assign:{
            Stmt *s=snew(STMT_ASSIGN,first_name(n->L[0].v[0]),n);
            for(int i=0;i<n->L[0].n;i++) add_target(s,target(c,n->L[0].v[i]));
            s->value=ex(c,n->n[0]);
            OUT(s); return; }
        case PK_AugAssign:{
            int op=aug_op(n->op);
            if(!op) fail(c,line,"the @= operator is not supported in compiled code");
            Stmt *s=snew(STMT_ASSIGN,first_name(n->n[0]),n);
            add_target(s,target(c,n->n[0])); s->aug=op; s->value=ex(c,n->n[1]);
            OUT(s); return; }
        case PK_AnnAssign:{
            if(!n->n[2] && n->n[0]->kind!=PK_Name){ OUT(snew(STMT_PASS,NULL,n)); return; }   /* self.x: int - nothing to do */
            Stmt *s=snew(n->n[2]?STMT_ASSIGN:STMT_EXPR,first_name(n->n[0]),n);
            add_target(s,target(c,n->n[0])); s->ann=ann(c,n->n[1]);
            if(n->n[2]) s->value=ex(c,n->n[2]);
            else { s->ann_only=1; s->expr=s->targets[0]; }
            OUT(s); return; }
        case PK_For: case PK_AsyncFor:{
            Stmt *s=snew(STMT_FOR,NULL,n); PyNode *t=n->n[0]; s->is_async=n->kind==PK_AsyncFor;
            int tuple= t->kind==PK_Tuple || t->kind==PK_List, starred=0;
            if(tuple) for(int i=0;i<t->L[0].n;i++) if(t->L[0].v[i]->kind==PK_Starred) starred=1;
            Stmt *unpack[16]; int nun=0;
            #define HIDDEN(node) do{ char hid_[32]; snprintf(hid_,sizeof hid_,".t%d",++c->hidden);    /* a hidden variable, unpacked at the top of the body */ \
                Stmt *u_=snew(STMT_ASSIGN,first_name(node),n); add_target(u_,target(c,node)); u_->value=name_x(hid_,line); \
                if(nun<16) unpack[nun++]=u_; name_add_unique(&s->params,&s->param_count,&s->param_cap,hid_); }while(0)
            if(t->kind==PK_Name) name_add_unique(&s->params,&s->param_count,&s->param_cap,t->id[0]);
            else if(tuple && !starred && t->L[0].n<=16)          /* for i, (a, b) in ...: i, and a hidden one for (a, b) */
                for(int i=0;i<t->L[0].n;i++){ PyNode *v=t->L[0].v[i]; if(v->kind==PK_Name) name_add_unique(&s->params,&s->param_count,&s->param_cap,v->id[0]); else HIDDEN(v); }
            else HIDDEN(t);                                      /* for a, *b in ..., for x.attr in ... */
            #undef HIDDEN
            if(s->param_count==1) s->name=xstrdup2(s->params[0]);
            s->expr=ex(c,n->n[1]);
            for(int i=0;i<nun;i++) stmt_add_body(s,unpack[i]);
            block(c,&n->L[0],s,0); block(c,&n->L[1],s,1);
            OUT(s); return; }
        case PK_While:{ Stmt *s=snew(STMT_WHILE,NULL,n); s->expr=ex(c,n->n[0]); block(c,&n->L[0],s,0); block(c,&n->L[1],s,1); OUT(s); return; }
        case PK_If:{ Stmt *s=snew(STMT_IF,NULL,n); s->expr=ex(c,n->n[0]);
            int ct=compiled_test(n->n[0]);                  /* if not sys._compiled: the interpreter's part (not made: may hold what compiled code has not) */
            if(ct>=0) block(c,&n->L[0],s,0);
            if(ct<=0) block(c,&n->L[1],s,1);
            OUT(s); return; }
        case PK_With: case PK_AsyncWith:{
            Stmt *s=snew(STMT_WITH,NULL,n); s->is_async=n->kind==PK_AsyncWith;
            s->withas=MPY_NEW_ARR(char*,n->L[3].n>0?n->L[3].n:1); s->withtgt=MPY_NEW_ARR(Expr*,n->L[3].n>0?n->L[3].n:1);
            for(int i=0;i<n->L[3].n;i++){ PyNode *w=n->L[3].v[i];
                add_target(s,ex(c,w->n[0]));
                s->withas[i]=NULL; s->withtgt[i]=NULL;
                if(w->n[1]){ if(w->n[1]->kind==PK_Name) s->withas[i]=xstrdup2(w->n[1]->id[0]); else s->withtgt[i]=ex(c,w->n[1]); } }
            s->expr=s->targets[0];
            block(c,&n->L[0],s,0);
            OUT(s); return; }
        case PK_Raise:{ Stmt *s=snew(STMT_RAISE,NULL,n); if(n->n[0]) s->expr=ex(c,n->n[0]); if(n->n[1]) s->expr2=ex(c,n->n[1]); OUT(s); return; }   /* expr2: `from` cause */
        case PK_Try: case PK_TryStar:{
            Stmt *s=snew(STMT_TRY,NULL,n); s->star=n->kind==PK_TryStar;
            block(c,&n->L[0],s,0);
            for(int i=0;i<n->L[3].n;i++){ PyNode *h=n->L[3].v[i];
                Stmt *b=snew(STMT_BLOCK,NULL,h); b->block_tag=1;
                if(h->n[0]) b->expr=ex(c,h->n[0]);
                if(h->id[0]) b->name=xstrdup2(h->id[0]);
                block(c,&h->L[0],b,0);
                stmt_add_orelse(s,b); }
            if(n->L[1].n){ Stmt *b=snew(STMT_BLOCK,"else",n); b->block_tag=2; block(c,&n->L[1],b,0); stmt_add_orelse(s,b); }
            if(n->L[2].n){ Stmt *b=snew(STMT_BLOCK,"finally",n); b->block_tag=3; block(c,&n->L[2],b,0); stmt_add_orelse(s,b); }
            OUT(s); return; }
        case PK_Assert:{ Stmt *s=snew(STMT_ASSERT,NULL,n); s->expr=ex(c,n->n[0]); if(n->n[1]) s->expr2=ex(c,n->n[1]); OUT(s); return; }
        case PK_Import:
            for(int i=0;i<n->L[3].n;i++){ PyNode *al=n->L[3].v[i];
                Stmt *s=snew(STMT_IMPORT,NULL,n);
                s->name2=dotted(al); s->module=xstrdup2(s->name2);
                if(al->id[1]){ s->name=xstrdup2(al->id[1]); s->block_tag=1; }
                else { const char *dot=strchr(s->name2,'.'); s->name=dot?xstrndup2(s->name2,(int)(dot-s->name2)):xstrdup2(s->name2); }
                OUT(s); }
            return;
        case PK_ImportFrom:{
            Stmt *s=snew(STMT_FROM_IMPORT,NULL,n);
            s->module=n->id[0]?xstrdup2(n->id[0]):NULL; s->level=n->op>0?n->op:0;      /* (relative: resolved against the importing module's package) */
            s->nnames=n->L[3].n;
            s->names=MPY_NEW_ARR(char*,s->nnames>0?s->nnames:1); s->asnames=MPY_NEW_ARR(char*,s->nnames>0?s->nnames:1);
            for(int i=0;i<s->nnames;i++){ PyNode *al=n->L[3].v[i]; s->names[i]=xstrdup2(al->id[0]); s->asnames[i]=al->id[1]?xstrdup2(al->id[1]):NULL; }
            s->name2=xstrdup2(s->names[0]); s->name=xstrdup2(s->asnames[0]?s->asnames[0]:s->names[0]);
            OUT(s); return; }
        case PK_Global: case PK_Nonlocal:{
            Stmt *s=snew(n->kind==PK_Global?STMT_GLOBAL:STMT_NONLOCAL,NULL,n);
            for(int i=0;i<n->L[3].n;i++) name_add_unique(&s->params,&s->param_count,&s->param_cap,n->L[3].v[i]->id[0]);
            OUT(s); return; }
        case PK_Expr:{
            PyNode *v=n->n[0];
            if(v->kind==PK_Constant && v->k->kind==PC_Ellipsis){ OUT(snew(STMT_PASS,NULL,n)); return; }   /* `...` */
            if(v->kind==PK_Yield || v->kind==PK_YieldFrom){
                Stmt *s=snew(STMT_YIELD,NULL,n);
                if(v->n[0]) s->expr=ex(c,v->n[0]);
                if(v->kind==PK_YieldFrom) s->block_tag=7;
                OUT(s); return; }
            if(v->kind==PK_Call && v->n[0]->kind==PK_Name && !strcmp(v->n[0]->id[0],"setattr") && v->L[0].n==3 && !v->L[1].n
               && v->L[0].v[1]->kind==PK_Constant && v->L[0].v[1]->k->kind==PC_Str){       /* setattr(o, "name", v): o.name = v */
                Stmt *s=snew(STMT_ASSIGN,first_name(v->L[0].v[0]),n);
                Expr *t=enew(EXPR_ATTRIBUTE,line); t->a=ex(c,v->L[0].v[0]);
                Expr *nm=ex(c,v->L[0].v[1]); t->name=xstrdup2(nm->tok->text);
                add_target(s,t); s->value=ex(c,v->L[0].v[2]); OUT(s); return; }
            Stmt *s=snew(STMT_EXPR,NULL,n); s->expr=ex(c,v); OUT(s); return; }
        case PK_Pass: OUT(snew(STMT_PASS,NULL,n)); return;
        case PK_Break: OUT(snew(STMT_BREAK,NULL,n)); return;
        case PK_Continue: OUT(snew(STMT_CONTINUE,NULL,n)); return;
        case PK_Match:{                                     /* match: its cases (made into ifs by the type checker) */
            Stmt *s=snew(STMT_MATCH,NULL,n); s->expr=ex(c,n->n[0]);
            for(int i=0;i<n->L[3].n;i++){ PyNode *k=n->L[3].v[i];
                Stmt *cs=stmt_new(STMT_CASE,NULL,k->n[0]?k->n[0]->line:n->line); cs->py=k;
                cs->expr=pattern(c,k->n[0]); if(k->n[1]) cs->expr2=ex(c,k->n[1]);
                block(c,&k->L[0],cs,0);
                stmt_add_body(s,cs); }
            OUT(s); return; }
        case PK_TypeAlias:{ Stmt *s=snew(STMT_PASS,n->n[0]->id[0],n); s->is_alias=1; type_params(s,n); s->ann=ann(c,n->n[1]); OUT(s); return; }
        default: fail(c,line,"unsupported statement (%s) in compiled code",py_kind_name(n->kind));
    }
    #undef OUT
}
static void block(Cv *c, PyList *l, Stmt *into, int orelse){ for(int i=0;i<l->n;i++) stmt(c,l->v[i],into,orelse); }

/* ---------------------------------------------------------------- entry points */
static PyNode *parse(Cv *c, const char *src){
    PyParse pp;
    if(py_parse(&pp,c->path,src,strlen(src))){
        fprintf(stderr,"%s:%d: error: SyntaxError: %s\n",c->path?c->path:"<source>",pp.error_line,pp.error);
        return NULL;
    }
    return pp.mod;
}
Ast *py_front(const char *path, const char *src){
    Cv c; c.path=path; c.hidden=0; c.def=NULL;
    PyNode *mod=parse(&c,src);
    if(!mod) return NULL;
    Stmt *root=stmt_new(STMT_MODULE,"<module>",1); root->py=mod;
    if(setjmp(c.jb)) return NULL;
    block(&c,&mod->L[0],root,0);
    return root;
}
Expr *py_front_expr(const char *path, const char *text, int line){
    Cv c; c.path=path; c.hidden=0; c.def=NULL;
    size_t n=strlen(text); char *src=(char*)xmalloc(n+4); src[0]='('; memcpy(src+1,text,n); src[n+1]=')'; src[n+2]='\n'; src[n+3]=0;
    PyParse pp;
    (void)line;
    if(py_parse(&pp,path,src,n+3) || pp.mod->L[0].n!=1 || pp.mod->L[0].v[0]->kind!=PK_Expr){ free(src); return NULL; }
    if(setjmp(c.jb)){ free(src); return NULL; }
    Expr *e=ann(&c,pp.mod->L[0].v[0]->n[0]);
    (void)src;                                   /* (kept: the tree's text points into it) */
    return e;
}
Stmt *py_front_stmts(const char *path, const char *src, int line){
    Cv c; c.path=path; c.hidden=0; c.def=NULL;
    PyParse pp;
    if(py_parse(&pp,path,src,strlen(src))) return NULL;
    Stmt *box=stmt_new(STMT_BLOCK,NULL,line);
    if(setjmp(c.jb)) return NULL;
    block(&c,&pp.mod->L[0],box,0);
    return box;
}
Stmt *py_front_copy(const char *path, Stmt *s){
    Cv c; c.path=path; c.hidden=0; c.def=NULL;
    if(!s->py) return NULL;
    Stmt *box=stmt_new(STMT_BLOCK,NULL,s->line);
    if(setjmp(c.jb)) return NULL;
    stmt(&c,(PyNode*)s->py,box,0);
    return box->body_count==1 ? box->body[0] : NULL;
}
