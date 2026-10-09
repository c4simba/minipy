#ifndef MPY_INTERP_H
#define MPY_INTERP_H

#include "util.h"
#include "py_ast.h"

/* ========================= The interpreter =========================
   Python run the way CPython runs it, over the full parser's tree (py_ast.h):
   i_compile.c turns a module into code objects (scopes as CPython's symtable:
   fast locals, cells for closures, globals), i_eval.c runs them (frames,
   generators and coroutines with frames of their own, exceptions as class
   instances, classes with a C3 MRO and descriptors), i_obj.c holds the
   object model (str as code points over UTF-8, int64 with OverflowError,
   insertion-ordered hash dicts / sets, tuples, lists) and the collector,
   i_builtins.c the built-in functions and the methods of the built-in types,
   i_modules.c sys, math, time, random, json, asyncio, functools, thread,
   minipy and _ctypes. The typed compiler (aot_*.c) gives compiled programs
   the same behaviour; tests/typed runs every program both ways. */

/* ---------------------------------------------------------------- values */
typedef enum { V_UNDEF=0, V_NONE, V_BOOL, V_INT, V_FLOAT, V_OBJ } VKind;
typedef struct Obj Obj;
typedef struct Value { int k; union { int64_t i; double f; Obj *o; } u; } Value;

static inline Value v_undef(void){ Value v; v.k=V_UNDEF; v.u.i=0; return v; }
static inline Value v_none(void){ Value v; v.k=V_NONE; v.u.i=0; return v; }
static inline Value v_bool(int b){ Value v; v.k=V_BOOL; v.u.i=b?1:0; return v; }
static inline Value v_int(int64_t i){ Value v; v.k=V_INT; v.u.i=i; return v; }
static inline Value v_float(double f){ Value v; v.k=V_FLOAT; v.u.f=f; return v; }
static inline Value v_obj(void *o){ Value v; v.k=V_OBJ; v.u.o=(Obj*)o; return v; }
#define IS_UNDEF(v) ((v).k==V_UNDEF)
#define IS_NONE(v)  ((v).k==V_NONE)

/* ---------------------------------------------------------------- objects */
typedef struct Type Type;
struct Obj { Type *type; Obj *gcnext; uint32_t size; uint16_t mark, flags; };

/* the C layout of a type's instances */
typedef enum {
    LY_OBJECT, LY_INSTANCE, LY_EXC, LY_TYPE, LY_STR, LY_BYTES, LY_TUPLE, LY_LIST, LY_DICT, LY_SET, LY_RANGE, LY_SLICE,
    LY_FUNC, LY_NATIVE, LY_METHOD, LY_MODULE, LY_CODE, LY_CELL, LY_GEN, LY_PROPERTY, LY_STATICMETHOD, LY_CLASSMETHOD,
    LY_SUPER, LY_ITER, LY_FILE, LY_BUFFER, LY_COMPLEX, LY_BOX
} Layout;

typedef struct StrObj { Obj h; int64_t len, cplen; uint64_t hash; int hashed, ascii; char s[]; } StrObj;   /* UTF-8, NUL-terminated */
typedef struct BytesObj { Obj h; int64_t len; uint64_t hash; int hashed; unsigned char s[]; } BytesObj;
typedef struct TupleObj { Obj h; int64_t len; uint64_t hash; int hashed; Value items[]; } TupleObj;
typedef struct ListObj { Obj h; Value *items; int64_t len, cap; } ListObj;
typedef struct { Value key, val; uint64_t hash; } DEnt;
typedef struct DictObj { Obj h; DEnt *ent; int64_t nent, cap, used; int64_t *idx; int64_t isize; } DictObj;   /* insertion-ordered */
/* set / frozenset: CPython's table (open addressing, linear probes + perturbation), so they iterate in its order */
typedef struct { Value key; uint64_t hash; } SEnt;
typedef struct SetObj { Obj h; SEnt *table; int64_t mask, fill, used, finger; uint64_t hash; int hashed; } SetObj;
typedef struct RangeObj { Obj h; int64_t start, stop, step; } RangeObj;
typedef struct SliceObj { Obj h; Value start, stop, step; } SliceObj;
typedef struct CellObj { Obj h; Value v; } CellObj;
typedef struct ComplexObj { Obj h; double re, im; } ComplexObj;
typedef struct BoxObj { Obj h; Value v; } BoxObj;            /* property / staticmethod / classmethod: v; also small wrappers */
typedef struct PropertyObj { Obj h; Value get, set, del, doc; } PropertyObj;
typedef struct InstObj { Obj h; DictObj *dict; } InstObj;
typedef struct ExcObj { Obj h; DictObj *dict; Value args, cause, context, tb, notes; int suppress; void *tb_frame; } ExcObj;   /* tb: a list of (file, line, name) */
typedef struct ModuleObj { Obj h; DictObj *dict; StrObj *name; } ModuleObj;
typedef struct BufferObj { Obj h; unsigned char *data; int64_t len; } BufferObj;
typedef struct FileObj { Obj h; Value name; char *buf; int64_t len, pos, cap; int mode /* 'r' 'w' 'a' */, closed, binary; } FileObj;

