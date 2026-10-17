/* ========================= Typed compiler, code generator: tuples, indexing, literals =========================
   Tuples (static constant ones too), indexing and slicing, container
   literals. */

#include "gen.h"
#include "targets/targets.h"

/* ---- tuples: +8 descriptor TD<i> (count, element kinds), items from +12 ---- */
int tuple_off(Ty *t, int i){ t=ty_find(t); int off=12; for(int k=0;k<i;k++) off+=esize(t->elems[k]); return off; }
int tuple_size(Ty *t){ t=ty_find(t); return tuple_off(t,t->nelems)-12; }
int tdesc(Ty *t){
    t=ty_find(t);
    for(int i=0;i<gg->ntdescs;i++) if(ty_same_exact(gg->tdescs[i],t)) return i;
    for(int k=0;k<t->nelems;k++) kd_of(t->elems[k]);        /* now, while functions are still being emitted */
    if(gg->ntdescs==gg->ctdescs){ gg->ctdescs=gg->ctdescs?gg->ctdescs*2:8; gg->tdescs=(Ty**)xrealloc(gg->tdescs,sizeof(Ty*)*(size_t)gg->ctdescs); }
    gg->tdescs[gg->ntdescs]=t;
    return gg->ntdescs++;
}
int gen_tuple(F *f, Expr *e){
    Ty *t=TY(e);
    Expr **items=e->kind==EXPR_DICT ? e->vals : e->items;          /* a record: {"key": value, ...} */
    E(f,"mov eax,TD%d",tdesc(t)); E(f,"mov edx,%d",tuple_size(t)); CALLRT(f,"rt_tuple_new");
    E(f,"push eax");
    for(int i=0;i<e->count;i++){ Ty *et=t->elems[i]; gen_as(f,items[i],et,1); E(f,"mov ecx,[esp]"); store_new(f,et,"ecx",tuple_off(t,i)); }
    E(f,"pop eax");
    return 1;
}
/* A tuple of constants - numbers, str and bytes literals, True, False, None
   and such tuples - is one static object, as CPython keeps it among a code
   object's constants: made with the program, never freed, the same object
   each time the expression runs instead of a new one built every time; equal
   ones are one object. A fixed tuple: TD<d> and its items; tuple[T, ...]:
   a list's fields, its items in CTI<i>. */
