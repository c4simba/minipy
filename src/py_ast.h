#ifndef MPY_PY_AST_H
#define MPY_PY_AST_H

#include "util.h"

/* ========================= Full Python 3.14 frontend =========================
   py_lex.c / py_parse.c read Python 3.14 source into the tree below - the
   node kinds and fields of CPython's `ast` module, so that `minipy --pyast`
   (py_dump.c) can be compared with CPython's own parser field by field
   (tests/pyast_check.py). py_front.c turns it into the tree the compilers
   work on (ast.h): the interpreter's bytecode compiler and the typed one. */

typedef enum {
    /* mod */
    PK_Module,
    /* stmt */
    PK_FunctionDef, PK_AsyncFunctionDef, PK_ClassDef, PK_Return, PK_Delete, PK_Assign, PK_TypeAlias,
    PK_AugAssign, PK_AnnAssign, PK_For, PK_AsyncFor, PK_While, PK_If, PK_With, PK_AsyncWith, PK_Match,
    PK_Raise, PK_Try, PK_TryStar, PK_Assert, PK_Import, PK_ImportFrom, PK_Global, PK_Nonlocal, PK_Expr,
    PK_Pass, PK_Break, PK_Continue,
    /* expr */
    PK_BoolOp, PK_NamedExpr, PK_BinOp, PK_UnaryOp, PK_Lambda, PK_IfExp, PK_Dict, PK_Set, PK_ListComp,
    PK_SetComp, PK_DictComp, PK_GeneratorExp, PK_Await, PK_Yield, PK_YieldFrom, PK_Compare, PK_Call,
    PK_FormattedValue, PK_Interpolation, PK_JoinedStr, PK_TemplateStr, PK_Constant, PK_Attribute,
    PK_Subscript, PK_Starred, PK_Name, PK_List, PK_Tuple, PK_Slice,
    /* other nodes */
    PK_comprehension, PK_ExceptHandler, PK_arguments, PK_arg, PK_keyword, PK_alias, PK_withitem,
    PK_match_case, PK_MatchValue, PK_MatchSingleton, PK_MatchSequence, PK_MatchMapping, PK_MatchClass,
    PK_MatchStar, PK_MatchAs, PK_MatchOr, PK_TypeVar, PK_ParamSpec, PK_TypeVarTuple,
    PK_ident,                 /* an identifier in a list (Global names, MatchClass kwd_attrs): id[0] */
    PK__COUNT
} PyKind;

/* operators (BinOp/AugAssign op, UnaryOp op, BoolOp op, Compare ops) */
typedef enum { OP_Add=1, OP_Sub, OP_Mult, OP_MatMult, OP_Div, OP_Mod, OP_Pow, OP_LShift, OP_RShift,
               OP_BitOr, OP_BitXor, OP_BitAnd, OP_FloorDiv,
               OP_Invert, OP_Not, OP_UAdd, OP_USub,
               OP_And, OP_Or,
               OP_Eq, OP_NotEq, OP_Lt, OP_LtE, OP_Gt, OP_GtE, OP_Is, OP_IsNot, OP_In, OP_NotIn } PyOp;
typedef enum { CTX_Load=1, CTX_Store, CTX_Del } PyCtx;

/* constants */
typedef enum { PC_None, PC_True, PC_False, PC_Ellipsis, PC_Int, PC_Float, PC_Complex, PC_Str, PC_Bytes } PyConstKind;
typedef struct {
    PyConstKind kind;
    char *text;               /* PC_Int / PC_Float / PC_Complex: the literal (underscores removed; complex: without j) */
    uint32_t *u; int ulen;    /* PC_Str: code points (lone surrogates allowed) */
    unsigned char *b; int blen;  /* PC_Bytes */
    int is_u;                 /* Constant.kind == 'u' */
} PyConst;

typedef struct PyNode PyNode;
typedef struct { PyNode **v; int n, cap; } PyList;
struct PyNode {
    PyKind kind;
    int line, col, end_line, end_col;
    PyNode *n[4];             /* single children (see the schema in py_dump.c) */
    PyList L[5];              /* lists of children (an element may be NULL: Dict keys, kw_defaults) */
    char *id[2];              /* identifiers, UTF-8 */
    int op;                   /* operator / ctx / conversion / level / is_async / simple */
    PyConst *k;               /* Constant value, MatchSingleton value, Interpolation str */
    int paren;                /* parser: the expression was in parentheses */
    int soff, eoff;           /* statements: source extent (decorators included), byte offsets */
    int aux1, aux2;           /* for the compiler */
};

typedef struct {
    const char *path;         /* for messages */
    const char *src; size_t len;
    PyNode *mod;              /* PK_Module */
    char *error; int error_line, error_col;   /* SyntaxError */
} PyParse;

/* Parse a whole module. Returns 0 on success (p->mod), else 1 with p->error. */
int py_parse(PyParse *p, const char *path, const char *src, size_t len);

/* The tree in the canonical text form tests/pyast_check.py prints for CPython's ast. */
void py_dump(PyNode *n, FILE *out);
/* minipy --pyast file.py */
int py_dump_main(int argc, char **argv);

PyNode *py_node(PyKind k, int line, int col);
void    py_list_add(PyList *l, PyNode *n);
const char *py_kind_name(PyKind k);

#endif /* MPY_PY_AST_H */
