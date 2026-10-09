/* ========================= Interpreter: formatting =========================
   format() / f-string specs (Python's mini-language: fill, align, sign, #,
   0, width, grouping, precision, type), the % operator of str and
   str.format(), and reading numbers from text (int(), float()). */

#include "interp.h"
#include <math.h>

void mp_repr_into(SBuf *b, Value v);

/* ---------------------------------------------------------------- the spec */
typedef struct { uint32_t fill; int align, sign, alt, zero, width, group, prec, type, z; } Spec;
static int parse_spec(const char *s, Spec *sp){
    memset(sp,0,sizeof *sp); sp->fill=' '; sp->prec=-1;
    int64_t n=(int64_t)strlen(s), p=0;
    if(n==0) return 1;
    /* [[fill]align] */
    int64_t q=0; uint32_t c0=(uint32_t)mp_utf8_decode(s,n,&q);
    if(q<n && strchr("<>=^",s[q])){ sp->fill=c0; sp->align=s[q]; p=q+1; }
    else if(strchr("<>=^",s[0])){ sp->align=s[0]; p=1; }
    if(p<n && strchr("+- ",s[p])) sp->sign=s[p++];
    if(p<n && s[p]=='z'){ sp->z=1; p++; }
    if(p<n && s[p]=='#'){ sp->alt=1; p++; }
    if(p<n && s[p]=='0'){ sp->zero=1; p++; }
    while(p<n && s[p]>='0' && s[p]<='9') sp->width=sp->width*10+(s[p++]-'0');
    if(p<n && (s[p]==','||s[p]=='_')) sp->group=s[p++];
    if(p<n && s[p]=='.'){ p++; if(p>=n || s[p]<'0' || s[p]>'9') return 0; sp->prec=0; while(p<n && s[p]>='0' && s[p]<='9') sp->prec=sp->prec*10+(s[p++]-'0'); }
    if(p<n) sp->type=s[p++];
    if(p!=n) return 0;
    if(sp->zero && !sp->align){ sp->fill='0'; sp->align='='; }
    return 1;
}
static void put_cp(SBuf *b, uint32_t c, int64_t n){ char t[4]; int m=mp_utf8_encode(t,c); for(int64_t i=0;i<n;i++) sb_put(b,t,m); }
static int64_t cplen(const char *s, int64_t n){ int64_t c=0, p=0; while(p<n){ mp_utf8_decode(s,n,&p); c++; } return c; }
/* pad body (sign/prefix part `head`, digits `body`) to the spec */
static void pad(SBuf *out, Spec *sp, const char *head, const char *body, int64_t blen, int numeric){
    int64_t hl=(int64_t)strlen(head), len=cplen(head,hl)+cplen(body,blen);
    int align=sp->align ? sp->align : numeric ? '>' : '<';
    int64_t padn= sp->width>len ? sp->width-len : 0;
    if(align=='='){ sb_puts(out,head); put_cp(out,sp->fill,padn); sb_put(out,body,blen); return; }
    int64_t left= align=='>' ? padn : align=='^' ? padn/2 : 0, right=padn-left;
    put_cp(out,sp->fill,left); sb_puts(out,head); sb_put(out,body,blen); put_cp(out,sp->fill,right);
}
/* digits with a thousands separator every 3 (or 4 for b/o/x) */
static void group_digits(SBuf *o, const char *d, int64_t n, int sep, int every){
    for(int64_t i=0;i<n;i++){ if(i && (n-i)%every==0) sb_putc(o,(char)sep); sb_putc(o,d[i]); }
}
static void fmt_int(SBuf *out, int64_t v, Spec *sp){
    int t=sp->type?sp->type:'d';
    if(t=='c'){ if(sp->sign) mp_raise_t(E_ValueError,"Sign not allowed with integer format specifier 'c'");
        if(v<0 || v>=0x110000) mp_raise_t(E_OverflowError,"%%c arg not in range(0x110000)");
        char u[8]; int m=mp_utf8_encode(u,(uint32_t)v); u[m]=0; pad(out,sp,"",u,m,0); return; }
    uint64_t a= v<0 ? (uint64_t)(-(v+1))+1 : (uint64_t)v;
    char digits[80]; int nd=0;
    int base= t=='b'?2 : t=='o'?8 : (t=='x'||t=='X')?16 : 10;
    const char *dig= t=='X' ? "0123456789ABCDEF" : "0123456789abcdef";
    do{ digits[nd++]=dig[a%(uint64_t)base]; a/=(uint64_t)base; }while(a);
    char rev[80]; for(int i=0;i<nd;i++) rev[i]=digits[nd-1-i]; rev[nd]=0;
    char head[8]; int hl=0;
    if(v<0) head[hl++]='-'; else if(sp->sign=='+') head[hl++]='+'; else if(sp->sign==' ') head[hl++]=' ';
    if(sp->alt && base!=10){ head[hl++]='0'; head[hl++]= t=='b'?'b':t=='o'?'o':t=='X'?'X':'x'; }
    head[hl]=0;
    SBuf body={0};
    if(sp->group){
        int every= base==10 ? 3 : 4;
        if(sp->group==',' && base!=10) mp_raise_t(E_ValueError,"Cannot specify ',' with '%c'.",t);
        if(sp->align=='=' && sp->fill=='0' && sp->width>0){
            /* zero padding takes part in the grouping */
            int64_t want=sp->width-hl; int64_t nds=nd;
            while(nds+(nds-1)/every<want) nds++;
            char *z=(char*)xmalloc((size_t)nds+1); memset(z,'0',(size_t)(nds-nd)); memcpy(z+nds-nd,rev,(size_t)nd); z[nds]=0;
            group_digits(&body,z,nds,sp->group,every); free(z);
        } else group_digits(&body,rev,nd,sp->group,every);
    } else sb_puts(&body,rev);
    pad(out,sp,head,body.s?body.s:"",body.n,1);
    free(body.s);
}
/* the digits of x as %e / %f / %g give them, without the sign */
static void fmt_float(SBuf *out, double x, Spec *sp){
    int t=sp->type;
    char head[4]; int hl=0;
    int neg=signbit(x) && !(sp->z && x==0);
    if(isnan(x)) neg=0;
    double ax=fabs(x);
    if(neg) head[hl++]='-'; else if(sp->sign=='+') head[hl++]='+'; else if(sp->sign==' ') head[hl++]=' ';
    head[hl]=0;
    char buf[512];
    if(isinf(ax) || isnan(ax)){
        const char *s= isnan(ax) ? "nan" : "inf";
        if(t=='E'||t=='F'||t=='G') s= isnan(ax) ? "NAN" : "INF";
        if(t=='%'){ snprintf(buf,sizeof buf,"%s%%",s); s=buf; }
        Spec sp2=*sp; if(sp2.fill=='0' && sp2.align=='='){ sp2.fill=' '; sp2.align=0; }
        pad(out,&sp2,head,s,(int64_t)strlen(s),1); return;
    }
    int prec=sp->prec;
    if(!t){
        if(prec<0){ mp_float_repr(buf,sizeof buf,ax); }
        else {
            /* like 'g', but with at least one digit after the point when fixed-point */
            int p= prec==0 ? 1 : prec;
            snprintf(buf,sizeof buf,"%.*e",p-1,ax);
            int e=atoi(strchr(buf,'e')+1);
            if(e>=-4 && e<p){
                snprintf(buf,sizeof buf,"%.*f",p-1-e,ax);
                if(!sp->alt && strchr(buf,'.')){ size_t l=strlen(buf); while(buf[l-1]=='0') buf[--l]=0; if(buf[l-1]=='.') { buf[l]='0'; buf[l+1]=0; } }
                else if(!strchr(buf,'.')) strcat(buf,".0");
            } else {
                snprintf(buf,sizeof buf,"%.*e",p-1,ax);
                if(!sp->alt){ char *ep=strchr(buf,'e'), tail[32]; snprintf(tail,sizeof tail,"%s",ep); *ep=0;
                    if(strchr(buf,'.')){ size_t l=strlen(buf); while(buf[l-1]=='0') buf[--l]=0; if(buf[l-1]=='.') buf[--l]=0; }
                    strcat(buf,tail); }
            }
        }
    } else if(t=='e'||t=='E'){ snprintf(buf,sizeof buf,sp->alt?"%#.*e":"%.*e",prec<0?6:prec,ax); }
    else if(t=='f'||t=='F'){ snprintf(buf,sizeof buf,sp->alt?"%#.*f":"%.*f",prec<0?6:prec,ax); }
    else if(t=='g'||t=='G'||t=='n'){ snprintf(buf,sizeof buf,sp->alt?"%#.*g":"%.*g",prec<0?6:prec==0?1:prec,ax); }
    else if(t=='%'){ snprintf(buf,sizeof buf,sp->alt?"%#.*f":"%.*f",prec<0?6:prec,ax*100); strcat(buf,"%"); }
    else mp_raise_t(E_ValueError,"Unknown format code '%c' for object of type 'float'",t);
    if(t=='E'||t=='G'||t=='F') for(char *q=buf;*q;q++) *q=(char)toupper((unsigned char)*q);
    /* grouping of the integer part */
    if(sp->group){
        char *dot=buf; while(*dot && *dot>='0' && *dot<='9') dot++;
        int64_t nint=dot-buf;
        SBuf g={0};
        if(sp->align=='=' && sp->fill=='0' && sp->width>0){
            int64_t rest=(int64_t)strlen(dot), want=sp->width-hl-rest, nds=nint;
            while(nds+(nds-1)/3<want) nds++;
            char *z=(char*)xmalloc((size_t)nds+1); memset(z,'0',(size_t)(nds-nint)); memcpy(z+nds-nint,buf,(size_t)nint); z[nds]=0;
            group_digits(&g,z,nds,sp->group,3); free(z);
        } else group_digits(&g,buf,nint,sp->group,3);
        sb_puts(&g,dot);
        pad(out,sp,head,g.s,g.n,1); free(g.s); return;
    }
    pad(out,sp,head,buf,(int64_t)strlen(buf),1);
}
Value mp_format(Value v, Value specv){
    const char *spec=mp_cstr(specv);
    Type *t=TYPE(v);
    if(t->flags&TF_DUNDERS){
        Value m=mp_type_lookup_s(t,"__format__");
        if(m.k!=V_UNDEF && !IS(m,T_native)){ Value r=mp_call2(m,v,specv); if(!IS_STR(r)) mp_raise_t(E_TypeError,"__format__ must return a str, not %s",mp_type_name(r)); return r; }
        if(mp_is_subtype(t,T_str) || mp_is_subtype(t,T_int) || mp_is_subtype(t,T_float)){}
        else {
            if(spec[0]) mp_raise_t(E_TypeError,"unsupported format string passed to %s.__format__",t->name->s);
            return mp_tostr(v);
        }
    }
    Spec sp;
    if(!parse_spec(spec,&sp)) mp_raise_t(E_ValueError,"Invalid format specifier '%s' for object of type '%s'",spec,t->name->s);
    SBuf out={0};
    if(v.k==V_INT || (v.k==V_BOOL && spec[0])){
        if(sp.type && strchr("eEfFgG%",sp.type)) fmt_float(&out,(double)v.u.i,&sp);
        else if(sp.type && !strchr("bcdoxXn",sp.type)) mp_raise_t(E_ValueError,"Unknown format code '%c' for object of type 'int'",sp.type);
        else { if(sp.prec>=0) mp_raise_t(E_ValueError,"Precision not allowed in integer format specifier"); fmt_int(&out,v.u.i,&sp); }
        return sb_value(&out);
    }
    if(v.k==V_FLOAT){
        if(sp.type && strchr("bcdoxX",sp.type)) mp_raise_t(E_ValueError,"Unknown format code '%c' for object of type 'float'",sp.type);
        fmt_float(&out,v.u.f,&sp); return sb_value(&out);
    }
    if(IS_STR(v) || v.k==V_BOOL || IS_NONE(v) || !spec[0]){
        Value s= IS_STR(v) ? v : mp_tostr(v);
        if(!spec[0]) return s;
        if(!IS_STR(v)) mp_raise_t(E_TypeError,"unsupported format string passed to %s.__format__",t->name->s);
        if(sp.type && sp.type!='s') mp_raise_t(E_ValueError,"Unknown format code '%c' for object of type 'str'",sp.type);
        if(sp.sign) mp_raise_t(E_ValueError,"Sign not allowed in string format specifier");
        if(sp.align=='=') mp_raise_t(E_ValueError,"'=' alignment not allowed in string format specifier");
        StrObj *x=AS_STR(s); int64_t n=x->len;
        if(sp.prec>=0 && sp.prec<x->cplen) n=mp_str_byteoff(x,sp.prec);
        pad(&out,&sp,"",x->s,n,0);
        return sb_value(&out);
    }
    if(IS(v,T_complex)){
        if(spec[0]==0) return mp_repr(v);
        ComplexObj *c=(ComplexObj*)v.u.o; Spec s2=sp; s2.width=0; s2.align=0;
        SBuf b={0}; fmt_float(&b,c->re,&s2); s2.sign='+'; fmt_float(&b,c->im,&s2); sb_putc(&b,'j');
        Value body=sb_value(&b); Spec s3=sp; s3.type=0;
        pad(&out,&s3,"",mp_cstr(body),AS_STR(body)->len,1); return sb_value(&out);
    }
    mp_raise_t(E_TypeError,"unsupported format string passed to %s.__format__",t->name->s);
}

