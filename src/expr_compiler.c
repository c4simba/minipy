/* ========================= Legacy recursive-descent grammar ========================= */

#include "compiler.h"
#include "lexer.h"

static void or_test(Parser *p);
/* kind: 'L' list, 'S' set, 'D' dict. e1s = element/key token start; e2s = value
   token start for dict (-1 otherwise). Consumes the `for ... [if ...]` clauses. */
static void compile_comprehension(Parser *p, int kind, int e1s, int e2s);
static int  call_is_complex(Parser *p);
static void compile_call_ex(Parser *p, int line);

int is_assignment(Parser *p){
    int i=p->pos; if(p->tv.v[i].kind!=T_NAME) return 0; i++;
    while(1){ if(p->tv.v[i].kind==T_DOT){ i++; if(p->tv.v[i].kind!=T_NAME)return 0; i++; }
        else if(p->tv.v[i].kind==T_LB){ int depth=1; i++; while(depth&&p->tv.v[i].kind!=T_EOF){ if(p->tv.v[i].kind==T_LB||p->tv.v[i].kind==T_LP||p->tv.v[i].kind==T_LC)depth++; else if(p->tv.v[i].kind==T_RB||p->tv.v[i].kind==T_RP||p->tv.v[i].kind==T_RC)depth--; i++; } }
        else break; }
    TokKind e=p->tv.v[i].kind;
    return e==T_ASSIGN||e==T_PLUS_ASSIGN||e==T_MINUS_ASSIGN||e==T_STAR_ASSIGN||e==T_SLASH_ASSIGN||e==T_PERCENT_ASSIGN||e==T_FLOOR_DIV_ASSIGN||e==T_POWER_ASSIGN||e==T_AMP_ASSIGN||e==T_PIPE_ASSIGN||e==T_CARET_ASSIGN||e==T_SHL_ASSIGN||e==T_SHR_ASSIGN||e==T_COLON;
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
        if(match(p,T_RP)){ emit_arg(p->chunk,OP_MAKE_TUPLE,0,line); }
        else {
            int save=p->chunk->count; int es=p->pos; expr(p);
            if(peek(p)->kind==T_FOR){ p->chunk->count=save; compile_comprehension(p,'L',es,-1); need(p,T_RP,")"); } /* genexp -> eager list */
            else if(match(p,T_COMMA)){ int n=1; if(!match(p,T_RP)){ do{ expr(p); n++; }while(match(p,T_COMMA)); need(p,T_RP,")"); } emit_arg(p->chunk,OP_MAKE_TUPLE,n,line); }
            else need(p,T_RP,")");
        }
    }
    else if(match(p,T_LB)){
        if(match(p,T_RB)){ emit_arg(p->chunk,OP_MAKE_LIST,0,line); }
        else {
            int save=p->chunk->count; int es=p->pos; expr(p);
            if(peek(p)->kind==T_FOR){ p->chunk->count=save; compile_comprehension(p,'L',es,-1); need(p,T_RB,"]"); }
            else { int n=1; while(match(p,T_COMMA)){ if(peek(p)->kind==T_RB) break; expr(p); n++; } need(p,T_RB,"]"); emit_arg(p->chunk,OP_MAKE_LIST,n,line); }
        }
    }
    else if(match(p,T_LC)){
        if(match(p,T_RC)){ emit_arg(p->chunk,OP_MAKE_DICT,0,line); }
        else {
            int save=p->chunk->count; int ks=p->pos; expr(p);
            if(match(p,T_COLON)){
                int vs=p->pos; expr(p);
                if(peek(p)->kind==T_FOR){ p->chunk->count=save; compile_comprehension(p,'D',ks,vs); need(p,T_RC,"}"); }
                else { int n=1; while(match(p,T_COMMA)){ if(peek(p)->kind==T_RC) break; expr(p); need(p,T_COLON,":"); expr(p); n++; } need(p,T_RC,"}"); emit_arg(p->chunk,OP_MAKE_DICT,n,line); }
            } else {
                if(peek(p)->kind==T_FOR){ p->chunk->count=save; compile_comprehension(p,'S',ks,-1); need(p,T_RC,"}"); }
                else { int n=1; while(match(p,T_COMMA)){ if(peek(p)->kind==T_RC) break; expr(p); n++; } need(p,T_RC,"}"); emit_arg(p->chunk,OP_MAKE_SET,n,line); }
            }
        }
    }
    else { fprintf(stderr,"parse error at line %d: expected expression\n",line); exit(1); }
    while(1){
        if(match(p,T_LP)){
            if(call_is_complex(p)){ compile_call_ex(p,line); }
            else { int argc=0; if(!match(p,T_RP)){ do{ expr(p); argc++; }while(match(p,T_COMMA)); need(p,T_RP,")"); } emit_arg(p->chunk,OP_CALL,argc,line); }
        }
        else if(match(p,T_DOT)){ Tok *n=need(p,T_NAME,"attribute name"); emit_arg(p->chunk,OP_GET_ATTR,name_const(p,n->text),n->line); }
        else if(match(p,T_LB)){
            if(peek(p)->kind==T_COLON) emit_op(p->chunk,OP_NONE,line); else expr(p);   /* start */
            if(match(p,T_COLON)){
                if(peek(p)->kind==T_COLON||peek(p)->kind==T_RB) emit_op(p->chunk,OP_NONE,line); else expr(p);  /* stop */
                if(match(p,T_COLON)){ if(peek(p)->kind==T_RB) emit_op(p->chunk,OP_NONE,line); else expr(p); } else emit_op(p->chunk,OP_NONE,line);  /* step */
                need(p,T_RB,"]"); emit_op(p->chunk,OP_GET_SLICE,line);
            } else { need(p,T_RB,"]"); emit_op(p->chunk,OP_GET_INDEX,line); }
        }
        else break;
    }
}
static void unary(Parser *p){
    if(match(p,T_MINUS)){ int line=prev(p)->line; unary(p); emit_op(p->chunk,OP_NEG,line); }
    else if(match(p,T_PLUS)){ unary(p); /* unary plus: no-op */ }
    else if(match(p,T_TILDE)){ int line=prev(p)->line; unary(p); emit_op(p->chunk,OP_BITNOT,line); }
    else if(match(p,T_NOT)){ int line=prev(p)->line; unary(p); emit_op(p->chunk,OP_NOT,line); }
    else primary(p);
}
static void power(Parser *p){ unary(p); while(peek(p)->kind==T_POWER){ int line=peek(p)->line; p->pos++; unary(p); emit_op(p->chunk,OP_POWER,line); } }
static void factor(Parser *p){ power(p); while(peek(p)->kind==T_STAR||peek(p)->kind==T_SLASH||peek(p)->kind==T_FLOOR_DIV||peek(p)->kind==T_PERCENT){ TokKind k=peek(p)->kind; int line=peek(p)->line; p->pos++; power(p); emit_op(p->chunk,k==T_STAR?OP_MUL:k==T_FLOOR_DIV?OP_FLOOR_DIV:k==T_PERCENT?OP_MOD:OP_DIV,line); } }
static void term(Parser *p){ factor(p); while(peek(p)->kind==T_PLUS||peek(p)->kind==T_MINUS){ TokKind k=peek(p)->kind; int line=peek(p)->line; p->pos++; factor(p); emit_op(p->chunk,k==T_PLUS?OP_ADD:OP_SUB,line); } }
static void shift_expr(Parser *p){ term(p); while(peek(p)->kind==T_SHL||peek(p)->kind==T_SHR){ TokKind k=peek(p)->kind; int line=peek(p)->line; p->pos++; term(p); emit_op(p->chunk,k==T_SHL?OP_SHL:OP_SHR,line); } }
static void bitand_expr(Parser *p){ shift_expr(p); while(peek(p)->kind==T_AMP){ int line=peek(p)->line; p->pos++; shift_expr(p); emit_op(p->chunk,OP_BITAND,line); } }
static void bitxor_expr(Parser *p){ bitand_expr(p); while(peek(p)->kind==T_CARET){ int line=peek(p)->line; p->pos++; bitand_expr(p); emit_op(p->chunk,OP_BITXOR,line); } }
static void bitor_expr(Parser *p){ bitxor_expr(p); while(peek(p)->kind==T_PIPE){ int line=peek(p)->line; p->pos++; bitxor_expr(p); emit_op(p->chunk,OP_BITOR,line); } }
static int cmp_op_ahead(Parser *p){ TokKind k=peek(p)->kind; if(k==T_LT||k==T_LE||k==T_GT||k==T_GE||k==T_EQ||k==T_NE||k==T_IN||k==T_IS) return 1; if(k==T_NOT&&p->tv.v[p->pos+1].kind==T_IN) return 1; return 0; }
/* Chained comparison: a < b < c  ==  (a<b) and (b<c) with short-circuit.
   Middle operands are re-emitted (evaluated twice) - a documented simplification. */
