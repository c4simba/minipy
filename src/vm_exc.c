/* ========================= VM: exception machinery ========================= */

#include "vm.h"

/* An instance of a class derived from a built-in exception class. */
int is_exc_instance(Value v){
    if(!is_obj(v,O_INSTANCE)) return 0;
    for(Class *k=v.as.obj->as.inst.klass;k;k=k->base) if(is_builtin_exc_name(k->name) && !k->base) return 1;
    return 0;
}
static const char *exception_type_name(Value v){
    if(is_obj(v,O_EXCEPTION)) return v.as.obj->as.exc.type_name;
    if(is_exc_instance(v)) return v.as.obj->as.inst.klass->name;
    return "RuntimeError";
}
static char *exception_message(Value v){
    if(is_obj(v,O_EXCEPTION)) return xstrdup2(v.as.obj->as.exc.message);
    if(is_exc_instance(v)) return value_to_cstr(builtin_str(v));
    return value_to_cstr(v);
}
void print_traceback(Value ex){
    char *msg=exception_message(ex);
    fprintf(stderr,"%s: %s\n",exception_type_name(ex),msg);
    free(msg);
    fprintf(stderr,"Traceback (most recent call last):\n");
    /* Printed from the snapshot captured at raise time: the live frames unwind
       as the exception propagates, so vm.frames is no longer the call stack. */
    for(int i=0;i<vm.tb_count;i++){ Function *f=vm.tb[i].fn; int ip=vm.tb[i].ip>0?vm.tb[i].ip-1:0; int line=f->chunk->line[ip]; fprintf(stderr,"  line %d, in %s\n",line,f->name); }
}
Value normalize_exception(Value v){
    if(is_obj(v,O_EXCEPTION)) return v;
    if(is_exc_instance(v)) return v;                  /* raised as itself: `except ItsClass as e` gets it */
    if(is_obj(v,O_CLASS) && !is_builtin_exc_name(v.as.obj->as.klass.name)){   /* raise MyError: an instance */
        Value inst=call_value(v,0,NULL); if(is_exc_instance(inst)) return inst; }
    if(is_obj(v,O_CLASS)){
        const char *tn=v.as.obj->as.klass.name;
        if(strcmp(tn,"BaseException")==0||strcmp(tn,"RuntimeError")==0||strcmp(tn,"StopIteration")==0) return exceptionv(tn,"",v);
    }
    char *m=value_to_cstr(v);
    Value ex=exceptionv("RuntimeError",m,v);
    free(m);
    return ex;
}
void raise_exception(Value ex){
    vm.pending_exception=normalize_exception(ex);
    /* Snapshot the call stack now, while it is intact: propagation unwinds
       vm.frames one at a time, so a later traceback could not reconstruct it. */
    vm.tb_count=0;
    for(int i=vm.fcount-1;i>=0 && vm.tb_count<256;i--){ vm.tb[vm.tb_count].fn=vm.frames[i].fn; vm.tb[vm.tb_count].ip=vm.frames[i].ip; vm.tb_count++; }
    if(vm.exc_depth>0) longjmp(vm.exc_jumps[vm.exc_depth-1].buf,1);
    char *m=exception_message(vm.pending_exception);
    free(vm.error_msg); vm.error_msg=m;
    longjmp(vm.panic,1);
}
void runtime_error(const char *msg){
    raise_exception(exceptionv("RuntimeError",msg?msg:"runtime error",nonev()));
}
void raise_named(const char *type,const char *msg){
    raise_exception(exceptionv(type,msg?msg:"",nonev()));
}
int dispatch_pending_exception(void){
    for(int i=vm.fcount-1;i>=0;i--){
        Frame *f=&vm.frames[i];
        if(f->hcount>0){
            Handler h=f->handlers[--f->hcount];
            vm.fcount=i+1;
            vm.sp=h.sp;
            push(vm.pending_exception);
            f->ip=h.ip;
            return 1;
        }
    }
    char *m=exception_message(vm.pending_exception);
    free(vm.error_msg); vm.error_msg=m;
    longjmp(vm.panic,1);
    return 0;
}