/* ---------------------------------------------------------------- str % args */
Value mp_percent_format(Value fmtv, Value args){
    StrObj *fmt=AS_STR(fmtv);
    SBuf out={0};
    int64_t ai=0; int argn; Value *argv; Value one[1];
    int is_map= args.k==V_OBJ && !IS(args,T_tuple) && !IS_STR(args) && (TYPE(args)->layout==LY_DICT || ((TYPE(args)->flags&TF_DUNDERS) && mp_type_lookup_s(TYPE(args),"__getitem__").k!=V_UNDEF));
    if(IS(args,T_tuple)){ argv=AS_TUPLE(args)->items; argn=(int)AS_TUPLE(args)->len; }
    else { one[0]=args; argv=one; argn=1; }
    int used_map=0;
    const char *s=fmt->s; int64_t n=fmt->len;
    for(int64_t i=0;i<n;i++){
        if(s[i]!='%'){ sb_putc(&out,s[i]); continue; }
        if(++i>=n) mp_raise_t(E_ValueError,"incomplete format");
        Value arg=v_undef();
        if(s[i]=='('){
            int64_t st=++i, depth=1; while(i<n && depth){ if(s[i]=='(') depth++; else if(s[i]==')') depth--; if(depth) i++; }
            if(i>=n) mp_raise_t(E_ValueError,"incomplete format key");
            if(!is_map) mp_raise_t(E_TypeError,"format requires a mapping");
            arg=mp_getitem(args,mp_strn(s+st,i-st)); i++; used_map=1;
        }
        Spec sp; memset(&sp,0,sizeof sp); sp.fill=' '; sp.prec=-1;
        int left=0;
        for(;i<n && strchr("-+ #0",s[i]);i++){ if(s[i]=='-') left=1; else if(s[i]=='+') sp.sign='+'; else if(s[i]==' '){ if(!sp.sign) sp.sign=' '; } else if(s[i]=='#') sp.alt=1; else sp.zero=1; }
        if(i<n && s[i]=='*'){ if(ai>=argn) mp_raise_t(E_TypeError,"not enough arguments for format string"); sp.width=(int)mp_index(argv[ai++],"*"); if(sp.width<0){ left=1; sp.width=-sp.width; } i++; }
        else while(i<n && s[i]>='0' && s[i]<='9') sp.width=sp.width*10+(s[i++]-'0');
        if(i<n && s[i]=='.'){ i++; sp.prec=0;
            if(i<n && s[i]=='*'){ if(ai>=argn) mp_raise_t(E_TypeError,"not enough arguments for format string"); sp.prec=(int)mp_index(argv[ai++],"*"); i++; }
            else while(i<n && s[i]>='0' && s[i]<='9') sp.prec=sp.prec*10+(s[i++]-'0'); }
        while(i<n && strchr("hlL",s[i])) i++;
        if(i>=n) mp_raise_t(E_ValueError,"incomplete format");
        int c=s[i];
        if(c=='%'){ sb_putc(&out,'%'); continue; }
        if(arg.k==V_UNDEF){
            if(is_map && !IS(args,T_tuple)){ arg=args; used_map=1; }
            else { if(ai>=argn) mp_raise_t(E_TypeError,"not enough arguments for format string"); arg=argv[ai++]; }
        }
        sp.align= left ? '<' : (sp.zero ? '=' : '>');
        if(sp.zero && !left) sp.fill='0';
        switch(c){
            case 'd': case 'i': case 'u':{
                if(arg.k==V_FLOAT) arg=v_int((int64_t)arg.u.f);
                if(!IS_INTLIKE(arg)){ if(TYPE(arg)->flags&TF_DUNDERS) arg=v_int(mp_index(arg,"%d")); else mp_raise_t(E_TypeError,"%%%c format: a real number is required, not %s",c,mp_type_name(arg)); }
                Spec s2=sp; s2.type='d';
                if(sp.prec>0){ char d[64]; snprintf(d,sizeof d,"%0*lld",sp.prec,(long long)(arg.u.i<0?-arg.u.i:arg.u.i)); char h[4]={0}; if(arg.u.i<0) h[0]='-'; else if(sp.sign) h[0]=(char)sp.sign; pad(&out,&s2,h,d,(int64_t)strlen(d),1); }
                else fmt_int(&out,arg.u.i,&s2);
                break; }
            case 'x': case 'X': case 'o':{
                if(!IS_INTLIKE(arg)) mp_raise_t(E_TypeError,"%%%c format: an integer is required, not %s",c,mp_type_name(arg));
                Spec s2=sp; s2.type=c; fmt_int(&out,arg.u.i,&s2); break; }
            case 'e': case 'E': case 'f': case 'F': case 'g': case 'G':{
                double x;
                if(arg.k==V_FLOAT) x=arg.u.f; else if(IS_INTLIKE(arg)) x=(double)arg.u.i;
                else mp_raise_t(E_TypeError,"must be real number, not %s",mp_type_name(arg));
                Spec s2=sp; s2.type=c; if(s2.prec<0) s2.prec=6;
                fmt_float(&out,x,&s2); break; }
            case 'c':{
                Value ch;
                if(IS_INTLIKE(arg)){ if(arg.u.i<0 || arg.u.i>=0x110000) mp_raise_t(E_OverflowError,"%%c arg not in range(0x110000)");
                    char u[8]; int m=mp_utf8_encode(u,(uint32_t)arg.u.i); ch=mp_strn(u,m); }
                else if(IS_STR(arg) && AS_STR(arg)->cplen==1) ch=arg;
                else mp_raise_t(E_TypeError,"%%c requires an int or a unicode character, not %s",mp_type_name(arg));
                Spec s2=sp; s2.fill=' '; if(s2.align=='=') s2.align='>'; pad(&out,&s2,"",mp_cstr(ch),AS_STR(ch)->len,0); break; }
            case 's': case 'r': case 'a':{
                Value str= c=='s' ? mp_tostr(arg) : mp_repr(arg);
                StrObj *x=AS_STR(str); int64_t len=x->len;
                if(sp.prec>=0 && sp.prec<x->cplen) len=mp_str_byteoff(x,sp.prec);
                Spec s2=sp; s2.fill=' '; if(s2.align=='=') s2.align='>';
                pad(&out,&s2,"",x->s,len,0); break; }
            default: mp_raise_t(E_ValueError,"unsupported format character '%c' (0x%x) at index %lld",c,c,(long long)i);
        }
    }
    if(!used_map && ai<argn) mp_raise_t(E_TypeError,"not all arguments converted during string formatting");
    return sb_value(&out);
}