struct Type {
    Obj h;
    StrObj *name;          /* __name__ */
    StrObj *qualname;
    Type *base;            /* the base whose layout it extends */
    TupleObj *bases;
    Type **mro; int nmro;
    DictObj *dict;
    Layout layout;
    int flags;
    /* built-in behaviour: construction (args as a call), iteration */
    Value (*make)(Type *t, int argc, Value *argv, TupleObj *kwnames);
};
enum { TF_BUILTIN=1, TF_BASETYPE=2, TF_HEAP=4, TF_DUNDERS=8 /* a class: look its dunders up */, TF_ABSTRACT_DONE=16 };

/* ---------------------------------------------------------------- code, functions */
enum { CO_VARARGS=1, CO_VARKW=2, CO_GEN=4, CO_CORO=8, CO_ASYNCGEN=16, CO_CLASS=32, CO_MODULE=64, CO_NESTED=128, CO_COMP=256 };
typedef struct CodeObj {
    Obj h;
    StrObj *name, *qualname;
    const char *file;
    int firstline;
    uint32_t *code; int *lines; int ncode;       /* instruction: opcode | arg<<8 */
    Value *consts; int nconsts;
    StrObj **names; int nnames;                  /* globals, attributes */
    StrObj **varnames; int nlocals;              /* the parameters first */
    StrObj **cellnames; int ncells;              /* cells, then the free variables */
    StrObj **freenames; int nfrees;
    int *cellarg;                                /* cell i starts as parameter cellarg[i] (-1: empty) */
    int argc, posonly, kwonly, flags;
    int stacksize, nblocks;
    char **annots;                               /* per parameter: the type its annotation names (int, str ...) or NULL */
    char **annname, **anntext; int nann;         /* __annotations__: parameter (or "return") and annotation source, in order */
    Value doc;                                   /* the docstring (None) */
} CodeObj;
typedef struct FuncObj {
    Obj h;
    CodeObj *code;
    DictObj *globals;
    TupleObj *defaults;                          /* NULL: none */
    DictObj *kwdefaults;
    TupleObj *closure;                           /* cells of the free variables */
    StrObj *name, *qualname;
    Value doc, module;
    DictObj *dict;                               /* __dict__ (__wrapped__, ...) */
} FuncObj;
typedef Value (*NFn)(int argc, Value *argv, TupleObj *kw);   /* keyword values follow the positional ones; kw: their names */
typedef struct NativeObj { Obj h; const char *name; NFn fn; Value self; int is_method; } NativeObj;   /* self: bound receiver (V_UNDEF: none) */
typedef struct MethodObj { Obj h; Value func, self; } MethodObj;
typedef struct SuperObj { Obj h; Type *start; Value obj; Type *objtype; } SuperObj;

/* ---------------------------------------------------------------- frames, generators */
typedef struct { int handler, sp, hdepth; } Block;
typedef struct Frame {
    struct Frame *back;
    CodeObj *code; FuncObj *func;
    DictObj *globals, *builtins, *locals;        /* locals: class / module bodies (NULL: fast locals) */
    Value *fast, *cells, *stack;
    int sp, ip, nblocks;
    Block *blocks;
    Obj *gen;                                    /* the generator / coroutine running it */
    Value yf; int yf_exit;                       /* delegating (yield from / await): the iterator, where SEND goes on */
    int state;                                   /* FS_* */
    int hbase;                                   /* the thread's handled-exception depth when it started running */
    int nopush;                                  /* resumed without a value (a yield from that ended) */
    Value saved_exc[8]; int nsaved;              /* a suspended generator's exceptions being handled */
} Frame;
enum { FS_NEW, FS_RUNNING, FS_SUSPENDED, FS_DONE };
enum { G_GEN, G_CORO, G_ASYNCGEN };
typedef struct GenObj { Obj h; Frame *f; int kind, running; StrObj *name, *qualname; Value awaiting;
                        int loop_owned; } GenObj;   /* made for a for loop (for x in gen():): closed when the loop is left early */

