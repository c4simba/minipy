/* ========================= Interpreter, modules: math, time =========================
   math (as CPython's: gamma, fsum, isclose ...); time.sleep. */

#include "modules.h"

double farg(Value v){ return mp_float_of(v); }
#define M1(name,expr) static Value ma_##name(int argc, Value *argv, TupleObj *kw){ mod_no_kw(#name,kw); mod_nargs(#name,argc,1,1); double x=farg(argv[0]); (void)x; return v_float(expr); }
static void domain(int bad){ if(bad) mp_raise_t(E_ValueError,"math domain error"); }
static Value ma_sqrt(int argc, Value *argv, TupleObj *kw){ mod_no_kw("sqrt",kw); mod_nargs("sqrt",argc,1,1); double x=farg(argv[0]); domain(x<0); return v_float(sqrt(x)); }
static Value ma_log(int argc, Value *argv, TupleObj *kw){ mod_no_kw("log",kw); mod_nargs("log",argc,1,2); double x=farg(argv[0]); domain(x<=0);
    if(argc==2){ double b=farg(argv[1]); domain(b<=0||b==1); return v_float(log(x)/log(b)); } return v_float(log(x)); }
static Value ma_log10(int argc, Value *argv, TupleObj *kw){ mod_no_kw("log10",kw); mod_nargs("log10",argc,1,1); double x=farg(argv[0]); domain(x<=0); return v_float(log10(x)); }
static Value ma_log2(int argc, Value *argv, TupleObj *kw){ mod_no_kw("log2",kw); mod_nargs("log2",argc,1,1); double x=farg(argv[0]); domain(x<=0); return v_float(log2(x)); }
static Value ma_log1p(int argc, Value *argv, TupleObj *kw){ mod_no_kw("log1p",kw); mod_nargs("log1p",argc,1,1); double x=farg(argv[0]); domain(x<=-1); return v_float(log1p(x)); }
static Value ma_asin(int argc, Value *argv, TupleObj *kw){ mod_no_kw("asin",kw); mod_nargs("asin",argc,1,1); double x=farg(argv[0]); domain(x<-1||x>1); return v_float(asin(x)); }
static Value ma_acos(int argc, Value *argv, TupleObj *kw){ mod_no_kw("acos",kw); mod_nargs("acos",argc,1,1); double x=farg(argv[0]); domain(x<-1||x>1); return v_float(acos(x)); }
static Value ma_exp(int argc, Value *argv, TupleObj *kw){ mod_no_kw("exp",kw); mod_nargs("exp",argc,1,1); double r=exp(farg(argv[0])); if(isinf(r) && isfinite(farg(argv[0]))) mp_raise_t(E_OverflowError,"math range error"); return v_float(r); }
M1(sin,sin(x)) M1(cos,cos(x)) M1(tan,tan(x)) M1(atan,atan(x)) M1(sinh,sinh(x)) M1(cosh,cosh(x)) M1(tanh,tanh(x))
M1(asinh,asinh(x)) M1(fabs,fabs(x)) M1(expm1,expm1(x)) M1(degrees,x*(180.0/M_PI)) M1(radians,x*(M_PI/180.0)) M1(erf,erf(x)) M1(erfc,erfc(x))
M1(cbrt,cbrt(x)) M1(exp2,exp2(x))
/* gamma and lgamma: CPython's own (Lanczos, N=13, g=6.0246...), not the C library's */
static const double ma_lanczos_g=6.024680040776729583740234375, ma_lanczos_gmh=5.524680040776729583740234375;
static const double ma_lanczos_num[13]={23531376880.410759688572007674451636754734846804940,42919803642.649098768957899047001988850926355848959,
    35711959237.355668049440185451547166705960488635843,17921034426.037209699919755754458931112671403265390,6039542586.3520280050642916443072979210699388420708,
    1439720407.3117216736632230727949123939715485786772,248874557.86205415651146038641322942321632125127801,31426415.585400194380614231628318205362874684987640,
    2876370.6289353724412254090516208496135991145378768,186056.26539522349504029498971604569928220784236328,8071.6720023658162106380029022722506138218516325024,
    210.82427775157934587250973392071336271166969580291,2.5066282746310002701649081771338373386264310793408};
