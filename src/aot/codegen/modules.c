/* ========================= Typed compiler, code generator: built-in modules =========================
   sys, asyncio (tasks, await), math, time, random. */

#include "gen.h"

int gen_sys(F *f, Expr *e, XInfo *xi){
    const char *m=xi->name;
    if(!strcmp(m,"buffer")){ Ty *t0=TY(e->items[0]); gen_borrow(f,e->items[0]); to_i32(f,t0); CALLRT(f,t0->k==TY_STR||t0->k==TY_BYTES?"rt_buf_from_str":"rt_buf_new"); return 1; }
    if(!strcmp(m,"addr")){ gen_borrow(f,e->items[0]); E(f,"add eax,12"); widen(f,0); return 0; }
    if(!strcmp(m,"_rawargs")){ gen(f,e->items[0]); CALLRT(f,"rt_sys_args"); return 1; }
    if(!strcmp(m,"_gc_collect")){ if(gg->gc_on) CALLRT(f,"rt_gc_collect"); else E(f,"xor eax,eax"); E(f,"xor edx,edx"); return 0; }   /* (gc.collect()) */
    if(!strcmp(m,"_sleep")){ gen_as(f,e->items[0],TY_FLOAT_T,0); CALLRT(f,"rt_time_sleep"); return 0; }
    if(!strcmp(m,"exit")){ if(e->count) gen(f,e->items[0]); else E(f,"xor eax,eax"); E(f,"mov ebx,eax"); CALLRT(f,"rt_exit"); return 0; }
    if(!strcmp(m,"peek_at")||!strcmp(m,"peek_str_at")||!strcmp(m,"cstr_at")||!strcmp(m,"poke_str_at")){   /* raw memory: (address, n | str) */
        gen(f,e->items[0]); E(f,"push eax");
        if(m[1]=='o') gen_borrow(f,e->items[1]); else gen(f,e->items[1]);
        E(f,"mov edx,eax"); E(f,"pop eax");
        if(!strcmp(m,"peek_at")){ CALLRT(f,"rt_mem_peek"); widen(f,0); return 0; }
        if(!strcmp(m,"poke_str_at")){ CALLRT(f,"rt_mem_poke_str"); return 0; }
        CALLRT(f,m[0]=='c'?"rt_mem_cstr":"rt_mem_peek_str"); return 1;
    }
    if(!strcmp(m,"poke_at")){ gen(f,e->items[0]); E(f,"push eax"); gen(f,e->items[1]); E(f,"push eax"); gen(f,e->items[2]);
        E(f,"mov ecx,eax"); E(f,"pop edx"); E(f,"pop eax"); CALLRT(f,"rt_mem_poke"); return 0; }
    int n=e->count;
    E(f,"sub esp,%d",4*n);
    for(int i=0;i<n;i++){ gen_borrow(f,e->items[i]); E(f,"mov [esp+%d],eax",4*i); }
    if(!strcmp(m,"poke")){ CALLRT(f,"rt_buf_poke"); E(f,"add esp,16"); return 0; }
    if(!strcmp(m,"peek")){ CALLRT(f,"rt_buf_peek"); E(f,"add esp,12"); widen(f,0); return 0; }
    E(f,"pop eax"); E(f,"pop edx"); E(f,"pop ecx");
    if(!strcmp(m,"poke_str")||!strcmp(m,"poke_bytes")){ CALLRT(f,"rt_buf_poke_str"); return 0; }   /* (bytes: the same layout) */
    CALLRT(f,!strcmp(m,"peek_bytes")?"rt_buf_peek_bytes":"rt_buf_peek_str"); return 1;
}

/* ---------------------------------------------------------------- asyncio */

/* TS<i>: where a task starts: calls the coroutine with the arguments above it,
   keeps the result in the task, drops the arguments, ends the task. */
