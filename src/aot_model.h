#ifndef MPY_AOT_MODEL_H
#define MPY_AOT_MODEL_H

#include "aot.h"

/* ========================= Typed compiler: program model =========================
   Shared by the type checker (aot_types.c) and the code generator
   (aot_codegen.c). Every variable, parameter, return value, field and
   container element has exactly one static type; types the source does not
   spell out are inferred by unification. */

/* ---- types ---- */
typedef enum {
    TY_VAR,                 /* not known yet (bound later through `link`) */
    TY_VOID,                /* result of a function that returns nothing */
    TY_INT, TY_BOOL, TY_FLOAT, TY_STR,
    TY_LIST, TY_DICT, TY_SET,  /* elem: element / value type; a dict's keys: key (int, str or tuple) */
    TY_OBJ,                 /* instance of cls */
    TY_BUF,                 /* sys.buffer: raw mutable bytes */
    TY_TASK,                /* asyncio task; elem: its result type */
    TY_TUPLE,               /* fixed-size tuple; elems[0..nelems) */
    TY_FILE,                /* an open file (open()) */
    TY_FUNC,                /* a function value (closure): parameters elems[0..nelems), result elem */
    TY_GEN                  /* a generator; elem: the type of the values it yields */
} TyKind;

typedef struct AClass AClass;
typedef struct Ty Ty;
struct Ty { TyKind k; Ty *elem; AClass *cls; Ty *link; int id; Ty **elems; int nelems; Ty *key;
            int tup;        /* TY_LIST: tuple[T, ...] (a variable-length tuple: *args) */
            char **names;   /* TY_TUPLE: a record - a dict literal with fixed str keys and values of different types */ };
Ty  *ty_tuple(Ty **elems, int n);
Ty  *ty_dict(Ty *key, Ty *val);                /* dict[key, val] */
Ty  *ty_dkey(Ty *dict);                        /* its key type (str unless given) */
Ty  *ty_func(Ty **params, int n, Ty *ret);     /* Callable[[params], ret] */

Ty  *ty_new(TyKind k, Ty *elem, AClass *cls);
Ty  *ty_var(void);
Ty  *ty_find(Ty *t);                     /* follow links */
int  ty_known(Ty *t);                    /* fully resolved, element types included */
int  ty_is_ptr(Ty *t);                   /* heap object (reference counted) */
int  ty_size(Ty *t);                     /* bytes in a slot: 8 for float, else 4 */
int  ty_same(Ty *a, Ty *b);              /* structural equality of resolved types */
const char *ty_name(Ty *t);              /* for messages and labels */
extern Ty *TY_INT_T, *TY_BOOL_T, *TY_FLOAT_T, *TY_STR_T, *TY_VOID_T, *TY_BUF_T;

/* ---- program entities ---- */
typedef struct AModule AModule;

typedef struct AFunc AFunc;
typedef struct AVar AVar;
struct AVar {
    char *name;
    Ty *ty;
    int global;             /* module-level variable (static storage) */
    int id;                 /* global: label G<id>; local: index in the frame */
    int offset;             /* local: ebp-relative offset (set by the code generator) */
    AModule *mod;
    AFunc *owner;           /* the function whose frame (or closure) holds it; NULL for globals */
    /* closures: a local of an enclosing function used by an inner function
       (lambda, nested def, generator expression) */
    int captured;           /* some inner function uses it */
    int cell;               /* kept in a heap cell shared with the closures (assigned after they capture it) */
    int nonlocal_set;       /* an inner function assigns it (nonlocal) */
    AVar *src;              /* capture: the variable of the enclosing function it is copied from (NULL: a real local) */
    int capoff;             /* capture: byte offset in the closure object */
    int nbind;              /* binding statements (assignments, loops, def, ...) */
    int bind_top;           /* the top-level statement index of its only binding (-1: nested or several) */
    int first_use_top;      /* the first top-level statement where a closure captures it */
    AFunc *fn_const;        /* bound only by `def name(...)` of this nested function */
    int borrowed;           /* code generator: a loop variable holding the item without a reference of its own */
    int consumed;           /* code generator: a parameter the function keeps (stores in a field...): callers hand it over owned */
};

