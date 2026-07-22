/* ========================= Frontend parser + symbol table ========================= */

#include "frontparser.h"
#include "lexer.h"

typedef struct { TokVec *tv; int pos; } FrontParser;
static Tok *fp_peek(FrontParser *p){ return &p->tv->v[p->pos]; }
static int fp_match(FrontParser *p,TokKind k){ if(fp_peek(p)->kind==k){ p->pos++; return 1; } return 0; }
static int fp_skip_nl(FrontParser *p){ int n=0; while(fp_match(p,T_NEWLINE)) n++; return n; }
static Tok *fp_need(FrontParser *p,TokKind k){ if(fp_peek(p)->kind!=k) return NULL; return &p->tv->v[p->pos++]; }
static void fp_skip_balanced_to(FrontParser *p,TokKind stop1,TokKind stop2){
    int depth=0;
    while(fp_peek(p)->kind!=T_EOF){
        TokKind k=fp_peek(p)->kind;
        if(depth==0&&(k==stop1||k==stop2||k==T_NEWLINE)) return;
        if(k==T_LP||k==T_LB||k==T_LC) depth++;
        else if(k==T_RP||k==T_RB||k==T_RC){ if(depth>0) depth--; }
        p->pos++;
    }
}
static Expr *fp_expr_until(FrontParser *p,TokKind stop1,TokKind stop2){
    int start=p->pos; int line=fp_peek(p)->line;
    fp_skip_balanced_to(p,stop1,stop2);
    return expr_new_range(start,p->pos,line);
}
static Stmt *fp_parse_stmt(FrontParser *p);
static void fp_parse_suite_into(FrontParser *p,Stmt *owner,int into_else){
    if(fp_peek(p)->kind!=T_NEWLINE){
        /* single-line suite: one simple statement on the same line as the ':' */
        Stmt *s=fp_parse_stmt(p); if(into_else) stmt_add_orelse(owner,s); else stmt_add_body(owner,s);
        return;
    }
    fp_need(p,T_NEWLINE); fp_need(p,T_INDENT); fp_skip_nl(p);
    while(fp_peek(p)->kind!=T_EOF && fp_peek(p)->kind!=T_DEDENT){ Stmt *s=fp_parse_stmt(p); if(into_else) stmt_add_orelse(owner,s); else stmt_add_body(owner,s); fp_skip_nl(p); }
    fp_need(p,T_DEDENT);
}
static int fp_target_name_after_from_import(FrontParser *p,Stmt *s){
    Tok *n=fp_need(p,T_NAME); if(!n) return 0;
    s->name2=xstrdup2(n->text);
    if(fp_match(p,T_AS)){ Tok *a=fp_need(p,T_NAME); if(a) s->name=xstrdup2(a->text); }
    else s->name=xstrdup2(n->text);
    return 1;
}
static Stmt *fp_parse_simple_name_list(FrontParser *p,StmtKind kind,int line,int start){
    Stmt *s=stmt_new(kind,NULL,line,start);
    while(1){ Tok *n=fp_need(p,T_NAME); if(n) name_add_unique(&s->params,&s->param_count,&s->param_cap,n->text); if(!fp_match(p,T_COMMA)) break; }
    fp_need(p,T_NEWLINE); s->end=p->pos; return s;
}
/* Does the current logical line contain a top-level assignment operator? */
static int fp_stmt_has_assign(FrontParser *p){
    int i=p->pos, depth=0;
    while(p->tv->v[i].kind!=T_EOF && !(depth==0 && p->tv->v[i].kind==T_NEWLINE)){
        TokKind k=p->tv->v[i].kind;
        if(k==T_LP||k==T_LB||k==T_LC) depth++;
        else if(k==T_RP||k==T_RB||k==T_RC){ if(depth>0) depth--; }
        else if(depth==0 && is_assign_op(k)) return 1;
        i++;
    }
    return 0;
}
static Stmt *fp_parse_stmt(FrontParser *p){
    fp_skip_nl(p); int start=p->pos; Tok *t=fp_peek(p); int line=t->line;
    char **pending_decorators=NULL; int dec_count=0, dec_cap=0;
    while(fp_match(p,T_AT)){
        Tok *d=fp_need(p,T_NAME); if(d) name_add_unique(&pending_decorators,&dec_count,&dec_cap,d->text);
        fp_skip_balanced_to(p,T_NEWLINE,T_EOF); fp_need(p,T_NEWLINE); fp_skip_nl(p);
        start=p->pos; t=fp_peek(p); line=t->line;
    }
    if(fp_match(p,T_IF)){
        Stmt *s=stmt_new(STMT_IF,NULL,line,start); s->expr=fp_expr_until(p,T_COLON,T_EOF); fp_need(p,T_COLON); fp_parse_suite_into(p,s,0);
        if(fp_match(p,T_ELIF)){ p->pos--; Stmt *elif_s=fp_parse_stmt(p); stmt_add_orelse(s,elif_s); }
        else if(fp_match(p,T_ELSE)){ fp_need(p,T_COLON); Stmt *b=stmt_new(STMT_BLOCK,"else",line,p->pos); fp_parse_suite_into(p,b,0); for(int i=0;i<b->body_count;i++) stmt_add_orelse(s,b->body[i]); }
        s->end=p->pos; return s;
    }
    if(fp_match(p,T_ELIF)){
        Stmt *s=stmt_new(STMT_IF,NULL,line,start); s->expr=fp_expr_until(p,T_COLON,T_EOF); fp_need(p,T_COLON); fp_parse_suite_into(p,s,0);
        if(fp_match(p,T_ELIF)){ p->pos--; Stmt *elif_s=fp_parse_stmt(p); stmt_add_orelse(s,elif_s); }
        else if(fp_match(p,T_ELSE)){ fp_need(p,T_COLON); Stmt *b=stmt_new(STMT_BLOCK,"else",line,p->pos); fp_parse_suite_into(p,b,0); for(int i=0;i<b->body_count;i++) stmt_add_orelse(s,b->body[i]); }
        s->end=p->pos; return s;
    }
    if(fp_match(p,T_WHILE)){ Stmt *s=stmt_new(STMT_WHILE,NULL,line,start); s->expr=fp_expr_until(p,T_COLON,T_EOF); fp_need(p,T_COLON); fp_parse_suite_into(p,s,0); if(fp_match(p,T_ELSE)){ fp_need(p,T_COLON); Stmt *b=stmt_new(STMT_BLOCK,"else",line,p->pos); fp_parse_suite_into(p,b,0); for(int i=0;i<b->body_count;i++) stmt_add_orelse(s,b->body[i]); } s->end=p->pos; return s; }
    if(fp_match(p,T_FOR)){ Stmt *s=stmt_new(STMT_FOR,NULL,line,start); do{ Tok *n=fp_need(p,T_NAME); if(n) name_add_unique(&s->params,&s->param_count,&s->param_cap,n->text); }while(fp_match(p,T_COMMA)); if(s->param_count==1) s->name=xstrdup2(s->params[0]); fp_need(p,T_IN); s->expr=fp_expr_until(p,T_COLON,T_EOF); fp_need(p,T_COLON); fp_parse_suite_into(p,s,0); if(fp_match(p,T_ELSE)){ fp_need(p,T_COLON); Stmt *b=stmt_new(STMT_BLOCK,"else",line,p->pos); fp_parse_suite_into(p,b,0); for(int i=0;i<b->body_count;i++) stmt_add_orelse(s,b->body[i]); } s->end=p->pos; return s; }
    if(fp_match(p,T_DEF)){
        Tok *n=fp_need(p,T_NAME); Stmt *s=stmt_new(STMT_FUNCTION_DEF,n?n->text:"<anon>",line,start);
        for(int di=0; di<dec_count; di++) stmt_add_decorator(s,pending_decorators[di]);
        fp_need(p,T_LP);
        while(fp_peek(p)->kind!=T_EOF && fp_peek(p)->kind!=T_RP){
            if(fp_match(p,T_POWER)){ Tok *a=fp_need(p,T_NAME); if(a){ s->dstar_index=s->param_count; name_add_unique(&s->params,&s->param_count,&s->param_cap,a->text); } }
            else if(fp_match(p,T_STAR)){ Tok *a=fp_need(p,T_NAME); if(a){ s->star_index=s->param_count; name_add_unique(&s->params,&s->param_count,&s->param_cap,a->text); } }
            else {
                Tok *a=fp_need(p,T_NAME); if(a) name_add_unique(&s->params,&s->param_count,&s->param_cap,a->text);
                if(fp_match(p,T_COLON)) fp_skip_balanced_to(p,T_COMMA,T_RP);
                if(fp_match(p,T_ASSIGN)) stmt_add_default(s,fp_expr_until(p,T_COMMA,T_RP));
            }
            if(!fp_match(p,T_COMMA)) break;
        }
        fp_need(p,T_RP); fp_need(p,T_COLON); fp_parse_suite_into(p,s,0); s->end=p->pos; return s;
    }
    if(fp_match(p,T_CLASS)){ Tok *n=fp_need(p,T_NAME); Stmt *s=stmt_new(STMT_CLASS_DEF,n?n->text:"<class>",line,start); for(int di=0; di<dec_count; di++) stmt_add_decorator(s,pending_decorators[di]); if(fp_match(p,T_LP)){ if(fp_peek(p)->kind==T_NAME){ Tok *b=fp_need(p,T_NAME); s->name2=xstrdup2(b->text); } fp_skip_balanced_to(p,T_RP,T_NEWLINE); fp_need(p,T_RP); } fp_need(p,T_COLON); fp_parse_suite_into(p,s,0); s->end=p->pos; return s; }
    if(fp_match(p,T_RETURN)){ Stmt *s=stmt_new(STMT_RETURN,NULL,line,start); if(fp_peek(p)->kind!=T_NEWLINE) s->expr=fp_expr_until(p,T_NEWLINE,T_EOF); fp_need(p,T_NEWLINE); s->end=p->pos; return s; }
    if(fp_match(p,T_RAISE)){ Stmt *s=stmt_new(STMT_RAISE,NULL,line,start); if(fp_peek(p)->kind!=T_NEWLINE) s->expr=fp_expr_until(p,T_NEWLINE,T_EOF); fp_need(p,T_NEWLINE); s->end=p->pos; return s; }
    if(fp_match(p,T_IMPORT)){
        Stmt *s=stmt_new(STMT_IMPORT,NULL,line,start);
        char buf[256]; int bl=0; buf[0]=0;                          /* dotted module name: NAME (. NAME)* */
        Tok *n=fp_need(p,T_NAME); if(n) bl+=snprintf(buf+bl,sizeof(buf)-(size_t)bl,"%s",n->text);
        while(fp_match(p,T_DOT)){ Tok *m=fp_need(p,T_NAME); if(m && bl<250) bl+=snprintf(buf+bl,sizeof(buf)-(size_t)bl,".%s",m->text); }
        s->name2=xstrdup2(buf);
        if(fp_match(p,T_AS)){ Tok *a=fp_need(p,T_NAME); if(a) s->name=xstrdup2(a->text); s->block_tag=1; }
        else { char top[128]; int i=0; while(buf[i] && buf[i]!='.' && i<127){ top[i]=buf[i]; i++; } top[i]=0; s->name=xstrdup2(top); }
        fp_skip_balanced_to(p,T_NEWLINE,T_EOF); fp_need(p,T_NEWLINE); s->end=p->pos; return s;
    }
    if(fp_match(p,T_FROM)){ Stmt *s=stmt_new(STMT_FROM_IMPORT,NULL,line,start); Tok *m=fp_need(p,T_NAME); if(m) s->name2=xstrdup2(m->text); fp_need(p,T_IMPORT); fp_target_name_after_from_import(p,s); fp_skip_balanced_to(p,T_NEWLINE,T_EOF); fp_need(p,T_NEWLINE); s->end=p->pos; return s; }
    if(fp_match(p,T_WITH)){ Stmt *s=stmt_new(STMT_WITH,NULL,line,start); s->expr=fp_expr_until(p,T_COLON,T_EOF); fp_need(p,T_COLON); fp_parse_suite_into(p,s,0); s->end=p->pos; return s; }
    if(fp_match(p,T_GLOBAL)) return fp_parse_simple_name_list(p,STMT_GLOBAL,line,start);
    if(fp_match(p,T_NONLOCAL)) return fp_parse_simple_name_list(p,STMT_NONLOCAL,line,start);
    if(fp_match(p,T_DEL)){ Stmt *s=stmt_new(STMT_DEL,NULL,line,start); s->expr=fp_expr_until(p,T_NEWLINE,T_EOF); fp_need(p,T_NEWLINE); s->end=p->pos; return s; }
    if(fp_match(p,T_ASSERT)){ Stmt *s=stmt_new(STMT_ASSERT,NULL,line,start); s->expr=fp_expr_until(p,T_COMMA,T_NEWLINE); if(fp_match(p,T_COMMA)) s->expr2=fp_expr_until(p,T_NEWLINE,T_EOF); fp_need(p,T_NEWLINE); s->end=p->pos; return s; }
    if(fp_match(p,T_BREAK)){ Stmt *s=stmt_new(STMT_BREAK,NULL,line,start); fp_need(p,T_NEWLINE); s->end=p->pos; return s; }
    if(fp_match(p,T_CONTINUE)){ Stmt *s=stmt_new(STMT_CONTINUE,NULL,line,start); fp_need(p,T_NEWLINE); s->end=p->pos; return s; }
    if(fp_match(p,T_PASS)){ Stmt *s=stmt_new(STMT_PASS,NULL,line,start); fp_need(p,T_NEWLINE); s->end=p->pos; return s; }
    if(fp_match(p,T_YIELD)){ Stmt *s=stmt_new(STMT_YIELD,NULL,line,start); if(fp_peek(p)->kind!=T_NEWLINE) s->expr=fp_expr_until(p,T_NEWLINE,T_EOF); fp_need(p,T_NEWLINE); s->end=p->pos; return s; }
    if(fp_match(p,T_TRY)){
        Stmt *s=stmt_new(STMT_TRY,NULL,line,start); fp_need(p,T_COLON); fp_parse_suite_into(p,s,0); fp_skip_nl(p);
        while(fp_match(p,T_EXCEPT)){
            Stmt *b=stmt_new(STMT_BLOCK,NULL,line,p->pos); b->block_tag=1;
            if(fp_peek(p)->kind!=T_COLON){ b->expr=fp_expr_until(p,T_AS,T_COLON); if(fp_match(p,T_AS)){ Tok *as=fp_need(p,T_NAME); if(as) b->name=xstrdup2(as->text); } }
            fp_need(p,T_COLON); fp_parse_suite_into(p,b,0); stmt_add_orelse(s,b); fp_skip_nl(p);
        }
        if(fp_match(p,T_ELSE)){ fp_need(p,T_COLON); Stmt *el=stmt_new(STMT_BLOCK,"else",line,p->pos); el->block_tag=2; fp_parse_suite_into(p,el,0); stmt_add_orelse(s,el); fp_skip_nl(p); }
        if(fp_match(p,T_FINALLY)){ fp_need(p,T_COLON); Stmt *fin=stmt_new(STMT_BLOCK,"finally",line,p->pos); fin->block_tag=3; fp_parse_suite_into(p,fin,0); stmt_add_orelse(s,fin); }
        s->end=p->pos; return s;
    }
    if(fp_peek(p)->kind==T_MATCH||fp_peek(p)->kind==T_ASYNC){ Stmt *s=stmt_new(STMT_UNSUPPORTED,fp_peek(p)->text,line,start); fp_skip_balanced_to(p,T_NEWLINE,T_EOF); fp_need(p,T_NEWLINE); s->end=p->pos; return s; }

    if(fp_stmt_has_assign(p)){
        Stmt *s=stmt_new(STMT_ASSIGN,(fp_peek(p)->kind==T_NAME?fp_peek(p)->text:NULL),line,start);
        while(fp_peek(p)->kind!=T_EOF && fp_peek(p)->kind!=T_NEWLINE) p->pos++;
        fp_need(p,T_NEWLINE); s->end=p->pos; return s;
    }
    Stmt *s=stmt_new(STMT_EXPR,NULL,line,start); s->expr=fp_expr_until(p,T_NEWLINE,T_EOF); fp_need(p,T_NEWLINE); s->end=p->pos; return s;
}

