/* ========================= Typed compiler: runtime of the C translation =========================
   minipy --compile --target macos turns the i386 listing into C (aot_x2c.c);
   this file is the start of every such C program (minipy carries it as a
   string). The program keeps running as the i386 code it was: 32-bit
   registers and flags, the x87 stack (in double precision, which compiled
   code sets anyway), and a 4 GiB guest address space, guest address a at
   host address M + a:

     0x00100000  code: addresses of the labels jumped to indirectly (return
                 addresses, function values), 4 bytes apart, nothing behind them
     0x08000000  the listing's data and bss, then the brk heap
     0x40000000  mmap2 (the allocator's pages, task stacks)
     0xB8000000  the stack (grows down from 0xC0000000; its lowest 64 KiB a guard)
     0xE0000000  copies of the C strings C functions return
     0xF0000000  handles: pointers C functions return that are outside M

   Linux system calls (int 0x80) run on the host's, C functions (ctypes) are
   called natively with the signature the listing gives each call, and the
   few runtime routines that compute in the x87's 64-bit precision (float
   formatting and parsing, exp, pow) are done here in C. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/stat.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int32_t i32;
typedef uint64_t u64; typedef int64_t i64; typedef int16_t i16; typedef int8_t i8;
typedef u16 u16u __attribute__((aligned(1),may_alias));
typedef u32 u32u __attribute__((aligned(1),may_alias));
typedef u64 u64u __attribute__((aligned(1),may_alias));
typedef double f64u __attribute__((aligned(1),may_alias));
typedef float f32u __attribute__((aligned(1),may_alias));

#define X2C_CODE_BASE   0x00100000u
#define X2C_DATA_BASE   0x08000000u
#define X2C_MMAP_BASE   0x40000000u
#define X2C_MMAP_END    0xB8000000u
#define X2C_STACK_TOP   0xC0000000u
#define X2C_STRS        0xE0000000u
#define X2C_STRS_SIZE   0x00100000u
#define X2C_HANDLES     0xF0000000u

/* guest memory of the translated code: m is a local copy of M */
#define B(a)  (*(u8*)(m+(u32)(a)))
#define W(a)  (*(u16u*)(m+(u32)(a)))
#define D(a)  (*(u32u*)(m+(u32)(a)))
#define Q(a)  (*(u64u*)(m+(u32)(a)))
#define FD(a) (*(f64u*)(m+(u32)(a)))
#define FS(a) (*(f32u*)(m+(u32)(a)))

/* partial registers */
#define SET8L(r,v)  (r=((r)&0xFFFFFF00u)|(u8)(v))
#define SET8H(r,v)  (r=((r)&0xFFFF00FFu)|((u32)(u8)(v)<<8))
#define SET16(r,v)  (r=((r)&0xFFFF0000u)|(u16)(v))

/* Flags are kept lazily: the kind of the last instruction that set them
   (fk; bits 8+: how far 8/16-bit operands are shifted up so that their sign
   is bit 31), its operands fa fb, result fr and a carry fc. */