static void comparison(Parser *p){
    bitor_expr(p);
    int endjumps[64]; int nend=0;
    while(cmp_op_ahead(p)){
        int line=peek(p)->line;
        TokKind k=peek(p)->kind;
        int is_notin=0,is_in=0,is_is=0,is_isnot=0,simple=0; Op cop=OP_EQ;
        if(k==T_NOT){ p->pos+=2; is_notin=1; }
        else if(k==T_IN){ p->pos++; is_in=1; }
        else if(k==T_IS){ p->pos++; if(match(p,T_NOT)) is_isnot=1; else is_is=1; }
        else { p->pos++; simple=1; cop=(k==T_LT?OP_LT:k==T_LE?OP_LE:k==T_GT?OP_GT:k==T_GE?OP_GE:k==T_EQ?OP_EQ:OP_NE); }
        int rstart=p->pos;
        bitor_expr(p);
        if(simple) emit_op(p->chunk,cop,line);
        else if(is_in) emit_op(p->chunk,OP_CONTAINS,line);
        else if(is_notin){ emit_op(p->chunk,OP_CONTAINS,line); emit_op(p->chunk,OP_NOT,line); }
        else if(is_is) emit_op(p->chunk,OP_IS,line);
        else if(is_isnot) emit_op(p->chunk,OP_IS_NOT,line);
        if(cmp_op_ahead(p)){
            emit_arg(p->chunk,OP_JUMP_IF_FALSE_KEEP,0,line); if(nend<64) endjumps[nend++]=p->chunk->count-1;
            emit_op(p->chunk,OP_POP,line);
            int save=p->pos; p->pos=rstart; bitor_expr(p); p->pos=save;
        }
    }
    for(int i=0;i<nend;i++) patch(p->chunk,endjumps[i],p->chunk->count);
}
static void and_expr(Parser *p){ comparison(p); while(match(p,T_AND)){ int line=prev(p)->line; emit_arg(p->chunk,OP_JUMP_IF_FALSE_KEEP,0,line); int end=p->chunk->count-1; emit_op(p->chunk,OP_POP,line); comparison(p); patch(p->chunk,end,p->chunk->count); } }
static void or_test(Parser *p){ and_expr(p); while(match(p,T_OR)){ int line=prev(p)->line; emit_arg(p->chunk,OP_JUMP_IF_TRUE_KEEP,0,line); int end=p->chunk->count-1; emit_op(p->chunk,OP_POP,line); and_expr(p); patch(p->chunk,end,p->chunk->count); } }
/* Conditional expression:  A if C else B  */
void expr(Parser *p){
    or_test(p);                                   /* A */
    if(peek(p)->kind==T_IF){
        int line=peek(p)->line; p->pos++;
        or_test(p);                               /* C */
        emit_arg(p->chunk,OP_JUMP_IF_FALSE,0,line); int jf=p->chunk->count-1;
        emit_arg(p->chunk,OP_JUMP,0,line); int jend=p->chunk->count-1;
        patch(p->chunk,jf,p->chunk->count);       /* false: discard A, eval B */
        emit_op(p->chunk,OP_POP,line);
        need(p,T_ELSE,"else");
        expr(p);                                  /* B (allows nested ternary) */
        patch(p->chunk,jend,p->chunk->count);
    }
}
static void block(Parser *p){ need(p,T_NEWLINE,"newline"); need(p,T_INDENT,"indent"); skip_nl(p); while(peek(p)->kind!=T_DEDENT&&peek(p)->kind!=T_EOF) legacy_statement(p); need(p,T_DEDENT,"dedent"); }
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
static void if_stmt(Parser *p){ int line=prev(p)->line; expr(p); need(p,T_COLON,":"); emit_arg(p->chunk,OP_JUMP_IF_FALSE,0,line); int jf=p->chunk->count-1; block(p); if(match(p,T_ELIF)){ emit_arg(p->chunk,OP_JUMP,0,line); int je=p->chunk->count-1; patch(p->chunk,jf,p->chunk->count); if_stmt(p); patch(p->chunk,je,p->chunk->count); } else if(match(p,T_ELSE)){ need(p,T_COLON,":"); emit_arg(p->chunk,OP_JUMP,0,line); int je=p->chunk->count-1; patch(p->chunk,jf,p->chunk->count); block(p); patch(p->chunk,je,p->chunk->count); } else patch(p->chunk,jf,p->chunk->count); }
static void while_stmt(Parser *p){ int start=p->chunk->count; int line=prev(p)->line; expr(p); need(p,T_COLON,":"); emit_arg(p->chunk,OP_JUMP_IF_FALSE,0,line); int jf=p->chunk->count-1; LoopCtx *lc=&p->loops[p->loop_depth++]; lc->start=start; lc->is_for=0; lc->bcount=0; block(p); emit_arg(p->chunk,OP_JUMP,start,line); int else_start=p->chunk->count; patch(p->chunk,jf,else_start); int after_else; if(match(p,T_ELSE)){ need(p,T_COLON,":"); block(p); } after_else=p->chunk->count; for(int i=0;i<lc->bcount;i++) patch(p->chunk,lc->breaks[i],after_else); p->loop_depth--; }
static void for_stmt(Parser *p){ int line=prev(p)->line; Tok *var=need(p,T_NAME,"loop variable"); need(p,T_IN,"in"); expr(p); need(p,T_COLON,":"); emit_op(p->chunk,OP_ITER,line); int start=p->chunk->count; emit_arg(p->chunk,OP_FOR_NEXT,0,line); int exit_jump=p->chunk->count-1; emit_arg(p->chunk,OP_STORE,name_const(p,var->text),line); LoopCtx *lc=&p->loops[p->loop_depth++]; lc->start=start; lc->is_for=1; lc->bcount=0; block(p); emit_arg(p->chunk,OP_JUMP,start,line); int else_start=p->chunk->count; patch(p->chunk,exit_jump,else_start); int after_else; if(match(p,T_ELSE)){ need(p,T_COLON,":"); block(p); } after_else=p->chunk->count; for(int i=0;i<lc->bcount;i++) patch(p->chunk,lc->breaks[i],after_else); p->loop_depth--; }
static void def_stmt(Parser *p){ int line=prev(p)->line; Tok *n=need(p,T_NAME,"function name"); need(p,T_LP,"("); char **params=NULL; int ar=0,cap=0; if(!match(p,T_RP)){ do{ Tok *a=need(p,T_NAME,"param"); if(ar==cap){cap=cap?cap*2:4; params=(char**)xrealloc(params,sizeof(char*)*(size_t)cap);} params[ar++]=xstrdup2(a->text); if(match(p,T_COLON)) skip_balanced_until(p,T_ASSIGN,T_COMMA); if(match(p,T_ASSIGN)){ fprintf(stderr,"parse error at line %d: default arguments are parsed but not implemented yet\n",line); exit(1); } }while(match(p,T_COMMA)); need(p,T_RP,")"); } need(p,T_COLON,":"); need(p,T_NEWLINE,"newline"); need(p,T_INDENT,"indent"); Function *fn=compile_function_from_parser(p,n->text,params,ar,1,0); emit_arg(p->chunk,OP_DEF,add_const(p->chunk,objv(fn->owner)),line); emit_arg(p->chunk,OP_STORE,name_const(p,n->text),line); }
static void class_stmt(Parser *p){ int line=prev(p)->line; Tok *n=need(p,T_NAME,"class name"); if(match(p,T_LP)){ skip_balanced_until(p,T_RP,T_NEWLINE); need(p,T_RP,")"); } need(p,T_COLON,":"); need(p,T_NEWLINE,"newline"); need(p,T_INDENT,"indent"); Function *body=compile_function_from_parser(p,n->text,NULL,0,1,1); int ci=add_const(p->chunk,objv(body->owner)); int ni=name_const(p,n->text); emit_arg(p->chunk,OP_CLASS,ci,line); emit(p->chunk,ni,line); emit_arg(p->chunk,OP_STORE,ni,line); }
static void import_stmt(Parser *p){ Tok *n=need(p,T_NAME,"module name"); int line=n->line; const char *alias=n->text; if(match(p,T_AS)){ Tok *a=need(p,T_NAME,"alias"); alias=a->text; } emit_arg(p->chunk,OP_IMPORT,name_const(p,n->text),line); emit_arg(p->chunk,OP_STORE,name_const(p,alias),line); need(p,T_NEWLINE,"newline"); }
void from_import_stmt(Parser *p){ Tok *m=need(p,T_NAME,"module name"); int line=m->line; need(p,T_IMPORT,"import"); do{ Tok *n=need(p,T_NAME,"imported name"); const char *alias=n->text; if(match(p,T_AS)){ Tok *a=need(p,T_NAME,"alias"); alias=a->text; } emit_arg(p->chunk,OP_IMPORT,name_const(p,m->text),line); emit_arg(p->chunk,OP_GET_ATTR,name_const(p,n->text),line); emit_arg(p->chunk,OP_STORE,name_const(p,alias),line); } while(match(p,T_COMMA)); need(p,T_NEWLINE,"newline"); }
static void with_stmt(Parser *p){ int line=prev(p)->line; expr(p); if(match(p,T_AS)){ Tok *n=need(p,T_NAME,"with target"); emit_arg(p->chunk,OP_STORE,name_const(p,n->text),line); } else emit_op(p->chunk,OP_POP,line); need(p,T_COLON,":"); block(p); }
static void raise_stmt(Parser *p){ int line=prev(p)->line; if(peek(p)->kind!=T_NEWLINE){ expr(p); emit_op(p->chunk,OP_RAISE,line); } else emit_op(p->chunk,OP_RERAISE,line); need(p,T_NEWLINE,"newline"); }
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