/* ---------------------------------------------------------------- iterators */
typedef struct IterObj { Obj h; int kind; Value src, aux, aux2; int64_t i, n; } IterObj;
enum { IT_SEQ, IT_STR, IT_BYTES, IT_RANGE, IT_DICTK, IT_DICTV, IT_DICTI, IT_SET, IT_ENUM, IT_ZIP, IT_MAP, IT_FILTER,
       IT_REVLIST, IT_CALL /* iter(f, sentinel) */, IT_GETITEM /* __getitem__ protocol */, IT_FILE, IT_NATIVE };

/* ---------------------------------------------------------------- per-thread state */
typedef struct Catch { jmp_buf jb; struct Catch *prev; Frame *frame; int depth; } Catch;
typedef struct ExcStack { Value v; struct ExcStack *prev; } ExcStack;
typedef struct Thread {
    Frame *frame;               /* innermost Python frame */
    Catch *catch;               /* innermost exception catch point */
    Value exc;                  /* the exception being raised */
    Value handled[64]; int nhandled;   /* exceptions being handled (except blocks): sys.exception(), bare raise */
    int depth;                  /* Python call depth (RecursionError) */
    void *cstack_base, *cstack_top;    /* for the collector: this thread's C stack */
    Value roots[8];             /* bootstrap values */
    int id;
} Thread;
extern Thread *mp_ts;           /* the running thread (the GIL holder) */
extern mpy_lock mp_gil;
#define MP_MAX_THREADS 64
extern Thread *mp_threads[MP_MAX_THREADS];
extern int mp_nthreads;
void mp_thread_add(Thread *t);
void mp_thread_remove(Thread *t);
void mp_gil_release(void);       /* around blocking calls: Thread *self=mp_ts; mp_gil_release(); ...; mp_gil_acquire(self); */
void mp_gil_acquire(Thread *self);

/* ---------------------------------------------------------------- the built-in types */
extern Type *T_object, *T_type, *T_none, *T_bool, *T_int, *T_float, *T_complex, *T_str, *T_bytes, *T_tuple, *T_list,
    *T_dict, *T_set, *T_frozenset, *T_range, *T_slice, *T_function, *T_native, *T_method, *T_module, *T_code,
    *T_cell, *T_generator, *T_coroutine, *T_asyncgen, *T_property, *T_staticmethod, *T_classmethod, *T_super,
    *T_iter, *T_file, *T_buffer, *T_notimpl, *T_ellipsis, *T_dict_keys, *T_dict_values, *T_dict_items, *T_box;
/* exceptions */
extern Type *E_BaseException, *E_Exception, *E_TypeError, *E_ValueError, *E_KeyError, *E_IndexError, *E_AttributeError,
    *E_NameError, *E_UnboundLocalError, *E_ZeroDivisionError, *E_OverflowError, *E_StopIteration, *E_StopAsyncIteration,
    *E_RuntimeError, *E_RecursionError, *E_NotImplementedError, *E_AssertionError, *E_ImportError, *E_ModuleNotFoundError,
    *E_OSError, *E_FileNotFoundError, *E_LookupError, *E_ArithmeticError, *E_GeneratorExit, *E_SystemExit,
    *E_KeyboardInterrupt, *E_EOFError, *E_UnicodeError, *E_UnicodeDecodeError, *E_UnicodeEncodeError, *E_SyntaxError, *E_MemoryError,
    *E_SystemError, *E_BaseExceptionGroup, *E_ExceptionGroup;
extern Value mp_NotImplemented, mp_Ellipsis;
extern DictObj *mp_builtins, *mp_modules;

/* ---------------------------------------------------------------- core API (i_obj.c) */
void   mp_init(void);                            /* types, builtins, modules */
void  *mp_alloc(Type *t, size_t size);           /* a collected object of type t */
void   mp_gc_maybe(void);                        /* at a safe point */
void   mp_gc_collect(void);
void   mp_gc_set_stack_base(void *p);
void   mp_gc_add_root(Value *v);

