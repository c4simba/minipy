/* ---------------------------------------------------------------- floats as text, exactly
   (aot/x2c_float.c: part of the C runtime of every translated program, host
   or port.) The runtime routines that turn floats into text and back compute
   in the x87's 80-bit precision; the C translation does them here instead,
   exactly - without the C library, so that a port needs none:

     x2c_fmt_gen    %g/%e/repr-like text: the value rounded to P significant digits
     x2c_fmt_fixed  %.*f
     x2c_parse_float  float(str): strtod's correctly rounded result

   The digits come from the exact decimal value of the double (a big number
   in base 10^9: the mantissa times a power of 2 or of 5), rounded half to
   even, as glibc's and CPython's conversions round. Text to double: the
   decimal digits as a big integer D and its exponent E; D * 10^E, or
   D / 10^-E by long division to 64 bits, then rounded once to the nearest
   double (subnormals included). The big numbers live in static memory (the
   C stack of a port may be small; the program runs one native at a time). */

typedef union { double d; u64 u; } X2cBits;
static int x2c_isnan(double v){ X2cBits b; b.d=v; return (b.u&0x7FF0000000000000ull)==0x7FF0000000000000ull && (b.u&0x000FFFFFFFFFFFFFull); }
static int x2c_isinf(double v){ X2cBits b; b.d=v; return (b.u&0x7FFFFFFFFFFFFFFFull)==0x7FF0000000000000ull; }
static int x2c_signbit(double v){ X2cBits b; b.d=v; return (int)(b.u>>63); }
static double x2c_from_bits(u64 u){ X2cBits b; b.u=u; return b.d; }
static char *x2c_puts(char *o, const char *s){ while(*s) *o++=*s++; return o; }
static char *x2c_putint(char *o, int v){                  /* v >= 0 */
    char t[12]; int n=0; do{ t[n++]=(char)('0'+v%10); v/=10; }while(v);
    while(n) *o++=t[--n];
    return o;
}

/* ---- double -> its exact decimal digits */
#define X2C_DEC_LIMBS 96
static u32 x2c_dec[X2C_DEC_LIMBS];
static char x2c_alldig[X2C_DEC_LIMBS*9+16];
/* |v| finite and nonzero: every decimal digit of it (no leading or trailing
   zeros) into x2c_alldig; returns their count, *e10 the exponent of the first */
static int x2c_dtoa_all(double v, int *e10){
    X2cBits b; b.d=v;
    int ex=(int)((b.u>>52)&0x7FF); u64 mant=b.u&0x000FFFFFFFFFFFFFull; int e2;
    if(ex){ mant|=0x0010000000000000ull; e2=ex-1075; } else e2=-1074;
    while(!(mant&1)){ mant>>=1; e2++; }
    int n=0;
    while(mant){ x2c_dec[n++]=(u32)(mant%1000000000u); mant/=1000000000u; }
    int dexp=0;
    if(e2>0){                                           /* * 2^e2, 28 bits at a time */
        while(e2>0){ int k=e2>28?28:e2; u64 c=0;
            for(int i=0;i<n;i++){ u64 t=((u64)x2c_dec[i]<<k)+c; x2c_dec[i]=(u32)(t%1000000000u); c=t/1000000000u; }
            while(c){ x2c_dec[n++]=(u32)(c%1000000000u); c/=1000000000u; }
            e2-=k; }
    } else if(e2<0){                                    /* v = mant * 5^-e2 * 10^e2 */
        dexp=e2;
        for(int k=-e2;k>0;){ int s=k>13?13:k; u32 mul=1; for(int j=0;j<s;j++) mul*=5u; u64 c=0;
            for(int i=0;i<n;i++){ u64 t=(u64)x2c_dec[i]*mul+c; x2c_dec[i]=(u32)(t%1000000000u); c=t/1000000000u; }
            while(c){ x2c_dec[n++]=(u32)(c%1000000000u); c/=1000000000u; }
            k-=s; }
    }
    char *o=x2c_alldig;
    { u32 top=x2c_dec[n-1]; char t[10]; int k=0; while(top){ t[k++]=(char)('0'+top%10); top/=10; } while(k) *o++=t[--k]; }
    for(int i=n-2;i>=0;i--){ u32 l=x2c_dec[i]; for(int j=8;j>=0;j--){ o[j]=(char)('0'+l%10); l/=10; } o+=9; }
    int nd=(int)(o-x2c_alldig);
    *e10=nd-1+dexp;
    while(nd>1 && x2c_alldig[nd-1]=='0') nd--;
    return nd;
}
/* dig[0..nd) (nd >= 1) rounded half to even to n >= 1 digits in dig[0..n):
   returns 1 when that carried into a new first digit ("99.." -> "10..") */