static int const_sign(Expr **e){                    /* -1 for -x, --x ... (+x: 1) */
    int neg=0;
    while((*e)->kind==EXPR_UNARY && ((*e)->op==T_MINUS||(*e)->op==T_PLUS)){ if((*e)->op==T_MINUS) neg=!neg; *e=(*e)->a; }
    return neg?-1:1;
}
static int const_item(Expr *e, Ty *t, Buf *b){      /* ",<dwords>" of the constant e as a t; 0 if it is none */
    t=ty_find(t);
    if(e->kind==EXPR_NONE){
        if(!ty_opt(t)) return 0;
        if(is_flt(t)) buf_printf(b,",0x6E650001,0x7FF86E6F");   /* (rt_fnone) */
        else if(is_lng(t)) buf_printf(b,",0," NONE_HI_INT);
        else if(t->k==TY_BOOL) buf_printf(b,",2");
        else if(is_ptr(t)) buf_printf(b,",0");
        else return 0;
        return 1;
    }
    Expr *x=e; int sign=const_sign(&x);
    Tok *tk= x->kind==EXPR_LITERAL ? x->tok : NULL;
    switch(t->k){
        case TY_INT:{
            if(!tk || tk->kind!=T_NUMBER || tk->is_float) return 0;
            uint64_t v=(uint64_t)tk->i; if(sign<0) v=0-v;
            buf_printf(b,",0x%08X,0x%08X",(unsigned)(v&0xFFFFFFFFu),(unsigned)(v>>32)); return 1; }
        case TY_FLOAT:{
            if(!tk || tk->kind!=T_NUMBER) return 0;
            char num[128]; int n=0; for(const char *q=tk->text;*q && n<127;q++) if(*q!='_') num[n++]=*q;
            num[n]=0;
            double d= tk->is_float ? strtod(num,NULL) : (double)tk->i;
            if(sign<0) d=-d;
            uint64_t v; memcpy(&v,&d,8);
            buf_printf(b,",0x%08X,0x%08X",(unsigned)(v&0xFFFFFFFFu),(unsigned)(v>>32)); return 1; }
        case TY_BOOL: if(x!=e || (e->kind!=EXPR_TRUE && e->kind!=EXPR_FALSE)) return 0; buf_printf(b,",%d",e->kind==EXPR_TRUE); return 1;
        case TY_STR: if(x!=e || !tk || tk->kind!=T_STRING || tk->i!=0) return 0; buf_printf(b,",S%d",str_litn(tk->text,(int)tk->len)); return 1;
        case TY_BYTES: if(x!=e || !tk || tk->kind!=T_STRING || tk->i!=2) return 0; buf_printf(b,",B%d",bytes_lit(tk->text,(int)tk->len)); return 1;
        case TY_TUPLE: case TY_LIST:{ int k= x==e ? const_tuple(e,t) : -1; if(k<0) return 0; buf_printf(b,",CT%d",k); return 1; }
        default: return 0;
    }
}
int const_tuple(Expr *e, Ty *t){             /* -> i of its CT<i>, or -1 if e is not such a tuple */
    t=ty_find(t);
    if(e->kind!=EXPR_TUPLE || !e->count || t->opt) return -1;
    if(t->k==TY_TUPLE ? t->names || t->nelems!=e->count : t->k!=TY_LIST || !t->tup) return -1;
    for(int i=0;i<e->count;i++) if(e->items[i]->akind) return -1;     /* (*xs) */
    Buf b={0};
    if(t->k==TY_TUPLE) buf_printf(&b,"TD%d",tdesc(t));
    else buf_printf(&b,"%s",kd_of(t->elem));
    for(int i=0;i<e->count;i++)
        if(!const_item(e->items[i],t->k==TY_TUPLE?t->elems[i]:t->elem,&b)){ free(b.s); return -1; }
    for(int i=0;i<gg->nctups;i++) if(!strcmp(gg->ctups[i],b.s)){ free(b.s); return i; }
    if(gg->nctups==gg->cctups){ gg->cctups=gg->cctups?gg->cctups*2:16; gg->ctups=(char**)xrealloc(gg->ctups,sizeof(char*)*(size_t)gg->cctups); }
    int k=gg->nctups++; gg->ctups[k]=b.s;
    rt("rt_static");
    char *items=strchr(b.s,',');                    /* (after the descriptor) */
    if(t->k==TY_TUPLE) buf_printf(&gg->data,"align 4\nCT%d dd 0x40000000,rt_static,%s   ; %s\n",k,b.s,ty_name(t));
    else buf_printf(&gg->data,"align 4\nCT%d dd 0x40000000,rt_static,%d,%d,CTI%d,%.*s   ; %s\nCTI%d dd %s\n",
                    k,e->count,e->count,k,(int)(items-b.s),b.s,ty_name(t),k,items+1);
    return k;
}
/* The fields of the tuple at [ebp+slot] (borrowed) into the targets. */
void unpack_tuple_slot(F *f, int slot, Ty *tt, Expr **targets, int n, int line){
    tt=ty_find(tt);
    for(int i=0;i<n;i++){
        Ty *et=tt->elems[i];
        E(f,"mov eax,[ebp%+d]",slot); panic_if_null(f);
        load_mem(f,et,"eax",tuple_off(tt,i));
        if(is_ptr(et)) incref(f);
        store_target(f,targets[i],et,line);
    }
}

/* ---- indexing ---- */
/* the registers of sys.syscall(...): eax..edi, ebp (0 unless given; ebp itself
   is kept around the call) -> after the call [esp] eax .. [esp+20] edi; 28 bytes
   (only those of `keep` stored: a mask, 1 eax, 2 ebx ... 32 edi) */
