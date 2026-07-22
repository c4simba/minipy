#ifndef MPY_VALUE_H
#define MPY_VALUE_H

#include "util.h"

/* ========================= Values / Objects ========================= */

typedef struct Obj Obj; typedef struct Function Function; typedef struct Native Native;
struct Chunk; /* defined in bytecode.h; Function only needs the pointer */

typedef enum { V_NONE, V_BOOL, V_INT, V_FLOAT, V_OBJ, V_NATIVE } VType;
typedef enum { O_STRING, O_LIST, O_TUPLE, O_SET, O_DICT, O_FUNCTION, O_CLASS, O_INSTANCE, O_BOUND_METHOD, O_BOUND_NATIVE, O_MODULE, O_ITER, O_GENERATOR, O_EXCEPTION } OType;

typedef struct { VType type; union { int boolean; int64_t i; double f; Obj *obj; Native *native; } as; } Value;

struct Native { const char *name; int arity; Value (*fn)(int argc, Value *argv); };

typedef struct { char *s; int len; } String;
typedef struct { Value *items; int count, cap; } List;
typedef struct { char **keys; Value *vals; int count, cap; } Dict;

struct Function { char *name; char **params; int arity; int min_arity; Value *defaults; int default_count; int star_index; int dstar_index; char **global_names; int global_count; char **nonlocal_names; int nonlocal_count; struct Chunk *chunk; Dict *globals; Dict *closure; char *module_dir; int store_globals; int is_generator; Obj *owner; };
typedef struct { char *name; Dict *methods; } Class;
typedef struct { Class *klass; Dict *fields; } Instance;
typedef struct { Value receiver; Function *fn; } BoundMethod;
typedef struct { Value receiver; char *name; } BoundNative;
typedef struct { char *name; Dict *dict; } Module;
typedef struct { Value iterable; int index; } Iter;
typedef struct { Function *fn; Dict *locals; int ip; int done; } Generator;
typedef struct { char *type_name; char *message; Value payload; } ExceptionObj;

struct Obj { OType type; union { String str; List list; List tuple; List set; Dict dict; Function fn; Class klass; Instance inst; BoundMethod bm; BoundNative bn; Module mod; Iter iter; Generator gen; ExceptionObj exc; } as; };

/* Constructors */
Value nonev(void);
Value boolv(int b);
Value intv(int64_t i);
Value floatv(double f);
Value objv(Obj *o);
Value nativev(Native *n);
Obj  *new_obj(OType t);
Value stringv_len(const char *s, int n);
Value stringv(const char *s);
Obj  *new_list(void);
Obj  *new_tuple(void);
Obj  *new_set(void);
Obj  *new_dict_obj(void);
Obj  *new_module(const char *name, Dict *d);
Value exceptionv(const char *type_name, const char *message, Value payload);

/* Inspection / coercion */
int      is_obj(Value v, OType t);
int      truthy(Value v);
int      is_number(Value v);
double   as_double(Value v);
int64_t  as_int(Value v);
char    *value_to_cstr(Value v);

/* Value operations. print_value lives in value.c; val_equal lives in
   vm_ops.c (it needs to dispatch __eq__), but is declared here because it is
   fundamentally a Value operation and is called from containers.c/vm. */
void print_value(Value v);
int  val_equal(Value a, Value b);
/* Python-style textual form. repr!=0 quotes strings; containers always show the
   repr of their elements. Returns a heap string the caller frees. */
char *value_repr(Value v, int repr);

#endif /* MPY_VALUE_H */