static const double ma_lanczos_den[13]={0.0,39916800.0,120543840.0,150917976.0,105258076.0,45995730.0,13339535.0,2637558.0,357423.0,32670.0,1925.0,66.0,1.0};
static const double ma_gamma_int[23]={1.0,1.0,2.0,6.0,24.0,120.0,720.0,5040.0,40320.0,362880.0,3628800.0,39916800.0,479001600.0,6227020800.0,87178291200.0,
    1307674368000.0,20922789888000.0,355687428096000.0,6402373705728000.0,121645100408832000.0,2432902008176640000.0,51090942171709440000.0,1124000727777607680000.0};
static double ma_sinpi(double x){
    double y=fmod(fabs(x),2.0), r; int n=(int)round(2.0*y);
    switch(n){ case 0: r=sin(M_PI*y); break; case 1: r=cos(M_PI*(y-0.5)); break; case 2: r=sin(M_PI*(1.0-y)); break; case 3: r=-cos(M_PI*(y-1.5)); break; default: r=sin(M_PI*(y-2.0)); }
    return copysign(1.0,x)*r;
}
static double ma_lanczos_sum(double x){
    double num=0.0, den=0.0;
    if(x<5.0) for(int i=13;--i>=0;){ num=num*x+ma_lanczos_num[i]; den=den*x+ma_lanczos_den[i]; }
    else for(int i=0;i<13;i++){ num=num/x+ma_lanczos_num[i]; den=den/x+ma_lanczos_den[i]; }
    return num/den;
}
static Value ma_gamma(int argc, Value *argv, TupleObj *kw){ mod_no_kw("gamma",kw); mod_nargs("gamma",argc,1,1); double x=farg(argv[0]), absx, r, y, z, sqrtpow;
    if(!isfinite(x)){ if(isnan(x) || x>0.0) return v_float(x); mp_raise_t(E_ValueError,"math domain error"); }
    if(x==0.0 || (x==floor(x) && x<0.0)) mp_raise_t(E_ValueError,"expected a noninteger or positive integer, got %s",AS_STR(mp_repr(argv[0]))->s);
    if(x==floor(x) && x<=23) return v_float(ma_gamma_int[(int)x-1]);
    absx=fabs(x);
    if(absx<1e-20){ r=1.0/x; if(isinf(r)) mp_raise_t(E_OverflowError,"math range error"); return v_float(r); }
    if(absx>200.0){ if(x<0.0) return v_float(0.0/ma_sinpi(x)); mp_raise_t(E_OverflowError,"math range error"); }
    y=absx+ma_lanczos_gmh;
    if(absx>ma_lanczos_gmh){ volatile double q=y-absx; z=q-ma_lanczos_gmh; } else { volatile double q=y-ma_lanczos_gmh; z=q-absx; }
    z=z*ma_lanczos_g/y;
    if(x<0.0){ r=-M_PI/ma_sinpi(absx)/absx*exp(y)/ma_lanczos_sum(absx); r-=z*r;
        if(absx<140.0) r/=pow(y,absx-0.5); else { sqrtpow=pow(y,absx/2.0-0.25); r/=sqrtpow; r/=sqrtpow; } }
    else { r=ma_lanczos_sum(absx)/exp(y); r+=z*r;
        if(absx<140.0) r*=pow(y,absx-0.5); else { sqrtpow=pow(y,absx/2.0-0.25); r*=sqrtpow; r*=sqrtpow; } }
    if(isinf(r)) mp_raise_t(E_OverflowError,"math range error");
    return v_float(r);
}
static Value ma_lgamma(int argc, Value *argv, TupleObj *kw){ mod_no_kw("lgamma",kw); mod_nargs("lgamma",argc,1,1); double x=farg(argv[0]), r, absx;
    if(!isfinite(x)) return v_float(isnan(x)?x:INFINITY);
    if(x==floor(x) && x<=2.0){ if(x<=0.0) mp_raise_t(E_ValueError,"math domain error"); return v_float(0.0); }
    absx=fabs(x);
    if(absx<1e-20) return v_float(-log(absx));
    r=log(ma_lanczos_sum(absx))-ma_lanczos_g;
    r+=(absx-0.5)*(log(absx+ma_lanczos_g-0.5)-1);
    if(x<0.0) r=1.144729885849400174143427351353058711647-log(fabs(ma_sinpi(absx)))-log(absx)-r;
    if(isinf(r)) mp_raise_t(E_OverflowError,"math range error");
    return v_float(r);
}
static Value ma_nextafter(int argc, Value *argv, TupleObj *kw){
    int np=argc-(kw?(int)kw->len:0); double x=farg(argv[0]), y=farg(argv[1]);
    if(np<2) mp_raise_t(E_TypeError,"nextafter expected 2 arguments, got %d",np);
    if(kw && kw->len){ int64_t steps=mp_index(argv[np],"steps"); if(steps<0) mp_raise_t(E_ValueError,"steps must be a non-negative integer");
        for(int64_t i=0;i<steps && x!=y;i++) x=nextafter(x,y); return v_float(x); }
    return v_float(nextafter(x,y)); }
