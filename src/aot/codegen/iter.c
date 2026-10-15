/* ========================= Typed compiler, code generator: iteration =========================
   for loops and comprehensions over ranges, sequences, strings, dicts,
   enumerate / zip and generators. */

#include "gen.h"

/* ---- iteration (for loops and comprehensions) ---- */

static ItKind seq_kind(Ty *t){ t=ty_find(t); return t->k==TY_STR?IT_STR:t->k==TY_BYTES?IT_BYTES:t->k==TY_DICT?IT_KEYS:t->k==TY_TUPLE?IT_TUP:t->k==TY_GEN?IT_GEN:IT_SEQ; }
static Ty *seq_elem(Ty *t){ t=ty_find(t); return t->k==TY_STR?TY_STR_T:t->k==TY_BYTES?TY_INT_T:t->k==TY_DICT?ty_dkey(t):t->k==TY_TUPLE?(t->nelems?t->elems[0]:TY_INT_T):t->elem; }   /* (an empty tuple: no items) */
/* the object a loop goes over, kept in a slot of its own: a sequence, a
   generator, obj.__iter__()'s result, a file's lines -> its kind and items */
static ItKind iter_source(F *f, Expr *x, int *slot, Ty **elem){
    Ty *t=TY(x);
    *slot=ref_slot(f);
    if(is_opt(t)){ int o=gen(f,x); if(o) hold(f); check_none(f,t,"TypeError","'NoneType' object is not iterable"); drop_value(f,t,0); }
    if(t->k==TY_OBJ){
        AFunc *m=aot_find_method(t->cls,"__iter__"); Ty *r=ty_find(m->ret);
        E(f,"sub esp,4"); gen_borrow(f,x); E(f,"mov [esp],eax"); call_method_on_top(f,m); E(f,"add esp,4");
        E(f,"mov [ebp%+d],eax",*slot); *elem=r->elem; return r->k==TY_GEN?IT_GEN:IT_SEQ;
    }
    if(t->k==TY_FILE){ gen_borrow(f,x); CALLRT(f,"rt_file_readlines"); E(f,"mov [ebp%+d],eax",*slot); *elem=t->elem?t->elem:TY_STR_T; return IT_SEQ; }
    if(t->k==TY_SET){ gen_borrow(f,x); E(f,"mov edx,%s",kd_of(t->elem)); CALLRT(f,"rt_set_to_list"); E(f,"mov [ebp%+d],eax",*slot); *elem=t->elem; return IT_SEQ; }   /* in its order */
    gen_owned(f,x); E(f,"mov [ebp%+d],eax",*slot);
    if(t->k==TY_DICT) CALLRT(f,"rt_dict_flat");                  /* its entries in order: no holes left by del */
    *elem=seq_elem(t); return seq_kind(t);
}
void iter_begin(F *f, Expr *it, Iter *I){
    memset(I,0,sizeof *I);
    I->top=new_label(); I->cont=new_label(); I->exit=new_label();
    XInfo *xi=xinfo(it);
    int ranged=xi->kind==X_BUILTIN && (!strcmp(xi->name,"range")||!strcmp(xi->name,"reversed_range"));
    I->idx=frame_slot(f,ranged?8:4);                    /* a range's own int (64 bits); else a position */
    if(ranged){
        Expr *r=it; int rev=xi->name[0]=='r'&&xi->name[1]=='e';
        if(rev) r=it->items[0];
        I->kind=IT_RANGE; I->elem=TY_INT_T; I->end=frame_slot(f,8);
        Expr *lo=r->count>=2?r->items[0]:NULL, *hi=r->count>=2?r->items[1]:r->items[0];
        if(rev){                                        /* stop-1 down to start */
            gen_as(f,hi,TY_INT_T,0); E(f,"sub eax,1"); E(f,"sbb edx,0"); store_new(f,TY_INT_T,"ebp",I->idx);
            if(lo){ gen_as(f,lo,TY_INT_T,0); E(f,"sub eax,1"); E(f,"sbb edx,0"); } else gen_int_const(f,TY_INT_T,-1);
            store_new(f,TY_INT_T,"ebp",I->end);
            I->has_step_const=1; I->step_const=-1;
        } else {
            if(lo) gen_as(f,lo,TY_INT_T,0); else gen_int_const(f,TY_INT_T,0);
            store_new(f,TY_INT_T,"ebp",I->idx);
            gen_as(f,hi,TY_INT_T,0); store_new(f,TY_INT_T,"ebp",I->end);
            I->has_step_const=1; I->step_const=1;
            if(r->count==3){
                Expr *s=r->items[2]; int neg=0;
                if(s->kind==EXPR_UNARY&&s->op==T_MINUS&&s->a->kind==EXPR_LITERAL){ neg=1; s=s->a; }
                if(s->kind==EXPR_LITERAL && s->tok->kind==T_NUMBER && s->tok->i<=0x7FFFFFFF){ I->step_const=(int)(neg?-s->tok->i:s->tok->i); if(!I->step_const) cg_fail(it->line,"range() step must not be zero"); }
                else { I->has_step_const=0; I->step=frame_slot(f,8); gen_as(f,r->items[2],TY_INT_T,0); store_new(f,TY_INT_T,"ebp",I->step);
                    int ok=new_label(); E(f,"or eax,edx"); E(f,"jnz L%d",ok); E(f,"mov esi,Z%d",zlit("range() arg 3 must not be zero")); CALLRT(f,"rt_panic_value"); LBL(f,ok); }
            }
        }
        LBL(f,I->top);
        int up=new_label(), go=new_label();
        if(!I->has_step_const){ E(f,"cmp dword [ebp%+d],0",I->step+4); E(f,"jge L%d",up); }
        if(!I->has_step_const || I->step_const<0){           /* down: stop once idx <= end */
            E(f,"mov eax,[ebp%+d]",I->end); E(f,"cmp eax,[ebp%+d]",I->idx); E(f,"mov eax,[ebp%+d]",I->end+4); E(f,"sbb eax,[ebp%+d]",I->idx+4); E(f,"jge L%d",I->exit);
            if(!I->has_step_const) E(f,"jmp L%d",go); }
        LBL(f,up);
        if(!I->has_step_const || I->step_const>0){           /* up: stop once idx >= end */
            E(f,"mov eax,[ebp%+d]",I->idx); E(f,"cmp eax,[ebp%+d]",I->end); E(f,"mov eax,[ebp%+d]",I->idx+4); E(f,"sbb eax,[ebp%+d]",I->end+4); E(f,"jge L%d",I->exit); }
        LBL(f,go);
        return;
    }
    E(f,"mov dword [ebp%+d],0",I->idx);
    if(xi->kind==X_BUILTIN && (!strcmp(xi->name,"enumerate")||!strcmp(xi->name,"zip"))){
        int isenum=xi->name[0]=='e'; Ty *e0;
        I->kind=isenum?IT_ENUM:IT_ZIP;
        if(isenum){ I->sub=iter_source(f,it->items[0],&I->src,&e0);
            I->elem=TY_INT_T; I->elem2=e0; I->start=frame_slot(f,8);
            if(it->count==2) gen_as(f,it->items[1],TY_INT_T,0); else gen_int_const(f,TY_INT_T,0);
            store_new(f,TY_INT_T,"ebp",I->start); }
        else { if(it->count>8) cg_fail(it->line,"zip() of more than 8 iterables");
            I->nsrc=it->count;
            for(int k=0;k<it->count;k++) I->subs[k]=iter_source(f,it->items[k],&I->srcs[k],&I->elems[k]);
            I->src=I->srcs[0]; I->elem=I->elems[0]; I->elem2=it->count>1?I->elems[1]:NULL; }
    } else if(xi->kind==X_TMETHOD && !strcmp(xi->name,"items")){
        Ty *e0; iter_source(f,it->a->a,&I->src,&e0); I->kind=IT_ITEMS; I->elem=ty_dkey(TY(it->a->a)); I->elem2=TY(it->a->a)->elem;
    } else {
        Ty *el; I->kind=iter_source(f,it,&I->src,&el); I->elem=el;
    }
    LBL(f,I->top);
    for(int k=0;k<(I->kind==IT_ZIP?I->nsrc:1);k++){
        ItKind sk= I->kind==IT_ZIP ? I->subs[k] : I->kind==IT_ENUM ? I->sub : I->kind;
        E(f,"mov eax,[ebp%+d]",I->kind==IT_ZIP?I->srcs[k]:I->src); E(f,"test eax,eax"); E(f,"jz L%d",I->exit);
        if(sk==IT_GEN){ CALLRT(f,"rt_gen_next"); E(f,"test eax,eax"); E(f,"jz L%d",I->exit); continue; }
        E(f,"mov ecx,[ebp%+d]",I->idx);
        if(sk==IT_TUP){ E(f,"mov edx,[eax+8]"); E(f,"cmp ecx,[edx]"); }
        else if(sk==IT_STR){ E(f,"push ecx"); CALLRT(f,"rt_str_cplen"); E(f,"pop ecx"); E(f,"cmp ecx,eax"); }
        else E(f,"cmp ecx,[eax+8]");
        E(f,"jae L%d",I->exit);
    }
}
/* value number k of the current iteration (enumerate, items: 0 or 1; zip: which iterable) -> eax/st0 (borrowed) */
static Ty *iter_elem(Iter *I, int k){ return I->kind==IT_ZIP ? I->elems[k] : k ? I->elem2 : I->elem; }
void iter_value(F *f, Iter *I, int k){
    ItKind kind=I->kind; int src=I->src; Ty *el=iter_elem(I,k);
    if(kind==IT_RANGE){ load_mem(f,TY_INT_T,"ebp",I->idx); return; }
    if(kind==IT_ENUM){ if(k==0){ E(f,"mov eax,[ebp%+d]",I->idx); E(f,"xor edx,edx"); E(f,"add eax,[ebp%+d]",I->start); E(f,"adc edx,[ebp%+d]",I->start+4); return; } kind=I->sub; }
    else if(kind==IT_ZIP){ kind=I->subs[k]; src=I->srcs[k]; }
    E(f,"mov eax,[ebp%+d]",src);
    if(kind==IT_GEN){ load_mem(f,el,"eax",24); return; }
    E(f,"mov ecx,[ebp%+d]",I->idx);
    if(kind==IT_STR){ E(f,"mov edx,ecx"); CALLRT(f,"rt_str_char"); return; }
    if(kind==IT_BYTES){ E(f,"movzx eax,byte [eax+12+ecx]"); E(f,"xor edx,edx"); return; }
    if(kind==IT_TUP){ E(f,"add eax,12"); load_at(f,el,"eax","ecx",esize(el)); return; }
    if(kind==IT_KEYS||(kind==IT_ITEMS&&k==0)){ E(f,"mov eax,[eax+16]"); E(f,"lea eax,[eax+ecx*8]"); load_mem(f,el,"eax",0); return; }   /* keys: 8 bytes each */
    if(kind==IT_ITEMS){ E(f,"mov eax,[eax+20]"); load_at(f,el,"eax","ecx",esize(el)); return; }
    E(f,"mov eax,[eax+16]");
    load_at(f,el,"eax","ecx",esize(el));
}
void iter_end(F *f, Iter *I){
    LBL(f,I->cont);
    if(I->kind==IT_RANGE){
        if(I->has_step_const){ E(f,"add dword [ebp%+d],%d",I->idx,I->step_const); E(f,"adc dword [ebp%+d],%d",I->idx+4,I->step_const<0?-1:0); }
        else { E(f,"mov eax,[ebp%+d]",I->step); E(f,"add [ebp%+d],eax",I->idx); E(f,"mov eax,[ebp%+d]",I->step+4); E(f,"adc [ebp%+d],eax",I->idx+4); } }
    else E(f,"inc dword [ebp%+d]",I->idx);
    E(f,"jmp L%d",I->top);
}
void iter_release(F *f, Iter *I){
    int n= I->kind==IT_ZIP ? I->nsrc : 1;
    for(int k=0;k<n;k++){ int sl= I->kind==IT_ZIP ? I->srcs[k] : I->src;
        if(sl){ rt("rt_decref"); E(f,"mov eax,[ebp%+d]",sl); E(f,"mov dword [ebp%+d],0",sl); E(f,"call rt_decref"); } }
}
/* store the iteration value (borrowed in eax/st0) into a variable */
void store_borrowed(F *f, AVar *v, Ty *from){
    if(v->borrowed){ conv_num(f,from,v->ty); E(f,"mov [ebp%+d],eax",v->offset); return; }   /* the item stays the container's */
    if(is_ptr(v->ty)) incref(f);
    conv(f,from,v->ty);
    store_var(f,v);
}
/* The loop variables of one iteration: `x`, `i, x` (enumerate/zip/items), or
   `a, b` unpacking each element (a list or a tuple). */
