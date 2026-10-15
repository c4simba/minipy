/* ========================= Typed compiler: code generator =========================
   Turns a checked program (src/aot/types) into one fasm listing for i386 that
   fasm assembles straight into an executable - a Linux ELF using int 0x80 or a
   KolibriOS MENUET01 application using int 0x40.

   Values: int/bool/pointers in eax, floats on the x87 stack (st0). Locals live
   in the machine frame ([ebp-..]); module variables in static storage.
   Strings, lists, dicts, sets, buffers and objects are reference counted: an
   expression result is either borrowed (a variable, a field, an element) or
   owned (a fresh object); owned temporaries are parked in frame slots and
   released at the end of the statement that made them.

   Functions use cdecl with all arguments on the stack (floats take 8 bytes),
   keep ebx/esi/edi/ebp, and return in eax/st0; a returned reference is owned
   by the caller. A function owns its reference parameters for its duration.

   Only the runtime routines a program uses are emitted (src/aot/runtime).

   This header is internal to src/aot/codegen: the generator's state (G: the
   program, F: the function being generated) and the functions its files
   share, grouped by the file defining them. */
#ifndef MPY_AOT_CODEGEN_GEN_H
#define MPY_AOT_CODEGEN_GEN_H

#include "aot/model.h"

typedef struct { char *s; size_t len, cap; } Buf;

typedef struct { char *s; int len; } Lit;
typedef struct { Ty *ty; int repr; } Fmt;

typedef struct G {
    AProg *p;
    AotTarget target;
    unsigned stack;
    AotRt *rt;
    Buf text, data, bss;
    int labels;
    Lit *lits; int nlits, clits;            /* static str objects S<i> */
    Lit *blits; int nblits, cblits;         /* static bytes objects B<i> */
    char *btypes[32]; int nbtypes;          /* built-in types used as values: descriptors TYD_<name> */
    Lit *plits; int nplits, cplits;         /* print literals P<i>: text + newline */
    Lit *flts; int nflts, cflts;            /* float constants FC<i>, as decimal text fasm converts */
    Lit *zlits; int nzlits, czlits;         /* NUL-terminated strings Z<i> */
    Lit *rlits; int nrlits, crlits;         /* raw bytes RL<i> (format string pieces) */
    char **ctups; int nctups, cctups;       /* static constant tuples CT<i>: their contents (to share equal ones) */
    int gc_on;                              /* some objects can be in reference cycles (the cycle collector is linked) */
    unsigned char *gc_cls, *gc_clo;         /* per class id / function id: its objects / closures can be in a cycle */
    signed char *gc_sub;                    /* per class id: a value of the class's type can be such an object (-1: not known yet) */
    int gc_func;                            /* function values can be in a cycle (closures, bound methods) */
    Buf gc_tab; int gc_ntab;                /* rt_gc_table: (destroy routine, map of references) of those classes and closures */
    AFunc **queue; int nq, cq;              /* functions to emit */
    Fmt *fmts; int nfmts, cfmts;            /* generated formatters FMT<i> (repr 2+v: JSON, variant v) */
    struct JVar { const char *isep, *ksep; int ascii; } *jvars; int njvars, cjvars;   /* json.dumps options */
    struct { AFunc *fn; int virt; } *stubs; int nstubs, cstubs;   /* task entries TS<i> */
    Ty **tdescs; int ntdescs, ctdescs;      /* tuple descriptors TD<i> */
    int *class_used;
    char *fv_used, *cd_used;                /* per function: static closure FV<id>, closure destroy routine CD<id> */
    struct { Ty *to, *from; } *adapters; int nadapters, cadapters;   /* AD<k> */
    char *bm_used;                          /* per method: BM<id>, the code of its bound values */
    char **kdlabels;                        /* per class: KDC<id>, the descriptor of its objects (class_kd) */
    int *nas; int nnas, cnas;               /* NA<z>: the AttributeError of None's attribute Z<z> (attr_check) */
    unsigned char *na_mark; int na_cap;
} G;

typedef struct Loop { int lcont, lbreak; } Loop;
/* an enclosing try statement: its handler record, whether that record is
   pushed right now, its finally block, the loop depth it started at */
typedef struct TryCtx { int rec; int active; Stmt **fin; int nfin; int nloops; } TryCtx;

