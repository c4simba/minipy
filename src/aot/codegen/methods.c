/* ========================= Typed compiler, code generator: methods of built-in types =========================
   Methods of str, bytes, list, dict, set ...; codecs. */

#include "gen.h"

/* the receiver of a method of a built-in type (AttributeError if None) */
static void gen_recv(F *f, Expr *obj, const char *m){
    gen_borrow(f,obj); check_none(f,TY(obj),"AttributeError",none_msg("'NoneType' object has no attribute '%s'",m));
}
/* s.find(sub[, start[, end]]) and the like: {s, sub, start, end, flags} on
   the stack for the runtime (flags: 1 no start, 2 no end, 4 sub is a tuple) */
static void gen_str_ranged(F *f, Expr *e, Expr *obj, const char *m, const char *routine, int mode){
    int flags=0, isbytes=TY(obj)->k==TY_BYTES;
    E(f,"sub esp,20");
    gen_recv(f,obj,m); if(isbytes) CALLRT(f,"rt_bytes_cp"); E(f,"mov [esp],eax");
    Ty *nt=TY(e->items[0]);
    if(isbytes && (nt->k==TY_INT||nt->k==TY_BOOL)){ gen(f,e->items[0]); if(!is_lng(nt)) widen(f,0); CALLRT(f,"rt_bytes_byte"); }   /* b.find(65): one byte */
    else gen_borrow(f,e->items[0]);
    E(f,"mov [esp+4],eax");
    if(TY(e->items[0])->k==TY_TUPLE) flags|=4;
    for(int k=1;k<3;k++){
        if(k<e->count && !is_none_lit(e->items[k])){ gen(f,e->items[k]); to_i32(f,TY(e->items[k])); E(f,"mov [esp+%d],eax",4+4*k); }
        else { flags|=k; E(f,"mov dword [esp+%d],0",4+4*k); }
    }
    E(f,"mov dword [esp+16],%d",flags);
    E(f,"mov eax,%d",mode); CALLRT(f,routine); E(f,"add esp,20");
}
/* an encoding's name -> rt_codec's number for it (-1: unknown, found out when the program runs) */
static int codec_number(const char *name){
    char n[40]; int k=0;
    if(strlen(name)>=sizeof n) return -1;
    for(const char *p=name;*p;p++){ char c=*p; if(c>='A'&&c<='Z') c=(char)(c+32); if(c=='_'||c==' ') c='-'; n[k++]=c; }
    n[k]=0;
    if(!strcmp(n,"utf-8")||!strcmp(n,"utf8")||!strcmp(n,"u8")) return 0;
    if(!strcmp(n,"ascii")||!strcmp(n,"us-ascii")) return 1;
    if(!strcmp(n,"latin-1")||!strcmp(n,"latin1")||!strcmp(n,"iso-8859-1")||!strcmp(n,"iso8859-1")||!strcmp(n,"l1")) return 2;
    return -1;
}
/* s.encode(encoding, errors), b.decode(...): routine gets eax = the object, edx = the codec, ecx = errors (0: strict) */
void gen_codec_call(F *f, Expr *obj, Expr *enc, Expr *errs, const char *m, const char *routine){
    if(m) gen_recv(f,obj,m); else gen_borrow(f,obj);
    E(f,"push eax");
    if(enc && !is_none_lit(enc)){
        int k= enc->kind==EXPR_LITERAL && enc->tok->kind==T_STRING ? codec_number(enc->tok->text) : -1;
        if(k>=0) E(f,"push %d",k); else { gen_borrow(f,enc); CALLRT(f,"rt_codec"); E(f,"push eax"); }
    } else E(f,"push 0");
    if(errs && !is_none_lit(errs)) gen_borrow(f,errs); else E(f,"xor eax,eax");
    E(f,"mov ecx,eax"); E(f,"pop edx"); E(f,"pop eax"); CALLRT(f,routine);
}
/* a byte order ("big" / "little") -> edx = 1 when little; checked when the program runs unless a literal */
void gen_byteorder(F *f, Expr *x){
    if(!x || is_none_lit(x)){ E(f,"xor edx,edx"); return; }
    if(x->kind==EXPR_LITERAL && x->tok->kind==T_STRING && (!strcmp(x->tok->text,"big")||!strcmp(x->tok->text,"little"))){ E(f,"mov edx,%d",x->tok->text[0]=='l'); return; }
    gen_borrow(f,x); CALLRT(f,"rt_byteorder"); E(f,"mov edx,eax");
}
int gen_tmethod(F *f, Expr *e, XInfo *xi){
    const char *m=xi->name; Expr *obj=e->a->a; Ty *t0=TY(obj);

    Expr *a0=e->count?e->items[0]:NULL, *a1=e->count>1?e->items[1]:NULL;
    if(t0->k==TY_STR){
        if(!strcmp(m,"format")) return gen_str_format(f,e);
        if(!strcmp(m,"ljust")||!strcmp(m,"rjust")||!strcmp(m,"center")||!strcmp(m,"zfill")){
            int mode= m[0]=='l' ? 1 : m[0]=='c' ? 3 : m[0]=='z' ? 4|'0'<<8 : 0;           /* see rt_sb_pad */
            rt("rt_sb_need"); E(f,"push dword [rt_sb_len]");
            gen_recv(f,obj,m); CALLRT(f,"rt_sb_str");
            gen(f,a0); to_i32(f,TY(a0)); E(f,"push eax");
            if(a1){ gen_borrow(f,a1); CALLRT(f,"rt_fillchar"); E(f,"shl eax,8"); E(f,"or eax,%d",mode); E(f,"mov edx,eax"); } else E(f,"mov edx,%d",mode);
            E(f,"pop ecx"); E(f,"mov eax,[esp]"); CALLRT(f,"rt_sb_pad");
            E(f,"pop eax"); CALLRT(f,"rt_sb_take"); return 1;
        }
        if(!strcmp(m,"strip")||!strcmp(m,"lstrip")||!strcmp(m,"rstrip")){
            gen_recv(f,obj,m);
            if(a0 && !is_none_lit(a0)){ E(f,"push eax"); gen_borrow(f,a0); E(f,"mov ecx,eax"); E(f,"pop eax"); } else E(f,"xor ecx,ecx");
            E(f,"mov edx,%d",m[0]=='s'?3:m[0]=='l'?1:2); CALLRT(f,"rt_str_strip"); return 1;
        }
        if(!strcmp(m,"encode")){ gen_codec_call(f,obj,a0,a1,m,"rt_str_encode"); return 1; }
        if(!strcmp(m,"startswith")||!strcmp(m,"endswith")){ gen_str_ranged(f,e,obj,m,"rt_str_affix",m[0]=='e'); return 0; }
        if(!strcmp(m,"find")||!strcmp(m,"rfind")||!strcmp(m,"index")||!strcmp(m,"rindex")){
            gen_str_ranged(f,e,obj,m,"rt_str_find",(m[0]=='r')|(strstr(m,"index")?2:0)); widen(f,1); return 0; }
        if(!strcmp(m,"count")){ gen_str_ranged(f,e,obj,m,"rt_str_count",0); widen(f,1); return 0; }
        if(!strcmp(m,"replace")){
            E(f,"sub esp,16"); gen_recv(f,obj,m); E(f,"mov [esp],eax");
            gen_borrow(f,a0); E(f,"mov [esp+4],eax"); gen_borrow(f,a1); E(f,"mov [esp+8],eax");
            if(e->count==3){ gen(f,e->items[2]); to_i32(f,TY(e->items[2])); E(f,"mov [esp+12],eax"); } else E(f,"mov dword [esp+12],-1");
            CALLRT(f,"rt_str_replace"); E(f,"add esp,16"); return 1;
        }
        if(!strcmp(m,"split")||!strcmp(m,"rsplit")){
            gen_recv(f,obj,m); E(f,"push eax");
            if(a0 && !is_none_lit(a0)) gen_borrow(f,a0); else E(f,"xor eax,eax");
            E(f,"push eax");
            if(a1){ gen(f,a1); to_i32(f,TY(a1)); E(f,"mov ecx,eax"); } else E(f,"or ecx,-1");
            E(f,"pop edx"); E(f,"pop eax"); rt("rt_str_split"); E(f,"call %s",m[0]=='r'?"rt_str_rsplit":"rt_str_split"); return 1;
        }
        if(!strcmp(m,"partition")||!strcmp(m,"rpartition")){
            gen_recv(f,obj,m); E(f,"push eax"); gen_borrow(f,a0); E(f,"mov edx,eax"); E(f,"pop eax");
            E(f,"mov ecx,TD%d",tdesc(TY(e))); rt("rt_str_partition"); E(f,"call %s",m[0]=='r'?"rt_str_rpartition":"rt_str_partition"); return 1;
        }
        if(!strcmp(m,"removeprefix")||!strcmp(m,"removesuffix")){
            gen_recv(f,obj,m); E(f,"push eax"); gen_borrow(f,a0); E(f,"mov edx,eax"); E(f,"pop eax");
            E(f,"mov ecx,%d",m[6]=='s'); CALLRT(f,"rt_str_remove"); return 1;
        }
        if(!strcmp(m,"expandtabs")){
            gen_recv(f,obj,m);
            if(a0){ E(f,"push eax"); gen(f,a0); to_i32(f,TY(a0)); E(f,"mov edx,eax"); E(f,"pop eax"); } else E(f,"mov edx,8");
            CALLRT(f,"rt_str_expandtabs"); return 1;
        }
        if(!strcmp(m,"splitlines")){
            gen_recv(f,obj,m);
            if(a0){ E(f,"push eax"); gen(f,a0); E(f,"mov edx,eax"); E(f,"pop eax"); } else E(f,"xor edx,edx");
            CALLRT(f,"rt_str_splitlines"); return 1;
        }
        gen_recv(f,obj,m);
        if(!strcmp(m,"upper")||!strcmp(m,"lower")||!strcmp(m,"swapcase")||!strcmp(m,"title")||!strcmp(m,"capitalize")||!strcmp(m,"casefold")){
            static const char *const modes[]={"upper","lower","swapcase","title","capitalize","casefold"};
            int k=0; for(int i=0;i<6;i++) if(!strcmp(m,modes[i])) k=i;
            E(f,"mov edx,%d",k); CALLRT(f,"rt_str_case"); return 1; }
        if(str_is_mode(m)>=0){ E(f,"mov edx,%d",str_is_mode(m)); CALLRT(f,"rt_str_is"); return 0; }
        E(f,"push eax");
        if(a0) gen_borrow(f,a0); else E(f,"xor eax,eax");
        if(a0 && !strcmp(m,"join") && TY(a0)->k==TY_STR){ CALLRT(f,"rt_str_chars"); hold(f); }    /* sep.join("abc") */
        if(a0 && !strcmp(m,"join") && TY(a0)->k==TY_SET){ E(f,"mov edx,%s",kd_of(TY(a0)->elem)); CALLRT(f,"rt_set_to_list"); hold(f); }
        E(f,"mov edx,eax"); E(f,"pop eax");
        if(!strcmp(m,"join")){ CALLRT(f,"rt_str_join"); return 1; }
    }
    if(t0->k==TY_BYTES){
        if(!strcmp(m,"decode")){ gen_codec_call(f,obj,a0,a1,m,"rt_bytes_decode"); return 1; }
        if(!strcmp(m,"startswith")||!strcmp(m,"endswith")){ gen_str_ranged(f,e,obj,m,"rt_str_affix",m[0]=='e'); return 0; }
        if(!strcmp(m,"find")||!strcmp(m,"rfind")||!strcmp(m,"index")||!strcmp(m,"rindex")){
            gen_str_ranged(f,e,obj,m,"rt_str_find",4|(m[0]=='r')|(strstr(m,"index")?2:0)); widen(f,1); return 0; }
        if(!strcmp(m,"count")){ gen_str_ranged(f,e,obj,m,"rt_str_count",0); widen(f,1); return 0; }
        if(!strcmp(m,"replace")){
            E(f,"sub esp,16"); gen_recv(f,obj,m); E(f,"mov [esp],eax");
            gen_borrow(f,a0); E(f,"mov [esp+4],eax"); gen_borrow(f,a1); E(f,"mov [esp+8],eax");
            if(e->count==3){ gen(f,e->items[2]); to_i32(f,TY(e->items[2])); E(f,"mov [esp+12],eax"); } else E(f,"mov dword [esp+12],-1");
            CALLRT(f,"rt_bytes_replace"); E(f,"add esp,16"); return 1;
        }
        if(!strcmp(m,"split")||!strcmp(m,"rsplit")){
            gen_recv(f,obj,m); E(f,"push eax");
            if(a0 && !is_none_lit(a0)) gen_borrow(f,a0); else E(f,"xor eax,eax");
            E(f,"push eax");
            if(a1){ gen(f,a1); to_i32(f,TY(a1)); E(f,"mov ecx,eax"); } else E(f,"or ecx,-1");
            E(f,"pop edx"); E(f,"pop eax"); rt("rt_bytes_split"); E(f,"call %s",m[0]=='r'?"rt_bytes_rsplit":"rt_bytes_split"); return 1;
        }
        if(!strcmp(m,"strip")||!strcmp(m,"lstrip")||!strcmp(m,"rstrip")){
            gen_recv(f,obj,m);
            if(a0 && !is_none_lit(a0)){ E(f,"push eax"); gen_borrow(f,a0); E(f,"mov ecx,eax"); E(f,"pop eax"); } else E(f,"xor ecx,ecx");
            E(f,"mov edx,%d",m[0]=='s'?3:m[0]=='l'?1:2); CALLRT(f,"rt_bytes_strip"); return 1;
        }
        if(!strcmp(m,"partition")||!strcmp(m,"rpartition")){
            gen_recv(f,obj,m); E(f,"push eax"); gen_borrow(f,a0); E(f,"mov edx,eax"); E(f,"pop eax");
            E(f,"mov ecx,TD%d",tdesc(TY(e))); rt("rt_str_partition"); E(f,"call %s",m[0]=='r'?"rt_str_rpartition":"rt_str_partition"); return 1;
        }
        if(!strcmp(m,"removeprefix")||!strcmp(m,"removesuffix")){
            gen_recv(f,obj,m); E(f,"push eax"); gen_borrow(f,a0); E(f,"mov edx,eax"); E(f,"pop eax");
            E(f,"mov ecx,%d",m[6]=='s'); CALLRT(f,"rt_str_remove"); return 1;
        }
        if(!strcmp(m,"splitlines")){
            gen_recv(f,obj,m);
            if(a0){ E(f,"push eax"); gen(f,a0); E(f,"mov edx,eax"); E(f,"pop eax"); } else E(f,"xor edx,edx");
            CALLRT(f,"rt_bytes_splitlines"); return 1;
        }
        if(!strcmp(m,"hex") && a0){
            gen_recv(f,obj,m); E(f,"push eax"); gen_borrow(f,a0); E(f,"push eax");
            if(a1){ gen_as(f,a1,TY_INT_T,0); to_i32(f,TY_INT_T); E(f,"mov ecx,eax"); } else E(f,"mov ecx,1");
            E(f,"pop edx"); E(f,"pop eax"); CALLRT(f,"rt_bytes_hex_sep"); return 1; }
        gen_recv(f,obj,m);
        if(!strcmp(m,"hex")){ CALLRT(f,"rt_bytes_hex"); return 1; }
        if(bytes_case_mode(m)>=0){ E(f,"mov edx,%d",bytes_case_mode(m)); CALLRT(f,"rt_bytes_case"); return 1; }
        if(bytes_is_mode(m)>=0){ E(f,"mov edx,%d",bytes_is_mode(m)); CALLRT(f,"rt_bytes_is"); return 0; }
        if(!strcmp(m,"join")){
            E(f,"push eax"); gen_borrow(f,a0);
            if(TY(a0)->k==TY_SET){ E(f,"mov edx,%s",kd_of(TY(a0)->elem)); CALLRT(f,"rt_set_to_list"); hold(f); }
            E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_str_join"); return 1; }
    }
    if(t0->k==TY_INT||t0->k==TY_BOOL){
        if(!strcmp(m,"to_bytes")){                              /* n.to_bytes(length=1, byteorder="big", signed=False) */
            gen_as(f,obj,TY_INT_T,0); push_value(f,TY_INT_T);
            if(a0 && !is_none_lit(a0)){ gen(f,a0); to_i32(f,TY(a0)); E(f,"push eax"); } else E(f,"push 1");
            gen_byteorder(f,a1); E(f,"push edx");
            if(e->count>2) gen(f,e->items[2]); else E(f,"xor eax,eax");
            E(f,"push eax"); CALLRT(f,"rt_int_to_bytes"); E(f,"add esp,20"); return 1; }
        gen_as(f,obj,TY_INT_T,0);
        if(!strcmp(m,"bit_length")){ CALLRT(f,"rt_int_bitlen"); widen(f,0); return 0; }
        if(!strcmp(m,"bit_count")){ CALLRT(f,"rt_int_bitcount"); widen(f,0); return 0; }
    }
    if(t0->k==TY_FLOAT && !strcmp(m,"is_integer")){             /* x - round(x) == 0 (a NaN for inf and nan) */
        gen_as(f,obj,TY_FLOAT_T,0); E(f,"fld st0"); E(f,"frndint"); E(f,"fsubp st1,st0"); E(f,"ftst"); E(f,"fnstsw ax"); E(f,"fstp st0"); E(f,"sahf");
        E(f,"sete al"); E(f,"setnp cl"); E(f,"and al,cl"); E(f,"movzx eax,al"); return 0; }
    if(t0->k==TY_LIST){
        Ty *el=t0->elem; int es=esize(el);
        gen_recv(f,obj,m);
        if(!strcmp(m,"reverse")){ CALLRT(f,"rt_list_reverse"); return 0; }
        if(!strcmp(m,"sort") && (xi->key||xi->rev)){
            int src=ref_slot(f); incref(f); E(f,"mov [ebp%+d],eax",src);
            int rev=gen_rev_flag(f,xi->rev);
            if(xi->key){ gen_sort_by_key(f,src,el,xi->key,rev,1); E(f,"mov eax,[ebp%+d]",src); E(f,"mov dword [ebp%+d],0",src); CALLRT(f,"rt_decref"); return 0; }
            E(f,"mov eax,[ebp%+d]",src); CALLRT(f,"rt_list_sort");
            { int l=new_label(); E(f,"cmp dword [ebp%+d],0",rev); E(f,"je L%d",l); E(f,"mov eax,[ebp%+d]",src); CALLRT(f,"rt_list_reverse"); LBL(f,l); }
            E(f,"mov eax,[ebp%+d]",src); E(f,"mov dword [ebp%+d],0",src); CALLRT(f,"rt_decref"); return 0;
        }
        if(!strcmp(m,"sort")){ CALLRT(f,"rt_list_sort"); return 0; }
        if(!strcmp(m,"clear")){ CALLRT(f,"rt_list_clear"); return 0; }
        if(!strcmp(m,"copy")){ E(f,"mov edx,%s",kd_of(el)); CALLRT(f,"rt_list_copy"); return 1; }
        E(f,"push eax");
        if(!strcmp(m,"append")){ gen_as(f,a0,el,1); list_append_top(f,el); E(f,"add esp,4"); return 0; }
        if(!strcmp(m,"pop")){ if(a0){ gen(f,a0); to_i32(f,TY(a0)); } else E(f,"or eax,-1");
            E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_list_pop"); load_mem(f,el,"eax",0); return is_ptr(el); }
        if(!strcmp(m,"insert")){ gen(f,a0); to_i32(f,TY(a0)); E(f,"push eax"); gen_as(f,a1,el,1); push_value(f,el);
            E(f,"mov eax,[esp+%d]",es+4); E(f,"mov edx,[esp+%d]",es); CALLRT(f,"rt_list_insert");
            E(f,"mov ecx,eax"); pop_value(f,el); store_new(f,el,"ecx",0);
            E(f,"add esp,8"); return 0; }
        if(!strcmp(m,"extend")){
            if(TY(a0)->k==TY_LIST) gen_borrow(f,a0); else { gen_list_of(f,a0,el); hold(f); }
            E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_list_extend"); return 0; }
        gen_as(f,a0,el,0); push_value(f,el);                     /* remove / index / count: the value by address */
        E(f,"mov edx,esp"); E(f,"mov eax,[esp+%d]",es);
        CALLRT(f,!strcmp(m,"remove")?"rt_list_remove":!strcmp(m,"index")?"rt_list_index":"rt_list_count");
        E(f,"add esp,%d",es+4);
        if(m[0]!='r') widen(f,0);
        return 0;
    }
    if(t0->k==TY_DICT){
        Ty *v=t0->elem, *kt=ty_dkey(t0); int vs=esize(v), ks=esize(kt);
        gen_recv(f,obj,m);
        if(!strcmp(m,"keys")){ CALLRT(f,"rt_dict_keys"); return 1; }
        if(!strcmp(m,"items")){ E(f,"mov edx,TD%d",tdesc(ty_find(TY(e))->elem)); CALLRT(f,"rt_dict_items"); return 1; }
        if(!strcmp(m,"values")){ CALLRT(f,"rt_dict_values"); return 1; }
        if(!strcmp(m,"clear")){ CALLRT(f,"rt_dict_clear"); return 0; }
        if(!strcmp(m,"copy")){ CALLRT(f,"rt_dict_copy"); return 1; }
        E(f,"push eax");
        if(!strcmp(m,"update")){ gen_borrow(f,a0); E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_dict_update"); return 0; }
        gen_as(f,a0,kt,0); push_value(f,kt);                     /* [esp] key, [esp+ks] the dict */
        E(f,"mov edx,esp"); E(f,"mov eax,[esp+%d]",ks); CALLRT(f,"rt_dict_find");
        int lmiss=new_label(), lend=new_label(), owned=0;
        E(f,"cmp eax,-1"); E(f,"je L%d",lmiss);
        if(!strcmp(m,"get")||!strcmp(m,"setdefault")){
            E(f,"mov ecx,[esp+%d]",ks); E(f,"mov ecx,[ecx+20]");
            if(vs==8) E(f,"lea ecx,[ecx+eax*8]"); else E(f,"lea ecx,[ecx+eax*4]");
            load_mem(f,v,"ecx",0);
            E(f,"jmp L%d",lend); LBL(f,lmiss);
            if(m[0]=='g'){ if(a1) gen_as(f,a1,v,0); else gen_none(f,v); }
            else { gen_as(f,a1,v,1); push_value(f,v);
                E(f,"lea edx,[esp+%d]",vs); E(f,"mov eax,[esp+%d]",vs+ks); CALLRT(f,"rt_dict_slot");
                E(f,"mov ecx,eax"); pop_value(f,v); store_new(f,v,"ecx",0); load_mem(f,v,"ecx",0); }
        } else {                                                    /* pop */
            E(f,"mov edx,esp"); E(f,"mov eax,[esp+%d]",ks); CALLRT(f,"rt_dict_del"); load_mem(f,v,"eax",0);
            E(f,"jmp L%d",lend); LBL(f,lmiss);
            if(a1) gen_as(f,a1,v,1); else { E(f,"mov eax,esp"); E(f,"mov ecx,%s",kd_of(kt)); CALLRT(f,"rt_raise_key"); }
            owned=1;
        }
        LBL(f,lend); E(f,"add esp,%d",ks+4);
        return owned && is_ptr(v);
    }
    if(t0->k==TY_FILE){
        gen_recv(f,obj,m);
        if(!strcmp(m,"read")){ CALLRT(f,"rt_file_read"); return 1; }
        if(!strcmp(m,"readline")){ CALLRT(f,"rt_file_readline"); return 1; }
        if(!strcmp(m,"readlines")){ CALLRT(f,"rt_file_readlines"); return 1; }
        if(!strcmp(m,"close")){ CALLRT(f,"rt_file_close"); return 0; }
        E(f,"push eax"); gen_borrow(f,a0); E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_file_write"); widen(f,0); return 0;
    }
    if(t0->k==TY_GEN){                                     /* g.send(v), g.throw(exc), g.close() */
        Ty *el=ty_find(t0->elem); int slot=frame_slot(f,4), lok=new_label();
        gen_borrow(f,obj); panic_if_null(f); E(f,"mov [ebp%+d],eax",slot);
        if(!strcmp(m,"close")||!strcmp(m,"aclose")){ E(f,"mov edx,1"); CALLRT(f,"rt_gen_close"); E(f,"test eax,eax"); E(f,"jz L%d",lok);
            rt("rt_raise_builtin"); E(f,"mov eax,VTX_RuntimeError"); E(f,"mov edx,DTX_RuntimeError"); E(f,"xor ecx,ecx");
            E(f,"mov esi,Z%d",zlit(m[0]=='a'?"async generator ignored GeneratorExit":"generator ignored GeneratorExit")); E(f,"call rt_raise_builtin");
            LBL(f,lok); return 0; }
        if(!strcmp(m,"throw")||!strcmp(m,"athrow")){ gen_owned(f,a0); E(f,"mov edx,eax"); E(f,"mov eax,[ebp%+d]",slot); CALLRT(f,"rt_gen_throw"); }
        else {
            Ty *st=gpart(t0,0);
            if(a0 && !is_none_lit(a0) && st->k!=TY_VOID){
                gen_as(f,a0,st,1); push_value(f,st);
                int ok=new_label(); E(f,"mov ecx,[ebp%+d]",slot); E(f,"cmp dword [ecx+8],0"); E(f,"jne L%d",ok);
                E(f,"mov esi,Z%d",zlit("can't send non-None value to a just-started generator")); CALLRT(f,"rt_typeerr_text");
                LBL(f,ok); pop_value(f,st);
                E(f,"mov ecx,[ebp%+d]",slot); store_mem(f,st,"ecx",88); E(f,"mov ecx,[ebp%+d]",slot); E(f,"mov dword [ecx+96],1"); }
            E(f,"mov eax,[ebp%+d]",slot); CALLRT(f,"rt_gen_next"); }
        E(f,"test eax,eax"); E(f,"jnz L%d",lok); gen_raise_stop(f,slot,t0);
        LBL(f,lok); E(f,"mov eax,[ebp%+d]",slot); load_mem(f,el,"eax",24); if(is_ptr(el)) incref(f);
        return is_ptr(el);
    }
    if(t0->k==TY_TASK){
        gen_recv(f,obj,m);
        if(!strcmp(m,"done")){ E(f,"cmp dword [eax+8],4"); E(f,"sete al"); E(f,"movzx eax,al"); return 0; }
        CALLRT(f,"rt_task_result"); if(ty_find(t0->elem)->k!=TY_VOID) load_mem(f,t0->elem,"eax",24); return 0;
    }
    if(t0->k==TY_SET){
        Ty *el=t0->elem; int es=esize(el);
        gen_recv(f,obj,m);
        if(!strcmp(m,"clear")){ CALLRT(f,"rt_set_clear"); return 0; }
        if(!strcmp(m,"copy")){ E(f,"mov edx,%s",kd_of(el)); CALLRT(f,"rt_set_copy"); return 1; }
        if(!strcmp(m,"pop")){ CALLRT(f,"rt_set_pop"); load_mem(f,el,"eax",0); return is_ptr(el); }
        E(f,"push eax");
        static const char *ops[]={"union","intersection","difference","symmetric_difference",NULL};
        static const char *iops[]={"update","intersection_update","difference_update","symmetric_difference_update",NULL};
        for(int k=0;ops[k];k++) if(!strcmp(m,ops[k])||!strcmp(m,iops[k])){
            if(TY(a0)->k!=TY_SET) cg_fail(e->line,"set methods take sets in compiled code");
            gen_borrow(f,a0);
            E(f,"mov edx,eax"); E(f,"pop eax"); E(f,"mov ecx,%d",k);
            if(!strcmp(m,ops[k])){ CALLRT(f,"rt_set_op"); return 1; }
            CALLRT(f,"rt_set_iop"); return 0; }
        if(!strcmp(m,"issubset")||!strcmp(m,"issuperset")||!strcmp(m,"isdisjoint")){
            gen_borrow(f,a0); E(f,"mov edx,eax"); E(f,"pop eax");
            if(!strcmp(m,"issuperset")) E(f,"xchg eax,edx");
            CALLRT(f,m[2]=='d'?"rt_set_disjoint":"rt_set_le"); return 0; }
        gen_as(f,a0,el,0);
        if(!strcmp(m,"add")){ set_add_top(f,el); E(f,"add esp,4"); return 0; }
        push_value(f,el);                                        /* remove / discard: the value by address */
        E(f,"mov edx,esp"); E(f,"mov eax,[esp+%d]",es); CALLRT(f,m[0]=='d'?"rt_set_discard":"rt_set_remove");
        E(f,"add esp,%d",es+4); return 0;
    }
    cg_fail(e->line,"unsupported method");
}