static int x2c_round_digits(char *dig, int nd, int n){
    if(n>=nd){ for(int i=nd;i<n;i++) dig[i]='0'; return 0; }
    int up=0;
    if(dig[n]>'5') up=1;
    else if(dig[n]=='5'){ up=(dig[n-1]-'0')&1; for(int i=n+1;i<nd;i++) if(dig[i]!='0'){ up=1; break; } }
    if(!up) return 0;
    int i=n-1; while(i>=0 && dig[i]=='9') dig[i--]='0';
    if(i>=0){ dig[i]++; return 0; }
    dig[0]='1'; return 1;
}

/* rt_sb_gen: edx significant digits (1..17), flags: 1 ".0" after an integer,
   2 fixed notation up to 1e16, 4 scientific keeping trailing zeros, 8 one
   unit more in the last digit; else %g */
static u32 x2c_fmt_gen(char *out, double v, u32 P, u32 flags){
    char *o=out;
    if(P<1) P=1;
    if(P>17) P=17;
    if(x2c_isnan(v)){ o=x2c_puts(o,"nan"); *o=0; return (u32)(o-out); }
    if(x2c_isinf(v)){ o=x2c_puts(o,v<0?"-inf":"inf"); *o=0; return (u32)(o-out); }
    if(v==0){ if(x2c_signbit(v)) *o++='-'; *o++='0'; if(flags&1){ *o++='.'; *o++='0'; } *o=0; return (u32)(o-out); }
    if(v<0){ *o++='-'; v=-v; }
    int e; int nd=x2c_dtoa_all(v,&e);
    char dig[24];
    { char *a=x2c_alldig; int n=(int)P; if(x2c_round_digits(a,nd,n)) e++; for(int i=0;i<n;i++) dig[i]=a[i]; }
    int n=(int)P;
    if(flags&8){ int i=n-1; while(i>=0 && dig[i]=='9') dig[i--]='0'; if(i<0){ dig[0]='1'; e++; } else dig[i]++; }   /* one unit more in the last digit */
    if(!(flags&4)) while(n>1 && dig[n-1]=='0') n--;
    int lim=(flags&2)?16:(int)P, dotzero=0;
    if((flags&4) || e<-4 || e>=lim){                    /* d[.ddd]e+XX */
        *o++=dig[0];
        if(n>1){ *o++='.'; for(int i=1;i<n;i++) *o++=dig[i]; }
        *o++='e'; *o++=e<0?'-':'+'; if(e<0) e=-e;
        if(e<10) *o++='0';
        o=x2c_putint(o,e);
    } else if(e>=0){
        int id=e+1;
        if(id>=n){ for(int i=0;i<n;i++) *o++=dig[i]; for(int i=n;i<id;i++) *o++='0'; dotzero=1; }
        else { for(int i=0;i<id;i++) *o++=dig[i]; *o++='.'; for(int i=id;i<n;i++) *o++=dig[i]; }
    } else {                                            /* 0.000ddd */
        *o++='0'; *o++='.'; for(int i=0;i<-e-1;i++) *o++='0';
        for(int i=0;i<n;i++) *o++=dig[i];
    }
    if(dotzero && (flags&1)){ *o++='.'; *o++='0'; }
    *o=0; return (u32)(o-out);
}

