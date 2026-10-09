/* ========================= Frontend tree: constructors ========================= */

#include "ast.h"

void name_add_unique(char ***arr,int *cnt,int *cap,const char *name){
    if(!name || !*name) return;
    for(int i=0;i<*cnt;i++) if(strcmp((*arr)[i],name)==0) return;
    if(*cnt==*cap){ *cap=*cap?*cap*2:8; *arr=(char**)xrealloc(*arr,sizeof(char*)*(size_t)*cap); }
    (*arr)[(*cnt)++]=xstrdup2(name);
}

Stmt *stmt_new(StmtKind k,const char *name,int line){
    Stmt *s=(Stmt*)xmalloc(sizeof(Stmt)); memset(s,0,sizeof(Stmt));
    s->kind=k; s->name=name?xstrdup2(name):NULL; s->line=line; s->star_index=-1; s->dstar_index=-1; s->kwonly_index=-1; return s;
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
void stmt_set_annotation(Stmt *s,int param,Expr *e){
    if(param<0) return;
    if(param>=s->annotation_cap){ int n=s->annotation_cap?s->annotation_cap:4; while(n<=param) n*=2;
        s->annotations=(Expr**)xrealloc(s->annotations,sizeof(Expr*)*(size_t)n);
        for(int i=s->annotation_cap;i<n;i++) s->annotations[i]=NULL;
        s->annotation_cap=n; }
    s->annotations[param]=e;
}
void stmt_add_decorator(Stmt *s,const char *name,Expr *e){
    if(!name) return;
    if(s->decorator_count==s->decorator_cap){ s->decorator_cap=s->decorator_cap?s->decorator_cap*2:4; s->decorators=(char**)xrealloc(s->decorators,sizeof(char*)*(size_t)s->decorator_cap);
        s->decorator_exprs=(Expr**)xrealloc(s->decorator_exprs,sizeof(Expr*)*(size_t)s->decorator_cap); }
    s->decorator_exprs[s->decorator_count]=e;
    s->decorators[s->decorator_count++]=xstrdup2(name);
}

const char *stmt_kind_name(StmtKind k){
    switch(k){
        case STMT_MODULE: return "Module"; case STMT_BLOCK: return "Block"; case STMT_IF: return "IfStmt";
        case STMT_WHILE: return "WhileStmt"; case STMT_FOR: return "ForStmt"; case STMT_FUNCTION_DEF: return "FunctionDef";
        case STMT_CLASS_DEF: return "ClassDef"; case STMT_RETURN: return "ReturnStmt"; case STMT_ASSIGN: return "AssignStmt";
        case STMT_IMPORT: return "ImportStmt"; case STMT_FROM_IMPORT: return "FromImportStmt"; case STMT_RAISE: return "RaiseStmt"; case STMT_TRY: return "TryStmt";
        case STMT_WITH: return "WithStmt"; case STMT_BREAK: return "BreakStmt"; case STMT_CONTINUE: return "ContinueStmt";
        case STMT_PASS: return "PassStmt"; case STMT_DEL: return "DelStmt"; case STMT_GLOBAL: return "GlobalStmt";
        case STMT_NONLOCAL: return "NonlocalStmt"; case STMT_EXPR: return "ExprStmt"; case STMT_YIELD: return "YieldStmt";
        case STMT_MATCH: return "MatchStmt"; case STMT_CASE: return "MatchCase";
        default: return "UnsupportedStmt";
    }
}
