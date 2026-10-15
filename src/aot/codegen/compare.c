/* ========================= Typed compiler, code generator: truth values and comparisons =========================
   Truth values, comparisons and comparison chains, conditional jumps. */

#include "gen.h"

/* ---- truth values ---- */
static int has_length(Ty *t){ t=ty_find(t); return t->k==TY_STR||t->k==TY_BYTES||t->k==TY_LIST||t->k==TY_DICT||t->k==TY_SET||t->k==TY_BUF; }
/* eax = truth of the value in eax/st0 (references stay put) */
static void truth_of(F *f, Ty *t);
void truth(F *f, Ty *t){
    t=ty_find(t);
    if(t->opt && !is_ptr(t)){                              /* None is false */
        int lnone=new_label(), lend=new_label();
        jump_if_none(f,t,lnone); truth_of(f,t); E(f,"jmp L%d",lend);
        LBL(f,lnone); drop_value(f,t,0); E(f,"xor eax,eax"); LBL(f,lend); return; }
    truth_of(f,t);
}
/* the method giving an object's truth: __bool__, else __len__ (NULL: always true) */
static AFunc *truth_method(Ty *t){
    t=ty_find(t); if(t->k!=TY_OBJ || !t->cls) return NULL;
    AFunc *m=aot_find_method(t->cls,"__bool__"); if(!m) m=aot_find_method(t->cls,"__len__");
    return m && m->nparams==1 ? m : NULL;
}
/* eax = the truth of the object in eax (kept in place) by its method; a null reference is false */
static void truth_call(F *f, AFunc *m){
    int l=new_label();
    E(f,"test eax,eax"); E(f,"jz L%d",l);
    E(f,"push eax"); call_method_on_top(f,m); E(f,"add esp,4");
    if(ty_find(m->ret)->k!=TY_BOOL){ E(f,"or eax,edx"); E(f,"setne al"); E(f,"movzx eax,al"); }
    LBL(f,l);
}
static void truth_of(F *f, Ty *t){
    t=ty_find(t);
    if(t->k==TY_BOOL) return;
    { AFunc *m=truth_method(t); if(m){ truth_call(f,m); return; } }
    if(t->k==TY_FLOAT){ E(f,"ftst"); E(f,"fnstsw ax"); E(f,"fstp st0"); E(f,"sahf"); E(f,"setne al"); E(f,"movzx eax,al"); return; }
    if(t->k==TY_INT){ E(f,"or eax,edx"); E(f,"setne al"); E(f,"movzx eax,al"); return; }
    if(has_length(t)){ int l=new_label(); E(f,"xor ecx,ecx"); E(f,"test eax,eax"); E(f,"jz L%d",l); E(f,"cmp dword [eax+8],0"); E(f,"setne cl"); LBL(f,l); E(f,"mov eax,ecx"); return; }
    E(f,"test eax,eax"); E(f,"setne al"); E(f,"movzx eax,al");
}
/* jump to `label` when the truth of eax/st0 equals want, keeping the value */
static void truth_jump(F *f, Ty *t, int label, int want){
    t=ty_find(t);
    if(t->opt && !is_ptr(t)){                              /* None is false */
        int lnone=new_label(), lend=new_label();
        jump_if_none(f,t,lnone);
        Ty plain=*t; plain.opt=0; truth_jump(f,&plain,label,want); E(f,"jmp L%d",lend);
        LBL(f,lnone); if(!want) E(f,"jmp L%d",label);
        LBL(f,lend); return; }
    if(t->k==TY_FLOAT){ E(f,"ftst"); E(f,"fnstsw ax"); E(f,"sahf"); E(f,"j%s L%d",want?"ne":"e",label); return; }
    if(t->k==TY_INT){ E(f,"mov ecx,eax"); E(f,"or ecx,edx"); E(f,"j%s L%d",want?"nz":"z",label); return; }
    { AFunc *m=truth_method(t);
      if(m){ E(f,"push eax"); truth_call(f,m); E(f,"test eax,eax"); E(f,"pop eax"); E(f,"j%s L%d",want?"nz":"z",label); return; } }
    if(has_length(t)){
        if(want){ int skip=new_label(); E(f,"test eax,eax"); E(f,"jz L%d",skip); E(f,"cmp dword [eax+8],0"); E(f,"jne L%d",label); LBL(f,skip); }
        else { E(f,"test eax,eax"); E(f,"jz L%d",label); E(f,"cmp dword [eax+8],0"); E(f,"je L%d",label); }
        return;
    }
    E(f,"test eax,eax"); E(f,"j%s L%d",want?"nz":"z",label);
}