enum { FK_ADD, FK_SUB, FK_LOGIC, FK_INC, FK_DEC, FK_SHIFT, FK_MUL, FK_SAHF, FK_ADC, FK_SBB };
enum { CC_O, CC_NO, CC_B, CC_AE, CC_E, CC_NE, CC_BE, CC_A, CC_S, CC_NS, CC_P, CC_NP, CC_L, CC_GE, CC_LE, CC_G };
#define X2C_INLINE static inline __attribute__((always_inline))
X2C_INLINE int x2c_cf(u32 k, u32 a, u32 b, u32 r, u32 c){
    switch(k&15){ case FK_ADD: return r<a; case FK_SUB: return a<b; case FK_LOGIC: return 0; case FK_SAHF: return r&1; default: return c&1; }   /* adc, sbb: c */
}
X2C_INLINE int x2c_of(u32 k, u32 a, u32 b, u32 r, u32 c){
    switch(k&15){ case FK_ADD: case FK_INC: case FK_ADC: return (~(a^b)&(a^r))>>31; case FK_SUB: case FK_DEC: case FK_SBB: return ((a^b)&(a^r))>>31;
                  case FK_MUL: return c&1; default: return 0; }
}
X2C_INLINE int x2c_zf(u32 k, u32 r){ return (k&15)==FK_SAHF ? (r>>6)&1 : r==0; }
X2C_INLINE int x2c_sf(u32 k, u32 r){ return (k&15)==FK_SAHF ? (r>>7)&1 : r>>31; }
X2C_INLINE int x2c_pf(u32 k, u32 r){ return (k&15)==FK_SAHF ? (r>>2)&1 : !__builtin_parity((r>>(k>>8))&255); }
X2C_INLINE int x2c_cc(int cc, u32 k, u32 a, u32 b, u32 r, u32 c){
    if((k&15)==FK_SUB) switch(cc){                      /* cmp: straight comparisons */
        case CC_B: return a<b; case CC_AE: return a>=b; case CC_E: return a==b; case CC_NE: return a!=b;
        case CC_BE: return a<=b; case CC_A: return a>b; case CC_L: return (i32)a<(i32)b; case CC_GE: return (i32)a>=(i32)b;
        case CC_LE: return (i32)a<=(i32)b; case CC_G: return (i32)a>(i32)b; default: break; }
    if((k&15)==FK_LOGIC) switch(cc){                    /* test, and, or, xor */
        case CC_B: return 0; case CC_AE: return 1; case CC_E: case CC_BE: return r==0; case CC_NE: case CC_A: return r!=0;
        case CC_L: case CC_S: return (i32)r<0; case CC_GE: case CC_NS: return (i32)r>=0; case CC_LE: return (i32)r<=0; case CC_G: return (i32)r>0;
        case CC_O: return 0; case CC_NO: return 1; default: break; }
    int cf=x2c_cf(k,a,b,r,c), zf=x2c_zf(k,r), sf=x2c_sf(k,r), of=x2c_of(k,a,b,r,c);
    switch(cc){
        case CC_O: return of; case CC_NO: return !of; case CC_B: return cf; case CC_AE: return !cf;
        case CC_E: return zf; case CC_NE: return !zf; case CC_BE: return cf||zf; case CC_A: return !cf&&!zf;
        case CC_S: return sf; case CC_NS: return !sf; case CC_P: return x2c_pf(k,r); case CC_NP: return !x2c_pf(k,r);
        case CC_L: return sf!=of; case CC_GE: return sf==of; case CC_LE: return zf||sf!=of; default: return !zf&&sf==of;
    }
}
#define CC(cc) x2c_cc(cc,fk,fa,fb,fr,fc)
#define CF_NOW x2c_cf(fk,fa,fb,fr,fc)

/* x87: st(i) = fs[(ft+i)&7] */
#define ST(i)    fs[(ft+(i))&7]
#define FPUSH(v) do{ double fpush_=(v); ft=(ft-1)&7; fs[ft]=fpush_; }while(0)
#define FPOP()   (ft=(ft+1)&7)
X2C_INLINE u32 x2c_fcmp(double a, double b){ return a!=a||b!=b ? 0x4500u : a<b ? 0x0100u : a==b ? 0x4000u : 0u; }
#define FCOM(a,b) (fsw=(fsw&~0x4700u)|x2c_fcmp(a,b))
X2C_INLINE double x2c_round(double v, u32 cw){
    switch((cw>>10)&3){ case 0: return nearbyint(v); case 1: return floor(v); case 2: return ceil(v); default: return trunc(v); }
}
X2C_INLINE u32 x2c_fist32(double v, u32 cw){ double r=x2c_round(v,cw); return r>=-2147483648.0 && r<2147483648.0 ? (u32)(i32)r : 0x80000000u; }
X2C_INLINE u16 x2c_fist16(double v, u32 cw){ double r=x2c_round(v,cw); return r>=-32768.0 && r<32768.0 ? (u16)(i16)r : 0x8000u; }
X2C_INLINE u64 x2c_fist64(double v, u32 cw){ double r=x2c_round(v,cw); return r>=-9223372036854775808.0 && r<9223372036854775808.0 ? (u64)(i64)r : 0x8000000000000000ull; }
/* fyl2x: st1 * log2(st0); with the constants compiled code loads for ln and
   log10, the library's functions (the x87 computes those exactly) */
