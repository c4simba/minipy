/* ========================= Typed compiler: model + type checker =========================
   Builds the program model (modules, functions, classes, variables) from the
   ASTs and gives every variable, parameter, return value, field and container
   element exactly one static type. Types come from annotations
   (`x: int`, `def f(a: str) -> list[int]`, class-body fields) or are inferred
   by unification from the code: `i = 1` makes i an int, `grid = []` followed
   by `grid.append(row)` makes grid a list of row's type. Assigning a value of
   another type is a compile error. `None` makes the type it meets optional
   (Ty.opt): whatever can hold None is Optional[T], and the code generator
   checks for None where Python would fail on it.

   Inference runs whole-program passes until nothing new is learned, then a
   final strict pass reports whatever is still unknown (check.c).

   This header is internal to src/aot/types: the checker's state (Ck) and the
   functions its files share, grouped by the file defining them. */
#ifndef PPY_AOT_TYPES_CHECK_H
#define PPY_AOT_TYPES_CHECK_H

#include "aot/model.h"
#include "pystdlib.h"
#include <stdarg.h>
#include "py_front.h"
#include "tokens.h"

typedef struct Ck {
    AProg *p;
    AModule *mod;           /* module being processed */
    AFunc *fn;              /* function being checked (module bodies included) */
    int changed;            /* some type known before this pass got bound */
    int pass_first;         /* first type id created in this pass */
    int strict;             /* final pass: anything still unknown is an error */
    int defaulting;         /* (passes after nothing changed) arguments of element types nothing decides: int, as variables' */
    unsigned none_lit;      /* (pick_instance) the parameters given a literal None */
    int mk_static;          /* (instantiate) the copy of a static method / classmethod: no self */
    const char *site_callee;   /* (site_args) the function a call site value is for */
    struct { const char *name; ASym sym; AFunc *fn; } cscope[256];   /* comprehension / except / key variables, class attributes in scope (of fn) */
    int ncscope;
    Expr *coro_ok;          /* the coroutine call being awaited / handed to asyncio */
    int ep_value;           /* checking an argument for a plainpy.Endpoint: an async function may be a value */
    int in_except;          /* inside an except clause (bare raise) */
    Ty **seen; int nseen, cseen;     /* strict pass: every expression type (unconstrained elements -> int) */
    jmp_buf fail;
    int reflected;          /* binop_type_m chose the right operand's __r<op>__ */
    AFunc *sig_fn; AClass *sig_cls;          /* the def / class whose annotations are being typed */
    AVar *nnv[64]; int nnn;                  /* variables known not to be None here (if x is not None: ...) */
    const char **alias_n; Ty **alias_t; int nalias;   /* a generic alias's parameters while it is expanded */
    int dc_conv;            /* dataclasses.asdict / astuple are used: dataclasses get __mp_asdict__ / __mp_astuple__ */
    int nhidden;            /* hidden variables made for rewritten calls */
    Expr *discard;          /* an expression statement's expression (its value is not used: json.loads only checks the text) */
    struct { Stmt *b; AFunc *fn; Ty *ty; } hst[64];   /* the except clauses around (sys.exception(): the innermost one's of this function) */
    int nhst;
} Ck;

/* check mode (aot_check_mode): an error in one function, class or module is
   reported, that one is marked as failed and the check goes on:
       CkTry t; ck_try_begin(c,&t);
       if(!setjmp(c->fail)){ ...; ck_try_end(c,&t); } else { ck_try_fail(c,&t); ...failed... } */
typedef struct {
    jmp_buf outer;
    AModule *mod; AFunc *fn; unsigned none_lit; int mk_static; const char *site_callee; int ncscope;
    Expr *coro_ok; int ep_value; int in_except; int reflected; AFunc *sig_fn; AClass *sig_cls; int nnn;
    const char **alias_n; Ty **alias_t; int nalias; Expr *discard; int nhst;
} CkTry;

typedef struct { char **v; int n, cap; } Names;

