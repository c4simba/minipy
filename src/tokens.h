#ifndef MPY_TOKENS_H
#define MPY_TOKENS_H

#include "util.h"

/* ========================= Operator and literal kinds =========================
   The frontend tree (ast.h) names its operators by these kinds (EXPR_BINARY
   op T_PLUS, augmented assignment T_PLUS_ASSIGN, ...) and keeps a literal's
   value in a Tok. */

typedef enum {
    T_EOF,T_NEWLINE,T_INDENT,T_DEDENT,T_NUMBER,T_STRING,T_NAME,
    T_IF,T_ELIF,T_ELSE,T_WHILE,T_FOR,T_IN,T_DEF,T_RETURN,T_PRINT,T_CLASS,T_IMPORT,T_FROM,T_AS,T_TRUE,T_FALSE,T_NONE,T_AND,T_OR,T_NOT,T_IS,T_BREAK,T_CONTINUE,T_PASS,T_RAISE,T_WITH,T_TRY,T_EXCEPT,T_FINALLY,T_GLOBAL,T_NONLOCAL,T_DEL,T_LAMBDA,T_YIELD,T_ASYNC,T_AWAIT,T_MATCH,T_CASE,T_ASSERT,
    T_LP,T_RP,T_LB,T_RB,T_LC,T_RC,T_COLON,T_COMMA,T_DOT,T_AT,T_ASSIGN,
    T_PLUS,T_MINUS,T_STAR,T_SLASH,T_POWER,T_FLOOR_DIV,T_PLUS_ASSIGN,T_MINUS_ASSIGN,T_STAR_ASSIGN,T_SLASH_ASSIGN,T_EQ,T_NE,T_LT,T_LE,T_GT,T_GE,
    T_PERCENT,T_AMP,T_PIPE,T_CARET,T_TILDE,T_SHL,T_SHR,
    T_PERCENT_ASSIGN,T_FLOOR_DIV_ASSIGN,T_POWER_ASSIGN,T_AMP_ASSIGN,T_PIPE_ASSIGN,T_CARET_ASSIGN,T_SHL_ASSIGN,T_SHR_ASSIGN,T_AT_ASSIGN
} TokKind;

/* a literal: T_NUMBER (i, or f when is_float; text as written) or T_STRING (text, UTF-8;
   an f-string's format piece: "%<printf spec>", a NUL, "%<Python spec>", with i=1; bytes: i=2) */
typedef struct { TokKind kind; char *text; int64_t len; int64_t i; double f; int is_float; int line; } Tok;   /* len: text's bytes (a str may hold NULs) */

#endif /* MPY_TOKENS_H */
