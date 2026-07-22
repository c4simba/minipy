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

Native N_LEN={"len",1,native_len};
Native N_RANGE={"range",-1,native_range};
int iterator_next_value(Value top, Value *out){
    if(is_obj(top,O_GENERATOR)){ return generator_next(top.as.obj,out); }
    if(!is_obj(top,O_ITER)) runtime_error("next() expects iterator");
    Iter *it=&top.as.obj->as.iter; Value seq=it->iterable;
    if(is_obj(seq,O_LIST)){ List *l=&seq.as.obj->as.list; if(it->index>=l->count) return 0; *out=l->items[it->index++]; return 1; }
    if(is_obj(seq,O_STRING)){ String *st=&seq.as.obj->as.str; if(it->index>=st->len) return 0; *out=stringv_len(&st->s[it->index],1); it->index++; return 1; }
    if(is_obj(seq,O_DICT)){ Dict *d=&seq.as.obj->as.dict; if(it->index>=d->count) return 0; *out=stringv(d->keys[it->index++]); return 1; }
    if(is_obj(seq,O_GENERATOR)){ return generator_next(seq.as.obj,out); }
    Value nr; if(call_instance_method0(seq,"__next__",&nr)){ *out=nr; return nr.type!=V_NONE; }
    runtime_error("object is not iterable"); return 0;
}
static Value native_next(int argc,Value *argv){ if(argc!=1) runtime_error("next() expects 1 argument"); Value out; if(iterator_next_value(argv[0],&out)) return out; raise_exception(exceptionv("StopIteration","",nonev())); return nonev(); }
Value native_iter(int argc,Value *argv){ if(argc!=1) runtime_error("iter() expects 1 argument"); Value itv=argv[0]; Value custom; if(call_instance_method0(itv,"__iter__",&custom)) return custom; if(!(is_obj(itv,O_LIST)||is_obj(itv,O_DICT)||is_obj(itv,O_STRING)||is_obj(itv,O_GENERATOR))) runtime_error("object is not iterable"); Obj *it=new_obj(O_ITER); it->as.iter.iterable=itv; it->as.iter.index=0; return objv(it); }
Native N_NEXT={"next",1,native_next};
Native N_ITER={"iter",1,native_iter};
