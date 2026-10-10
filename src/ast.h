#ifndef MPY_AST_H
#define MPY_AST_H

#include "tokens.h"

/* ========================= Frontend tree =========================
   What the compilers work on: the interpreter's bytecode compiler
   (compiler.c) and the typed compiler (aot_types.c, aot_codegen.c).
   py_front.c makes it from the full Python parser's tree (py_ast.h). */

typedef enum {
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
    EXPR_COMPREHENSION, /* comp_kind 'L'/'S'/'D'/'G' (generator expression), a = element/key, b = dict value, clauses */
    EXPR_LAMBDA,        /* eparams, a = body */
    EXPR_AWAIT,         /* await a */
    EXPR_WALRUS,        /* name := a (b: the name as an EXPR_NAME target) */
    EXPR_YIELD,         /* (yield a) / (yield from a): akind 7; its value: what send() gives */
    EXPR_PATTERN        /* a match statement's pattern: akind PAT_* (below) */
} ExprKind;
/* EXPR_PATTERN kinds: PAT_VALUE a == subject; PAT_SINGLETON a (None / True / False) is subject;
   PAT_AS a (sub-pattern or NULL) bound to name (NULL: `_`); PAT_OR items; PAT_SEQ items (a PAT_STAR:
   name or NULL); PAT_MAP keys items, patterns vals, name: **rest; PAT_CLASS a (the class), items
   positional patterns, then keyword ones (their kw: the attribute) */
enum { PAT_VALUE=1, PAT_SINGLETON, PAT_AS, PAT_OR, PAT_SEQ, PAT_STAR, PAT_MAP, PAT_CLASS };

/* Comparison codes stored in EXPR_COMPARE operands (items[i>=1]->akind). */
typedef enum { CMP_LT, CMP_LE, CMP_GT, CMP_GE, CMP_EQ, CMP_NE, CMP_IN, CMP_NOTIN, CMP_IS, CMP_ISNOT } CmpCode;

typedef struct Expr Expr;
typedef struct CompClause { char **vars; int nvars; Expr *iter; Expr **conds; int ncond; Expr *target, *target2; int is_async; } CompClause;   /* target(2): unpacked from vars[0] ([1]) (nested / starred / many names) */
struct Expr {
    ExprKind kind;
    char *name;              /* NAME / attribute name */
    char *kw;                /* call argument: its keyword (akind 3), e.g. f(x=...) */
    int line;
    Tok *tok;                /* EXPR_LITERAL: the literal (a token's fields) */
    TokKind op;              /* operator (unary/binary/bool) */
    int akind;               /* call-arg kind (0 pos,1 *,2 **,3 kw) / compare code */
    Expr *a, *b, *c, *d;     /* operands */
    Expr **items; int count, cap;    /* sequence elems / call args / dict keys / compare operands */
    Expr **vals;  int vcount, vcap;  /* dict values (parallel to items) */
    char **eparams; int neparam;     /* lambda parameters (as a def's: positional, *args, keyword-only, **kwargs) */
    Expr **edefaults;                /* lambda: per parameter, its default or NULL */
    int estar, edstar, ekwonly;      /* lambda: index of *args / **kwargs (-1: none), first keyword-only */
    int comp_kind;                   /* comprehension accumulator kind */
    CompClause *clauses; int nclause, ccap;
    void *ty;                        /* static type, filled in by the compiler (aot_types.c) */
    int cm_done;                     /* a call of a @contextmanager function already wrapped (aot_types.c) */
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
    STMT_MATCH,         /* match expr: body = its cases (STMT_CASE: expr the pattern, expr2 the guard, body) */
    STMT_CASE,
    STMT_UNSUPPORTED
} StmtKind;

typedef struct Stmt Stmt;
struct Stmt {
    StmtKind kind;
    char *name;
    char *name2;
    int line;
    Expr *expr;
    Expr *expr2;
    char **params; int param_count, param_cap;
    Expr **annotations; int annotation_cap;   /* def: per-parameter type annotation or NULL */
    Expr *returns;                            /* def: `-> type` annotation or NULL */
    char **decorators; int decorator_count, decorator_cap;   /* each decorator's first name ... */
    Expr **decorator_exprs;                                   /* ... and its whole expression */
    Stmt **body; int body_count, body_cap;
    Stmt **orelse; int orelse_count, orelse_cap;
    int star_index, dstar_index;   /* def params: index of *args / **kwargs, else -1 */
    int kwonly_index;              /* def params: first keyword-only parameter after a bare `*`, else -1 */
    int block_tag;                 /* try-clause blocks: 0 normal, 1 except, 2 else, 3 finally;
                                      import: 1 `as`; yield: 7 `yield from` */
    int is_async;                  /* async def */
    void *aux;                     /* the typed compiler's view of it (aot_types.c) */
    Expr **targets; int ntargets;  /* assignment: its targets (a = b = v: two); del: what it deletes; with: the items */
    Expr *value;                   /* assignment: the value (NULL: a bare annotation) */
    int aug;                       /* augmented assignment: its operator's token (T_PLUS_ASSIGN ...), else 0 */
    Expr *ann;                     /* annotated assignment: the annotation */
    int ann_only;                  /* an STMT_EXPR `name: type` (no value) */
    char **withas;                 /* with: per item, its `as` name or NULL */
    Expr **withtgt;                /* with: per item, its `as` target when not a name (a tuple, an attribute) */
    char *module;                  /* import / from-import: the dotted module name */
    char **names, **asnames; int nnames;   /* from-import: the names ("*") and their aliases (NULL: none) */
    Expr **pdefaults;              /* def: per parameter, its default value or NULL */
    void *py;                      /* the PyNode it was made from (copies are made again from it) */
    void *src_mod;                 /* (aot_types.c) a mixin's member copied into a class of another module: the mixin's */
    int packargs;                  /* (aot_types.c) index + 1 of a *args: sys._PackArgs made a plain parameter */
    char **tparams; int ntparams;  /* def / class / type alias: its type parameters ([T, U]) */
    int is_alias;                  /* `type name = ann` (an STMT_PASS): a type alias */
    int yield_expr;                /* def: a yield in an expression (x = yield v): a generator */
    int level;                     /* from-import: its dots (from .. import x: 2); module NULL for `from . import x` */
    int star;                      /* try: its handlers are except* ones */
};

typedef Stmt Ast;

/* Growable name-list helper (dedups). */
void name_add_unique(char ***arr, int *cnt, int *cap, const char *name);

/* tree constructors */
Stmt    *stmt_new(StmtKind k, const char *name, int line);
void     stmt_add_body(Stmt *s, Stmt *child);
void     stmt_add_orelse(Stmt *s, Stmt *child);
void     stmt_set_annotation(Stmt *s, int param, Expr *e);
void     stmt_add_decorator(Stmt *s, const char *name, Expr *e);

const char *stmt_kind_name(StmtKind k);   /* for messages */

#endif /* MPY_AST_H */