static inline Type *TYPE(Value v){
    extern Type *T_none, *T_bool, *T_int, *T_float;
    switch(v.k){ case V_OBJ: return v.u.o->type; case V_INT: return T_int; case V_FLOAT: return T_float; case V_BOOL: return T_bool; default: return T_none; }
}
static inline int IS(Value v, Type *t){ return v.k==V_OBJ && v.u.o->type==t; }
#define AS_STR(v)   ((StrObj*)(v).u.o)
#define AS_BYTES(v) ((BytesObj*)(v).u.o)
#define AS_TUPLE(v) ((TupleObj*)(v).u.o)
#define AS_LIST(v)  ((ListObj*)(v).u.o)
#define AS_DICT(v)  ((DictObj*)(v).u.o)
#define AS_TYPE(v)  ((Type*)(v).u.o)
#define AS_FUNC(v)  ((FuncObj*)(v).u.o)
#define AS_INST(v)  ((InstObj*)(v).u.o)
#define AS_EXC(v)   ((ExcObj*)(v).u.o)
int    mp_is_subtype(Type *t, Type *base);
int    mp_isinstance(Value v, Type *t);
static inline int IS_STR(Value v){ extern Type *T_str; return v.k==V_OBJ && (v.u.o->type==T_str || mp_is_subtype(v.u.o->type,T_str)); }
static inline int IS_INTLIKE(Value v){ return v.k==V_INT || v.k==V_BOOL; }

/* strings */
Value   mp_str(const char *s);                   /* from UTF-8 */
Value   mp_strn(const char *s, int64_t n);
Value   mp_intern(const char *s);                /* the one str object of that text */
Value   mp_strf(const char *fmt, ...);
int     mp_str_eq_c(Value s, const char *c);
int64_t mp_utf8_decode(const char *s, int64_t n, int64_t *pos);   /* code point at *pos, advances (invalid byte: itself) */
int     mp_codec(Value encoding);                                 /* 0 utf-8, 1 ascii, 2 latin-1 (LookupError otherwise) */
Value   mp_decode(const unsigned char *s, int64_t n, int codec, Value errors);   /* errors: V_UNDEF = strict */
Value   mp_encode(const char *s, int64_t n, int codec, Value errors);
int     mp_utf8_encode(char *out, uint32_t cp);
int64_t mp_str_byteoff(StrObj *s, int64_t cpi);  /* byte offset of code point cpi */
Value   mp_bytes(const void *s, int64_t n);
/* text buffers */
typedef struct { char *s; int64_t n, cap; } SBuf;
void    sb_put(SBuf *b, const char *s, int64_t n);
void    sb_puts(SBuf *b, const char *s);
void    sb_putc(SBuf *b, char c);
void    sb_printf(SBuf *b, const char *fmt, ...);
Value   sb_value(SBuf *b);                       /* a str of it (frees the buffer) */

/* containers */
Value   mp_tuple(int64_t n, const Value *items);  /* items may be NULL: filled later */
Value   mp_list(int64_t n, const Value *items);
void    mp_list_append(Value l, Value v);
Value   mp_dict(void);
Value   mp_set(Type *t);
int     mp_dict_get(DictObj *d, Value key, Value *out);
void    mp_dict_set(DictObj *d, Value key, Value val);
int     mp_dict_del(DictObj *d, Value key);
int     mp_dict_get_s(DictObj *d, const char *key, Value *out);
void    mp_dict_set_s(DictObj *d, const char *key, Value val);
int     mp_dict_next(DictObj *d, int64_t *pos, Value *key, Value *val);   /* iteration in insertion order */
void    mp_set_add(SetObj *s, Value v);
int     mp_set_has(SetObj *s, Value v);
int     mp_set_del(SetObj *s, Value v);
Value   mp_range(int64_t start, int64_t stop, int64_t step);
Value   mp_slice(Value a, Value b, Value c);
Value   mp_cell(Value v);

/* numbers */
int64_t mp_int_checked(int op, int64_t a, int64_t b);   /* '+', '-', '*': OverflowError */
void    mp_float_repr(char *out, size_t n, double f);   /* repr(): shortest that reads back */
Value   mp_complex(double re, double im);

