/* ========================= Frontend AST + Symbol Table: constructors ========================= */

#include "ast.h"

void name_add_unique(char ***arr,int *cnt,int *cap,const char *name){
    if(!name || !*name) return;
    for(int i=0;i<*cnt;i++) if(strcmp((*arr)[i],name)==0) return;
    if(*cnt==*cap){ *cap=*cap?*cap*2:8; *arr=(char**)xrealloc(*arr,sizeof(char*)*(size_t)*cap); }
    (*arr)[(*cnt)++]=xstrdup2(name);
}

Expr *expr_new_range(int start,int end,int line){
    Expr *e=(Expr*)xmalloc(sizeof(Expr)); memset(e,0,sizeof(Expr));
    e->kind=EXPR_TOKEN_RANGE; e->start=start; e->end=end; e->line=line; return e;
}
Stmt *stmt_new(StmtKind k,const char *name,int line,int start){
    Stmt *s=(Stmt*)xmalloc(sizeof(Stmt)); memset(s,0,sizeof(Stmt));
    s->kind=k; s->name=name?xstrdup2(name):NULL; s->line=line; s->start=start; s->end=start; s->star_index=-1; s->dstar_index=-1; return s;
}
void stmt_add_body(Stmt *s,Stmt *child){
    if(!child) return;
    if(s->body_count==s->body_cap){ s->body_cap=s->body_cap?s->body_cap*2:8; s->body=(Stmt**)xrealloc(s->body,sizeof(Stmt*)*(size_t)s->body_cap); }
    s->body[s->body_count++]=child;
}
void stmt_add_orelse(Stmt *s,Stmt *child){
    if(!child) return;
    if(s->orelse_count==s->orelse_cap){ s->orelse_cap=s->orelse_cap?s->orelse_cap*2:4; s->orelse=(Stmt**)xrealloc(s->orelse,sizeof(Stmt*)*(size_t)s->orelse_cap); }
    s->orelse[s->orelse_count++]=child;
}
void stmt_add_default(Stmt *s,Expr *e){
    if(s->default_count==s->default_cap){ s->default_cap=s->default_cap?s->default_cap*2:4; s->defaults=(Expr**)xrealloc(s->defaults,sizeof(Expr*)*(size_t)s->default_cap); }
    s->defaults[s->default_count++]=e;
}
void stmt_add_decorator(Stmt *s,const char *name){
    if(!name) return;
    if(s->decorator_count==s->decorator_cap){ s->decorator_cap=s->decorator_cap?s->decorator_cap*2:4; s->decorators=(char**)xrealloc(s->decorators,sizeof(char*)*(size_t)s->decorator_cap); }
    s->decorators[s->decorator_count++]=xstrdup2(name);
}
SymScope *scope_new(SymScopeKind k,const char *name,int line){
    SymScope *sc=(SymScope*)xmalloc(sizeof(SymScope)); memset(sc,0,sizeof(SymScope));
    sc->kind=k; sc->name=xstrdup2(name?name:"<scope>"); sc->line=line; return sc;
}
void scope_add_child(SymScope *p,SymScope *c){
    if(!p||!c) return;
    if(p->child_count==p->child_cap){ p->child_cap=p->child_cap?p->child_cap*2:4; p->children=(SymScope**)xrealloc(p->children,sizeof(SymScope*)*(size_t)p->child_cap); }
    p->children[p->child_count++]=c;
}
void scope_def(SymScope *s,const char *name){ name_add_unique(&s->defs,&s->def_count,&s->def_cap,name); }
void scope_use(SymScope *s,const char *name){ name_add_unique(&s->uses,&s->use_count,&s->use_cap,name); }
void scope_global(SymScope *s,const char *name){ name_add_unique(&s->globals,&s->global_count,&s->global_cap,name); }
void scope_nonlocal(SymScope *s,const char *name){ name_add_unique(&s->nonlocals,&s->nonlocal_count,&s->nonlocal_cap,name); }

const char *stmt_kind_name(StmtKind k){
    switch(k){
        case STMT_MODULE: return "Module"; case STMT_BLOCK: return "Block"; case STMT_IF: return "IfStmt";
        case STMT_WHILE: return "WhileStmt"; case STMT_FOR: return "ForStmt"; case STMT_FUNCTION_DEF: return "FunctionDef";
        case STMT_CLASS_DEF: return "ClassDef"; case STMT_RETURN: return "ReturnStmt"; case STMT_ASSIGN: return "AssignStmt";
        case STMT_IMPORT: return "ImportStmt"; case STMT_FROM_IMPORT: return "FromImportStmt"; case STMT_RAISE: return "RaiseStmt"; case STMT_TRY: return "TryStmt";
        case STMT_WITH: return "WithStmt"; case STMT_BREAK: return "BreakStmt"; case STMT_CONTINUE: return "ContinueStmt";
        case STMT_PASS: return "PassStmt"; case STMT_DEL: return "DelStmt"; case STMT_GLOBAL: return "GlobalStmt";
        case STMT_NONLOCAL: return "NonlocalStmt"; case STMT_EXPR: return "ExprStmt"; case STMT_YIELD: return "YieldStmt";
        default: return "UnsupportedStmt";
    }
}
const char *scope_kind_name(SymScopeKind k){ return k==SYM_MODULE?"module":k==SYM_FUNCTION?"function":"class"; }

int is_expr_name_token(TokKind k){ return k==T_NAME; }
int is_assign_op(TokKind k){ return k==T_ASSIGN||k==T_PLUS_ASSIGN||k==T_MINUS_ASSIGN||k==T_STAR_ASSIGN||k==T_SLASH_ASSIGN||k==T_PERCENT_ASSIGN||k==T_FLOOR_DIV_ASSIGN||k==T_POWER_ASSIGN||k==T_AMP_ASSIGN||k==T_PIPE_ASSIGN||k==T_CARET_ASSIGN||k==T_SHL_ASSIGN||k==T_SHR_ASSIGN; }
