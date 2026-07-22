/* ========================= VM: native builtins + iteration ========================= */

#include "vm.h"
#include "containers.h"

static Value native_len(int argc,Value *argv){ if(argc!=1) runtime_error("len() expects 1 argument"); Value v=argv[0]; Value mr; if(call_instance_method0(v,"__len__",&mr)) return mr; if(is_obj(v,O_STRING)) return intv(v.as.obj->as.str.len); if(is_obj(v,O_LIST)) return intv(v.as.obj->as.list.count); if(is_obj(v,O_TUPLE)) return intv(v.as.obj->as.tuple.count); if(is_obj(v,O_SET)) return intv(v.as.obj->as.set.count); if(is_obj(v,O_DICT)) return intv(v.as.obj->as.dict.count); runtime_error("len() unsupported type"); return nonev(); }
static Value native_range(int argc,Value *argv){
    if(argc<1||argc>3) runtime_error("range() expects 1 to 3 arguments");
    for(int i=0;i<argc;i++) if(!is_number(argv[i])) runtime_error("range() arguments must be numbers");
    int64_t start=0, stop=0, step=1;
    if(argc==1){ stop=as_int(argv[0]); }
    else if(argc==2){ start=as_int(argv[0]); stop=as_int(argv[1]); }
    else { start=as_int(argv[0]); stop=as_int(argv[1]); step=as_int(argv[2]); }
    if(step==0) runtime_error("range() step cannot be zero");
    Obj *o=new_list();
    if(step>0){ for(int64_t i=start;i<stop;i+=step) list_push(&o->as.list,intv(i)); }
    else { for(int64_t i=start;i>stop;i+=step) list_push(&o->as.list,intv(i)); }
    return objv(o);
}
/* input() builtin - reads a line from console */
static Value native_input(int argc,Value *argv){
#if defined(MPY_KOLIBRI)
    if(argc > 0){
        char *prompt = value_to_cstr(argv[0]);
        kol_console_puts(prompt);
        free(prompt);
    }
    char buf[1024];
    kol_console_gets(buf, (int)sizeof(buf));
    return stringv(buf);
#else
    if(argc > 0){
        char *prompt = value_to_cstr(argv[0]);
        printf("%s", prompt);
        free(prompt);
    }
    char buf[1024];
    if(!fgets(buf, (int)sizeof(buf), stdin)){
        raise_exception(exceptionv("RuntimeError","EOF on input",nonev()));
        return nonev();
    }
    size_t len = strlen(buf);
    if(len > 0 && buf[len-1] == '\n') buf[len-1] = 0;
    return stringv(buf);
#endif
}
Native N_INPUT={"input",-1,native_input};

/* sys.syscall(eax, ebx=0, ecx=0, edx=0, esi=0, edi=0) -> [eax,ebx,ecx,edx,esi,edi]
   The low-level KolibriOS gateway. Each argument is an int (a register value)
   or a str (its buffer address is passed -- e.g. an ASCIIZ string for fn 4).
   Returns the six general-purpose registers after `int 0x40`. Raises on any
   non-KolibriOS build (wrong platform). */
static Value native_syscall(int argc,Value *argv){
    if(!mpy_platform_has_syscall()) raise_exception(exceptionv("RuntimeError","sys.syscall is only available on the KolibriOS build (wrong platform)",nonev()));
    if(argc<1||argc>6) runtime_error("syscall() takes 1 to 6 register arguments (eax..edi)");
    uint32_t in[6]={0}, out[6]={0};
    for(int i=0;i<argc;i++){
        Value a=argv[i];
        if(a.type==V_INT) in[i]=(uint32_t)a.as.i;
        else if(a.type==V_BOOL) in[i]=(uint32_t)a.as.boolean;
        else if(is_obj(a,O_STRING)) in[i]=(uint32_t)(uintptr_t)a.as.obj->as.str.s;   /* pass buffer address */
        else runtime_error("syscall() arguments must be int or str");
    }
    mpy_platform_syscall(in,out);
    Obj *o=new_list();
    for(int i=0;i<6;i++) list_push(&o->as.list,intv((int64_t)out[i]));   /* zero-extended */
    return objv(o);
}
Native N_SYSCALL={"syscall",-1,native_syscall};