/* ---------------------------------------------------------------- str.format */
static Value field_value(const char *s, int64_t n, int *auto_ix, int argc, Value *argv, TupleObj *kw, Value *kwv){
    int64_t i=0;
    while(i<n && s[i]!='.' && s[i]!='[') i++;
    Value v;
    if(i==0){
        if(*auto_ix<0) mp_raise_t(E_ValueError,"cannot switch from manual field specification to automatic field numbering");
        int k=(*auto_ix)++;
        if(k>=argc) mp_raise_t(E_IndexError,"Replacement index %d out of range for positional args tuple",k);
        v=argv[k];
    } else if(s[0]>='0' && s[0]<='9'){
        if(*auto_ix>0) mp_raise_t(E_ValueError,"cannot switch from automatic field numbering to manual field specification");
        *auto_ix=-1;
        int k=atoi(s);
        if(k>=argc) mp_raise_t(E_IndexError,"Replacement index %d out of range for positional args tuple",k);
        v=argv[k];
    } else {
        Value name=mp_strn(s,i); int found=0;
        if(kw) for(int64_t j=0;j<kw->len;j++) if(!strcmp(mp_cstr(kw->items[j]),mp_cstr(name))){ v=kwv[j]; found=1; }
        if(!found) mp_raise(mp_exc_args(E_KeyError,mp_tuple(1,&name)));
    }
    while(i<n){
        if(s[i]=='.'){ int64_t st=++i; while(i<n && s[i]!='.' && s[i]!='[') i++; v=mp_getattr(v,mp_strn(s+st,i-st)); }
        else { int64_t st=++i; while(i<n && s[i]!=']') i++;
            Value key; int digits=1; for(int64_t j=st;j<i;j++) if(s[j]<'0'||s[j]>'9') digits=0;
            key= digits && i>st ? v_int(atoll(s+st)) : mp_strn(s+st,i-st);
            v=mp_getitem(v,key); i++; }
    }
    return v;
}
static Value format_with(const char *s, int64_t n, int *auto_ix, int argc, Value *argv, TupleObj *kw, Value *kwv, int depth){
    if(depth>2) mp_raise_t(E_ValueError,"Max string recursion exceeded");
    SBuf out={0};
    for(int64_t i=0;i<n;i++){
        char c=s[i];
        if(c=='}'){ if(i+1<n && s[i+1]=='}'){ sb_putc(&out,'}'); i++; continue; } mp_raise_t(E_ValueError,"Single '}' encountered in format string"); }
        if(c!='{'){ sb_putc(&out,c); continue; }
        if(i+1<n && s[i+1]=='{'){ sb_putc(&out,'{'); i++; continue; }
        int64_t st=++i, depthb=1;
        while(i<n && depthb){ if(s[i]=='{') depthb++; else if(s[i]=='}') depthb--; if(depthb) i++; }
        if(i>=n) mp_raise_t(E_ValueError,"expected '}' before end of string");
        /* field [!conv] [:spec] */
        int64_t fe=st; while(fe<i && s[fe]!='!' && s[fe]!=':'){ if(s[fe]=='[') while(fe<i && s[fe]!=']') fe++; fe++; }
        if(fe>i) fe=i;
        Value v=field_value(s+st,fe-st,auto_ix,argc,argv,kw,kwv);
        int64_t p=fe;
        if(p<i && s[p]=='!'){ p++; char conv=s[p++];
            if(conv=='r') v=mp_repr(v); else if(conv=='s') v=mp_tostr(v); else if(conv=='a') v=mp_repr(v);
            else mp_raise_t(E_ValueError,"Unknown conversion specifier %c",conv); }
        Value spec=mp_str("");
        if(p<i && s[p]==':'){ p++; spec=format_with(s+p,i-p,auto_ix,argc,argv,kw,kwv,depth+1); }
        Value r=mp_format(v,spec);
        sb_put(&out,AS_STR(r)->s,AS_STR(r)->len);
    }
    return sb_value(&out);
}
Value mp_str_format(Value fmt, int argc, Value *argv, TupleObj *kw){
    int auto_ix=0; int np=argc-(kw?(int)kw->len:0);
    return format_with(AS_STR(fmt)->s,AS_STR(fmt)->len,&auto_ix,np,argv,kw,argv+np,0);
}

