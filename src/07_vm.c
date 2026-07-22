/* ========================= VM ========================= */

typedef struct { int ip; int sp; } Handler;
typedef struct { Function *fn; int ip; Dict *locals; Handler handlers[32]; int hcount; } Frame;
typedef struct { jmp_buf buf; } ExcJump;
typedef struct { Value stack[4096]; int sp; Frame frames[256]; int fcount; Dict *builtins; Dict *modules; jmp_buf panic; ExcJump exc_jumps[256]; int exc_depth; Value pending_exception; char *error_msg; } VM;
static VM vm;

static void print_value(Value v){
    switch(v.type){ case V_NONE: printf("None"); break; case V_BOOL: printf(v.as.boolean?"True":"False"); break; case V_INT: printf("%lld",(long long)v.as.i); break; case V_FLOAT: printf("%.15g",v.as.f); break; case V_NATIVE: printf("<native %s>",v.as.native->name); break; case V_OBJ:{ Obj *o=v.as.obj; switch(o->type){
        case O_STRING: printf("%s",o->as.str.s); break;
        case O_LIST: printf("["); for(int i=0;i<o->as.list.count;i++){ if(i)printf(", "); print_value(o->as.list.items[i]); } printf("]"); break;
        case O_TUPLE: printf("("); for(int i=0;i<o->as.tuple.count;i++){ if(i)printf(", "); print_value(o->as.tuple.items[i]); } if(o->as.tuple.count==1) printf(","); printf(")"); break;
        case O_SET: printf("{"); for(int i=0;i<o->as.set.count;i++){ if(i)printf(", "); print_value(o->as.set.items[i]); } printf("}"); break;
        case O_DICT: printf("{"); for(int i=0;i<o->as.dict.count;i++){ if(i)printf(", "); printf("%s: ",o->as.dict.keys[i]); print_value(o->as.dict.vals[i]); } printf("}"); break;
        case O_FUNCTION: printf("<function %s>",o->as.fn.name); break; case O_CLASS: printf("<class %s>",o->as.klass.name); break; case O_INSTANCE: printf("<%s instance>",o->as.inst.klass->name); break; case O_BOUND_METHOD: printf("<bound method %s>",o->as.bm.fn->name); break; case O_BOUND_NATIVE: printf("<bound native %s>",o->as.bn.name); break; case O_MODULE: printf("<module %s>",o->as.mod.name); break; case O_ITER: printf("<iterator>"); break; case O_GENERATOR: printf("<generator>"); break; case O_EXCEPTION: printf("%s",o->as.exc.message); break; } break; }
    }
}
static void push(Value v){ if(vm.sp>=4096) die("stack overflow"); vm.stack[vm.sp++]=v; }
static Value popv(void){ if(vm.sp<=0) die("stack underflow"); return vm.stack[--vm.sp]; }
static Value normalize_exception(Value v);
static void raise_exception(Value ex);
static int dispatch_pending_exception(void);