static double x2c_fyl2x(double y, double x){
    if(y==0.6931471805599453) return log(x);
    if(y==0.30102999566398120) return log10(x);
    if(y==1.0) return log2(x);
    return y*log2(x);
}

/* ---- the guest address space */
static u8 *M;
static u32 x2c_brk_start, x2c_brk;
static int x2c_errno;                                   /* errno after the last C function */
static int x2c_argc; static char **x2c_argv;

static void x2c_fatal(const char *fmt, ...){
    va_list ap; va_start(ap,fmt); fprintf(stderr,"minipy (native code): "); vfprintf(stderr,fmt,ap); fprintf(stderr,"\n"); va_end(ap);
    exit(70);
}
static void x2c_bad_jump(u32 a){ x2c_fatal("jump to 0x%08x, not a code address",a); }

static void x2c_map(void){
    void *p=mmap(NULL,(size_t)1<<32,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);
    if(p==MAP_FAILED) x2c_fatal("cannot reserve the 4 GiB address space");
    M=(u8*)p;
    if(mprotect(M+X2C_DATA_BASE,(size_t)(X2C_HANDLES-X2C_DATA_BASE),PROT_READ|PROT_WRITE)) x2c_fatal("cannot map the guest memory");
    mprotect(M+X2C_MMAP_END,0x10000,PROT_NONE);        /* below the stack: an overflow faults */
}

/* mmap2 / munmap of guest pages: first fit in [X2C_MMAP_BASE, X2C_MMAP_END) */
typedef struct { u32 a, n; } X2cRange;
static X2cRange *x2c_free; static int x2c_nfree, x2c_capfree;
static void x2c_zero(u32 a, u32 n){                     /* back to fresh zero pages */
    uintptr_t pg=(uintptr_t)getpagesize(), lo=((uintptr_t)(M+a)+pg-1)&~(pg-1), hi=((uintptr_t)(M+a)+n)&~(pg-1);
    if(hi>lo){
        memset(M+a,0,(size_t)(lo-(uintptr_t)(M+a)));
        if(mmap((void*)lo,hi-lo,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0)==MAP_FAILED) memset((void*)lo,0,hi-lo);
        memset((void*)hi,0,(size_t)((uintptr_t)(M+a)+n-hi));
    } else memset(M+a,0,n);
}
static u32 x2c_mmap(u32 n){
    n=(n+4095u)&~4095u;
    if(!n) return (u32)-22;
    if(!x2c_capfree){ x2c_capfree=16; x2c_free=(X2cRange*)malloc(sizeof(X2cRange)*16); x2c_free[0].a=X2C_MMAP_BASE; x2c_free[0].n=X2C_MMAP_END-X2C_MMAP_BASE; x2c_nfree=1; }
    for(int i=0;i<x2c_nfree;i++) if(x2c_free[i].n>=n){
        u32 a=x2c_free[i].a; x2c_free[i].a+=n; x2c_free[i].n-=n;
        if(!x2c_free[i].n){ memmove(x2c_free+i,x2c_free+i+1,sizeof(X2cRange)*(size_t)(x2c_nfree-i-1)); x2c_nfree--; }
        return a;
    }
    return (u32)-12;                                    /* ENOMEM */
}
static u32 x2c_munmap(u32 a, u32 n){
    n=(n+4095u)&~4095u;
    if(a<X2C_MMAP_BASE || a+n>X2C_MMAP_END || (a&4095u)) return (u32)-22;
    x2c_zero(a,n);
    int i=0; while(i<x2c_nfree && x2c_free[i].a<a) i++;
    if(x2c_nfree==x2c_capfree){ x2c_capfree*=2; x2c_free=(X2cRange*)realloc(x2c_free,sizeof(X2cRange)*(size_t)x2c_capfree); }
    memmove(x2c_free+i+1,x2c_free+i,sizeof(X2cRange)*(size_t)(x2c_nfree-i)); x2c_nfree++;
    x2c_free[i].a=a; x2c_free[i].n=n;
    if(i+1<x2c_nfree && x2c_free[i].a+x2c_free[i].n==x2c_free[i+1].a){ x2c_free[i].n+=x2c_free[i+1].n; memmove(x2c_free+i+1,x2c_free+i+2,sizeof(X2cRange)*(size_t)(x2c_nfree-i-2)); x2c_nfree--; }
    if(i>0 && x2c_free[i-1].a+x2c_free[i-1].n==x2c_free[i].a){ x2c_free[i-1].n+=x2c_free[i].n; memmove(x2c_free+i,x2c_free+i+1,sizeof(X2cRange)*(size_t)(x2c_nfree-i-1)); x2c_nfree--; }
    return 0;
}

