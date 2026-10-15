/* ========================= Interpreter, methods: int, float =========================
   The methods of int and float (to_bytes, as_integer_ratio, hex, ...). */

#include "methods.h"

static Value m_bit_length(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; uint64_t a= SELF.u.i<0 ? (uint64_t)(-(SELF.u.i+1))+1 : (uint64_t)SELF.u.i; int n=0; while(a){ n++; a>>=1; } return v_int(n); }
static Value m_bit_count(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; uint64_t a= SELF.u.i<0 ? (uint64_t)(-(SELF.u.i+1))+1 : (uint64_t)SELF.u.i; int n=0; while(a){ n+=(int)(a&1); a>>=1; } return v_int(n); }
/* "big" / "little" -> 0 / 1 */
static int byteorder_of(Value ord){
    if(!IS_STR(ord)) mp_raise_t(E_TypeError,"to_bytes() argument 'byteorder' must be str, not %s",mp_type_name(ord));
    if(mp_str_eq_c(ord,"big")) return 0;
    if(mp_str_eq_c(ord,"little")) return 1;
    mp_raise_t(E_ValueError,"byteorder must be either 'little' or 'big'");
}
static Value m_to_bytes(int argc, Value *argv, TupleObj *kw){
    int np=npos_of(argc,kw);
    int64_t len= np>1 ? mp_index(argv[1],"length") : mp_index(kwarg(argc,argv,kw,"length",v_int(1)),"length");
    Value ord= np>2 ? argv[2] : kwarg(argc,argv,kw,"byteorder",mp_str("big"));
    int sig=mp_truth(kwarg(argc,argv,kw,"signed",v_bool(0)));
    if(len<0) mp_raise_t(E_ValueError,"length argument must be non-negative");
    int little=byteorder_of(ord);
    int64_t v=SELF.u.i;
    if(!sig && v<0) mp_raise_t(E_OverflowError,"can't convert negative int to unsigned");
    if(len<8){
        int fits= !sig ? (len==0 ? v==0 : ((uint64_t)v>>(8*len))==0)
                       : (len==0 ? v==0 : (v>>(8*len-1))==0 || (v>>(8*len-1))==-1);
        if(!fits) mp_raise_t(E_OverflowError,"int too big to convert");
    }
    unsigned char *buf=(unsigned char*)xmalloc((size_t)len+1);
    for(int64_t i=0;i<len;i++){ unsigned char c=(unsigned char)(i<8 ? (uint64_t)v>>(8*i) : (v<0?0xff:0)); buf[little?i:len-1-i]=c; }
    Value r=mp_bytes(buf,len); free(buf); return r;
}
static Value m_from_bytes(int argc, Value *argv, TupleObj *kw){
    int np=npos_of(argc,kw); int base= IS_TYPE(argv[0]) ? 1 : 0;
    const char *s; int64_t n; char one; bytes_arg(argv[base],0,&s,&n,&one);
    Value ord= np>base+1 ? argv[base+1] : kwarg(argc,argv,kw,"byteorder",mp_str("big"));
    int sig=mp_truth(kwarg(argc,argv,kw,"signed",v_bool(0)));
    int little=byteorder_of(ord);
    uint64_t v=0;                                   /* the low 8 bytes; the rest must only extend them */
    for(int64_t k=0;k<n && k<8;k++) v|=(uint64_t)(unsigned char)s[little?k:n-1-k]<<(8*k);
    int neg= sig && n>0 && ((unsigned char)s[little?(n<8?n-1:7):(n<8?0:n-8)]&0x80);
    for(int64_t k=8;k<n;k++) if((unsigned char)s[little?k:n-1-k]!=(neg?0xff:0)) mp_raise_t(E_OverflowError,"integer overflow (ints are 64-bit)");
    if(!sig && (v>>63)) mp_raise_t(E_OverflowError,"integer overflow (ints are 64-bit)");
    if(neg && n<8) v|=~(uint64_t)0<<(n*8);
    return v_int((int64_t)v);
}
static Value m_conjugate(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; if(IS(SELF,T_complex)) return mp_complex(((ComplexObj*)SELF.u.o)->re,-((ComplexObj*)SELF.u.o)->im); return SELF.k==V_BOOL?v_int(SELF.u.i):SELF; }
static Value m_is_integer(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; if(SELF.k==V_FLOAT) return v_bool(isfinite(SELF.u.f) && SELF.u.f==floor(SELF.u.f)); return v_bool(1); }
static Value m_as_integer_ratio(int argc, Value *argv, TupleObj *kw){
    (void)kw; (void)argc;
    if(SELF.k!=V_FLOAT){ Value r[2]={v_int(SELF.u.i),v_int(1)}; return mp_tuple(2,r); }
    double x=SELF.u.f;
    if(isinf(x)) mp_raise_t(E_OverflowError,"cannot convert Infinity to integer ratio");
    if(isnan(x)) mp_raise_t(E_ValueError,"cannot convert NaN to integer ratio");
    int e; double m=frexp(x,&e); int64_t den=1;
    for(int i=0;i<300 && m!=floor(m);i++){ m*=2; e--; }
    if(e>0){ if(e>62) mp_raise_t(E_OverflowError,"integer overflow (ints are 64-bit)"); m=ldexp(m,e); }
    else { if(-e>62) mp_raise_t(E_OverflowError,"integer overflow (ints are 64-bit)"); den=(int64_t)1<<(-e); }
    int64_t num=(int64_t)m;
    while(den>1 && !(num&1) && !(den&1)){ num/=2; den/=2; }
    Value r[2]={v_int(num),v_int(den)}; return mp_tuple(2,r);
}
static Value m_fhex(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; char b[64]; snprintf(b,sizeof b,"%a",SELF.u.f); return mp_str(b); }
/* float.fromhex(s): [sign] [0x] hex digits [. hex digits] [p exponent], rounded half-even (subnormals too) */
static int hexdig(int c){ return c>='0'&&c<='9' ? c-'0' : c>='a'&&c<='f' ? c-'a'+10 : c>='A'&&c<='F' ? c-'A'+10 : -1; }
static int ci_is(const char *p, size_t n, const char *w){ if(strlen(w)!=n) return 0; for(size_t i=0;i<n;i++) if(tolower((unsigned char)p[i])!=w[i]) return 0; return 1; }
static Value m_ffromhex(int argc, Value *argv, TupleObj *kw){
    (void)kw; if(argc!=2) mp_raise_t(E_TypeError,"fromhex() takes exactly one argument (%d given)",argc-1);
    if(!IS_STR(argv[1])) mp_raise_t(E_TypeError,"must be str, not %s",mp_type_name(argv[1]));
    StrObj *str=S(argv[1]); const char *p=str->s, *e=str->s+str->len;
    while(p<e && isspace((unsigned char)*p)) p++;
    while(e>p && isspace((unsigned char)e[-1])) e--;
    int neg=0; double r;
    if(p<e && (*p=='+'||*p=='-')){ neg= *p=='-'; p++; }
    if(ci_is(p,(size_t)(e-p),"inf") || ci_is(p,(size_t)(e-p),"infinity")){ r=neg?-INFINITY:INFINITY; goto done; }
    if(ci_is(p,(size_t)(e-p),"nan")){ r=NAN; goto done; }
    if(e-p>=2 && p[0]=='0' && (p[1]=='x'||p[1]=='X')) p+=2;
    { uint64_t m=0; int sticky=0, ndig=0, nsig=0; int64_t ex=0; int d;
      while(p<e && (d=hexdig(*p))>=0){ p++; ndig++; if(!m && !d) continue; if(nsig<15){ m=m*16+(uint64_t)d; nsig++; } else { ex+=4; if(d) sticky=1; } }
      if(p<e && *p=='.'){ p++;
          while(p<e && (d=hexdig(*p))>=0){ p++; ndig++; if(!m && !d){ ex-=4; continue; } if(nsig<15){ m=m*16+(uint64_t)d; nsig++; ex-=4; } else if(d) sticky=1; } }
      if(!ndig) goto bad;
      if(p<e && (*p=='p'||*p=='P')){ p++; int eneg=0; int64_t pe=0;
          if(p<e && (*p=='+'||*p=='-')){ eneg= *p=='-'; p++; }
          if(!(p<e && isdigit((unsigned char)*p))) goto bad;
          while(p<e && isdigit((unsigned char)*p)){ if(pe<100000000) pe=pe*10+(*p-'0'); p++; }
          ex+= eneg ? -pe : pe; }
      if(p!=e) goto bad;
      if(!m){ r=neg?-0.0:0.0; goto done; }
      int nb=64-__builtin_clzll(m); int64_t top=ex+nb-1;
      if(top>1023) goto big;
      int64_t keep= top>=-1022 ? 53 : 53-(-1022-top);
      int64_t shift=nb-keep;
      if(keep<0){ r=neg?-0.0:0.0; goto done; }
      if(shift>0){
          uint64_t rem= shift>=64 ? m : m&((1ULL<<shift)-1), half=1ULL<<(shift-1);
          m= shift>=64 ? 0 : m>>shift;
          if(rem>half || (rem==half && (sticky || (m&1)))) m++;
          ex+=shift; }
      r=ldexp((double)m,(int)ex);
      if(isinf(r)) goto big;
      if(neg) r=-r; }
done:
    if(IS_TYPE(argv[0]) && (Type*)argv[0].u.o!=T_float) return mp_call1(argv[0],v_float(r));
    return v_float(r);
bad: mp_raise_t(E_ValueError,"invalid hexadecimal floating-point string");
big: mp_raise_t(E_OverflowError,"hexadecimal value too large to represent as a float");
}
/* float.from_number(x): float(x) for numbers only (not str) */
static Value m_ffromnumber(int argc, Value *argv, TupleObj *kw){
    (void)kw; if(argc!=2) mp_raise_t(E_TypeError,"from_number() takes exactly one argument (%d given)",argc-1);
    Value x=argv[1], u=mp_unbox(x);
    double r;
    if(u.k==V_FLOAT) r=u.u.f; else if(IS_INTLIKE(u)) r=(double)u.u.i;
    else { Value m= x.k==V_OBJ ? mp_type_lookup_s(TYPE(x),"__float__") : v_undef(); if(m.k==V_UNDEF && x.k==V_OBJ) m=mp_type_lookup_s(TYPE(x),"__index__");
        if(m.k==V_UNDEF || IS_STR(x) || IS_BYTES(x)) mp_raise_t(E_TypeError,"must be real number, not %s",mp_type_name(x));
        Value v=mp_unbox(mp_call1(m,x)); r= v.k==V_FLOAT ? v.u.f : (double)v.u.i; }
    if(IS_TYPE(argv[0]) && (Type*)argv[0].u.o!=T_float) return mp_call1(argv[0],v_float(r));
    return v_float(r);
}
/* int / float .real .imag .numerator .denominator (as properties: they show in dir()) */
static Value n_real(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; Value u=mp_unbox(argv[0]); return u.k==V_FLOAT ? u : v_int(u.u.i); }
static Value n_imag(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; Value u=mp_unbox(argv[0]); return u.k==V_FLOAT ? v_float(0.0) : v_int(0); }
static Value n_one(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; (void)argv; return v_int(1); }
static void add_prop(Type *t, const char *name, NFn get){
    PropertyObj *p=(PropertyObj*)mp_alloc(T_property,sizeof(PropertyObj));
    p->get=mp_native(name,get); p->set=v_none(); p->del=v_none(); p->doc=v_none();
    mp_dict_set(t->dict,mp_intern(name),v_obj(p));
}
/* the methods of int, float and complex (mp_methods_init) */
void mp_number_methods_init(void){
    Type *t=T_int;
    mp_type_add(t,"bit_length",m_bit_length); mp_type_add(t,"bit_count",m_bit_count); mp_type_add(t,"to_bytes",m_to_bytes); mp_type_add(t,"conjugate",m_conjugate);
    mp_type_add(t,"is_integer",m_is_integer); mp_type_add(t,"as_integer_ratio",m_as_integer_ratio);
    { Value fb=mp_native("from_bytes",m_from_bytes); BoxObj *cm=(BoxObj*)mp_alloc(T_classmethod,sizeof(BoxObj)); cm->v=fb; mp_dict_set(t->dict,mp_intern("from_bytes"),v_obj(cm)); }
    add_prop(T_int,"real",n_real); add_prop(T_int,"imag",n_imag); add_prop(T_int,"numerator",n_real); add_prop(T_int,"denominator",n_one);
    add_prop(T_float,"real",n_real); add_prop(T_float,"imag",n_imag);
    { Value f=mp_native("fromhex",m_ffromhex); BoxObj *cm=(BoxObj*)mp_alloc(T_classmethod,sizeof(BoxObj)); cm->v=f; mp_dict_set(T_float->dict,mp_intern("fromhex"),v_obj(cm));
      Value g=mp_native("from_number",m_ffromnumber); BoxObj *cn=(BoxObj*)mp_alloc(T_classmethod,sizeof(BoxObj)); cn->v=g; mp_dict_set(T_float->dict,mp_intern("from_number"),v_obj(cn)); }
    t=T_float; mp_type_add(t,"is_integer",m_is_integer); mp_type_add(t,"as_integer_ratio",m_as_integer_ratio); mp_type_add(t,"hex",m_fhex); mp_type_add(t,"conjugate",m_conjugate);
    t=T_complex; mp_type_add(t,"conjugate",m_conjugate);
}

