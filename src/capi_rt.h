/* ========================= cpython target: runtime (header) =========================
   Every program of minipy --compile --target cpython is C over the CPython
   API: one file per compiled module (capi_codegen.c), this runtime, and a
   main. The code of a function, class body or module is a C function; what
   Python sees is a real function object whose code is a tiny trampoline
   (`def f(a, b): return __mpy_env(a, b)`, compiled at build time): CPython
   binds the arguments, frames, introspection (inspect.signature, __code__,
   __globals__) and pickling work as usual, and __mpy_env - an MpyEnv - calls
   the C. Parts CPython compiles for us (annotations, generators for now, ...)
   are code objects of the module's own source, marshalled at build time. */
#ifndef MPY_CAPI_RT_H
#define MPY_CAPI_RT_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <marshal.h>

PyAPI_FUNC(void) _PyTraceback_Add(const char *, const char *, int);
PyAPI_FUNC(int) _PyObject_GetMethod(PyObject *obj, PyObject *name, PyObject **method);   /* exported; not in the headers */

typedef struct MpyEnv MpyEnv;
typedef PyObject *(*MpyImpl)(MpyEnv *env, PyObject *const *args);
struct MpyEnv {
    PyObject_HEAD
    vectorcallfunc vectorcall;
    MpyImpl impl;
    PyObject *cells;            /* the free variables' cells (a tuple), or NULL */
    PyObject *globals;          /* the module's dict */
    Py_ssize_t direct;          /* only positional parameters: their number (callable without the trampoline); else -1 */
};
extern PyTypeObject MpyEnv_Type;

/* Calls: a compiled function given just its positional arguments runs its C
   directly (no trampoline frame); anything else goes through CPython. */
static inline PyObject *mpy_vcall(PyObject *f, PyObject *const *args, size_t nargsf, PyObject *kwnames){
    if(!kwnames && PyFunction_Check(f)){
        PyObject *clo=PyFunction_GET_CLOSURE(f);
        if(clo && PyTuple_GET_SIZE(clo)==1){
            PyObject *e=PyCell_GET(PyTuple_GET_ITEM(clo,0));
            if(e && Py_IS_TYPE(e,&MpyEnv_Type) && ((MpyEnv*)e)->direct==(Py_ssize_t)PyVectorcall_NARGS(nargsf)){
                if(Py_EnterRecursiveCall(" in compiled code")) return NULL;
                PyObject *r=((MpyEnv*)e)->impl((MpyEnv*)e,args);
                Py_LeaveRecursiveCall();
                return r;
            }
        }
    }
    return PyObject_Vectorcall(f,args,nargsf,kwnames);
}

/* Global names: a per-site cache, valid while no module dict / builtins changed (dict watchers) */
typedef struct { PyObject *dict, *val; uint64_t ver; } MpyGCache;
extern uint64_t mpy_gver;
PyObject *mpy_load_global(PyObject *globals, PyObject *name);
static inline PyObject *mpy_load_global_cached(PyObject *g, PyObject *name, MpyGCache *c){
    if(c->ver==mpy_gver && c->dict==g && c->val) return Py_NewRef(c->val);
    PyObject *v=mpy_load_global(g,name);
    if(v){ Py_XSETREF(c->val,Py_NewRef(v)); c->dict=g; c->ver=mpy_gver; }
    return v;
}

/* small ints: arithmetic and comparisons in C */
static inline int mpy_small2(PyObject *a, PyObject *b, long long *x, long long *y){
    if(!PyLong_CheckExact(a) || !PyLong_CheckExact(b)) return 0;
    if(!PyUnstable_Long_IsCompact((PyLongObject*)a) || !PyUnstable_Long_IsCompact((PyLongObject*)b)) return 0;
    *x=PyUnstable_Long_CompactValue((PyLongObject*)a); *y=PyUnstable_Long_CompactValue((PyLongObject*)b);
    return 1;
}
static inline PyObject *mpy_add(PyObject *a, PyObject *b){ long long x,y; if(mpy_small2(a,b,&x,&y)) return PyLong_FromLongLong(x+y); return PyNumber_Add(a,b); }
static inline PyObject *mpy_sub(PyObject *a, PyObject *b){ long long x,y; if(mpy_small2(a,b,&x,&y)) return PyLong_FromLongLong(x-y); return PyNumber_Subtract(a,b); }
static inline PyObject *mpy_iadd(PyObject *a, PyObject *b){ long long x,y; if(mpy_small2(a,b,&x,&y)) return PyLong_FromLongLong(x+y); return PyNumber_InPlaceAdd(a,b); }
static inline PyObject *mpy_isub(PyObject *a, PyObject *b){ long long x,y; if(mpy_small2(a,b,&x,&y)) return PyLong_FromLongLong(x-y); return PyNumber_InPlaceSubtract(a,b); }
static inline PyObject *mpy_mul(PyObject *a, PyObject *b){
    long long x,y,r; if(mpy_small2(a,b,&x,&y) && !__builtin_mul_overflow(x,y,&r)) return PyLong_FromLongLong(r);
    return PyNumber_Multiply(a,b);
}
static inline PyObject *mpy_mod(PyObject *a, PyObject *b){
    long long x,y; if(mpy_small2(a,b,&x,&y) && y>0 && x>=0) return PyLong_FromLongLong(x%y);
    return PyNumber_Remainder(a,b);
}
static inline PyObject *mpy_richcmp(PyObject *a, PyObject *b, int op){
    long long x,y;
    if(mpy_small2(a,b,&x,&y)){
        int r= op==Py_LT?x<y : op==Py_LE?x<=y : op==Py_EQ?x==y : op==Py_NE?x!=y : op==Py_GT?x>y : x>=y;
        return Py_NewRef(r?Py_True:Py_False);
    }
    return PyObject_RichCompare(a,b,op);
}