/* and/or: the deciding operand's value */
int gen_boolop(F *f, Expr *e){
    Ty *t=TY(e); int lend=new_label();
    gen_as(f,e->a,t,1);
    truth_jump(f,t,lend,e->op==T_OR);
    drop_value(f,t,1);
    gen_as(f,e->b,t,1);
    LBL(f,lend);
    return 1;
}

/* ---- comparisons ---- */
int is_none_lit(Expr *e){ return e->kind==EXPR_NONE; }
static const char *cc_of(int code, int is_float){
    switch(code){
        case CMP_LT: return is_float?"b":"l";
        case CMP_LE: return is_float?"be":"le";
        case CMP_GT: return is_float?"a":"g";
        case CMP_GE: return is_float?"ae":"ge";
        case CMP_EQ: case CMP_IS: case CMP_IN: return "e";
        default: return "ne";
    }
}
/* the type two numbers are compared as: float if either is, int if either is, else bool */
static Ty *num_common(Ty *a, Ty *b){
    a=ty_find(a); b=ty_find(b);
    if(a->k==TY_FLOAT||b->k==TY_FLOAT) return TY_FLOAT_T;
    if(a->k==TY_INT||b->k==TY_INT) return TY_INT_T;
    return TY_BOOL_T;
}
/* the int on the stack (dropped) <code> the one in edx:eax -> eax = bool */
static void cmp64(F *f, int code){
    switch(code){
        case CMP_EQ: case CMP_NE: case CMP_IS: case CMP_ISNOT:
            E(f,"xor eax,[esp]"); E(f,"xor edx,[esp+4]"); E(f,"or eax,edx"); E(f,"set%s al",(code==CMP_EQ||code==CMP_IS)?"e":"ne"); break;
        case CMP_LT: case CMP_GE:                       /* left - right */
            E(f,"mov ecx,[esp]"); E(f,"sub ecx,eax"); E(f,"mov ecx,[esp+4]"); E(f,"sbb ecx,edx"); E(f,"set%s al",code==CMP_LT?"l":"ge"); break;
        default:                                        /* >, <=: right - left */
            E(f,"sub eax,[esp]"); E(f,"sbb edx,[esp+4]"); E(f,"set%s al",code==CMP_GT?"l":"ge"); break;
    }
    E(f,"movzx eax,al"); E(f,"add esp,8");
}
static const char *neg_cc(const char *cc);
/* jump to `label` when (the int on the stack <code> the one in edx:eax) == want; the stack int is dropped */
static void jcc64(F *f, int code, int want, int label){
    const char *cc;
    switch(code){
        case CMP_EQ: case CMP_NE: E(f,"xor eax,[esp]"); E(f,"xor edx,[esp+4]"); E(f,"or eax,edx"); cc=code==CMP_EQ?"e":"ne"; break;
        case CMP_LT: case CMP_GE: E(f,"mov ecx,[esp]"); E(f,"sub ecx,eax"); E(f,"mov ecx,[esp+4]"); E(f,"sbb ecx,edx"); cc=code==CMP_LT?"l":"ge"; break;
        default: E(f,"sub eax,[esp]"); E(f,"sbb edx,[esp+4]"); cc=code==CMP_GT?"l":"ge"; break;
    }
    E(f,"lea esp,[esp+8]");                             /* keeps the flags */
    E(f,"j%s L%d",want?cc:neg_cc(cc),label);
}
/* Left value pushed on the machine stack, right in eax/st0 -> eax = bool. */
static void cmp_finish(F *f, int code, Ty *ta, Ty *tb, int line){
    ta=ty_find(ta); tb=ty_find(tb);
    if(code==CMP_IN||code==CMP_NOTIN){
        if(tb->k==TY_STR||(tb->k==TY_BYTES&&ta->k==TY_BYTES)){ E(f,"pop edx"); CALLRT(f,"rt_str_contains"); if(code==CMP_NOTIN) E(f,"xor eax,1"); return; }
        if(tb->k==TY_BYTES){                                    /* a byte value in bytes */
            if(is_lng(ta)){ E(f,"pop ecx"); E(f,"pop edx"); } else { E(f,"pop ecx"); E(f,"xor edx,edx"); }
            CALLRT(f,"rt_bytes_has"); if(code==CMP_NOTIN) E(f,"xor eax,1"); return; }
        /* the value is on the machine stack: its address goes to the runtime */
        int sz=esize(ta);
        E(f,"mov edx,esp");
        if(tb->k==TY_SET){ CALLRT(f,"rt_set_contains"); E(f,"add esp,%d",sz); if(code==CMP_NOTIN) E(f,"xor eax,1"); return; }
        CALLRT(f,tb->k==TY_DICT?"rt_dict_find":"rt_list_find"); E(f,"add esp,%d",sz);
        E(f,"cmp eax,-1"); E(f,"set%s al",code==CMP_IN?"ne":"e"); E(f,"movzx eax,al"); return;
    }
    if(numeric_ty(ta)&&numeric_ty(tb)){
        if(ta->k==TY_FLOAT||tb->k==TY_FLOAT){
            conv_num(f,tb,TY_FLOAT_T);
            if(ta->k==TY_FLOAT){ E(f,"fld qword [esp]"); E(f,"add esp,8"); }
            else if(ta->k==TY_INT){ E(f,"fild qword [esp]"); E(f,"add esp,8"); }
            else { E(f,"fild dword [esp]"); E(f,"add esp,4"); }
            E(f,"fcompp"); E(f,"fnstsw ax"); E(f,"sahf");
            E(f,"set%s al",cc_of(code,1));                     /* a NaN (unordered: PF) is unequal to everything */
            if(code==CMP_NE){ E(f,"setp cl"); E(f,"or al,cl"); } else { E(f,"setnp cl"); E(f,"and al,cl"); }
            E(f,"movzx eax,al"); return;
        }
        if(ta->k==TY_INT||tb->k==TY_INT){ conv_num(f,tb,TY_INT_T); cmp64(f,code); return; }     /* (both int: see num_common) */
        E(f,"mov ecx,eax"); E(f,"pop eax"); E(f,"cmp eax,ecx"); E(f,"set%s al",cc_of(code,0)); E(f,"movzx eax,al"); return;
    }
    if((ta->k==TY_STR&&tb->k==TY_STR)||(ta->k==TY_BYTES&&tb->k==TY_BYTES)){
        E(f,"mov edx,eax"); E(f,"pop eax");
        if(code==CMP_EQ||code==CMP_NE){ CALLRT(f,"rt_str_eq"); if(code==CMP_NE) E(f,"xor eax,1"); return; }
        CALLRT(f,"rt_str_cmp"); E(f,"cmp eax,0"); E(f,"set%s al",cc_of(code,0)); E(f,"movzx eax,al"); return;
    }
    if(ta->k==TY_TUPLE&&tb->k==TY_TUPLE){
        E(f,"mov edx,eax"); E(f,"pop eax");
        if((code==CMP_EQ||code==CMP_NE) && aot_tuple_prefix(ta,tb)){ E(f,"mov eax,%d",code==CMP_NE); return; }   /* different lengths */
        if(code==CMP_EQ||code==CMP_NE){ CALLRT(f,"rt_tuple_eq"); if(code==CMP_NE) E(f,"xor eax,1"); return; }
        CALLRT(f,"rt_tuple_cmp"); E(f,"cmp eax,0"); E(f,"set%s al",cc_of(code,0)); E(f,"movzx eax,al"); return;
    }
    if(ta->k==TY_DICT&&(code==CMP_EQ||code==CMP_NE)){
        E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_dict_eq");
        if(code==CMP_NE) E(f,"xor eax,1"); return;
    }
    if((ta->k==TY_LIST||ta->k==TY_SET)&&(code==CMP_EQ||code==CMP_NE)){
        E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,ta->k==TY_LIST?"rt_list_eq":"rt_set_eq");
        if(code==CMP_NE) E(f,"xor eax,1"); return;
    }
    if(ta->k==TY_LIST&&tb->k==TY_LIST&&code<=CMP_GE){         /* lists: the first unequal elements decide */
        E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_list_cmp"); E(f,"cmp eax,0"); E(f,"set%s al",cc_of(code,0)); E(f,"movzx eax,al"); return;
    }
    if(ta->k==TY_SET&&tb->k==TY_SET&&code<=CMP_GE){          /* sets: subset / superset */
        int sup=code==CMP_GT||code==CMP_GE, strict=code==CMP_LT||code==CMP_GT;
        E(f,"mov edx,eax"); E(f,"pop eax");
        if(sup) E(f,"xchg eax,edx");                          /* a >= b: b <= a */
        if(strict){ int l=new_label(); E(f,"push eax"); E(f,"push edx"); CALLRT(f,"rt_set_le");
            E(f,"pop edx"); E(f,"pop ecx"); E(f,"test eax,eax"); E(f,"jz L%d",l);
            E(f,"mov eax,[ecx+8]"); E(f,"cmp eax,[edx+8]"); E(f,"setne al"); E(f,"movzx eax,al"); LBL(f,l); return; }
        CALLRT(f,"rt_set_le"); return;
    }
    if(code==CMP_EQ||code==CMP_NE||code==CMP_IS||code==CMP_ISNOT){     /* identity */
        E(f,"mov ecx,eax"); E(f,"pop eax"); E(f,"cmp eax,ecx"); E(f,"set%s al",cc_of(code,0)); E(f,"movzx eax,al"); return;
    }
    cg_fail(line,"unsupported comparison");
}
static void gen_cmp2_m(F *f, int code, Expr *a, Expr *b, int line, AFunc *m, int neg, int swap){
    (void)line;
    Ty *r=ty_find(m->ret);
    if(swap){ gen_op_call_r(f,m,b,a); if(r->k!=TY_BOOL) truth(f,r); if(neg) E(f,"xor eax,1"); return; }   /* 1 < obj: obj.__gt__(1) */
    if((code==CMP_EQ||code==CMP_NE) && is_opt(TY(a))){       /* a maybe None: None == b is b is None */
        Ty *pt=m->params[1]->ty; int total=op_frame(m), lcall=new_label(), ldone=new_label();
        E(f,"sub esp,%d",total);
        gen_borrow(f,a); E(f,"mov [esp],eax");
        gen_as(f,b,pt,0); put_arg(f,pt,4);
        op_defaults(f,m,2);
        E(f,"mov eax,[esp]"); E(f,"test eax,eax"); E(f,"jnz L%d",lcall);
        if(is_ptr(pt)){ E(f,"mov eax,[esp+4]"); E(f,"test eax,eax"); E(f,"sete al"); E(f,"movzx eax,al"); }
        else E(f,"xor eax,eax");
        E(f,"jmp L%d",ldone);
        LBL(f,lcall);
        call_on_top(f,m);
        if(r->k!=TY_BOOL) truth(f,r);
        LBL(f,ldone);
        E(f,"add esp,%d",total);
        if(neg) E(f,"xor eax,1");
        return; }
    if(code==CMP_IN||code==CMP_NOTIN){ int o=gen_op_call(f,m,b,a); (void)o; }     /* b.__contains__(a) */
    else gen_op_call(f,m,a,b);
    if(r->k!=TY_BOOL) truth(f,r);
    if(neg || code==CMP_NOTIN) E(f,"xor eax,1");
}
static void gen_cmp2(F *f, int code, Expr *a, Expr *b, int line){
    if(is_none_lit(a)||is_none_lit(b)){                 /* x is None / x == None */
        Expr *x=is_none_lit(a)?b:a; int eq=code==CMP_EQ||code==CMP_IS;
        if(is_none_lit(x)){ E(f,"mov eax,%d",eq); return; }
        Ty *t=TY(x); int o=gen(f,x);
        if(!is_opt(t) && !is_ptr(t)){ drop_value(f,t,o); E(f,"mov eax,%d",!eq); return; }   /* never None */
        if(is_ptr(t)){ if(o) hold(f); E(f,"test eax,eax"); E(f,"set%s al",eq?"z":"nz"); E(f,"movzx eax,al"); return; }
        int lnone=new_label(), lend=new_label();
        jump_if_none(f,t,lnone); drop_value(f,t,0); E(f,"mov eax,%d",!eq); E(f,"jmp L%d",lend);
        LBL(f,lnone); drop_value(f,t,0); E(f,"mov eax,%d",eq); LBL(f,lend);
        return;
    }
    Ty *ta=TY(a), *tb=TY(b);
    if((code==CMP_IN||code==CMP_NOTIN) && (tb->k==TY_LIST||tb->k==TY_SET)) ta=tb->elem;           /* as the elements are */
    if((code==CMP_IN||code==CMP_NOTIN) && tb->k==TY_DICT) ta=ty_dkey(tb);
    if(code!=CMP_IN && code!=CMP_NOTIN && numeric_ty(ta) && numeric_ty(tb)){                      /* numbers: as one type */
        Ty *ct=num_common(ta,tb);
        if((is_opt(ta)||is_opt(tb)) && (code==CMP_EQ||code==CMP_NE||code==CMP_IS||code==CMP_ISNOT)){   /* None == x: no error */
            int sa=frame_slot(f,8), sb=frame_slot(f,8), cnt=frame_slot(f,4), lnum=new_label(), lend=new_label();
            gen_borrow(f,a); store_new(f,ta,"ebp",sa); gen_borrow(f,b); store_new(f,tb,"ebp",sb);
            E(f,"mov dword [ebp%+d],0",cnt);
            for(int k=0;k<2;k++){ Ty *tk=k?tb:ta; if(!is_opt(tk)) continue; int ln=new_label(), lx=new_label();
                load_mem(f,tk,"ebp",k?sb:sa); jump_if_none(f,tk,ln); drop_value(f,tk,0); E(f,"jmp L%d",lx);
                LBL(f,ln); drop_value(f,tk,0); E(f,"add dword [ebp%+d],%d",cnt,k?2:1); LBL(f,lx); }
            E(f,"cmp dword [ebp%+d],0",cnt); E(f,"je L%d",lnum);
            E(f,"cmp dword [ebp%+d],3",cnt); E(f,"set%s al",(code==CMP_EQ||code==CMP_IS)?"e":"ne"); E(f,"movzx eax,al"); E(f,"jmp L%d",lend);
            LBL(f,lnum);
            load_mem(f,ta,"ebp",sa); conv_num(f,ta,ct); push_value(f,ct); load_mem(f,tb,"ebp",sb); conv_num(f,tb,ct);
            cmp_finish(f,code,ct,ct,line); LBL(f,lend); return;
        }
        static const char *sym[]={"<","<=",">",">="};
        if(code<=CMP_GE) g_none_text=none_msg("'%s' not supported between instances of 'NoneType' and '%s'",sym[code],py_type_name(tb));
        gen_as(f,a,ct,0); push_value(f,ct);
        if(code<=CMP_GE) g_none_text=none_msg("'%s' not supported between instances of '%s' and 'NoneType'",sym[code],py_type_name(ta));
        gen_as(f,b,ct,0); g_none_text=NULL;
        cmp_finish(f,code,ct,ct,line); return;
    }
    if(code<=CMP_GE && (is_opt(ta)||is_opt(tb))){                  /* ordering None: TypeError */
        static const char *sym[]={"<","<=",">",">="};
        gen_as(f,a,ta,0); check_none(f,ta,"TypeError",none_msg("'%s' not supported between instances of 'NoneType' and '%s'",sym[code],py_type_name(tb)));
        push_value(f,ta); gen_borrow(f,b);
        check_none(f,tb,"TypeError",none_msg("'%s' not supported between instances of '%s' and 'NoneType'",sym[code],py_type_name(ta)));
        cmp_finish(f,code,ta,tb,line); return;
    }
    gen_as(f,a,ta,0); push_value(f,ta);
    gen_borrow(f,b);
    if(code==CMP_IN||code==CMP_NOTIN) check_none(f,tb,"TypeError","argument of type 'NoneType' is not a container or iterable");
    cmp_finish(f,code,ta,tb,line);
}
/* a < b < c where some comparison is an object's method: each operand once, kept in a slot */
static void gen_compare_chain_m(F *f, Expr *e){
    XInfo *xi=xinfo(e); int lend=new_label(), ps=frame_slot(f,8), cs=frame_slot(f,8);
    Ty *prev=TY(e->items[0]);
    gen_borrow(f,e->items[0]); store_new(f,prev,"ebp",ps);
    for(int i=1;i<e->count;i++){
        Ty *t=TY(e->items[i]); int code=e->items[i]->akind; AFunc *m=xi->cmpfn[i];
        gen_borrow(f,e->items[i]); store_new(f,t,"ebp",cs);
        if(m){
            int contains= code==CMP_IN||code==CMP_NOTIN || ((xi->cmpswap>>i)&1);   /* b.__contains__(a), b.__gt__(a) */
            Ty *at=contains?prev:t, *pt=m->params[1]->ty; int sslot=contains?cs:ps, aslot=contains?ps:cs, total=op_frame(m);
            E(f,"sub esp,%d",total);
            E(f,"mov eax,[ebp%+d]",sslot); E(f,"mov [esp],eax");
            load_mem(f,at,"ebp",aslot); conv_num(f,at,pt); put_arg(f,pt,4);
            op_defaults(f,m,2);
            call_on_top(f,m); E(f,"add esp,%d",total);
            Ty *r=ty_find(m->ret); if(r->k!=TY_BOOL) truth(f,r);
            if(((xi->cmpneg>>i)&1) || code==CMP_NOTIN) E(f,"xor eax,1");
        } else {
            Ty *ct=(code!=CMP_IN && code!=CMP_NOTIN && numeric_ty(prev) && numeric_ty(t)) ? num_common(prev,t) : NULL;
            Ty *lt=ct?ct:prev, *rtt=ct?ct:t;
            load_mem(f,prev,"ebp",ps); conv_num(f,prev,lt); push_value(f,lt);
            load_mem(f,t,"ebp",cs); conv_num(f,t,rtt);
            cmp_finish(f,code,lt,rtt,e->line);
        }
        if(i<e->count-1){ E(f,"test eax,eax"); E(f,"jz L%d",lend); load_mem(f,t,"ebp",cs); store_new(f,t,"ebp",ps); }
        prev=t;
    }
    LBL(f,lend);
}
void gen_compare(F *f, Expr *e){
    XInfo *xi=xinfo(e);
    if(e->count==2 && xi->cmpfn && xi->cmpfn[1]){ gen_cmp2_m(f,e->items[1]->akind,e->items[0],e->items[1],e->line,xi->cmpfn[1],(xi->cmpneg>>1)&1,(xi->cmpswap>>1)&1); return; }
    if(e->count==2){ gen_cmp2(f,e->items[1]->akind,e->items[0],e->items[1],e->line); return; }
    if(xi->cmpfn) for(int i=1;i<e->count;i++) if(xi->cmpfn[i]){ gen_compare_chain_m(f,e); return; }
    int lend=new_label(), slot=frame_slot(f,8);           /* a < b < c: each middle operand once */
    Ty *prev=TY(e->items[0]);
    gen_borrow(f,e->items[0]);
    store_new(f,prev,"ebp",slot);
    for(int i=1;i<e->count;i++){
        Ty *t=TY(e->items[i]); int code=e->items[i]->akind;
        Ty *ct=(code!=CMP_IN && code!=CMP_NOTIN && numeric_ty(prev) && numeric_ty(t)) ? num_common(prev,t) : NULL;
        Ty *lt=ct?ct:prev, *rtt=ct?ct:t;
        load_mem(f,prev,"ebp",slot); conv_num(f,prev,lt); push_value(f,lt);
        gen_borrow(f,e->items[i]);
        if(i<e->count-1){ if(is_flt(t)) E(f,"fst qword [ebp%+d]",slot); else store_new(f,t,"ebp",slot); }
        conv_num(f,t,rtt);
        cmp_finish(f,code,lt,rtt,e->line);
        if(i<e->count-1){ E(f,"test eax,eax"); E(f,"jz L%d",lend); }
        prev=t;
    }
    LBL(f,lend);
}