typedef enum { AS_VAR, AS_FUNC, AS_CLASS, AS_MODULE, AS_SYS /* built-in module, p = its name */,
               AS_CLIB /* ctypes.CDLL(...): p = ACLib */, AS_CFUNC /* a function of one: p = ACFunc */ } SymKind;

/* ctypes: a shared library (lib = ctypes.CDLL("libc.so.6") at module level)
   and the functions called through it. A function's restype / argtypes are
   module-level declarations (lib.f.restype = ctypes.c_char_p), so they are
   known before anything is checked. Calls are cdecl, through the dynamic
   linker (Linux: ld-linux.so.2). */
typedef enum { CT_DEFAULT, CT_INT, CT_UINT, CT_SHORT, CT_USHORT, CT_BYTE, CT_UBYTE, CT_BOOL,
               CT_LONGLONG, CT_ULONGLONG, CT_DOUBLE, CT_FLOAT, CT_CHARP, CT_VOIDP, CT_VOID,
               CT_LONG, CT_ULONG /* 32 bits on i386, 64 on macos */ } CType;
typedef struct ACLib { char *soname; int id; int used; } ACLib;
typedef struct ACFunc {
    ACLib *lib; char *sym; int id, used;
    CType restype;                      /* CT_DEFAULT: c_int */
    CType argtypes[16]; int nargtypes;  /* nargtypes -1: no argtypes (each argument by its type) */
} ACFunc;
typedef struct { char *name; SymKind kind; void *p; } ASym;
typedef struct { ASym *v; int n, cap; } SymTab;
ASym *symtab_find(SymTab *t, const char *name);
ASym *symtab_add(SymTab *t, const char *name, SymKind kind, void *p);

typedef struct AField { char *name; Ty *ty; int offset; Expr *init; AModule *mod; } AField;

struct AFunc {
    char *name;
    int id;                 /* code label F<id> */
    AModule *mod;
    AClass *cls;            /* method of cls, or NULL */
    Stmt *def;              /* NULL for a module body */
    Stmt **body; int nbody;
    int nparams;
    AVar **params;          /* params[i] is also locals.v[i] */
    Expr **defaults;        /* per parameter, NULL if none */
    Ty *ret;                /* TY_VOID when nothing is returned */
    int returns_value;      /* some `return expr` exists */
    SymTab locals;          /* name -> AS_VAR */
    AVar **vars; int nvars, vcap;   /* every local (parameters first) */
    char **globals_decl; int nglobals_decl;
    int is_static;          /* @staticmethod */
    int is_async;           /* async def: runs on a task's stack; await = a plain call */
    int calls_coroutines;   /* may call async functions without await (an endpoint adapter: it runs on a task) */
    struct AFunc *ep_adapter; /* minipy.Endpoint: the adapter made for this function (dict of str -> JSON) */
    int is_property;        /* @property: obj.name calls it */
    int ncalls;             /* call sites seen by the checker */
    int unused;             /* a module function nothing calls whose types are unknown: not compiled */
    int star, dstar;        /* parameter index of *args / **kwargs, else -1 */
    int ndeco;              /* decorators (other than @staticmethod/@property/@wraps): name = d1(d2(function)) */
    AVar *decovar;          /* decorated method: the variable holding Class.name (calls go through it) */
    int kwonly;             /* first keyword-only parameter (nparams if none) */
    AFunc *outer;           /* nested function (def inside a function, lambda, generator expression) */
    Expr *lam;              /* lambda: its expression (the body is lam->a) */
    Expr *genexp;           /* generator expression (comprehension 'G'); parameter 0 is its first iterable */
    int is_gen;             /* generator function: calling it makes a generator */
    Ty *yield_ty;           /* generator: what it yields */
    AVar **caps; int ncaps, capcap;   /* captured variables (closure fields) */
    int capsize;            /* bytes of captured values */
    AVar *selfvar;          /* nested def: the enclosing function's variable bound to it */
    int top_index;          /* checker: the top-level statement being checked */
    int value_used;         /* referenced as a value (needs a closure object) */
    Ty *fty;                /* its type as a value: Callable[[params], ret] */
    ASym **capsym;          /* name-table entries of the captured variables */
    ASym *selfsym;          /* nested def: its own name inside itself */
    Stmt *pristine;         /* module function: an unparsed copy of its def (instances of generic decorators) */
    int ninst;              /* instances made of it */
    int vslot;              /* method: vtable slot */
    int overridden;         /* method: some subclass overrides it (virtual call needed) */
    int used;
    int line;
};