/* protocol */
uint64_t mp_hash(Value v);
int     mp_eq(Value a, Value b);                 /* == (dispatching __eq__) */
int     mp_truth(Value v);
Value   mp_repr(Value v);
Value   mp_tostr(Value v);                       /* str(v) */
const char *mp_cstr(Value s);                    /* a str's UTF-8 */
const char *mp_type_name(Value v);
Value   mp_binop(int op, Value a, Value b);      /* op: an OP_* of py_ast.h (PyOp) */
Value   mp_inplace(int op, Value a, Value b);
Value   mp_unary(int op, Value a);
Value   mp_compare(int op, Value a, Value b);    /* OP_Lt ... OP_NotIn */
int     mp_contains(Value container, Value item);
Value   mp_getitem(Value o, Value key);
void    mp_setitem(Value o, Value key, Value val);
void    mp_delitem(Value o, Value key);
int64_t mp_len(Value v);
Value   mp_iter(Value v);
int     mp_next(Value it, Value *out);           /* 0: exhausted */
Value   mp_list_of(Value iterable);              /* list(iterable) */
int64_t mp_index(Value v, const char *what);     /* an int (or __index__) */
double  mp_float_of(Value v);                    /* int / float / bool -> double (TypeError otherwise) */
int     mp_sort(Value list, Value key, int reverse);

/* attributes, calls (i_eval.c) */
Value   mp_getattr(Value o, Value name);
Value   mp_getattr_s(Value o, const char *name);
int     mp_getattr_opt(Value o, Value name, Value *out);   /* 0: no such attribute (AttributeError not raised) */
void    mp_setattr(Value o, Value name, Value v);
void    mp_delattr(Value o, Value name);
Value   mp_type_lookup(Type *t, Value name);     /* through the MRO; V_UNDEF if none */
Value   mp_type_lookup_s(Type *t, const char *name);
Value   mp_call(Value f, int argc, Value *argv, TupleObj *kw);
Value   mp_call0(Value f);
Value   mp_call1(Value f, Value a);
Value   mp_call2(Value f, Value a, Value b);
Value   mp_callmethod(Value o, const char *name, int argc, Value *argv);   /* o.name(*argv) */
int     mp_callmethod_opt(Value o, const char *name, int argc, Value *argv, Value *out);   /* 0: no such method */
Value   mp_native(const char *name, NFn fn);     /* a built-in function */
Value   mp_new_type(const char *name, Type *base, Layout layout, int flags);
void    mp_type_add(Type *t, const char *name, NFn fn);       /* a method of a built-in type */
Value   mp_make_class(Value name, Value bases, DictObj *ns);
Value   mp_instance(Type *t);
Value   mp_gen_send(Value gen, Value v, int *done);      /* done: it returned (value: the return value) */
Value   mp_gen_throw(Value gen, Value exc, int *done);
void    mp_gen_close(Value gen);
Value   mp_run_code(CodeObj *co, DictObj *globals, DictObj *locals);
Value   mp_eval_annotation(const char *text, DictObj *globals, DictObj *overlay);
int     mp_gen_needs_close(Value g);                 /* a suspended generator in a try / with / yield from */
void    mp_asyncgen_init(void);                      /* async generators' methods */
Value   mp_excgroup(const char *fn);                 /* BaseExceptionGroup's methods installed; a function of the _excgroup module */
Value   mp_anext(Value ait, int has_default, Value dflt);   /* anext(): an awaitable */
void    mp_gen_close_quietly(Value g);               /* close() it; what that raises is dropped */   /* an annotation's value (its source text) */
Value   mp_await_iter(Value awaitable);          /* the iterator `await` drives */

/* exceptions */
MPY_NORETURN void mp_raise(Value exc);           /* an instance or a class */
MPY_NORETURN void mp_raise_t(Type *t, const char *fmt, ...);
Value   mp_exc(Type *t, const char *fmt, ...);   /* an instance */
Value   mp_exc_args(Type *t, Value args);
int     mp_exc_matches(Value exc, Value spec);   /* except spec: a class or a tuple of them */
void    mp_print_exception(Value exc);           /* the traceback CPython prints, to stderr */
Value   mp_exc_str(Value exc);
/* catching in C: if(!CATCH_BEGIN(c)){ protected; CATCH_END(c); } else { exc = c.exc ... } */
void    mp_catch_push(Catch *c);
void    mp_catch_pop(Catch *c);
Value   mp_catch_exc(Catch *c);
#define CATCH_BEGIN(c) (mp_catch_push(&(c)), setjmp((c).jb))
#define CATCH_END(c)   mp_catch_pop(&(c))

