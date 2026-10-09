#ifndef MPY_PY_FRONT_H
#define MPY_PY_FRONT_H

#include "ast.h"
#include "py_ast.h"

/* ========================= Full parser -> compiler frontend =========================
   The compiler (aot_types.c, aot_codegen.c) works on the frontend tree of
   ast.h. py_front() reads a module with the full Python parser (py_parse.c:
   CPython's grammar and tree) and turns that tree into it: statements with
   their parts spelled out (assignment targets, with items, imported names,
   per-parameter annotations and defaults), expressions as Expr trees.
   What compiled programs cannot have is reported at its line. */

/* The module's tree; NULL after printing "path:line: error: ..." to stderr. */
Ast  *py_front(const char *path, const char *src);
/* One expression (a type annotation written as a string); NULL after an error. */
Expr *py_front_expr(const char *path, const char *text, int line);
/* Statements made from source text (methods the compiler writes: @dataclass); in a STMT_BLOCK. */
Stmt *py_front_stmts(const char *path, const char *src, int line);
/* A fresh copy of statement s (its own Expr nodes: an instance of a generic function). */
Stmt *py_front_copy(const char *path, Stmt *s);

#endif /* MPY_PY_FRONT_H */
