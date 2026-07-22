/* ========================= Tree-based expression compiler =========================
   The expression grammar PARSES tokens into an `Expr` tree (parse_*), then a
   separate walker (emit_expr) generates bytecode from that tree. The public
   `expr()` is a thin parse+emit wrapper, so `compile_expr_ast` (which re-parses
   a token range) and the handful of statement helpers kept here are unaffected.

   Statement compilation itself lives entirely in compiler.c (AST-driven). This
   file only retains the pieces that compiler.c delegates back to over a token
   range: expression parsing, assignment targets, from-import, and print(). */

#include "compiler.h"
#include "lexer.h"

/* ---- node model ---- */
enum { CMP_LT, CMP_LE, CMP_GT, CMP_GE, CMP_EQ, CMP_NE, CMP_IN, CMP_NOTIN, CMP_IS, CMP_ISNOT };

static Expr *enew(ExprKind k, int line){ Expr *e=MPY_NEW0(Expr); e->kind=k; e->line=line; return e; }
static void ep_push(Expr ***arr, int *cnt, int *cap, Expr *v){
    if(*cnt==*cap){ *cap=*cap?*cap*2:4; *arr=(Expr**)xrealloc(*arr,sizeof(Expr*)*(size_t)*cap); }
    (*arr)[(*cnt)++]=v;
}
static CompClause *comp_add_clause(Expr *e){
    if(e->nclause==e->ccap){ e->ccap=e->ccap?e->ccap*2:4; e->clauses=(CompClause*)xrealloc(e->clauses,sizeof(CompClause)*(size_t)e->ccap); }
    CompClause *c=&e->clauses[e->nclause++]; c->vars=NULL; c->nvars=0; c->iter=NULL; c->conds=NULL; c->ncond=0; return c;
}

static Expr *parse_expr(Parser *p);
static Expr *parse_or_test(Parser *p);
static void  emit_expr(Parser *p, Expr *e);

int is_assignment(Parser *p){
    int i=p->pos; if(p->tv.v[i].kind!=T_NAME) return 0; i++;
    while(1){ if(p->tv.v[i].kind==T_DOT){ i++; if(p->tv.v[i].kind!=T_NAME)return 0; i++; }
        else if(p->tv.v[i].kind==T_LB){ int depth=1; i++; while(depth&&p->tv.v[i].kind!=T_EOF){ if(p->tv.v[i].kind==T_LB||p->tv.v[i].kind==T_LP||p->tv.v[i].kind==T_LC)depth++; else if(p->tv.v[i].kind==T_RB||p->tv.v[i].kind==T_RP||p->tv.v[i].kind==T_RC)depth--; i++; } }
        else break; }
    TokKind e=p->tv.v[i].kind;
    return e==T_ASSIGN||e==T_PLUS_ASSIGN||e==T_MINUS_ASSIGN||e==T_STAR_ASSIGN||e==T_SLASH_ASSIGN||e==T_PERCENT_ASSIGN||e==T_FLOOR_DIV_ASSIGN||e==T_POWER_ASSIGN||e==T_AMP_ASSIGN||e==T_PIPE_ASSIGN||e==T_CARET_ASSIGN||e==T_SHL_ASSIGN||e==T_SHR_ASSIGN||e==T_COLON;
}

/* ---- parser: build an Expr tree ---- */

/* Consumes `for VARS in IT [if C]...` clauses into a comprehension node. */
static void parse_comp_tail(Parser *p, Expr *comp){
    while(peek(p)->kind==T_FOR){
        p->pos++;
        CompClause *cl=comp_add_clause(comp);
        do{ Tok *v=need(p,T_NAME,"comprehension variable"); cl->vars=(char**)xrealloc(cl->vars,sizeof(char*)*(size_t)(cl->nvars+1)); cl->vars[cl->nvars++]=v->text; }while(match(p,T_COMMA));
        need(p,T_IN,"in");
        cl->iter=parse_or_test(p);                       /* or_test: `if` starts a filter, not a ternary */
        while(match(p,T_IF)){ Expr *c=parse_or_test(p); cl->conds=(Expr**)xrealloc(cl->conds,sizeof(Expr*)*(size_t)(cl->ncond+1)); cl->conds[cl->ncond++]=c; }
    }
}

static Expr *parse_lambda(Parser *p, int line){
    Expr *e=enew(EXPR_LAMBDA,line);
    if(peek(p)->kind!=T_COLON){ do{ Tok *a=need(p,T_NAME,"lambda param"); e->eparams=(char**)xrealloc(e->eparams,sizeof(char*)*(size_t)(e->neparam+1)); e->eparams[e->neparam++]=xstrdup2(a->text); }while(match(p,T_COMMA)); }
    need(p,T_COLON,":");
    e->a=parse_expr(p);
    return e;
}