void iter_store_vars(F *f, Iter *I, AVar **vars, int n){
    if(n==1 && (I->kind==IT_ENUM || I->kind==IT_ZIP || I->kind==IT_ITEMS)){     /* for t in zip(a, b): a new tuple each time */
        Ty *tt=ty_find(vars[0]->ty); int parts= I->kind==IT_ZIP ? I->nsrc : 2;
        E(f,"mov eax,TD%d",tdesc(tt)); E(f,"mov edx,%d",tuple_size(tt)); CALLRT(f,"rt_tuple_new"); E(f,"push eax");
        for(int k=0;k<parts;k++){ Ty *et=tt->elems[k];
            iter_value(f,I,k); conv_num(f,iter_elem(I,k),et); if(is_ptr(et)) incref(f);
            E(f,"mov ecx,[esp]"); store_mem(f,et,"ecx",tuple_off(tt,k)); }
        E(f,"pop eax"); conv(f,tt,vars[0]->ty); store_var(f,vars[0]);
        return;
    }
    if(n==1 || I->kind==IT_ENUM || I->kind==IT_ZIP || I->kind==IT_ITEMS){
        for(int k=0;k<n;k++){ iter_value(f,I,k); store_borrowed(f,vars[k],iter_elem(I,k)); }
        return;
    }
    Ty *et=ty_find(I->elem);
    iter_value(f,I,0); E(f,"push eax");
    if(et->k==TY_TUPLE){
        for(int k=0;k<n;k++){ E(f,"mov eax,[esp]"); panic_if_null(f); load_mem(f,et->elems[k],"eax",tuple_off(et,k)); store_borrowed(f,vars[k],et->elems[k]); }
    } else {
        Ty *el=et->elem;
        E(f,"mov edx,%d",n); CALLRT(f,"rt_unpack_check");
        for(int k=0;k<n;k++){
            E(f,"mov eax,[esp]"); E(f,"mov eax,[eax+16]");
            load_mem(f,el,"eax",esize(el)*k);
            store_borrowed(f,vars[k],el);
        }
    }
    E(f,"add esp,4");
}