Native N_LEN={"len",1,native_len};
Native N_RANGE={"range",-1,native_range};
int iterator_next_value(Value top, Value *out){
    if(is_obj(top,O_GENERATOR)){ return generator_next(top.as.obj,out); }
    if(!is_obj(top,O_ITER)) runtime_error("next() expects iterator");
    Iter *it=&top.as.obj->as.iter; Value seq=it->iterable;
    if(is_obj(seq,O_LIST)||is_obj(seq,O_TUPLE)||is_obj(seq,O_SET)){ List *l=is_obj(seq,O_LIST)?&seq.as.obj->as.list:(is_obj(seq,O_TUPLE)?&seq.as.obj->as.tuple:&seq.as.obj->as.set); if(it->index>=l->count) return 0; *out=l->items[it->index++]; return 1; }
    if(is_obj(seq,O_STRING)){ String *st=&seq.as.obj->as.str; if(it->index>=st->len) return 0; *out=stringv_len(&st->s[it->index],1); it->index++; return 1; }
    if(is_obj(seq,O_DICT)){ Dict *d=&seq.as.obj->as.dict; if(it->index>=d->count) return 0; *out=stringv(d->keys[it->index++]); return 1; }
    if(is_obj(seq,O_GENERATOR)){ return generator_next(seq.as.obj,out); }
    Value nr; if(call_instance_method0(seq,"__next__",&nr)){ *out=nr; return nr.type!=V_NONE; }
    runtime_error("object is not iterable"); return 0;
}
static Value native_next(int argc,Value *argv){ if(argc!=1) runtime_error("next() expects 1 argument"); Value out; if(iterator_next_value(argv[0],&out)) return out; raise_exception(exceptionv("StopIteration","",nonev())); return nonev(); }
Value native_iter(int argc,Value *argv){ if(argc!=1) runtime_error("iter() expects 1 argument"); Value itv=argv[0]; Value custom; if(call_instance_method0(itv,"__iter__",&custom)) return custom; if(!(is_obj(itv,O_LIST)||is_obj(itv,O_TUPLE)||is_obj(itv,O_SET)||is_obj(itv,O_DICT)||is_obj(itv,O_STRING)||is_obj(itv,O_GENERATOR))) runtime_error("object is not iterable"); Obj *it=new_obj(O_ITER); it->as.iter.iterable=itv; it->as.iter.index=0; return objv(it); }
Native N_NEXT={"next",1,native_next};
Native N_ITER={"iter",1,native_iter};

/* ========================= Builtins: constructors, math, iteration ========================= */

/* forward declarations */
static Value native_str(int,Value*); static Value native_repr(int,Value*); static Value native_int(int,Value*);
static Value native_float(int,Value*); static Value native_bool(int,Value*); static Value native_list(int,Value*);
static Value native_tuple(int,Value*); static Value native_set(int,Value*); static Value native_dict(int,Value*);
static Value native_abs(int,Value*); static Value native_min(int,Value*); static Value native_max(int,Value*);
static Value native_sum(int,Value*); static Value native_sorted(int,Value*); static Value native_reversed(int,Value*);
static Value native_enumerate(int,Value*); static Value native_zip(int,Value*); static Value native_map(int,Value*);
static Value native_filter(int,Value*); static Value native_type(int,Value*); static Value native_isinstance(int,Value*);
static Value native_ord(int,Value*); static Value native_chr(int,Value*); static Value native_round(int,Value*);
static Value native_any(int,Value*); static Value native_all(int,Value*);
static Value native_super(int,Value*); static Value native_staticmethod(int,Value*); static Value native_classmethod(int,Value*); static Value native_property(int,Value*);

Native N_STR={"str",1,native_str}; Native N_REPR={"repr",1,native_repr}; Native N_INT={"int",-1,native_int};
Native N_FLOAT={"float",-1,native_float}; Native N_BOOL={"bool",-1,native_bool}; Native N_LIST={"list",-1,native_list};
Native N_TUPLE={"tuple",-1,native_tuple}; Native N_SET={"set",-1,native_set}; Native N_DICT={"dict",-1,native_dict};
Native N_ABS={"abs",1,native_abs}; Native N_MIN={"min",-1,native_min}; Native N_MAX={"max",-1,native_max};
Native N_SUM={"sum",-1,native_sum}; Native N_SORTED={"sorted",1,native_sorted}; Native N_REVERSED={"reversed",1,native_reversed};
Native N_ENUMERATE={"enumerate",-1,native_enumerate}; Native N_ZIP={"zip",-1,native_zip}; Native N_MAP={"map",2,native_map};
Native N_FILTER={"filter",2,native_filter}; Native N_TYPE={"type",1,native_type}; Native N_ISINSTANCE={"isinstance",2,native_isinstance};
Native N_ORD={"ord",1,native_ord}; Native N_CHR={"chr",1,native_chr}; Native N_ROUND={"round",-1,native_round};
Native N_ANY={"any",1,native_any}; Native N_ALL={"all",1,native_all};
Native N_SUPER={"super",-1,native_super}; Native N_STATICMETHOD={"staticmethod",1,native_staticmethod}; Native N_CLASSMETHOD={"classmethod",1,native_classmethod}; Native N_PROPERTY={"property",1,native_property};

