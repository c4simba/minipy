/* ========================= AST-driven compiler + driver ========================= */

#include "compiler.h"
#include "lexer.h"
#include "frontparser.h"

/* ---- cursor primitives (shared with expr_compiler.c) ---- */
Tok *peek(Parser *p){ return &p->tv.v[p->pos]; }
Tok *prev(Parser *p){ return &p->tv.v[p->pos-1]; }
int match(Parser *p,TokKind k){ if(peek(p)->kind==k){p->pos++;return 1;} return 0; }
Tok *need(Parser *p,TokKind k,const char *msg){ if(peek(p)->kind!=k){ fprintf(stderr,"parse error at line %d: expected %s\n",peek(p)->line,msg); exit(1);} return &p->tv.v[p->pos++]; }
int skip_nl(Parser *p){ int n=0; while(match(p,T_NEWLINE)) n++; return n; }
int name_const(Parser *p,const char *s){ return add_const(p->chunk,stringv(s)); }

/* ---- AST driven statement compiler ---- */
void compile_expr_ast(Parser *p, Expr *e){
    if(!e){ emit_op(p->chunk,OP_NONE,peek(p)->line); return; }
    int old=p->pos;
    p->pos=e->start;
    expr(p);
    if(p->pos!=e->end){ p->pos=e->end; }
    (void)old;
}
static Value const_value_from_expr(Parser *p, Expr *e){
    if(!e) return nonev();
    if(e->end==e->start+1){ Tok *t=&p->tv.v[e->start];
        if(t->kind==T_NUMBER) return t->is_float?floatv(t->f):intv(t->i);
        if(t->kind==T_STRING) return stringv(t->text);
        if(t->kind==T_TRUE) return boolv(1);
        if(t->kind==T_FALSE) return boolv(0);
        if(t->kind==T_NONE) return nonev();
    }
    fprintf(stderr,"parse error at line %d: only literal default arguments are implemented in this mini runtime\n", e->line);
    exit(1);
}
static void copy_scope_directives(Function *fn, Stmt **body, int body_count){
    for(int i=0;i<body_count;i++){ Stmt *s=body[i]; if(!s) continue;
        if(s->kind==STMT_GLOBAL){ fn->global_names=s->params; fn->global_count=s->param_count; }
        if(s->kind==STMT_NONLOCAL){ fn->nonlocal_names=s->params; fn->nonlocal_count=s->param_count; }
    }
}
static void apply_decorators(Parser *p, Stmt *s){
    for(int i=s->decorator_count-1;i>=0;i--){
        emit_arg(p->chunk,OP_LOAD,name_const(p,s->decorators[i]),s->line);
        emit_arg(p->chunk,OP_LOAD,name_const(p,s->name),s->line);
        emit_arg(p->chunk,OP_CALL,1,s->line);
        emit_arg(p->chunk,OP_STORE,name_const(p,s->name),s->line);
    }
}
static Function *compile_function_from_ast(Parser *p,const char *name,char **params,int arity,Stmt **body,int body_count,int store_globals){
    Chunk *outer=p->chunk;
    Function *fn=alloc_function(name,params,arity,p->globals,p->module_dir,store_globals);
    copy_scope_directives(fn,body,body_count);
    p->chunk=fn->chunk;
    compile_block_ast(p,body,body_count);
    emit_op(p->chunk,OP_NONE,body_count?body[body_count-1]->line:1);
    emit_op(p->chunk,OP_RETURN,body_count?body[body_count-1]->line:1);
    if(p->chunk->has_yield) fn->is_generator=1;
    p->chunk=outer;
    return fn;
}
static void compile_assign_ast(Parser *p,Stmt *s){ int old=p->pos; p->pos=s->start; assign_stmt(p); p->pos=old; }
static void compile_import_ast(Parser *p,Stmt *s){
    int line=s->line;
    const char *module=s->name2?s->name2:s->name;
    const char *alias=s->name?s->name:module;
    emit_arg(p->chunk,OP_IMPORT,name_const(p,module),line);
    emit_arg(p->chunk,OP_STORE,name_const(p,alias),line);
}
static void compile_from_import_ast(Parser *p,Stmt *s){ (void)s; int old=p->pos; p->pos=s->start; legacy_statement(p); p->pos=old; }
void compile_block_ast(Parser *p, Stmt **stmts, int count){ for(int i=0;i<count;i++) compile_stmt_ast(p,stmts[i]); }
static void compile_if_ast(Parser *p,Stmt *s){
    compile_expr_ast(p,s->expr);
    emit_arg(p->chunk,OP_JUMP_IF_FALSE,0,s->line);
    int jf=p->chunk->count-1;
    compile_block_ast(p,s->body,s->body_count);
    if(s->orelse_count){ emit_arg(p->chunk,OP_JUMP,0,s->line); int je=p->chunk->count-1; patch(p->chunk,jf,p->chunk->count); compile_block_ast(p,s->orelse,s->orelse_count); patch(p->chunk,je,p->chunk->count); }
    else patch(p->chunk,jf,p->chunk->count);
}
static void compile_while_ast(Parser *p,Stmt *s){
    int start=p->chunk->count; compile_expr_ast(p,s->expr);
    emit_arg(p->chunk,OP_JUMP_IF_FALSE,0,s->line); int jf=p->chunk->count-1;
    LoopCtx *lc=&p->loops[p->loop_depth++]; lc->start=start; lc->is_for=0; lc->bcount=0;
    compile_block_ast(p,s->body,s->body_count); emit_arg(p->chunk,OP_JUMP,start,s->line);
    int else_start=p->chunk->count; patch(p->chunk,jf,else_start);
    if(s->orelse_count) compile_block_ast(p,s->orelse,s->orelse_count);
    int after=p->chunk->count; for(int i=0;i<lc->bcount;i++) patch(p->chunk,lc->breaks[i],after); p->loop_depth--;
}
static void compile_for_ast(Parser *p,Stmt *s){
    compile_expr_ast(p,s->expr); emit_op(p->chunk,OP_ITER,s->line); int start=p->chunk->count;
    emit_arg(p->chunk,OP_FOR_NEXT,0,s->line); int exit_jump=p->chunk->count-1;
    emit_arg(p->chunk,OP_STORE,name_const(p,s->name),s->line);
    LoopCtx *lc=&p->loops[p->loop_depth++]; lc->start=start; lc->is_for=1; lc->bcount=0;
    compile_block_ast(p,s->body,s->body_count); emit_arg(p->chunk,OP_JUMP,start,s->line);
    int else_start=p->chunk->count; patch(p->chunk,exit_jump,else_start);
    if(s->orelse_count) compile_block_ast(p,s->orelse,s->orelse_count);
    int after=p->chunk->count; for(int i=0;i<lc->bcount;i++) patch(p->chunk,lc->breaks[i],after); p->loop_depth--;
}
void compile_stmt_ast(Parser *p, Stmt *s){
    if(!s) return;
    switch(s->kind){
        case STMT_MODULE: case STMT_BLOCK: compile_block_ast(p,s->body,s->body_count); break;
        case STMT_IF: compile_if_ast(p,s); break;
        case STMT_WHILE: compile_while_ast(p,s); break;
        case STMT_FOR: compile_for_ast(p,s); break;
        case STMT_FUNCTION_DEF:{
            Function *fn=compile_function_from_ast(p,s->name,s->params,s->param_count,s->body,s->body_count,0);
            fn->default_count=s->default_count; fn->min_arity=s->param_count-s->default_count;
            if(s->default_count){ fn->defaults=(Value*)xmalloc(sizeof(Value)*(size_t)s->default_count); for(int di=0; di<s->default_count; di++) fn->defaults[di]=const_value_from_expr(p,s->defaults[di]); }
            emit_arg(p->chunk,OP_DEF,add_const(p->chunk,objv(fn->owner)),s->line); emit_arg(p->chunk,OP_STORE,name_const(p,s->name),s->line); apply_decorators(p,s); break;
        }
        case STMT_CLASS_DEF:{
            Function *body=compile_function_from_ast(p,s->name,NULL,0,s->body,s->body_count,1);
            int ci=add_const(p->chunk,objv(body->owner)); int ni=name_const(p,s->name);
            emit_arg(p->chunk,OP_CLASS,ci,s->line); emit(p->chunk,ni,s->line); emit_arg(p->chunk,OP_STORE,ni,s->line); apply_decorators(p,s); break;
        }
        case STMT_RETURN: if(s->expr) compile_expr_ast(p,s->expr); else emit_op(p->chunk,OP_NONE,s->line); emit_op(p->chunk,OP_RETURN,s->line); break;
        case STMT_ASSIGN: compile_assign_ast(p,s); break;
        case STMT_IMPORT: compile_import_ast(p,s); break;
        case STMT_FROM_IMPORT: compile_from_import_ast(p,s); break;
        case STMT_RAISE: if(s->expr) compile_expr_ast(p,s->expr); else emit_arg(p->chunk,OP_CONST,add_const(p->chunk,stringv("raise")),s->line); emit_op(p->chunk,OP_RAISE,s->line); break;
        case STMT_TRY:{
            emit_arg(p->chunk,OP_SETUP_EXCEPT,0,s->line); int handler=p->chunk->count-1; compile_block_ast(p,s->body,s->body_count); emit_op(p->chunk,OP_POP_EXCEPT,s->line);
            emit_arg(p->chunk,OP_JUMP,0,s->line); int done=p->chunk->count-1; patch(p->chunk,handler,p->chunk->count);
            if(s->name) emit_arg(p->chunk,OP_STORE,name_const(p,s->name),s->line); else emit_op(p->chunk,OP_POP,s->line);
            for(int i=0;i<s->orelse_count;i++){ if(s->orelse[i]->kind!=STMT_BLOCK || !s->orelse[i]->name || strcmp(s->orelse[i]->name,"finally")!=0) compile_stmt_ast(p,s->orelse[i]); }
            patch(p->chunk,done,p->chunk->count);
            for(int i=0;i<s->orelse_count;i++){ if(s->orelse[i]->kind==STMT_BLOCK && s->orelse[i]->name && strcmp(s->orelse[i]->name,"finally")==0) compile_block_ast(p,s->orelse[i]->body,s->orelse[i]->body_count); }
            break;
        }
        case STMT_WITH: compile_expr_ast(p,s->expr); if(s->name) emit_arg(p->chunk,OP_STORE,name_const(p,s->name),s->line); else emit_op(p->chunk,OP_POP,s->line); compile_block_ast(p,s->body,s->body_count); break;
        case STMT_BREAK:{
            if(p->loop_depth<=0){ fprintf(stderr,"parse error at line %d: break outside loop\n",s->line); exit(1); }
            LoopCtx *lc=&p->loops[p->loop_depth-1]; if(lc->is_for) emit_op(p->chunk,OP_POP,s->line);
            emit_arg(p->chunk,OP_JUMP,0,s->line); lc->breaks[lc->bcount++]=p->chunk->count-1; break;
        }
        case STMT_CONTINUE:{
            if(p->loop_depth<=0){ fprintf(stderr,"parse error at line %d: continue outside loop\n",s->line); exit(1); }
            LoopCtx *lc=&p->loops[p->loop_depth-1]; emit_arg(p->chunk,OP_JUMP,lc->start,s->line); break;
        }
        case STMT_PASS: case STMT_GLOBAL: case STMT_NONLOCAL: break;
        case STMT_DEL: emit_op(p->chunk,OP_NONE,s->line); emit_arg(p->chunk,OP_STORE,name_const(p,s->name),s->line); break;
        case STMT_EXPR:{
            int old=p->pos; p->pos=s->start; if(peek(p)->kind==T_PRINT || is_assignment(p)){ legacy_statement(p); p->pos=old; break; } p->pos=old;
            compile_expr_ast(p,s->expr); emit_op(p->chunk,OP_POP,s->line); break;
        }
        case STMT_YIELD: if(s->expr) compile_expr_ast(p,s->expr); else emit_op(p->chunk,OP_NONE,s->line); emit_op(p->chunk,OP_YIELD,s->line); break;
        case STMT_UNSUPPORTED:
        default: fprintf(stderr,"parse error at line %d: %s syntax is recognized but not implemented in this mini runtime\n",s->line,s->name?s->name:"unsupported"); exit(1);
    }
}