typedef struct F {
    AFunc *fn;
    Buf code;
    int frame;                  /* bytes of locals below the saved registers */
    int *tmp; int ntmp, ctmp;   /* temporary reference slots (offsets) */
    int tmp_used;
    int *refs; int nrefs, crefs;/* every reference slot: released when the function returns */
    Loop loops[64]; int nloops;
    int lret;
    char param_stored[16];      /* parameter assigned in the body: the function takes its own reference */
    TryCtx tries[32]; int ntries;
    int excs[32]; int nexcs;    /* slots of the exceptions being handled (bare raise) */
    int env;                    /* frame slot of its closure (edx at entry); 0 without captures */
    struct LiveTab *live;       /* liveness of its variables after each simple statement (moves) */
    unsigned *live_after;       /* the statement being generated: variables still needed after it */
    unsigned char *occ;         /* ... and how often each occurs in it (2: more, or evaluated repeatedly) */
    Ty *ret;                    /* what `return` gives (None in a generator's function) */
} F;

/* a format spec, and where a formatted value comes from (an expression or a tuple field) */
typedef struct { int fill, align, sign, alt, zero, width, comma, prec; char type; int wslot, pslot; } Spec;   /* wslot / pslot: the width / precision is a value in that frame slot */
typedef struct { Expr *e; Ty *tt; int tslot, idx; } ArgSrc;

typedef struct { char **v; int n, cap; } LabelSet;

enum { LB_CODE, LB_VALUE, LB_FREE, LB_BOUND, LB_TASK, LB_VTASK, LB_BODY, LB_GEN, LB_KINDS };

#define NONE_HI_INT "0x80000000"

typedef enum { IT_RANGE, IT_SEQ, IT_STR, IT_KEYS, IT_ITEMS, IT_ENUM, IT_ZIP, IT_TUP, IT_GEN, IT_BYTES } ItKind;
typedef struct {
    ItKind kind;
    int top, cont, exit;
    int idx, end, step, start;      /* frame slots */
    int step_const, has_step_const;
    int src;                        /* reference slot of the iterated object (zip: srcs) */
    Ty *elem, *elem2;               /* element types (enumerate: elem2 = the items) */
    ItKind sub;                     /* enumerate: what the underlying object is */
    int nsrc, srcs[8]; ItKind subs[8]; Ty *elems[8];   /* zip: its iterables */
} Iter;

typedef struct LiveTab { Stmt **keys; unsigned **vals; int cap, n, W; } LiveTab;

/* ---- core.c: state */
void buf_put(Buf *b, const char *s, size_t n);
void buf_printf(Buf *b, const char *fmt, ...);
void buf_cat(Buf *b, Buf *x);
void put_cb(void *ctx, const char *s, size_t n);
int u8_cplen(const char *s, int n);
void buf_bytes(Buf *b, const char *s, int n, int nul);
extern G *gg;
int new_label(void);
void rt(const char *name);
void E(F *f, const char *fmt, ...);
void LBL(F *f, int l);
void CALLRT(F *f, const char *name);
Ty *TY(Expr *e);
int is_ptr(Ty *t);
int is_flt(Ty *t);
int is_lng(Ty *t);
const char *kd_of(Ty *t);
int esize(Ty *t);
void new_dict(F *f, Ty *t);
void new_list(F *f, Ty *elem);
void new_set(F *f, Ty *elem);
int str_lit(const char *s);
int print_lit(const char *s, int n);
int str_litn(const char *s, int n);
int bytes_lit(const char *s, int n);
int zlit(const char *s);
int rlit(const char *s, int n);
int float_const(const char *src);
void use_fn(AFunc *fn);
void use_class(AClass *c);

/* ---- labels.c: labels */
extern LabelSet g_labels;
extern const char **g_fnlabels[LB_KINDS];
extern const char **g_vtlabels, **g_dtlabels, **g_glabels, **g_mlabels;
const char *fnl(AFunc *fn, int kind);
const char *class_label(AClass *c, int destroy);
const char *global_label(AVar *v);
const char *module_label(AModule *m);