/* eax = truth of e */
void gen_bool(F *f, Expr *e){
    if(e->kind==EXPR_COMPARE){ gen_compare(f,e); return; }
    if(e->kind==EXPR_UNARY && e->op==T_NOT){ gen_bool(f,e->a); E(f,"xor eax,1"); return; }
    if(e->kind==EXPR_BOOL){ int yes=new_label(), done=new_label();       /* (its operands may differ in type) */
        gen_jump(f,e,yes,1); E(f,"xor eax,eax"); E(f,"jmp L%d",done); LBL(f,yes); E(f,"mov eax,1"); LBL(f,done); return; }
    if(e->kind==EXPR_TRUE){ E(f,"mov eax,1"); return; }
    if(e->kind==EXPR_FALSE||e->kind==EXPR_NONE){ E(f,"xor eax,eax"); return; }
    gen_borrow(f,e);
    truth(f,TY(e));
}
static const char *neg_cc(const char *cc){
    static const char *pairs[][2]={{"l","ge"},{"ge","l"},{"le","g"},{"g","le"},{"e","ne"},{"ne","e"}};
    for(int i=0;i<6;i++) if(!strcmp(cc,pairs[i][0])) return pairs[i][1];
    return "ne";
}
static int int_like(Ty *t){ t=ty_find(t); return t->k==TY_INT||t->k==TY_BOOL; }
/* Jump to `label` when the truth of e equals want. Temporaries are released first. */
void gen_jump(F *f, Expr *e, int label, int want){
    if(e->kind==EXPR_UNARY && e->op==T_NOT){ gen_jump(f,e->a,label,!want); return; }
    if(e->kind==EXPR_TRUE||e->kind==EXPR_FALSE){ if((e->kind==EXPR_TRUE)==want) E(f,"jmp L%d",label); return; }
    if(e->kind==EXPR_BOOL){
        int both=(e->op==T_OR);
        if(want==both){ gen_jump(f,e->a,label,want); gen_jump(f,e->b,label,want); }
        else { int skip=new_label(); gen_jump(f,e->a,skip,!want); gen_jump(f,e->b,label,want); LBL(f,skip); }
        return;
    }
    int mark=scope_open(f);
    if(e->kind==EXPR_COMPARE && e->count==2 && int_like(TY(e->items[0])) && int_like(TY(e->items[1]))
       && !is_opt(TY(e->items[0])) && !is_opt(TY(e->items[1]))                    /* (None == 5: gen_cmp2's way) */
       && !is_none_lit(e->items[0]) && !is_none_lit(e->items[1]) && e->items[1]->akind<=CMP_NE){
        Ty *ct=num_common(TY(e->items[0]),TY(e->items[1])); int code=e->items[1]->akind;
        gen_as(f,e->items[0],ct,0); push_value(f,ct); gen_as(f,e->items[1],ct,0);
        if(is_lng(ct)){
            if(f->tmp_used==mark){ jcc64(f,code,want,label); return; }
            cmp64(f,code);
        } else {
            E(f,"mov ecx,eax"); E(f,"pop eax");
            const char *cc=cc_of(code,0);
            if(f->tmp_used==mark){ E(f,"cmp eax,ecx"); E(f,"j%s L%d",want?cc:neg_cc(cc),label); return; }
            E(f,"cmp eax,ecx"); E(f,"set%s al",cc); E(f,"movzx eax,al");
        }
    } else gen_bool(f,e);
    if(f->tmp_used>mark){ E(f,"push eax"); scope_close(f,mark); E(f,"pop eax"); }
    E(f,"test eax,eax"); E(f,"j%s L%d",want?"nz":"z",label);
}

