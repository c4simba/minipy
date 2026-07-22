/* ========================= VM: value / runtime operations ========================= */

#include "vm.h"
#include "containers.h"

int function_declares(char **names,int n,const char *name){ for(int i=0;i<n;i++) if(strcmp(names[i],name)==0) return 1; return 0; }
int get_instance_method(Value obj,const char *name,Value *out){
    if(is_obj(obj,O_INSTANCE)){ Instance *in=&obj.as.obj->as.inst; if(dict_get(in->klass->methods,name,out) && is_obj(*out,O_FUNCTION)){ Obj *b=new_obj(O_BOUND_METHOD); b->as.bm.receiver=obj; b->as.bm.fn=&out->as.obj->as.fn; *out=objv(b); return 1; } }
    return 0;
}
int call_instance_method0(Value obj,const char *name, Value *out){
    Value m; if(!get_instance_method(obj,name,&m)) return 0;
    *out=call_value(m,0,NULL); return 1;
}
int call_instance_method1(Value obj,const char *name, Value arg, Value *out){
    Value m; if(!get_instance_method(obj,name,&m)) return 0;
    Value argv[1]; argv[0]=arg; *out=call_value(m,1,argv); return 1;
}
int val_equal(Value a,Value b){
    if(a.type!=b.type){ if(is_number(a)&&is_number(b)) return as_double(a)==as_double(b); return 0; }
    if(a.type==V_NONE)return 1;
    if(a.type==V_BOOL)return a.as.boolean==b.as.boolean;
    if(a.type==V_INT)return a.as.i==b.as.i;
    if(a.type==V_FLOAT)return a.as.f==b.as.f;
    if(is_obj(a,O_STRING)&&is_obj(b,O_STRING)) return strcmp(a.as.obj->as.str.s,b.as.obj->as.str.s)==0;
    Value r; if(call_instance_method1(a,"__eq__",b,&r)) return truthy(r);
    return a.as.obj==b.as.obj;
}
Value binary_add(Value a,Value b){
    Value r; if(call_instance_method1(a,"__add__",b,&r)) return r;
    if(is_number(a)&&is_number(b)){ if(a.type==V_FLOAT||b.type==V_FLOAT) return floatv(as_double(a)+as_double(b)); return intv(as_int(a)+as_int(b)); }
    if(is_obj(a,O_STRING)||is_obj(b,O_STRING)){ char *sa=value_to_cstr(a); char *sb=value_to_cstr(b); int n=(int)(strlen(sa)+strlen(sb)); char *r=(char*)xmalloc((size_t)n+1); strcpy(r,sa); strcat(r,sb); Value v=stringv(r); free(sa); free(sb); free(r); return v; }
    runtime_error("unsupported operands for +"); return nonev();
}
/* printf-style string formatting for  "fmt" % args  (args: single value or tuple). */
static Value str_format(Value fmtv, Value args){
    const char *fmt=fmtv.as.obj->as.str.s;
    Value single[1]; Value *av; int an, ai=0;
    if(is_obj(args,O_TUPLE)){ av=args.as.obj->as.tuple.items; an=args.as.obj->as.tuple.count; }
    else { single[0]=args; av=single; an=1; }
    size_t cap=64, len=0; char *out=(char*)xmalloc(cap+1);
    #define APPEND_CH(c) do{ if(len>=cap){ cap*=2; out=(char*)xrealloc(out,cap+1);} out[len++]=(c); }while(0)
    #define APPEND_STR(s) do{ const char *_s=(s); while(*_s){ APPEND_CH(*_s); _s++; } }while(0)
    for(const char *p=fmt; *p; p++){
        if(*p!='%'){ APPEND_CH(*p); continue; }
        p++;
        if(*p=='%'){ APPEND_CH('%'); continue; }
        char spec[32]; int si=0; spec[si++]='%';
        while(*p && (*p=='-'||*p=='+'||*p==' '||*p=='0'||*p=='#'||(*p>='0'&&*p<='9')||*p=='.')){ if(si<28) spec[si++]=*p; p++; }
        char conv=*p; if(!conv) break;
        Value arg = (ai<an)?av[ai++]:nonev();
        char tmp[128];
        if(conv=='s'){ spec[si++]=conv; spec[si]=0; char *sv=value_to_cstr(arg); size_t need=strlen(sv)+64; char *buf=(char*)xmalloc(need); snprintf(buf,need,spec,sv); APPEND_STR(buf); free(buf); free(sv); }
        else if(conv=='d'||conv=='i'||conv=='x'||conv=='X'||conv=='o'||conv=='u'){ spec[si++]='l'; spec[si++]='l'; spec[si++]=(conv=='i')?'d':conv; spec[si]=0; snprintf(tmp,sizeof(tmp),spec,(long long)as_int(arg)); APPEND_STR(tmp); }
        else if(conv=='f'||conv=='F'||conv=='e'||conv=='E'||conv=='g'||conv=='G'){ spec[si++]=conv; spec[si]=0; snprintf(tmp,sizeof(tmp),spec,as_double(arg)); APPEND_STR(tmp); }
        else if(conv=='c'){ APPEND_CH((char)as_int(arg)); }
        else { APPEND_CH('%'); APPEND_CH(conv); }
    }
    out[len]=0;
    Value r=stringv_len(out,(int)len); free(out); return r;
    #undef APPEND_CH
    #undef APPEND_STR
}
Value binary_num(Value a,Value b,Op op){
    if(op==OP_MOD && is_obj(a,O_STRING)) return str_format(a,b);
    if(!is_number(a)||!is_number(b)) runtime_error("numeric operator on non-number");
    if(op==OP_BITAND||op==OP_BITOR||op==OP_BITXOR||op==OP_SHL||op==OP_SHR){ if(a.type==V_FLOAT||b.type==V_FLOAT) runtime_error("bitwise operator on float"); int64_t x=as_int(a),y=as_int(b); switch(op){ case OP_BITAND: return intv(x&y); case OP_BITOR: return intv(x|y); case OP_BITXOR: return intv(x^y); case OP_SHL: return intv(x<<y); case OP_SHR: return intv(x>>y); default: break; } }
    if(op==OP_MOD){ if(a.type==V_FLOAT||b.type==V_FLOAT){ double x=as_double(a),y=as_double(b); if(y==0.0) runtime_error("float modulo by zero"); double q=x/y; int64_t t=(int64_t)q; if((double)t>q) t--; return floatv(x-(double)t*y); } int64_t x=as_int(a),y=as_int(b); if(y==0) runtime_error("integer modulo by zero"); int64_t r=x%y; if(r!=0 && ((r<0)!=(y<0))) r+=y; return intv(r); }
    if(op==OP_DIV) return floatv(as_double(a)/as_double(b)); if(op==OP_FLOOR_DIV){ if(a.type==V_FLOAT||b.type==V_FLOAT){ double x=as_double(a),y=as_double(b); if(y==0.0) runtime_error("float floor division by zero"); double q=x/y; int64_t t=(int64_t)q; if((double)t>q) t--; return floatv((double)t); } int64_t x=as_int(a),y=as_int(b); if(y==0) runtime_error("integer division or modulo by zero"); int64_t q=x/y; if((x%y!=0) && ((x<0)!=(y<0))) q--; return intv(q); } if(op==OP_POWER){ if((a.type==V_INT||a.type==V_BOOL)&&(b.type==V_INT||b.type==V_BOOL)&&as_int(b)>=0){ int64_t base=as_int(a),e=as_int(b),r=1; for(int64_t i=0;i<e;i++) r*=base; return intv(r); } double base=as_double(a); double exp=as_double(b); double r=1; int e_neg=0; if(exp<0){ e_neg=1; exp=-exp; } int ei=(int)exp; for(int i=0;i<ei;i++) r*=base; if(e_neg) r=1.0/r; return floatv(r); } if(a.type==V_FLOAT||b.type==V_FLOAT){ double x=as_double(a),y=as_double(b); if(op==OP_SUB)return floatv(x-y); if(op==OP_MUL)return floatv(x*y); } int64_t x=as_int(a),y=as_int(b); if(op==OP_SUB)return intv(x-y); return intv(x*y); }