void gen_syscall_regs(F *f, Expr *e, int keep){
    E(f,"sub esp,28");
    for(int i=e->count;i<7;i++) E(f,"mov dword [esp+%d],0",4*i);      /* registers no argument sets */
    for(int i=0;i<e->count;i++){
        Ty *t=TY(e->items[i]);
        gen_borrow(f,e->items[i]);
        if(t->k==TY_STR||t->k==TY_BYTES){ int l=new_label(), l2=new_label();            /* its bytes, NUL-terminated ("" too) */
            E(f,"test eax,eax"); E(f,"jnz L%d",l); E(f,"mov eax,Z%d",zlit("")); E(f,"jmp L%d",l2); LBL(f,l); E(f,"add eax,12"); LBL(f,l2); }
        else if(t->k==TY_BUF){ int l=new_label(); E(f,"test eax,eax"); E(f,"jz L%d",l); E(f,"add eax,12"); LBL(f,l); }
        E(f,"mov [esp+%d],eax",4*i);
    }
    static const char *regs[6]={"eax","ebx","ecx","edx","esi","edi"};
    E(f,"push ebp"); E(f,"mov ebp,[esp+28]");                         /* (on Linux: the 6th argument) */
    for(int i=5;i>=0;i--) E(f,"mov %s,[esp+%d]",regs[i],4+4*i);
    Tok *n0= e->count && e->items[0]->kind==EXPR_LITERAL ? e->items[0]->tok : NULL;
    if(aot_target(gg->target)->abi==AOT_ABI_KOLIBRI) E(f,"int 0x40");
    else if(n0 && n0->kind==T_NUMBER && !n0->is_float && n0->i==265){ rt("rt_sys_clock_gettime"); E(f,"call rt_sys_clock_gettime"); }   /* (clock_gettime: the vDSO's) */
    else E(f,"int 0x80");
    for(int i=0;i<6;i++) if(keep&(1<<i)) E(f,"mov [esp+%d],%s",4+4*i,regs[i]);
    E(f,"pop ebp");
}
int gen_index(F *f, Expr *e){
    if(e->a->kind==EXPR_CALL && xinfo(e->a)->kind==X_SYSCALL && e->b->kind==EXPR_LITERAL && e->b->tok->kind==T_NUMBER
       && !e->b->tok->is_float && e->b->tok->i>=0 && e->b->tok->i<6){        /* sys.syscall(...)[k]: just register k */
        gen_syscall_regs(f,e->a,1<<(int)e->b->tok->i); E(f,"mov eax,[esp+%d]",4*(int)e->b->tok->i); E(f,"add esp,28"); widen(f,1); return 0;
    }
    Ty *ct=TY(e->a);
    if(xinfo(e)->kind==X_TYPEVAL){ E(f,"mov eax,%s",typeval_label(xinfo(e))); return 0; }      /* Box[int]: the class */
    if(ct->k==TY_OBJ) return gen_op_call(f,xinfo(e)->kind==X_OPMETHOD && xinfo(e)->fn ? xinfo(e)->fn : aot_find_method(ct->cls,"__getitem__"),e->a,e->b);   /* obj[key] (a generic one: its copy) */
    if(is_opt(ct)){ int o=gen(f,e->a); if(o) hold(f); check_none(f,ct,"TypeError","'NoneType' object is not subscriptable"); drop_value(f,ct,0); }
    if(ct->k==TY_TUPLE){
        XInfo *xi=xinfo(e);
        if(xi->argmap[1]){ gen_borrow(f,e->a); panic_if_null(f); load_mem(f,ct->elems[xi->argmap[0]],"eax",tuple_off(ct,xi->argmap[0])); return 0; }
        Ty *et=ct->elems[0];                                     /* all items of one type */
        gen_borrow(f,e->a); E(f,"push eax"); gen(f,e->b); to_i32(f,TY(e->b)); E(f,"mov edx,eax"); E(f,"pop eax");
        CALLRT(f,"rt_tuple_at"); load_mem(f,et,"eax",0); return 0;
    }
    gen_borrow(f,e->a); E(f,"push eax");
    if(ct->k==TY_DICT){                                          /* the key goes by address */
        Ty *kt=ty_dkey(ct); gen_as(f,e->b,kt,0); push_value(f,kt);
        E(f,"mov edx,esp"); E(f,"mov eax,[esp+%d]",esize(kt)); CALLRT(f,"rt_dict_get"); E(f,"add esp,%d",esize(kt)+4);
        load_mem(f,ct->elem,"eax",0); return 0;
    }
    gen(f,e->b); to_i32(f,TY(e->b));
    E(f,"mov edx,eax"); E(f,"pop eax");
    if(ct->k==TY_STR){ CALLRT(f,"rt_str_char"); return 0; }
    if(ct->k==TY_BYTES){ CALLRT(f,"rt_bytes_at"); widen(f,0); return 0; }
    CALLRT(f,"rt_list_at"); load_mem(f,ct->elem,"eax",0); return 0;
}
int gen_slice(F *f, Expr *e){
    Ty *t=TY(e->a); int list=t->k==TY_LIST, n=list?24:20, flags=0;
    E(f,"sub esp,%d",n);
    gen_borrow(f,e->a); check_none(f,t,"TypeError","'NoneType' object is not subscriptable"); E(f,"mov [esp],eax");
    if(t->k==TY_BYTES) CALLRT(f,"rt_bytes_cp");                 /* (a byte a code point) */
    Expr *parts[3]={e->b,e->c,e->d};
    for(int k=0;k<3;k++){
        if(parts[k] && parts[k]->kind!=EXPR_NONE){ gen(f,parts[k]); to_i32(f,TY(parts[k])); E(f,"mov [esp+%d],eax",4+4*k); }
        else { flags|=1<<k; E(f,"mov dword [esp+%d],0",4+4*k); }
    }
    E(f,"mov dword [esp+16],%d",flags);
    if(list) E(f,"mov dword [esp+20],%s",kd_of(t->elem));
    CALLRT(f,list?"rt_list_slice":"rt_str_slice");
    E(f,"add esp,%d",n);
    return 1;
}