/* ---- pointers to and from C functions */
static void **x2c_handles; static u32 x2c_nhandles;
static void *x2c_ptr(u32 a){
    if(!a) return NULL;
    if(a>=X2C_HANDLES){ u32 i=(a-X2C_HANDLES)>>4; if(i<x2c_nhandles) return (char*)x2c_handles[i]+(a&15u); }
    return M+a;
}
static u32 x2c_unptr(void *p){
    if(!p) return 0;
    if((u8*)p>=M && (u8*)p<M+X2C_HANDLES) return (u32)((u8*)p-M);
    for(u32 i=0;i<x2c_nhandles;i++) if(x2c_handles[i]==p) return X2C_HANDLES+16*i;
    if((x2c_nhandles&(x2c_nhandles-1))==0) x2c_handles=(void**)realloc(x2c_handles,sizeof(void*)*(x2c_nhandles?2*x2c_nhandles:16));
    x2c_handles[x2c_nhandles]=p;
    return X2C_HANDLES+16*x2c_nhandles++;
}
static u32 x2c_strs_at;
static u32 x2c_unstr(const char *s){                    /* a char * result: guest memory, or a copy there */
    if(!s) return 0;
    if((const u8*)s>=M && (const u8*)s<M+X2C_HANDLES) return (u32)((const u8*)s-M);
    size_t n=strlen(s)+1;
    if(n>X2C_STRS_SIZE/2) n=X2C_STRS_SIZE/2;
    if(x2c_strs_at+n>X2C_STRS_SIZE) x2c_strs_at=0;
    u32 a=X2C_STRS+x2c_strs_at; x2c_strs_at+=(u32)((n+15)&~(size_t)15);
    memcpy(M+a,s,n); M[a+n-1]=0;
    return a;
}

/* ---- C functions (ctypes): looked up when first called */
typedef struct { const char *lib, *sym; void *fn; } X2cImport;
static void *x2c_lib(const char *so){
    static const char *libc[]={"libc.so.6","libm.so.6","libpthread.so.0","libdl.so.2","librt.so.1","libc.so","libm.so",NULL};
    for(int i=0;libc[i];i++) if(!strcmp(so,libc[i])) return RTLD_DEFAULT;
    void *h=dlopen(so,RTLD_NOW|RTLD_GLOBAL);
    if(!h){                                             /* libfoo.so.N -> libfoo.dylib */
        char name[512]; const char *dot=strstr(so,".so");
        if(dot && dot-so<480){ snprintf(name,sizeof name,"%.*s.dylib",(int)(dot-so),so); h=dlopen(name,RTLD_NOW|RTLD_GLOBAL); }
    }
    if(!h) x2c_fatal("cannot load the C library %s: %s",so,dlerror());
    return h;
}
static void *x2c_errno_cell(void);
static void *x2c_resolve(X2cImport *im){
    if(!strcmp(im->sym,"__errno_location")){ im->fn=(void*)x2c_errno_cell; return im->fn; }
    void *f=dlsym(x2c_lib(im->lib),im->sym);
    if(!f) x2c_fatal("C function %s not found in %s",im->sym,im->lib);
    im->fn=f; return f;
}
#define CFN(k) (x2c_imports[k].fn ? x2c_imports[k].fn : x2c_resolve(&x2c_imports[k]))

