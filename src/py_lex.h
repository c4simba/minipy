#ifndef MPY_PY_LEX_H
#define MPY_PY_LEX_H

#include "py_ast.h"

/* Tokens of full Python (py_lex.c), for py_parse.c. */
typedef enum { PT_END, PT_NAME, PT_NUMBER, PT_STRING, PT_NEWLINE, PT_INDENT, PT_DEDENT, PT_OP } PyTokKind;

/* operators and delimiters */
typedef enum {
    O_NONE,
    O_LPAR, O_RPAR, O_LSQB, O_RSQB, O_LBRACE, O_RBRACE, O_COLON, O_COMMA, O_SEMI, O_PLUS, O_MINUS, O_STAR,
    O_SLASH, O_VBAR, O_AMPER, O_LESS, O_GREATER, O_EQUAL, O_DOT, O_PERCENT, O_EQEQUAL, O_NOTEQUAL,
    O_LESSEQUAL, O_GREATEREQUAL, O_TILDE, O_CIRCUMFLEX, O_LEFTSHIFT, O_RIGHTSHIFT, O_DOUBLESTAR,
    O_PLUSEQUAL, O_MINEQUAL, O_STAREQUAL, O_SLASHEQUAL, O_PERCENTEQUAL, O_AMPEREQUAL, O_VBAREQUAL,
    O_CIRCUMFLEXEQUAL, O_LEFTSHIFTEQUAL, O_RIGHTSHIFTEQUAL, O_DOUBLESTAREQUAL, O_DOUBLESLASH,
    O_DOUBLESLASHEQUAL, O_AT, O_ATEQUAL, O_RARROW, O_ELLIPSIS, O_COLONEQUAL, O_EXCLAMATION
} PyOpTok;

/* keywords (hard ones, then the soft ones the parser checks by context) */
typedef enum {
    KW_NONE, KW_False, KW_None, KW_True, KW_and, KW_as, KW_assert, KW_async, KW_await, KW_break, KW_class,
    KW_continue, KW_def, KW_del, KW_elif, KW_else, KW_except, KW_finally, KW_for, KW_from, KW_global,
    KW_if, KW_import, KW_in, KW_is, KW_lambda, KW_nonlocal, KW_not, KW_or, KW_pass, KW_raise, KW_return,
    KW_try, KW_while, KW_with, KW_yield,
    KW_SOFT,                  /* below: soft keywords, also plain names */
    KW_match, KW_case, KW_type, KW_underscore
} PyKw;

typedef struct {
    PyTokKind kind;
    int start, end;           /* byte offsets in the source */
    int line, col, end_line, end_col;
    int op;                   /* PT_OP: PyOpTok; PT_NAME: PyKw */
} PyTok;

typedef struct {
    const char *src; int len;
    int *line_starts; int nlines;
    PyTok *v; int n, cap;
    char *error; int error_line, error_col;
} PyLexer;

/* Tokenize src[start..len): mode 0 a whole module (NEWLINE / INDENT / DEDENT);
   mode 1 a replacement field of an f-string: stops before a '}', '!', ':' or
   '=' outside brackets, *stop is where. Returns 0, or 1 with lx->error. */
int  py_lex(PyLexer *lx, const char *src, int len, int start, int mode, int *stop);
void py_lex_lines(PyLexer *lx, const char *src, int len);
void py_pos(PyLexer *lx, int off, int *line, int *col);
/* end of a string literal starting at its prefix: the offset after the closing quote, or -1 */
int  py_scan_string(const char *src, int len, int pos, char **error);

#endif /* MPY_PY_LEX_H */