/* ---- container literals ---- */
void list_append_top(F *f, Ty *el){         /* list at [esp], value in eax/st0 (owned) */
    CALLRT(f,is_flt(el)?"rt_list_addf":is_lng(el)?"rt_list_add64":"rt_list_add32");
}
void set_add_top(F *f, Ty *el){             /* set at [esp], value in eax/st0 (borrowed) */
    push_value(f,el);
    E(f,"mov edx,esp"); E(f,"mov eax,[esp+%d]",esize(el)); CALLRT(f,"rt_set_add"); E(f,"add esp,%d",esize(el));
}
/* an expression CPython's optimizer folds into a constant (see set displays) */
static int folds_const(Expr *e){
    switch(e->kind){
        case EXPR_LITERAL: case EXPR_TRUE: case EXPR_FALSE: case EXPR_NONE: return 1;
        case EXPR_UNARY: return e->op!=T_NOT && folds_const(e->a);
        case EXPR_BINARY: return folds_const(e->a) && folds_const(e->b);
        case EXPR_TUPLE: for(int i=0;i<e->count;i++) if(!folds_const(e->items[i])) return 0; return 1;
        default: return 0;
    }
}
int gen_literal_container(F *f, Expr *e){
    Ty *t=TY(e), *el=t->elem;
    if(e->kind==EXPR_DICT){
        Ty *kt=ty_dkey(t);
        new_dict(f,t); E(f,"push eax");
        for(int i=0;i<e->count;i++){
            if(e->items[i]->akind==2){ gen_borrow(f,e->items[i]); E(f,"mov edx,eax"); E(f,"mov eax,[esp]"); CALLRT(f,"rt_dict_update"); continue; }   /* **d */
            gen_as(f,e->vals[i],el,1); push_value(f,el);
            gen_as(f,e->items[i],kt,0); push_value(f,kt);
            E(f,"mov edx,esp"); E(f,"mov eax,[esp+%d]",esize(kt)+esize(el)); CALLRT(f,"rt_dict_slot"); E(f,"add esp,%d",esize(kt));
            E(f,"mov ecx,eax"); pop_value(f,el); store_mem(f,el,"ecx",0);
        }
        E(f,"pop eax"); return 1;
    }
    if(e->kind==EXPR_SET){
        int consts=e->count>=3;
        new_set(f,el); E(f,"push eax");
        for(int i=0;i<e->count;i++){
            if(e->items[i]->akind==1){ consts=0; gen_list_of(f,e->items[i],el); hold(f); E(f,"mov edx,eax"); E(f,"mov eax,[esp]"); CALLRT(f,"rt_set_add_list"); continue; }   /* *xs */
            if(!folds_const(e->items[i])) consts=0; gen_as(f,e->items[i],el,0); set_add_top(f,el); }
        if(consts){                                 /* CPython: a frozenset constant merged into a new set (its order) */
            new_set(f,el); E(f,"push eax"); E(f,"mov edx,[esp+4]"); CALLRT(f,"rt_set_merge");
            E(f,"pop eax"); E(f,"xchg eax,[esp]"); CALLRT(f,"rt_decref"); }
        E(f,"pop eax"); return 1;
    }
    new_list(f,el); E(f,"push eax");
    for(int i=0;i<e->count;i++){
        if(e->items[i]->akind==1){ gen_list_of(f,e->items[i],el); hold(f); E(f,"mov edx,eax"); E(f,"mov eax,[esp]"); CALLRT(f,"rt_list_extend"); continue; }   /* *xs */
        gen_as(f,e->items[i],el,1); list_append_top(f,el); }
    E(f,"pop eax"); return 1;
}

