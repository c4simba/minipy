/* ========================= Typed compiler, code generator: formatters =========================
   json.dumps; the routines writing str() / repr() / JSON of containers
   and objects (FMT<i>). */

#include "gen.h"

/* ---- json.dumps: JSON writers by type (JSON formatters FMT<i> have repr 2+variant) */
static int json_variant(const char *isep, const char *ksep, int ascii){
    for(int i=0;i<gg->njvars;i++) if(!strcmp(gg->jvars[i].isep,isep) && !strcmp(gg->jvars[i].ksep,ksep) && gg->jvars[i].ascii==ascii) return i;
    if(gg->njvars==gg->cjvars){ gg->cjvars=gg->cjvars?gg->cjvars*2:4; gg->jvars=xrealloc(gg->jvars,sizeof(*gg->jvars)*(size_t)gg->cjvars); }
    gg->jvars[gg->njvars].isep=isep; gg->jvars[gg->njvars].ksep=ksep; gg->jvars[gg->njvars].ascii=ascii;
    return gg->njvars++;
}
static void sb_cstr_lit(F *f, const char *s){ E(f,"mov eax,Z%d",zlit(s)); CALLRT(f,"rt_sb_cstr"); }
/* append the JSON text of the value in eax/st0 (borrowed) */
static void gen_json(F *f, Ty *t, int v){
    t=ty_find(t);
    if(t->opt && (!is_ptr(t) || t->k==TY_STR)){             /* None: null */
        int lnone=new_label(), lend=new_label();
        jump_if_none(f,t,lnone);
        Ty plain=*t; plain.opt=0; gen_json(f,&plain,v); E(f,"jmp L%d",lend);
        LBL(f,lnone); drop_value(f,t,0); sb_cstr_lit(f,"null");
        LBL(f,lend); return; }
    switch(t->k){
        case TY_INT: CALLRT(f,"rt_sb_int"); return;
        case TY_BOOL:{ int l=new_label(); E(f,"mov ecx,Z%d",zlit("true")); E(f,"test eax,eax"); E(f,"jnz L%d",l); E(f,"mov ecx,Z%d",zlit("false")); LBL(f,l); E(f,"mov eax,ecx"); CALLRT(f,"rt_sb_cstr"); return; }
        case TY_FLOAT: CALLRT(f,"rt_sb_jfloat"); return;
        case TY_STR: E(f,"mov edx,%d",gg->jvars[v].ascii); CALLRT(f,"rt_sb_json_str"); return;
        case TY_LIST: case TY_DICT: case TY_OBJ: case TY_TUPLE: rt("rt_sb_need"); E(f,"call FMT%d",fmt_index(t,2+v)); return;
        default: sb_cstr_lit(f,"null"); return;
    }
}
static void emit_json_formatter(int i){
    F ff; memset(&ff,0,sizeof ff); F *f=&ff;
    Ty *t=gg->fmts[i].ty; int v=gg->fmts[i].repr-2;
    const char *isep=gg->jvars[v].isep, *ksep=gg->jvars[v].ksep;
    buf_printf(&f->code,"\nFMT%d:                         ; JSON of %s\n",i,ty_name(t));
    int lnull=new_label();
    E(f,"test eax,eax"); E(f,"jz L%d",lnull);
    if(t->k==TY_OBJ || (t->k==TY_TUPLE && t->names)){        /* {"field": value, ...} */
        E(f,"push eax"); E(f,"mov al,'{'"); CALLRT(f,"rt_sb_char");
        AField *fl[256]; int nf=0;
        if(t->k==TY_OBJ){ AClass *chain[32]; int nc=0; for(AClass *k=t->cls;k && nc<32;k=k->base) chain[nc++]=k;
            for(int c=nc-1;c>=0;c--) for(int k=0;k<chain[c]->nfields && nf<256;k++){ AField *fd=chain[c]->fields[k];
                if(fd->name[0]!='_' && !fd->over && !aot_field_shared(fd) && !(fd->cvar && !aot_field_root(fd)->inst_set)) fl[nf++]=fd; } }
        int n=t->k==TY_OBJ?nf:t->nelems;
        for(int k=0;k<n;k++){
            const char *name=t->k==TY_OBJ?fl[k]->name:t->names[k];
            Ty *ft=t->k==TY_OBJ?fl[k]->ty:t->elems[k]; int off=t->k==TY_OBJ?fl[k]->offset:tuple_off(t,k);
            char key[400]; snprintf(key,sizeof key,"%s\"%s\"%s",k?isep:"",name,ksep);
            sb_cstr_lit(f,key);
            E(f,"mov eax,[esp]"); load_mem(f,ft,"eax",off); gen_json(f,ft,v);
        }
        E(f,"pop eax"); E(f,"mov al,'}'"); E(f,"jmp rt_sb_char");
    } else if(t->k==TY_TUPLE){                                 /* a tuple: a JSON array */
        E(f,"push eax"); E(f,"mov al,'['"); CALLRT(f,"rt_sb_char");
        for(int k=0;k<t->nelems;k++){
            if(k) sb_cstr_lit(f,isep);
            E(f,"mov eax,[esp]"); load_mem(f,t->elems[k],"eax",tuple_off(t,k)); gen_json(f,t->elems[k],v);
        }
        E(f,"pop eax"); E(f,"mov al,']'"); E(f,"jmp rt_sb_char");
    } else {                                                   /* list / dict */
        int ltop=new_label(), lend=new_label(), lskip=new_label(), isdict=t->k==TY_DICT;
        Ty *el=t->elem;
        E(f,"push ebx"); E(f,"push esi"); E(f,"mov ebx,eax");
        if(isdict) CALLRT(f,"rt_dict_flat");
        E(f,"mov al,'%c'",isdict?'{':'['); CALLRT(f,"rt_sb_char");
        E(f,"xor esi,esi");
        LBL(f,ltop);
        E(f,"cmp esi,[ebx+8]"); E(f,"jae L%d",lend);
        E(f,"test esi,esi"); E(f,"jz L%d",lskip); sb_cstr_lit(f,isep); LBL(f,lskip);
        if(isdict){
            Ty *kt=ty_find(ty_dkey(t));
            if(kt->k==TY_STR){ E(f,"mov eax,[ebx+16]"); load_at(f,kt,"eax","esi",8); E(f,"mov edx,%d",gg->jvars[v].ascii); CALLRT(f,"rt_sb_json_str"); }
            else { E(f,"mov al,'\"'"); CALLRT(f,"rt_sb_char");
                E(f,"mov eax,[ebx+16]"); load_at(f,kt,"eax","esi",8);
                gen_json(f,kt,v); E(f,"mov al,'\"'"); CALLRT(f,"rt_sb_char"); }
            sb_cstr_lit(f,ksep);
            E(f,"mov eax,[ebx+20]");
        } else E(f,"mov eax,[ebx+16]");
        load_at(f,el,"eax","esi",esize(el));
        gen_json(f,el,v);
        E(f,"inc esi"); E(f,"jmp L%d",ltop);
        LBL(f,lend); E(f,"mov al,'%c'",isdict?'}':']'); CALLRT(f,"rt_sb_char");
        E(f,"pop esi"); E(f,"pop ebx"); E(f,"ret");
    }
    LBL(f,lnull); E(f,"mov eax,Z%d",zlit("null")); E(f,"jmp rt_sb_cstr"); rt("rt_sb_cstr");
    buf_cat(&gg->text,&f->code); free(f->code.s);
}
int gen_json_dumps(F *f, Expr *e, XInfo *xi){
    const char *isep=", ", *ksep=": ";
    if(xi->key){ isep=xi->key->items[0]->tok->text; ksep=xi->key->items[1]->tok->text; }
    int v=json_variant(isep,ksep,xi->argmap[0]);
    Expr *x=e->items[0];
    rt("rt_sb_need"); E(f,"push dword [rt_sb_len]");
    if(x->kind==EXPR_NONE) sb_cstr_lit(f,"null");
    else { gen_borrow(f,x); gen_json(f,TY(x),v); }
    E(f,"pop eax"); CALLRT(f,"rt_sb_take");
    return 1;
}