/* A dataclass's fields (its bases' first) and options. Its methods are written as Python
   source and made like the class's own: __init__ (assigning the fields), __repr__, __eq__,
   the orderings, __hash__; frozen ones raise FrozenInstanceError when a field is assigned. */
typedef struct DcField { const char *name; Expr *ann; const char *defsym; Expr *factory;
    int has_default, init, repr, compare, kw_only, hash, initvar; } DcField;
struct DcInfo { DcField *f; int n, cap; int init, repr, eq, order, frozen, unsafe_hash; Stmt *init_def; };
typedef struct { char *s; size_t n, cap; } Txt;

typedef struct { char *s; size_t n, cap; } SrcBuf;

/* ---- ty.c: types and symbol tables */
extern int ty_ids;
int ty_shared(Ty *t);
Ty *ty_copy(Ty *t);
int has_elem(Ty *t);
int names_eq(Ty *a, Ty *b);

/* ---- syntax.c: syntax made by the checker */
Expr *xnew(ExprKind k, int line);
void xpush(Expr *e, Expr *item);

/* ---- unify.c: checker state, unification */
PPY_NORETURN void ck_err(Ck *c, int line, const char *fmt, ...);
void ck_try_begin(Ck *c, CkTry *t);
void ck_try_end(Ck *c, CkTry *t);
void ck_try_fail(Ck *c, CkTry *t);
void ck_unclear(Ck *c, AFunc *f);
int is_func(AFunc *f);
int is_sys_call(Expr *d, const char *name);
void cs_push(Ck *c, const char *name, AVar *v, int line);
AVar *scope_hidden(Ck *c, const char *name);
void set_opt(Ck *c, Ty *t);
int unify(Ck *c, Ty *a, Ty *b);
extern AClass *complex_class;
int assignable(Ck *c, Ty *dst, Ty *src);
void expect(Ck *c, Ty *dst, Ty *src, int line, const char *what);
int numeric(Ty *t);
int orderable(Ty *t);
int is_var(Ty *t);

/* ---- annot.c: annotations */
ASym *module_sym(AModule *m, const char *name);
extern SymTab builtin_syms;
extern AClass *exc_root;
ASym *global_sym(AModule *m, const char *name);
void make_builtin_exceptions(AProg *p, AModule *m);
AClass *abc_class(Ck *c);
AModule *find_module(AProg *p, const char *name);
Ty *optional(Ty *t);
Ty *endpoint_type(void);
int is_endpoint(Ty *t);
int names_has(char **v, int n, const char *name);
Ty *type_of(Ck *c, Expr *e, int line);
Ty *annotation(Ck *c, Expr *e);

/* ---- model.c: building the model */
void names_bound(Ck *c, Stmt **b, int n, Names *out, Names *globals);
AVar *new_global(Ck *c, AModule *m, const char *name);
AVar *global_var(Ck *c, AModule *m, const char *name, int line);
AVar *hidden_local(AFunc *f, const char *name, Ty *ty);
AVar *new_local(AFunc *f, const char *name, Ty *ty);
void add_func(AProg *p, AFunc *f);
void no_nested_defs(Ck *c, Stmt **b, int n, int in_func);
void classmethod_def(AClass *cls, Stmt *def);
int general_decos(AotUnit *u, Stmt *def, int in_class);
char *hidden_name(const char *a, const char *b);
Expr *deco_app(Ck *c, AotUnit *u, Stmt *def, const char *fnref, int in_class);
AFunc *new_func(Ck *c, AFunc *f, AModule *m, AClass *cls, Stmt *def, AFunc *outer);
AFunc *new_lambda(Ck *c, Expr *e);
const char *fn_type_problem(AFunc *f);
Ty *fn_type(Ck *c, AFunc *f, int line);
int class_assigns_self(AClass *k, const char *name);
void annotated_self_fields(Ck *c, AClass *cls, AotUnit *u, const char *self, Stmt **b, int n);
AField *add_field(AClass *cls, const char *name, Ty *ty, Expr *init);
AClass *new_class(Ck *c, AModule *m, Stmt *def);
AClass *new_inner_class(Ck *c, AModule *m, Stmt *s, AClass *outer, AFunc *encl);
AFunc *next_scope(AFunc *e);
ASym *local_class_in(AFunc *e, const char *name);
ASym *local_class(Ck *c, const char *name);