Value compare(Value a,Value b,Op op){ int r=0; if(is_number(a)&&is_number(b)){ double x=as_double(a),y=as_double(b); r=(op==OP_LT?x<y:op==OP_LE?x<=y:op==OP_GT?x>y:x>=y); } else if(is_obj(a,O_STRING)&&is_obj(b,O_STRING)){ int c=strcmp(a.as.obj->as.str.s,b.as.obj->as.str.s); r=(op==OP_LT?c<0:op==OP_LE?c<=0:op==OP_GT?c>0:c>=0); } else runtime_error("comparison between unsupported types"); return boolv(r); }

Value get_attr(Value obj,const char *name){
    if(is_obj(obj,O_INSTANCE)){ Instance *in=&obj.as.obj->as.inst; Value v; if(dict_get(in->fields,name,&v)) return v; if(dict_get(in->klass->methods,name,&v) && is_obj(v,O_FUNCTION)){ Obj *b=new_obj(O_BOUND_METHOD); b->as.bm.receiver=obj; b->as.bm.fn=&v.as.obj->as.fn; return objv(b); } runtime_error("unknown instance attribute"); }
    if(is_obj(obj,O_CLASS)){ Value v; if(dict_get(obj.as.obj->as.klass.methods,name,&v)) return v; runtime_error("unknown class attribute"); }
    if(is_obj(obj,O_MODULE)){ Value v; if(dict_get(obj.as.obj->as.mod.dict,name,&v)) return v; runtime_error("unknown module attribute"); }
    if(is_obj(obj,O_LIST)&&strcmp(name,"append")==0){ Obj *b=new_obj(O_BOUND_NATIVE); b->as.bn.receiver=obj; b->as.bn.name=xstrdup2("append"); return objv(b); }
    if(is_obj(obj,O_SET)&&strcmp(name,"add")==0){ Obj *b=new_obj(O_BOUND_NATIVE); b->as.bn.receiver=obj; b->as.bn.name=xstrdup2("add"); return objv(b); }
    runtime_error("attribute access on unsupported type"); return nonev();
}
void set_attr(Value obj,const char *name,Value val){ if(is_obj(obj,O_INSTANCE)){ dict_set(obj.as.obj->as.inst.fields,name,val); return; } if(is_obj(obj,O_MODULE)){ dict_set(obj.as.obj->as.mod.dict,name,val); return; } runtime_error("cannot set attribute on this object"); }
Value get_index(Value obj,Value idx){
    Value r; if(call_instance_method1(obj,"__getitem__",idx,&r)) return r;
    if(is_obj(obj,O_LIST)){ if(!is_number(idx)) runtime_error("list index must be int"); int64_t i=as_int(idx); List *l=&obj.as.obj->as.list; if(i<0) i+=l->count; if(i<0||i>=l->count) runtime_error("list index out of range"); return l->items[i]; }
    if(is_obj(obj,O_DICT)){ char *k=value_to_cstr(idx); Value v; int ok=dict_get(&obj.as.obj->as.dict,k,&v); free(k); if(!ok) runtime_error("dict key not found"); return v; }
    if(is_obj(obj,O_STRING)){ if(!is_number(idx)) runtime_error("string index must be int"); int64_t i=as_int(idx); String *s=&obj.as.obj->as.str; if(i<0)i+=s->len; if(i<0||i>=s->len) runtime_error("string index out of range"); return stringv_len(&s->s[i],1); }
    runtime_error("indexing unsupported type"); return nonev();
}
void set_index(Value obj,Value idx,Value val){
    Value m; if(get_instance_method(obj,"__setitem__",&m)){ Value argv[2]; argv[0]=idx; argv[1]=val; call_value(m,2,argv); return; }
    if(is_obj(obj,O_LIST)){ if(!is_number(idx)) runtime_error("list index must be int"); int64_t i=as_int(idx); List *l=&obj.as.obj->as.list; if(i<0)i+=l->count; if(i<0||i>=l->count) runtime_error("list index out of range"); l->items[i]=val; return; }
    if(is_obj(obj,O_DICT)){ char *k=value_to_cstr(idx); dict_set(&obj.as.obj->as.dict,k,val); free(k); return; }
    runtime_error("item assignment unsupported type");
}