/* a compiled module */
typedef struct {
    const char *name;           /* dotted */
    int (*exec)(PyObject *module);
    int is_package;
    const char *file;           /* its source (__file__) */
    const char *dir;            /* package: its directory (__path__) */
} MpyModule;

/* the code objects a module needs: trampolines (in its stub source, by co_name)
   and code CPython compiled from its source (by qualname, first line, n-th) */
typedef struct {
    int stub;                   /* 1: a trampoline; 0: from the module's code */
    const char *name;           /* stub: co_name in the stub source; else co_qualname */
    const char *rename;         /* stub: the real co_name */
    const char *qualname;       /* stub: the real co_qualname */
    int line;                   /* stub: the real co_firstlineno; else co_firstlineno to match */
    int nth;                    /* else: which of several matches */
} MpyCodeRef;

int  mpy_main(int argc, char **argv, const MpyModule *mods, int nmods, const char *python, const char *script_dir);
int  mpy_load_codes(const unsigned char *code, Py_ssize_t codelen, const unsigned char *stub, Py_ssize_t stublen,
                    const MpyCodeRef *refs, int n, PyObject **out, const char *modname);

/* functions and classes */
PyObject *mpy_func(PyObject *code, PyObject *globals, MpyImpl impl, PyObject *cells, PyObject *defaults, PyObject *kwdefaults,
                   PyObject *annotate, PyObject *annotations, PyObject *doc, Py_ssize_t direct);
PyObject *mpy_func_cpython(PyObject *code, PyObject *globals, PyObject *const *cellv, PyObject *const *cellnames, int ncells,
                           PyObject *defaults, PyObject *kwdefaults, PyObject *annotate, PyObject *annotations);
PyObject *mpy_class(PyObject *code, PyObject *globals, MpyImpl impl, PyObject *cells, PyObject *name,
                    PyObject *const *bases, Py_ssize_t nbases, PyObject *kwnames, PyObject *const *kwvals, Py_ssize_t nkw);
PyObject *mpy_class_cpython(PyObject *code, PyObject *globals, PyObject *const *cellv, PyObject *const *cellnames, int ncells, PyObject *name,
                    PyObject *const *bases, Py_ssize_t nbases, PyObject *kwnames, PyObject *const *kwvals, Py_ssize_t nkw);
PyObject *mpy_class_ns(void);   /* in a class body: its namespace (new reference) */

/* names */
PyObject *mpy_load_name(PyObject *ns, PyObject *globals, PyObject *name);
PyObject *mpy_load_classderef(PyObject *ns, PyObject *cell, PyObject *name);
PyObject *mpy_load_cell(PyObject *cell, PyObject *name, int free);
int  mpy_unbound(PyObject *name);
int  mpy_store_name(PyObject *ns, PyObject *name, PyObject *v);
int  mpy_delete_name(PyObject *ns, PyObject *name);
int  mpy_delete_global(PyObject *globals, PyObject *name);

/* operations */
int  mpy_unpack(PyObject *v, Py_ssize_t n, PyObject **out);
int  mpy_unpack_ex(PyObject *v, Py_ssize_t before, Py_ssize_t after, PyObject **out);
PyObject *mpy_call(PyObject *f, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames);
PyObject *mpy_call_method(PyObject *name, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames);
int  mpy_args_extend(PyObject *list, PyObject *it);
int  mpy_kwargs_merge(PyObject *dict, PyObject *m, PyObject *func);
PyObject *mpy_call_ex(PyObject *f, PyObject *list, PyObject *dict);
int  mpy_list_extend(PyObject *list, PyObject *it);
int  mpy_set_update(PyObject *set, PyObject *it);
int  mpy_dict_update(PyObject *d, PyObject *m);
PyObject *mpy_format(PyObject *v, int conv, PyObject *spec);
PyObject *mpy_join(PyObject *const *parts, Py_ssize_t n);
PyObject *mpy_compare(PyObject *a, PyObject *b, int op);   /* 0..5 rich compare, 6 in, 7 not in, 8 is, 9 is not */
int  mpy_truth(PyObject *v);
PyObject *mpy_slice(PyObject *lo, PyObject *hi, PyObject *step);
PyObject *mpy_iter(PyObject *v);
int  mpy_next(PyObject *it, PyObject **item);   /* 1 item, 0 done, -1 error */

/* exceptions */
int  mpy_raise(PyObject *exc, PyObject *cause);
int  mpy_reraise(void);
int  mpy_exc_matches(PyObject *exc, PyObject *type);   /* 1, 0, -1 */
int  mpy_assert_fail(PyObject *msg);
PyObject *mpy_with_enter(PyObject *mgr, PyObject **exit_out, int is_async);
int  mpy_with_exit(PyObject *exit, PyObject *exc);    /* exc NULL: normal exit; returns 1 to swallow, 0, -1 */

/* imports */
PyObject *mpy_import(PyObject *globals, PyObject *name, PyObject *fromlist, int level);
PyObject *mpy_import_from(PyObject *module, PyObject *name);
int  mpy_import_star(PyObject *module, PyObject *ns);

/* tracebacks */
void mpy_traceback(const char *func, const char *file, int line);
int  mpy_prepare_globals(PyObject *g);   /* __builtins__, as exec() gives a module */
int  mpy_exec_source(PyObject **cache, const char *src, const char *file, int future_flags, PyObject *globals, PyObject *locals);

#endif /* MPY_CAPI_RT_H */