/* ---- dataclass.c: @dataclass */
void tx(Txt *t, const char *fmt, ...);
Stmt *body_def(Stmt *def, const char *name);
void dc_conv(AClass *cls, Expr *a, const char *x, int asdict, int depth, Txt *t);
void class_decorators(Ck *c, AClass *cls);

/* ---- mro.c: several base classes */
ASym *dotted_rest(Ck *c, ASym *ms, const char *rest);
void several_bases(Ck *c, AClass *cls);

/* ---- enum.c: enum */
int enum_members_expr(Ck *c, Expr *e);
void fill_class(Ck *c, AClass *cls);

/* ---- ctypes.c: ctypes */
ACLib *clib_get(AProg *p, const char *soname);
ACFunc *cfunc_get(AProg *p, ACLib *lib, const char *sym);
void collect_ctypes(Ck *c, AModule *m);

/* ---- decls.c: module declarations */
void fold_platform(Ck *c, Stmt ***arr, int *n, int *cap);
void collect_module(Ck *c, AModule *m);

/* ---- generic.c: generic functions */
Stmt *stmt_clone(Ck *c, AModule *m, Stmt *s);
int has_untyped_params(Stmt *def);
int mentions_name_of(Expr *e, char **names, int n);
int mentions_typevar(AModule *m, Expr *e);
int protocol_params(Stmt *def);
int typevar_params(AModule *m, Stmt *def);
int foreign_typevar_params(AModule *m, Stmt *def, Stmt *cls);
int lambda_decides(AModule *m, Stmt *def, int p, Ty **at, int first);
int method_overridden(Ck *c, AFunc *fn);
void pack_args(Ck *c, Expr *e, AFunc *fn, int first);
AFunc *pick_instance(Ck *c, Expr *e, AFunc *fn, XInfo *xi);
AFunc *instance_for_none(Ck *c, AFunc *fn, Ty **at, Ty **fnarg, unsigned omit, unsigned fnm, unsigned gv, int line);
const char *instantiate(Ck *c, AFunc *fn);

/* ---- imports.c: imports */
void collect_functions(Ck *c, AModule *m);
const char *string_const(const char *n);
void link_imports(Ck *c, AModule *m);

/* ---- names.c: names */
Ty *stop_value(Ck *c, Ty *g, int line);
AVar *var_root(AVar *v);
ASym *lookup(Ck *c, const char *name);
AModule *module_expr(Ck *c, Expr *e);
AClass *class_expr(Ck *c, Expr *e);
const char *bmod(Ck *c, Expr *e);
int is_bmod(Ck *c, Expr *e, const char *name);
int is_sys(Ck *c, Expr *e);
void no_void(Ck *c, Ty *t, int line);
Ty *pending(Ck *c, int line, const char *what);

/* ---- operators.c: operators */
const char *op_method(TokKind op);
Expr *slice_call(Expr *sl, const char *name, int line);
AFunc *op_method_of(Ck *c, Ty *obj, const char *name, Ty *arg, int line);
Ty *binop_type(Ck *c, TokKind op, Ty *a, Ty *b, int line);
Ty *binop_type_m(Ck *c, TokKind op, Ty *a, Ty *b, int line, AFunc **opfn);
int is_none(Expr *e);
void ck_compare_pair_m(Ck *c, int code, Expr *ea, Expr *eb, Ty *a, Ty *b, int line, AFunc **opfn, int *neg, int *swap);