/* 10^0 .. 10^40 as doubles (the nearest ones: what pow(10, d) gives) */
static const double x2c_p10[41]={1e0,1e1,1e2,1e3,1e4,1e5,1e6,1e7,1e8,1e9,1e10,1e11,1e12,1e13,1e14,1e15,1e16,1e17,1e18,1e19,1e20,
    1e21,1e22,1e23,1e24,1e25,1e26,1e27,1e28,1e29,1e30,1e31,1e32,1e33,1e34,1e35,1e36,1e37,1e38,1e39,1e40};
/* rt_sb_fixed: printf %.*f (edx digits after the point); from 2^61 on, %.17g */
static u32 x2c_fmt_fixed(char *out, double v, u32 d){
    if(x2c_isnan(v) || x2c_isinf(v)) return x2c_fmt_gen(out,v,17,0);
    char *o=out;
    if(x2c_signbit(v)){ *o++='-'; v=-v; }
    if(d>40) d=40;
    if(v*x2c_p10[d]>=2305843009213693952.0) return (u32)(o-out)+x2c_fmt_gen(o,v,17,0);
    int dd=(int)d;
    if(v==0){ *o++='0'; if(dd){ *o++='.'; for(int i=0;i<dd;i++) *o++='0'; } *o=0; return (u32)(o-out); }
    int e; int nd=x2c_dtoa_all(v,&e);
    char *dig=x2c_alldig;
    int n=e+1+dd;                                       /* the digits kept: up to the dd-th after the point */
    if(n<=0){                                           /* below the last place: 0, or one unit of it */
        int up= n==0 && (dig[0]>'5' || (dig[0]=='5' && nd>1));
        *o++='0';
        if(dd){ *o++='.'; for(int i=0;i<dd-1;i++) *o++='0'; *o++=up?'1':'0'; }
        else if(up) o[-1]='1';
        *o=0; return (u32)(o-out);
    }
    if(x2c_round_digits(dig,nd,n)){ e++; dig[n++]='0'; }
    if(e>=0){ for(int i=0;i<=e;i++) *o++=dig[i]; }
    else *o++='0';
    if(dd){
        *o++='.';
        if(e>=0) for(int i=e+1;i<n;i++) *o++=dig[i];
        else { for(int i=0;i<-e-1;i++) *o++='0'; for(int i=0;i<n;i++) *o++=dig[i]; }
    }
    *o=0; return (u32)(o-out);
}