/* ---------------------------------------------------------------- numbers from text */
/* int(s, base): Python's syntax (sign, base prefix, underscores between digits, spaces around) */
int mp_parse_int(const char *s, int64_t n, int base, int64_t *out){
    int64_t i=0, e=n;
    while(i<e && isspace((unsigned char)s[i])) i++;
    while(e>i && isspace((unsigned char)s[e-1])) e--;
    int neg=0;
    if(i<e && (s[i]=='+'||s[i]=='-')){ neg= s[i]=='-'; i++; }
    if(base==0){
        if(i+1<e && s[i]=='0' && strchr("xX",s[i+1])){ base=16; i+=2; }
        else if(i+1<e && s[i]=='0' && strchr("oO",s[i+1])){ base=8; i+=2; }
        else if(i+1<e && s[i]=='0' && strchr("bB",s[i+1])){ base=2; i+=2; }
        else { base=10; for(int64_t j=i;j+1<e;j++){ if(s[j]!='0' && s[j]!='_') break; if(j>i && s[j+1]!='0' && s[j+1]!='_') return 0; } }
        if(i<e && s[i]=='_') i++;
    } else if((base==16 && i+1<e && s[i]=='0' && strchr("xX",s[i+1])) || (base==8 && i+1<e && s[i]=='0' && strchr("oO",s[i+1])) || (base==2 && i+1<e && s[i]=='0' && strchr("bB",s[i+1]))){ i+=2; if(i<e && s[i]=='_') i++; }
    if(i>=e) return 0;
    uint64_t v=0; int prev_us=1;
    for(;i<e;i++){
        char c=s[i];
        if(c=='_'){ if(prev_us) return 0; prev_us=1; continue; }
        int d= c>='0'&&c<='9' ? c-'0' : c>='a'&&c<='z' ? c-'a'+10 : c>='A'&&c<='Z' ? c-'A'+10 : 99;
        if(d>=base) return 0;
        if(v>(UINT64_MAX-(uint64_t)d)/(uint64_t)base) return -1;
        v=v*(uint64_t)base+(uint64_t)d; prev_us=0;
    }
    if(prev_us) return 0;
    if(neg){ if(v>(uint64_t)INT64_MAX+1) return -1; *out=(int64_t)(0-v); }
    else { if(v>(uint64_t)INT64_MAX) return -1; *out=(int64_t)v; }
    return 1;
}
static int ci_eq(const char *a, const char *b, int n){ for(int i=0;i<n;i++) if(tolower((unsigned char)a[i])!=b[i]) return 0; return 1; }
int mp_parse_float(const char *s, int64_t n, double *out){
    int64_t i=0, e=n;
    while(i<e && isspace((unsigned char)s[i])) i++;
    while(e>i && isspace((unsigned char)s[e-1])) e--;
    if(i>=e) return 0;
    char buf[512]; int64_t k=0; int sign=1;
    if(s[i]=='+'||s[i]=='-'){ if(s[i]=='-') sign=-1; i++; }
    int64_t len=e-i;
    if(len==3 && ci_eq(s+i,"inf",3)){ *out=sign*INFINITY; return 1; }
    if(len==8 && ci_eq(s+i,"infinity",8)){ *out=sign*INFINITY; return 1; }
    if(len==3 && ci_eq(s+i,"nan",3)){ *out=sign*NAN; return 1; }
    int prev_digit=0, seen_digit=0;
    for(;i<e && k<500;i++){
        char c=s[i];
        if(c=='_'){ if(!prev_digit || i+1>=e || !isdigit((unsigned char)s[i+1])) return 0; continue; }
        if(!(isdigit((unsigned char)c)||c=='.'||c=='e'||c=='E'||c=='+'||c=='-')) return 0;
        prev_digit=isdigit((unsigned char)c)!=0; if(prev_digit) seen_digit=1;
        buf[k++]=c;
    }
    buf[k]=0;
    if(!seen_digit) return 0;
    char *end; double v=strtod(buf,&end);
    if(*end) return 0;
    *out=sign*v;
    return 1;
}
