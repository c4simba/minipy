/* ========================= Typed compiler, code generator: built-in functions =========================
   key= functions (sorted, min, max), any / all, reduce, list(), type(),
   and the other built-in functions (gen_builtin_in). */

#include "gen.h"

/* ---- key= functions ---- */
/* key(element): the element (borrowed) in eax/st0 -> the key in eax/st0 (borrowed; an owned one is held) */
static void gen_key_call(F *f, Expr *key, Ty *elem){
    XInfo *ki=xinfo(key);
    if(key->kind==EXPR_LAMBDA){ store_borrowed(f,ki->var,elem); gen_borrow(f,key->a); return; }
    if(ki->kind!=X_FUNC){                                   /* a function value */
        Ty *ft=TY(key), *pt=ft->elems[0];
        conv_num(f,elem,pt); push_value(f,pt);
        gen_borrow(f,key); panic_if_null(f); E(f,"mov edx,eax"); E(f,"call dword [edx+8]"); E(f,"add esp,%d",esize(pt));
        if(is_ptr(ft->elem)) hold(f);
        return;
    }
    AFunc *fn=ki->fn; Ty *pt=fn->params[0]->ty; use_fn(fn);
    conv_num(f,elem,pt); push_value(f,pt);
    E(f,"call %s",fnl(fn,LB_CODE)); E(f,"add esp,%d",esize(pt));
    if(is_ptr(fn->ret)) hold(f);
}
static Ty *key_type(Expr *key){
    Ty *t=ty_find(xinfo(key)->ty);
    if(key->kind!=EXPR_LAMBDA && xinfo(key)->kind!=X_FUNC && t->k==TY_FUNC) return ty_find(t->elem);   /* a function value: its result */
    return t;
}
/* reverse= evaluated once -> a frame slot holding 0/1 (0 when absent) */
int gen_rev_flag(F *f, Expr *rev){
    int slot=frame_slot(f,4);
    if(rev){ gen_bool(f,rev); E(f,"mov [ebp%+d],eax",slot); } else E(f,"mov dword [ebp%+d],0",slot);
    return slot;
}
/* Sort the list at [ebp+src] by key: (key, index) pairs sorted, then the
   elements in that order -> eax = new list (owned); in place: swapped into src. */