static Value ma_ulp(int argc, Value *argv, TupleObj *kw){ mod_no_kw("ulp",kw); mod_nargs("ulp",argc,1,1); double x=farg(argv[0]);
    if(isnan(x)) return v_float(x); x=fabs(x); if(isinf(x)) return v_float(x);
    double x2=nextafter(x,INFINITY); if(isinf(x2)){ x2=nextafter(x,-INFINITY); return v_float(x-x2); } return v_float(x2-x); }
static Value ma_fma(int argc, Value *argv, TupleObj *kw){ mod_no_kw("fma",kw); mod_nargs("fma",argc,3,3);
    double x=farg(argv[0]), y=farg(argv[1]), z=farg(argv[2]), r=fma(x,y,z);
    if(isfinite(r)) return v_float(r);
    if(isnan(r)){ if(!isnan(x) && !isnan(y) && !isnan(z)) mp_raise_t(E_ValueError,"invalid operation in fma"); return v_float(r); }
    if(isfinite(x) && isfinite(y) && isfinite(z)) mp_raise_t(E_OverflowError,"overflow in fma");
    return v_float(r); }
static Value ma_sumprod(int argc, Value *argv, TupleObj *kw){ mod_no_kw("sumprod",kw); mod_nargs("sumprod",argc,2,2);
    Value ip=mp_iter(argv[0]), iq=mp_iter(argv[1]), a, b, total=v_int(0);
    for(;;){ int ha=mp_next(ip,&a), hb=mp_next(iq,&b);
        if(ha!=hb) mp_raise_t(E_ValueError,"Inputs are not the same length");
        if(!ha) break;
        total=mp_binop(OP_Add,total,mp_binop(OP_Mult,a,b)); }
    return total; }
static Value ma_acosh(int argc, Value *argv, TupleObj *kw){ mod_no_kw("acosh",kw); mod_nargs("acosh",argc,1,1); double x=farg(argv[0]); domain(x<1); return v_float(acosh(x)); }
static Value ma_atanh(int argc, Value *argv, TupleObj *kw){ mod_no_kw("atanh",kw); mod_nargs("atanh",argc,1,1); double x=farg(argv[0]); domain(x<=-1||x>=1); return v_float(atanh(x)); }
static Value ma_atan2(int argc, Value *argv, TupleObj *kw){ mod_no_kw("atan2",kw); mod_nargs("atan2",argc,2,2); return v_float(atan2(farg(argv[0]),farg(argv[1]))); }
static Value ma_pow(int argc, Value *argv, TupleObj *kw){ mod_no_kw("pow",kw); mod_nargs("pow",argc,2,2); double x=farg(argv[0]), y=farg(argv[1]);
    domain(x==0 && y<0); domain(x<0 && y!=floor(y) && isfinite(y)); double r=pow(x,y); if(isinf(r) && isfinite(x) && isfinite(y)) mp_raise_t(E_OverflowError,"math range error"); return v_float(r); }
static Value ma_fmod(int argc, Value *argv, TupleObj *kw){ mod_no_kw("fmod",kw); mod_nargs("fmod",argc,2,2); double x=farg(argv[0]), y=farg(argv[1]); domain(y==0 && !isnan(x)); return v_float(fmod(x,y)); }
static Value ma_remainder(int argc, Value *argv, TupleObj *kw){ mod_no_kw("remainder",kw); mod_nargs("remainder",argc,2,2); double y=farg(argv[1]); domain(y==0); return v_float(remainder(farg(argv[0]),y)); }
static Value ma_copysign(int argc, Value *argv, TupleObj *kw){ mod_no_kw("copysign",kw); mod_nargs("copysign",argc,2,2); return v_float(copysign(farg(argv[0]),farg(argv[1]))); }
static Value ma_ldexp(int argc, Value *argv, TupleObj *kw){ mod_no_kw("ldexp",kw); mod_nargs("ldexp",argc,2,2); double x=farg(argv[0]); int64_t i=mp_index(argv[1],"ldexp");
    if(i>100000) i=100000; if(i<-100000) i=-100000;
    double r=ldexp(x,(int)i); if(isinf(r) && isfinite(x)) mp_raise_t(E_OverflowError,"math range error"); return v_float(r); }