static int task_stub(AFunc *fn, int virt){
    for(int i=0;i<gg->nstubs;i++) if(gg->stubs[i].fn==fn && gg->stubs[i].virt==virt) return i;
    if(gg->nstubs==gg->cstubs){ gg->cstubs=gg->cstubs?gg->cstubs*2:8; gg->stubs=xrealloc(gg->stubs,sizeof(*gg->stubs)*(size_t)gg->cstubs); }
    gg->stubs[gg->nstubs].fn=fn; gg->stubs[gg->nstubs].virt=virt;
    use_fn(fn); rt("rt_task_exit"); rt("rt_task_new");
    return gg->nstubs++;
}
void emit_task_stub(int i){
    AFunc *fn=gg->stubs[i].fn; Buf *o=&gg->text; Ty *ret=ty_find(fn->ret);
    buf_printf(o,"\n%s:                          ; task: %s%s%s()\n",fnl(fn,gg->stubs[i].virt?LB_VTASK:LB_TASK),fn->cls?fn->cls->name:"",fn->cls?".":"",fn->name);
    if(gg->stubs[i].virt) buf_printf(o,"        mov eax,[esp]\n        mov eax,[eax+8]\n        call dword [eax+%d]\n",8+4*fn->vslot);
    else buf_printf(o,"        call %s\n",fnl(fn,LB_CODE));
    buf_printf(o,"        mov ecx,[rt_cur_task]\n");
    if(is_flt(ret)) buf_printf(o,"        fstp qword [ecx+24]\n");
    else if(ret->k!=TY_VOID){ buf_printf(o,"        mov [ecx+24],eax\n"); if(is_lng(ret)) buf_printf(o,"        mov [ecx+28],edx\n"); if(is_ptr(ret)) buf_printf(o,"        mov dword [ecx+44],1\n"); }
    int off=0;
    for(int k=0;k<fn->nparams;k++){ Ty *pt=fn->params[k]->ty;
        if(is_ptr(pt)){ rt("rt_decref"); buf_printf(o,"        mov eax,[esp+%d]\n        call rt_decref\n",off); }
        off+=esize(pt); }
    buf_printf(o,"        jmp rt_task_exit\n");
}
/* A task running the coroutine call `call` -> eax (owned). Its arguments are
   evaluated now (and owned by the task). */