/* Collect any iterable into a fresh list. */
static void collect_iterable(Value v, List *out){
    Value it=native_iter(1,&v); Value nx;
    while(iterator_next_value(it,&nx)) list_push(out,nx);
}

/* str() with __str__/__repr__ dispatch (shared with OP_PRINT). */
Value builtin_str(Value v){
    Value r;
    if(call_instance_method0(v,"__str__",&r)) return r;
    if(call_instance_method0(v,"__repr__",&r)) return r;
    if(is_obj(v,O_STRING)) return v;
    char *s=value_repr(v,0); Value o=stringv(s); free(s); return o;
}
static Value native_str(int argc,Value*argv){ if(argc==0) return stringv(""); return builtin_str(argv[0]); }
static Value native_repr(int argc,Value*argv){ (void)argc; Value r; if(call_instance_method0(argv[0],"__repr__",&r)) return r; char *s=value_repr(argv[0],1); Value o=stringv(s); free(s); return o; }
/* Hook for value_repr: repr of an instance via __repr__ (or __str__). */
char *mpy_instance_repr(Value v){ Value r; if(call_instance_method0(v,"__repr__",&r)||call_instance_method0(v,"__str__",&r)) return value_to_cstr(r); return NULL; }
static Value native_super(int argc,Value*argv){ (void)argc;(void)argv;
    Frame *fr=&vm.frames[vm.fcount-1]; Function *m=fr->fn;
    if(!m->defining_class||!m->defining_class->base) runtime_error("super(): no base class");
    if(m->arity<1) runtime_error("super(): enclosing method has no self");
    Value self; if(!dict_get(fr->locals,m->params[0],&self)) runtime_error("super(): cannot find self");
    Obj *o=new_obj(O_SUPER); o->as.super.self=self; o->as.super.start=m->defining_class->base; return objv(o);
}
static Value native_staticmethod(int argc,Value*argv){ (void)argc; Obj *o=new_obj(O_METHWRAP); o->as.mw.kind=0; o->as.mw.fn=argv[0]; return objv(o); }
static Value native_classmethod(int argc,Value*argv){ (void)argc; Obj *o=new_obj(O_METHWRAP); o->as.mw.kind=1; o->as.mw.fn=argv[0]; return objv(o); }
static Value native_property(int argc,Value*argv){ (void)argc; Obj *o=new_obj(O_METHWRAP); o->as.mw.kind=2; o->as.mw.fn=argv[0]; return objv(o); }
static Value native_int(int argc,Value*argv){ if(argc==0) return intv(0); Value v=argv[0]; if(v.type==V_INT||v.type==V_BOOL) return intv(as_int(v)); if(v.type==V_FLOAT) return intv((int64_t)v.as.f); if(is_obj(v,O_STRING)){ char *e; long long n=strtoll(v.as.obj->as.str.s,&e,10); return intv(n); } runtime_error("int() argument must be a number or string"); return nonev(); }
static Value native_float(int argc,Value*argv){ if(argc==0) return floatv(0.0); Value v=argv[0]; if(is_number(v)) return floatv(as_double(v)); if(is_obj(v,O_STRING)) return floatv(strtod(v.as.obj->as.str.s,NULL)); runtime_error("float() argument must be a number or string"); return nonev(); }
static Value native_bool(int argc,Value*argv){ if(argc==0) return boolv(0); return boolv(truthy(argv[0])); }
static Value native_list(int argc,Value*argv){ Obj*o=new_list(); if(argc>0) collect_iterable(argv[0],&o->as.list); return objv(o); }
static Value native_tuple(int argc,Value*argv){ Obj*o=new_tuple(); if(argc>0) collect_iterable(argv[0],&o->as.tuple); return objv(o); }
static Value native_set(int argc,Value*argv){ Obj*o=new_set(); if(argc>0){ List tmp; memset(&tmp,0,sizeof(tmp)); collect_iterable(argv[0],&tmp); for(int i=0;i<tmp.count;i++) set_add(&o->as.set,tmp.items[i]); } return objv(o); }
static Value native_dict(int argc,Value*argv){ Obj*o=new_dict_obj(); if(argc>0){ List pairs; memset(&pairs,0,sizeof(pairs)); collect_iterable(argv[0],&pairs); for(int i=0;i<pairs.count;i++){ Value pr=pairs.items[i]; List *pl=is_obj(pr,O_LIST)?&pr.as.obj->as.list:(is_obj(pr,O_TUPLE)?&pr.as.obj->as.tuple:NULL); if(!pl||pl->count!=2) runtime_error("dictionary update sequence element is not a pair"); char *k=value_to_cstr(pl->items[0]); dict_set(&o->as.dict,k,pl->items[1]); free(k); } } return objv(o); }
static Value native_abs(int argc,Value*argv){ (void)argc; Value v=argv[0]; if(v.type==V_FLOAT) return floatv(v.as.f<0?-v.as.f:v.as.f); if(is_number(v)){ int64_t i=as_int(v); return intv(i<0?-i:i); } runtime_error("abs() requires a number"); return nonev(); }
static Value native_min(int argc,Value*argv){ List tmp; memset(&tmp,0,sizeof(tmp)); Value *xs; int n; if(argc==1){ collect_iterable(argv[0],&tmp); xs=tmp.items; n=tmp.count; } else { xs=argv; n=argc; } if(n==0) runtime_error("min() arg is an empty sequence"); Value best=xs[0]; for(int i=1;i<n;i++) if(truthy(compare(xs[i],best,OP_LT))) best=xs[i]; return best; }
static Value native_max(int argc,Value*argv){ List tmp; memset(&tmp,0,sizeof(tmp)); Value *xs; int n; if(argc==1){ collect_iterable(argv[0],&tmp); xs=tmp.items; n=tmp.count; } else { xs=argv; n=argc; } if(n==0) runtime_error("max() arg is an empty sequence"); Value best=xs[0]; for(int i=1;i<n;i++) if(truthy(compare(xs[i],best,OP_GT))) best=xs[i]; return best; }
static Value native_sum(int argc,Value*argv){ if(argc<1||argc>2) runtime_error("sum() expects 1 or 2 arguments"); List tmp; memset(&tmp,0,sizeof(tmp)); collect_iterable(argv[0],&tmp); Value acc=argc==2?argv[1]:intv(0); for(int i=0;i<tmp.count;i++) acc=binary_add(acc,tmp.items[i]); return acc; }
static Value native_sorted(int argc,Value*argv){ (void)argc; Obj*o=new_list(); collect_iterable(argv[0],&o->as.list); List *l=&o->as.list; for(int i=1;i<l->count;i++){ Value key=l->items[i]; int j=i-1; while(j>=0 && truthy(compare(key,l->items[j],OP_LT))){ l->items[j+1]=l->items[j]; j--; } l->items[j+1]=key; } return objv(o); }
static Value native_reversed(int argc,Value*argv){ (void)argc; List tmp; memset(&tmp,0,sizeof(tmp)); collect_iterable(argv[0],&tmp); Obj*o=new_list(); for(int i=tmp.count-1;i>=0;i--) list_push(&o->as.list,tmp.items[i]); return objv(o); }
static Value native_enumerate(int argc,Value*argv){ int64_t start=argc>=2?as_int(argv[1]):0; List tmp; memset(&tmp,0,sizeof(tmp)); collect_iterable(argv[0],&tmp); Obj*o=new_list(); for(int i=0;i<tmp.count;i++){ Obj*pr=new_tuple(); list_push(&pr->as.tuple,intv(start+i)); list_push(&pr->as.tuple,tmp.items[i]); list_push(&o->as.list,objv(pr)); } return objv(o); }
static Value native_zip(int argc,Value*argv){ if(argc==0) return objv(new_list()); List *cols=(List*)xmalloc(sizeof(List)*(size_t)argc); int minlen=-1; for(int i=0;i<argc;i++){ memset(&cols[i],0,sizeof(List)); collect_iterable(argv[i],&cols[i]); if(minlen<0||cols[i].count<minlen) minlen=cols[i].count; } Obj*o=new_list(); for(int r=0;r<minlen;r++){ Obj*pr=new_tuple(); for(int cI=0;cI<argc;cI++) list_push(&pr->as.tuple,cols[cI].items[r]); list_push(&o->as.list,objv(pr)); } free(cols); return objv(o); }
static Value native_map(int argc,Value*argv){ (void)argc; Value fn=argv[0]; List tmp; memset(&tmp,0,sizeof(tmp)); collect_iterable(argv[1],&tmp); Obj*o=new_list(); for(int i=0;i<tmp.count;i++){ Value a=tmp.items[i]; list_push(&o->as.list,call_value(fn,1,&a)); } return objv(o); }
static Value native_filter(int argc,Value*argv){ (void)argc; Value fn=argv[0]; List tmp; memset(&tmp,0,sizeof(tmp)); collect_iterable(argv[1],&tmp); Obj*o=new_list(); for(int i=0;i<tmp.count;i++){ Value a=tmp.items[i]; int keep; if(fn.type==V_NONE) keep=truthy(a); else keep=truthy(call_value(fn,1,&a)); if(keep) list_push(&o->as.list,a); } return objv(o); }
static Value native_type(int argc,Value*argv){ (void)argc; return stringv(value_type_name(argv[0])); }
static int isinstance_one(Value x, Value t){
    if(t.type==V_NATIVE){ Native *n=t.as.native;
        if(n==&N_INT) return x.type==V_INT||x.type==V_BOOL; if(n==&N_FLOAT) return x.type==V_FLOAT; if(n==&N_BOOL) return x.type==V_BOOL;
        if(n==&N_STR) return is_obj(x,O_STRING); if(n==&N_LIST) return is_obj(x,O_LIST); if(n==&N_TUPLE) return is_obj(x,O_TUPLE);
        if(n==&N_SET) return is_obj(x,O_SET); if(n==&N_DICT) return is_obj(x,O_DICT); return 0; }
    if(is_obj(t,O_CLASS)){ if(!is_obj(x,O_INSTANCE)) return 0; for(Class *k=x.as.obj->as.inst.klass;k;k=k->base) if(k==&t.as.obj->as.klass) return 1; return 0; }
    return 0;
}
static Value native_isinstance(int argc,Value*argv){ (void)argc; Value x=argv[0],t=argv[1]; if(is_obj(t,O_TUPLE)){ List *l=&t.as.obj->as.tuple; for(int i=0;i<l->count;i++) if(isinstance_one(x,l->items[i])) return boolv(1); return boolv(0); } return boolv(isinstance_one(x,t)); }
static Value native_ord(int argc,Value*argv){ (void)argc; if(!is_obj(argv[0],O_STRING)||argv[0].as.obj->as.str.len!=1) runtime_error("ord() expects a character"); return intv((unsigned char)argv[0].as.obj->as.str.s[0]); }
static Value native_chr(int argc,Value*argv){ (void)argc; char c=(char)as_int(argv[0]); return stringv_len(&c,1); }
static Value native_round(int argc,Value*argv){ double x=as_double(argv[0]); if(argc>=2){ int nd=(int)as_int(argv[1]); double p=1; for(int i=0;i<(nd<0?-nd:nd);i++) p*=10; double sx=(nd>=0)?x*p:x/p; int64_t fl=(int64_t)sx; double fr=sx-fl; if(sx<0&&fr!=0){fl--;fr=sx-fl;} int64_t rr; if(fr<0.5) rr=fl; else if(fr>0.5) rr=fl+1; else rr=(fl%2==0)?fl:fl+1; double res=(nd>=0)?(double)rr/p:(double)rr*p; return floatv(res); } int64_t fl=(int64_t)x; double fr=x-fl; if(x<0&&fr!=0){fl--;fr=x-fl;} int64_t rr; if(fr<0.5) rr=fl; else if(fr>0.5) rr=fl+1; else rr=(fl%2==0)?fl:fl+1; return intv(rr); }
static Value native_any(int argc,Value*argv){ (void)argc; List tmp; memset(&tmp,0,sizeof(tmp)); collect_iterable(argv[0],&tmp); for(int i=0;i<tmp.count;i++) if(truthy(tmp.items[i])) return boolv(1); return boolv(0); }
static Value native_all(int argc,Value*argv){ (void)argc; List tmp; memset(&tmp,0,sizeof(tmp)); collect_iterable(argv[0],&tmp); for(int i=0;i<tmp.count;i++) if(!truthy(tmp.items[i])) return boolv(0); return boolv(1); }