static Value ma_frexp(int argc, Value *argv, TupleObj *kw){ mod_no_kw("frexp",kw); mod_nargs("frexp",argc,1,1); int e; double m=frexp(farg(argv[0]),&e); Value r[2]={v_float(m),v_int(e)}; return mp_tuple(2,r); }
static Value ma_modf(int argc, Value *argv, TupleObj *kw){ mod_no_kw("modf",kw); mod_nargs("modf",argc,1,1); double ip; double fp=modf(farg(argv[0]),&ip); Value r[2]={v_float(fp),v_float(ip)}; return mp_tuple(2,r); }
static Value ma_hypot(int argc, Value *argv, TupleObj *kw){ mod_no_kw("hypot",kw); double s=0; double mx=0;
    for(int i=0;i<argc;i++){ double x=fabs(farg(argv[i])); if(isinf(x)) return v_float(INFINITY); if(x>mx) mx=x; }
    if(mx==0) return v_float(0);
    if(argc==2) return v_float(hypot(farg(argv[0]),farg(argv[1])));
    for(int i=0;i<argc;i++){ double x=farg(argv[i])/mx; s+=x*x; } return v_float(mx*sqrt(s)); }
static Value int_result(double r){
    if(isnan(r)) mp_raise_t(E_ValueError,"cannot convert float NaN to integer");
    if(isinf(r)) mp_raise_t(E_OverflowError,"cannot convert float infinity to integer");
    if(r>=9.2233720368547758e18 || r<-9.2233720368547758e18) mp_raise_t(E_OverflowError,"int too large (ints are 64-bit)");
    return v_int((int64_t)r);
}
static Value ma_floor(int argc, Value *argv, TupleObj *kw){ mod_no_kw("floor",kw); mod_nargs("floor",argc,1,1); if(IS_INTLIKE(argv[0])) return v_int(argv[0].u.i);
    if(TYPE(argv[0])->flags&TF_DUNDERS){ Value m=mp_type_lookup_s(TYPE(argv[0]),"__floor__"); if(m.k!=V_UNDEF) return mp_call1(m,argv[0]); } return int_result(floor(farg(argv[0]))); }
static Value ma_ceil(int argc, Value *argv, TupleObj *kw){ mod_no_kw("ceil",kw); mod_nargs("ceil",argc,1,1); if(IS_INTLIKE(argv[0])) return v_int(argv[0].u.i);
    if(TYPE(argv[0])->flags&TF_DUNDERS){ Value m=mp_type_lookup_s(TYPE(argv[0]),"__ceil__"); if(m.k!=V_UNDEF) return mp_call1(m,argv[0]); } return int_result(ceil(farg(argv[0]))); }
static Value ma_trunc(int argc, Value *argv, TupleObj *kw){ mod_no_kw("trunc",kw); mod_nargs("trunc",argc,1,1); if(IS_INTLIKE(argv[0])) return v_int(argv[0].u.i);
    if(TYPE(argv[0])->flags&TF_DUNDERS){ Value m=mp_type_lookup_s(TYPE(argv[0]),"__trunc__"); if(m.k!=V_UNDEF) return mp_call1(m,argv[0]); } return int_result(trunc(farg(argv[0]))); }
static int64_t gcd2(int64_t a, int64_t b){ if(a<0) a=-a; if(b<0) b=-b; while(b){ int64_t t=a%b; a=b; b=t; } return a; }
static Value ma_gcd(int argc, Value *argv, TupleObj *kw){ mod_no_kw("gcd",kw); int64_t g=0; for(int i=0;i<argc;i++) g=gcd2(g,mp_index(argv[i],"gcd")); return v_int(g); }
static Value ma_lcm(int argc, Value *argv, TupleObj *kw){ mod_no_kw("lcm",kw); int64_t l=1; if(!argc) return v_int(1);
    for(int i=0;i<argc;i++){ int64_t x=mp_index(argv[i],"lcm"); if(x==0) return v_int(0); if(x<0) x=-x; l=mp_int_checked('*',l/gcd2(l,x),x); } return v_int(l); }