static Expr *parse_call(Parser *p, Expr *callee, int line){
    Expr *e=enew(EXPR_CALL,line); e->a=callee;
    if(peek(p)->kind!=T_RP){
        do{
            if(peek(p)->kind==T_RP) break;                /* trailing comma */
            Expr *arg;
            if(match(p,T_STAR)){ arg=parse_expr(p); arg->akind=1; }
            else if(match(p,T_POWER)){ arg=parse_expr(p); arg->akind=2; }
            else if(peek(p)->kind==T_NAME && p->tv.v[p->pos+1].kind==T_ASSIGN){ Tok *n=need(p,T_NAME,"keyword name"); need(p,T_ASSIGN,"="); arg=parse_expr(p); arg->akind=3; arg->name=n->text; }
            else { arg=parse_expr(p); arg->akind=0; }
            ep_push(&e->items,&e->count,&e->cap,arg);
        }while(match(p,T_COMMA));
    }
    need(p,T_RP,")");
    return e;
}

static Expr *parse_subscript(Parser *p, Expr *obj, int line){
    Expr *lo=NULL,*hi=NULL,*step=NULL; int is_slice=0;
    if(peek(p)->kind!=T_COLON) lo=parse_expr(p);
    if(match(p,T_COLON)){
        is_slice=1;
        if(peek(p)->kind!=T_COLON && peek(p)->kind!=T_RB) hi=parse_expr(p);
        if(match(p,T_COLON)){ if(peek(p)->kind!=T_RB) step=parse_expr(p); }
    }
    need(p,T_RB,"]");
    if(is_slice){ Expr *e=enew(EXPR_SLICE,line); e->a=obj; e->b=lo; e->c=hi; e->d=step; return e; }
    Expr *e=enew(EXPR_INDEX,line); e->a=obj; e->b=lo; return e;
}