static const char *exception_type_name(Value v){
    if(is_obj(v,O_EXCEPTION)) return v.as.obj->as.exc.type_name;
    return "RuntimeError";
}
static char *exception_message(Value v){
    if(is_obj(v,O_EXCEPTION)) return xstrdup2(v.as.obj->as.exc.message);
    return value_to_cstr(v);
}
static void print_traceback(Value ex){
    char *msg=exception_message(ex);
    fprintf(stderr,"%s: %s\n",exception_type_name(ex),msg);
    free(msg);
    fprintf(stderr,"Traceback (most recent call last):\n");
    for(int i=vm.fcount-1;i>=0;i--){ Frame *f=&vm.frames[i]; int ip=f->ip>0?f->ip-1:0; int line=f->fn->chunk->line[ip]; fprintf(stderr,"  line %d, in %s\n",line,f->fn->name); }
}
static Value normalize_exception(Value v){
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
static void raise_exception(Value ex){
    vm.pending_exception=normalize_exception(ex);
    if(vm.exc_depth>0) longjmp(vm.exc_jumps[vm.exc_depth-1].buf,1);
    char *m=exception_message(vm.pending_exception);
    free(vm.error_msg); vm.error_msg=m;
    longjmp(vm.panic,1);
}
static void runtime_error(const char *msg){
    raise_exception(exceptionv("RuntimeError",msg?msg:"runtime error",nonev()));
}
static int dispatch_pending_exception(void){
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
static Value call_value(Value callee,int argc,Value *args);
static int function_declares(char **names,int n,const char *name){ for(int i=0;i<n;i++) if(strcmp(names[i],name)==0) return 1; return 0; }
static int get_instance_method(Value obj,const char *name,Value *out){
    if(is_obj(obj,O_INSTANCE)){ Instance *in=&obj.as.obj->as.inst; if(dict_get(in->klass->methods,name,out) && is_obj(*out,O_FUNCTION)){ Obj *b=new_obj(O_BOUND_METHOD); b->as.bm.receiver=obj; b->as.bm.fn=&out->as.obj->as.fn; *out=objv(b); return 1; } }
    return 0;
}
static int call_instance_method0(Value obj,const char *name, Value *out){
    Value m; if(!get_instance_method(obj,name,&m)) return 0;
    *out=call_value(m,0,NULL); return 1;
}
static int call_instance_method1(Value obj,const char *name, Value arg, Value *out){
    Value m; if(!get_instance_method(obj,name,&m)) return 0;
    Value argv[1]; argv[0]=arg; *out=call_value(m,1,argv); return 1;
}
static int val_equal(Value a,Value b){
    if(a.type!=b.type){ if(is_number(a)&&is_number(b)) return as_double(a)==as_double(b); return 0; }
    if(a.type==V_NONE)return 1;
    if(a.type==V_BOOL)return a.as.boolean==b.as.boolean;
    if(a.type==V_INT)return a.as.i==b.as.i;
    if(a.type==V_FLOAT)return a.as.f==b.as.f;
    if(is_obj(a,O_STRING)&&is_obj(b,O_STRING)) return strcmp(a.as.obj->as.str.s,b.as.obj->as.str.s)==0;
    Value r; if(call_instance_method1(a,"__eq__",b,&r)) return truthy(r);
    return a.as.obj==b.as.obj;
}
static Value binary_add(Value a,Value b){
    Value r; if(call_instance_method1(a,"__add__",b,&r)) return r;
    if(is_number(a)&&is_number(b)){ if(a.type==V_FLOAT||b.type==V_FLOAT) return floatv(as_double(a)+as_double(b)); return intv(as_int(a)+as_int(b)); }
    if(is_obj(a,O_STRING)||is_obj(b,O_STRING)){ char *sa=value_to_cstr(a); char *sb=value_to_cstr(b); int n=(int)(strlen(sa)+strlen(sb)); char *r=(char*)xmalloc((size_t)n+1); strcpy(r,sa); strcat(r,sb); Value v=stringv(r); free(sa); free(sb); free(r); return v; }
    runtime_error("unsupported operands for +"); return nonev();
}
static Value binary_num(Value a,Value b,Op op){ if(!is_number(a)||!is_number(b)) runtime_error("numeric operator on non-number"); if(op==OP_DIV) return floatv(as_double(a)/as_double(b)); if(op==OP_POWER){ double base=as_double(a); double exp=as_double(b); double r=1; int e_neg=0; if(exp<0){ e_neg=1; exp=-exp; } int ei=(int)exp; for(int i=0;i<ei;i++) r*=base; if(e_neg) r=1.0/r; return floatv(r); } if(a.type==V_FLOAT||b.type==V_FLOAT){ double x=as_double(a),y=as_double(b); if(op==OP_SUB)return floatv(x-y); if(op==OP_MUL)return floatv(x*y); } int64_t x=as_int(a),y=as_int(b); if(op==OP_SUB)return intv(x-y); return intv(x*y); }
static Value compare(Value a,Value b,Op op){ int r=0; if(is_number(a)&&is_number(b)){ double x=as_double(a),y=as_double(b); r=(op==OP_LT?x<y:op==OP_LE?x<=y:op==OP_GT?x>y:x>=y); } else if(is_obj(a,O_STRING)&&is_obj(b,O_STRING)){ int c=strcmp(a.as.obj->as.str.s,b.as.obj->as.str.s); r=(op==OP_LT?c<0:op==OP_LE?c<=0:op==OP_GT?c>0:c>=0); } else runtime_error("comparison between unsupported types"); return boolv(r); }

static Value run_function(Function *fn,int argc,Value *args);
static int generator_next(Obj *g, Value *out);
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
#if defined(MPY_PLATFORM_KOLIBRI) || defined(MPY_TARGET_KOLIBRIOS) || defined(MPY_FS_KOLIBRI)
    extern void kol_console_puts(const char *s);
    extern void kol_console_gets(char *buf, int maxlen);
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
static Native N_INPUT={"input",-1,native_input};

static Native N_LEN={"len",1,native_len};
static Native N_RANGE={"range",-1,native_range};
static int iterator_next_value(Value top, Value *out){
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
static Value native_iter(int argc,Value *argv){ if(argc!=1) runtime_error("iter() expects 1 argument"); Value itv=argv[0]; Value custom; if(call_instance_method0(itv,"__iter__",&custom)) return custom; if(!(is_obj(itv,O_LIST)||is_obj(itv,O_DICT)||is_obj(itv,O_STRING)||is_obj(itv,O_GENERATOR))) runtime_error("object is not iterable"); Obj *it=new_obj(O_ITER); it->as.iter.iterable=itv; it->as.iter.index=0; return objv(it); }
static Native N_NEXT={"next",1,native_next};
static Native N_ITER={"iter",1,native_iter};

static Value get_attr(Value obj,const char *name){
    if(is_obj(obj,O_INSTANCE)){ Instance *in=&obj.as.obj->as.inst; Value v; if(dict_get(in->fields,name,&v)) return v; if(dict_get(in->klass->methods,name,&v) && is_obj(v,O_FUNCTION)){ Obj *b=new_obj(O_BOUND_METHOD); b->as.bm.receiver=obj; b->as.bm.fn=&v.as.obj->as.fn; return objv(b); } runtime_error("unknown instance attribute"); }
    if(is_obj(obj,O_CLASS)){ Value v; if(dict_get(obj.as.obj->as.klass.methods,name,&v)) return v; runtime_error("unknown class attribute"); }
    if(is_obj(obj,O_MODULE)){ Value v; if(dict_get(obj.as.obj->as.mod.dict,name,&v)) return v; runtime_error("unknown module attribute"); }
    if(is_obj(obj,O_LIST)&&strcmp(name,"append")==0){ Obj *b=new_obj(O_BOUND_NATIVE); b->as.bn.receiver=obj; b->as.bn.name=xstrdup2("append"); return objv(b); }
    runtime_error("attribute access on unsupported type"); return nonev();
}
static void set_attr(Value obj,const char *name,Value val){ if(is_obj(obj,O_INSTANCE)){ dict_set(obj.as.obj->as.inst.fields,name,val); return; } if(is_obj(obj,O_MODULE)){ dict_set(obj.as.obj->as.mod.dict,name,val); return; } runtime_error("cannot set attribute on this object"); }
static Value get_index(Value obj,Value idx){
    Value r; if(call_instance_method1(obj,"__getitem__",idx,&r)) return r;
    if(is_obj(obj,O_LIST)){ if(!is_number(idx)) runtime_error("list index must be int"); int64_t i=as_int(idx); List *l=&obj.as.obj->as.list; if(i<0) i+=l->count; if(i<0||i>=l->count) runtime_error("list index out of range"); return l->items[i]; }
    if(is_obj(obj,O_DICT)){ char *k=value_to_cstr(idx); Value v; int ok=dict_get(&obj.as.obj->as.dict,k,&v); free(k); if(!ok) runtime_error("dict key not found"); return v; }
    if(is_obj(obj,O_STRING)){ if(!is_number(idx)) runtime_error("string index must be int"); int64_t i=as_int(idx); String *s=&obj.as.obj->as.str; if(i<0)i+=s->len; if(i<0||i>=s->len) runtime_error("string index out of range"); return stringv_len(&s->s[i],1); }
    runtime_error("indexing unsupported type"); return nonev();
}
static void set_index(Value obj,Value idx,Value val){
    Value m; if(get_instance_method(obj,"__setitem__",&m)){ Value argv[2]; argv[0]=idx; argv[1]=val; call_value(m,2,argv); return; }
    if(is_obj(obj,O_LIST)){ if(!is_number(idx)) runtime_error("list index must be int"); int64_t i=as_int(idx); List *l=&obj.as.obj->as.list; if(i<0)i+=l->count; if(i<0||i>=l->count) runtime_error("list index out of range"); l->items[i]=val; return; }
    if(is_obj(obj,O_DICT)){ char *k=value_to_cstr(idx); dict_set(&obj.as.obj->as.dict,k,val); free(k); return; }
    runtime_error("item assignment unsupported type");
}

static int same_identity(Value a,Value b){
    if(a.type==V_NONE&&b.type==V_NONE) return 1;
    if(a.type==V_BOOL&&b.type==V_BOOL) return a.as.boolean==b.as.boolean;
    if(a.type==V_OBJ&&b.type==V_OBJ) return a.as.obj==b.as.obj;
    if(a.type==V_NATIVE&&b.type==V_NATIVE) return a.as.native==b.as.native;
    return 0;
}
static Value contains_value(Value needle,Value hay){
    Value r; if(call_instance_method1(hay,"__contains__",needle,&r)) return boolv(truthy(r));
    if(is_obj(hay,O_LIST)){ List *l=&hay.as.obj->as.list; for(int i=0;i<l->count;i++) if(val_equal(needle,l->items[i])) return boolv(1); return boolv(0); }
    if(is_obj(hay,O_TUPLE)){ List *l=&hay.as.obj->as.tuple; for(int i=0;i<l->count;i++) if(val_equal(needle,l->items[i])) return boolv(1); return boolv(0); }
    if(is_obj(hay,O_SET)){ List *l=&hay.as.obj->as.set; for(int i=0;i<l->count;i++) if(val_equal(needle,l->items[i])) return boolv(1); return boolv(0); }
    if(is_obj(hay,O_DICT)){ char *k=value_to_cstr(needle); Value tmp; int ok=dict_get(&hay.as.obj->as.dict,k,&tmp); free(k); return boolv(ok); }
    if(is_obj(hay,O_STRING)){ char *n=value_to_cstr(needle); int ok=strstr(hay.as.obj->as.str.s,n)!=NULL; free(n); return boolv(ok); }
    runtime_error("right operand is not container"); return boolv(0);
}
static int norm_bound(int64_t x,int n){ if(x<0) x+=n; if(x<0) x=0; if(x>n) x=n; return (int)x; }
static Value get_slice(Value obj,Value startv,Value endv){
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


static Value call_value(Value callee,int argc,Value *args);
static Value run_function_ex(Function *fn,int argc,Value *args, Obj *gen_obj){
    if(vm.fcount>=256) runtime_error("call stack overflow");
    if(!gen_obj && (argc<fn->min_arity || argc>fn->arity)){ char b[160]; snprintf(b,sizeof(b),"%s() expects %d..%d args, got %d",fn->name,fn->min_arity,fn->arity,argc); runtime_error(b); }
    Frame *fr=&vm.frames[vm.fcount++]; fr->fn=fn;
    if(gen_obj){ fr->ip=gen_obj->as.gen.ip; fr->locals=gen_obj->as.gen.locals; }
    else { fr->ip=0; fr->hcount=0; fr->locals=dict_clone(fn->closure); for(int i=0;i<argc;i++) dict_set(fr->locals,fn->params[i],args[i]); for(int i=argc;i<fn->arity;i++){ int di=i-fn->min_arity; dict_set(fr->locals,fn->params[i],fn->defaults[di]); } }
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
            case OP_ADD:{ Value b=popv(),a=popv(); push(binary_add(a,b)); break; } case OP_SUB: case OP_MUL: case OP_DIV: case OP_POWER:{ Value b=popv(),a=popv(); push(binary_num(a,b,op)); break; } case OP_NEG:{ Value a=popv(); if(!is_number(a)) runtime_error("bad unary -"); if(a.type==V_FLOAT) push(floatv(-a.as.f)); else push(intv(-as_int(a))); break; }
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
            case OP_YIELD:{ if(!gen_obj) runtime_error("yield outside generator"); Value r=popv(); gen_obj->as.gen.ip=fr->ip; gen_obj->as.gen.locals=fr->locals; vm.fcount--; vm.exc_depth=exc_slot; return r; }
            case OP_RETURN:{ Value r=popv(); if(gen_obj) gen_obj->as.gen.done=1; vm.fcount--; vm.exc_depth=exc_slot; return r; }
            case OP_MAKE_LIST:{ int n=c->code[fr->ip++]; Obj *o=new_list(); for(int i=0;i<n;i++) list_push(&o->as.list,vm.stack[vm.sp-n+i]); vm.sp-=n; push(objv(o)); break; }
            case OP_MAKE_TUPLE:{ int n=c->code[fr->ip++]; Obj *o=new_tuple(); for(int i=0;i<n;i++) list_push(&o->as.tuple,vm.stack[vm.sp-n+i]); vm.sp-=n; push(objv(o)); break; }
            case OP_MAKE_SET:{ int n=c->code[fr->ip++]; Obj *o=new_set(); for(int i=0;i<n;i++) set_add(&o->as.set,vm.stack[vm.sp-n+i]); vm.sp-=n; push(objv(o)); break; }
            case OP_MAKE_DICT:{ int n=c->code[fr->ip++]; Obj *o=new_dict_obj(); for(int i=0;i<n;i++){ Value val=popv(); Value key=popv(); char *ks=value_to_cstr(key); dict_set(&o->as.dict,ks,val); free(ks); } push(objv(o)); break; }
            case OP_GET_INDEX:{ Value idx=popv(),obj=popv(); push(get_index(obj,idx)); break; } case OP_GET_SLICE:{ Value endv=popv(),startv=popv(),obj=popv(); push(get_slice(obj,startv,endv)); break; } case OP_SET_INDEX:{ Value val=popv(),idx=popv(),obj=popv(); set_index(obj,idx,val); push(val); break; }
            case OP_GET_ATTR:{ Value namev=c->consts[c->code[fr->ip++]]; Value obj=popv(); push(get_attr(obj,namev.as.obj->as.str.s)); break; } case OP_SET_ATTR:{ Value namev=c->consts[c->code[fr->ip++]]; Value val=popv(),obj=popv(); set_attr(obj,namev.as.obj->as.str.s,val); push(val); break; }
            case OP_DEF:{ Value fv=c->consts[c->code[fr->ip++]]; Function *nf=clone_function(&fv.as.obj->as.fn,fr->locals); push(objv(nf->owner)); break; }
            case OP_CLASS:{ Value bodyv=c->consts[c->code[fr->ip++]]; Value namev=c->consts[c->code[fr->ip++]]; Function *body=&bodyv.as.obj->as.fn; Dict *oldg=body->globals; Dict *classdict=dict_new(); body->globals=classdict; run_function(body,0,NULL); body->globals=oldg; Obj *co=new_obj(O_CLASS); co->as.klass.name=xstrdup2(namev.as.obj->as.str.s); co->as.klass.methods=classdict; push(objv(co)); fr=&vm.frames[vm.fcount-1]; fn=fr->fn; c=fr->fn->chunk; break; }
            case OP_IMPORT:{ Value namev=c->consts[c->code[fr->ip++]]; char *mn=namev.as.obj->as.str.s; Value modv; if(dict_get(vm.modules,mn,&modv)){ push(modv); break; } char *path=mpy_fs_module_path(fn->module_dir,mn); char *err=NULL; char *src=mpy_fs_try_read_file(path,&err); if(!src){ Value ex=exceptionv("RuntimeError",err?err:"cannot import module",nonev()); free(err); free(path); raise_exception(ex); break; } char *dir=mpy_fs_dirname(path); Dict *g=dict_new(); Function *mf=compile_source(src,mn,dir,g); modv=objv(new_module(mn,g)); dict_set(vm.modules,mn,modv); run_function(mf,0,NULL); push(modv); free(src); free(dir); free(path); fr=&vm.frames[vm.fcount-1]; fn=fr->fn; c=fr->fn->chunk; break; }
        }
    }
}
static Value run_function(Function *fn,int argc,Value *args){ return run_function_ex(fn,argc,args,NULL); }

static Obj *new_generator(Function *fn,int argc,Value *args){
    if(argc<fn->min_arity || argc>fn->arity){ char b[160]; snprintf(b,sizeof(b),"%s() expects %d..%d args, got %d",fn->name,fn->min_arity,fn->arity,argc); runtime_error(b); }
    Obj *g=new_obj(O_GENERATOR); g->as.gen.fn=fn; g->as.gen.locals=dict_clone(fn->closure); g->as.gen.ip=0; g->as.gen.done=0;
    for(int i=0;i<argc;i++) dict_set(g->as.gen.locals,fn->params[i],args[i]);
    for(int i=argc;i<fn->arity;i++){ int di=i-fn->min_arity; dict_set(g->as.gen.locals,fn->params[i],fn->defaults[di]); }
    return g;
}
static int generator_next(Obj *g, Value *out){
    if(g->as.gen.done) return 0;
    Value r=run_function_ex(g->as.gen.fn,0,NULL,g);
    if(g->as.gen.done) return 0;
    *out=r;
    return 1;
}
static Value call_value(Value callee,int argc,Value *args){
    if(callee.type==V_NATIVE) return callee.as.native->fn(argc,args);
    if(is_obj(callee,O_FUNCTION)){ Function *fn=&callee.as.obj->as.fn; if(fn->is_generator) return objv(new_generator(fn,argc,args)); return run_function(fn,argc,args); }
    if(is_obj(callee,O_BOUND_METHOD)){ BoundMethod *bm=&callee.as.obj->as.bm; Value *argv=(Value*)xmalloc(sizeof(Value)*(size_t)(argc+1)); argv[0]=bm->receiver; for(int i=0;i<argc;i++) argv[i+1]=args[i]; Value r; if(bm->fn->is_generator) r=objv(new_generator(bm->fn,argc+1,argv)); else r=run_function(bm->fn,argc+1,argv); free(argv); return r; }
    if(is_obj(callee,O_BOUND_NATIVE)){ BoundNative *bn=&callee.as.obj->as.bn; if(strcmp(bn->name,"append")==0){ if(argc!=1) runtime_error("append() expects 1 arg"); list_push(&bn->receiver.as.obj->as.list,args[0]); return nonev(); } }
    if(is_obj(callee,O_CLASS)){ Class *kl=&callee.as.obj->as.klass; if(strcmp(kl->name,"BaseException")==0||strcmp(kl->name,"RuntimeError")==0||strcmp(kl->name,"StopIteration")==0){ if(argc>1) runtime_error("exception constructor expects 0 or 1 argument"); char *m=argc==1?value_to_cstr(args[0]):xstrdup2(""); Value ex=exceptionv(kl->name,m,argc==1?args[0]:nonev()); free(m); return ex; } Obj *in=new_obj(O_INSTANCE); in->as.inst.klass=kl; in->as.inst.fields=dict_new(); Value self=objv(in); Value init; if(dict_get(kl->methods,"__init__",&init)){ Value *argv=(Value*)xmalloc(sizeof(Value)*(size_t)(argc+1)); argv[0]=self; for(int i=0;i<argc;i++) argv[i+1]=args[i]; run_function(&init.as.obj->as.fn,argc+1,argv); free(argv); } else if(argc!=0) runtime_error("class constructor takes no args"); return self; }
    runtime_error("object is not callable"); return nonev();
}

extern void _pei386_runtime_relocator(void);
int main(int argc,char **argv){
    const char *script_path = NULL;
#if defined(MPY_PLATFORM_KOLIBRI) || defined(MPY_TARGET_KOLIBRIOS) || defined(MPY_FS_KOLIBRI)
    fprintf(stderr, "[minipy] start\n");
    fflush(stderr);
    fprintf(stderr, "[minipy] calling reloc\n");
    fflush(stderr);
    _pei386_runtime_relocator();
    fprintf(stderr, "[minipy] reloc ok\n");
    fflush(stderr);
    /* Initialize process heap (required before any malloc) - f68.11 */
    {
        int heap_ptr;
        __asm__ __volatile__("int $0x40"
            : "=a"(heap_ptr)
            : "a"(68), "b"(11)
            : "memory");
        fprintf(stderr, "[minipy] heap init: ptr=0x%x\n", heap_ptr);
        fflush(stderr);
    }
#endif
    const char *program = (argc>0 && argv && argv[0]) ? argv[0] : "minipy";

    if(argc<2){
#if defined(MPY_PLATFORM_KOLIBRI) || defined(MPY_TARGET_KOLIBRIOS) || defined(MPY_FS_KOLIBRI)
#ifndef MPY_DEFAULT_SCRIPT
#define MPY_DEFAULT_SCRIPT MPY_FS_DEFAULT_IMPORT_DIR "/main.mpy"
#endif
        script_path = MPY_DEFAULT_SCRIPT;
#else
        fprintf(stderr,"usage: %s [--dump-ast|--dump-symbols|--dump-bytecode|--fs-info] file.mpy\n",program);
        return 2;
#endif
    }
    if(!script_path && strcmp(argv[1],"--fs-info")==0){
        printf("%s\n",mpy_fs_backend_name());
        return 0;
    }
    if(!script_path && (strcmp(argv[1],"--dump-ast")==0 || strcmp(argv[1],"--dump-symbols")==0 || strcmp(argv[1],"--dump-bytecode")==0)){
        if(argc<3){ fprintf(stderr,"usage: %s %s file.mpy\n",program,argv[1]); return 2; }
        char *src=mpy_fs_read_file(argv[2]);
        if(strcmp(argv[1],"--dump-ast")==0) dump_ast_for_source(src);
        else if(strcmp(argv[1],"--dump-symbols")==0) dump_symbols_for_source(src);
        else {
            char *dir=mpy_fs_dirname(argv[2]); Dict *globals=dict_new(); Function *mainfn=compile_source(src,"__main__",dir,globals); dump_function_bytecode(mainfn); free(dir);
        }
        free(src);
        return 0;
    }
    if(!script_path) script_path = argv[1];

#if defined(MPY_PLATFORM_KOLIBRI) || defined(MPY_TARGET_KOLIBRIOS) || defined(MPY_FS_KOLIBRI)
    kol_console_init();
    kol_console_puts("MiniPy - KolibriOS\n\r");
    kol_console_puts(script_path ? script_path : "(default)");
    kol_console_puts("\n\r");
#else
    printf("MiniPy - Mini Python Interpreter\n");
    printf("Script: %s\n\n", script_path ? script_path : "(default)");
#endif
    memset(&vm,0,sizeof(vm));
#if defined(MPY_PLATFORM_KOLIBRI) || defined(MPY_TARGET_KOLIBRIOS) || defined(MPY_FS_KOLIBRI)
    fprintf(stderr, "[minipy] memset done, setjmp...\n");
    fflush(stderr);
#endif
    if(setjmp(vm.panic)){ print_traceback(is_obj(vm.pending_exception,O_EXCEPTION)?vm.pending_exception:exceptionv("RuntimeError",vm.error_msg?vm.error_msg:"error",nonev())); return 1; }
#if defined(MPY_PLATFORM_KOLIBRI) || defined(MPY_TARGET_KOLIBRIOS) || defined(MPY_FS_KOLIBRI)
    fprintf(stderr, "[minipy] setjmp ok, reading %s\n", script_path);
    fflush(stderr);
#endif
    vm.builtins=dict_new(); vm.modules=dict_new();
    dict_set(vm.builtins,"len",nativev(&N_LEN)); dict_set(vm.builtins,"range",nativev(&N_RANGE)); dict_set(vm.builtins,"next",nativev(&N_NEXT)); dict_set(vm.builtins,"iter",nativev(&N_ITER)); dict_set(vm.builtins,"input",nativev(&N_INPUT));
    Obj *be=new_obj(O_CLASS); be->as.klass.name=xstrdup2("BaseException"); be->as.klass.methods=dict_new();
    Obj *re=new_obj(O_CLASS); re->as.klass.name=xstrdup2("RuntimeError"); re->as.klass.methods=dict_new();
    Obj *se=new_obj(O_CLASS); se->as.klass.name=xstrdup2("StopIteration"); se->as.klass.methods=dict_new();
    dict_set(vm.builtins,"BaseException",objv(be)); dict_set(vm.builtins,"RuntimeError",objv(re)); dict_set(vm.builtins,"StopIteration",objv(se));
    char *src=mpy_fs_read_file(script_path);
#if defined(MPY_PLATFORM_KOLIBRI) || defined(MPY_TARGET_KOLIBRIOS) || defined(MPY_FS_KOLIBRI)
    fprintf(stderr, "[minipy] file read ok, %d bytes\n", src ? (int)strlen(src) : -1);
    fflush(stderr);
#endif
    if(!src){
        fprintf(stderr, "[minipy] no source, exiting\n");
        fflush(stderr);
        return 1;
    }
#if defined(MPY_PLATFORM_KOLIBRI) || defined(MPY_TARGET_KOLIBRIOS) || defined(MPY_FS_KOLIBRI)
    fprintf(stderr, "[minipy] compiling...\n"); fflush(stderr);
#endif
    char *dir=mpy_fs_dirname(script_path);
#if defined(MPY_PLATFORM_KOLIBRI) || defined(MPY_TARGET_KOLIBRIOS) || defined(MPY_FS_KOLIBRI)
    fprintf(stderr, "[minipy] dir=%s\n", dir); fflush(stderr);
#endif
    Dict *globals=dict_new();
#if defined(MPY_PLATFORM_KOLIBRI) || defined(MPY_TARGET_KOLIBRIOS) || defined(MPY_FS_KOLIBRI)
    fprintf(stderr, "[minipy] globals created, calling compile_source...\n"); fflush(stderr);
#endif
    Function *mainfn=compile_source(src,"__main__",dir,globals);
#if defined(MPY_PLATFORM_KOLIBRI) || defined(MPY_TARGET_KOLIBRIOS) || defined(MPY_FS_KOLIBRI)
    fprintf(stderr, "[minipy] compile done, running...\n"); fflush(stderr);
#endif
    run_function(mainfn,0,NULL);
#if defined(MPY_PLATFORM_KOLIBRI) || defined(MPY_TARGET_KOLIBRIOS) || defined(MPY_FS_KOLIBRI)
    fprintf(stderr, "[minipy] done\n"); fflush(stderr);
#endif
    free(src); free(dir);
#if defined(MPY_PLATFORM_KOLIBRI) || defined(MPY_TARGET_KOLIBRIOS) || defined(MPY_FS_KOLIBRI)
    kol_console_deinit();
#endif
    return 0;
}