/* ---- text -> double, correctly rounded */
#define X2C_BIG 160
typedef struct { u32 w[X2C_BIG]; int n; } X2cBig;
static X2cBig x2c_bd, x2c_bp;
static void x2c_big_trim(X2cBig *a){ while(a->n && !a->w[a->n-1]) a->n--; }
static void x2c_big_muladd(X2cBig *a, u32 m, u32 add){
    u64 c=add;
    for(int i=0;i<a->n;i++){ u64 t=(u64)a->w[i]*m+c; a->w[i]=(u32)t; c=t>>32; }
    if(c && a->n<X2C_BIG) a->w[a->n++]=(u32)c;
}
static void x2c_big_pow10(X2cBig *a, int k){ while(k>0){ int s=k>9?9:k; u32 m=1; for(int j=0;j<s;j++) m*=10u; x2c_big_muladd(a,m,0); k-=s; } }
static int x2c_big_bits(const X2cBig *a){
    if(!a->n) return 0;
    u32 t=a->w[a->n-1]; int b=0; while(t){ b++; t>>=1; }
    return (a->n-1)*32+b;
}
static void x2c_big_shl(X2cBig *a, int s){
    if(!a->n || s<=0) return;
    int ws=s/32, bs=s%32;
    int nn=a->n+ws+1; if(nn>X2C_BIG) nn=X2C_BIG;
    for(int i=nn-1;i>=0;i--){
        int j=i-ws; u32 hi= j>=0 && j<a->n ? a->w[j] : 0, lo= j-1>=0 && j-1<a->n ? a->w[j-1] : 0;
        a->w[i]= bs ? (hi<<bs)|(lo>>(32-bs)) : hi;
    }
    a->n=nn; x2c_big_trim(a);
}
static void x2c_big_shr1(X2cBig *a){
    for(int i=0;i<a->n;i++) a->w[i]=(a->w[i]>>1)|(i+1<a->n ? a->w[i+1]<<31 : 0);
    x2c_big_trim(a);
}
static int x2c_big_cmp(const X2cBig *a, const X2cBig *b){
    if(a->n!=b->n) return a->n<b->n ? -1 : 1;
    for(int i=a->n-1;i>=0;i--) if(a->w[i]!=b->w[i]) return a->w[i]<b->w[i] ? -1 : 1;
    return 0;
}
static void x2c_big_sub(X2cBig *a, const X2cBig *b){     /* a -= b, a >= b */
    u32 br=0;
    for(int i=0;i<a->n;i++){ u64 t=(u64)a->w[i]-(i<b->n?b->w[i]:0)-br; a->w[i]=(u32)t; br=(u32)(t>>63); }
    x2c_big_trim(a);
}
/* (q + d) * 2^e2 to the nearest double, 0 <= d < 1, d > 0 when sticky */
static double x2c_make_double(u64 q, int e2, int sticky){
    if(!q) return 0.0;
    int L=0; { u64 t=q; while(t){ L++; t>>=1; } }
    int E=L-1+e2;
    if(E>1023) return x2c_from_bits(0x7FF0000000000000ull);
    int prec= E<-1022 ? E+1075 : 53;
    if(prec<=0){                                       /* below the smallest subnormal */
        if(prec==0 && (q!=((u64)1<<(L-1)) || sticky)) return x2c_from_bits(1);   /* more than half of it */
        return 0.0;
    }
    int shift=L-prec; u64 m;
    if(shift>0){
        m=q>>shift; u64 rem=q&((((u64)1)<<shift)-1), half=((u64)1)<<(shift-1);
        if(rem>half || (rem==half && (sticky || (m&1)))) m++;
    } else m=q<<(-shift);
    if(prec<53) return x2c_from_bits(m);               /* a subnormal: units of 2^-1074 (2^52 of them: the smallest normal) */
    if(m==((u64)1<<53)){ m>>=1; E++; if(E>1023) return x2c_from_bits(0x7FF0000000000000ull); }
    return x2c_from_bits(((u64)(E+1023)<<52)|(m&0x000FFFFFFFFFFFFFull));
}
/* s[0..n): digits[.digits][(e|E)[+-]digits], as checked by x2c_parse_float */
static double x2c_strtod(const u8 *s, const u8 *end){
    static char sig[804]; int k=0, dexp=0, sticky=0, seen_dot=0, nz=0;
    for(;s<end && ((*s>='0'&&*s<='9')||*s=='.');s++){
        if(*s=='.'){ seen_dot=1; continue; }
        if(*s=='0' && !nz){ if(seen_dot) dexp--; continue; }   /* leading zeros */
        nz=1;
        if(k<800){ sig[k++]=(char)*s; if(seen_dot) dexp--; }
        else { if(*s!='0') sticky=1; if(!seen_dot) dexp++; }
    }
    if(s<end && (*s|32)=='e'){
        s++; int neg=0; long ev=0;
        if(s<end && (*s=='-'||*s=='+')){ neg=*s=='-'; s++; }
        for(;s<end && *s>='0'&&*s<='9';s++) if(ev<100000) ev=ev*10+(*s-'0');
        dexp+= neg ? -(int)ev : (int)ev;
    }
    if(!k) return 0.0;
    if(sticky){ sig[k++]='1'; dexp--; }                /* (beyond 800 digits: only whether the rest is zero matters) */
    if(k+dexp>310) return x2c_from_bits(0x7FF0000000000000ull);
    if(k+dexp<-400) return 0.0;
    X2cBig *D=&x2c_bd, *P=&x2c_bp;
    D->n=0;
    for(int i=0;i<k;){ int c=k-i>9?9:k-i; u32 v=0, m=1; for(int j=0;j<c;j++){ v=v*10u+(u32)(sig[i+j]-'0'); m*=10u; } if(!D->n){ D->w[0]=v; D->n=1; } else x2c_big_muladd(D,m,v); i+=c; }
    x2c_big_trim(D);
    if(dexp>=0){
        x2c_big_pow10(D,dexp);
        int L=x2c_big_bits(D);
        if(L<=64){ u64 q=D->w[0]|(D->n>1?(u64)D->w[1]<<32:0); return x2c_make_double(q,0,0); }
        int sh=L-64, st=0;                              /* the top 64 bits, and whether any bit below them is 1 */
        for(int i=0;i<sh/32;i++) if(D->w[i]) st=1;
        if(sh%32 && (D->w[sh/32]&((1u<<(sh%32))-1))) st=1;
        u64 q=0; for(int b=63;b>=0;b--){ int bit=sh+b; q|=(u64)((D->w[bit/32]>>(bit%32))&1)<<b; }
        return x2c_make_double(q,sh,st);
    }
    P->n=1; P->w[0]=1; x2c_big_pow10(P,-dexp);
    int s2=63-(x2c_big_bits(D)-x2c_big_bits(P));      /* q = D * 2^s2 / P in [2^62, 2^64) */
    if(s2>0) x2c_big_shl(D,s2); else x2c_big_shl(P,-s2);
    x2c_big_shl(P,63);
    u64 q=0;
    for(int b=63;b>=0;b--){ if(x2c_big_cmp(D,P)>=0){ x2c_big_sub(D,P); q|=(u64)1<<b; } x2c_big_shr1(P); }
    return x2c_make_double(q,-s2,D->n!=0);
}

