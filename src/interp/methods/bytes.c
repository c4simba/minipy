/* ========================= Interpreter, methods: bytes =========================
   The methods of bytes (and what bytearray shares with them). */

#include "methods.h"

static BytesObj *B(Value v){ return AS_BYTES(v); }
static Value bytes_of(const void *s, int64_t n){ return mp_bytes(s,n); }
static Value m_decode(int argc, Value *argv, TupleObj *kw){
    int np=npos_of(argc,kw);
    Value enc= np>1 ? argv[1] : kwarg(argc,argv,kw,"encoding",v_undef());
    Value errs= np>2 ? argv[2] : kwarg(argc,argv,kw,"errors",v_undef());
    if(enc.k!=V_UNDEF && !IS_STR(enc)) mp_raise_t(E_TypeError,"decode() argument 'encoding' must be str, not %s",mp_type_name(enc));
    if(enc.k!=V_UNDEF && !codec_known(enc)) return py_codec("decode",SELF,enc,errs);
    return mp_decode(B(SELF)->s,B(SELF)->len,enc.k==V_UNDEF?0:mp_codec(enc),errs);
}
/* bytes.hex([sep[, bytes_per_sep]]): sep (one ASCII character) between groups of bytes_per_sep
   bytes, counted from the right (from the left when negative) */