/* a comprehension clause with a target: its loop variable unpacked into it */
void comp_unpack(F *f, Expr *e, int i){
    CompClause *cl=&e->clauses[i];
    for(int k=0;k<2;k++){ Expr *t=k?cl->target2:cl->target; if(!t) continue;
        AVar *v=xinfo(e)->cvars[2*i+k];
        load_var(f,v); if(is_ptr(v->ty)) incref(f);
        store_target(f,t,v->ty,e->line); }
}
int gen_comprehension(F *f, Expr *e){
    Ty *t=TY(e);
    int res=ref_slot(f);
    if(e->comp_kind=='D') new_dict(f,t);
    else if(e->comp_kind=='S') new_set(f,t->elem);
    else new_list(f,t->elem);
    E(f,"mov [ebp%+d],eax",res);
    Iter its[8]; int n=e->nclause;
    if(n>8) cg_fail(e->line,"too many for clauses");
    for(int i=0;i<n;i++){
        CompClause *cl=&e->clauses[i];
        iter_begin(f,cl->iter,&its[i]);
        iter_store_vars(f,&its[i],&xinfo(e)->cvars[2*i],cl->nvars); comp_unpack(f,e,i);
        for(int k=0;k<cl->ncond;k++) gen_jump(f,cl->conds[k],its[i].cont,0);
    }
    int mark=scope_open(f);
    Ty *el=t->elem;
    if(e->comp_kind=='L'||e->comp_kind=='g'){
        gen_as(f,e->a,el,1);
        E(f,"push dword [ebp%+d]",res); list_append_top(f,el); E(f,"add esp,4");
    } else if(e->comp_kind=='S'){
        gen_as(f,e->a,el,0);
        E(f,"push dword [ebp%+d]",res); set_add_top(f,el); E(f,"add esp,4");
    } else {
        Ty *kt=ty_dkey(t);
        gen_as(f,e->b,el,1); push_value(f,el);
        gen_as(f,e->a,kt,0); push_value(f,kt);
        E(f,"mov edx,esp"); E(f,"mov eax,[ebp%+d]",res); CALLRT(f,"rt_dict_slot"); E(f,"add esp,%d",esize(kt));
        E(f,"mov ecx,eax"); pop_value(f,el); store_mem(f,el,"ecx",0);
    }
    scope_close(f,mark);
    for(int i=n-1;i>=0;i--){ iter_end(f,&its[i]); LBL(f,its[i].exit); iter_release(f,&its[i]); }
    E(f,"mov eax,[ebp%+d]",res); E(f,"mov dword [ebp%+d],0",res);
    return 1;
}

