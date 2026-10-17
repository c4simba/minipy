/* ========================= Interpreter: modules =========================
   Importing (the importer's folder, the main script's, PLAINPYPATH, <plainpy>/lib;
   packages, relative imports, a module in sys.modules before its body runs),
   the modules written in C (sys, math, time, random, json, thread, _ctypes)
   and those written in Python, compiled from the text in pysrc.c when first
   imported (asyncio with a real event loop, plainpy.endpoint, functools,
   typing, abc, collections.abc, dataclasses). random is xorshift32, the same
   numbers as compiled programs get.

   This header is internal to src/interp/modules: what its files share,
   grouped by the file defining it. */
#ifndef PPY_INTERP_MODULES_H
#define PPY_INTERP_MODULES_H

#include "interp/interp.h"
#include "pystdlib.h"
#include "fs.h"
#include <math.h>
#include <time.h>
#if !defined(PPY_KOLIBRI)
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#endif

void mp_remember_source(const char *file, const char *src);
char *mp_file_source(const char *file);
Value mp_std_stream(int err);
void mp_repr_into(SBuf *b, Value v);
int mp_parse_int(const char *s, int64_t n, int base, int64_t *out);
int mp_parse_float(const char *s, int64_t n, double *out);

int64_t mp_gc_objects(void);

/* ---- modules.c: native modules */
extern Value argv_list;
int mod_npos(int argc, TupleObj *kw);
Value mod_kwarg(int argc, Value *argv, TupleObj *kw, const char *name, Value dflt);
void mod_no_kw(const char *fn, TupleObj *kw);
void mod_nargs(const char *fn, int n, int lo, int hi);
Value native_module(const char *name);

/* ---- import.c: import */
DictObj *mdict(Value m);
void set_fn(Value m, const char *name, NFn fn);
Value register_module(const char *name);
Value mp_builtin_import(int argc, Value *argv, TupleObj *kw);

/* ---- sys.c: sys */
Value make_sys(void);

/* ---- math.c: math, time */
double farg(Value v);
Value make_math(void);
Value ti_sleep(int argc, Value *argv, TupleObj *kw);

/* ---- json.c: json */
Value make_json(void);

/* ---- thread.c: threads */
Value make__thread(void);
Value make_thread(void);

/* ---- ctypes.c: _ctypes */
Value make_ctypes_native(void);

/* ---- pysrc.c: modules written in Python */
const char *py_source(const char *name);

#endif /* PPY_INTERP_MODULES_H */
