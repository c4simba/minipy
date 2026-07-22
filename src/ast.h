#ifndef MPY_AST_H
#define MPY_AST_H

#include "lexer.h"

/* ========================= Frontend AST + Symbol Table types =========================
   The typed frontend tree. Expressions are stored as token-index ranges
   (EXPR_TOKEN_RANGE); the bytecode compiler re-parses those ranges later. */

typedef enum {
    EXPR_TOKEN_RANGE,   /* frontend: unparsed span of tokens (start..end) */
    EXPR_LITERAL,       /* number / string, in `tok` */
    EXPR_TRUE, EXPR_FALSE, EXPR_NONE,
    EXPR_NAME,          /* `name` */
    EXPR_UNARY,         /* op, a */
    EXPR_BINARY,        /* op, a, b (arith/bitwise/shift) */
    EXPR_BOOL,          /* op = T_AND/T_OR, a, b */
    EXPR_COMPARE,       /* items = operands; items[i>=1]->akind = comparison code */
    EXPR_TERNARY,       /* a = cond, b = then, c = else */
    EXPR_CALL,          /* a = callee; items = args; arg->akind selects pos/star/dstar/kw */
    EXPR_ATTRIBUTE,     /* a = obj, name */
    EXPR_INDEX,         /* a = obj, b = index */
    EXPR_SLICE,         /* a = obj, b = lo, c = hi, d = step (NULL = missing) */
    EXPR_LIST, EXPR_TUPLE, EXPR_SET,   /* items */
    EXPR_DICT,          /* items = keys, vals = values */
    EXPR_COMPREHENSION, /* comp_kind 'L'/'S'/'D', a = element/key, b = dict value, clauses */
    EXPR_LAMBDA         /* eparams, a = body */
} ExprKind;

typedef struct Expr Expr;
typedef struct CompClause { char **vars; int nvars; Expr *iter; Expr **conds; int ncond; } CompClause;
struct Expr {
    ExprKind kind;
    char *name;              /* NAME / attribute / keyword-arg name */
    int line;
    int start, end;          /* EXPR_TOKEN_RANGE */
    Tok *tok;                /* EXPR_LITERAL token */
    TokKind op;              /* operator (unary/binary/bool) */
    int akind;               /* call-arg kind (0 pos,1 *,2 **,3 kw) / compare code */
    Expr *a, *b, *c, *d;     /* operands */
    Expr **items; int count, cap;    /* sequence elems / call args / dict keys / compare operands */
    Expr **vals;  int vcount, vcap;  /* dict values (parallel to items) */
    char **eparams; int neparam;     /* lambda parameters */
    int comp_kind;                   /* comprehension accumulator kind */
    CompClause *clauses; int nclause, ccap;
};

typedef enum {
    STMT_MODULE,
    STMT_BLOCK,
    STMT_IF,
    STMT_WHILE,
    STMT_FOR,
    STMT_FUNCTION_DEF,
    STMT_CLASS_DEF,
    STMT_RETURN,
    STMT_ASSIGN,
    STMT_IMPORT,
    STMT_FROM_IMPORT,
    STMT_RAISE,
    STMT_TRY,
    STMT_WITH,
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_PASS,
    STMT_DEL,
    STMT_GLOBAL,
    STMT_NONLOCAL,
    STMT_EXPR,
    STMT_YIELD,
    STMT_ASSERT,
    STMT_UNSUPPORTED
} StmtKind;

typedef enum { SYM_MODULE, SYM_FUNCTION, SYM_CLASS } SymScopeKind;

typedef struct SymScope SymScope;
struct SymScope {
    SymScopeKind kind;
    char *name;
    int line;
    char **defs; int def_count, def_cap;
    char **uses; int use_count, use_cap;
    char **globals; int global_count, global_cap;
    char **nonlocals; int nonlocal_count, nonlocal_cap;
    SymScope **children; int child_count, child_cap;
};

typedef struct Stmt Stmt;
struct Stmt {
    StmtKind kind;
    char *name;
    char *name2;
    int line;
    int start, end;
    Expr *expr;
    Expr *expr2;
    char **params; int param_count, param_cap;
    Expr **defaults; int default_count, default_cap;
    char **decorators; int decorator_count, decorator_cap;
    Stmt **body; int body_count, body_cap;
    Stmt **orelse; int orelse_count, orelse_cap;
    int star_index, dstar_index;   /* def params: index of *args / **kwargs, else -1 */
    int block_tag;                 /* try-clause blocks: 0 normal, 1 except, 2 else, 3 finally */
    SymScope *scope;
};

typedef Stmt Ast;

typedef struct {
    char **defs; int def_count, def_cap;
    char **uses; int use_count, use_cap;
    SymScope *root_scope;
} SymTable;

/* Growable name-list helper (dedups). */
void name_add_unique(char ***arr, int *cnt, int *cap, const char *name);

/* AST / scope constructors */
Expr    *expr_new_range(int start, int end, int line);
Stmt    *stmt_new(StmtKind k, const char *name, int line, int start);
void     stmt_add_body(Stmt *s, Stmt *child);
void     stmt_add_orelse(Stmt *s, Stmt *child);
void     stmt_add_default(Stmt *s, Expr *e);
void     stmt_add_decorator(Stmt *s, const char *name);
SymScope *scope_new(SymScopeKind k, const char *name, int line);
void     scope_add_child(SymScope *p, SymScope *c);
void     scope_def(SymScope *s, const char *name);
void     scope_use(SymScope *s, const char *name);
void     scope_global(SymScope *s, const char *name);
void     scope_nonlocal(SymScope *s, const char *name);

/* Debug-name helpers */
const char *stmt_kind_name(StmtKind k);
const char *scope_kind_name(SymScopeKind k);
int         is_expr_name_token(TokKind k);
int         is_assign_op(TokKind k);

#endif /* MPY_AST_H */