static void collect_expr_uses(TokVec *tv,Expr *e,SymScope *scope){
    if(!e) return;
    for(int i=e->start;i<e->end;i++){
        if(i>=0 && i<tv->n && is_expr_name_token(tv->v[i].kind)) scope_use(scope,tv->v[i].text);
    }
    for(int i=0;i<e->count;i++) collect_expr_uses(tv,e->items[i],scope);
}
static void sym_visit_stmt(TokVec *tv,Stmt *s,SymScope *scope){
    if(!s) return;
    switch(s->kind){
        case STMT_MODULE:
        case STMT_BLOCK:
            for(int i=0;i<s->body_count;i++) sym_visit_stmt(tv,s->body[i],scope);
            break;
        case STMT_FUNCTION_DEF:{
            scope_def(scope,s->name);
            SymScope *fn=scope_new(SYM_FUNCTION,s->name,s->line); s->scope=fn; scope_add_child(scope,fn);
            for(int i=0;i<s->param_count;i++) scope_def(fn,s->params[i]);
            for(int i=0;i<s->default_count;i++) collect_expr_uses(tv,s->defaults[i],scope);
            for(int i=0;i<s->decorator_count;i++) scope_use(scope,s->decorators[i]);
            for(int i=0;i<s->body_count;i++) sym_visit_stmt(tv,s->body[i],fn);
            break;
        }
        case STMT_CLASS_DEF:{
            scope_def(scope,s->name);
            SymScope *cl=scope_new(SYM_CLASS,s->name,s->line); s->scope=cl; scope_add_child(scope,cl);
            for(int i=0;i<s->decorator_count;i++) scope_use(scope,s->decorators[i]);
            for(int i=0;i<s->body_count;i++) sym_visit_stmt(tv,s->body[i],cl);
            break;
        }
        case STMT_ASSIGN: scope_def(scope,s->name); collect_expr_uses(tv,s->expr,scope); break;
        case STMT_FOR: scope_def(scope,s->name); collect_expr_uses(tv,s->expr,scope); for(int i=0;i<s->body_count;i++) sym_visit_stmt(tv,s->body[i],scope); for(int i=0;i<s->orelse_count;i++) sym_visit_stmt(tv,s->orelse[i],scope); break;
        case STMT_IF: case STMT_WHILE: case STMT_WITH: case STMT_TRY:
            collect_expr_uses(tv,s->expr,scope); if(s->kind==STMT_WITH && s->name) scope_def(scope,s->name);
            for(int i=0;i<s->body_count;i++) sym_visit_stmt(tv,s->body[i],scope);
            for(int i=0;i<s->orelse_count;i++) sym_visit_stmt(tv,s->orelse[i],scope);
            break;
        case STMT_IMPORT: if(s->name) scope_def(scope,s->name); break;
        case STMT_FROM_IMPORT: if(s->name) scope_def(scope,s->name); break;
        case STMT_GLOBAL: for(int i=0;i<s->param_count;i++) scope_global(scope,s->params[i]); break;
        case STMT_NONLOCAL: for(int i=0;i<s->param_count;i++) scope_nonlocal(scope,s->params[i]); break;
        case STMT_DEL: if(s->name) scope_def(scope,s->name); break;
        case STMT_RETURN: case STMT_RAISE: case STMT_EXPR: case STMT_YIELD: collect_expr_uses(tv,s->expr,scope); break;
        default: break;
    }
}
static void flatten_scope_into_table(SymScope *sc,SymTable *st){
    for(int i=0;i<sc->def_count;i++) name_add_unique(&st->defs,&st->def_count,&st->def_cap,sc->defs[i]);
    for(int i=0;i<sc->use_count;i++) name_add_unique(&st->uses,&st->use_count,&st->use_cap,sc->uses[i]);
    for(int i=0;i<sc->child_count;i++) flatten_scope_into_table(sc->children[i],st);
}
Ast *build_ast_and_symbols(TokVec *tv, SymTable *st){
    FrontParser p={0}; p.tv=tv; p.pos=0;
    Stmt *root=stmt_new(STMT_MODULE,"<module>",1,0);
    fp_skip_nl(&p);
    while(fp_peek(&p)->kind!=T_EOF){ Stmt *s=fp_parse_stmt(&p); stmt_add_body(root,s); fp_skip_nl(&p); }
    root->end=p.pos;
    SymScope *mod=scope_new(SYM_MODULE,"<module>",1); root->scope=mod; st->root_scope=mod;
    sym_visit_stmt(tv,root,mod); flatten_scope_into_table(mod,st);
    return root;
}

