#ifndef MPY_FRONTPARSER_H
#define MPY_FRONTPARSER_H

#include "ast.h"

/* Build the statement AST and symbol table from a token stream. */
Ast *build_ast_and_symbols(TokVec *tv, SymTable *st);

/* Diagnostic dumpers used by the CLI (--dump-ast / --dump-symbols). */
void dump_ast_for_source(const char *src);
void dump_symbols_for_source(const char *src);

#endif /* MPY_FRONTPARSER_H */
