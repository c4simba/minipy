/* ========================= Legacy recursive-descent grammar ========================= */

#include "compiler.h"
#include "lexer.h"

int is_assignment(Parser *p){
    int i=p->pos; if(p->tv.v[i].kind!=T_NAME) return 0; i++;
    while(1){ if(p->tv.v[i].kind==T_DOT){ i++; if(p->tv.v[i].kind!=T_NAME)return 0; i++; }
        else if(p->tv.v[i].kind==T_LB){ int depth=1; i++; while(depth&&p->tv.v[i].kind!=T_EOF){ if(p->tv.v[i].kind==T_LB||p->tv.v[i].kind==T_LP||p->tv.v[i].kind==T_LC)depth++; else if(p->tv.v[i].kind==T_RB||p->tv.v[i].kind==T_RP||p->tv.v[i].kind==T_RC)depth--; i++; } }
        else break; }
    return p->tv.v[i].kind==T_ASSIGN||p->tv.v[i].kind==T_PLUS_ASSIGN||p->tv.v[i].kind==T_MINUS_ASSIGN||p->tv.v[i].kind==T_STAR_ASSIGN||p->tv.v[i].kind==T_SLASH_ASSIGN||p->tv.v[i].kind==T_COLON;
}
static void primary(Parser *p){
    Tok *t=peek(p); int line=t->line;
    if(match(p,T_NUMBER)){ if(prev(p)->is_float) emit_arg(p->chunk,OP_CONST,add_const(p->chunk,floatv(prev(p)->f)),line); else emit_arg(p->chunk,OP_CONST,add_const(p->chunk,intv(prev(p)->i)),line); }
    else if(match(p,T_STRING)){ emit_arg(p->chunk,OP_CONST,add_const(p->chunk,stringv(prev(p)->text)),line); }
    else if(match(p,T_TRUE)) emit_op(p->chunk,OP_TRUE,line); else if(match(p,T_FALSE)) emit_op(p->chunk,OP_FALSE,line); else if(match(p,T_NONE)) emit_op(p->chunk,OP_NONE,line);
    else if(match(p,T_NAME)){ emit_arg(p->chunk,OP_LOAD,name_const(p,prev(p)->text),line); }
    else if(match(p,T_LAMBDA)){
        char **params=NULL; int ar=0,cap=0; if(peek(p)->kind!=T_COLON){ do{ Tok *a=need(p,T_NAME,"lambda param"); if(ar==cap){cap=cap?cap*2:4; params=(char**)xrealloc(params,sizeof(char*)*(size_t)cap);} params[ar++]=xstrdup2(a->text); }while(match(p,T_COMMA)); }
        need(p,T_COLON,":"); Chunk *outer=p->chunk; Function *fn=alloc_function("<lambda>",params,ar,p->globals,p->module_dir,0); p->chunk=fn->chunk; expr(p); emit_op(p->chunk,OP_RETURN,line); p->chunk=outer; emit_arg(p->chunk,OP_DEF,add_const(p->chunk,objv(fn->owner)),line);
    }
    else if(match(p,T_LP)){
        if(match(p,T_RP)){ emit_arg(p->chunk,OP_MAKE_TUPLE,0,line); } else { expr(p); if(match(p,T_COMMA)){ int n=1; if(!match(p,T_RP)){ do{ expr(p); n++; }while(match(p,T_COMMA)); need(p,T_RP,")"); } emit_arg(p->chunk,OP_MAKE_TUPLE,n,line); } else need(p,T_RP,")"); }
    }
    else if(match(p,T_LB)){ int n=0; if(!match(p,T_RB)){ do{ expr(p); n++; }while(match(p,T_COMMA)); need(p,T_RB,"]"); } emit_arg(p->chunk,OP_MAKE_LIST,n,line); }
    else if(match(p,T_LC)){
        int n=0; if(match(p,T_RC)){ emit_arg(p->chunk,OP_MAKE_DICT,0,line); }
        else { expr(p); if(match(p,T_COLON)){ expr(p); n=1; while(match(p,T_COMMA)){ if(peek(p)->kind==T_RC) break; expr(p); need(p,T_COLON,":"); expr(p); n++; } need(p,T_RC,"}"); emit_arg(p->chunk,OP_MAKE_DICT,n,line); }
        else { n=1; while(match(p,T_COMMA)){ if(peek(p)->kind==T_RC) break; expr(p); n++; } need(p,T_RC,"}"); emit_arg(p->chunk,OP_MAKE_SET,n,line); } }
    }
    else { fprintf(stderr,"parse error at line %d: expected expression\n",line); exit(1); }
    while(1){
        if(match(p,T_LP)){ int argc=0; if(!match(p,T_RP)){ do{ expr(p); argc++; }while(match(p,T_COMMA)); need(p,T_RP,")"); } emit_arg(p->chunk,OP_CALL,argc,line); }
        else if(match(p,T_DOT)){ Tok *n=need(p,T_NAME,"attribute name"); emit_arg(p->chunk,OP_GET_ATTR,name_const(p,n->text),n->line); }
        else if(match(p,T_LB)){
            if(match(p,T_COLON)){ emit_op(p->chunk,OP_NONE,line); if(peek(p)->kind!=T_RB) expr(p); else emit_op(p->chunk,OP_NONE,line); need(p,T_RB,"]"); emit_op(p->chunk,OP_GET_SLICE,line); }
            else { expr(p); if(match(p,T_COLON)){ if(peek(p)->kind!=T_RB) expr(p); else emit_op(p->chunk,OP_NONE,line); need(p,T_RB,"]"); emit_op(p->chunk,OP_GET_SLICE,line); } else { need(p,T_RB,"]"); emit_op(p->chunk,OP_GET_INDEX,line); } }
        }
        else break;
    }
}
static void unary(Parser *p){
    if(match(p,T_MINUS)){ int line=prev(p)->line; unary(p); emit_op(p->chunk,OP_NEG,line); }
    else if(match(p,T_NOT)){ int line=prev(p)->line; unary(p); emit_op(p->chunk,OP_NOT,line); }
    else primary(p);
}
static void power(Parser *p){ unary(p); while(peek(p)->kind==T_POWER){ int line=peek(p)->line; p->pos++; unary(p); emit_op(p->chunk,OP_POWER,line); } }
static void factor(Parser *p){ power(p); while(peek(p)->kind==T_STAR||peek(p)->kind==T_SLASH||peek(p)->kind==T_FLOOR_DIV){ TokKind k=peek(p)->kind; int line=peek(p)->line; p->pos++; power(p); emit_op(p->chunk,k==T_STAR?OP_MUL:(k==T_FLOOR_DIV?OP_FLOOR_DIV:OP_DIV),line); } }
static void term(Parser *p){ factor(p); while(peek(p)->kind==T_PLUS||peek(p)->kind==T_MINUS){ TokKind k=peek(p)->kind; int line=peek(p)->line; p->pos++; factor(p); emit_op(p->chunk,k==T_PLUS?OP_ADD:OP_SUB,line); } }
static void cmp(Parser *p){
    term(p);
    while(peek(p)->kind==T_LT||peek(p)->kind==T_LE||peek(p)->kind==T_GT||peek(p)->kind==T_GE||peek(p)->kind==T_IN||peek(p)->kind==T_IS||(peek(p)->kind==T_NOT&&p->tv.v[p->pos+1].kind==T_IN)){
        TokKind k=peek(p)->kind; int line=peek(p)->line;
        if(k==T_NOT){ p->pos+=2; term(p); emit_op(p->chunk,OP_CONTAINS,line); emit_op(p->chunk,OP_NOT,line); }
        else if(k==T_IN){ p->pos++; term(p); emit_op(p->chunk,OP_CONTAINS,line); }
        else if(k==T_IS){ p->pos++; int neg=match(p,T_NOT); term(p); emit_op(p->chunk,neg?OP_IS_NOT:OP_IS,line); }
        else { p->pos++; term(p); emit_op(p->chunk,k==T_LT?OP_LT:k==T_LE?OP_LE:k==T_GT?OP_GT:OP_GE,line); }
    }
}
static void equality(Parser *p){ cmp(p); while(peek(p)->kind==T_EQ||peek(p)->kind==T_NE){ TokKind k=peek(p)->kind; int line=peek(p)->line; p->pos++; cmp(p); emit_op(p->chunk,k==T_EQ?OP_EQ:OP_NE,line); } }
static void and_expr(Parser *p){ equality(p); while(match(p,T_AND)){ int line=prev(p)->line; emit_arg(p->chunk,OP_JUMP_IF_FALSE_KEEP,0,line); int end=p->chunk->count-1; emit_op(p->chunk,OP_POP,line); equality(p); patch(p->chunk,end,p->chunk->count); } }
void expr(Parser *p){ and_expr(p); while(match(p,T_OR)){ int line=prev(p)->line; emit_arg(p->chunk,OP_JUMP_IF_TRUE_KEEP,0,line); int end=p->chunk->count-1; emit_op(p->chunk,OP_POP,line); and_expr(p); patch(p->chunk,end,p->chunk->count); } }
static void block(Parser *p){ need(p,T_NEWLINE,"newline"); need(p,T_INDENT,"indent"); skip_nl(p); while(peek(p)->kind!=T_DEDENT&&peek(p)->kind!=T_EOF) legacy_statement(p); need(p,T_DEDENT,"dedent"); }
static void skip_balanced_until(Parser *p, TokKind a, TokKind b){ int d=0; while(peek(p)->kind!=T_EOF){ TokKind k=peek(p)->kind; if(d==0&&(k==a||k==b||k==T_NEWLINE)) return; if(k==T_LP||k==T_LB||k==T_LC) d++; if(k==T_RP||k==T_RB||k==T_RC) d--; p->pos++; } }
void assign_stmt(Parser *p){
    Tok *base=need(p,T_NAME,"name"); int line=base->line;
    if(match(p,T_COLON)){ skip_balanced_until(p,T_ASSIGN,T_NEWLINE); if(match(p,T_ASSIGN)){ expr(p); emit_arg(p->chunk,OP_STORE,name_const(p,base->text),line); } need(p,T_NEWLINE,"newline"); return; }
    if(match(p,T_ASSIGN)){ expr(p); emit_arg(p->chunk,OP_STORE,name_const(p,base->text),line); need(p,T_NEWLINE,"newline"); return; }
    TokKind aug=peek(p)->kind;
    if(aug==T_PLUS_ASSIGN||aug==T_MINUS_ASSIGN||aug==T_STAR_ASSIGN||aug==T_SLASH_ASSIGN){ p->pos++; emit_arg(p->chunk,OP_LOAD,name_const(p,base->text),line); expr(p); emit_op(p->chunk,aug==T_PLUS_ASSIGN?OP_ADD:aug==T_MINUS_ASSIGN?OP_SUB:aug==T_STAR_ASSIGN?OP_MUL:OP_DIV,line); emit_arg(p->chunk,OP_STORE,name_const(p,base->text),line); need(p,T_NEWLINE,"newline"); return; }
    emit_arg(p->chunk,OP_LOAD,name_const(p,base->text),line);
    while(1){ if(match(p,T_DOT)){ Tok *n=need(p,T_NAME,"attr"); if(match(p,T_ASSIGN)){ expr(p); emit_arg(p->chunk,OP_SET_ATTR,name_const(p,n->text),line); need(p,T_NEWLINE,"newline"); return; } emit_arg(p->chunk,OP_GET_ATTR,name_const(p,n->text),line); } else if(match(p,T_LB)){ expr(p); need(p,T_RB,"]"); if(match(p,T_ASSIGN)){ expr(p); emit_op(p->chunk,OP_SET_INDEX,line); need(p,T_NEWLINE,"newline"); return; } emit_op(p->chunk,OP_GET_INDEX,line); } else break; }
    fprintf(stderr,"parse error at line %d: invalid assignment target\n",line); exit(1);
}
static void if_stmt(Parser *p){ int line=prev(p)->line; expr(p); need(p,T_COLON,":"); emit_arg(p->chunk,OP_JUMP_IF_FALSE,0,line); int jf=p->chunk->count-1; block(p); if(match(p,T_ELIF)){ emit_arg(p->chunk,OP_JUMP,0,line); int je=p->chunk->count-1; patch(p->chunk,jf,p->chunk->count); if_stmt(p); patch(p->chunk,je,p->chunk->count); } else if(match(p,T_ELSE)){ need(p,T_COLON,":"); emit_arg(p->chunk,OP_JUMP,0,line); int je=p->chunk->count-1; patch(p->chunk,jf,p->chunk->count); block(p); patch(p->chunk,je,p->chunk->count); } else patch(p->chunk,jf,p->chunk->count); }
static void while_stmt(Parser *p){ int start=p->chunk->count; int line=prev(p)->line; expr(p); need(p,T_COLON,":"); emit_arg(p->chunk,OP_JUMP_IF_FALSE,0,line); int jf=p->chunk->count-1; LoopCtx *lc=&p->loops[p->loop_depth++]; lc->start=start; lc->is_for=0; lc->bcount=0; block(p); emit_arg(p->chunk,OP_JUMP,start,line); int else_start=p->chunk->count; patch(p->chunk,jf,else_start); int after_else; if(match(p,T_ELSE)){ need(p,T_COLON,":"); block(p); } after_else=p->chunk->count; for(int i=0;i<lc->bcount;i++) patch(p->chunk,lc->breaks[i],after_else); p->loop_depth--; }
static void for_stmt(Parser *p){ int line=prev(p)->line; Tok *var=need(p,T_NAME,"loop variable"); need(p,T_IN,"in"); expr(p); need(p,T_COLON,":"); emit_op(p->chunk,OP_ITER,line); int start=p->chunk->count; emit_arg(p->chunk,OP_FOR_NEXT,0,line); int exit_jump=p->chunk->count-1; emit_arg(p->chunk,OP_STORE,name_const(p,var->text),line); LoopCtx *lc=&p->loops[p->loop_depth++]; lc->start=start; lc->is_for=1; lc->bcount=0; block(p); emit_arg(p->chunk,OP_JUMP,start,line); int else_start=p->chunk->count; patch(p->chunk,exit_jump,else_start); int after_else; if(match(p,T_ELSE)){ need(p,T_COLON,":"); block(p); } after_else=p->chunk->count; for(int i=0;i<lc->bcount;i++) patch(p->chunk,lc->breaks[i],after_else); p->loop_depth--; }
static void def_stmt(Parser *p){ int line=prev(p)->line; Tok *n=need(p,T_NAME,"function name"); need(p,T_LP,"("); char **params=NULL; int ar=0,cap=0; if(!match(p,T_RP)){ do{ Tok *a=need(p,T_NAME,"param"); if(ar==cap){cap=cap?cap*2:4; params=(char**)xrealloc(params,sizeof(char*)*(size_t)cap);} params[ar++]=xstrdup2(a->text); if(match(p,T_COLON)) skip_balanced_until(p,T_ASSIGN,T_COMMA); if(match(p,T_ASSIGN)){ fprintf(stderr,"parse error at line %d: default arguments are parsed but not implemented yet\n",line); exit(1); } }while(match(p,T_COMMA)); need(p,T_RP,")"); } need(p,T_COLON,":"); need(p,T_NEWLINE,"newline"); need(p,T_INDENT,"indent"); Function *fn=compile_function_from_parser(p,n->text,params,ar,1,0); emit_arg(p->chunk,OP_DEF,add_const(p->chunk,objv(fn->owner)),line); emit_arg(p->chunk,OP_STORE,name_const(p,n->text),line); }
static void class_stmt(Parser *p){ int line=prev(p)->line; Tok *n=need(p,T_NAME,"class name"); if(match(p,T_LP)){ skip_balanced_until(p,T_RP,T_NEWLINE); need(p,T_RP,")"); } need(p,T_COLON,":"); need(p,T_NEWLINE,"newline"); need(p,T_INDENT,"indent"); Function *body=compile_function_from_parser(p,n->text,NULL,0,1,1); int ci=add_const(p->chunk,objv(body->owner)); int ni=name_const(p,n->text); emit_arg(p->chunk,OP_CLASS,ci,line); emit(p->chunk,ni,line); emit_arg(p->chunk,OP_STORE,ni,line); }
static void import_stmt(Parser *p){ Tok *n=need(p,T_NAME,"module name"); int line=n->line; const char *alias=n->text; if(match(p,T_AS)){ Tok *a=need(p,T_NAME,"alias"); alias=a->text; } emit_arg(p->chunk,OP_IMPORT,name_const(p,n->text),line); emit_arg(p->chunk,OP_STORE,name_const(p,alias),line); need(p,T_NEWLINE,"newline"); }
void from_import_stmt(Parser *p){ Tok *m=need(p,T_NAME,"module name"); int line=m->line; need(p,T_IMPORT,"import"); do{ Tok *n=need(p,T_NAME,"imported name"); const char *alias=n->text; if(match(p,T_AS)){ Tok *a=need(p,T_NAME,"alias"); alias=a->text; } emit_arg(p->chunk,OP_IMPORT,name_const(p,m->text),line); emit_arg(p->chunk,OP_GET_ATTR,name_const(p,n->text),line); emit_arg(p->chunk,OP_STORE,name_const(p,alias),line); } while(match(p,T_COMMA)); need(p,T_NEWLINE,"newline"); }
static void with_stmt(Parser *p){ int line=prev(p)->line; expr(p); if(match(p,T_AS)){ Tok *n=need(p,T_NAME,"with target"); emit_arg(p->chunk,OP_STORE,name_const(p,n->text),line); } else emit_op(p->chunk,OP_POP,line); need(p,T_COLON,":"); block(p); }
static void raise_stmt(Parser *p){ int line=prev(p)->line; if(peek(p)->kind!=T_NEWLINE) expr(p); else emit_arg(p->chunk,OP_CONST,add_const(p->chunk,stringv("raise")),line); emit_op(p->chunk,OP_RAISE,line); need(p,T_NEWLINE,"newline"); }
static void global_nonlocal_stmt(Parser *p){ do{ need(p,T_NAME,"name"); }while(match(p,T_COMMA)); need(p,T_NEWLINE,"newline"); }
static void del_stmt(Parser *p){ Tok *n=need(p,T_NAME,"name"); emit_op(p->chunk,OP_NONE,n->line); emit_arg(p->chunk,OP_STORE,name_const(p,n->text),n->line); need(p,T_NEWLINE,"newline"); }
static void unsupported_stmt(Parser *p,const char *what){ fprintf(stderr,"parse error at line %d: %s syntax is recognized but not implemented in this mini runtime\n",prev(p)->line,what); exit(1); }
static void print_stmt(Parser *p){ int line=prev(p)->line; need(p,T_LP,"("); expr(p); need(p,T_RP,")"); emit_op(p->chunk,OP_PRINT,line); need(p,T_NEWLINE,"newline"); }
void legacy_statement(Parser *p){
    skip_nl(p); if(peek(p)->kind==T_DEDENT||peek(p)->kind==T_EOF) return;
    if(match(p,T_IF)) if_stmt(p); else if(match(p,T_WHILE)) while_stmt(p); else if(match(p,T_FOR)) for_stmt(p); else if(match(p,T_DEF)) def_stmt(p); else if(match(p,T_CLASS)) class_stmt(p); else if(match(p,T_IMPORT)) import_stmt(p); else if(match(p,T_FROM)) from_import_stmt(p); else if(match(p,T_WITH)) with_stmt(p); else if(match(p,T_RAISE)) raise_stmt(p); else if(match(p,T_GLOBAL)||match(p,T_NONLOCAL)) global_nonlocal_stmt(p); else if(match(p,T_DEL)) del_stmt(p); else if(match(p,T_TRY)) unsupported_stmt(p,"try/except/finally"); else if(match(p,T_MATCH)) unsupported_stmt(p,"match/case"); else if(match(p,T_ASYNC)) unsupported_stmt(p,"async/await"); else if(match(p,T_YIELD)){ int line=prev(p)->line; if(peek(p)->kind!=T_NEWLINE) expr(p); else emit_op(p->chunk,OP_NONE,line); emit_op(p->chunk,OP_YIELD,line); need(p,T_NEWLINE,"newline"); } else if(match(p,T_LAMBDA)) unsupported_stmt(p,"lambda statement") ;
    else if(match(p,T_PRINT)) print_stmt(p); else if(match(p,T_PASS)){ need(p,T_NEWLINE,"newline"); }
    else if(match(p,T_BREAK)){ int line=prev(p)->line; if(p->loop_depth<=0){ fprintf(stderr,"parse error at line %d: break outside loop\n",line); exit(1); } LoopCtx *lc=&p->loops[p->loop_depth-1]; if(lc->is_for) emit_op(p->chunk,OP_POP,line); emit_arg(p->chunk,OP_JUMP,0,line); lc->breaks[lc->bcount++]=p->chunk->count-1; need(p,T_NEWLINE,"newline"); }
    else if(match(p,T_CONTINUE)){ int line=prev(p)->line; if(p->loop_depth<=0){ fprintf(stderr,"parse error at line %d: continue outside loop\n",line); exit(1); } LoopCtx *lc=&p->loops[p->loop_depth-1]; emit_arg(p->chunk,OP_JUMP,lc->start,line); need(p,T_NEWLINE,"newline"); }
    else if(match(p,T_RETURN)){ int line=prev(p)->line; if(peek(p)->kind!=T_NEWLINE) expr(p); else emit_op(p->chunk,OP_NONE,line); emit_op(p->chunk,OP_RETURN,line); need(p,T_NEWLINE,"newline"); }
    else if(is_assignment(p)) assign_stmt(p); else { int line=peek(p)->line; expr(p); emit_op(p->chunk,OP_POP,line); need(p,T_NEWLINE,"newline"); }
}
Function *compile_function_from_parser(Parser *p,const char *name,char **params,int arity,int until_dedent,int store_globals){
    Chunk *outer=p->chunk; Function *fn=alloc_function(name,params,arity,p->globals,p->module_dir,store_globals); p->chunk=fn->chunk; skip_nl(p); while(peek(p)->kind!=T_EOF && !(until_dedent && peek(p)->kind==T_DEDENT)) legacy_statement(p); if(until_dedent) need(p,T_DEDENT,"dedent"); emit_op(p->chunk,OP_NONE,peek(p)->line); emit_op(p->chunk,OP_RETURN,peek(p)->line); if(p->chunk->has_yield) fn->is_generator=1; p->chunk=outer; return fn;
}
