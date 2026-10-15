/* ========================= Interpreter: running code =========================
   Exceptions (raised with longjmp to the innermost catch point: a frame's
   eval loop or a native that catches) and their tracebacks (raise.c,
   traceback.c), attributes (the MRO, descriptors: functions bind, property,
   staticmethod, classmethod, super: attr.c), calls (the argument binding and
   messages of CPython: call.c), classes (C3 MRO: class.c), generators and
   coroutines (a frame of their own, suspended at yield: gen.c), and the eval
   loop over compile.c's instructions (eval.c).

   This header is internal to those files: what they share, grouped by the
   file defining it. */
#ifndef MPY_INTERP_RUN_H
#define MPY_INTERP_RUN_H

#include "interp.h"
#include <math.h>

int mp_next2(IterObj *it, Value *a, Value *b);   /* (ops.c) */

#define RECURSION_LIMIT 1000
Value mp_iter_kind(int kind, Value src, Value aux, Value aux2);

Value mp_set_copy(SetObj *s, Type *t);
void  mp_set_merge(SetObj *s, SetObj *o);
int   mp_native_send(Value it, Value v, Value *out);   /* modules/pysrc.c: 1 yielded, 0 returned (out: the value) */

/* ---- raise.c: raising exceptions */
MPY_NORETURN void raise_name(Type *t, Frame *f, const char *name, const char *fmt, ...);
Value instantiate_exc(Value e);
MPY_NORETURN void reraise(Value e);
void tb_add(Value e, Frame *f);

/* ---- attr.c: attributes */
Value descr_get(Value d, Value obj, Type *t);
DictObj *inst_dict(Value o);

/* ---- call.c: calls */
Frame *frame_new(CodeObj *co, FuncObj *fn, DictObj *globals, DictObj *locals);
Value call_function(FuncObj *fn, int argc, Value *argv, TupleObj *kw, DictObj *locals);

/* ---- class.c: classes */
Value subval_call(Type *c, int argc, Value *argv, TupleObj *kw);
Value mp_instance_call(Type *t, int argc, Value *argv, TupleObj *kw);

/* ---- gen.c: generators, coroutines */
Value gen_new(Frame *f, FuncObj *fn);
void close_loop_gens(Value *v, int n);
Value ag_wrap(Value v);
int send_into(Value recv, Value v, Value *out);

/* ---- eval.c: the eval loop */
Value eval(Frame *f, Value sent, int resume, int throwing);

#endif /* MPY_INTERP_RUN_H */
