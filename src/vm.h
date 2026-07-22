#ifndef MPY_VM_H
#define MPY_VM_H

#include "value.h"
#include "bytecode.h"

/* ========================= VM state ========================= */

typedef struct { int ip; int sp; } Handler;
typedef struct { Function *fn; int ip; Dict *locals; Handler handlers[32]; int hcount; } Frame;
typedef struct { jmp_buf buf; } ExcJump;
typedef struct { Value stack[4096]; int sp; Frame frames[256]; int fcount; Dict *builtins; Dict *modules; jmp_buf panic; ExcJump exc_jumps[256]; int exc_depth; Value pending_exception; char *error_msg; } VM;

extern VM vm;

/* Stack primitives (vm.c) */
void  push(Value v);
Value popv(void);

/* Exception machinery (vm_exc.c) */
Value normalize_exception(Value v);
MPY_NORETURN void  raise_exception(Value ex);
MPY_NORETURN void  runtime_error(const char *msg);
MPY_NORETURN void  raise_named(const char *type, const char *msg);
int   is_builtin_exc_name(const char *n);
int   dispatch_pending_exception(void);
void  print_traceback(Value ex);

/* Method binding + value/runtime operations (vm_ops.c) */
int   function_declares(char **names, int n, const char *name);
int   class_find(Class *kl, const char *name, Value *out);
int   get_instance_method(Value obj, const char *name, Value *out);
int   call_instance_method0(Value obj, const char *name, Value *out);
int   call_instance_method1(Value obj, const char *name, Value arg, Value *out);
Value binary_add(Value a, Value b);
Value binary_num(Value a, Value b, Op op);
Value compare(Value a, Value b, Op op);
Value get_attr(Value obj, const char *name);
void  set_attr(Value obj, const char *name, Value val);
Value get_index(Value obj, Value idx);
void  set_index(Value obj, Value idx, Value val);
int   same_identity(Value a, Value b);
Value contains_value(Value needle, Value hay);
Value get_slice(Value obj, Value startv, Value endv, Value stepv);

/* Type methods (vm_methods.c): str/list/dict/set instance methods. */
Value call_builtin_method(Value recv, const char *name, int argc, Value *argv);

/* Builtins (vm_builtins.c) */
int   iterator_next_value(Value top, Value *out);
Value native_iter(int argc, Value *argv);
Value builtin_str(Value v);
char *mpy_instance_repr(Value v);
extern Native N_LEN, N_RANGE, N_NEXT, N_ITER, N_INPUT, N_SYSCALL;
extern Native N_STR, N_REPR, N_INT, N_FLOAT, N_BOOL, N_LIST, N_TUPLE, N_SET, N_DICT;
extern Native N_ABS, N_MIN, N_MAX, N_SUM, N_SORTED, N_REVERSED, N_ENUMERATE, N_ZIP, N_MAP, N_FILTER;
extern Native N_TYPE, N_ISINSTANCE, N_ORD, N_CHR, N_ROUND, N_ANY, N_ALL;
extern Native N_SUPER, N_STATICMETHOD, N_CLASSMETHOD, N_PROPERTY;

/* Core dispatch / calling convention / generators (vm.c) */
Value run_function(Function *fn, int argc, Value *args);
Value call_value(Value callee, int argc, Value *args);
int   generator_next(Obj *g, Value *out);

#endif /* MPY_VM_H */