/* ---- iter.c: iteration */
Ty *elem_of(Ck *c, Ty *t, int line, const char *what);
int is_builtin_call(Ck *c, Expr *e, const char *name);
void iterator_protocol(Ck *c, Expr *it);
void iter_types(Ck *c, Expr *it, int n, Ty **out, int line);
void async_iter_ok(Ck *c, Expr *it, int is_async, int line);
Expr *expr_subst(Expr *e, const char *var, Expr *with);
int pure_ref(Expr *e);
Ty *ck_comprehension(Ck *c, Expr *e);
Ty *ck_genexp(Ck *c, Expr *e);

/* ---- literal.c: container literals */
int record_keys(Expr *e);
Ty *record_of(Expr *e, Ty **vts);
int has_splat(Expr *e);
Ty *literal_elem(Ck *c, Expr **items, int n);
Ty *ck_tuple_as_list(Ck *c, Expr *e);
int builtin_fn_value(Ck *c, Expr *e, int arity);
int ann_callable_arity(Expr *a);
Ty *ck_expr_want(Ck *c, Expr *e, Ty *want);

/* ---- narrow.c: narrowing */
int pure_test(Expr *e);
int static_isinstance(Ck *c, Expr *e);
int static_none_param(Ck *c, Expr *e);
Ty *ck_cond(Ck *c, Expr *e);
int int_enum_ty(Ty *t);
Ty *as_enum_value(Ck *c, Expr *x);

/* ---- expr.c: expressions */
Ty *ck_expr_inner(Ck *c, Expr *e);
Ty *ck_expr(Ck *c, Expr *e);

/* ---- args.c: call arguments */
Ty *ck_default(Ck *c, Expr *d);
void ck_raise_cause(Ck *c, Stmt *s);
void ck_def_defaults(Ck *c, AFunc *f, int line);
void int_push(int **v, int *n, int x);
void fspath_arg(Ck *c, Expr *a, Ty *want, Ty **t);
void ck_args(Ck *c, Expr *e, AFunc *fn, int skip, XInfo *xi);

/* ---- endpoint.c: plainpy.Endpoint */
void sb_add(SrcBuf *b, const char *fmt, ...);
void lit_source(Ck *c, Expr *d, SrcBuf *b, int line);
void ep_arg(Ck *c, Ty *pt, Expr *a, int line);

/* ---- ctor.c: function values, constructors */
Ty *ck_callval(Ck *c, Expr *e, Ty *ft);
int class_has_subclass(Ck *c, AClass *cls);
AClass *class_instance(Ck *c, AClass *cls);
Ty *ck_ctor(Ck *c, Expr *e, AClass *cls);
Ty *ck_positional(Ck *c, Expr *e, int min, int max, const char *name);
Ty *arg(Ck *c, Expr *e, int i);

/* ---- iterfuncs.c: functions of iterables */
void take_sort_kwargs(Ck *c, Expr *e, int allow_reverse);
Ty *ck_sort_key(Ck *c, Expr *key, Ty *elem, int line);
void genexp_as_list(Expr *e, int i);
Ty *gen_as_list(Ck *c, Expr *e, int i);
Expr *name_expr(const char *name, int line);
Ty *ck_mapfilter(Ck *c, Expr *e, const char *name);
void make_comprehension(Expr *e, int kind, Expr *elem, Expr *val, char **vars, int nvars, Expr *iter);
Ty *ck_pairs(Ck *c, Expr *e, const char *name);
Ty *ck_iter(Ck *c, Expr *e);
Ty *ck_reduce(Ck *c, Expr *e);
Ty *ck_open(Ck *c, Expr *e);

/* ---- builtin.c: built-in functions */
Ty *dunder_of_arg(Ck *c, Expr *e, const char *const *ms);
Ty *ck_builtin(Ck *c, Expr *e, const char *name);
void kw_to_pos(Ck *c, Expr *e, const char *m, const char *const *names, int n);
void ck_range_args(Ck *c, Expr *e, int first);

/* ---- methods.c: methods of built-in types */
Ty *ck_codec_call(Ck *c, Expr *e, Expr *obj, int decode, int first);
int codecs_module(Ck *c);
Ty *ck_tmethod(Ck *c, Expr *e, Ty *t, const char *m);

