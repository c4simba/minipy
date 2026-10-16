/* The exact float conversions of the C translation (src/aot/x2c_float.c)
   against the C library's (which are exact too, on glibc and macOS):
   %.*e digits, %.*f, and strtod on random and edge-case values.

     cc -O2 -Isrc tests/port/float_check.c -o build/float_check -lm && build/float_check [N]

   (make test-port runs it.) Prints the first mismatches and a summary;
   exit status 1 when there is any. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64; typedef int32_t i32;
static u8 *m_unused;
#define D(a) (*(u32*)(m_unused+(u32)(a)))
#include "aot/x2c_float.c"

static u64 rng=0x9E3779B97F4A7C15ull;
static u64 rnd(void){ rng^=rng<<13; rng^=rng>>7; rng^=rng<<17; return rng; }
static double bits(u64 u){ double d; memcpy(&d,&u,8); return d; }
static u64 ubits(double d){ u64 u; memcpy(&u,&d,8); return u; }

static int fails=0;
static void fail(const char *what, double v, const char *got, const char *want){
    if(fails++<20) fprintf(stderr,"MISMATCH %s v=%a (%.17g): got '%s' want '%s'\n",what,v,v,got,want);
}

/* my %.*e from x2c_dtoa_all + x2c_round_digits */
static void my_e(char *out, double v, int P){
    char *o=out; if(v<0){ *o++='-'; v=-v; }
    if(v==0){ sprintf(o,"%.*e",P-1,0.0); return; }
    int e, nd=x2c_dtoa_all(v,&e); if(x2c_round_digits(x2c_alldig,nd,P)) e++;
    *o++=x2c_alldig[0]; if(P>1){ *o++='.'; for(int i=1;i<P;i++) *o++=x2c_alldig[i]; }
    sprintf(o,"e%c%02d",e<0?'-':'+',e<0?-e:e);
}
static void check_e(double v){
    char a[64], b[64];
    for(int P=1;P<=17;P++){ my_e(a,v,P); snprintf(b,sizeof b,"%.*e",P-1,v); if(strcmp(a,b)){ fail("%e",v,a,b); return; } }
}
static void check_f(double v){
    char a[512], b[512];
    for(int d=0;d<=20;d++){
        if(fabs(v)*x2c_p10[d]>=2305843009213693952.0) break;
        x2c_fmt_fixed(a,v,(u32)d); snprintf(b,sizeof b,"%.*f",d,v);
        if(strcmp(a,b)){ fail("%f",v,a,b); return; }
    }
}
static void check_s(const char *s){
    const char *p=s; int neg=0; if(*p=='-'){ neg=1; p++; }
    double a=x2c_strtod((const u8*)p,(const u8*)p+strlen(p)); if(neg) a=-a;
    double b=strtod(s,NULL);
    if(ubits(a)!=ubits(b)){ char ga[64], wb[64]; snprintf(ga,64,"%a",a); snprintf(wb,64,"%a",b); fail(s,b,ga,wb); }
}

/* lines "text<TAB>hex bits": strings with the double they must give
   (tests/port/midpoints.py writes exact midpoints between neighbours) */
static int check_file(const char *path){
    FILE *f=fopen(path,"r"); if(!f){ perror(path); return 1; }
    char line[4096]; long n=0;
    while(fgets(line,sizeof line,f)){
        char *tab=strchr(line,'\t'); if(!tab) continue; *tab=0;
        u64 want=strtoull(tab+1,NULL,16);
        const char *p=line; int neg=0; if(*p=='-'){ neg=1; p++; }
        double a=x2c_strtod((const u8*)p,(const u8*)p+strlen(p)); if(neg) a=-a;
        if(ubits(a)!=want){ char ga[64], wb[64]; snprintf(ga,64,"%a",a); snprintf(wb,64,"%a",bits(want)); fail(line,bits(want),ga,wb); }
        n++;
    }
    fclose(f);
    printf("float strings of %s: %ld, %d mismatches\n",path,n,fails);
    return fails?1:0;
}

int main(int argc, char **argv){
    if(argc>2 && !strcmp(argv[1],"--strings")) return check_file(argv[2]);
    long N=argc>1?atol(argv[1]):300000;
    static const double edge[]={0.1,0.2,0.3,1.0/3,2.0/3,1e-7,1e16,1e17,1e22,1e23,123456789012345678.0,5e-324,1e-323,2.2250738585072009e-308,
        2.2250738585072014e-308,1.7976931348623157e308,0.5,1.5,2.5,0.25,0.125,0.05,0.15,0.35,9.995,99.5,0.0049999999,1e-5,0.000123456,
        3.141592653589793,2.718281828459045,6.02214076e23,1.602176634e-19,4.35,1e300,1e-300,65536.0,4294967296.0,9007199254740993.0};
    for(size_t i=0;i<sizeof edge/sizeof edge[0];i++){ check_e(edge[i]); check_e(-edge[i]); check_f(edge[i]); check_f(-edge[i]);
        char s[64]; for(int P=1;P<=17;P++){ snprintf(s,64,"%.*g",P,edge[i]); check_s(s); } }
    for(long i=0;i<N;i++){
        u64 u=rnd();
        double v=bits(u);
        if(isnan(v)||isinf(v)) continue;
        check_e(v);
        double w=bits((u&0x800FFFFFFFFFFFFFull) | ((u64)(1023+(int)(rnd()%80)-40)<<52));   /* moderate exponents: %f */
        check_f(w);
        char s[1200];
        snprintf(s,sizeof s,"%.17g",v); check_s(s);
        snprintf(s,sizeof s,"%.*g",(int)(rnd()%17)+1,v); check_s(s);
        if(i%16==0){                                    /* the exact midpoint of two neighbours: ties round to even */
            u64 au=ubits(v)&0x7FFFFFFFFFFFFFFFull; if(au>=0x7FEFFFFFFFFFFFFFull) continue;
            double lo=bits(au), hi=bits(au+1);
            long double mid=((long double)lo+(long double)hi)/2;   /* exact where long double has 64 bits; else skipped */
            if(sizeof(long double)>8){ snprintf(s,sizeof s,"%.900Le",mid); check_s(s); }
            snprintf(s,sizeof s,"%.40e",lo); check_s(s);
            snprintf(s,sizeof s,"%.780e",lo); check_s(s);
        }
        if(i%8==0){                                     /* random digit strings */
            int nd=(int)(rnd()%40)+1, k=0; s[k++]=(char)('1'+rnd()%9);
            for(int j=1;j<nd;j++) s[k++]=(char)('0'+rnd()%10);
            if(rnd()%2){ int dot=(int)(rnd()%nd); memmove(s+dot+1,s+dot,(size_t)(k-dot)); s[dot]='.'; k++; }
            k+=snprintf(s+k,64,"e%d",(int)(rnd()%700)-350); s[k]=0;
            check_s(s);
        }
    }
    /* the subnormal and overflow edges */
    { const char *t[]={"4.9406564584124654e-324","2.4703282292062327e-324","2.4703282292062328e-324","2.4703282292062327208828439643411068618252990130716238221279284125033775364e-324",
        "1.7976931348623158e308","1.7976931348623159e308","1e309","1e-400","0.000","0","1e0","123456789e-30","2.2250738585072011e-308",NULL};
      for(int i=0;t[i];i++) check_s(t[i]); }
    printf("float conversions: %ld random values, %d mismatches\n",N,fails);
    return fails?1:0;
}
