/* ========================= Typed compiler, code generator: formatting =========================
   "%" formatting, format specs (printf and Python style, f-strings),
   str.format. */

#include "gen.h"

/* ---- "%" formatting with a literal format string (also what f-strings with a spec become) ---- */
void sb_bytes_lit(F *f, const char *s, int n){
    if(n<=0) return;
    if(n==1){ E(f,"mov al,%d",(unsigned char)s[0]); CALLRT(f,"rt_sb_char"); return; }
    E(f,"mov eax,RL%d",rlit(s,n)); E(f,"mov edx,%d",n); CALLRT(f,"rt_sb_bytes");
}
static void fmt_arg(F *f, Expr *a, Ty *tt, int tslot, int i, int as_float){
    if(tslot){ Ty *et=ty_find(tt)->elems[i]; E(f,"mov eax,[ebp%+d]",tslot); load_mem(f,et,"eax",tuple_off(tt,i)); if(as_float) conv_num(f,et,TY_FLOAT_T); return; }
    if(as_float) gen_as(f,a,TY_FLOAT_T,0); else gen_borrow(f,a);
}

/* ---- format specs: printf ("%-8.2f") and Python ("{:>8,.2f}", f-strings) ---- */
/* [[fill]align][sign][#][0][width][,][.precision][type], printf flags (- + space # 0) accepted too */
int parse_spec(const char *s, int n, Spec *sp){
    memset(sp,0,sizeof *sp); sp->prec=-1;
    int i=0;
    unsigned b0=n?(unsigned char)s[0]:0; int fl= b0>=0xF0 ? 4 : b0>=0xE0 ? 3 : b0>=0xC0 ? 2 : 1;   /* the fill: one UTF-8 character */
    if(n>fl && s[fl] && strchr("<>^=",s[fl])){
        unsigned c= fl==1 ? b0 : b0&(0xFF>>(fl+1));
        for(int k=1;k<fl;k++) c=(c<<6)|((unsigned char)s[k]&0x3F);
        sp->fill=(int)c; sp->align=s[fl]; i=fl+1; }
    else if(n>=1 && s[0] && strchr("<>^=",s[0])){ sp->align=s[0]; i=1; }
    for(;i<n;i++){ char c=s[i];
        if(c=='-') sp->align='<'; else if(c=='+'||c==' ') sp->sign=c; else if(c=='#') sp->alt=1; else if(c=='0') sp->zero=1; else break; }
    if(i<n && s[i]=='\001'){ sp->wslot=-1; i++; }                        /* {w}: filled in by the caller */
    else for(;i<n && s[i]>='0' && s[i]<='9';i++) sp->width=sp->width*10+(s[i]-'0');
    if(i<n && (s[i]==','||s[i]=='_')){ sp->comma=s[i]; i++; }
    if(i<n && s[i]=='.'){ i++; sp->prec=0; if(i<n && s[i]=='\001'){ sp->pslot=-1; i++; } else for(;i<n && s[i]>='0' && s[i]<='9';i++) sp->prec=sp->prec*10+(s[i]-'0'); }
    if(i<n) sp->type=s[i++];
    return i;
}
static void src_load(F *f, ArgSrc *a, int as_float){ fmt_arg(f,a->e,a->tt,a->tslot,a->idx,as_float); }
/* Append one formatted value. pystyle: Python's defaults (strings left-aligned). */
void emit_field(F *f, Spec *sp, Ty *t, ArgSrc *src, int repr, int pystyle, int line){
    t=ty_find(t);
    int num=t->k==TY_INT||t->k==TY_BOOL||t->k==TY_FLOAT;
    char ty=sp->type;
    if(!ty||ty=='n') ty = t->k==TY_FLOAT ? (sp->prec>=0?1:2) : (t->k==TY_INT||(t->k==TY_BOOL&&sp->width)) ? 'd' : 's';   /* 1: like g, keeps ".0"; 2: repr */
    int align=sp->align;
    if(!align) align = (pystyle && !num && ty=='s') ? '<' : '>';
    int padded=sp->width>0 || sp->wslot;
    if(padded) E(f,"push dword [rt_sb_len]");
    if(sp->sign && num && ty!='s') E(f,"push dword [rt_sb_len]");
    int prec=sp->prec, ps=sp->pslot;
    #define PREC(dflt,lo,hi,add) do{ if(ps){ int l1_=new_label(), l2_=new_label(); E(f,"mov edx,[ebp%+d]",ps); E(f,"cmp edx,%d",lo); E(f,"jge L%d",l1_); E(f,"mov edx,%d",lo); LBL(f,l1_); \
        E(f,"cmp edx,%d",hi); E(f,"jle L%d",l2_); E(f,"mov edx,%d",hi); LBL(f,l2_); if(add) E(f,"add edx,%d",add); } \
        else E(f,"mov edx,%d",(prec<0?(dflt):prec<(lo)?(lo):prec>(hi)?(hi):prec)+(add)); }while(0)
    int grp= sp->comma && num && ty!='s' && ty!='c';             /* 1,234,567 / 1_2345_6789 */
    if(grp) E(f,"push dword [rt_sb_len]");
    switch(ty){
        case 's': case 'r':
            if(prec>=0||ps) E(f,"push dword [rt_sb_len]");
            src_load(f,src,0); gen_fmt(f,t,repr||ty=='r');
            if(prec>=0||ps){ E(f,"pop eax"); if(ps) E(f,"mov edx,[ebp%+d]",ps); else E(f,"mov edx,%d",prec); CALLRT(f,"rt_sb_trunc"); }       /* "%.3s": the first 3 characters */
            break;
        case 'd': case 'i': case 'u':
            src_load(f,src,0); if(is_flt(t)) CALLRT(f,"rt_ftoi"); else if(!is_lng(t)) widen(f,0);
            CALLRT(f,"rt_sb_int"); break;
        case 'x': case 'X': case 'o': case 'b':{
            src_load(f,src,0); if(is_flt(t)) CALLRT(f,"rt_ftoi"); else if(!is_lng(t)) widen(f,0);
            if(sp->alt){ int l=new_label(); E(f,"test edx,edx"); E(f,"jns L%d",l); E(f,"push edx"); E(f,"push eax"); E(f,"mov al,'-'"); CALLRT(f,"rt_sb_char"); E(f,"pop eax"); E(f,"pop edx");
                E(f,"neg eax"); E(f,"adc edx,0"); E(f,"neg edx"); LBL(f,l);
                E(f,"push edx"); E(f,"push eax"); E(f,"mov al,'0'"); CALLRT(f,"rt_sb_char"); E(f,"mov al,'%c'",ty=='o'?'o':ty=='b'?'b':ty); CALLRT(f,"rt_sb_char"); E(f,"pop eax"); E(f,"pop edx"); }
            E(f,"mov ecx,%d",(ty=='o'?8:ty=='b'?2:16)|(ty=='X'?0x100:0)); CALLRT(f,"rt_sb_radix"); break; }
        case 'f': case 'F': case '%':
            src_load(f,src,1);
            if(ty=='%'){ E(f,"push 100"); E(f,"fimul dword [esp]"); E(f,"add esp,4"); }
            PREC(6,0,40,0); CALLRT(f,"rt_sb_fixed");
            if(ty=='%'){ E(f,"mov al,'%%'"); CALLRT(f,"rt_sb_char"); }
            break;
        case 'e': case 'E':
            src_load(f,src,1); PREC(6,0,16,1); E(f,"mov ecx,4"); CALLRT(f,"rt_sb_gen"); break;
        case 'g': case 'G': case 1:
            src_load(f,src,1); PREC(6,1,17,0); E(f,"mov ecx,%d",ty==1?1:0); CALLRT(f,"rt_sb_gen"); break;
        case 2: src_load(f,src,1); CALLRT(f,"rt_sb_float"); break;
        case 'c': src_load(f,src,0); to_i32(f,t); CALLRT(f,"rt_sb_cp"); break;
        default: cg_fail(line,"unsupported format type");
    }
    if(grp){ E(f,"pop eax"); E(f,"mov dl,'%c'",sp->comma); E(f,"mov ecx,%d",(ty=='x'||ty=='X'||ty=='o'||ty=='b')?4:3); CALLRT(f,"rt_sb_group"); }
    if(sp->sign && num && ty!='s'){ E(f,"pop eax"); E(f,"mov dl,'%c'",sp->sign); CALLRT(f,"rt_sb_sign"); }
    if(padded){                                                  /* see rt_sb_pad */
        unsigned mode= align=='<' ? 1 : align=='^' ? 2 : 0, fill=(unsigned)sp->fill;
        if(sp->zero && !sp->align && num){ mode|=4; fill='0'; }
        if(align=='=' && num) mode|=4;
        E(f,"pop eax"); if(sp->wslot) E(f,"mov ecx,[ebp%+d]",sp->wslot); else E(f,"mov ecx,%d",sp->width); E(f,"mov edx,0x%x",mode|fill<<8); CALLRT(f,"rt_sb_pad");
    }
}
void gen_format_body(F *f, Expr *e){
    const char *fmt=e->a->tok->text; int pystyle=e->a->tok->i==1;
    if(pystyle) fmt+=strlen(fmt)+1;                          /* an f-string: the spec as written (see lex_fstring) */
    int len=(int)strlen(fmt);
    Expr **args=e->b->kind==EXPR_TUPLE?e->b->items:&e->b; int nargs=e->b->kind==EXPR_TUPLE?e->b->count:1, ai=0;
    Ty *tt=NULL; int tslot=0;
    if(e->b->kind!=EXPR_TUPLE && TY(e->b)->k==TY_TUPLE){          /* "%s=%d" % pair */
        tt=TY(e->b); nargs=tt->nelems; gen_borrow(f,e->b); tslot=frame_slot(f,4); E(f,"mov [ebp%+d],eax",tslot);
    }
    for(int i=0;i<len;){
        if(fmt[i]!='%'){ int j=i; while(j<len && fmt[j]!='%') j++; sb_bytes_lit(f,fmt+i,j-i); i=j; continue; }
        i++;
        if(i<len && fmt[i]=='%'){ E(f,"mov al,'%%'"); CALLRT(f,"rt_sb_char"); i++; continue; }
        Spec sp;
        if(pystyle){                                         /* f-string: "%" + the whole Python spec */
            if(parse_spec(fmt+i,len-i,&sp)!=len-i) cg_fail(e->line,"invalid format spec");
            i=len;
            int nx=0;                                          /* {w} / {p}: their values, now (into slots) */
            if(sp.wslot<0 || sp.pslot<0){ int *slots[2]; int ns=0; if(sp.wslot<0) slots[ns++]=&sp.wslot; if(sp.pslot<0) slots[ns++]=&sp.pslot;
                if(ns!=e->a->count) cg_fail(e->line,"a replacement field there is not supported in compiled code");
                for(int k=0;k<ns;k++){ *slots[k]=frame_slot(f,4); gen(f,e->a->items[k]); to_i32(f,TY(e->a->items[k])); E(f,"mov [ebp%+d],eax",*slots[k]); nx++; } }
            (void)nx;
        } else {
            int j=i;                                            /* printf: flags, width, precision, then the type letter */
            while(j<len && !((fmt[j]>='a'&&fmt[j]<='z')||(fmt[j]>='A'&&fmt[j]<='Z')||fmt[j]=='%')) j++;
            if(j>=len) cg_fail(e->line,"incomplete format");
            parse_spec(fmt+i,j+1-i,&sp); i=j+1;
        }
        if(ai>=nargs) cg_fail(e->line,"not enough arguments for the format string");
        int ix=ai++; ArgSrc src={tslot?NULL:args[ix],tt,tslot,ix};
        emit_field(f,&sp,tslot?ty_find(tt->elems[ix]):TY(args[ix]),&src,0,pystyle,e->line);
    }
    if(ai<nargs) cg_fail(e->line,"not all arguments converted during string formatting");
}
/* "{} and {name:>8}".format(a, name=b): the literal is parsed here */
int gen_str_format(F *f, Expr *call){
    Expr *lit=call->a->a; const char *fmt=lit->tok->text; int len=(int)strlen(fmt), auto_ix=0;
    rt("rt_sb_need"); E(f,"push dword [rt_sb_len]");
    for(int i=0;i<len;){
        if(fmt[i]=='{' && i+1<len && fmt[i+1]=='{'){ sb_bytes_lit(f,"{",1); i+=2; continue; }
        if(fmt[i]=='}' && i+1<len && fmt[i+1]=='}'){ sb_bytes_lit(f,"}",1); i+=2; continue; }
        if(fmt[i]!='{'){ int j=i; while(j<len && fmt[j]!='{' && fmt[j]!='}') j++; if(j==i) j++; sb_bytes_lit(f,fmt+i,j-i); i=j; continue; }
        int j=i+1; while(j<len && fmt[j]!='}') j++;
        if(j>=len) cg_fail(call->line,"unmatched '{' in the format string");
        const char *fld=fmt+i+1; int flen=j-i-1; i=j+1;
        int nl=0; while(nl<flen && fld[nl]!='!' && fld[nl]!=':') nl++;
        int repr=0; const char *spec=""; int slen=0;
        if(nl<flen && fld[nl]=='!'){ repr=fld[nl+1]=='r'; nl+=0; }
        { const char *c=memchr(fld,':',(size_t)flen); if(c){ spec=c+1; slen=flen-(int)(c+1-fld); } }
        Expr *arg=NULL;
        if(nl==0) arg = auto_ix<call->count && !call->items[auto_ix]->akind ? call->items[auto_ix] : NULL, auto_ix++;
        else if(fld[0]>='0'&&fld[0]<='9'){ int k=atoi(fld); if(k<call->count && !call->items[k]->akind) arg=call->items[k]; }
        else for(int k=0;k<call->count;k++) if(call->items[k]->akind==3 && (int)strlen(call->items[k]->kw)==nl && !strncmp(call->items[k]->kw,fld,(size_t)nl)) arg=call->items[k];
        if(!arg) cg_fail(call->line,"format() field without a matching argument");
        Spec sp; if(parse_spec(spec,slen,&sp)!=slen) cg_fail(call->line,"invalid format spec");
        ArgSrc src={arg,NULL,0,0};
        emit_field(f,&sp,TY(arg),&src,repr,1,call->line);
    }
    E(f,"pop eax"); CALLRT(f,"rt_sb_take");
    return 1;
}
int gen_format(F *f, Expr *e){
    rt("rt_sb_need"); E(f,"push dword [rt_sb_len]");
    gen_format_body(f,e);
    E(f,"pop eax"); CALLRT(f,"rt_sb_take");
    return 1;
}

/* a closure of g made here: its captured values (or cells) copied from this function -> eax (owned) */