int same_identity(Value a,Value b){
    if(a.type==V_NONE&&b.type==V_NONE) return 1;
    if(a.type==V_BOOL&&b.type==V_BOOL) return a.as.boolean==b.as.boolean;
    if(a.type==V_OBJ&&b.type==V_OBJ) return a.as.obj==b.as.obj;
    if(a.type==V_NATIVE&&b.type==V_NATIVE) return a.as.native==b.as.native;
    return 0;
}
Value contains_value(Value needle,Value hay){
    Value r; if(call_instance_method1(hay,"__contains__",needle,&r)) return boolv(truthy(r));
    if(is_obj(hay,O_LIST)){ List *l=&hay.as.obj->as.list; for(int i=0;i<l->count;i++) if(val_equal(needle,l->items[i])) return boolv(1); return boolv(0); }
    if(is_obj(hay,O_TUPLE)){ List *l=&hay.as.obj->as.tuple; for(int i=0;i<l->count;i++) if(val_equal(needle,l->items[i])) return boolv(1); return boolv(0); }
    if(is_obj(hay,O_SET)){ List *l=&hay.as.obj->as.set; for(int i=0;i<l->count;i++) if(val_equal(needle,l->items[i])) return boolv(1); return boolv(0); }
    if(is_obj(hay,O_DICT)){ char *k=value_to_cstr(needle); Value tmp; int ok=dict_get(&hay.as.obj->as.dict,k,&tmp); free(k); return boolv(ok); }
    if(is_obj(hay,O_STRING)){ char *n=value_to_cstr(needle); int ok=strstr(hay.as.obj->as.str.s,n)!=NULL; free(n); return boolv(ok); }
    runtime_error("right operand is not container"); return boolv(0);
}
static int norm_bound(int64_t x,int n){ if(x<0) x+=n; if(x<0) x=0; if(x>n) x=n; return (int)x; }
Value get_slice(Value obj,Value startv,Value endv){
    int has_start=startv.type!=V_NONE, has_end=endv.type!=V_NONE;
    if(is_obj(obj,O_LIST)||is_obj(obj,O_TUPLE)){
        List *l=is_obj(obj,O_LIST)?&obj.as.obj->as.list:&obj.as.obj->as.tuple;
        int a=has_start?norm_bound(as_int(startv),l->count):0; int b=has_end?norm_bound(as_int(endv),l->count):l->count; if(b<a)b=a;
        Obj *o=is_obj(obj,O_LIST)?new_list():new_tuple(); for(int i=a;i<b;i++) list_push(is_obj(obj,O_LIST)?&o->as.list:&o->as.tuple,l->items[i]); return objv(o);
    }
    if(is_obj(obj,O_STRING)){
        String *st=&obj.as.obj->as.str; int a=has_start?norm_bound(as_int(startv),st->len):0; int b=has_end?norm_bound(as_int(endv),st->len):st->len; if(b<a)b=a; return stringv_len(st->s+a,b-a);
    }
    runtime_error("slicing unsupported type"); return nonev();
}