struct AClass {
    char *name;
    int id;                 /* labels VT<id>, DT<id> */
    AModule *mod;
    AClass *base;
    Stmt *def;
    AField **fields; int nfields, fcap;   /* own fields; layout includes the base's */
    int size;               /* instance size in bytes (header included) */
    AFunc **methods; int nmethods, mcap; /* own methods */
    AFunc **vt; int nvt;    /* vtable: inherited + own */
    int used;
    int builtin;            /* a built-in exception class (no source; def is NULL) */
};
int aot_is_exception(AClass *c);              /* derives from BaseException */

struct AModule {
    AotUnit *unit;
    char *name;
    int index;
    SymTab syms;            /* module namespace */
    AFunc *body;
    int used;
};

/* Per-expression information computed by the checker for the code generator. */
typedef enum {
    X_NONE,
    X_VAR,          /* name -> var */
    X_FUNC,         /* call of a module-level function */
    X_CTOR,         /* class instantiation */
    X_METHOD,       /* obj.method(...) */
    X_SUPER,        /* super().method(...) */
    X_STATIC,       /* Class.method(...) (unbound call) */
    X_FIELD,        /* obj.field */
    X_BUILTIN,      /* builtin function: len, str, ... */
    X_TMETHOD,      /* method of a builtin type: list.append, str.split, ... */
    X_SYSCALL,      /* sys.syscall(...) */
    X_SYS,          /* sys.buffer/poke/... */
    X_MODATTR,      /* module.name (resolved to the target's info) */
    X_ASYNC,        /* asyncio.run/create_task (call), await forms (name: call/task/sleep/gather) */
    X_BMOD,         /* call of a function of a built-in module (math, time, random): name */
    X_OPMETHOD,     /* operator implemented by a method (fn): a + b, a[i], -a, x in a */
    X_PROP,         /* obj.name of a @property (fn) */
    X_CLASSCONST,   /* Class.NAME: the constant default of a class field (field) */
    X_FUNCREF,      /* a function as a value: fn (module function, nested def, lambda, generator expression) */
    X_CALLVAL,      /* call of a function value (the callee expression has a TY_FUNC type) */
    X_CALLNEST,     /* call of a nested function by name: fn, var = the variable holding its closure (NULL: itself) */
    X_YIELDFROM,    /* (statement) yield from: iterate the expression */
    X_CALLDECO,     /* obj.m(args) of a decorated method: var(obj, args) */
    X_BOUND,        /* obj.m as a value: a closure of the object calling method fn */
    X_CONST_STR,    /* compile-time string (sys.platform) */
    X_CCALL         /* call of a C function through ctypes: cfn, argmap[i] = the CType of argument i */
} XKind;