/* ---- Comprehensions ----
   Desugars  [E for x in IT if C ...]  into an accumulator temp filled by nested
   loops:   tmp=[]; for x in IT: if C: tmp.append(E); result tmp.
   Iterator/condition use or_test() (so a filter `if` is not mistaken for a
   ternary); the element re-uses full expr(). Only simple (single-name) loop
   targets are supported. Generator expressions are evaluated eagerly. */
static void comp_body(Parser *p, int kind, const char *tmp, int e1s, int e2s){
    int line=peek(p)->line;
    need(p,T_FOR,"for");
    Tok *var=need(p,T_NAME,"comprehension variable");
    need(p,T_IN,"in");
    or_test(p);                                   /* iterable */
    emit_op(p->chunk,OP_ITER,line);
    int start=p->chunk->count;
    emit_arg(p->chunk,OP_FOR_NEXT,0,line); int exit_jump=p->chunk->count-1;
    emit_arg(p->chunk,OP_STORE,name_const(p,var->text),line);
    while(peek(p)->kind==T_IF){                    /* filters -> skip to next iter */
        p->pos++; or_test(p);
        emit_arg(p->chunk,OP_JUMP_IF_FALSE,start,line);
    }
    if(peek(p)->kind==T_FOR){
        comp_body(p,kind,tmp,e1s,e2s);             /* nested loop */
    } else {
        int save=p->pos;
        if(kind=='L'||kind=='S'){
            emit_arg(p->chunk,OP_LOAD,name_const(p,tmp),line);
            emit_arg(p->chunk,OP_GET_ATTR,name_const(p,kind=='L'?"append":"add"),line);
            p->pos=e1s; expr(p);
            emit_arg(p->chunk,OP_CALL,1,line);
            emit_op(p->chunk,OP_POP,line);
        } else {                                   /* dict */
            emit_arg(p->chunk,OP_LOAD,name_const(p,tmp),line);
            p->pos=e1s; expr(p);                   /* key */
            p->pos=e2s; expr(p);                   /* value */
            emit_op(p->chunk,OP_SET_INDEX,line);
            emit_op(p->chunk,OP_POP,line);
        }
        p->pos=save;
    }
    emit_arg(p->chunk,OP_JUMP,start,line);
    patch(p->chunk,exit_jump,p->chunk->count);
}
static void compile_comprehension(Parser *p, int kind, int e1s, int e2s){
    static int comp_ctr=0;
    char tmp[24]; snprintf(tmp,sizeof(tmp),"$comp%d",comp_ctr++);
    int line=peek(p)->line;
    if(kind=='L') emit_arg(p->chunk,OP_MAKE_LIST,0,line);
    else if(kind=='S') emit_arg(p->chunk,OP_MAKE_SET,0,line);
    else emit_arg(p->chunk,OP_MAKE_DICT,0,line);
    emit_arg(p->chunk,OP_STORE,name_const(p,tmp),line);
    comp_body(p,kind,tmp,e1s,e2s);
    emit_arg(p->chunk,OP_LOAD,name_const(p,tmp),line);
}