/* ---- Linux system calls (i386 numbers and structures) */
typedef struct { u32 eax, ebx, ecx, edx, esi, edi, ebp; } X2cRegs;
static u32 x2c_err(void){ return (u32)-errno; }
#define LX_WORD u32
#define LX_PTR(a) x2c_ptr((u32)(a))
#define LX_BLOCKING_BEGIN ((void)0)
#define LX_BLOCKING_END ((void)0)
#define LX_FLUSH() ((void)0)
#include "lx_emul.c"
static u32 x2c_syscall(u8 *m, X2cRegs *r){
    switch(r->eax){
        case 45:{                                       /* brk */
            u32 want=r->ebx;
            if(want>=x2c_brk_start && want<X2C_MMAP_BASE){ if(want<x2c_brk) memset(M+want,0,x2c_brk-want); x2c_brk=want; }
            return x2c_brk; }
        case 91: return x2c_munmap(r->ebx,r->ecx);
        case 192:                                       /* mmap2: anonymous memory only */
            if(!(r->esi&0x20)) return (u32)-19;         /* ENODEV */
            return x2c_mmap(r->ecx);
    }
    u32 regs[7]={r->eax,r->ebx,r->ecx,r->edx,r->esi,r->edi,r->ebp};
    int64_t v=lx_syscall(regs);
    if(v==-ENOSYS) fprintf(stderr,"minipy (native code): Linux system call %u is not supported\n",r->eax);
    (void)m;
    return (u32)v;
}

/* ---- the runtime routines done natively (they compute in 64-bit precision on the x87) */
/* rt_sb_gen: edx significant digits (1..17), flags: 1 ".0" after an integer,
   2 fixed notation up to 1e16, 4 scientific keeping trailing zeros; else %g */