static Value m_hex(int argc, Value *argv, TupleObj *kw){
    BytesObj *b=B(SELF); int np=npos_of(argc,kw);
    Value sepv= np>1 ? argv[1] : kwarg(argc,argv,kw,"sep",v_none());
    int64_t per= np>2 ? mp_index(argv[2],"bytes_per_sep") : mp_index(kwarg(argc,argv,kw,"bytes_per_sep",v_int(1)),"bytes_per_sep");
    int sep=-1;
    if(sepv.k!=V_NONE){ const unsigned char *t; int64_t n;
        if(IS_STR(sepv)){ t=(const unsigned char*)S(sepv)->s; n=S(sepv)->len; }
        else if(IS_BYTES(sepv)){ t=B(sepv)->s; n=B(sepv)->len; }
        else mp_raise_t(E_TypeError,"object of type '%s' has no len()",mp_type_name(sepv));
        for(int64_t i=0;i<n;i++) if(t[i]>=0x80) mp_raise_t(E_ValueError,"sep must be ASCII.");
        if(n!=1) mp_raise_t(E_ValueError,"sep must be length 1.");
        sep=t[0]; }
    int64_t k= per<0 ? -per : per;
    SBuf o={0}; sb_puts(&o,"");
    for(int64_t i=0;i<b->len;i++){
        if(sep>=0 && k && i && (per>0 ? (b->len-i)%k==0 : i%k==0)) sb_putc(&o,(char)sep);
        sb_printf(&o,"%02x",b->s[i]); }
    return sb_value(&o);
}
static Value m_fromhex(int argc, Value *argv, TupleObj *kw){
    (void)kw; Value x=argv[argc-1];
    if(!IS_STR(x)) mp_raise_t(E_TypeError,"fromhex() argument must be str, not %s",mp_type_name(x));
    StrObj *s=S(x); SBuf o={0}; int64_t i=0;
    while(i<s->len){
        unsigned char c=(unsigned char)s->s[i];
        if(c==' '||(c>=9&&c<=13)){ i++; continue; }
        int hi= isxdigit(c) ? (c<='9'?c-'0':(tolower(c)-'a'+10)) : -1;
        if(hi<0){ free(o.s); mp_raise_t(E_ValueError,"non-hexadecimal number found in fromhex() arg at position %lld",(long long)cp_index(s,i)); }
        if(i+1>=s->len){ free(o.s); mp_raise_t(E_ValueError,"fromhex() arg must contain an even number of hexadecimal digits"); }
        unsigned char d=(unsigned char)s->s[i+1]; int lo= isxdigit(d) ? (d<='9'?d-'0':(tolower(d)-'a'+10)) : -1;
        if(lo<0){ free(o.s); if(d==' '||(d>=9&&d<=13)||!d) mp_raise_t(E_ValueError,"fromhex() arg must contain an even number of hexadecimal digits"); mp_raise_t(E_ValueError,"non-hexadecimal number found in fromhex() arg at position %lld",(long long)cp_index(s,i+1)); }
        sb_putc(&o,(char)(hi*16+lo)); i+=2;
    }
    Value r=mp_bytes(o.s?o.s:"",o.n); free(o.s); return r;
}
/* a bytes argument (or one byte given as an int, when byte_ok) */
void bytes_arg(Value v, int byte_ok, const char **s, int64_t *n, char *one){
    if(IS_BYTES(v)){ *s=(const char*)B(v)->s; *n=B(v)->len; return; }
    if(IS(v,T_buffer)){ *s=(const char*)((BufferObj*)v.u.o)->data; *n=((BufferObj*)v.u.o)->len; return; }
    if(IS_BA(v)){ *s=(const char*)((ByteArrayObj*)v.u.o)->data; *n=((ByteArrayObj*)v.u.o)->len; return; }
    if(byte_ok && IS_INTLIKE(v)){ if(v.u.i<0||v.u.i>255) mp_raise_t(E_ValueError,"byte must be in range(0, 256)"); *one=(char)v.u.i; *s=one; *n=1; return; }
    if(byte_ok) mp_raise_t(E_TypeError,"argument should be integer or bytes-like object, not '%s'",mp_type_name(v));
    mp_raise_t(E_TypeError,"a bytes-like object is required, not '%s'",mp_type_name(v));
}
/* start / end arguments -> [*a, *e) (a > length: nothing fits) */
static void brange(BytesObj *b, int argc, Value *argv, int first, int64_t *a, int64_t *e){
    *a=0; *e=b->len;
    if(argc>first && !IS_NONE(argv[first])) *a=mp_index(argv[first],"slice");
    if(argc>first+1 && !IS_NONE(argv[first+1])) *e=mp_index(argv[first+1],"slice");
    if(*a<0){ *a+=b->len; if(*a<0) *a=0; } if(*a>b->len) *a=b->len+1;
    if(*e<0){ *e+=b->len; if(*e<0) *e=0; } if(*e>b->len) *e=b->len;
}
static Value bfind(int argc, Value *argv, int rev, int raise, const char *name){
    nargs(name,argc,1,3);
    BytesObj *b=B(SELF); const char *s; int64_t n; char one; bytes_arg(argv[1],1,&s,&n,&one);
    int64_t a, e; brange(b,argc,argv,2,&a,&e);
    int64_t f= a>b->len ? -1 : find_bytes((const char*)b->s,a,e,s,n,rev);
    if(f<0 && raise) mp_raise_t(E_ValueError,"subsection not found");
    return v_int(f);
}
static Value m_bfind(int argc, Value *argv, TupleObj *kw){ (void)kw; return bfind(argc,argv,0,0,"find"); }
static Value m_brfind(int argc, Value *argv, TupleObj *kw){ (void)kw; return bfind(argc,argv,1,0,"rfind"); }
static Value m_bindex(int argc, Value *argv, TupleObj *kw){ (void)kw; return bfind(argc,argv,0,1,"index"); }
static Value m_brindex(int argc, Value *argv, TupleObj *kw){ (void)kw; return bfind(argc,argv,1,1,"rindex"); }
static Value m_bcount(int argc, Value *argv, TupleObj *kw){
    (void)kw; nargs("count",argc,1,3);
    BytesObj *b=B(SELF); const char *s; int64_t n; char one; bytes_arg(argv[1],1,&s,&n,&one);
    int64_t a, e; brange(b,argc,argv,2,&a,&e);
    if(a>b->len || a>e) return v_int(0);
    if(!n) return v_int(e-a+1);
    int64_t c=0; while(1){ int64_t f=find_bytes((const char*)b->s,a,e,s,n,0); if(f<0) break; c++; a=f+n; }
    return v_int(c);
}
static Value bstarts(int argc, Value *argv, int ends, const char *name){
    nargs(name,argc,1,3);
    BytesObj *b=B(SELF); int64_t a, e; brange(b,argc,argv,2,&a,&e);
    Value pre=argv[1];
    if(IS_TUPLE(pre)){
        for(int64_t i=0;i<AS_TUPLE(pre)->len;i++){ Value x[4]={SELF,AS_TUPLE(pre)->items[i],argc>2?argv[2]:v_none(),argc>3?argv[3]:v_none()}; if(mp_truth(bstarts(argc<2?2:argc,x,ends,name))) return v_bool(1); }
        return v_bool(0);
    }
    if(!IS_BYTES(pre) && !IS(pre,T_buffer) && !IS_BA(pre)) mp_raise_t(E_TypeError,"%s first arg must be bytes or a tuple of bytes, not %s",name,mp_type_name(pre));
    const char *s; int64_t n; char one; bytes_arg(pre,0,&s,&n,&one);
    if(a>b->len || e-a<n) return v_bool(a<=b->len && n==0 && e>=a);
    return v_bool(!memcmp(b->s+(ends?e-n:a),s,(size_t)n));
}
static Value m_bstartswith(int argc, Value *argv, TupleObj *kw){ (void)kw; return bstarts(argc,argv,0,"startswith"); }
static Value m_bendswith(int argc, Value *argv, TupleObj *kw){ (void)kw; return bstarts(argc,argv,1,"endswith"); }
static int bspace(unsigned char c){ return c==' '||(c>=9&&c<=13); }
static Value bsplit(int argc, Value *argv, TupleObj *kw, int rev){
    int np=npos_of(argc,kw);
    Value sepv= np>1 ? argv[1] : kwarg(argc,argv,kw,"sep",v_none());
    Value maxv= np>2 ? argv[2] : kwarg(argc,argv,kw,"maxsplit",v_int(-1));
    BytesObj *b=B(SELF); int64_t maxsplit=mp_index(maxv,"maxsplit"), n=b->len; const unsigned char *s=b->s;
    Value l=mp_list(0,NULL), parts=rev?mp_list(0,NULL):l;
    if(IS_NONE(sepv)){
        if(!rev){
            int64_t p=0;
            while(p<n){
                if(bspace(s[p])){ p++; continue; }
                if(maxsplit==0){ int64_t e=n; while(e>p && bspace(s[e-1])) e--; mp_list_append(l,bytes_of(s+p,e-p)); break; }
                int64_t st=p; while(p<n && !bspace(s[p])) p++;
                mp_list_append(l,bytes_of(s+st,p-st)); maxsplit--;
            }
            return l;
        }
        int64_t e=n;
        while(e>0){
            if(bspace(s[e-1])){ e--; continue; }
            if(maxsplit==0){ int64_t a=0; while(a<e && bspace(s[a])) a++; mp_list_append(parts,bytes_of(s+a,e-a)); break; }
            int64_t st=e; while(e>0 && !bspace(s[e-1])) e--;
            mp_list_append(parts,bytes_of(s+e,st-e)); maxsplit--;
        }
    } else {
        const char *sep; int64_t sn; char one; bytes_arg(sepv,0,&sep,&sn,&one);
        if(!sn) mp_raise_t(E_ValueError,"empty separator");
        if(!rev){
            int64_t p=0;
            while(maxsplit!=0){ int64_t f=find_bytes((const char*)s,p,n,sep,sn,0); if(f<0) break; mp_list_append(l,bytes_of(s+p,f-p)); p=f+sn; maxsplit--; }
            mp_list_append(l,bytes_of(s+p,n-p));
            return l;
        }
        int64_t e=n;
        while(maxsplit!=0){ int64_t f=find_bytes((const char*)s,0,e,sep,sn,1); if(f<0) break; mp_list_append(parts,bytes_of(s+f+sn,e-f-sn)); e=f; maxsplit--; }
        mp_list_append(parts,bytes_of(s,e));
    }
    ListObj *pl=AS_LIST(parts); for(int64_t i=pl->len-1;i>=0;i--) mp_list_append(l,pl->items[i]);
    return l;
}
static Value m_bsplit(int argc, Value *argv, TupleObj *kw){ return bsplit(argc,argv,kw,0); }
static Value m_brsplit(int argc, Value *argv, TupleObj *kw){ return bsplit(argc,argv,kw,1); }
static Value bstrip(int argc, Value *argv, int left, int right, const char *name){
    nargs(name,argc,0,1);
    BytesObj *b=B(SELF); const char *cs=NULL; int64_t cn=0; char one;
    if(argc>1 && !IS_NONE(argv[1])) bytes_arg(argv[1],0,&cs,&cn,&one);
    int64_t a=0, e=b->len;
    #define GOES(c) (cs ? memchr(cs,(c),(size_t)cn)!=NULL : bspace(c))
    if(left) while(a<e && GOES(b->s[a])) a++;
    if(right) while(e>a && GOES(b->s[e-1])) e--;
    #undef GOES
    return bytes_of(b->s+a,e-a);
}
static Value m_bstrip(int argc, Value *argv, TupleObj *kw){ (void)kw; return bstrip(argc,argv,1,1,"strip"); }
static Value m_blstrip(int argc, Value *argv, TupleObj *kw){ (void)kw; return bstrip(argc,argv,1,0,"lstrip"); }
static Value m_brstrip(int argc, Value *argv, TupleObj *kw){ (void)kw; return bstrip(argc,argv,0,1,"rstrip"); }
static Value m_bjoin(int argc, Value *argv, TupleObj *kw){
    (void)kw; nargs("join",argc,1,1); BytesObj *sep=B(SELF); Value l=mp_list_of(argv[1]); SBuf o={0};
    for(int64_t i=0;i<AS_LIST(l)->len;i++){
        Value x=AS_LIST(l)->items[i];
        if(!IS_BYTES(x) && !IS(x,T_buffer) && !IS_BA(x)){ free(o.s); mp_raise_t(E_TypeError,"sequence item %lld: expected a bytes-like object, %s found",(long long)i,mp_type_name(x)); }
        const char *s; int64_t n; char one; bytes_arg(x,0,&s,&n,&one);
        if(i) sb_put(&o,(const char*)sep->s,sep->len);
        sb_put(&o,s,n);
    }
    Value r=mp_bytes(o.s?o.s:"",o.n); free(o.s); return r;
}
static Value m_breplace(int argc, Value *argv, TupleObj *kw){
    int np=npos_of(argc,kw);
    if(np<3) mp_raise_t(E_TypeError,"replace expected at least 2 arguments, got %d",np-1);
    BytesObj *b=B(SELF); const char *o, *n; int64_t on, nn; char one1, one2;
    bytes_arg(argv[1],0,&o,&on,&one1); bytes_arg(argv[2],0,&n,&nn,&one2);
    int64_t count= np>3 ? mp_index(argv[3],"count") : mp_index(kwarg(argc,argv,kw,"count",v_int(-1)),"count");
    SBuf out={0}; int64_t p=0, k=0;
    if(!on){
        if(count!=0){ sb_put(&out,n,nn); k++; }
        for(int64_t i=0;i<b->len;i++){ sb_putc(&out,(char)b->s[i]); if(count<0||k<count){ sb_put(&out,n,nn); k++; } }
    } else {
        while(count<0||k<count){ int64_t f=find_bytes((const char*)b->s,p,b->len,o,on,0); if(f<0) break; sb_put(&out,(const char*)b->s+p,f-p); sb_put(&out,n,nn); p=f+on; k++; }
        sb_put(&out,(const char*)b->s+p,b->len-p);
    }
    Value r=mp_bytes(out.s?out.s:"",out.n); free(out.s); return r;
}
static Value bcase(Value self, int mode){        /* 0 upper, 1 lower, 2 swapcase, 3 title, 4 capitalize: ASCII letters only */
    BytesObj *b=B(self); Value r=mp_bytes(b->s,b->len); unsigned char *q=B(r)->s; int prev=0;
    for(int64_t i=0;i<b->len;i++){
        unsigned char c=q[i]; int up=c>='A'&&c<='Z', lo=c>='a'&&c<='z';
        switch(mode){
            case 0: if(lo) c-=32; break;
            case 1: if(up) c+=32; break;
            case 2: if(lo) c-=32; else if(up) c+=32; break;
            case 3: if(prev){ if(up) c+=32; } else if(lo) c-=32; prev=up||lo; break;
            case 4: if(i==0){ if(lo) c-=32; } else if(up) c+=32; break;
        }
        q[i]=c;
    }
    return r;
}
static Value m_bupper(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("upper",argc,0,0); return bcase(SELF,0); }
static Value m_blower(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("lower",argc,0,0); return bcase(SELF,1); }
static Value m_bswapcase(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("swapcase",argc,0,0); return bcase(SELF,2); }
static Value m_btitle(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("title",argc,0,0); return bcase(SELF,3); }
static Value m_bcapitalize(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("capitalize",argc,0,0); return bcase(SELF,4); }
static Value bis(Value self, int mode){          /* 0 isalpha 1 isdigit 2 isalnum 3 isspace 4 islower 5 isupper 6 istitle 7 isascii */
    BytesObj *b=B(self); int any=0, prev=0;
    if(!b->len) return v_bool(mode==7);
    for(int64_t i=0;i<b->len;i++){
        unsigned char c=b->s[i]; int up=c>='A'&&c<='Z', lo=c>='a'&&c<='z', dg=c>='0'&&c<='9';
        switch(mode){
            case 0: if(!up&&!lo) return v_bool(0); break;
            case 1: if(!dg) return v_bool(0); break;
            case 2: if(!up&&!lo&&!dg) return v_bool(0); break;
            case 3: if(!bspace(c)) return v_bool(0); break;
            case 4: if(up) return v_bool(0); if(lo) any=1; break;
            case 5: if(lo) return v_bool(0); if(up) any=1; break;
            case 6: if(up){ if(prev) return v_bool(0); prev=any=1; } else if(lo){ if(!prev) return v_bool(0); prev=any=1; } else prev=0; break;
            case 7: if(c>=0x80) return v_bool(0); break;
        }
    }
    return v_bool(mode>=4 && mode<=6 ? any : 1);
}
#define BISFN(name,mode) static Value m_b##name(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs(#name,argc,0,0); return bis(SELF,mode); }
BISFN(isalpha,0) BISFN(isdigit,1) BISFN(isalnum,2) BISFN(isspace,3) BISFN(islower,4) BISFN(isupper,5) BISFN(istitle,6) BISFN(isascii,7)
static Value bpartition(int argc, Value *argv, int rev){
    nargs(rev?"rpartition":"partition",argc,1,1);
    BytesObj *b=B(SELF); const char *s; int64_t n; char one; bytes_arg(argv[1],0,&s,&n,&one);
    if(!n) mp_raise_t(E_ValueError,"empty separator");
    int64_t f=find_bytes((const char*)b->s,0,b->len,s,n,rev); Value r[3];
    if(f<0){ if(rev){ r[0]=bytes_of("",0); r[1]=bytes_of("",0); r[2]=SELF; } else { r[0]=SELF; r[1]=bytes_of("",0); r[2]=bytes_of("",0); } }
    else { r[0]=bytes_of(b->s,f); r[1]=bytes_of(s,n); r[2]=bytes_of(b->s+f+n,b->len-f-n); }
    return mp_tuple(3,r);
}
static Value m_bpartition(int argc, Value *argv, TupleObj *kw){ (void)kw; return bpartition(argc,argv,0); }
static Value m_brpartition(int argc, Value *argv, TupleObj *kw){ (void)kw; return bpartition(argc,argv,1); }
static Value m_bremoveprefix(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("removeprefix",argc,1,1); BytesObj *b=B(SELF); const char *s; int64_t n; char one; bytes_arg(argv[1],0,&s,&n,&one);
    if(n<=b->len && !memcmp(b->s,s,(size_t)n)) return bytes_of(b->s+n,b->len-n); return SELF; }