int gen_sort_by_key(F *f, int src, Ty *el, Expr *key, int revslot, int inplace){
    Ty *kt=key_type(key); Ty *pe[2]={kt,TY_INT_T}; Ty *pt=ty_tuple(pe,2);
    int td=tdesc(pt), koff=12, ioff=12+esize(kt);
    int pairs=ref_slot(f), idx=frame_slot(f,4), res=ref_slot(f);
    int ltop=new_label(), lend=new_label(), l2=new_label(), l2end=new_label();
    new_list(f,pt); E(f,"mov [ebp%+d],eax",pairs);
    E(f,"mov dword [ebp%+d],0",idx);
    LBL(f,ltop);
    E(f,"mov eax,[ebp%+d]",src); E(f,"test eax,eax"); E(f,"jz L%d",lend);
    E(f,"mov ecx,[ebp%+d]",idx); E(f,"cmp ecx,[eax+8]"); E(f,"jae L%d",lend);
    int mark=scope_open(f);
    E(f,"mov eax,[eax+16]"); load_at(f,el,"eax","ecx",esize(el));
    gen_key_call(f,key,el); push_value(f,kt);
    E(f,"mov eax,TD%d",td); E(f,"mov edx,%d",tuple_size(pt)); CALLRT(f,"rt_tuple_new");
    E(f,"mov ecx,eax"); pop_value(f,kt);
    if(is_ptr(kt)){ E(f,"push ecx"); incref(f); E(f,"pop ecx"); }
    store_new(f,kt,"ecx",koff);
    { int l=new_label(); E(f,"mov eax,[ebp%+d]",idx); E(f,"cmp dword [ebp%+d],0",revslot); E(f,"je L%d",l); E(f,"neg eax"); LBL(f,l); }  /* reverse keeps equal keys in order */
    E(f,"cdq"); E(f,"mov [ecx+%d],eax",ioff); E(f,"mov [ecx+%d],edx",ioff+4);
    E(f,"push ecx"); E(f,"mov eax,[ebp%+d]",pairs); CALLRT(f,"rt_list_push"); E(f,"pop ecx"); E(f,"mov [eax],ecx");
    scope_close(f,mark);
    E(f,"inc dword [ebp%+d]",idx); E(f,"jmp L%d",ltop);
    LBL(f,lend);
    E(f,"mov eax,[ebp%+d]",pairs); CALLRT(f,"rt_list_sort");
    { int l=new_label(); E(f,"cmp dword [ebp%+d],0",revslot); E(f,"je L%d",l); E(f,"mov eax,[ebp%+d]",pairs); CALLRT(f,"rt_list_reverse"); LBL(f,l); }
    new_list(f,el); E(f,"mov [ebp%+d],eax",res);
    E(f,"mov dword [ebp%+d],0",idx);
    LBL(f,l2);
    E(f,"mov eax,[ebp%+d]",pairs); E(f,"mov ecx,[ebp%+d]",idx); E(f,"cmp ecx,[eax+8]"); E(f,"jae L%d",l2end);
    E(f,"mov eax,[eax+16]"); E(f,"mov eax,[eax+ecx*4]"); E(f,"mov ecx,[eax+%d]",ioff);
    { int l=new_label(); E(f,"test ecx,ecx"); E(f,"jns L%d",l); E(f,"neg ecx"); LBL(f,l); }
    E(f,"mov eax,[ebp%+d]",src); E(f,"mov eax,[eax+16]");
    load_at(f,el,"eax","ecx",esize(el)); if(is_ptr(el)) incref(f);
    E(f,"push dword [ebp%+d]",res);
    list_append_top(f,el);
    E(f,"add esp,4");
    E(f,"inc dword [ebp%+d]",idx); E(f,"jmp L%d",l2);
    LBL(f,l2end);
    E(f,"mov eax,[ebp%+d]",pairs); E(f,"mov dword [ebp%+d],0",pairs); CALLRT(f,"rt_decref");
    if(inplace){                                         /* the new order into the list itself */
        E(f,"mov eax,[ebp%+d]",src); E(f,"mov ecx,[ebp%+d]",res);
        E(f,"test eax,eax"); { int l=new_label(); E(f,"jz L%d",l);
        for(int k=8;k<=16;k+=4){ E(f,"mov edx,[eax+%d]",k); E(f,"xchg edx,[ecx+%d]",k); E(f,"mov [eax+%d],edx",k); }
        LBL(f,l); }
        E(f,"mov eax,[ebp%+d]",res); E(f,"mov dword [ebp%+d],0",res); CALLRT(f,"rt_decref");
        return 0;
    }
    E(f,"mov eax,[ebp%+d]",src); E(f,"mov dword [ebp%+d],0",src); CALLRT(f,"rt_decref");   /* the copy sorted */
    E(f,"mov eax,[ebp%+d]",res); E(f,"mov dword [ebp%+d],0",res);
    return 1;
}
/* min/max(xs, key=f): the first element with the smallest/largest key -> eax/st0 (owned) */
static int gen_minmax_key(F *f, Expr *e, int is_max){
    Expr *key=xinfo(e)->key; Ty *lt=TY(e->items[0]), *el=lt->k==TY_DICT?ty_dkey(lt):lt->elem, *kt=key_type(key);
    int src=ref_slot(f), idx=frame_slot(f,4), best=frame_slot(f,4), bkey=is_ptr(kt)?ref_slot(f):frame_slot(f,8);
    int ltop=new_label(), lend=new_label(), ltake=new_label(), lnext=new_label();
    if(lt->k==TY_LIST) gen_owned(f,e->items[0]); else gen_list_of(f,e->items[0],el);
    E(f,"mov [ebp%+d],eax",src);
    { int l=new_label(), lok=new_label(); E(f,"test eax,eax"); E(f,"jz L%d",l); E(f,"cmp dword [eax+8],0"); E(f,"jne L%d",lok); LBL(f,l);
      E(f,"mov esi,Z%d",zlit(is_max?"max() iterable argument is empty":"min() iterable argument is empty")); CALLRT(f,"rt_panic_value"); LBL(f,lok); }
    E(f,"mov dword [ebp%+d],0",idx); E(f,"mov dword [ebp%+d],-1",best);
    LBL(f,ltop);
    E(f,"mov eax,[ebp%+d]",src); E(f,"mov ecx,[ebp%+d]",idx); E(f,"cmp ecx,[eax+8]"); E(f,"jae L%d",lend);
    int mark=scope_open(f);
    E(f,"mov eax,[eax+16]"); load_at(f,el,"eax","ecx",esize(el));
    gen_key_call(f,key,el);
    E(f,"cmp dword [ebp%+d],-1",best); E(f,"je L%d",ltake);
    if(is_flt(kt)){ E(f,"fld qword [ebp%+d]",bkey); E(f,"fcomp st1"); E(f,"fnstsw ax"); E(f,"sahf"); E(f,"j%s L%d",is_max?"b":"a",ltake); E(f,"fstp st0"); E(f,"jmp L%d",lnext); }
    else {                                                  /* the key type's ordering: cmp(key, best) */
        push_value(f,kt); E(f,"mov eax,esp"); E(f,"lea edx,[ebp%+d]",bkey); E(f,"mov ecx,%s",kd_of(kt)); E(f,"call dword [ecx+12]");
        E(f,"mov ecx,eax"); pop_value(f,kt); E(f,"cmp ecx,0");
        E(f,"j%s L%d",is_max?"g":"l",ltake); E(f,"jmp L%d",lnext);
    }
    LBL(f,ltake);
    if(is_ptr(kt)){ incref(f); E(f,"xchg eax,[ebp%+d]",bkey); CALLRT(f,"rt_decref"); }
    else store_new(f,kt,"ebp",bkey);
    E(f,"mov eax,[ebp%+d]",idx); E(f,"mov [ebp%+d],eax",best);
    LBL(f,lnext);
    scope_close(f,mark);
    E(f,"inc dword [ebp%+d]",idx); E(f,"jmp L%d",ltop);
    LBL(f,lend);
    E(f,"mov eax,[ebp%+d]",src); E(f,"mov ecx,[ebp%+d]",best); E(f,"mov eax,[eax+16]");
    load_at(f,el,"eax","ecx",esize(el)); if(is_ptr(el)) incref(f);
    if(!is_flt(el)) push_value(f,el);                                         /* the sequence is released */
    E(f,"mov eax,[ebp%+d]",src); E(f,"mov dword [ebp%+d],0",src); CALLRT(f,"rt_decref");
    if(!is_flt(el)) pop_value(f,el);
    return is_ptr(el);
}