static Value ma_isqrt(int argc, Value *argv, TupleObj *kw){ mod_no_kw("isqrt",kw); mod_nargs("isqrt",argc,1,1); int64_t n=mp_index(argv[0],"isqrt");
    if(n<0) mp_raise_t(E_ValueError,"isqrt() argument must be nonnegative");
    int64_t r=(int64_t)sqrt((double)n); while(r*r>n) r--; while((r+1)*(r+1)<=n) r++; return v_int(r); }
static Value ma_factorial(int argc, Value *argv, TupleObj *kw){ mod_no_kw("factorial",kw); mod_nargs("factorial",argc,1,1); int64_t n=mp_index(argv[0],"factorial");
    if(n<0) mp_raise_t(E_ValueError,"factorial() not defined for negative values"); int64_t r=1; for(int64_t i=2;i<=n;i++) r=mp_int_checked('*',r,i); return v_int(r); }
static Value ma_comb(int argc, Value *argv, TupleObj *kw){ mod_no_kw("comb",kw); mod_nargs("comb",argc,2,2); int64_t n=mp_index(argv[0],"comb"), k=mp_index(argv[1],"comb");
    if(n<0||k<0) mp_raise_t(E_ValueError,"n must be a non-negative integer"); if(k>n) return v_int(0); if(k>n-k) k=n-k;
    int64_t r=1; for(int64_t i=1;i<=k;i++){ r=mp_int_checked('*',r,n-k+i)/i; } return v_int(r); }
static Value ma_perm(int argc, Value *argv, TupleObj *kw){ mod_no_kw("perm",kw); mod_nargs("perm",argc,1,2); int64_t n=mp_index(argv[0],"perm"), k= argc>1 && !IS_NONE(argv[1]) ? mp_index(argv[1],"perm") : n;
    if(n<0||k<0) mp_raise_t(E_ValueError,"n must be a non-negative integer"); if(k>n) return v_int(0); int64_t r=1; for(int64_t i=0;i<k;i++) r=mp_int_checked('*',r,n-i); return v_int(r); }
static Value ma_prod(int argc, Value *argv, TupleObj *kw){ mod_nargs("prod",mod_npos(argc,kw),1,1); Value acc=mod_kwarg(argc,argv,kw,"start",v_int(1)); Value it=mp_iter(argv[0]), x; while(mp_next(it,&x)) acc=mp_binop(OP_Mult,acc,x); return acc; }
static Value ma_fsum(int argc, Value *argv, TupleObj *kw){
    mod_no_kw("fsum",kw); mod_nargs("fsum",argc,1,1);
    /* Shewchuk's exact summation, as CPython's math.fsum */
    double p[64]; int n=0; double special=0; int inf_nan=0;
    Value it=mp_iter(argv[0]), v;
    while(mp_next(it,&v)){
        double x=farg(v);
        if(!isfinite(x)){ inf_nan=1; special+=x; continue; }
        int i=0;
        for(int j=0;j<n;j++){ double y=p[j]; if(fabs(x)<fabs(y)){ double t=x; x=y; y=t; } double hi=x+y, lo=y-(hi-x); if(lo!=0) p[i++]=lo; x=hi; }
        n=i; if(n<64) p[n++]=x;
    }
    if(inf_nan) return v_float(special);
    double hi=0;
    if(n>0){
        hi=p[--n];
        while(n>0){ double x=hi, y=p[--n]; hi=x+y; double yr=hi-x, lo=y-yr; if(lo!=0){
            if(n>0 && ((lo<0 && p[n-1]<0) || (lo>0 && p[n-1]>0))){ y=lo*2; x=hi+y; yr=x-hi; if(y==yr) hi=x; }
            break; } }
    }
    return v_float(hi);
}
static Value ma_isfinite(int argc, Value *argv, TupleObj *kw){ mod_no_kw("isfinite",kw); mod_nargs("isfinite",argc,1,1); return v_bool(isfinite(farg(argv[0]))); }
static Value ma_isinf(int argc, Value *argv, TupleObj *kw){ mod_no_kw("isinf",kw); mod_nargs("isinf",argc,1,1); return v_bool(isinf(farg(argv[0]))); }
static Value ma_isnan(int argc, Value *argv, TupleObj *kw){ mod_no_kw("isnan",kw); mod_nargs("isnan",argc,1,1); return v_bool(isnan(farg(argv[0]))); }
static Value ma_isclose(int argc, Value *argv, TupleObj *kw){
    mod_nargs("isclose",mod_npos(argc,kw),2,2);
    double a=farg(argv[0]), b=farg(argv[1]);
    double rt=farg(mod_kwarg(argc,argv,kw,"rel_tol",v_float(1e-9))), at=farg(mod_kwarg(argc,argv,kw,"abs_tol",v_float(0)));
    if(a==b) return v_bool(1);
    if(isinf(a)||isinf(b)) return v_bool(0);
    double d=fabs(b-a);
    return v_bool(d<=fabs(rt*b) || d<=fabs(rt*a) || d<=at);
}
static Value ma_dist(int argc, Value *argv, TupleObj *kw){ mod_no_kw("dist",kw); mod_nargs("dist",argc,2,2); Value p=mp_list_of(argv[0]), q=mp_list_of(argv[1]);
    if(AS_LIST(p)->len!=AS_LIST(q)->len) mp_raise_t(E_ValueError,"both points must have the same number of dimensions");
    double s=0; for(int64_t i=0;i<AS_LIST(p)->len;i++){ double d=farg(AS_LIST(p)->items[i])-farg(AS_LIST(q)->items[i]); s+=d*d; } return v_float(sqrt(s)); }