static Expr *parse_primary(Parser *p){
    Tok *t=peek(p); int line=t->line; Expr *e;
    if(match(p,T_NUMBER)){ e=enew(EXPR_LITERAL,line); e->tok=prev(p); }
    else if(match(p,T_STRING)){ e=enew(EXPR_LITERAL,line); e->tok=prev(p); }
    else if(match(p,T_TRUE)) e=enew(EXPR_TRUE,line);
    else if(match(p,T_FALSE)) e=enew(EXPR_FALSE,line);
    else if(match(p,T_NONE)) e=enew(EXPR_NONE,line);
    else if(match(p,T_NAME)){ e=enew(EXPR_NAME,line); e->name=prev(p)->text; }
    else if(match(p,T_LAMBDA)) e=parse_lambda(p,line);
    else if(match(p,T_LP)){
        if(match(p,T_RP)) e=enew(EXPR_TUPLE,line);                     /* () */
        else {
            Expr *first=parse_expr(p);
            if(peek(p)->kind==T_FOR){ e=enew(EXPR_COMPREHENSION,line); e->comp_kind='L'; e->a=first; parse_comp_tail(p,e); need(p,T_RP,")"); }
            else if(match(p,T_COMMA)){
                e=enew(EXPR_TUPLE,line); ep_push(&e->items,&e->count,&e->cap,first);
                if(!match(p,T_RP)){ do{ Expr *x=parse_expr(p); ep_push(&e->items,&e->count,&e->cap,x); }while(match(p,T_COMMA)); need(p,T_RP,")"); }
            }
            else { need(p,T_RP,")"); e=first; }
        }
    }
    else if(match(p,T_LB)){
        if(match(p,T_RB)) e=enew(EXPR_LIST,line);
        else {
            Expr *first=parse_expr(p);
            if(peek(p)->kind==T_FOR){ e=enew(EXPR_COMPREHENSION,line); e->comp_kind='L'; e->a=first; parse_comp_tail(p,e); need(p,T_RB,"]"); }
            else { e=enew(EXPR_LIST,line); ep_push(&e->items,&e->count,&e->cap,first); while(match(p,T_COMMA)){ if(peek(p)->kind==T_RB) break; Expr *x=parse_expr(p); ep_push(&e->items,&e->count,&e->cap,x); } need(p,T_RB,"]"); }
        }
    }
    else if(match(p,T_LC)){
        if(match(p,T_RC)) e=enew(EXPR_DICT,line);
        else {
            Expr *k=parse_expr(p);
            if(match(p,T_COLON)){                                       /* dict / dict-comp */
                Expr *v=parse_expr(p);
                if(peek(p)->kind==T_FOR){ e=enew(EXPR_COMPREHENSION,line); e->comp_kind='D'; e->a=k; e->b=v; parse_comp_tail(p,e); need(p,T_RC,"}"); }
                else { e=enew(EXPR_DICT,line); ep_push(&e->items,&e->count,&e->cap,k); ep_push(&e->vals,&e->vcount,&e->vcap,v);
                    while(match(p,T_COMMA)){ if(peek(p)->kind==T_RC) break; Expr *k2=parse_expr(p); need(p,T_COLON,":"); Expr *v2=parse_expr(p); ep_push(&e->items,&e->count,&e->cap,k2); ep_push(&e->vals,&e->vcount,&e->vcap,v2); } need(p,T_RC,"}"); }
            } else {                                                    /* set / set-comp */
                if(peek(p)->kind==T_FOR){ e=enew(EXPR_COMPREHENSION,line); e->comp_kind='S'; e->a=k; parse_comp_tail(p,e); need(p,T_RC,"}"); }
                else { e=enew(EXPR_SET,line); ep_push(&e->items,&e->count,&e->cap,k); while(match(p,T_COMMA)){ if(peek(p)->kind==T_RC) break; Expr *x=parse_expr(p); ep_push(&e->items,&e->count,&e->cap,x); } need(p,T_RC,"}"); }
            }
        }
    }
    else { fprintf(stderr,"parse error at line %d: expected expression\n",line); exit(1); }
    /* postfix trailers */
    for(;;){
        if(match(p,T_LP)) e=parse_call(p,e,line);
        else if(match(p,T_DOT)){ Tok *n=need(p,T_NAME,"attribute name"); Expr *a=enew(EXPR_ATTRIBUTE,n->line); a->a=e; a->name=n->text; e=a; }
        else if(match(p,T_LB)) e=parse_subscript(p,e,line);
        else break;
    }
    return e;
}
static Expr *parse_unary(Parser *p){
    if(match(p,T_MINUS)){ int line=prev(p)->line; Expr *e=enew(EXPR_UNARY,line); e->op=T_MINUS; e->a=parse_unary(p); return e; }
    if(match(p,T_PLUS)) return parse_unary(p);                          /* unary plus: no-op */
    if(match(p,T_TILDE)){ int line=prev(p)->line; Expr *e=enew(EXPR_UNARY,line); e->op=T_TILDE; e->a=parse_unary(p); return e; }
    if(match(p,T_NOT)){ int line=prev(p)->line; Expr *e=enew(EXPR_UNARY,line); e->op=T_NOT; e->a=parse_unary(p); return e; }
    return parse_primary(p);
}
static Expr *bin_left(Parser *p, Expr *(*sub)(Parser*), const TokKind *ops, int nops){
    Expr *a=sub(p);
    for(;;){ TokKind k=peek(p)->kind; int m=0; for(int i=0;i<nops;i++) if(k==ops[i]){ m=1; break; } if(!m) break;
        int line=peek(p)->line; p->pos++; Expr *b=sub(p); Expr *e=enew(EXPR_BINARY,line); e->op=k; e->a=a; e->b=b; a=e; }
    return a;
}
static Expr *parse_power(Parser *p){ static const TokKind o[]={T_POWER}; return bin_left(p,parse_unary,o,1); }
static Expr *parse_factor(Parser *p){ static const TokKind o[]={T_STAR,T_SLASH,T_FLOOR_DIV,T_PERCENT}; return bin_left(p,parse_power,o,4); }
static Expr *parse_term(Parser *p){ static const TokKind o[]={T_PLUS,T_MINUS}; return bin_left(p,parse_factor,o,2); }
static Expr *parse_shift(Parser *p){ static const TokKind o[]={T_SHL,T_SHR}; return bin_left(p,parse_term,o,2); }
static Expr *parse_bitand(Parser *p){ static const TokKind o[]={T_AMP}; return bin_left(p,parse_shift,o,1); }
static Expr *parse_bitxor(Parser *p){ static const TokKind o[]={T_CARET}; return bin_left(p,parse_bitand,o,1); }
static Expr *parse_bitor(Parser *p){ static const TokKind o[]={T_PIPE}; return bin_left(p,parse_bitxor,o,1); }
static int cmp_op_ahead(Parser *p){ TokKind k=peek(p)->kind; if(k==T_LT||k==T_LE||k==T_GT||k==T_GE||k==T_EQ||k==T_NE||k==T_IN||k==T_IS) return 1; if(k==T_NOT&&p->tv.v[p->pos+1].kind==T_IN) return 1; return 0; }
static Expr *parse_comparison(Parser *p){
    Expr *left=parse_bitor(p);
    if(!cmp_op_ahead(p)) return left;
    Expr *e=enew(EXPR_COMPARE,left->line);
    ep_push(&e->items,&e->count,&e->cap,left);
    for(;;){
        TokKind k=peek(p)->kind; int code;
        if(k==T_NOT){ p->pos+=2; code=CMP_NOTIN; }
        else if(k==T_IN){ p->pos++; code=CMP_IN; }
        else if(k==T_IS){ p->pos++; code=match(p,T_NOT)?CMP_ISNOT:CMP_IS; }
        else { p->pos++; code=(k==T_LT?CMP_LT:k==T_LE?CMP_LE:k==T_GT?CMP_GT:k==T_GE?CMP_GE:k==T_EQ?CMP_EQ:CMP_NE); }
        Expr *right=parse_bitor(p); right->akind=code;
        ep_push(&e->items,&e->count,&e->cap,right);
        if(!cmp_op_ahead(p)) break;
    }
    return e;
}
static Expr *parse_and(Parser *p){ Expr *a=parse_comparison(p); while(match(p,T_AND)){ int line=prev(p)->line; Expr *b=parse_comparison(p); Expr *e=enew(EXPR_BOOL,line); e->op=T_AND; e->a=a; e->b=b; a=e; } return a; }
static Expr *parse_or_test(Parser *p){ Expr *a=parse_and(p); while(match(p,T_OR)){ int line=prev(p)->line; Expr *b=parse_and(p); Expr *e=enew(EXPR_BOOL,line); e->op=T_OR; e->a=a; e->b=b; a=e; } return a; }
static Expr *parse_expr(Parser *p){
    Expr *a=parse_or_test(p);                                          /* A */
    if(peek(p)->kind==T_IF){
        int line=peek(p)->line; p->pos++;
        Expr *cond=parse_or_test(p);                                   /* C */
        need(p,T_ELSE,"else");
        Expr *b=parse_expr(p);                                         /* B (nested ternary ok) */
        Expr *e=enew(EXPR_TERNARY,line); e->a=cond; e->b=a; e->c=b; return e;
    }
    return a;
}

