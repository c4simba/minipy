#ifndef MPY_CAPI_H
#define MPY_CAPI_H

#include "py_ast.h"

/* ========================= The cpython target =========================
   minipy --compile --target cpython app.py: Python modules (full Python 3.14,
   py_parse.c) become C over the CPython API (capi_codegen.c), linked with the
   libpython of a given Python (--python) into a native executable whose
   imports of other modules - the standard library, site-packages, .so
   extensions - go through that Python as usual.

   capi_sym.c: scopes and the meaning of every name (CPython's symtable). */

typedef enum { SC_MODULE, SC_CLASS, SC_FUNCTION, SC_ANNOTATION } CScopeKind;
/* what a scope does with a name */
enum { DF_LOCAL=1, DF_PARAM=2, DF_GLOBAL=4, DF_NONLOCAL=8, DF_USE=16, DF_IMPORT=32, DF_FREE_PASS=64, DF_COMP_ITER=128 };
/* where the name lives */
typedef enum { R_NONE, R_LOCAL, R_GLOBAL_EXPLICIT, R_GLOBAL_IMPLICIT, R_FREE, R_CELL } CRes;

typedef struct { char *name; int flags; CRes res; } CSym;
typedef struct CScope CScope;
struct CScope {
    CScopeKind kind;
    char *name;                 /* function / class name (mangled), "<lambda>", "<genexpr>", "<listcomp>", ... */
    char *qualname;
    PyNode *node;               /* Module, FunctionDef, AsyncFunctionDef, Lambda, ClassDef, *Comp, GeneratorExp */
    CScope *parent;
    CSym *syms; int nsyms, capsyms;
    CScope **kids; int nkids, capkids;
    const char *private_name;   /* the class whose private names (__x) are mangled here, or NULL */
    int is_generator, is_coroutine, is_comprehension, is_genexp, is_lambda;
    int has_await;
    int needs_class_cell;       /* class: a method uses __class__ or super */
    int dynamic_locals;         /* locals() / vars() / eval / exec / dir(): compiled as bytecode */
    int island;                 /* compiled to bytecode by CPython instead (see island_reason) */
    const char *island_reason;
    int id;                     /* number of the code unit in its module */
    char **freevars; int nfree; /* ordered */
    char **cellvars; int ncell;
};

typedef struct {
    CScope *top;                /* the module */
    CScope **all; int nall;     /* every scope, in source order (ids) */
    char *error; int error_line;
} CSymtable;

int     capi_symtable(CSymtable *st, PyNode *mod, const char *modname);
CScope *capi_scope_of(CSymtable *st, PyNode *n);          /* the scope a def / class / lambda / comprehension opens */
CSym   *capi_lookup(CScope *s, const char *name);          /* in this scope (name already mangled) */
char   *capi_mangle(const char *private_name, const char *name);   /* __x in class C -> _C__x */
CRes    capi_resolve(CScope *s, const char *name);         /* name mangled; R_GLOBAL_IMPLICIT when unknown */

/* ---- what CPython compiles from the same source (capi_helper.py's .meta) */
typedef struct {
    char *qualname, *name; int line; int flags;
    char **cellvars; int ncell;
    char **freevars; int nfree;
    int parent;                 /* index of the code object it is a constant of (-1: the module) */
} CpyCode;
typedef struct { int line, col; char *doc; } CpyDoc;
typedef struct { int line; char *name, *text; } CpyAnn;
typedef struct {
    CpyCode *codes; int ncodes;
    CpyDoc *docs; int ndocs;
    CpyAnn *anns; int nanns;
} CpyMeta;
int capi_read_meta(const char *path, CpyMeta *m);

/* ---- capi_codegen.c: one module -> C (and the Python text of its trampolines) */
typedef struct {
    const char *modname;        /* dotted; "__main__" for the script */
    const char *path;           /* the source file */
    const char *src; size_t len;
    int index;                  /* the module's number in the program (C names) */
    CpyMeta *meta;
    int fast_calls;             /* compiled functions called without their trampoline frame (sys._getframe depths change) */
} CapiIn;
typedef struct {
    char *c; size_t clen;       /* the C file (code and trampoline blobs are #included: m<i>_code.inc, m<i>_stub.inc) */
    char *stub; size_t stublen; /* trampolines, Python */
    char *error;                /* not compiled (and why): the module is imported from source */
    int nunits, nislands;       /* statistics */
} CapiOut;
int capi_codegen(CapiIn *in, CapiOut *out);

#endif /* MPY_CAPI_H */