/* any/all of a generator expression: its loops, stopping at the first deciding item */
static int gen_comp_anyall(F *f, Expr *e, int isall){
    int res=frame_slot(f,4), lfound=new_label(), ldone=new_label();
    E(f,"mov dword [ebp%+d],%d",res,isall);
    Iter its[8]; int n=e->nclause;
    if(n>8) cg_fail(e->line,"too many for clauses");
    for(int i=0;i<n;i++){
        CompClause *cl=&e->clauses[i];
        iter_begin(f,cl->iter,&its[i]);
        iter_store_vars(f,&its[i],&xinfo(e)->cvars[2*i],cl->nvars); comp_unpack(f,e,i);
        for(int k=0;k<cl->ncond;k++) gen_jump(f,cl->conds[k],its[i].cont,0);
    }
    gen_jump(f,e->a,lfound,!isall);
    for(int i=n-1;i>=0;i--){ iter_end(f,&its[i]); LBL(f,its[i].exit); iter_release(f,&its[i]); }
    E(f,"jmp L%d",ldone);
    LBL(f,lfound);
    E(f,"mov dword [ebp%+d],%d",res,!isall);
    for(int i=0;i<n;i++) iter_release(f,&its[i]);
    LBL(f,ldone);
    E(f,"mov eax,[ebp%+d]",res);
    return 0;
}
/* functools.reduce(f, xs[, init]) */
static int gen_reduce(F *f, Expr *e, XInfo *xi){
    Ty *lt=TY(e->items[1]), *el=lt->elem; AVar *acc=xi->cvars[0], *x=xi->cvars[1]; Ty *at=acc->ty;
    int src=ref_slot(f), idx=frame_slot(f,4), ltop=new_label(), lend=new_label();
    gen_owned(f,e->items[1]); E(f,"mov [ebp%+d],eax",src);
    if(e->count==3){ gen_as(f,e->items[2],at,1); store_var(f,acc); E(f,"mov dword [ebp%+d],0",idx); }
    else {
        int lok=new_label(), lempty=new_label();
        E(f,"mov eax,[ebp%+d]",src); E(f,"test eax,eax"); E(f,"jz L%d",lempty); E(f,"cmp dword [eax+8],0"); E(f,"jne L%d",lok);
        LBL(f,lempty); E(f,"mov esi,Z%d",zlit("reduce() of empty iterable with no initial value")); CALLRT(f,"rt_panic_type");
        LBL(f,lok); E(f,"mov eax,[eax+16]"); load_mem(f,el,"eax",0); store_borrowed(f,acc,el); E(f,"mov dword [ebp%+d],1",idx);
    }
    LBL(f,ltop);
    E(f,"mov eax,[ebp%+d]",src); E(f,"mov ecx,[ebp%+d]",idx); E(f,"cmp ecx,[eax+8]"); E(f,"jae L%d",lend);
    E(f,"mov eax,[eax+16]"); load_at(f,el,"eax","ecx",esize(el));
    store_borrowed(f,x,el);
    { int mark=scope_open(f); gen_as(f,xi->key,at,1); store_var(f,acc); scope_close(f,mark); }
    E(f,"inc dword [ebp%+d]",idx); E(f,"jmp L%d",ltop);
    LBL(f,lend);
    E(f,"mov eax,[ebp%+d]",src); E(f,"mov dword [ebp%+d],0",src); CALLRT(f,"rt_decref");
    load_var(f,acc); if(is_ptr(at)) incref(f);
    return is_ptr(at);
}
/* list(range(...)) of the range call r -> eax (owned) */
static void gen_range_list(F *f, Expr *r){
    E(f,"sub esp,24");                                   /* start, stop, step: 8 bytes each */
    Expr *lo=r->count>=2?r->items[0]:NULL, *hi=r->count>=2?r->items[1]:r->items[0], *st=r->count==3?r->items[2]:NULL;
    if(lo){ gen_as(f,lo,TY_INT_T,0); put_arg(f,TY_INT_T,0); } else { E(f,"mov dword [esp],0"); E(f,"mov dword [esp+4],0"); }
    gen_as(f,hi,TY_INT_T,0); put_arg(f,TY_INT_T,8);
    if(st){ gen_as(f,st,TY_INT_T,0); put_arg(f,TY_INT_T,16); } else { E(f,"mov dword [esp+16],1"); E(f,"mov dword [esp+20],0"); }
    CALLRT(f,"rt_range_list"); E(f,"add esp,24");
}
/* eax = a new list of x's items: a list copied, a dict's keys, a str's
   characters, a set's elements (in its order), what a generator yields */