/* ---- emitter: walk the Expr tree ---- */

static Op binop_opcode(TokKind k){
    switch(k){
        case T_PLUS: return OP_ADD; case T_MINUS: return OP_SUB; case T_STAR: return OP_MUL;
        case T_SLASH: return OP_DIV; case T_FLOOR_DIV: return OP_FLOOR_DIV; case T_PERCENT: return OP_MOD;
        case T_POWER: return OP_POWER; case T_SHL: return OP_SHL; case T_SHR: return OP_SHR;
        case T_AMP: return OP_BITAND; case T_CARET: return OP_BITXOR; case T_PIPE: return OP_BITOR;
        default: return OP_ADD;
    }
}
static void emit_cmp(Parser *p, int code, int line){
    switch(code){
        case CMP_LT: emit_op(p->chunk,OP_LT,line); break;
        case CMP_LE: emit_op(p->chunk,OP_LE,line); break;
        case CMP_GT: emit_op(p->chunk,OP_GT,line); break;
        case CMP_GE: emit_op(p->chunk,OP_GE,line); break;
        case CMP_EQ: emit_op(p->chunk,OP_EQ,line); break;
        case CMP_NE: emit_op(p->chunk,OP_NE,line); break;
        case CMP_IN: emit_op(p->chunk,OP_CONTAINS,line); break;
        case CMP_NOTIN: emit_op(p->chunk,OP_CONTAINS,line); emit_op(p->chunk,OP_NOT,line); break;
        case CMP_IS: emit_op(p->chunk,OP_IS,line); break;
        case CMP_ISNOT: emit_op(p->chunk,OP_IS_NOT,line); break;
    }
}
static int comp_ctr=0;
static void emit_comp_clause(Parser *p, Expr *e, int ci, const char *tmp){
    int line=e->line;
    if(ci==e->nclause){                                                /* innermost body */
        if(e->comp_kind=='D'){
            emit_arg(p->chunk,OP_LOAD,name_const(p,tmp),line);
            emit_expr(p,e->a);                                         /* key */
            emit_expr(p,e->b);                                         /* value */
            emit_op(p->chunk,OP_SET_INDEX,line); emit_op(p->chunk,OP_POP,line);
        } else {
            emit_arg(p->chunk,OP_LOAD,name_const(p,tmp),line);
            emit_arg(p->chunk,OP_GET_ATTR,name_const(p,e->comp_kind=='L'?"append":"add"),line);
            emit_expr(p,e->a);
            emit_arg(p->chunk,OP_CALL,1,line);
            emit_op(p->chunk,OP_POP,line);
        }
        return;
    }
    CompClause *cl=&e->clauses[ci];
    emit_expr(p,cl->iter);
    emit_op(p->chunk,OP_ITER,line);
    int start=p->chunk->count;
    emit_arg(p->chunk,OP_FOR_NEXT,0,line); int exit_jump=p->chunk->count-1;
    if(cl->nvars>1){ emit_arg(p->chunk,OP_UNPACK,cl->nvars,line); for(int j=0;j<cl->nvars;j++) emit_arg(p->chunk,OP_STORE,name_const(p,cl->vars[j]),line); }
    else emit_arg(p->chunk,OP_STORE,name_const(p,cl->vars[0]),line);
    for(int k=0;k<cl->ncond;k++){ emit_expr(p,cl->conds[k]); emit_arg(p->chunk,OP_JUMP_IF_FALSE,start,line); }
    emit_comp_clause(p,e,ci+1,tmp);
    emit_arg(p->chunk,OP_JUMP,start,line);
    patch(p->chunk,exit_jump,p->chunk->count);
}
static void emit_comprehension(Parser *p, Expr *e){
    char tmp[24]; snprintf(tmp,sizeof(tmp),"$comp%d",comp_ctr++);
    int line=e->line;
    if(e->comp_kind=='L') emit_arg(p->chunk,OP_MAKE_LIST,0,line);
    else if(e->comp_kind=='S') emit_arg(p->chunk,OP_MAKE_SET,0,line);
    else emit_arg(p->chunk,OP_MAKE_DICT,0,line);
    emit_arg(p->chunk,OP_STORE,name_const(p,tmp),line);
    emit_comp_clause(p,e,0,tmp);
    emit_arg(p->chunk,OP_LOAD,name_const(p,tmp),line);
}
static void emit_call(Parser *p, Expr *e){
    int line=e->line;
    emit_expr(p,e->a);                                                 /* callee */
    int cx=0; for(int i=0;i<e->count;i++) if(e->items[i]->akind!=0){ cx=1; break; }
    if(!cx){
        for(int i=0;i<e->count;i++) emit_expr(p,e->items[i]);
        emit_arg(p->chunk,OP_CALL,e->count,line);
        return;
    }
    emit_arg(p->chunk,OP_MAKE_LIST,0,line);                            /* positional list */
    for(int i=0;i<e->count;i++){ int ak=e->items[i]->akind; if(ak==0||ak==1){ emit_expr(p,e->items[i]); emit_op(p->chunk,ak==1?OP_LIST_EXTEND:OP_LIST_APPEND,line); } }
    emit_arg(p->chunk,OP_MAKE_DICT,0,line);                            /* keyword dict */
    for(int i=0;i<e->count;i++){ int ak=e->items[i]->akind; if(ak==2){ emit_expr(p,e->items[i]); emit_op(p->chunk,OP_DICT_MERGE,line); } else if(ak==3){ emit_expr(p,e->items[i]); emit_arg(p->chunk,OP_DICT_SETNAME,name_const(p,e->items[i]->name),line); } }
    emit_op(p->chunk,OP_CALL_EX,line);
}
static void emit_expr(Parser *p, Expr *e){
    int line=e->line;
    switch(e->kind){
        case EXPR_LITERAL:{ Tok *t=e->tok; if(t->kind==T_NUMBER){ if(t->is_float) emit_arg(p->chunk,OP_CONST,add_const(p->chunk,floatv(t->f)),line); else emit_arg(p->chunk,OP_CONST,add_const(p->chunk,intv(t->i)),line); } else emit_arg(p->chunk,OP_CONST,add_const(p->chunk,stringv(t->text)),line); break; }
        case EXPR_TRUE: emit_op(p->chunk,OP_TRUE,line); break;
        case EXPR_FALSE: emit_op(p->chunk,OP_FALSE,line); break;
        case EXPR_NONE: emit_op(p->chunk,OP_NONE,line); break;
        case EXPR_NAME: emit_arg(p->chunk,OP_LOAD,name_const(p,e->name),line); break;
        case EXPR_UNARY: emit_expr(p,e->a); if(e->op==T_MINUS) emit_op(p->chunk,OP_NEG,line); else if(e->op==T_TILDE) emit_op(p->chunk,OP_BITNOT,line); else emit_op(p->chunk,OP_NOT,line); break;
        case EXPR_BINARY: emit_expr(p,e->a); emit_expr(p,e->b); emit_op(p->chunk,binop_opcode(e->op),line); break;
        case EXPR_BOOL:{
            emit_expr(p,e->a);
            emit_arg(p->chunk,e->op==T_AND?OP_JUMP_IF_FALSE_KEEP:OP_JUMP_IF_TRUE_KEEP,0,line); int end=p->chunk->count-1;
            emit_op(p->chunk,OP_POP,line); emit_expr(p,e->b); patch(p->chunk,end,p->chunk->count); break;
        }
        case EXPR_COMPARE:{
            int n=e->count; emit_expr(p,e->items[0]);
            int scjumps[64]; int nsc=0; int cline=line;
            for(int i=1;i<n;i++){
                Expr *r=e->items[i]; cline=r->line;
                emit_expr(p,r);
                int more=(i<n-1);
                if(more){ emit_op(p->chunk,OP_DUP,cline); emit_op(p->chunk,OP_ROT3,cline); }
                emit_cmp(p,r->akind,cline);
                if(!more) break;
                emit_arg(p->chunk,OP_JUMP_IF_FALSE_KEEP,0,cline); if(nsc<64) scjumps[nsc++]=p->chunk->count-1;
                emit_op(p->chunk,OP_POP,cline);
            }
            if(nsc>0){
                emit_arg(p->chunk,OP_JUMP,0,cline); int donej=p->chunk->count-1;
                for(int i=0;i<nsc;i++) patch(p->chunk,scjumps[i],p->chunk->count);
                emit_op(p->chunk,OP_ROT2,cline); emit_op(p->chunk,OP_POP,cline);
                patch(p->chunk,donej,p->chunk->count);
            }
            break;
        }
        case EXPR_TERNARY:{
            emit_expr(p,e->b);                                         /* A (then value) */
            emit_expr(p,e->a);                                         /* C (cond) */
            emit_arg(p->chunk,OP_JUMP_IF_FALSE,0,line); int jf=p->chunk->count-1;
            emit_arg(p->chunk,OP_JUMP,0,line); int jend=p->chunk->count-1;
            patch(p->chunk,jf,p->chunk->count); emit_op(p->chunk,OP_POP,line);
            emit_expr(p,e->c);                                         /* B (else value) */
            patch(p->chunk,jend,p->chunk->count);
            break;
        }
        case EXPR_CALL: emit_call(p,e); break;
        case EXPR_ATTRIBUTE: emit_expr(p,e->a); emit_arg(p->chunk,OP_GET_ATTR,name_const(p,e->name),line); break;
        case EXPR_INDEX: emit_expr(p,e->a); emit_expr(p,e->b); emit_op(p->chunk,OP_GET_INDEX,line); break;
        case EXPR_SLICE:{
            emit_expr(p,e->a);
            if(e->b) emit_expr(p,e->b); else emit_op(p->chunk,OP_NONE,line);
            if(e->c) emit_expr(p,e->c); else emit_op(p->chunk,OP_NONE,line);
            if(e->d) emit_expr(p,e->d); else emit_op(p->chunk,OP_NONE,line);
            emit_op(p->chunk,OP_GET_SLICE,line);
            break;
        }
        case EXPR_LIST: for(int i=0;i<e->count;i++) emit_expr(p,e->items[i]); emit_arg(p->chunk,OP_MAKE_LIST,e->count,line); break;
        case EXPR_TUPLE: for(int i=0;i<e->count;i++) emit_expr(p,e->items[i]); emit_arg(p->chunk,OP_MAKE_TUPLE,e->count,line); break;
        case EXPR_SET: for(int i=0;i<e->count;i++) emit_expr(p,e->items[i]); emit_arg(p->chunk,OP_MAKE_SET,e->count,line); break;
        case EXPR_DICT: for(int i=0;i<e->count;i++){ emit_expr(p,e->items[i]); emit_expr(p,e->vals[i]); } emit_arg(p->chunk,OP_MAKE_DICT,e->count,line); break;
        case EXPR_COMPREHENSION: emit_comprehension(p,e); break;
        case EXPR_LAMBDA:{
            Chunk *outer=p->chunk; Function *fn=alloc_function("<lambda>",e->eparams,e->neparam,p->globals,p->module_dir,0);
            p->chunk=fn->chunk; emit_expr(p,e->a); emit_op(p->chunk,OP_RETURN,line); p->chunk=outer;
            emit_arg(p->chunk,OP_DEF,add_const(p->chunk,objv(fn->owner)),line);
            break;
        }
        default: fprintf(stderr,"internal error: cannot emit expr kind %d\n",e->kind); exit(1);
    }
}

