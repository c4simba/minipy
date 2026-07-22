/* ========================= VM: exception machinery ========================= */

#include "vm.h"

static const char *exception_type_name(Value v){
    if(is_obj(v,O_EXCEPTION)) return v.as.obj->as.exc.type_name;
    return "RuntimeError";
}
static char *exception_message(Value v){
    if(is_obj(v,O_EXCEPTION)) return xstrdup2(v.as.obj->as.exc.message);
    return value_to_cstr(v);
}
void print_traceback(Value ex){
    char *msg=exception_message(ex);
    fprintf(stderr,"%s: %s\n",exception_type_name(ex),msg);
    free(msg);
    fprintf(stderr,"Traceback (most recent call last):\n");
    for(int i=vm.fcount-1;i>=0;i--){ Frame *f=&vm.frames[i]; int ip=f->ip>0?f->ip-1:0; int line=f->fn->chunk->line[ip]; fprintf(stderr,"  line %d, in %s\n",line,f->fn->name); }
}
Value normalize_exception(Value v){
    if(is_obj(v,O_EXCEPTION)) return v;
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