/* rt_float_parse: float(str) -> 1, or 0 when it is not a number */
static int x2c_is_space(u8 c){ return c==' ' || (c>=9 && c<=13); }
static int x2c_parse_float(u8 *m, u32 s, double *out){
    if(!s) return 0;
    u32 n=D(s+8); const u8 *p=m+s+12, *end=p+n;
    while(p<end && x2c_is_space(*p)) p++;
    if(p>=end) return 0;
    int neg=0;
    if(*p=='-'){ neg=1; p++; } else if(*p=='+') p++;
    const u8 *num=p; int digits=0;
    while(p<end && *p>='0' && *p<='9'){ p++; digits++; }
    if(p<end && *p=='.'){ p++; while(p<end && *p>='0' && *p<='9'){ p++; digits++; } }
    double v;
    if(!digits){                                        /* inf, infinity, nan (any case) */
        if(end-p<3) return 0;
        int c0=p[0]|32, c1=p[1]|32, c2=p[2]|32;
        if(c0=='i' && c1=='n' && c2=='f'){ v=x2c_from_bits(0x7FF0000000000000ull); p+=3;
            if(end-p>=5 && (p[0]|32)=='i' && (p[1]|32)=='n' && (p[2]|32)=='i' && (p[3]|32)=='t' && (p[4]|32)=='y') p+=5; }
        else if(c0=='n' && c1=='a' && c2=='n'){ v=x2c_from_bits(0x7FF8000000000000ull); p+=3; }
        else return 0;
    } else {
        if(p<end && (*p|32)=='e'){
            p++;
            if(p>=end) return 0;
            if(*p=='-'||*p=='+') p++;
            while(p<end && *p>='0' && *p<='9') p++;
        }
        v=x2c_strtod(num,p);
    }
    while(p<end && x2c_is_space(*p)) p++;
    if(p<end) return 0;
    *out=neg?-v:v;
    return 1;
}
