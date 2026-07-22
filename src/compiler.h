#ifndef MPY_COMPILER_H
#define MPY_COMPILER_H

#include "ast.h"
#include "bytecode.h"
#include "containers.h"

/* ========================= Parser / Compiler =========================
   Two compile paths share the Parser state and cursor primitives:
   - compiler.c       : AST-driven statement compiler + top-level driver
   - expr_compiler.c  : legacy recursive-descent expression/statement grammar
   The two meet through the cross declarations below. */

typedef struct { int start; int is_for; int breaks[256]; int bcount; } LoopCtx;
typedef struct Parser { TokVec tv; int pos; Chunk *chunk; Dict *globals; char *module_dir; LoopCtx loops[64]; int loop_depth; } Parser;

/* Cursor primitives (defined in compiler.c). */
Tok *peek(Parser *p);
Tok *prev(Parser *p);
int  match(Parser *p, TokKind k);
Tok *need(Parser *p, TokKind k, const char *msg);
int  skip_nl(Parser *p);
int  name_const(Parser *p, const char *s);

/* Object-model helpers + AST-driven compiler (compiler.c). */
Function *alloc_function(const char *name, char **params, int arity, Dict *globals, char *dir, int store_globals);
Function *clone_function(Function *src, Dict *closure);
void      compile_expr_ast(Parser *p, Expr *e);
void      compile_block_ast(Parser *p, Stmt **stmts, int count);
void      compile_stmt_ast(Parser *p, Stmt *s);
Function *compile_source(const char *src, const char *name, const char *dir, Dict *globals);

/* Legacy recursive-descent grammar (expr_compiler.c). */
void      expr(Parser *p);
void      assign_stmt(Parser *p);
void      from_import_stmt(Parser *p);
void      legacy_statement(Parser *p);
int       is_assignment(Parser *p);
Function *compile_function_from_parser(Parser *p, const char *name, char **params, int arity, int until_dedent, int store_globals);

#endif /* MPY_COMPILER_H */