/* Public entry: parse an expression to a tree, then emit it. */
void expr(Parser *p){ Expr *t=parse_expr(p); emit_expr(p,t); }

/* ========================= Statement helpers for the AST compiler =========================
   compiler.c owns statement compilation; it re-enters this file only for the
   token-range pieces below: assignment targets, from-import, and print(). */

static void skip_balanced_until(Parser *p, TokKind a, TokKind b){ int d=0; while(peek(p)->kind!=T_EOF){ TokKind k=peek(p)->kind; if(d==0&&(k==a||k==b||k==T_NEWLINE)) return; if(k==T_LP||k==T_LB||k==T_LC) d++; if(k==T_RP||k==T_RB||k==T_RC) d--; p->pos++; } }
/* Map an augmented-assignment token to its binary opcode. */
static int aug_assign_op(TokKind k, Op *out){
    switch(k){
        case T_PLUS_ASSIGN: *out=OP_ADD; return 1;
        case T_MINUS_ASSIGN: *out=OP_SUB; return 1;
        case T_STAR_ASSIGN: *out=OP_MUL; return 1;
        case T_SLASH_ASSIGN: *out=OP_DIV; return 1;
        case T_PERCENT_ASSIGN: *out=OP_MOD; return 1;
        case T_FLOOR_DIV_ASSIGN: *out=OP_FLOOR_DIV; return 1;
        case T_POWER_ASSIGN: *out=OP_POWER; return 1;
        case T_AMP_ASSIGN: *out=OP_BITAND; return 1;
        case T_PIPE_ASSIGN: *out=OP_BITOR; return 1;
        case T_CARET_ASSIGN: *out=OP_BITXOR; return 1;
        case T_SHL_ASSIGN: *out=OP_SHL; return 1;
        case T_SHR_ASSIGN: *out=OP_SHR; return 1;
        default: return 0;
    }
}
/* Is there another top-level '=' before the end of the line? */
static int top_level_eq_ahead(Parser *p){
    int i=p->pos, depth=0;
    while(p->tv.v[i].kind!=T_EOF && !(depth==0 && p->tv.v[i].kind==T_NEWLINE)){
        TokKind k=p->tv.v[i].kind;
        if(k==T_LP||k==T_LB||k==T_LC) depth++;
        else if(k==T_RP||k==T_RB||k==T_RC){ if(depth>0) depth--; }
        else if(depth==0 && k==T_ASSIGN) return 1;
        i++;
    }
    return 0;
}
void assign_stmt(Parser *p){
    int line=peek(p)->line;
    /* Scan the line for: count of top-level '=', a top-level comma before the
       first '=' (tuple target), and any top-level augmented-assign operator. */
    { int i=p->pos, depth=0, n_eq=0, top_comma=0, has_aug=0; Op tmp;
      while(p->tv.v[i].kind!=T_EOF && !(depth==0 && p->tv.v[i].kind==T_NEWLINE)){
          TokKind k=p->tv.v[i].kind;
          if(k==T_LP||k==T_LB||k==T_LC) depth++;
          else if(k==T_RP||k==T_RB||k==T_RC){ if(depth>0) depth--; }
          else if(depth==0){ if(k==T_ASSIGN) n_eq++; else if(k==T_COMMA && n_eq==0) top_comma=1; else if(aug_assign_op(k,&tmp)) has_aug=1; }
          i++;
      }
      if(!has_aug && (n_eq>=2 || top_comma)){
          /* chained and/or tuple assignment - simple name targets only */
          char *segn[16][32]; int seglen[16]; int nseg=0;
          while(top_level_eq_ahead(p)){
              int nl=0;
              do{ Tok *n=need(p,T_NAME,"assignment target"); if(nseg<16 && nl<32) segn[nseg][nl++]=n->text; }while(match(p,T_COMMA));
              if(nseg<16){ seglen[nseg]=nl; nseg++; } else nseg++;
              need(p,T_ASSIGN,"=");
          }
          if(nseg>16) nseg=16;
          expr(p); int rn=1; while(match(p,T_COMMA)){ if(peek(p)->kind==T_NEWLINE) break; expr(p); rn++; }
          if(rn>1) emit_arg(p->chunk,OP_MAKE_TUPLE,rn,line);
          need(p,T_NEWLINE,"newline");
          for(int s=0;s<nseg;s++){
              if(s<nseg-1) emit_op(p->chunk,OP_DUP,line);
              if(seglen[s]==1) emit_arg(p->chunk,OP_STORE,name_const(p,segn[s][0]),line);
              else { emit_arg(p->chunk,OP_UNPACK,seglen[s],line); for(int j=0;j<seglen[s];j++) emit_arg(p->chunk,OP_STORE,name_const(p,segn[s][j]),line); }
          }
          return;
      }
    }
    Tok *base=need(p,T_NAME,"name");
    if(match(p,T_COLON)){ skip_balanced_until(p,T_ASSIGN,T_NEWLINE); if(match(p,T_ASSIGN)){ expr(p); emit_arg(p->chunk,OP_STORE,name_const(p,base->text),line); } need(p,T_NEWLINE,"newline"); return; }
    if(match(p,T_ASSIGN)){ expr(p); emit_arg(p->chunk,OP_STORE,name_const(p,base->text),line); need(p,T_NEWLINE,"newline"); return; }
    TokKind aug=peek(p)->kind; Op ao;
    if(aug_assign_op(aug,&ao)){ p->pos++; emit_arg(p->chunk,OP_LOAD,name_const(p,base->text),line); expr(p); emit_op(p->chunk,ao,line); emit_arg(p->chunk,OP_STORE,name_const(p,base->text),line); need(p,T_NEWLINE,"newline"); return; }
    emit_arg(p->chunk,OP_LOAD,name_const(p,base->text),line);
    while(1){
        if(match(p,T_DOT)){ Tok *n=need(p,T_NAME,"attr"); Op ao2;
            if(aug_assign_op(peek(p)->kind,&ao2)){ p->pos++; emit_op(p->chunk,OP_DUP,line); emit_arg(p->chunk,OP_GET_ATTR,name_const(p,n->text),line); expr(p); emit_op(p->chunk,ao2,line); emit_arg(p->chunk,OP_SET_ATTR,name_const(p,n->text),line); emit_op(p->chunk,OP_POP,line); need(p,T_NEWLINE,"newline"); return; }
            if(match(p,T_ASSIGN)){ expr(p); emit_arg(p->chunk,OP_SET_ATTR,name_const(p,n->text),line); emit_op(p->chunk,OP_POP,line); need(p,T_NEWLINE,"newline"); return; }
            emit_arg(p->chunk,OP_GET_ATTR,name_const(p,n->text),line);
        } else if(match(p,T_LB)){ expr(p); need(p,T_RB,"]"); Op ao2;
            if(aug_assign_op(peek(p)->kind,&ao2)){ p->pos++; emit_op(p->chunk,OP_DUP2,line); emit_op(p->chunk,OP_GET_INDEX,line); expr(p); emit_op(p->chunk,ao2,line); emit_op(p->chunk,OP_SET_INDEX,line); emit_op(p->chunk,OP_POP,line); need(p,T_NEWLINE,"newline"); return; }
            if(match(p,T_ASSIGN)){ expr(p); emit_op(p->chunk,OP_SET_INDEX,line); emit_op(p->chunk,OP_POP,line); need(p,T_NEWLINE,"newline"); return; }
            emit_op(p->chunk,OP_GET_INDEX,line);
        } else break;
    }
    fprintf(stderr,"parse error at line %d: invalid assignment target\n",line); exit(1);
}
/* from a.b import c [as d], ...   |   from m import *
   OP_IMPORT pushes the top package; we descend .b to reach the actual module,
   then bind each name off it (or splash all public names for `*`). */