void gen_task_new(F *f, Expr *call){
    XInfo *xi=xinfo(call); AFunc *fn=xi->fn;
    if(fn->star>=0||fn->dstar>=0) cg_fail(call->line,"coroutines with *args/**kwargs are not supported in compiled code");
    for(int i=0;i<16;i++) if(xi->argelem[i]) cg_fail(call->line,"f(*seq) of a coroutine is not supported in compiled code");
    int self_kind=xi->kind==X_METHOD&&!fn->is_static ? 1 : xi->kind==X_SUPER&&!fn->is_static ? 2 : 0;
    int off[16], total=0;
    for(int i=0;i<fn->nparams;i++){ off[i]=total; total+=esize(fn->params[i]->ty); }
    int ts=task_stub(fn,self_kind==1&&fn->overridden);
    E(f,"mov eax,%d",total); E(f,"mov edx,%s",fnl(gg->stubs[ts].fn,gg->stubs[ts].virt?LB_VTASK:LB_TASK)); CALLRT(f,"rt_task_new");
    E(f,"push eax");
    #define TASK_ARG(pt,o_) do{ E(f,"mov ecx,[esp]"); E(f,"mov ecx,[ecx+12]"); store_new(f,pt,"ecx",20+(o_)); }while(0)
    if(self_kind==1){ gen_owned(f,call->a->a); panic_if_null(f); TASK_ARG(fn->params[0]->ty,off[0]); }
    if(self_kind==2){ E(f,"mov eax,[ebp+8]"); incref(f); TASK_ARG(fn->params[0]->ty,off[0]); }
    for(int ai=0;ai<call->count;ai++)
        for(int i=0;i<fn->nparams;i++) if(xi->argmap[i]==ai){ Ty *pt=fn->params[i]->ty; gen_as(f,call->items[ai],pt,1); TASK_ARG(pt,off[i]); }
    for(int i=(self_kind?1:0);i<fn->nparams;i++) if(xi->argmap[i]<0 && fn->defaults[i]){ Ty *pt=fn->params[i]->ty; gen_as(f,fn->defaults[i],pt,1); TASK_ARG(pt,off[i]); }
    #undef TASK_ARG
    E(f,"pop eax");
}
int gen_asyncio(F *f, Expr *e, XInfo *xi){
    if(!strcmp(xi->name,"create_task")){ gen_task_new(f,e->items[0]); return 1; }
    /* asyncio.run(main()): the event loop runs until that task is done */
    Ty *t=TY(e);
    gen_task_new(f,e->items[0]); hold(f);
    CALLRT(f,"rt_async_run");
    if(ty_find(t)->k!=TY_VOID) load_mem(f,t,"eax",24);
    return 0;
}
int gen_await(F *f, Expr *e){
    XInfo *xi=xinfo(e); Ty *t=TY(e); Expr *x=e->a;
    if(!strcmp(xi->name,"call")||!strcmp(xi->name,"plain")) return gen(f,x);
    if(!strcmp(xi->name,"sleep")){ gen_as(f,x->items[0],TY_FLOAT_T,0); CALLRT(f,"rt_async_sleep"); return 0; }
    if(!strcmp(xi->name,"task")){
        gen_borrow(f,x); CALLRT(f,"rt_task_wait");
        if(ty_find(t)->k!=TY_VOID) load_mem(f,t,"eax",24);
        return 0;
    }
    /* gather: start every task, then collect the results in order */
    int n=x->count, slots[32];
    if(n>32) cg_fail(e->line,"too many coroutines for gather()");
    for(int i=0;i<n;i++){ gen_task_new(f,x->items[i]); hold(f); slots[i]=f->tmp[f->tmp_used-1]; }
    if(ty_find(t)->k==TY_VOID){ for(int i=0;i<n;i++){ E(f,"mov eax,[ebp%+d]",slots[i]); CALLRT(f,"rt_task_wait"); } return 0; }
    Ty *el=ty_find(t)->elem;
    new_list(f,el); E(f,"push eax");
    for(int i=0;i<n;i++){
        E(f,"mov eax,[ebp%+d]",slots[i]); CALLRT(f,"rt_task_wait");
        load_mem(f,el,"eax",24); if(is_ptr(el)) incref(f);
        list_append_top(f,el);
    }
    E(f,"pop eax");
    return 1;
}

/* ---------------------------------------------------------------- math, time, random */