Value make_math(void){
    Value m=register_module("math");
    mp_dict_set_s(mdict(m),"pi",v_float(M_PI)); mp_dict_set_s(mdict(m),"e",v_float(M_E)); mp_dict_set_s(mdict(m),"tau",v_float(2*M_PI));
    mp_dict_set_s(mdict(m),"inf",v_float(INFINITY)); mp_dict_set_s(mdict(m),"nan",v_float(NAN));
    struct { const char *n; NFn f; } fs[]={{"sqrt",ma_sqrt},{"log",ma_log},{"log10",ma_log10},{"log2",ma_log2},{"log1p",ma_log1p},{"exp",ma_exp},{"expm1",ma_expm1},
        {"sin",ma_sin},{"cos",ma_cos},{"tan",ma_tan},{"asin",ma_asin},{"acos",ma_acos},{"atan",ma_atan},{"atan2",ma_atan2},{"sinh",ma_sinh},{"cosh",ma_cosh},
        {"tanh",ma_tanh},{"asinh",ma_asinh},{"acosh",ma_acosh},{"atanh",ma_atanh},{"fabs",ma_fabs},{"pow",ma_pow},{"fmod",ma_fmod},{"remainder",ma_remainder},
        {"copysign",ma_copysign},{"ldexp",ma_ldexp},{"frexp",ma_frexp},{"modf",ma_modf},{"hypot",ma_hypot},{"floor",ma_floor},{"ceil",ma_ceil},{"trunc",ma_trunc},
        {"gcd",ma_gcd},{"lcm",ma_lcm},{"isqrt",ma_isqrt},{"factorial",ma_factorial},{"comb",ma_comb},{"perm",ma_perm},{"prod",ma_prod},{"fsum",ma_fsum},
        {"isfinite",ma_isfinite},{"isinf",ma_isinf},{"isnan",ma_isnan},{"isclose",ma_isclose},{"degrees",ma_degrees},{"radians",ma_radians},{"erf",ma_erf},
        {"erfc",ma_erfc},{"cbrt",ma_cbrt},{"exp2",ma_exp2},{"gamma",ma_gamma},{"lgamma",ma_lgamma},{"dist",ma_dist},
        {"nextafter",ma_nextafter},{"ulp",ma_ulp},{"fma",ma_fma},{"sumprod",ma_sumprod},{NULL,NULL}};
    for(int i=0;fs[i].n;i++) set_fn(m,fs[i].n,fs[i].f);
    return m;
}

/* ---------------------------------------------------------------- time */
Value ti_sleep(int argc, Value *argv, TupleObj *kw){
    mod_no_kw("sleep",kw); mod_nargs("sleep",argc,1,1);
    double s=farg(argv[0]);
    if(s<0) mp_raise_t(E_ValueError,"sleep length must be non-negative");
    Thread *self=mp_ts; mp_flush_stdout(); mp_gil_release();
    ppy_thread_sleep_ms((int)(s*1000+0.5));
    mp_gil_acquire(self);
    return v_none();
}