/* ---- vars.c: frame and variables */
int frame_slot(F *f, int size);
void add_ref_slot(F *f, int off);
void hold(F *f);
int scope_open(F *f);
void scope_close(F *f, int mark);
int ref_slot(F *f);
int is_fn_body(AFunc *f);
AVar *root_var(AVar *v);
void layout_defaults(AFunc *fn);
int dyn_defaults(AFunc *fn);
int has_defaults(AFunc *fn);
void layout_caps(AFunc *fn);
const char *fn_display_name(AFunc *g);
void var_addr(char *buf, size_t n, AVar *v);
void check_bound(F *f, AVar *v);
void load_var(F *f, AVar *v);
void store_var(F *f, AVar *v);
void store_mem(F *f, Ty *t, const char *reg, int off);
void store_new(F *f, Ty *t, const char *reg, int off);
void load_mem(F *f, Ty *t, const char *reg, int off);
void load_at(F *f, Ty *t, const char *base, const char *idx, int scale);

/* ---- none.c: None */
int is_opt(Ty *t);
void gen_none(F *f, Ty *t);
void jump_if_none(F *f, Ty *t, int label);
void check_none(F *f, Ty *t, const char *exc, const char *msg);
const char *btype_label(const char *name);
const char *typeval_label(XInfo *xi);
int btype_match(Ty *t, const char *name);
const char *py_type_name(Ty *t);
const char *none_msg(const char *fmt, ...);
extern const char *g_none_exc, *g_none_text;
void push_value(F *f, Ty *t);
void pop_value(F *f, Ty *t);
void to_i32(F *f, Ty *t);
void widen(F *f, int is_signed);
void drop_value(F *f, Ty *t, int owned);
void attr_check(F *f, const char *name);
void panic_if_null(F *f);
int self_nonnull(AFunc *fn);
int known_object(F *f, Expr *x);
void attr_check_of(F *f, Expr *obj, const char *name);
void incref(F *f);

/* ---- conv.c: conversions */
Ty *gpart(Ty *g, int i);
extern const char *cg_path;
MPY_NORETURN void cg_fail(int line, const char *msg);
AVar *find_var(F *f, const char *name, int line);
void gen_owned(F *f, Expr *e);
void gen_borrow(F *f, Expr *e);
void conv_num(F *f, Ty *from, Ty *to);
void conv(F *f, Ty *from, Ty *to);
void gen_as(F *f, Expr *e, Ty *want, int owned);

/* ---- binop.c: binary operators */
int numeric_ty(Ty *t);
int apply_binop(F *f, TokKind op, Ty *t, Ty *ta, Ty *tb, int line);
void call_on_top(F *f, AFunc *m);
int op_frame(AFunc *m);
void op_defaults(F *f, AFunc *m, int used);
int gen_op_call_r(F *f, AFunc *m, Expr *self, Expr *arg);
int gen_op_call(F *f, AFunc *m, Expr *self, Expr *arg);
const char *binop_none_msg(TokKind op, Ty *ta, Ty *tb, int left);
int gen_binary(F *f, Expr *e);

/* ---- compare.c: truth values and comparisons */
void truth(F *f, Ty *t);
int gen_boolop(F *f, Expr *e);
int is_none_lit(Expr *e);
void gen_compare(F *f, Expr *e);
void gen_bool(F *f, Expr *e);
void gen_jump(F *f, Expr *e, int label, int want);

/* ---- iter.c: iteration */
void iter_begin(F *f, Expr *it, Iter *I);
void iter_value(F *f, Iter *I, int k);
void iter_end(F *f, Iter *I);
void iter_release(F *f, Iter *I);
void store_borrowed(F *f, AVar *v, Ty *from);
void iter_store_vars(F *f, Iter *I, AVar **vars, int n);
void comp_unpack(F *f, Expr *e, int i);
int gen_comprehension(F *f, Expr *e);

/* ---- containers.c: tuples, indexing, literals */
int tuple_off(Ty *t, int i);
int tuple_size(Ty *t);
int tdesc(Ty *t);
int gen_tuple(F *f, Expr *e);
int const_tuple(Expr *e, Ty *t);
void unpack_tuple_slot(F *f, int slot, Ty *tt, Expr **targets, int n, int line);
void gen_syscall_regs(F *f, Expr *e, int keep);
int gen_index(F *f, Expr *e);
int gen_slice(F *f, Expr *e);
void list_append_top(F *f, Ty *el);
void set_add_top(F *f, Ty *el);
int gen_literal_container(F *f, Expr *e);