void gen_list_of(F *f, Expr *x, Ty *elem){
    Ty *t=TY(x);
    gen_borrow(f,x);
    switch(t->k){
        case TY_DICT: CALLRT(f,"rt_dict_keys"); break;
        case TY_STR: CALLRT(f,"rt_str_chars"); break;
        case TY_BYTES: CALLRT(f,"rt_bytes_list"); break;
        case TY_SET: E(f,"mov edx,%s",kd_of(t->elem)); CALLRT(f,"rt_set_to_list"); break;
        case TY_GEN: E(f,"mov edx,%s",kd_of(elem)); CALLRT(f,"rt_gen_drain"); break;
        case TY_TUPLE:{                                     /* a tuple's items, one by one */
            E(f,"push eax"); new_list(f,elem); E(f,"push eax");
            for(int i=0;i<t->nelems;i++){ Ty *et=t->elems[i];
                E(f,"mov eax,[esp+4]"); load_mem(f,et,"eax",tuple_off(t,i)); if(is_ptr(et)) incref(f);
                conv_num(f,et,elem); list_append_top(f,elem); }
            E(f,"pop eax"); E(f,"add esp,4"); break; }
        default: E(f,"mov edx,%s",kd_of(t->elem)); CALLRT(f,"rt_list_copy"); break;
    }
}
/* type(x) -> eax: the class of an object, else the descriptor of x's built-in type */
void gen_type_of(F *f, Expr *x){
    Ty *t=TY(x);
    if(is_none_lit(x)){ E(f,"mov eax,%s",btype_label("NoneType")); return; }
    if(t->k==TY_OBJ){
        gen_borrow(f,x);
        if(is_opt(t)){ int l=new_label(), l2=new_label(); E(f,"test eax,eax"); E(f,"jnz L%d",l); E(f,"mov eax,%s",btype_label("NoneType")); E(f,"jmp L%d",l2); LBL(f,l); E(f,"mov eax,[eax+8]"); LBL(f,l2); }
        else E(f,"mov eax,[eax+8]");
        return; }
    int o=gen(f,x);
    Ty plain=*ty_find(t); plain.opt=0;
    const char *lbl=btype_label(py_type_name(&plain));
    if(is_opt(t)){ int lnone=new_label(), lend=new_label();
        jump_if_none(f,t,lnone); drop_value(f,t,o); E(f,"mov eax,%s",lbl); E(f,"jmp L%d",lend);
        LBL(f,lnone); drop_value(f,t,o); E(f,"mov eax,%s",btype_label("NoneType")); LBL(f,lend); return; }
    drop_value(f,t,o); E(f,"mov eax,%s",lbl);
}
/* the generator in [ebp+gslot] finished: StopIteration, its return value as .value (and the message) */
void gen_raise_stop(F *f, int gslot, Ty *gt){
    if(ty_find(gt)->tup){                                  /* an async generator: StopAsyncIteration */
        AClass *sa=NULL; for(int i=0;i<gg->p->nclasses;i++) if(gg->p->classes[i]->builtin && !strcmp(gg->p->classes[i]->name,"StopAsyncIteration")) sa=gg->p->classes[i];
        use_class(sa); E(f,"push 0");
        E(f,"mov eax,%d",sa->size); E(f,"mov edx,%s",class_label(sa,0)); E(f,"mov ecx,%s",class_label(sa,1)); CALLRT(f,"rt_obj_new");
        E(f,"pop ecx"); E(f,"mov [eax+12],ecx"); CALLRT(f,"rt_throw"); return; }
    AClass *si=NULL; for(int i=0;i<gg->p->nclasses;i++) if(gg->p->classes[i]->builtin && !strcmp(gg->p->classes[i]->name,"StopIteration")) si=gg->p->classes[i];
    use_class(si);
    AField *vf=aot_find_field(si,"value"); Ty *vt=ty_find(vf->ty), *gr=gpart(gt,1);
    int lnone=new_label(), lgo=new_label();
    if(gr->k!=TY_VOID){
        E(f,"mov ecx,[ebp%+d]",gslot); E(f,"cmp dword [ecx+108],0"); E(f,"je L%d",lnone);
        rt("rt_sb_need"); E(f,"push dword [rt_sb_len]"); E(f,"mov ecx,[ebp%+d]",gslot); load_mem(f,gr,"ecx",100); gen_fmt(f,gr,0); E(f,"pop eax"); CALLRT(f,"rt_sb_take");
        E(f,"jmp L%d",lgo); }
    LBL(f,lnone); E(f,"xor eax,eax");
    LBL(f,lgo); E(f,"push eax");
    E(f,"mov eax,%d",si->size); E(f,"mov edx,%s",class_label(si,0)); E(f,"mov ecx,%s",class_label(si,1)); CALLRT(f,"rt_obj_new");
    E(f,"pop ecx"); E(f,"mov [eax+12],ecx"); E(f,"push eax");
    int l2=new_label(), l3=new_label();
    if(gr->k!=TY_VOID){ E(f,"mov ecx,[ebp%+d]",gslot); E(f,"cmp dword [ecx+108],0"); E(f,"je L%d",l2);
        load_mem(f,gr,"ecx",100); if(is_ptr(gr)) incref(f); conv(f,gr,vt); E(f,"jmp L%d",l3); }
    LBL(f,l2); gen_none(f,vt);
    LBL(f,l3); E(f,"mov ecx,[esp]"); store_new(f,vt,"ecx",vf->offset); E(f,"pop eax");
    CALLRT(f,"rt_throw");
}
static int gen_builtin_in(F *f, Expr *e, XInfo *xi);
int gen_builtin(F *f, Expr *e, XInfo *xi){
    const char *save=g_none_text; int o=gen_builtin_in(f,e,xi); g_none_text=save; return o;
}
static int gen_builtin_in(F *f, Expr *e, XInfo *xi){
    const char *n=xi->name; Ty *t=TY(e);
    Expr *a0=e->count?e->items[0]:NULL; Ty *t0=a0?TY(a0):NULL;
    if(!strcmp(n,"reduce")) return gen_reduce(f,e,xi);
    if(!strcmp(n,"downcast")) return gen(f,e->items[1]);                  /* (the same object, known to be of a subclass) */
    if(!strcmp(n,"aiter")) return gen(f,a0);
    if(!strncmp(n,"thread.",7)){ const char *m=n+7;                       /* threads: tasks */
        if(!strcmp(m,"start")){ gen_task_new(f,xi->key); rt("rt_time_sleep"); rt("rt_thread_sleep");
            E(f,"mov dword [rt_sleep_hook],rt_thread_sleep"); return 1; }          /* (time.sleep() lets the threads run from now on) */      /* (at the end the program does not wait for them, as the interpreter) */
        if(!strcmp(m,"join")){ gen_borrow(f,a0); CALLRT(f,"rt_thread_join"); return 0; }
        if(!strcmp(m,"lock")){ CALLRT(f,"rt_thread_lock"); return 0; }
        if(!strcmp(m,"acquire")||!strcmp(m,"release")){ gen(f,a0); to_i32(f,TY(a0)); CALLRT(f,m[0]=='a'?"rt_thread_acquire":"rt_thread_release"); return 0; }
        if(!strcmp(m,"sleep")){ gen_as(f,a0,TY_FLOAT_T,0); CALLRT(f,"rt_thread_sleep"); return 0; }
        if(!strcmp(m,"current")){ CALLRT(f,"rt_thread_current"); return 0; } }
    if(!strcmp(n,"next")||!strcmp(n,"anext")){
        Ty *el=t; int slot=frame_slot(f,4), lmiss=new_label(), lend=new_label();
        gen_borrow(f,a0); panic_if_null(f); E(f,"mov [ebp%+d],eax",slot);
        CALLRT(f,"rt_gen_next"); E(f,"test eax,eax"); E(f,"jz L%d",lmiss);
        E(f,"mov eax,[ebp%+d]",slot); load_mem(f,el,"eax",24); if(is_ptr(el)) incref(f);
        E(f,"jmp L%d",lend);
        LBL(f,lmiss);
        if(e->count==2){ gen_as(f,e->items[1],el,1); }
        else gen_raise_stop(f,slot,TY(a0));
        LBL(f,lend);
        return is_ptr(el);
    }
    if((!strcmp(n,"any")||!strcmp(n,"all")) && a0->kind==EXPR_COMPREHENSION && a0->comp_kind=='g') return gen_comp_anyall(f,a0,n[1]=='l');
    if(!strcmp(n,"len") && t0->k==TY_TUPLE){ int o=gen(f,a0); drop_value(f,t0,o); E(f,"mov eax,%d",t0->nelems); widen(f,0); return 0; }
    if(!strcmp(n,"divmod")){
        Ty *qt=t->elems[0];
        E(f,"mov eax,TD%d",tdesc(t)); E(f,"mov edx,%d",tuple_size(t)); CALLRT(f,"rt_tuple_new"); E(f,"push eax");
        gen_as(f,a0,qt,0); push_value(f,qt); gen_as(f,e->items[1],qt,0); push_value(f,qt);   /* [esp] b, then a, then the tuple */
        int sz=esize(qt);
        if(is_flt(qt)){
            E(f,"fld qword [esp]"); E(f,"fld qword [esp+8]"); CALLRT(f,"rt_ffloordiv"); E(f,"mov ecx,[esp+16]"); E(f,"fstp qword [ecx+12]");
            E(f,"fld qword [esp+8]"); E(f,"fld qword [esp]"); E(f,"fxch"); CALLRT(f,"rt_fmod"); E(f,"mov ecx,[esp+16]"); E(f,"fstp qword [ecx+20]");
        } else {                                            /* [esp] b, [esp+8] a, [esp+16] the tuple */
            E(f,"push dword [esp+12]"); E(f,"push dword [esp+12]"); E(f,"mov eax,[esp+8]"); E(f,"mov edx,[esp+12]");
            CALLRT(f,"rt_idivmod"); E(f,"add esp,8");
            E(f,"mov ecx,[esp+16]"); E(f,"mov [ecx+12],eax"); E(f,"mov [ecx+16],edx");
            E(f,"mov eax,[rt_scratch]"); E(f,"mov [ecx+20],eax"); E(f,"mov eax,[rt_scratch+4]"); E(f,"mov [ecx+24],eax");
        }
        E(f,"add esp,%d",2*sz); E(f,"pop eax");
        return 1;
    }
    if(!strcmp(n,"len")){
        if(xi->fn){ E(f,"sub esp,4"); gen_borrow(f,a0); E(f,"mov [esp],eax"); call_method_on_top(f,xi->fn); E(f,"add esp,4"); return 0; }
        int l=new_label(); gen_borrow(f,a0); check_none(f,t0,"TypeError","object of type 'NoneType' has no len()");
        if(t0->k==TY_STR){ CALLRT(f,"rt_str_cplen"); widen(f,0); return 0; }
        E(f,"test eax,eax"); E(f,"jz L%d",l); E(f,"mov eax,[eax+8]"); LBL(f,l); widen(f,0); return 0;
    }
    if(!strcmp(n,"str_decode")){ gen_codec_call(f,a0,e->items[1],e->count>2?e->items[2]:NULL,NULL,"rt_bytes_decode"); return 1; }
    if(!strcmp(n,"bytes_empty")){ E(f,"xor eax,eax"); CALLRT(f,"rt_bytes_new"); return 1; }
    if(!strcmp(n,"bytes_encode")){ gen_codec_call(f,a0,e->items[1],e->count>2?e->items[2]:NULL,NULL,"rt_str_encode"); return 1; }
    if(!strcmp(n,"bytes_zeros")){ gen(f,a0); to_i32(f,t0); CALLRT(f,"rt_bytes_new"); return 1; }
    if(!strcmp(n,"bytes_same")) return gen(f,a0);
    if(!strcmp(n,"bytes_list")){ gen_borrow(f,a0);
        if(t0->k==TY_SET){ E(f,"mov edx,%s",kd_of(t0->elem)); CALLRT(f,"rt_set_to_list"); hold(f); }
        CALLRT(f,"rt_bytes_from_list"); return 1; }
    if(!strcmp(n,"bytes.fromhex")){ gen_borrow(f,a0); CALLRT(f,"rt_bytes_fromhex"); return 1; }
    if(!strcmp(n,"int.from_bytes")){
        gen_borrow(f,a0); E(f,"push eax");
        gen_byteorder(f,e->count>1?e->items[1]:NULL); E(f,"push edx");
        if(e->count>2) gen(f,e->items[2]); else E(f,"xor eax,eax");
        E(f,"mov ecx,eax"); E(f,"pop edx"); E(f,"pop eax"); CALLRT(f,"rt_int_from_bytes"); return 0; }
    if(!strcmp(n,"str")||!strcmp(n,"repr")){
        if(!a0){ E(f,"xor eax,eax"); return 0; }
        if(t0->k==TY_STR && !t0->opt && n[0]=='s') return gen(f,a0);
        rt("rt_sb_need"); E(f,"push dword [rt_sb_len]");
        gen_borrow(f,a0); gen_fmt(f,t0,n[0]=='r');
        E(f,"pop eax"); CALLRT(f,"rt_sb_take"); return 1;
    }
    if(!strcmp(n,"int")||!strcmp(n,"float")) g_none_text=n[0]=='i'?"int() argument must be a string, a bytes-like object or a real number, not 'NoneType'":"float() argument must be a string or a real number, not 'NoneType'";
    if(!strcmp(n,"abs")) g_none_text="bad operand type for abs(): 'NoneType'";
    if(!strcmp(n,"round")) g_none_text="type NoneType doesn't define __round__ method";
    if(!strcmp(n,"int")){
        if(!a0){ gen_int_const(f,TY_INT_T,0); return 0; }
        if(!g_none_text) g_none_text="int() argument must be a string, a bytes-like object or a real number, not 'NoneType'";
        if(t0->k==TY_FLOAT){ gen(f,a0); CALLRT(f,"rt_ftoi"); return 0; }
        if(e->count==2 || t0->k==TY_BYTES){                                       /* int(s, base), int(b"12") */
            if(e->count==2){ gen_as(f,e->items[1],TY_INT_T,0); to_i32(f,TY_INT_T); E(f,"push eax"); } else E(f,"push 10");
            gen_borrow(f,a0); check_none(f,t0,"TypeError",g_none_text);
            E(f,"pop ecx"); E(f,"mov edx,%d",t0->k==TY_BYTES); CALLRT(f,"rt_int_parse_base"); return 0; }
        if(t0->k==TY_STR){ gen_borrow(f,a0); check_none(f,t0,"TypeError",g_none_text); CALLRT(f,"rt_int_parse"); return 0; }
        gen_as(f,a0,TY_INT_T,0); return 0;
    }
    if(!strcmp(n,"float")){
        if(!a0){ E(f,"fldz"); return 0; }
        if(t0->k==TY_STR){ gen_borrow(f,a0); check_none(f,t0,"TypeError",g_none_text); CALLRT(f,"rt_float_parse"); return 0; }
        gen_as(f,a0,TY_FLOAT_T,0); return 0;
    }
    if(!strcmp(n,"bool")){ if(!a0) E(f,"xor eax,eax"); else gen_bool(f,a0); return 0; }
    if(!strcmp(n,"abs")){ if(t0->k==TY_FLOAT){ gen(f,a0); E(f,"fabs"); } else { gen_as(f,a0,TY_INT_T,0); CALLRT(f,"rt_iabs"); } return 0; }
    if((!strcmp(n,"min")||!strcmp(n,"max")) && xi->key) return gen_minmax_key(f,e,n[1]=='a');
    if(!strcmp(n,"min")||!strcmp(n,"max")){
        int mx=n[1]=='a';
        if(e->count==1){                                   /* of a container: the address of the element */
            gen_borrow(f,a0);
            if(t0->k==TY_DICT){ CALLRT(f,"rt_dict_keys"); hold(f); }
            else if(t0->k==TY_SET){ E(f,"mov edx,%s",kd_of(t0->elem)); CALLRT(f,"rt_set_to_list"); hold(f); }
            else if(t0->k==TY_STR){ CALLRT(f,"rt_str_chars"); hold(f); }
            E(f,"mov edx,%d",mx); CALLRT(f,"rt_list_minmax"); load_mem(f,t,"eax",0); return 0; }
        int slot=frame_slot(f,8);
        gen_as(f,a0,t,0); store_new(f,t,"ebp",slot);
        for(int i=1;i<e->count;i++){
            int skip=new_label();
            gen_as(f,e->items[i],t,0);
            if(is_flt(t)){
                int take=new_label();
                E(f,"fld qword [ebp%+d]",slot); E(f,"fcomp st1"); E(f,"fnstsw ax"); E(f,"sahf");   /* best vs x */
                E(f,"j%s L%d",mx?"b":"a",take); E(f,"fstp st0"); E(f,"jmp L%d",skip);
                LBL(f,take); E(f,"fstp qword [ebp%+d]",slot);
            } else if(is_lng(t)){                               /* the first of equals stays */
                if(mx){ E(f,"mov ecx,[ebp%+d]",slot); E(f,"sub ecx,eax"); E(f,"mov ecx,[ebp%+d]",slot+4); E(f,"sbb ecx,edx"); E(f,"jge L%d",skip); }
                else { E(f,"cmp eax,[ebp%+d]",slot); E(f,"mov ecx,edx"); E(f,"sbb ecx,[ebp%+d]",slot+4); E(f,"jge L%d",skip); }
                store_new(f,t,"ebp",slot);
            } else if(ty_find(t)->k==TY_BOOL){ E(f,"cmp eax,[ebp%+d]",slot); E(f,"j%s L%d",mx?"le":"ge",skip); E(f,"mov [ebp%+d],eax",slot); }
            else {                                              /* others: the type's ordering */
                E(f,"push eax"); E(f,"mov eax,esp"); E(f,"lea edx,[ebp%+d]",slot); E(f,"mov ecx,%s",kd_of(t)); E(f,"call dword [ecx+12]");
                E(f,"pop ecx"); E(f,"cmp eax,0"); E(f,"j%s L%d",mx?"le":"ge",skip); E(f,"mov [ebp%+d],ecx",slot);
            }
            LBL(f,skip);
        }
        load_mem(f,t,"ebp",slot);
        return 0;
    }
    if(!strcmp(n,"ord")){ gen_borrow(f,a0); CALLRT(f,"rt_ord"); widen(f,0); return 0; }
    if(!strcmp(n,"id")){                                    /* an object's address (an int, a bool: its value) */
        if(is_ptr(t0)){ gen_borrow(f,a0); widen(f,0); return 0; }
        gen_borrow(f,a0); if(esize(t0)<8) widen(f,1); return 0; }
    if(!strcmp(n,"hash")){                                  /* the type's hash (the interpreter's numbers) */
        gen_borrow(f,a0); push_value(f,t0); E(f,"mov eax,esp"); E(f,"mov ecx,%s",kd_of(t0)); E(f,"call dword [ecx+16]"); E(f,"add esp,%d",esize(t0)); return 0; }
    if(!strcmp(n,"open")){
        gen_borrow(f,a0); E(f,"push eax");
        if(e->count==2) gen_borrow(f,e->items[1]); else E(f,"xor eax,eax");
        E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_file_open"); return 1;
    }
    if(!strcmp(n,"any")||!strcmp(n,"all")){
        int isall=n[1]=='l', res=frame_slot(f,4), ldone=new_label();
        E(f,"mov dword [ebp%+d],%d",res,isall);
        Iter I; iter_begin(f,a0,&I);
        iter_value(f,&I,0); truth(f,I.elem);
        E(f,"test eax,eax"); E(f,"j%s L%d",isall?"nz":"z",I.cont);
        E(f,"mov dword [ebp%+d],%d",res,!isall); E(f,"jmp L%d",ldone);
        iter_end(f,&I); LBL(f,I.exit); LBL(f,ldone); iter_release(f,&I);
        E(f,"mov eax,[ebp%+d]",res); return 0;
    }
    if(!strcmp(n,"reversed")){
        if(t0->k==TY_TUPLE) cg_fail(e->line,"reversed() of a tuple: use a list");
        gen_list_of(f,a0,t->elem);
        E(f,"push eax"); CALLRT(f,"rt_list_reverse"); E(f,"pop eax");
        return 1;
    }
    if(!strcmp(n,"hex")||!strcmp(n,"bin")||!strcmp(n,"oct")||!strcmp(n,"format")){
        Spec sp; memset(&sp,0,sizeof sp); sp.prec=-1;
        if(n[0]=='f'){ if(e->count==2){ const char *ss=e->items[1]->tok->text; if(parse_spec(ss,(int)strlen(ss),&sp)!=(int)strlen(ss)) cg_fail(e->line,"invalid format spec"); } }
        else { sp.alt=1; sp.type=n[0]=='h'?'x':n[0]=='b'?'b':'o'; }
        rt("rt_sb_need"); E(f,"push dword [rt_sb_len]");
        ArgSrc src={a0,NULL,0,0}; emit_field(f,&sp,t0,&src,0,1,e->line);
        E(f,"pop eax"); CALLRT(f,"rt_sb_take"); return 1;
    }
    if(!strcmp(n,"pow")){
        if(e->count==3){                                    /* [esp] base, [esp+8] exp, the modulus in edx:eax */
            gen_as(f,e->items[1],TY_INT_T,0); push_value(f,TY_INT_T); gen_as(f,a0,TY_INT_T,0); push_value(f,TY_INT_T);
            gen_as(f,e->items[2],TY_INT_T,0); CALLRT(f,"rt_ipowmod"); E(f,"add esp,16"); return 0; }
        int num=1; (void)num;
        gen_as(f,a0,t,0); push_value(f,t); gen_as(f,e->items[1],t,0);
        return apply_binop(f,T_POWER,t,t,TY(e->items[1]),e->line);
    }
    if(!strcmp(n,"chr")){ gen(f,a0); to_i32(f,t0); CALLRT(f,"rt_chr"); return 0; }
    if(!strcmp(n,"sum")){
        gen_borrow(f,a0); if(t0->k==TY_SET){ E(f,"mov edx,%s",kd_of(t0->elem)); CALLRT(f,"rt_set_to_list"); hold(f); }
        if(is_lng(t0->elem)) CALLRT(f,"rt_list_isum");
        else { CALLRT(f,"rt_list_sum"); if(!is_flt(t0->elem)) widen(f,0); }     /* floats; bools counted */
        return 0; }
    if(!strcmp(n,"sorted") && (xi->key||xi->rev)){
        int rev=gen_rev_flag(f,xi->rev), src=ref_slot(f);
        if(xi->key && t0->k==TY_LIST){ gen_borrow(f,a0); incref(f); }     /* sorting by key makes a new list anyway */
        else gen_list_of(f,a0,t->elem);
        E(f,"mov [ebp%+d],eax",src);
        if(xi->key) return gen_sort_by_key(f,src,t->elem,xi->key,rev,0);
        CALLRT(f,"rt_list_sort");
        { int l=new_label(); E(f,"cmp dword [ebp%+d],0",rev); E(f,"je L%d",l); E(f,"mov eax,[ebp%+d]",src); CALLRT(f,"rt_list_reverse"); LBL(f,l); }
        E(f,"mov eax,[ebp%+d]",src); E(f,"mov dword [ebp%+d],0",src); return 1;
    }
    if(!strcmp(n,"sorted")){
        gen_list_of(f,a0,t->elem);
        E(f,"push eax"); CALLRT(f,"rt_list_sort"); E(f,"pop eax");
        return 1;
    }
    if(!strcmp(n,"type")){ gen_type_of(f,a0); return 0; }
    if(!strcmp(n,"issubclass")){
        Expr *k=e->items[1]; int nk=k->kind==EXPR_TUPLE?k->count:1;
        gen(f,a0); E(f,"push eax"); E(f,"push 0");
        for(int i=0;i<nk;i++){ XInfo *ki=xinfo(k->kind==EXPR_TUPLE?k->items[i]:k);
            if(ki->kind!=X_TYPEVAL){                         /* a class value: when running */
                gen(f,k); E(f,"mov edx,eax"); E(f,"mov eax,[esp+4]"); CALLRT(f,"rt_issubclass"); E(f,"or [esp],eax"); continue; }
            if(!ki->cls && !strcmp(ki->name,"object")){ E(f,"mov dword [esp],1"); continue; }
            E(f,"mov eax,[esp+4]"); E(f,"mov edx,%s",typeval_label(ki)); CALLRT(f,"rt_issubclass"); E(f,"or [esp],eax"); }
        E(f,"pop eax"); E(f,"add esp,4"); return 0;
    }
    if(!strcmp(n,"isinstance")){
        Expr *k=e->items[1]; int nk=k->kind==EXPR_TUPLE?k->count:1;
        if(t0->k==TY_OBJ){                                     /* an object: its class (None: an instance of nothing here) */
            gen_borrow(f,a0); E(f,"push eax"); E(f,"push 0");
            for(int i=0;i<nk;i++){ XInfo *ki=xinfo(k->kind==EXPR_TUPLE?k->items[i]:k);
                if(ki->kind!=X_TYPEVAL){                         /* a class value: its descriptor, when running */
                    gen(f,k->kind==EXPR_TUPLE?k->items[i]:k); E(f,"mov edx,eax"); E(f,"mov eax,[esp+4]"); CALLRT(f,"rt_isinstance"); E(f,"or [esp],eax");
                    continue; }
                if(!ki->cls){ if(!strcmp(ki->name,"object")){ int l=new_label(); E(f,"cmp dword [esp+4],0"); E(f,"je L%d",l); E(f,"mov dword [esp],1"); LBL(f,l); }
                    else if(!strcmp(ki->name,"NoneType")){ int l=new_label(); E(f,"cmp dword [esp+4],0"); E(f,"jne L%d",l); E(f,"mov dword [esp],1"); LBL(f,l); }
                    continue; }
                for(int q=-1;q<ki->cls->ninsts;q++){ AClass *kc=q<0?ki->cls:ki->cls->insts[q];   /* (a generic class: any of its instances) */
                    use_class(kc);
                    E(f,"mov eax,[esp+4]"); E(f,"mov edx,%s",class_label(kc,0)); CALLRT(f,"rt_isinstance"); E(f,"or [esp],eax"); }
                for(int q=0;q<gg->p->nclasses;q++){ AClass *kc=gg->p->classes[q];       /* a mixin: the classes it was copied into */
                    for(int v=0;v<kc->nvbases;v++) if(kc->vbases[v]==ki->cls){ use_class(kc);
                        E(f,"mov eax,[esp+4]"); E(f,"mov edx,%s",class_label(kc,0)); CALLRT(f,"rt_isinstance"); E(f,"or [esp],eax"); } } }
            E(f,"pop eax"); E(f,"add esp,4"); return 0;
        }
        int match=0, none_ok=0;                                 /* a value of a built-in type: known but for None */
        for(int i=0;i<nk;i++){ XInfo *ki=xinfo(k->kind==EXPR_TUPLE?k->items[i]:k);
            if(ki->cls) continue;
            if(!strcmp(ki->name,"NoneType")) none_ok=1; else if(btype_match(t0,ki->name)) match=1; }
        if(!is_opt(t0) && !is_none_lit(a0)){ int o=gen(f,a0); drop_value(f,t0,o); E(f,"mov eax,%d",match); return 0; }
        if(is_none_lit(a0)){ E(f,"mov eax,%d",none_ok); return 0; }
        int o=gen(f,a0); int lnone=new_label(), lend=new_label();
        jump_if_none(f,t0,lnone); drop_value(f,t0,o); E(f,"mov eax,%d",match); E(f,"jmp L%d",lend);
        LBL(f,lnone); drop_value(f,t0,o); E(f,"mov eax,%d",none_ok); LBL(f,lend); return 0;
    }
    if(!strcmp(n,"input")){ if(a0) gen_borrow(f,a0); else E(f,"xor eax,eax"); CALLRT(f,"rt_input"); return 1; }
    if(!strcmp(n,"round")){
        if(e->count==1){ gen_as(f,a0,TY_FLOAT_T,0); CALLRT(f,"rt_fround"); return 0; }
        gen_as(f,a0,TY_FLOAT_T,0); E(f,"sub esp,8"); E(f,"fstp qword [esp]");
        gen(f,e->items[1]); to_i32(f,TY(e->items[1])); E(f,"fld qword [esp]"); E(f,"add esp,8"); CALLRT(f,"rt_fround_n"); return 0;
    }
    if(!strcmp(n,"list")){
        if(!a0){ new_list(f,t->elem); return 1; }
        XInfo *ai=xinfo(a0);
        if(ai->kind==X_BUILTIN && ai->name && !strcmp(ai->name,"range")){ gen_range_list(f,a0); return 1; }
        gen_list_of(f,a0,t->elem);
        return 1;
    }
    if(!strcmp(n,"set")){
        if(!a0){ new_set(f,t->elem); return 1; }
        if(t0->k==TY_SET){ gen_borrow(f,a0); E(f,"mov edx,%s",kd_of(t->elem)); CALLRT(f,"rt_set_copy"); return 1; }   /* CPython: merged */
        if(t0->k==TY_LIST) gen_borrow(f,a0);
        else { gen_list_of(f,a0,t->elem); hold(f); }
        E(f,"mov edx,%s",kd_of(t->elem)); CALLRT(f,"rt_set_from_list"); return 1;
    }
    if(!strcmp(n,"range_list")){ gen_range_list(f,e); return 1; }  /* range(...) as a value */
    if(!strcmp(n,"dict_copy")){ gen_borrow(f,a0); CALLRT(f,"rt_dict_copy"); return 1; }
    if(!strcmp(n,"dict")){ new_dict(f,t); return 1; }
    cg_fail(e->line,"unsupported builtin");
}

