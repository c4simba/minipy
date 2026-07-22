/* ========================= Values / Objects ========================= */

#include "value.h"

Value nonev(void){ Value v; v.type=V_NONE; return v; }
Value boolv(int b){ Value v; v.type=V_BOOL; v.as.boolean=!!b; return v; }
Value intv(int64_t i){ Value v; v.type=V_INT; v.as.i=i; return v; }
Value floatv(double f){ Value v; v.type=V_FLOAT; v.as.f=f; return v; }
Value objv(Obj *o){ Value v; v.type=V_OBJ; v.as.obj=o; return v; }
Value nativev(Native *n){ Value v; v.type=V_NATIVE; v.as.native=n; return v; }

Obj *new_obj(OType t){ Obj *o=(Obj*)xmalloc(sizeof(Obj)); memset(o,0,sizeof(Obj)); o->type=t; return o; }
Value stringv_len(const char *s,int n){ Obj *o=new_obj(O_STRING); o->as.str.s=xstrndup2(s,n); o->as.str.len=n; return objv(o); }
Value stringv(const char *s){ return stringv_len(s,(int)strlen(s)); }
Obj *new_list(void){ Obj *o=new_obj(O_LIST); return o; }
Obj *new_tuple(void){ Obj *o=new_obj(O_TUPLE); return o; }
Obj *new_set(void){ Obj *o=new_obj(O_SET); return o; }
Obj *new_dict_obj(void){ Obj *o=new_obj(O_DICT); return o; }
Obj *new_module(const char *name, Dict *d){ Obj *o=new_obj(O_MODULE); o->as.mod.name=xstrdup2(name); o->as.mod.dict=d; return o; }
Value exceptionv(const char *type_name,const char *message,Value payload){ Obj *o=new_obj(O_EXCEPTION); o->as.exc.type_name=xstrdup2(type_name?type_name:"RuntimeError"); o->as.exc.message=xstrdup2(message?message:""); o->as.exc.payload=payload; return objv(o); }

int is_obj(Value v,OType t){ return v.type==V_OBJ && v.as.obj && v.as.obj->type==t; }
int truthy(Value v){
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
int is_number(Value v){ return v.type==V_INT || v.type==V_FLOAT || v.type==V_BOOL; }
double as_double(Value v){ if(v.type==V_FLOAT) return v.as.f; if(v.type==V_BOOL) return (double)v.as.boolean; return (double)v.as.i; }
int64_t as_int(Value v){ if(v.type==V_BOOL) return v.as.boolean; return v.as.i; }

char *value_to_cstr(Value v){
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

void print_value(Value v){
    switch(v.type){ case V_NONE: printf("None"); break; case V_BOOL: printf(v.as.boolean?"True":"False"); break; case V_INT: printf("%lld",(long long)v.as.i); break; case V_FLOAT: printf("%.15g",v.as.f); break; case V_NATIVE: printf("<native %s>",v.as.native->name); break; case V_OBJ:{ Obj *o=v.as.obj; switch(o->type){
        case O_STRING: printf("%s",o->as.str.s); break;
        case O_LIST: printf("["); for(int i=0;i<o->as.list.count;i++){ if(i)printf(", "); print_value(o->as.list.items[i]); } printf("]"); break;
        case O_TUPLE: printf("("); for(int i=0;i<o->as.tuple.count;i++){ if(i)printf(", "); print_value(o->as.tuple.items[i]); } if(o->as.tuple.count==1) printf(","); printf(")"); break;
        case O_SET: printf("{"); for(int i=0;i<o->as.set.count;i++){ if(i)printf(", "); print_value(o->as.set.items[i]); } printf("}"); break;
        case O_DICT: printf("{"); for(int i=0;i<o->as.dict.count;i++){ if(i)printf(", "); printf("%s: ",o->as.dict.keys[i]); print_value(o->as.dict.vals[i]); } printf("}"); break;
        case O_FUNCTION: printf("<function %s>",o->as.fn.name); break; case O_CLASS: printf("<class %s>",o->as.klass.name); break; case O_INSTANCE: printf("<%s instance>",o->as.inst.klass->name); break; case O_BOUND_METHOD: printf("<bound method %s>",o->as.bm.fn->name); break; case O_BOUND_NATIVE: printf("<bound native %s>",o->as.bn.name); break; case O_MODULE: printf("<module %s>",o->as.mod.name); break; case O_ITER: printf("<iterator>"); break; case O_GENERATOR: printf("<generator>"); break; case O_EXCEPTION: printf("%s",o->as.exc.message); break; } break; }
    }
}