typedef struct XInfo {
    Ty *ty;
    XKind kind;
    AVar *var;
    AFunc *fn;
    AClass *cls;
    AField *field;
    const char *name;       /* builtin / type method name */
    int argmap[16];         /* call: parameter index -> argument index (-1: default) */
    AVar **cvars;           /* comprehension: variables of clause i at [2*i], [2*i+1] */
    Expr *key, *rev;        /* sorted/sort/min/max: key= and reverse= (lambda: its parameter in xinfo(key)->var) */
    AFunc **cmpfn;          /* comparison: the method for items[i] (__eq__, __lt__, __contains__ ...) or NULL */
    int cmpneg;             /* a != b done as not a.__eq__(b) (bit i) */
    int *xargs; int nxargs; /* call: arguments collected by *args (positional) */
    int *kwargs; int nkwargs; /* call: keyword arguments collected by **kwargs */
    int splat, dsplat;      /* call: index+1 of the f(*xs) / f(**d) argument feeding *args / **kwargs, else 0 */
    int argelem[16];        /* call: parameter i comes from element argelem[i]-1 of the f(*tuple) argument argmap[i] (0: the argument itself) */
    int emptysplat;         /* call: index+1 of an f(*list) argument with no parameter left (it must be empty) */
    struct ACFunc *cfn;     /* X_CCALL */
} XInfo;

XInfo *xinfo(Expr *e);

/* ---- runtime routines (aot_rtlib.c over aot_rtlib.asm) ---- */
typedef struct AotRt AotRt;
AotRt *aot_rt_new(AotTarget target);
void   aot_rt_use(AotRt *rt, const char *name);          /* pull a routine and its dependencies */
int    aot_rt_used(AotRt *rt, const char *name);
void   aot_rt_emit(AotRt *rt, int kind /* 0 code, 1 data, 2 bss */, void (*put)(void *ctx, const char *s, size_t n), void *ctx);

/* ---- the whole program ---- */
typedef struct AProg {
    AotTarget target;
    AModule **mods; int nmods;
    AFunc **funcs; int nfuncs, fcap;
    AClass **classes; int nclasses, ccap;
    AVar **globals; int nglobals, gcap;
    ACLib **clibs; int nclibs;           /* ctypes libraries and functions */
    ACFunc **cfuncs; int ncfuncs;
} AProg;

/* aot_types.c: build the model and infer/check every type. Returns NULL after
   printing "file:line: error: ..." diagnostics. */
AProg *aot_check(AotUnit **units, int nunits, AotTarget target);

/* aot_codegen.c: emit the fasm listing of a checked program. */
int aot_generate(AProg *p, const AotCodegenOptions *opt, char **out, size_t *outlen);

/* ---- statements parsed by the checker, cached in Stmt.aux (aot_types.c) ----
   The checker and the code generator must see the same Expr nodes: the
   types are attached to them. */
typedef struct AAssign {
    AFunc *opfn;                        /* augmented operator implemented by a method (__add__ ...) */
    Expr *target[16]; int ntarget;      /* a = b = value; a tuple target is an EXPR_TUPLE */
    Expr *value;                        /* NULL for a bare annotation `x: T` */
    TokKind aug;                        /* augmented operator token, or 0 */
    int ann_start, ann_end;             /* annotation tokens [start,end), or -1 */
    Ty *annot;                          /* annotation type (filled in by the checker) */
} AAssign;
typedef struct AWith { Expr *e[8]; char *as[8]; int n; } AWith;
typedef struct ADel { Expr *t[16]; int n; } ADel;
typedef struct APrint { Expr *args[32]; char star[32]; int n; Expr *sep, *end; } APrint;

Expr    *aot_expr(AotUnit *u, Expr **slot);          /* parse a token-range expression in place */
AAssign *aot_assign(AotUnit *u, Stmt *s);
AWith   *aot_with(AotUnit *u, Stmt *s);
ADel    *aot_del(AotUnit *u, Stmt *s);
APrint  *aot_print(AotUnit *u, Stmt *s);             /* NULL unless s is a print statement */
int      aot_is_annotation_only(AotUnit *u, Stmt *s);
AField  *aot_find_field(AClass *c, const char *name);
AFunc   *aot_find_method(AClass *c, const char *name);
int      aot_subclass(AClass *c, AClass *base);

#endif /* MPY_AOT_MODEL_H */
