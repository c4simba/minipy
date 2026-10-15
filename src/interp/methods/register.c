/* ========================= Interpreter, methods: registration =========================
   Argument helpers of the methods; mp_methods_init. */

#include "methods.h"

int npos_of(int argc, TupleObj *kw){ return argc-(kw?(int)kw->len:0); }
Value kwarg(int argc, Value *argv, TupleObj *kw, const char *name, Value dflt){
    if(!kw) return dflt;
    int np=npos_of(argc,kw);
    for(int64_t i=0;i<kw->len;i++) if(!strcmp(mp_cstr(kw->items[i]),name)) return argv[np+i];
    return dflt;
}
void nargs(const char *name, int argc, int lo, int hi){
    int n=argc-1;
    if(n<lo || n>hi){
        if(lo==hi && lo==0) mp_raise_t(E_TypeError,"%s() takes no arguments (%d given)",name,n);
        if(lo==hi) mp_raise_t(E_TypeError,"%s() takes exactly one argument (%d given)",name,n);
        if(n<lo) mp_raise_t(E_TypeError,"%s expected at least %d argument%s, got %d",name,lo,lo==1?"":"s",n);
        mp_raise_t(E_TypeError,"%s expected at most %d argument%s, got %d",name,hi,hi==1?"":"s",n);
    }
}
StrObj *S(Value v){ return AS_STR(v); }
StrObj *str_arg(Value v, const char *what){
    if(!IS_STR(v)) mp_raise_t(E_TypeError,"%s must be str, not %s",what,mp_type_name(v));
    return AS_STR(v);
}

/* ---------------------------------------------------------------- registration */
void mp_methods_init(void){                 /* (the order they are added in: the types' dicts) */
    mp_str_methods_init();
    mp_bytes_methods_init();
    mp_container_methods_init();
    mp_number_methods_init();
    mp_object_methods_init();
}