/* ---- call.c: calls */
void put_arg(F *f, Ty *pt, int off);
int is_keyerror(AClass *c);
int exc_rawoff(AClass *cls);
void load_field(F *f, AField *fd);
void seed_fields(F *f, AClass *cls);
int gen_ctor(F *f, Expr *e, XInfo *xi);
void call_method_on_top(F *f, AFunc *m);
int gen_call(F *f, Expr *e);

/* ---- builtin.c: built-in functions */
int gen_rev_flag(F *f, Expr *rev);
int gen_sort_by_key(F *f, int src, Ty *el, Expr *key, int revslot, int inplace);
void gen_list_of(F *f, Expr *x, Ty *elem);
void gen_type_of(F *f, Expr *x);
void gen_raise_stop(F *f, int gslot, Ty *gt);
int gen_builtin(F *f, Expr *e, XInfo *xi);

/* ---- methods.c: methods of built-in types */
void gen_codec_call(F *f, Expr *obj, Expr *enc, Expr *errs, const char *m, const char *routine);
void gen_byteorder(F *f, Expr *x);
int gen_tmethod(F *f, Expr *e, XInfo *xi);

/* ---- modules.c: built-in modules */
int gen_sys(F *f, Expr *e, XInfo *xi);
void emit_task_stub(int i);
void gen_task_new(F *f, Expr *call);
int gen_asyncio(F *f, Expr *e, XInfo *xi);
int gen_await(F *f, Expr *e);
int gen_bmod(F *f, Expr *e, XInfo *xi);

/* ---- ctypes.c: ctypes */
int gen_ccall(F *f, Expr *e, XInfo *xi);
int gen_ctypes_fn(F *f, Expr *e, XInfo *xi);

/* ---- format.c: formatting */
void sb_bytes_lit(F *f, const char *s, int n);
int parse_spec(const char *s, int n, Spec *sp);
void emit_field(F *f, Spec *sp, Ty *t, ArgSrc *src, int repr, int pystyle, int line);
void gen_format_body(F *f, Expr *e);
int gen_str_format(F *f, Expr *call);
int gen_format(F *f, Expr *e);

/* ---- expr.c: expressions */
void fill_defaults(F *f, AFunc *g, const char *lbl);
int gen_funcref(F *f, AFunc *g);
void gen_int_const(F *f, Ty *t, int64_t n);
int gen(F *f, Expr *e);
int gen_zero(F *f, Ty *t);

/* ---- strbuild.c: string building */
int is_str_lit(Expr *e);
int is_str_call(Expr *e);
int is_fmt_expr(Expr *e);
int is_concat(Expr *e);
int concat_worth(Expr *e);
void gen_into_sb(F *f, Expr *x);
int gen_concat(F *f, Expr *e);
int gen_str_of(F *f, Expr *x);
int fmt_index(Ty *t, int repr);
const char *class_kd(AClass *c);
void gen_fmt(F *f, Ty *t, int repr);

/* ---- formatters.c: formatters */
int gen_json_dumps(F *f, Expr *e, XInfo *xi);
void emit_formatter(int i);

/* ---- stmt.c: statements */
void gen_stmts(F *f, Stmt **b, int n);

/* ---- assign.c: assignments */
void store_target(F *f, Expr *t, Ty *vt, int line);
int park_value(F *f, Ty *t, int owned);
void gen_assign(F *f, Stmt *s, AAssign *a);

/* ---- try.c: exceptions */
void leave_tries(F *f, int to);
void gen_try(F *f, Stmt *s);
int loop_tries(F *f);

/* ---- borrow.c: borrowed references */
void plan_borrowed(F *f);

/* ---- moves.c: moves */
void plan_moves(F *f);
void moves_begin(F *f, Stmt *s);
int try_move(F *f, Expr *e);
void plan_consumed(AFunc *fn);

/* ---- function.c: functions */
void gen_yield_value(F *f, Ty *yt);
void drop_sent(F *f);
int take_sent(F *f, Ty *st);
int gen_yield_from(F *f, Expr *src, int want);
void emit_function(AFunc *fn);
int adapter_index(Ty *to, Ty *from);
void emit_adapter(int k);
void emit_bound_method(AFunc *m);
void emit_closure_destroy(AFunc *fn);

/* ---- gc.c: cycles */
int gc_type(Ty *t);
void gc_analyze(void);
const char *gc_class_map(AClass *c);
void gc_closure_map(AFunc *fn);

#endif /* MPY_AOT_CODEGEN_GEN_H */