/* what str() / repr() of an object of class k calls: __str__ (an exception: none, its message), else __repr__ */
static AFunc *fmt_method(AClass *k, int repr){
    AFunc *m=repr?NULL:aot_find_method(k,"__str__");
    if(!m && (repr || !aot_is_exception(k))) m=aot_find_method(k,"__repr__");
    if(m && (m->is_static || ty_find(m->ret)->k!=TY_STR)) m=NULL;
    return m;
}
void emit_formatter(int i){
    if(gg->fmts[i].repr>=2){ emit_json_formatter(i); return; }
    F ff; memset(&ff,0,sizeof ff); F *f=&ff;
    Ty *t=gg->fmts[i].ty; int repr=gg->fmts[i].repr;
    buf_printf(&f->code,"\nFMT%d:                         ; %s of %s\n",i,repr?"repr":"str",ty_name(t));
    if(t->k==TY_OBJ){
        AClass *cls=t->cls;
        AFunc *m=fmt_method(cls,repr);
        int lnone=new_label();
        E(f,"test eax,eax"); E(f,"jz L%d",lnone);
        {   /* an object of a subclass with a __str__ / __repr__ of its own (not an override of this one's) */
            AClass *ds[64]; int nd=0;
            for(int i=0;i<gg->p->nclasses && nd<64;i++){ AClass *d=gg->p->classes[i]; AFunc *dm;
                if(d==cls || !aot_subclass(d,cls) || !(dm=fmt_method(d,repr)) || dm->cls!=d || dm==m) continue;
                if(m && !strcmp(m->name,dm->name)) continue;          /* (the vtable call reaches it) */
                ds[nd++]=d; }
            for(int a=0;a<nd;a++) for(int b=a+1;b<nd;b++){ int da=0, db=0; for(AClass *x=ds[a];x;x=x->base) da++; for(AClass *x=ds[b];x;x=x->base) db++; if(db>da){ AClass *tmp=ds[a]; ds[a]=ds[b]; ds[b]=tmp; } }
            for(int i=0;i<nd;i++){ AFunc *dm=fmt_method(ds[i],repr); int lnext=new_label();
                use_class(ds[i]); use_fn(dm);
                E(f,"push eax"); E(f,"mov edx,%s",class_label(ds[i],0)); CALLRT(f,"rt_isinstance"); E(f,"test eax,eax"); E(f,"pop eax"); E(f,"jz L%d",lnext);
                E(f,"push eax");
                if(dm->overridden){ E(f,"mov eax,[eax+8]"); E(f,"call dword [eax+%d]",8+4*dm->vslot); }
                else E(f,"call %s",fnl(dm,LB_CODE));
                E(f,"mov [esp],eax"); CALLRT(f,"rt_sb_str"); E(f,"pop eax"); CALLRT(f,"rt_decref"); E(f,"ret");
                LBL(f,lnext); }
        }
        if(m){
            use_fn(m);
            E(f,"push eax");
            if(m->overridden){ E(f,"mov eax,[eax+8]"); E(f,"call dword [eax+%d]",8+4*m->vslot); }
            else E(f,"call %s",fnl(m,LB_CODE));
            E(f,"mov [esp],eax"); CALLRT(f,"rt_sb_str"); E(f,"pop eax"); CALLRT(f,"rt_decref"); E(f,"ret");
        } else if(aot_is_exception(cls)){                   /* str(e): the message; repr(e): Name('message') */
            if(!repr){ E(f,"mov eax,[eax+12]"); E(f,"jmp rt_sb_str"); rt("rt_sb_str"); }
            else {
                int lno=new_label(), lstr=new_label(), ldone=new_label();
                E(f,"push eax"); E(f,"mov eax,[eax+8]"); E(f,"mov eax,[eax]"); CALLRT(f,"rt_sb_str");
                E(f,"mov al,'('"); CALLRT(f,"rt_sb_char");
                AClass *ke=NULL; for(int i=0;i<gg->p->nclasses;i++) if(gg->p->classes[i]->builtin && !strcmp(gg->p->classes[i]->name,"KeyError")) ke=gg->p->classes[i];
                { AField *ar=aot_find_field(cls,"_argrepr");          /* OSError(errno, strerror): repr shows those two */
                  if(ar){ int lnoar=new_label(); E(f,"mov eax,[esp]"); E(f,"mov eax,[eax+%d]",ar->offset); E(f,"test eax,eax"); E(f,"jz L%d",lnoar); CALLRT(f,"rt_sb_str"); E(f,"jmp L%d",ldone); LBL(f,lnoar); } }
                E(f,"mov eax,[esp]"); E(f,"cmp dword [eax+12],0"); E(f,"je L%d",lno);
                E(f,"cmp dword [eax+%d],0",exc_rawoff(cls)); E(f,"jne L%d",lstr);          /* the message is the repr already */
                if(is_keyerror(cls)) E(f,"jmp L%d",lstr);                                  /* (a KeyError the run time raised) */
                else if(ke && aot_subclass(ke,cls)){ use_class(ke); E(f,"mov edx,%s",class_label(ke,0)); CALLRT(f,"rt_isinstance"); E(f,"test eax,eax"); E(f,"jnz L%d",lstr); E(f,"mov eax,[esp]"); }
                E(f,"mov eax,[eax+12]"); CALLRT(f,"rt_sb_repr_str"); E(f,"jmp L%d",ldone);
                LBL(f,lstr); E(f,"mov eax,[esp]"); E(f,"mov eax,[eax+12]"); CALLRT(f,"rt_sb_str");
                LBL(f,ldone); LBL(f,lno); E(f,"pop eax"); E(f,"mov al,')'"); E(f,"jmp rt_sb_char");
            }
        } else {
            E(f,"push eax"); E(f,"mov al,'<'"); CALLRT(f,"rt_sb_char");
            E(f,"pop eax"); E(f,"mov eax,[eax+8]"); E(f,"mov eax,[eax]"); CALLRT(f,"rt_sb_str");
            E(f,"mov eax,Z%d",zlit(" object>")); CALLRT(f,"rt_sb_cstr"); E(f,"ret");
        }
        LBL(f,lnone); E(f,"mov eax,Z%d",zlit("None")); E(f,"jmp rt_sb_cstr"); rt("rt_sb_cstr");
        buf_cat(&gg->text,&f->code); free(f->code.s);
        return;
    }
    if(t->k==TY_TUPLE && t->names){                       /* a record: {'key': value, ...} */
        int lnone=new_label();
        E(f,"test eax,eax"); E(f,"jz L%d",lnone);
        E(f,"push eax"); E(f,"mov al,'{'"); CALLRT(f,"rt_sb_char");
        for(int i=0;i<t->nelems;i++){
            char key[300]; snprintf(key,sizeof key,"%s'%s': ",i?", ":"",t->names[i]);
            E(f,"mov eax,Z%d",zlit(key)); CALLRT(f,"rt_sb_cstr");
            E(f,"mov eax,[esp]"); load_mem(f,t->elems[i],"eax",tuple_off(t,i)); gen_fmt(f,t->elems[i],1);
        }
        E(f,"pop eax"); E(f,"mov al,'}'"); E(f,"jmp rt_sb_char");
        LBL(f,lnone); E(f,"mov eax,Z%d",zlit("None")); E(f,"jmp rt_sb_cstr"); rt("rt_sb_cstr");
        buf_cat(&gg->text,&f->code); free(f->code.s);
        return;
    }
    if(t->k==TY_TUPLE){                                   /* (a, b) / (a,) */
        int lnone=new_label();
        E(f,"test eax,eax"); E(f,"jz L%d",lnone);
        E(f,"push eax"); E(f,"mov al,'('"); CALLRT(f,"rt_sb_char");
        for(int i=0;i<t->nelems;i++){
            if(i){ E(f,"mov eax,Z%d",zlit(", ")); CALLRT(f,"rt_sb_cstr"); }
            E(f,"mov eax,[esp]"); load_mem(f,t->elems[i],"eax",tuple_off(t,i)); gen_fmt(f,t->elems[i],1);
        }
        if(t->nelems==1){ E(f,"mov al,','"); CALLRT(f,"rt_sb_char"); }
        E(f,"pop eax"); E(f,"mov al,')'"); E(f,"jmp rt_sb_char");
        LBL(f,lnone); E(f,"mov eax,Z%d",zlit("None")); E(f,"jmp rt_sb_cstr"); rt("rt_sb_cstr");
        buf_cat(&gg->text,&f->code); free(f->code.s);
        return;
    }
    int ltop=new_label(), lend=new_label(), lskip=new_label(), lout=new_label();
    int isdict=t->k==TY_DICT, isset=t->k==TY_SET;
    Ty *el=t->elem;
    E(f,"push ebx"); E(f,"push esi"); E(f,"push 0"); E(f,"mov ebx,eax");
    { int l=new_label(); E(f,"test ebx,ebx"); E(f,"jnz L%d",l); E(f,"mov eax,Z%d",zlit("None")); CALLRT(f,"rt_sb_cstr"); E(f,"jmp L%d",lout); LBL(f,l); }
    if(isdict){ E(f,"mov eax,ebx"); CALLRT(f,"rt_dict_flat"); }
    if(isset){ int lne=new_label(), lgo=new_label();               /* the empty set prints as set() */
        E(f,"test ebx,ebx"); E(f,"jz L%d",lne); E(f,"cmp dword [ebx+8],0"); E(f,"jne L%d",lgo);
        LBL(f,lne); E(f,"mov eax,Z%d",zlit("set()")); CALLRT(f,"rt_sb_cstr"); E(f,"jmp L%d",lout);
        LBL(f,lgo);                                                 /* its elements in its order, as a list */
        E(f,"mov eax,ebx"); E(f,"mov edx,%s",kd_of(el)); CALLRT(f,"rt_set_to_list"); E(f,"mov ebx,eax"); E(f,"mov [esp],eax"); }
    E(f,"mov al,'%c'",t->k==TY_LIST?(t->tup?'(':'['):'{'); CALLRT(f,"rt_sb_char");
    E(f,"xor esi,esi");
    LBL(f,ltop);
    E(f,"test ebx,ebx"); E(f,"jz L%d",lend); E(f,"cmp esi,[ebx+8]"); E(f,"jae L%d",lend);
    E(f,"test esi,esi"); E(f,"jz L%d",lskip); E(f,"mov eax,Z%d",zlit(", ")); CALLRT(f,"rt_sb_cstr"); LBL(f,lskip);
    if(isdict){
        E(f,"mov eax,[ebx+16]"); load_at(f,ty_dkey(t),"eax","esi",8); gen_fmt(f,ty_dkey(t),1);
        E(f,"mov eax,Z%d",zlit(": ")); CALLRT(f,"rt_sb_cstr");
        E(f,"mov eax,[ebx+20]");
    } else E(f,"mov eax,[ebx+16]");
    load_at(f,el,"eax","esi",esize(el));
    gen_fmt(f,el,1);
    E(f,"inc esi"); E(f,"jmp L%d",ltop);
    LBL(f,lend);
    if(t->k==TY_LIST && t->tup){ int l=new_label(); E(f,"cmp esi,1"); E(f,"jne L%d",l); E(f,"mov al,','"); CALLRT(f,"rt_sb_char"); LBL(f,l); }   /* (x,) */
    E(f,"mov al,'%c'",t->k==TY_LIST?(t->tup?')':']'):'}'); CALLRT(f,"rt_sb_char");
    LBL(f,lout); E(f,"pop eax"); CALLRT(f,"rt_decref"); E(f,"pop esi"); E(f,"pop ebx"); E(f,"ret");
    buf_cat(&gg->text,&f->code); free(f->code.s);
}