void from_import_stmt(Parser *p){
    char buf[256]; int bl=0;
    Tok *m=need(p,T_NAME,"module name"); int line=m->line; bl+=snprintf(buf+bl,sizeof(buf)-(size_t)bl,"%s",m->text);
    while(match(p,T_DOT)){ Tok *n=need(p,T_NAME,"module name"); if(bl<250) bl+=snprintf(buf+bl,sizeof(buf)-(size_t)bl,".%s",n->text); }
    need(p,T_IMPORT,"import");
    emit_arg(p->chunk,OP_IMPORT,name_const(p,buf),line);
    for(const char *dot=strchr(buf,'.'); dot; ){ const char *comp=dot+1; const char *nd=strchr(comp,'.'); int clen=nd?(int)(nd-comp):(int)strlen(comp); char b[128]; if(clen>127) clen=127; memcpy(b,comp,clen); b[clen]=0; emit_arg(p->chunk,OP_GET_ATTR,name_const(p,b),line); dot=nd; }
    if(match(p,T_STAR)){ emit_op(p->chunk,OP_IMPORT_STAR,line); need(p,T_NEWLINE,"newline"); return; }
    do{
        Tok *n=need(p,T_NAME,"imported name"); const char *alias=n->text;
        if(match(p,T_AS)){ Tok *a=need(p,T_NAME,"alias"); alias=a->text; }
        emit_op(p->chunk,OP_DUP,line);
        emit_arg(p->chunk,OP_GET_ATTR,name_const(p,n->text),line);
        emit_arg(p->chunk,OP_STORE,name_const(p,alias),line);
    } while(match(p,T_COMMA));
    emit_op(p->chunk,OP_POP,line);                        /* drop the module left on the stack */
    need(p,T_NEWLINE,"newline");
}
/* print(EXPR): keyword-statement form. Enter with the cursor at `print`. */
void print_stmt(Parser *p){ need(p,T_PRINT,"print"); int line=prev(p)->line; need(p,T_LP,"("); expr(p); need(p,T_RP,")"); emit_op(p->chunk,OP_PRINT,line); need(p,T_NEWLINE,"newline"); }
