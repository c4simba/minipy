/* ========================= VM: dispatch loop + calling convention ========================= */

#include "vm.h"
#include "containers.h"
#include "compiler.h"
#include "fs.h"

VM vm;

void push(Value v){ if(vm.sp>=4096) die("stack overflow"); vm.stack[vm.sp++]=v; }
Value popv(void){ if(vm.sp<=0) die("stack underflow"); return vm.stack[--vm.sp]; }

static Value call_value_ex(Value callee, List *pos, Dict *kw);

/* Bind call arguments into `locals`, honoring positional params, keyword args,
   defaults, *args (collect extra positionals into a tuple) and **kwargs
   (collect extra keywords into a dict). Raises on arity/keyword errors. */
static void bind_arguments(Function *fn, Value *pos, int npos, Dict *kw, Dict *locals){
    int star=fn->star_index, dstar=fn->dstar_index;
    int nreg=fn->arity-(star>=0?1:0)-(dstar>=0?1:0);
    int fb[128]; for(int i=0;i<nreg && i<128;i++) fb[i]=0;
    int i=0;
    for(; i<npos && i<nreg; i++){ dict_set(locals,fn->params[i],pos[i]); if(i<128) fb[i]=1; }
    if(star>=0){ Obj *t=new_tuple(); for(int j=nreg;j<npos;j++) list_push(&t->as.tuple,pos[j]); dict_set(locals,fn->params[star],objv(t)); }
    else if(npos>nreg) runtime_error("too many positional arguments");
    Dict extra; memset(&extra,0,sizeof(extra));
    if(kw){ for(int ki=0;ki<kw->count;ki++){ char *key=kw->keys[ki]; Value val=kw->vals[ki];
        int r=-1; for(int x=0;x<nreg;x++) if(strcmp(fn->params[x],key)==0){ r=x; break; }
        if(r>=0){ if(r<128 && fb[r]) runtime_error("multiple values for argument"); dict_set(locals,key,val); if(r<128) fb[r]=1; }
        else if(dstar>=0) dict_set(&extra,key,val);
        else runtime_error("unexpected keyword argument"); } }
    if(dstar>=0){ Obj *d=new_dict_obj(); d->as.dict=extra; dict_set(locals,fn->params[dstar],objv(d)); }
    for(int r=0;r<nreg;r++){ if(r<128 && fb[r]) continue; int di=r-(nreg-fn->default_count); if(di>=0 && di<fn->default_count) dict_set(locals,fn->params[r],fn->defaults[di]); else runtime_error("missing required argument"); }
}
static Value run_prepared(Function *fn, Dict *locals, Obj *gen_obj){
    if(vm.fcount>=256) runtime_error("call stack overflow");
    Frame *fr=&vm.frames[vm.fcount++]; fr->fn=fn;
    if(gen_obj){ fr->ip=gen_obj->as.gen.ip; fr->locals=gen_obj->as.gen.locals; }
    else { fr->ip=0; fr->hcount=0; fr->locals=locals; }
    Chunk *c=fn->chunk;
    int exc_slot=vm.exc_depth++;
    if(exc_slot>=256) runtime_error("exception jump stack overflow");
    if(setjmp(vm.exc_jumps[exc_slot].buf)!=0){
        vm.exc_depth=exc_slot+1;
        if(!dispatch_pending_exception()){ vm.exc_depth=exc_slot; return nonev(); }
        fr=&vm.frames[vm.fcount-1];
        fn=fr->fn;
        c=fr->fn->chunk;
    }
    for(;;){ Op op=(Op)c->code[fr->ip++];
        switch(op){
            case OP_CONST: push(c->consts[c->code[fr->ip++]]); break; case OP_NONE: push(nonev()); break; case OP_TRUE: push(boolv(1)); break; case OP_FALSE: push(boolv(0)); break;
            case OP_LOAD:{ Value namev=c->consts[c->code[fr->ip++]]; char *name=namev.as.obj->as.str.s; Value v; if(function_declares(fn->global_names,fn->global_count,name)){ if(dict_get(fn->globals,name,&v)||dict_get(vm.builtins,name,&v)) push(v); else { char b[256]; snprintf(b,sizeof(b),"name '%s' is not defined",name); runtime_error(b); } } else if(dict_get(fr->locals,name,&v)||dict_get(fn->globals,name,&v)||dict_get(vm.builtins,name,&v)) push(v); else { char b[256]; snprintf(b,sizeof(b),"name '%s' is not defined",name); runtime_error(b); } break; }
            case OP_STORE:{ Value namev=c->consts[c->code[fr->ip++]]; char *name=namev.as.obj->as.str.s; Value v=popv(); if(fn->store_globals||function_declares(fn->global_names,fn->global_count,name)) dict_set(fn->globals,name,v); else { if(function_declares(fn->nonlocal_names,fn->nonlocal_count,name) && fn->closure) dict_set(fn->closure,name,v); dict_set(fr->locals,name,v); } break; }
            case OP_STORE_GLOBAL:{ Value namev=c->consts[c->code[fr->ip++]]; dict_set(fn->globals,namev.as.obj->as.str.s,popv()); break; }
            case OP_POP: popv(); break;
            case OP_ADD:{ Value b=popv(),a=popv(); push(binary_add(a,b)); break; } case OP_SUB: case OP_MUL: case OP_DIV: case OP_FLOOR_DIV: case OP_MOD: case OP_POWER: case OP_BITAND: case OP_BITOR: case OP_BITXOR: case OP_SHL: case OP_SHR:{ Value b=popv(),a=popv(); push(binary_num(a,b,op)); break; } case OP_NEG:{ Value a=popv(); if(!is_number(a)) runtime_error("bad unary -"); if(a.type==V_FLOAT) push(floatv(-a.as.f)); else push(intv(-as_int(a))); break; } case OP_BITNOT:{ Value a=popv(); if(a.type==V_FLOAT||!is_number(a)) runtime_error("bad unary ~"); push(intv(~as_int(a))); break; }
            case OP_EQ:{Value b=popv(),a=popv(); push(boolv(val_equal(a,b)));break;} case OP_NE:{Value b=popv(),a=popv(); push(boolv(!val_equal(a,b)));break;} case OP_LT:case OP_LE:case OP_GT:case OP_GE:{Value b=popv(),a=popv(); push(compare(a,b,op));break;} case OP_IS:{Value b=popv(),a=popv(); push(boolv(same_identity(a,b)));break;} case OP_IS_NOT:{Value b=popv(),a=popv(); push(boolv(!same_identity(a,b)));break;} case OP_CONTAINS:{Value hay=popv(),needle=popv(); push(contains_value(needle,hay));break;}
            case OP_JUMP: fr->ip=c->code[fr->ip]; break;
            case OP_JUMP_IF_FALSE:{ int target=c->code[fr->ip++]; Value cond=popv(); if(!truthy(cond)) fr->ip=target; break; }
            case OP_JUMP_IF_FALSE_KEEP:{ int target=c->code[fr->ip++]; Value cond=vm.stack[vm.sp-1]; if(!truthy(cond)) fr->ip=target; break; }
            case OP_JUMP_IF_TRUE_KEEP:{ int target=c->code[fr->ip++]; Value cond=vm.stack[vm.sp-1]; if(truthy(cond)) fr->ip=target; break; }
            case OP_PRINT: print_value(popv()); printf("\n"); break;
            case OP_NOT:{ Value a=popv(); push(boolv(!truthy(a))); break; }
            case OP_RAISE:{ Value a=popv(); raise_exception(a); break; }
            case OP_SETUP_EXCEPT:{ int target=c->code[fr->ip++]; if(fr->hcount>=32) runtime_error("too many exception handlers"); fr->handlers[fr->hcount].ip=target; fr->handlers[fr->hcount].sp=vm.sp; fr->hcount++; break; }
            case OP_POP_EXCEPT:{ if(fr->hcount>0) fr->hcount--; break; }
            case OP_ITER:{ Value itv=popv(); push(native_iter(1,&itv)); break; }
            case OP_FOR_NEXT:{ int target=c->code[fr->ip++]; Value top=vm.stack[vm.sp-1]; Value next=nonev(); if(!iterator_next_value(top,&next)){ popv(); fr->ip=target; } else push(next); break; }
            case OP_CALL:{ int argc2=c->code[fr->ip++]; Value *argv=&vm.stack[vm.sp-argc2]; Value cal=vm.stack[vm.sp-argc2-1]; Value r=call_value(cal,argc2,argv); vm.sp-=argc2+1; push(r); fr=&vm.frames[vm.fcount-1]; fn=fr->fn; c=fr->fn->chunk; break; }
            case OP_LIST_APPEND:{ Value v=popv(); list_push(&vm.stack[vm.sp-1].as.obj->as.list,v); break; }
            case OP_LIST_EXTEND:{ Value it=popv(); Value lst=vm.stack[vm.sp-1]; Value iter=native_iter(1,&it); Value nx; while(iterator_next_value(iter,&nx)) list_push(&lst.as.obj->as.list,nx); break; }
            case OP_DICT_SETNAME:{ Value namev=c->consts[c->code[fr->ip++]]; Value v=popv(); dict_set(&vm.stack[vm.sp-1].as.obj->as.dict,namev.as.obj->as.str.s,v); break; }
            case OP_DICT_MERGE:{ Value src=popv(); if(!is_obj(src,O_DICT)) runtime_error("argument after ** must be a dict"); Dict *sd=&src.as.obj->as.dict; Dict *dd=&vm.stack[vm.sp-1].as.obj->as.dict; for(int i=0;i<sd->count;i++) dict_set(dd,sd->keys[i],sd->vals[i]); break; }
            case OP_CALL_EX:{ Value kw=popv(),pos=popv(),cal=popv(); Value r=call_value_ex(cal,&pos.as.obj->as.list,&kw.as.obj->as.dict); push(r); fr=&vm.frames[vm.fcount-1]; fn=fr->fn; c=fr->fn->chunk; break; }
            case OP_YIELD:{ if(!gen_obj) runtime_error("yield outside generator"); Value r=popv(); gen_obj->as.gen.ip=fr->ip; gen_obj->as.gen.locals=fr->locals; vm.fcount--; vm.exc_depth=exc_slot; return r; }
            case OP_RETURN:{ Value r=popv(); if(gen_obj) gen_obj->as.gen.done=1; vm.fcount--; vm.exc_depth=exc_slot; return r; }
            case OP_DUP:{ Value v=vm.stack[vm.sp-1]; push(v); break; }
            case OP_DUP2:{ Value a=vm.stack[vm.sp-2],b=vm.stack[vm.sp-1]; push(a); push(b); break; }
            case OP_UNPACK:{ int n=c->code[fr->ip++]; Value seq=popv();
                if(is_obj(seq,O_LIST)||is_obj(seq,O_TUPLE)){ List *l=is_obj(seq,O_LIST)?&seq.as.obj->as.list:&seq.as.obj->as.tuple; if(l->count!=n) runtime_error("wrong number of values to unpack"); for(int i=n-1;i>=0;i--) push(l->items[i]); }
                else if(is_obj(seq,O_STRING)){ String *s=&seq.as.obj->as.str; if(s->len!=n) runtime_error("wrong number of values to unpack"); for(int i=n-1;i>=0;i--) push(stringv_len(&s->s[i],1)); }
                else runtime_error("cannot unpack non-sequence"); break; }
            case OP_MAKE_LIST:{ int n=c->code[fr->ip++]; Obj *o=new_list(); for(int i=0;i<n;i++) list_push(&o->as.list,vm.stack[vm.sp-n+i]); vm.sp-=n; push(objv(o)); break; }
            case OP_MAKE_TUPLE:{ int n=c->code[fr->ip++]; Obj *o=new_tuple(); for(int i=0;i<n;i++) list_push(&o->as.tuple,vm.stack[vm.sp-n+i]); vm.sp-=n; push(objv(o)); break; }
            case OP_MAKE_SET:{ int n=c->code[fr->ip++]; Obj *o=new_set(); for(int i=0;i<n;i++) set_add(&o->as.set,vm.stack[vm.sp-n+i]); vm.sp-=n; push(objv(o)); break; }
            case OP_MAKE_DICT:{ int n=c->code[fr->ip++]; Obj *o=new_dict_obj(); for(int i=0;i<n;i++){ Value val=popv(); Value key=popv(); char *ks=value_to_cstr(key); dict_set(&o->as.dict,ks,val); free(ks); } push(objv(o)); break; }
            case OP_GET_INDEX:{ Value idx=popv(),obj=popv(); push(get_index(obj,idx)); break; } case OP_GET_SLICE:{ Value endv=popv(),startv=popv(),obj=popv(); push(get_slice(obj,startv,endv)); break; } case OP_SET_INDEX:{ Value val=popv(),idx=popv(),obj=popv(); set_index(obj,idx,val); push(val); break; }
            case OP_GET_ATTR:{ Value namev=c->consts[c->code[fr->ip++]]; Value obj=popv(); push(get_attr(obj,namev.as.obj->as.str.s)); break; } case OP_SET_ATTR:{ Value namev=c->consts[c->code[fr->ip++]]; Value val=popv(),obj=popv(); set_attr(obj,namev.as.obj->as.str.s,val); push(val); break; }
            case OP_DEF:{ Value fv=c->consts[c->code[fr->ip++]]; Function *tmpl=&fv.as.obj->as.fn; Function *nf=clone_function(tmpl,fr->locals); if(tmpl->default_count>0){ nf->defaults=(Value*)xmalloc(sizeof(Value)*(size_t)tmpl->default_count); for(int i=tmpl->default_count-1;i>=0;i--) nf->defaults[i]=popv(); } push(objv(nf->owner)); break; }
            case OP_CLASS:{ Value bodyv=c->consts[c->code[fr->ip++]]; Value namev=c->consts[c->code[fr->ip++]]; Function *body=&bodyv.as.obj->as.fn; Dict *oldg=body->globals; Dict *classdict=dict_new(); body->globals=classdict; run_function(body,0,NULL); body->globals=oldg; Obj *co=new_obj(O_CLASS); co->as.klass.name=xstrdup2(namev.as.obj->as.str.s); co->as.klass.methods=classdict; push(objv(co)); fr=&vm.frames[vm.fcount-1]; fn=fr->fn; c=fr->fn->chunk; break; }
            case OP_IMPORT:{ Value namev=c->consts[c->code[fr->ip++]]; char *mn=namev.as.obj->as.str.s; Value modv; if(dict_get(vm.modules,mn,&modv)){ push(modv); break; } char *path=mpy_fs_module_path(fn->module_dir,mn); char *err=NULL; char *src=mpy_fs_try_read_file(path,&err); if(!src){ Value ex=exceptionv("RuntimeError",err?err:"cannot import module",nonev()); free(err); free(path); raise_exception(ex); break; } char *dir=mpy_fs_dirname(path); Dict *g=dict_new(); Function *mf=compile_source(src,mn,dir,g); modv=objv(new_module(mn,g)); dict_set(vm.modules,mn,modv); run_function(mf,0,NULL); push(modv); free(src); free(dir); free(path); fr=&vm.frames[vm.fcount-1]; fn=fr->fn; c=fr->fn->chunk; break; }
        }
    }
}
static Value run_function_ex(Function *fn,int argc,Value *args, Obj *gen_obj){
    Dict *locals=NULL;
    if(!gen_obj){ locals=dict_clone(fn->closure); bind_arguments(fn,args,argc,NULL,locals); }
    return run_prepared(fn,locals,gen_obj);
}
Value run_function(Function *fn,int argc,Value *args){ return run_function_ex(fn,argc,args,NULL); }