/* ---- Function object allocation + top-level driver ---- */
Function *alloc_function(const char *name,char **params,int arity,Dict *globals,char *dir,int store_globals){ Obj *o=new_obj(O_FUNCTION); o->as.fn.name=xstrdup2(name); o->as.fn.params=params; o->as.fn.arity=arity; o->as.fn.min_arity=arity; o->as.fn.default_count=0; o->as.fn.defaults=NULL; o->as.fn.chunk=chunk_new(); o->as.fn.globals=globals; o->as.fn.closure=NULL; o->as.fn.module_dir=dir?xstrdup2(dir):xstrdup2("."); o->as.fn.store_globals=store_globals; o->as.fn.is_generator=0; o->as.fn.owner=o; return &o->as.fn; }
Function *clone_function(Function *src, Dict *closure){ Obj *o=new_obj(O_FUNCTION); o->as.fn=*src; o->as.fn.name=xstrdup2(src->name); o->as.fn.closure=closure; o->as.fn.owner=o; return &o->as.fn; }
Function *compile_source(const char *src,const char *name,const char *dir,Dict *globals){
    fprintf(stderr, "[minipy] compile: lex...\n"); fflush(stderr);
    TokVec tv=lex(src);
    fprintf(stderr, "[minipy] compile: lex done, %d tokens\n", tv.n); fflush(stderr);
    SymTable sym={0};
    fprintf(stderr, "[minipy] compile: build_ast...\n"); fflush(stderr);
    Ast *ast_root=build_ast_and_symbols(&tv,&sym);
    fprintf(stderr, "[minipy] compile: ast done\n"); fflush(stderr);
    Parser p={0}; p.tv=tv; p.globals=globals; p.module_dir=(char*)dir;
    fprintf(stderr, "[minipy] compile: alloc_function...\n"); fflush(stderr);
    Function *mainfn=alloc_function(name,NULL,0,globals,(char*)dir,1);
    fprintf(stderr, "[minipy] compile: alloc done, compiling ast...\n"); fflush(stderr);
    p.chunk=mainfn->chunk;
    compile_stmt_ast(&p,ast_root);
    fprintf(stderr, "[minipy] compile: ast compiled, emitting RETURN\n"); fflush(stderr);
    emit_op(p.chunk,OP_NONE,peek(&p)->line);
    emit_op(p.chunk,OP_RETURN,peek(&p)->line);
    fprintf(stderr, "[minipy] compile: done\n"); fflush(stderr);
    return mainfn;
}