/* ---- sysmods.c: built-in modules */
Ty *lexical_exception(Ck *c, Expr *e);
Expr *lit_int(int line, long long v);
Expr *lit_str(int line, const char *v);
Expr *site_value(Ck *c, Expr *d, int line);
Ty *ck_sys(Ck *c, Expr *e, const char *m);
Ty *ck_asyncio(Ck *c, Expr *e, const char *m);
Ty *ck_bmod(Ck *c, Expr *e, const char *mod, const char *m);
Ty *ck_ccall(Ck *c, Expr *e, ACFunc *cf);
Ty *ck_ctypes_fn(Ck *c, Expr *e, const char *m);

/* ---- json.c: json, pickle */
void json_ok(Ck *c, Ty *t, int line, int depth);
Ty *ck_json_loads(Ck *c, Expr *e);
Ty *ck_pickle_loads(Ck *c, Expr *e, AModule *mod, int load);

/* ---- unittest.c: unittest */
const char *ut_deco_name(const char *src, Expr *d);
void ut_generate(Ck *c, int line);

/* ---- stdfuncs.c: library functions */
int unicode_variant(Ck *c, Expr *e, AModule *mod, const char *m);
int lit_variant(Ck *c, Expr *e, AModule *mod, const char *m);
Ty *ck_text_loads(Ck *c, Expr *e, AModule *mod, int load);
Ty *ck_json(Ck *c, Expr *e, const char *m);
Ty *ck_plainpy(Ck *c, Expr *e, const char *m);
Ty *ck_call(Ck *c, Expr *e);
Ty *ck_coro(Ck *c, Expr *x);
AVar *named_hidden(Ck *c, const char *base, char *out, size_t n);
Ty *ck_dataclasses(Ck *c, Expr *e, const char *m);
Ty *ck_thread(Ck *c, Expr *e, const char *m);
Ty *ck_capwords(Ck *c, Expr *e);
Ty *ck_partial(Ck *c, Expr *e);

/* ---- struct.c: struct, tempfile */
const char *struct_func(Ck *c, Expr *f);
const char *tempfile_func(Ck *c, Expr *f);
Ty *ck_struct(Ck *c, Expr *e, const char *fn);

/* ---- call.c: calls */
void note_re_groups(Ck *c, Expr *t, Expr *v);
Ty *ck_call_inner(Ck *c, Expr *e);

/* ---- assign.c: assignments */
void ck_store(Ck *c, Expr *t, Ty *vt, int line);
void ck_assign(Ck *c, Stmt *s);

/* ---- match.c: match */
Expr *nx(const char *name, int line);
Stmt *st_assign(Expr *t, Expr *v, int line);
Expr *ex_call(const char *fn, Expr *a, Expr *b, int line);
Expr *ex_index(Expr *o, Expr *i, int line);
const char *hidden_for(Ck *c, const char *base);
int desugar_match(Ck *c, Stmt *s);

/* ---- desugar.c: statements rewritten */
int desugar_with(Ck *c, Stmt *s);
void desugar_aug_item(Ck *c, Stmt *s, AAssign *a);
void desugar_async_for(Ck *c, Stmt *s);
int isinstance_test(Ck *c, Expr *e, const char **name, AClass **k, int allow_and);
int stores_name(Ck *c, Stmt **b, int n, const char *name);
void desugar_trystar(Ck *c, Stmt *s);
void eg_class_arg(Ck *c, Expr *e);

/* ---- stmt.c: statements */
void ck_stmt(Ck *c, Stmt *s);
int nn_has(Ck *c, AVar *v);
void nn_drop(Ck *c, AVar *v);
int falls_through(Stmt **b, int n);
void ck_body(Ck *c, AFunc *f);

/* ---- check.c: the whole program */
void ck_class_attrs(Ck *c, AClass *cls);
Ty *default_of(Ty *v, Ty *t);
void default_elems(Ty *t);

#endif /* PPY_AOT_TYPES_CHECK_H */