static Obj *new_generator_bind(Function *fn, Value *pos, int npos, Dict *kw){
    Obj *g=new_obj(O_GENERATOR); g->as.gen.fn=fn; g->as.gen.locals=dict_clone(fn->closure); g->as.gen.ip=0; g->as.gen.done=0;
    bind_arguments(fn,pos,npos,kw,g->as.gen.locals);
    return g;
}
static Obj *new_generator(Function *fn,int argc,Value *args){ return new_generator_bind(fn,args,argc,NULL); }
int generator_next(Obj *g, Value *out){
    if(g->as.gen.done) return 0;
    Value r=run_function_ex(g->as.gen.fn,0,NULL,g);
    if(g->as.gen.done) return 0;
    *out=r;
    return 1;
}
Value call_value(Value callee,int argc,Value *args){
    if(callee.type==V_NATIVE) return callee.as.native->fn(argc,args);
    if(is_obj(callee,O_FUNCTION)){ Function *fn=&callee.as.obj->as.fn; if(fn->is_generator) return objv(new_generator(fn,argc,args)); return run_function(fn,argc,args); }
    if(is_obj(callee,O_BOUND_METHOD)){ BoundMethod *bm=&callee.as.obj->as.bm; Value *argv=(Value*)xmalloc(sizeof(Value)*(size_t)(argc+1)); argv[0]=bm->receiver; for(int i=0;i<argc;i++) argv[i+1]=args[i]; Value r; if(bm->fn->is_generator) r=objv(new_generator(bm->fn,argc+1,argv)); else r=run_function(bm->fn,argc+1,argv); free(argv); return r; }
    if(is_obj(callee,O_BOUND_NATIVE)){ BoundNative *bn=&callee.as.obj->as.bn; if(strcmp(bn->name,"append")==0){ if(argc!=1) runtime_error("append() expects 1 arg"); list_push(&bn->receiver.as.obj->as.list,args[0]); return nonev(); } if(strcmp(bn->name,"add")==0){ if(argc!=1) runtime_error("add() expects 1 arg"); set_add(&bn->receiver.as.obj->as.set,args[0]); return nonev(); } }
    if(is_obj(callee,O_CLASS)){ Class *kl=&callee.as.obj->as.klass; if(strcmp(kl->name,"BaseException")==0||strcmp(kl->name,"RuntimeError")==0||strcmp(kl->name,"StopIteration")==0){ if(argc>1) runtime_error("exception constructor expects 0 or 1 argument"); char *m=argc==1?value_to_cstr(args[0]):xstrdup2(""); Value ex=exceptionv(kl->name,m,argc==1?args[0]:nonev()); free(m); return ex; } Obj *in=new_obj(O_INSTANCE); in->as.inst.klass=kl; in->as.inst.fields=dict_new(); Value self=objv(in); Value init; if(dict_get(kl->methods,"__init__",&init)){ Value *argv=(Value*)xmalloc(sizeof(Value)*(size_t)(argc+1)); argv[0]=self; for(int i=0;i<argc;i++) argv[i+1]=args[i]; run_function(&init.as.obj->as.fn,argc+1,argv); free(argv); } else if(argc!=0) runtime_error("class constructor takes no args"); return self; }
    runtime_error("object is not callable"); return nonev();
}
/* Call with an explicit positional list and keyword dict (OP_CALL_EX). */
static Value call_value_ex(Value callee, List *pos, Dict *kw){
    if(callee.type==V_NATIVE){ if(kw->count) runtime_error("this function takes no keyword arguments"); return callee.as.native->fn(pos->count,pos->items); }
    if(is_obj(callee,O_FUNCTION)){ Function *fn=&callee.as.obj->as.fn; if(fn->is_generator) return objv(new_generator_bind(fn,pos->items,pos->count,kw)); Dict *locals=dict_clone(fn->closure); bind_arguments(fn,pos->items,pos->count,kw,locals); return run_prepared(fn,locals,NULL); }
    if(is_obj(callee,O_BOUND_METHOD)){ BoundMethod *bm=&callee.as.obj->as.bm; int n=pos->count; Value *argv=(Value*)xmalloc(sizeof(Value)*(size_t)(n+1)); argv[0]=bm->receiver; for(int i=0;i<n;i++) argv[i+1]=pos->items[i]; Function *fn=bm->fn; Value r; if(fn->is_generator) r=objv(new_generator_bind(fn,argv,n+1,kw)); else { Dict *locals=dict_clone(fn->closure); bind_arguments(fn,argv,n+1,kw,locals); r=run_prepared(fn,locals,NULL); } free(argv); return r; }
    if(is_obj(callee,O_BOUND_NATIVE)){ if(kw->count) runtime_error("this function takes no keyword arguments"); return call_value(callee,pos->count,pos->items); }
    if(is_obj(callee,O_CLASS)){ Class *kl=&callee.as.obj->as.klass;
        if(strcmp(kl->name,"BaseException")==0||strcmp(kl->name,"RuntimeError")==0||strcmp(kl->name,"StopIteration")==0){ if(kw->count) runtime_error("exception takes no keyword arguments"); return call_value(callee,pos->count,pos->items); }
        Obj *in=new_obj(O_INSTANCE); in->as.inst.klass=kl; in->as.inst.fields=dict_new(); Value self=objv(in); Value init;
        if(dict_get(kl->methods,"__init__",&init)){ int n=pos->count; Value *argv=(Value*)xmalloc(sizeof(Value)*(size_t)(n+1)); argv[0]=self; for(int i=0;i<n;i++) argv[i+1]=pos->items[i]; Function *fn=&init.as.obj->as.fn; Dict *locals=dict_clone(fn->closure); bind_arguments(fn,argv,n+1,kw,locals); run_prepared(fn,locals,NULL); free(argv); }
        else if(pos->count!=0 || kw->count!=0) runtime_error("class constructor takes no args");
        return self; }
    runtime_error("object is not callable"); return nonev();
}