static Value m_bremovesuffix(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("removesuffix",argc,1,1); BytesObj *b=B(SELF); const char *s; int64_t n; char one; bytes_arg(argv[1],0,&s,&n,&one);
    if(n && n<=b->len && !memcmp(b->s+b->len-n,s,(size_t)n)) return bytes_of(b->s,b->len-n); return SELF; }
static Value m_bsplitlines(int argc, Value *argv, TupleObj *kw){
    int keep= npos_of(argc,kw)>1 ? mp_truth(argv[1]) : mp_truth(kwarg(argc,argv,kw,"keepends",v_bool(0)));
    BytesObj *b=B(SELF); Value l=mp_list(0,NULL); int64_t st=0, p=0;
    while(p<b->len){
        unsigned char c=b->s[p];
        if(c=='\n'||c=='\r'){ int64_t e=p+1; if(c=='\r' && e<b->len && b->s[e]=='\n') e++; mp_list_append(l,bytes_of(b->s+st,(keep?e:p)-st)); st=p=e; continue; }
        p++;
    }
    if(st<b->len) mp_list_append(l,bytes_of(b->s+st,b->len-st));
    return l;
}
/* bytes.center / ljust / rjust (fill: one byte), zfill, expandtabs, translate, maketrans */
static Value bjustify(int argc, Value *argv, int mode, const char *name){
    nargs(name,argc,1,2);
    BytesObj *s=B(SELF); int64_t w=mp_index(argv[1],"width"); unsigned char fill=' ';
    if(argc>2){ const unsigned char *f; int64_t fn;
        if(!mp_byteslike(argv[2],&f,&fn)) mp_raise_t(E_TypeError,"%s() argument 2 must be a byte string of length 1, not %s",name,mp_type_name(argv[2]));
        if(fn!=1) mp_raise_t(E_TypeError,"%s(): argument 2 must be a byte string of length 1, not a %s object of length %lld",name,mp_type_name(argv[2]),(long long)fn);
        fill=f[0]; }
    if(w<=s->len) return mp_bytes(s->s,s->len);
    int64_t padn=w-s->len, left= mode==0 ? 0 : mode==1 ? padn : padn/2 + (padn&w&1);
    SBuf b={0};
    for(int64_t i=0;i<left;i++) sb_putc(&b,(char)fill);
    sb_put(&b,(const char*)s->s,s->len);
    for(int64_t i=0;i<padn-left;i++) sb_putc(&b,(char)fill);
    Value r=mp_bytes(b.s,b.n); free(b.s); return r;
}
static Value m_bljust(int argc, Value *argv, TupleObj *kw){ (void)kw; return bjustify(argc,argv,0,"ljust"); }
static Value m_brjust(int argc, Value *argv, TupleObj *kw){ (void)kw; return bjustify(argc,argv,1,"rjust"); }
static Value m_bcenter(int argc, Value *argv, TupleObj *kw){ (void)kw; return bjustify(argc,argv,2,"center"); }
static Value m_bzfill(int argc, Value *argv, TupleObj *kw){
    (void)kw; nargs("zfill",argc,1,1);
    BytesObj *s=B(SELF); int64_t w=mp_index(argv[1],"width");
    if(w<=s->len) return mp_bytes(s->s,s->len);
    SBuf b={0}; int64_t st=0;
    if(s->len && (s->s[0]=='+'||s->s[0]=='-')){ sb_putc(&b,(char)s->s[0]); st=1; }
    for(int64_t i=0;i<w-s->len;i++) sb_putc(&b,'0');
    sb_put(&b,(const char*)s->s+st,s->len-st);
    Value r=mp_bytes(b.s,b.n); free(b.s); return r;
}
static Value m_bexpandtabs(int argc, Value *argv, TupleObj *kw){
    int64_t ts= npos_of(argc,kw)>1 ? mp_index(argv[1],"tabsize") : mp_index(kwarg(argc,argv,kw,"tabsize",v_int(8)),"tabsize");
    BytesObj *s=B(SELF); SBuf b={0}; int64_t col=0;
    for(int64_t i=0;i<s->len;i++){ unsigned char c=s->s[i];
        if(c=='\t'){ if(ts>0){ int64_t n=ts-col%ts; for(int64_t k=0;k<n;k++) sb_putc(&b,' '); col+=n; } }
        else if(c=='\n'||c=='\r'){ sb_putc(&b,(char)c); col=0; }
        else { sb_putc(&b,(char)c); col++; } }
    Value r=mp_bytes(b.s?b.s:"",b.n); free(b.s); return r;
}
static Value m_btranslate(int argc, Value *argv, TupleObj *kw){
    int np=npos_of(argc,kw); nargs("translate",np,1,2);
    Value table=argv[1], del= np>2 ? argv[2] : kwarg(argc,argv,kw,"delete",v_undef());
    const unsigned char *t=NULL, *d=NULL; int64_t tn=0, dn=0;
    if(!IS_NONE(table)){
        if(!mp_byteslike(table,&t,&tn)) mp_raise_t(E_TypeError,"a bytes-like object is required, not '%s'",mp_type_name(table));
        if(tn!=256) mp_raise_t(E_ValueError,"translation table must be 256 characters long"); }
    if(del.k!=V_UNDEF && !mp_byteslike(del,&d,&dn)) mp_raise_t(E_TypeError,"a bytes-like object is required, not '%s'",mp_type_name(del));
    unsigned char drop[256]={0}; for(int64_t i=0;i<dn;i++) drop[d[i]]=1;
    BytesObj *s=B(SELF); SBuf b={0};
    for(int64_t i=0;i<s->len;i++){ unsigned char c=s->s[i]; if(drop[c]) continue; sb_putc(&b,(char)(t?t[c]:c)); }
    Value r=mp_bytes(b.s?b.s:"",b.n); free(b.s); return r;
}
static Value m_bmaketrans(int argc, Value *argv, TupleObj *kw){
    (void)kw; int base= argc==3 ? 1 : 0;
    if(argc-base!=2) mp_raise_t(E_TypeError,"maketrans expected 2 arguments, got %d",argc-base);
    const unsigned char *a, *b; int64_t an, bn;
    if(!mp_byteslike(argv[base],&a,&an)) mp_raise_t(E_TypeError,"a bytes-like object is required, not '%s'",mp_type_name(argv[base]));
    if(!mp_byteslike(argv[base+1],&b,&bn)) mp_raise_t(E_TypeError,"a bytes-like object is required, not '%s'",mp_type_name(argv[base+1]));
    if(an!=bn) mp_raise_t(E_ValueError,"maketrans arguments must have same length");
    unsigned char t[256]; for(int i=0;i<256;i++) t[i]=(unsigned char)i;
    for(int64_t i=0;i<an;i++) t[a[i]]=b[i];
    return mp_bytes(t,256);
}
/* the methods of bytes; bytearray shares maketrans (mp_methods_init) */
void mp_bytes_methods_init(void){
    Type *t=T_bytes;
    mp_type_add(t,"decode",m_decode); mp_type_add(t,"hex",m_hex); mp_type_add(t,"find",m_bfind); mp_type_add(t,"rfind",m_brfind); mp_type_add(t,"index",m_bindex); mp_type_add(t,"rindex",m_brindex);
    mp_type_add(t,"count",m_bcount); mp_type_add(t,"startswith",m_bstartswith); mp_type_add(t,"endswith",m_bendswith);
    mp_type_add(t,"split",m_bsplit); mp_type_add(t,"rsplit",m_brsplit); mp_type_add(t,"strip",m_bstrip); mp_type_add(t,"lstrip",m_blstrip); mp_type_add(t,"rstrip",m_brstrip);
    mp_type_add(t,"join",m_bjoin); mp_type_add(t,"replace",m_breplace); mp_type_add(t,"upper",m_bupper); mp_type_add(t,"lower",m_blower); mp_type_add(t,"swapcase",m_bswapcase);
    mp_type_add(t,"title",m_btitle); mp_type_add(t,"capitalize",m_bcapitalize); mp_type_add(t,"isalpha",m_bisalpha); mp_type_add(t,"isdigit",m_bisdigit); mp_type_add(t,"isalnum",m_bisalnum);
    mp_type_add(t,"isspace",m_bisspace); mp_type_add(t,"islower",m_bislower); mp_type_add(t,"isupper",m_bisupper); mp_type_add(t,"istitle",m_bistitle); mp_type_add(t,"isascii",m_bisascii);
    mp_type_add(t,"partition",m_bpartition); mp_type_add(t,"rpartition",m_brpartition); mp_type_add(t,"removeprefix",m_bremoveprefix); mp_type_add(t,"removesuffix",m_bremovesuffix);
    mp_type_add(t,"splitlines",m_bsplitlines); mp_type_add(t,"center",m_bcenter); mp_type_add(t,"ljust",m_bljust); mp_type_add(t,"rjust",m_brjust);
    mp_type_add(t,"zfill",m_bzfill); mp_type_add(t,"expandtabs",m_bexpandtabs); mp_type_add(t,"translate",m_btranslate);
    { Value mt=mp_native("maketrans",m_bmaketrans); BoxObj *sm=(BoxObj*)mp_alloc(T_staticmethod,sizeof(BoxObj)); sm->v=mt; mp_dict_set(t->dict,mp_intern("maketrans"),v_obj(sm));
      if(T_bytearray){ BoxObj *sm2=(BoxObj*)mp_alloc(T_staticmethod,sizeof(BoxObj)); sm2->v=mt; mp_dict_set(T_bytearray->dict,mp_intern("maketrans"),v_obj(sm2)); } }
    { Value fh=mp_native("fromhex",m_fromhex); BoxObj *cm=(BoxObj*)mp_alloc(T_classmethod,sizeof(BoxObj)); cm->v=fh; mp_dict_set(t->dict,mp_intern("fromhex"),v_obj(cm)); }
}