static u32 x2c_fmt_gen(char *out, double v, u32 P, u32 flags){
    char *o=out;
    if(P<1) P=1;
    if(P>17) P=17;
    if(isnan(v)){ strcpy(o,"nan"); return 3; }
    if(isinf(v)){ strcpy(o,v<0?"-inf":"inf"); return (u32)strlen(o); }
    if(v==0){ if(signbit(v)) *o++='-'; *o++='0'; if(flags&1){ *o++='.'; *o++='0'; } *o=0; return (u32)(o-out); }
    if(v<0){ *o++='-'; v=-v; }
    char t[64]; snprintf(t,sizeof t,"%.*e",(int)P-1,v);   /* d.ddde[+-]x: the digits and the exponent */
    char dig[24]; int nd=0; const char *p=t;
    for(;*p && *p!='e';p++) if(*p>='0'&&*p<='9') dig[nd++]=*p;
    int e=atoi(p+1), n=nd;
    if(flags&8){ int i=nd-1; while(i>=0 && dig[i]=='9') dig[i--]='0'; if(i<0){ dig[0]='1'; e++; } else dig[i]++; }   /* one unit more in the last digit */
    if(!(flags&4)) while(n>1 && dig[n-1]=='0') n--;
    int lim=(flags&2)?16:(int)P, dotzero=0;
    if((flags&4) || e<-4 || e>=lim){                    /* d[.ddd]e+XX */
        *o++=dig[0];
        if(n>1){ *o++='.'; memcpy(o,dig+1,(size_t)(n-1)); o+=n-1; }
        *o++='e'; *o++=e<0?'-':'+'; if(e<0) e=-e;
        if(e<10) *o++='0';
        o+=sprintf(o,"%d",e);
    } else if(e>=0){
        int id=e+1;
        if(id>=n){ memcpy(o,dig,(size_t)n); o+=n; for(int i=n;i<id;i++) *o++='0'; dotzero=1; }
        else { memcpy(o,dig,(size_t)id); o+=id; *o++='.'; memcpy(o,dig+id,(size_t)(n-id)); o+=n-id; }
    } else {                                            /* 0.000ddd */
        *o++='0'; *o++='.'; for(int i=0;i<-e-1;i++) *o++='0';
        memcpy(o,dig,(size_t)n); o+=n;
    }
    if(dotzero && (flags&1)){ *o++='.'; *o++='0'; }
    *o=0; return (u32)(o-out);
}
/* rt_sb_fixed: printf %.*f (edx digits after the point); from 2^61 on, %.17g */
static u32 x2c_fmt_fixed(char *out, double v, u32 d){
    if(!isfinite(v)) return x2c_fmt_gen(out,v,17,0);
    char *o=out;
    if(v<0){ *o++='-'; v=-v; }
    if(d>40) d=40;
    if(v*pow(10.0,(double)d)>=2305843009213693952.0) return (u32)(o-out)+x2c_fmt_gen(o,v,17,0);
    return (u32)(o-out)+(u32)sprintf(o,"%.*f",(int)d,v);
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
        char w[4]={(char)(p[0]|32),(char)(p[1]|32),(char)(p[2]|32),0};
        if(!strcmp(w,"inf")){ v=INFINITY; p+=3;
            if(end-p>=5 && (p[0]|32)=='i' && (p[1]|32)=='n' && (p[2]|32)=='i' && (p[3]|32)=='t' && (p[4]|32)=='y') p+=5; }
        else if(!strcmp(w,"nan")){ v=NAN; p+=3; }
        else return 0;
    } else {
        if(p<end && (*p|32)=='e'){
            p++;
            if(p>=end) return 0;
            if(*p=='-'||*p=='+') p++;
            while(p<end && *p>='0' && *p<='9') p++;
        }
        size_t len=(size_t)(p-num); char buf[512], *b=len<sizeof buf?buf:(char*)malloc(len+1);
        memcpy(b,num,len); b[len]=0; v=strtod(b,NULL);
        if(b!=buf) free(b);
    }
    while(p<end && x2c_is_space(*p)) p++;
    if(p<end) return 0;
    *out=neg?-v:v;
    return 1;
}

/* __errno_location: the errno of the last C function, in guest memory (ctypes.get_errno()) */
static void *x2c_errno_cell(void){ u32 a=X2C_STRS+X2C_STRS_SIZE; *(u32*)(M+a)=(u32)x2c_errno; return M+a; }

/* ---- start: Linux's initial stack (argc, argv, envp, auxv) */
extern char **environ;
static u32 x2c_stack(void){
    u32 sp=X2C_STACK_TOP-16;
    int envc=0; while(environ[envc]) envc++;
    u32 *ptrs=(u32*)malloc(sizeof(u32)*(size_t)(x2c_argc+envc+1));
    for(int i=0;i<x2c_argc+envc;i++){
        const char *s=i<x2c_argc?x2c_argv[i]:environ[i-x2c_argc];
        size_t n=strlen(s)+1;
        if(n>65536) n=65536;
        sp-=(u32)n; memcpy(M+sp,s,n); M[sp+n-1]=0; ptrs[i]=sp;
    }
    sp&=~15u;
    u32 words=1+(u32)x2c_argc+1+(u32)envc+1+2;
    sp-=4*words; sp&=~15u;
    u32 a=sp;
    *(u32*)(M+a)=(u32)x2c_argc; a+=4;
    for(int i=0;i<x2c_argc;i++){ *(u32*)(M+a)=ptrs[i]; a+=4; }
    *(u32*)(M+a)=0; a+=4;
    for(int i=0;i<envc;i++){ *(u32*)(M+a)=ptrs[x2c_argc+i]; a+=4; }
    *(u32*)(M+a)=0; a+=4; *(u32*)(M+a)=0; *(u32*)(M+a+4)=0;
    free(ptrs);
    return sp;
}
