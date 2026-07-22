/* ========================= Values / Objects ========================= */

#include "value.h"

Value nonev(void){ Value v; v.type=V_NONE; return v; }
Value boolv(int b){ Value v; v.type=V_BOOL; v.as.boolean=!!b; return v; }
Value intv(int64_t i){ Value v; v.type=V_INT; v.as.i=i; return v; }
Value floatv(double f){ Value v; v.type=V_FLOAT; v.as.f=f; return v; }
Value objv(Obj *o){ Value v; v.type=V_OBJ; v.as.obj=o; return v; }
Value nativev(Native *n){ Value v; v.type=V_NATIVE; v.as.native=n; return v; }

/* Format a double the way Python str()/repr() does: integral floats keep a
   trailing ".0" (2.0 -> "2.0", not "2"). */
static void fmt_float(char *buf, size_t n, double f){ snprintf(buf,n,"%.15g",f); if(!strpbrk(buf,".eEnN")){ size_t l=strlen(buf); if(l+2<n){ buf[l]='.'; buf[l+1]='0'; buf[l+2]=0; } } }

Obj *new_obj(OType t){ Obj *o=(Obj*)xmalloc(sizeof(Obj)); memset(o,0,sizeof(Obj)); o->type=t; return o; }
Value class_to_value(Class *k){ Obj *o=(Obj*)((char*)k - offsetof(Obj,as)); return objv(o); }
char *(*mpy_repr_hook)(Value v)=NULL;
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
    if(v.type==V_FLOAT){ fmt_float(buf,sizeof(buf),v.as.f); return xstrdup2(buf); }
    if(v.type==V_NATIVE) return xstrdup2(v.as.native->name);
    if(is_obj(v,O_STRING)) return xstrdup2(v.as.obj->as.str.s);
    if(is_obj(v,O_EXCEPTION)) return xstrdup2(v.as.obj->as.exc.message);
    snprintf(buf,sizeof(buf),"<object %p>",(void*)v.as.obj); return xstrdup2(buf);
}

/* print() uses str() semantics: bare strings unquoted, containers show the
   repr of their elements - exactly value_repr(v, repr=0). */
void print_value(Value v){ char *s=value_repr(v,0); printf("%s",s); free(s); }

/* ---- value_repr: Python-style str()/repr() ---- */
typedef struct { char *s; int len, cap; } RBuf;
static void rb_ch(RBuf *b, char c){ if(b->len>=b->cap){ b->cap=b->cap?b->cap*2:64; b->s=(char*)xrealloc(b->s,(size_t)b->cap+1); } b->s[b->len++]=c; }
static void rb_str(RBuf *b, const char *s){ while(*s) rb_ch(b,*s++); }
static void repr_into(RBuf *b, Value v, int repr){
    char tmp[128];
    if(v.type==V_NONE){ rb_str(b,"None"); return; }
    if(v.type==V_BOOL){ rb_str(b,v.as.boolean?"True":"False"); return; }
    if(v.type==V_INT){ snprintf(tmp,sizeof(tmp),"%lld",(long long)v.as.i); rb_str(b,tmp); return; }
    if(v.type==V_FLOAT){ fmt_float(tmp,sizeof(tmp),v.as.f); rb_str(b,tmp); return; }
    if(v.type==V_NATIVE){ rb_str(b,"<built-in function "); rb_str(b,v.as.native->name); rb_ch(b,'>'); return; }
    if(is_obj(v,O_STRING)){ if(repr){ rb_ch(b,'\''); rb_str(b,v.as.obj->as.str.s); rb_ch(b,'\''); } else rb_str(b,v.as.obj->as.str.s); return; }
    if(is_obj(v,O_LIST)){ List *l=&v.as.obj->as.list; rb_ch(b,'['); for(int i=0;i<l->count;i++){ if(i) rb_str(b,", "); repr_into(b,l->items[i],1); } rb_ch(b,']'); return; }
    if(is_obj(v,O_TUPLE)){ List *l=&v.as.obj->as.tuple; rb_ch(b,'('); for(int i=0;i<l->count;i++){ if(i) rb_str(b,", "); repr_into(b,l->items[i],1); } if(l->count==1) rb_ch(b,','); rb_ch(b,')'); return; }
    if(is_obj(v,O_SET)){ List *l=&v.as.obj->as.set; if(l->count==0){ rb_str(b,"set()"); return; } rb_ch(b,'{'); for(int i=0;i<l->count;i++){ if(i) rb_str(b,", "); repr_into(b,l->items[i],1); } rb_ch(b,'}'); return; }
    if(is_obj(v,O_DICT)){ Dict *d=&v.as.obj->as.dict; rb_ch(b,'{'); for(int i=0;i<d->count;i++){ if(i) rb_str(b,", "); rb_ch(b,'\''); rb_str(b,d->keys[i]); rb_str(b,"': "); repr_into(b,d->vals[i],1); } rb_ch(b,'}'); return; }
    if(is_obj(v,O_FUNCTION)){ rb_str(b,"<function "); rb_str(b,v.as.obj->as.fn.name); rb_ch(b,'>'); return; }
    if(is_obj(v,O_CLASS)){ rb_str(b,"<class "); rb_str(b,v.as.obj->as.klass.name); rb_ch(b,'>'); return; }
    if(is_obj(v,O_INSTANCE)){ if(mpy_repr_hook){ char *s=mpy_repr_hook(v); if(s){ rb_str(b,s); free(s); return; } } rb_ch(b,'<'); rb_str(b,v.as.obj->as.inst.klass->name); rb_str(b," instance>"); return; }
    if(is_obj(v,O_BOUND_METHOD)){ rb_str(b,"<bound method "); rb_str(b,v.as.obj->as.bm.fn->name); rb_ch(b,'>'); return; }
    if(is_obj(v,O_BOUND_NATIVE)){ rb_str(b,"<bound native "); rb_str(b,v.as.obj->as.bn.name); rb_ch(b,'>'); return; }
    if(is_obj(v,O_MODULE)){ rb_str(b,"<module "); rb_str(b,v.as.obj->as.mod.name); rb_ch(b,'>'); return; }
    if(is_obj(v,O_ITER)){ rb_str(b,"<iterator>"); return; }
    if(is_obj(v,O_GENERATOR)){ rb_str(b,"<generator>"); return; }
    if(is_obj(v,O_EXCEPTION)){ rb_str(b,v.as.obj->as.exc.message); return; }
    { char *s=value_to_cstr(v); rb_str(b,s); free(s); }
}
char *value_repr(Value v, int repr){ RBuf b; b.s=NULL; b.len=0; b.cap=0; repr_into(&b,v,repr); rb_ch(&b,0); b.len--; return b.s?b.s:xstrdup2(""); }