/* Does the argument list starting at p->pos (just after '(') contain any
   *arg, **kwarg, or name=value keyword argument at top level? */
static int call_is_complex(Parser *p){
    int i=p->pos, depth=0;
    while(p->tv.v[i].kind!=T_EOF){
        TokKind k=p->tv.v[i].kind;
        if(k==T_LP||k==T_LB||k==T_LC) depth++;
        else if(k==T_RP||k==T_RB||k==T_RC){ if(depth==0) return 0; depth--; }
        else if(depth==0){
            if(k==T_STAR||k==T_POWER) return 1;
            if(k==T_NAME && p->tv.v[i+1].kind==T_ASSIGN) return 1;
        }
        i++;
    }
    return 0;
}
/* Compile a call with *args / **kwargs / keyword arguments into a positional
   list and a keyword dict, then OP_CALL_EX. Callable is already on the stack. */
static void compile_call_ex(Parser *p, int line){
    emit_arg(p->chunk,OP_MAKE_LIST,0,line);       /* positional list */
    int in_kw=0;
    if(peek(p)->kind!=T_RP){
        while(1){
            TokKind k=peek(p)->kind;
            if(k==T_POWER || (k==T_NAME && p->tv.v[p->pos+1].kind==T_ASSIGN)){ in_kw=1; break; }
            if(match(p,T_STAR)){ expr(p); emit_op(p->chunk,OP_LIST_EXTEND,line); }
            else { expr(p); emit_op(p->chunk,OP_LIST_APPEND,line); }
            if(!match(p,T_COMMA)) break;
            if(peek(p)->kind==T_RP) break;
        }
    }
    emit_arg(p->chunk,OP_MAKE_DICT,0,line);        /* keyword dict */
    if(in_kw){
        while(1){
            if(match(p,T_POWER)){ expr(p); emit_op(p->chunk,OP_DICT_MERGE,line); }
            else { Tok *n=need(p,T_NAME,"keyword name"); need(p,T_ASSIGN,"="); expr(p); emit_arg(p->chunk,OP_DICT_SETNAME,name_const(p,n->text),line); }
            if(!match(p,T_COMMA)) break;
            if(peek(p)->kind==T_RP) break;
        }
    }
    need(p,T_RP,")");
    emit_op(p->chunk,OP_CALL_EX,line);
}
