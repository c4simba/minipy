/* ========================= Values / Objects ========================= */

typedef struct Obj Obj; typedef struct Function Function; typedef struct Native Native;

typedef enum { V_NONE, V_BOOL, V_INT, V_FLOAT, V_OBJ, V_NATIVE } VType;
typedef enum { O_STRING, O_LIST, O_TUPLE, O_SET, O_DICT, O_FUNCTION, O_CLASS, O_INSTANCE, O_BOUND_METHOD, O_BOUND_NATIVE, O_MODULE, O_ITER, O_GENERATOR, O_EXCEPTION } OType;

typedef struct { VType type; union { int boolean; int64_t i; double f; Obj *obj; Native *native; } as; } Value;

struct Native { const char *name; int arity; Value (*fn)(int argc, Value *argv); };

typedef struct { char *s; int len; } String;
typedef struct { Value *items; int count, cap; } List;
typedef struct { char **keys; Value *vals; int count, cap; } Dict;

struct Function { char *name; char **params; int arity; int min_arity; Value *defaults; int default_count; char **global_names; int global_count; char **nonlocal_names; int nonlocal_count; struct Chunk *chunk; Dict *globals; Dict *closure; char *module_dir; int store_globals; int is_generator; Obj *owner; };
typedef struct { char *name; Dict *methods; } Class;
typedef struct { Class *klass; Dict *fields; } Instance;
typedef struct { Value receiver; Function *fn; } BoundMethod;
typedef struct { Value receiver; char *name; } BoundNative;
typedef struct { char *name; Dict *dict; } Module;
typedef struct { Value iterable; int index; } Iter;
typedef struct { Function *fn; Dict *locals; int ip; int done; } Generator;
typedef struct { char *type_name; char *message; Value payload; } ExceptionObj;

struct Obj { OType type; union { String str; List list; List tuple; List set; Dict dict; Function fn; Class klass; Instance inst; BoundMethod bm; BoundNative bn; Module mod; Iter iter; Generator gen; ExceptionObj exc; } as; };

static Value nonev(void){ Value v; v.type=V_NONE; return v; }
static Value boolv(int b){ Value v; v.type=V_BOOL; v.as.boolean=!!b; return v; }
static Value intv(int64_t i){ Value v; v.type=V_INT; v.as.i=i; return v; }
static Value floatv(double f){ Value v; v.type=V_FLOAT; v.as.f=f; return v; }
static Value objv(Obj *o){ Value v; v.type=V_OBJ; v.as.obj=o; return v; }
static Value nativev(Native *n){ Value v; v.type=V_NATIVE; v.as.native=n; return v; }

static Obj *new_obj(OType t){ Obj *o=(Obj*)xmalloc(sizeof(Obj)); memset(o,0,sizeof(Obj)); o->type=t; return o; }
static Value stringv_len(const char *s,int n){ Obj *o=new_obj(O_STRING); o->as.str.s=xstrndup2(s,n); o->as.str.len=n; return objv(o); }
static Value stringv(const char *s){ return stringv_len(s,(int)strlen(s)); }
static Obj *new_list(void){ Obj *o=new_obj(O_LIST); return o; }
static Obj *new_tuple(void){ Obj *o=new_obj(O_TUPLE); return o; }
static Obj *new_set(void){ Obj *o=new_obj(O_SET); return o; }
static Obj *new_dict_obj(void){ Obj *o=new_obj(O_DICT); return o; }
static Obj *new_module(const char *name, Dict *d){ Obj *o=new_obj(O_MODULE); o->as.mod.name=xstrdup2(name); o->as.mod.dict=d; return o; }
static Value exceptionv(const char *type_name,const char *message,Value payload){ Obj *o=new_obj(O_EXCEPTION); o->as.exc.type_name=xstrdup2(type_name?type_name:"RuntimeError"); o->as.exc.message=xstrdup2(message?message:""); o->as.exc.payload=payload; return objv(o); }

static int is_obj(Value v,OType t){ return v.type==V_OBJ && v.as.obj && v.as.obj->type==t; }
static int truthy(Value v){
    if(v.type==V_NONE) return 0;
    if(v.type==V_BOOL) return v.as.boolean;
    if(v.type==V_INT) return v.as.i!=0;
    if(v.type==V_FLOAT) return v.as.f!=0.0;
    if(is_obj(v,O_STRING)) return v.as.obj->as.str.len>0;
    if(is_obj(v,O_LIST)) return v.as.obj->as.list.count>0;
    if(is_obj(v,O_TUPLE)) return v.as.obj->as.tuple.count>0;
    if(is_obj(v,O_SET)) return v.as.obj->as.set.count>0;
    if(is_obj(v,O_DICT)) return v.as.obj->as.dict.count>0;
    return 1;
}
static int is_number(Value v){ return v.type==V_INT || v.type==V_FLOAT || v.type==V_BOOL; }
static double as_double(Value v){ if(v.type==V_FLOAT) return v.as.f; if(v.type==V_BOOL) return (double)v.as.boolean; return (double)v.as.i; }
static int64_t as_int(Value v){ if(v.type==V_BOOL) return v.as.boolean; return v.as.i; }

static void print_value(Value v);
static char *value_to_cstr(Value v){
    char buf[128];
    if(v.type==V_NONE) return xstrdup2("None");
    if(v.type==V_BOOL) return xstrdup2(v.as.boolean?"True":"False");
    if(v.type==V_INT){ snprintf(buf,sizeof(buf),"%lld",(long long)v.as.i); return xstrdup2(buf); }
    if(v.type==V_FLOAT){ snprintf(buf,sizeof(buf),"%.15g",v.as.f); return xstrdup2(buf); }
    if(v.type==V_NATIVE) return xstrdup2(v.as.native->name);
    if(is_obj(v,O_STRING)) return xstrdup2(v.as.obj->as.str.s);
    if(is_obj(v,O_EXCEPTION)) return xstrdup2(v.as.obj->as.exc.message);
    snprintf(buf,sizeof(buf),"<object %p>",(void*)v.as.obj); return xstrdup2(buf);
}