/* compiler (i_compile.c) */
CodeObj *mp_compile(PyNode *mod, const char *file, const char *modname, char **error, int *error_line);
void     mp_dump_code(CodeObj *co);

/* modules (i_modules.c) */
void   mp_modules_init(void);
Value  mp_import(const char *name, DictObj *importer_globals, int level, int fromlist);   /* fromlist: return the leaf */
Value  mp_import_from(Value module, Value name);
Value  mp_new_module(const char *name);
Value  mp_run_main(const char *path);           /* run a script as __main__ */
void   mp_set_argv(int argc, char **argv);
void   mp_flush_stdout(void);
void   mp_write_out(const char *s, int64_t n);  /* stdout (buffered) */
void   mp_write_err(const char *s, int64_t n);
extern const char *mp_main_dir;
int64_t mp_rand_bits(void);                      /* xorshift32, as compiled programs */

/* builtins (i_builtins.c) */
void   mp_builtins_init(void);
Value  mp_format(Value v, Value spec);           /* format(v, spec) */
Value  mp_percent_format(Value fmt, Value args); /* fmt % args */
Value  mp_str_format(Value fmt, int argc, Value *argv, TupleObj *kw);
Value  mp_open(Value path, Value mode);
int    mp_print_to(SBuf *b, int argc, Value *argv, Value sep, Value end);

/* opcodes (i_compile.c emits them, i_eval.c runs them) */
typedef enum {
    I_NOP, I_POP, I_DUP, I_DUP2, I_ROT2, I_ROT3, I_ROT4, I_CONST, I_NONE,
    I_LOAD_FAST, I_STORE_FAST, I_DEL_FAST, I_LOAD_DEREF, I_STORE_DEREF, I_DEL_DEREF, I_LOAD_GLOBAL, I_STORE_GLOBAL, I_DEL_GLOBAL,
    I_LOAD_NAME, I_STORE_NAME, I_DEL_NAME, I_LOAD_CLASSDEREF, I_LOAD_ATTR, I_STORE_ATTR, I_DEL_ATTR, I_LOAD_METHOD, I_CALL_METHOD,
    I_SUBSCR, I_STORE_SUBSCR, I_DEL_SUBSCR, I_BINOP, I_INPLACE, I_UNARY, I_COMPARE, I_NOT, I_TRUTH,
    I_JUMP, I_JUMP_IF_FALSE, I_JUMP_IF_TRUE, I_JUMP_IF_FALSE_KEEP, I_JUMP_IF_TRUE_KEEP, I_JUMP_IF_NOT_EXC,
    I_BUILD_TUPLE, I_BUILD_LIST, I_BUILD_SET, I_BUILD_DICT, I_BUILD_SLICE, I_BUILD_STRING, I_LIST_APPEND, I_LIST_EXTEND,
    I_SET_ADD, I_SET_UPDATE, I_DICT_SET, I_DICT_UPDATE, I_LIST_TO_TUPLE, I_FORMAT, I_FORMAT_SPEC,
    I_UNPACK, I_UNPACK_EX, I_GET_ITER, I_FOR_ITER, I_CALL, I_CALL_KW, I_CALL_EX, I_RETURN, I_MAKE_FUNCTION,
    I_MAKE_CLASS, I_IMPORT, I_IMPORT_FROM, I_IMPORT_STAR, I_SETUP, I_POP_BLOCK, I_RAISE, I_RERAISE, I_PUSH_EXC, I_POP_EXC,
    I_EXC_MATCH, I_EXC_MATCH_STAR, I_YIELD, I_GET_YIELD_FROM_ITER, I_GET_AWAITABLE, I_SEND, I_GET_AITER, I_GET_ANEXT,
    I_END_ASYNC_FOR, I_ASSERT_FAIL, I_LOAD_BUILD_CLASS, I_WITH_ENTER, I_WITH_EXIT, I_ASYNC_WITH_ENTER,
    I_RETURN_GEN, I_PRINT_EXPR, I_CHECK_EXC_GROUP, I_MATCH_CLASS, I_MATCH_SEQ, I_MATCH_MAP, I_MATCH_KEYS, I_COPY, I_SWAP,
    I_LOAD_LOCALS, I_SETUP_ANNOTATIONS, I_DEL_SUBSCR_SLICE, I_CALL_INTRINSIC, I_LOAD_CELL, I__COUNT
} IOp;

#endif /* MPY_INTERP_H */