static void dump_expr(TokVec *tv,Expr *e,int indent){
    if(!e) return;
    for(int i=0;i<indent;i++) printf("  ");
    printf("ExprRange line %d tokens %d..%d",e->line,e->start,e->end);
    if(tv){ printf(" ["); for(int i=e->start;i<e->end && i<tv->n;i++){ if(i>e->start) printf(" "); printf("%s",tv->v[i].text?tv->v[i].text:"?"); } printf("]"); }
    printf("\n");
}
static void dump_ast_with_tokens(TokVec *tv,Ast *a,int indent){
    for(int i=0;i<indent;i++) printf("  ");
    printf("%s",stmt_kind_name(a->kind));
    if(a->name) printf(" %s",a->name);
    if(a->name2) printf(" from %s",a->name2);
    printf(" (line %d, tokens %d..%d)\n",a->line,a->start,a->end);
    if(a->decorator_count){ for(int i=0;i<indent+1;i++) printf("  "); printf("decorators:"); for(int i=0;i<a->decorator_count;i++) printf(" @%s",a->decorators[i]); printf("\n"); }
    if(a->expr) dump_expr(tv,a->expr,indent+1);
    if(a->param_count){ for(int i=0;i<indent+1;i++) printf("  "); printf("params:"); for(int i=0;i<a->param_count;i++) printf(" %s",a->params[i]); printf("\n"); }
    if(a->default_count){ for(int i=0;i<indent+1;i++) printf("  "); printf("defaults: %d\n",a->default_count); for(int i=0;i<a->default_count;i++) dump_expr(tv,a->defaults[i],indent+2); }
    for(int i=0;i<a->body_count;i++) dump_ast_with_tokens(tv,a->body[i],indent+1);
    if(a->orelse_count){ for(int i=0;i<indent+1;i++) printf("  "); printf("orelse:\n"); for(int i=0;i<a->orelse_count;i++) dump_ast_with_tokens(tv,a->orelse[i],indent+2); }
}
static void dump_scope(SymScope *s,int indent){
    for(int i=0;i<indent;i++) printf("  ");
    printf("scope %s %s (line %d)\n",scope_kind_name(s->kind),s->name,s->line);
    if(s->def_count){ for(int i=0;i<indent+1;i++) printf("  "); printf("defs:"); for(int i=0;i<s->def_count;i++) printf(" %s",s->defs[i]); printf("\n"); }
    if(s->use_count){ for(int i=0;i<indent+1;i++) printf("  "); printf("uses:"); for(int i=0;i<s->use_count;i++) printf(" %s",s->uses[i]); printf("\n"); }
    if(s->global_count){ for(int i=0;i<indent+1;i++) printf("  "); printf("globals:"); for(int i=0;i<s->global_count;i++) printf(" %s",s->globals[i]); printf("\n"); }
    if(s->nonlocal_count){ for(int i=0;i<indent+1;i++) printf("  "); printf("nonlocals:"); for(int i=0;i<s->nonlocal_count;i++) printf(" %s",s->nonlocals[i]); printf("\n"); }
    for(int i=0;i<s->child_count;i++) dump_scope(s->children[i],indent+1);
}
static Ast *parse_ast_for_source(const char *src, TokVec *out_tv, SymTable *out_st){
    *out_tv=lex(src);
    memset(out_st,0,sizeof(*out_st));
    return build_ast_and_symbols(out_tv,out_st);
}
void dump_ast_for_source(const char *src){
    TokVec tv; SymTable st; Ast *root=parse_ast_for_source(src,&tv,&st);
    (void)st;
    printf("AST:\n");
    dump_ast_with_tokens(&tv,root,1);
}
void dump_symbols_for_source(const char *src){
    TokVec tv; SymTable st; Ast *root=parse_ast_for_source(src,&tv,&st);
    (void)root;
    printf("Symbol table:\n"); if(st.root_scope) dump_scope(st.root_scope,1);
    printf("\nFlat definitions:"); if(st.def_count==0) printf(" <none>"); for(int i=0;i<st.def_count;i++) printf(" %s", st.defs[i]);
    printf("\nFlat uses:"); if(st.use_count==0) printf(" <none>"); for(int i=0;i<st.use_count;i++) printf(" %s", st.uses[i]); printf("\n");
}