static void farg(F *f, Expr *a){ gen_as(f,a,TY_FLOAT_T,0); }
int gen_bmod(F *f, Expr *e, XInfo *xi){
    const char *m=xi->name; Expr **a=e->items;
    Expr *call=e->a; const char *mod="?";
    if(call->kind==EXPR_ATTRIBUTE && call->a->kind==EXPR_NAME){
        ASym *s=NULL; AFunc *fn=f->fn;
        if(fn->def) s=symtab_find(&fn->locals,call->a->name);
        if(!s) s=symtab_find(&fn->mod->syms,call->a->name);
        if(s && s->kind==AS_SYS) mod=(const char*)s->p;
    }
    if(!strcmp(mod,"math")){
        if(!strcmp(m,"floor")||!strcmp(m,"ceil")){ farg(f,a[0]); if(m[0]=='c') E(f,"fchs"); CALLRT(f,"rt_ffloor"); if(m[0]=='c') E(f,"fchs"); CALLRT(f,"rt_ftoi"); return 0; }
        if(!strcmp(m,"trunc")){ farg(f,a[0]); CALLRT(f,"rt_ftoi"); return 0; }
        if(!strcmp(m,"gcd")){ gen_as(f,a[0],TY_INT_T,0); push_value(f,TY_INT_T); gen_as(f,a[1],TY_INT_T,0); CALLRT(f,"rt_gcd"); E(f,"add esp,8"); return 0; }
        if(!strcmp(m,"isqrt")){ gen_as(f,a[0],TY_INT_T,0); CALLRT(f,"rt_isqrt"); return 0; }
        if(!strcmp(m,"frexp")){ Ty *t=TY(e); farg(f,a[0]); CALLRT(f,"rt_frexp");        /* (m, e): a new tuple */
            E(f,"push eax"); E(f,"sub esp,8"); E(f,"fstp qword [esp]");
            E(f,"mov eax,TD%d",tdesc(t)); E(f,"mov edx,%d",tuple_size(t)); CALLRT(f,"rt_tuple_new");
            E(f,"pop ecx"); E(f,"mov [eax+%d],ecx",tuple_off(t,0)); E(f,"pop ecx"); E(f,"mov [eax+%d],ecx",tuple_off(t,0)+4);
            E(f,"pop ecx"); E(f,"mov [eax+%d],ecx",tuple_off(t,1)); E(f,"sar ecx,31"); E(f,"mov [eax+%d],ecx",tuple_off(t,1)+4);
            return 1; }
        if(!strcmp(m,"ldexp")){ gen_as(f,a[1],TY_INT_T,0); push_value(f,TY_INT_T); farg(f,a[0]); E(f,"pop eax"); E(f,"pop edx"); CALLRT(f,"rt_ldexp"); return 0; }
        if(e->count==2){                                                   /* two floats */
            farg(f,a[0]); E(f,"sub esp,8"); E(f,"fstp qword [esp]");
            farg(f,a[1]); E(f,"fld qword [esp]"); E(f,"add esp,8");          /* st0 = x, st1 = y */
            if(!strcmp(m,"atan2")){ E(f,"fxch"); CALLRT(f,"rt_fatan2"); }      /* atan(st1/st0): st1 = x(=y arg), st0 = y(=x arg) */
            else if(!strcmp(m,"pow")) CALLRT(f,"rt_fpow");
            else if(!strcmp(m,"hypot")){ E(f,"fmul st0,st0"); E(f,"fxch"); E(f,"fmul st0,st0"); E(f,"faddp st1,st0"); E(f,"fsqrt"); }
            else if(!strcmp(m,"fmod")) CALLRT(f,"rt_fmod_c");
            else if(!strcmp(m,"copysign")){ int l=new_label(); E(f,"fabs"); E(f,"fxch"); E(f,"sub esp,8"); E(f,"fstp qword [esp]");   /* |x|, y's sign */
                E(f,"mov eax,[esp+4]"); E(f,"add esp,8"); E(f,"test eax,eax"); E(f,"jns L%d",l); E(f,"fchs"); LBL(f,l); }
            else { E(f,"fldln2"); E(f,"fxch"); E(f,"fyl2x"); E(f,"fxch"); E(f,"fldln2"); E(f,"fxch"); E(f,"fyl2x"); E(f,"fdivp st1,st0"); }  /* log(x, base) */
            return 0;
        }
        if(!strcmp(m,"isnan")||!strcmp(m,"isinf")||!strcmp(m,"isfinite")){         /* (comparisons: unordered for a NaN) */
            farg(f,a[0]);
            if(m[2]=='n' && m[3]=='a'){ E(f,"fld st0"); E(f,"fcompp"); E(f,"fnstsw ax"); E(f,"sahf"); E(f,"setp al"); }
            else { rt("rt_finf"); E(f,"fabs"); E(f,"fld qword [rt_finf]"); E(f,"fcompp"); E(f,"fnstsw ax"); E(f,"sahf");
                if(m[2]=='i') E(f,"sete al"); else E(f,"seta al");
                E(f,"setnp cl"); E(f,"and al,cl"); }
            E(f,"movzx eax,al"); return 0; }
        farg(f,a[0]);
        if(!strcmp(m,"sqrt")) E(f,"fsqrt");
        else if(!strcmp(m,"sin")) CALLRT(f,"rt_fsin");
        else if(!strcmp(m,"cos")) CALLRT(f,"rt_fcos");
        else if(!strcmp(m,"tan")) CALLRT(f,"rt_ftan");
        else if(!strcmp(m,"atan")) CALLRT(f,"rt_fatan");
        else if(!strcmp(m,"asin")) CALLRT(f,"rt_fasin");
        else if(!strcmp(m,"acos")) CALLRT(f,"rt_facos");
        else if(!strcmp(m,"exp")) CALLRT(f,"rt_fexp");
        else if(!strcmp(m,"log")) CALLRT(f,"rt_flog");
        else if(!strcmp(m,"log10")) CALLRT(f,"rt_flog10");
        else if(!strcmp(m,"log2")) CALLRT(f,"rt_flog2");
        else if(!strcmp(m,"fabs")) E(f,"fabs");
        else if(!strcmp(m,"degrees")) E(f,"fmul qword [FC%d]",float_const("57.29577951308232"));      /* 180/pi, as Python */
        else if(!strcmp(m,"radians")) E(f,"fmul qword [FC%d]",float_const("0.017453292519943295"));
        else cg_fail(e->line,"unsupported math function");
        return 0;
    }
    if(!strcmp(mod,"time")){
        if(!strcmp(m,"sleep")){ farg(f,a[0]); CALLRT(f,"rt_time_sleep"); return 0; }
        CALLRT(f,"rt_time_now"); return 0;
    }
    if(!strcmp(mod,"random")){
        if(!strcmp(m,"random")){ CALLRT(f,"rt_rand_float"); return 0; }
        if(!strcmp(m,"seed")){ gen(f,a[0]); CALLRT(f,"rt_rand_seed"); return 0; }
        if(!strcmp(m,"uniform")){ farg(f,a[0]); E(f,"sub esp,8"); E(f,"fstp qword [esp]"); farg(f,a[1]); E(f,"fsub qword [esp]");
            CALLRT(f,"rt_rand_float"); E(f,"fmulp st1,st0"); E(f,"fadd qword [esp]"); E(f,"add esp,8"); return 0; }
        if(!strcmp(m,"randint")||!strcmp(m,"randrange")){        /* [esp] lo, edx:eax hi (exclusive), ecx: 0 randrange(n), 1 randrange(a, b), 2 randint */
            if(e->count==1){ E(f,"push 0"); E(f,"push 0"); gen_as(f,a[0],TY_INT_T,0); E(f,"xor ecx,ecx"); }
            else { gen_as(f,a[0],TY_INT_T,0); push_value(f,TY_INT_T); gen_as(f,a[1],TY_INT_T,0); E(f,"mov ecx,%d",m[4]=='i'?2:1); }
            CALLRT(f,"rt_rand_range"); E(f,"add esp,8"); return 0; }
        if(!strcmp(m,"choice")){
            Ty *t=TY(a[0]);
            gen_borrow(f,a[0]); E(f,"push eax"); E(f,"push 0"); E(f,"push 0");
            E(f,"xor eax,eax"); E(f,"xor edx,edx"); E(f,"mov ecx,[esp+8]"); E(f,"test ecx,ecx"); int l=new_label(); E(f,"jz L%d",l);
            if(t->k==TY_STR){ E(f,"mov eax,ecx"); CALLRT(f,"rt_str_cplen"); E(f,"xor edx,edx"); } else E(f,"mov eax,[ecx+8]");
            LBL(f,l);
            E(f,"mov ecx,3"); CALLRT(f,"rt_rand_range"); E(f,"add esp,8"); E(f,"mov edx,eax"); E(f,"pop eax");
            if(t->k==TY_STR){ CALLRT(f,"rt_str_char"); return 0; }
            E(f,"mov ecx,%d",esize(t->elem)); CALLRT(f,"rt_list_at"); load_mem(f,t->elem,"eax",0); return 0; }
        if(!strcmp(m,"shuffle")){ Ty *t=TY(a[0]); gen_borrow(f,a[0]); E(f,"mov edx,%d",esize(t->elem)); CALLRT(f,"rt_shuffle"); return 0; }
    }
    cg_fail(e->line,"unsupported built-in module function");
}

