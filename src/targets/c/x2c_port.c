/* ========================= Typed compiler: the C runtime of a program for a port =========================
   plainpy --compile --target c (and esp32) turns the i386 listing into C
   (aot/x2c.c); this file is the start of every such C program (plainpy
   carries it in plainpy.lib with the files it includes). The machine is
   aot/x2c_machine.c. Everything the program needs from outside comes through
   ppy_port.h, so the C file builds without a C library (-ffreestanding):

     memory     one arena from ppy_port_arena(); guest address a is at host
                address m + a, with m = arena - X2C_DATA_BASE:
                  X2C_DATA_BASE  the listing's data and bss
                  then           copies of the C strings C functions return
                  then           pages for mmap2 (the allocator's chunks,
                                 generator and task stacks), first fit
                  the top        the main stack (plainpy --stack)
     the system the Linux calls the runtime makes become ppy_port_write,
                read, clock, sleep, random; any other (files, sockets, the
                device calls of lib/machine.py) goes to ppy_port_syscall
     floats     text exactly (aot/x2c_float.c); fabs, floor, ceil, trunc,
                rounding, sqrt, fmod, ldexp exactly, here; sin, cos, tan,
                atan, asin, acos, atan2, exp, exp2, log, log10, log2, pow come
                from the C library's libm when the program uses them (link
                -lm, or give them)
     C functions (ctypes) are linked with the program: the C file names
                them as externs (CDLL's library name does not matter)
     exit       the program's exit makes ppy_main() return its status

   (The C compiler may still call memcpy/memset for its own copies; the C
   libraries of embedded toolchains have them, and ppy_port_template.c shows
   them for a build without one.) */

#include <stdint.h>
#include <stddef.h>
#include "ppy_port.h"

/* ---- what the C library would give: the program's own */
static void *x2c_memcpy(void *d, const void *s, size_t n){ unsigned char *a=(unsigned char*)d; const unsigned char *b=(const unsigned char*)s; while(n--) *a++=*b++; return d; }
static void *x2c_memmove(void *d, const void *s, size_t n){
    unsigned char *a=(unsigned char*)d; const unsigned char *b=(const unsigned char*)s;
    if(a==b || !n) return d;
    if(a<b || a>=b+n){ while(n--) *a++=*b++; }
    else { a+=n; b+=n; while(n--) *--a=*--b; }
    return d;
}
static void *x2c_memset(void *d, int c, size_t n){ unsigned char *a=(unsigned char*)d; while(n--) *a++=(unsigned char)c; return d; }
static size_t x2c_strlen(const char *s){ size_t n=0; while(s[n]) n++; return n; }
#define memcpy  x2c_memcpy
#define memmove x2c_memmove
#define memset  x2c_memset
#define strlen  x2c_strlen

/* ---- libm: done here exactly, or the library's (declared: only what the program calls is linked) */
double sin(double); double cos(double); double tan(double); double atan(double); double asin(double); double acos(double);
double atan2(double, double); double exp(double); double exp2(double); double log(double); double log10(double); double log2(double);
double pow(double, double);
typedef union { double d; uint64_t u; } X2cPortBits;
static uint64_t x2c_dbits(double v){ X2cPortBits b; b.d=v; return b.u; }
static double x2c_bitsd(uint64_t u){ X2cPortBits b; b.u=u; return b.d; }
static double x2c_fabs(double x){ return x2c_bitsd(x2c_dbits(x)&0x7FFFFFFFFFFFFFFFull); }
static double x2c_trunc(double x){
    uint64_t u=x2c_dbits(x); int e=(int)((u>>52)&0x7FF)-1023;
    if(e>=52) return x;                                 /* an integer already, or inf / nan */
    if(e<0) return x2c_bitsd(u&0x8000000000000000ull);  /* |x| < 1: a signed zero */
    return x2c_bitsd(u&~(0x000FFFFFFFFFFFFFull>>e));
}
static double x2c_floor(double x){ double t=x2c_trunc(x); return x<0 && t!=x ? t-1.0 : t; }
static double x2c_ceil(double x){ double t=x2c_trunc(x); return x>0 && t!=x ? t+1.0 : t; }
static double x2c_rint(double x){                       /* to the nearest integer, ties to even (the default rounding) */
    uint64_t u=x2c_dbits(x); int e=(int)((u>>52)&0x7FF)-1023;
    if(e>=52) return x;
    volatile double big=4503599627370496.0, t;          /* 2^52: adding it rounds away the fraction */
    if(x>=0){ t=x+big; t-=big; return x2c_bitsd(x2c_dbits(t)|(u&0x8000000000000000ull)); }
    t=-x+big; t-=big; return -t;
}
/* d = (q + sticky) * 2^e2 to the nearest double: x2c_make_double (aot/x2c_float.c) */
static double x2c_make_double(uint64_t q, int e2, int sticky);
static int x2c_unpack(double x, uint64_t *m, int *e2){  /* finite nonzero |x| = m * 2^e2, m < 2^53; 0 for 0/inf/nan */
    uint64_t u=x2c_dbits(x); int ex=(int)((u>>52)&0x7FF);
    if(ex==0x7FF) return 0;
    uint64_t f=u&0x000FFFFFFFFFFFFFull;
    if(ex){ *m=f|0x0010000000000000ull; *e2=ex-1075; } else { if(!f) return 0; *m=f; *e2=-1074; }
    return 1;
}
static double x2c_ldexp(double x, int n){
    uint64_t m; int e2;
    if(!x2c_unpack(x,&m,&e2)) return x;
    if(n>4000) n=4000; if(n<-4000) n=-4000;
    double r=x2c_make_double(m,e2+n,0);
    return x2c_dbits(x)>>63 ? -r : r;
}
static double x2c_sqrt(double x){                       /* correctly rounded, digit by digit */
    uint64_t u=x2c_dbits(x);
    if(x==0 || (u>>52)==0x7FF) return x;               /* +-0, +inf, nan */
    if(u>>63) return x2c_bitsd(0x7FF8000000000000ull);
    uint64_t m; int e2; x2c_unpack(x,&m,&e2);
    while(!(m&0x0010000000000000ull)){ m<<=1; e2--; }  /* (subnormals) */
    if(e2&1){ m<<=1; e2--; }                           /* an even exponent: m in [2^52, 2^54) */
    /* r = floor(sqrt(m * 2^54)): 54 bits; the operand is m:0 (m * 2^54, 108 bits) taken two bits at a time */
    uint64_t hi=m>>10, lo=m<<54, root=0, rem=0;
    for(int i=0;i<55;i++){                             /* bits 109..0 of hi:lo, from the top */
        int sh=108-2*i;                                /* (even: a pair never straddles hi and lo) */
        uint64_t two= sh>=64 ? (hi>>(sh-64))&3 : (lo>>sh)&3;
        rem=(rem<<2)|two;
        uint64_t trial=(root<<2)|1;
        if(rem>=trial){ rem-=trial; root=(root<<1)|1; } else root<<=1;
    }
    /* root in [2^53, 2^54): sqrt(x) = root * 2^((e2-54)/2), the rest below its last bit when rem != 0 */
    return x2c_make_double(root,(e2-54)/2,rem!=0);
}
static double x2c_fmod(double x, double y){             /* exact: x - trunc(x/y)*y */
    uint64_t ux=x2c_dbits(x), uy=x2c_dbits(y);
    uint64_t mx, my; int ex, ey;
    if(y==0 || (ux>>52&0x7FF)==0x7FF || ((uy>>52&0x7FF)==0x7FF && (uy&0x000FFFFFFFFFFFFFull))) return x2c_bitsd(0x7FF8000000000000ull);
    if((uy>>52&0x7FF)==0x7FF) return x;                 /* y infinite */
    if(!x2c_unpack(x,&mx,&ex)) return x;               /* x = +-0 */
    x2c_unpack(y,&my,&ey);
    while(!(mx&0x0010000000000000ull)){ mx<<=1; ex--; }
    while(!(my&0x0010000000000000ull)){ my<<=1; ey--; }
    if(ex<ey || (ex==ey && mx<my)) return x;
    for(;ex>ey;ex--){ if(mx>=my){ mx-=my; if(!mx) return x2c_bitsd(ux&0x8000000000000000ull); } mx<<=1; }
    if(mx>=my) mx-=my;
    if(!mx) return x2c_bitsd(ux&0x8000000000000000ull);
    double r=x2c_make_double(mx,ey,0);
    return ux>>63 ? -r : r;
}
#define fabs      x2c_fabs
#define trunc     x2c_trunc
#define floor     x2c_floor
#define ceil      x2c_ceil
#define nearbyint x2c_rint
#define ldexp     x2c_ldexp
#define sqrt      x2c_sqrt
#define fmod      x2c_fmod

#include "aot/x2c_machine.c"
#include "aot/x2c_float.c"

/* ---- the program's memory */
#define X2C_STRS_SIZE 1024u
static u8 *M;                                           /* m: arena - X2C_DATA_BASE */
static u32 x2c_arena_end;                               /* guest address of the arena's end */
static u32 x2c_strs, x2c_strs_at;                       /* C strings C functions return */
static u32 x2c_mmap_lo, x2c_mmap_hi;                    /* pages for mmap2 */
static u32 x2c_brk;
static u32 x2c_stack_size;
static int x2c_exited, x2c_status;                      /* the program called exit */
static int x2c_errno;                                   /* errno after the last C function (ctypes.get_errno()) */
__attribute__((weak)) int ppy_port_errno(void){ return 0; }   /* (a port with a C library gives errno: ppy_port.h) */

static void x2c_fatal(const char *msg){
    static const char pre[]="plainpy (native code): ";
    ppy_port_write(2,pre,(u32)sizeof pre-1); ppy_port_write(2,msg,(u32)x2c_strlen(msg)); ppy_port_write(2,"\n",1);
    ppy_port_panic(msg);
    for(;;){}
}
static void x2c_bad_jump(u32 a){ (void)a; x2c_fatal("a jump to an address that is no code"); }

void *ppy_guest_ptr(uint32_t a){ return a ? (void*)(M+a) : (void*)0; }
uint32_t ppy_guest_addr(const void *p){
    const u8 *q=(const u8*)p;
    if(!q || q<M+X2C_DATA_BASE || q>=M+x2c_arena_end) return 0;
    return (u32)(q-M);
}

/* mmap2 / munmap of guest pages: first fit; the pages handed out are zero */
typedef struct { u32 a, n; } X2cRange;
#define X2C_MAX_FREE 256
static X2cRange x2c_free[X2C_MAX_FREE]; static int x2c_nfree;
static u32 x2c_mmap(u32 n){
    n=(n+4095u)&~4095u;
    if(!n) return (u32)-PPY_EINVAL;
    for(int i=0;i<x2c_nfree;i++) if(x2c_free[i].n>=n){
        u32 a=x2c_free[i].a; x2c_free[i].a+=n; x2c_free[i].n-=n;
        if(!x2c_free[i].n){ memmove(x2c_free+i,x2c_free+i+1,sizeof(X2cRange)*(size_t)(x2c_nfree-i-1)); x2c_nfree--; }
        memset(M+a,0,n);
        return a;
    }
    return (u32)-PPY_ENOMEM;
}
static u32 x2c_munmap(u32 a, u32 n){
    n=(n+4095u)&~4095u;
    if(a<x2c_mmap_lo || a+n>x2c_mmap_hi || ((a-x2c_mmap_lo)&4095u)) return (u32)-PPY_EINVAL;
    int i=0; while(i<x2c_nfree && x2c_free[i].a<a) i++;
    if(x2c_nfree==X2C_MAX_FREE) return 0;              /* (too fragmented to keep: those pages stay used) */
    memmove(x2c_free+i+1,x2c_free+i,sizeof(X2cRange)*(size_t)(x2c_nfree-i)); x2c_nfree++;
    x2c_free[i].a=a; x2c_free[i].n=n;
    if(i+1<x2c_nfree && x2c_free[i].a+x2c_free[i].n==x2c_free[i+1].a){ x2c_free[i].n+=x2c_free[i+1].n; memmove(x2c_free+i+1,x2c_free+i+2,sizeof(X2cRange)*(size_t)(x2c_nfree-i-2)); x2c_nfree--; }
    if(i>0 && x2c_free[i-1].a+x2c_free[i-1].n==x2c_free[i].a){ x2c_free[i-1].n+=x2c_free[i].n; memmove(x2c_free+i,x2c_free+i+1,sizeof(X2cRange)*(size_t)(x2c_nfree-i-1)); x2c_nfree--; }
    return 0;
}

/* ---- pointers to and from C functions (ctypes) */
#define X2C_HANDLES 0xF0000000u
#define X2C_MAX_HANDLES 64
static void *x2c_handles[X2C_MAX_HANDLES]; static u32 x2c_nhandles;
static void *x2c_ptr(u32 a){
    if(!a) return (void*)0;
    if(a>=X2C_HANDLES){ u32 i=(a-X2C_HANDLES)>>4; if(i<x2c_nhandles) return (char*)x2c_handles[i]+(a&15u); }
    return M+a;
}
static u32 x2c_unptr(void *p){
    if(!p) return 0;
    u32 g=ppy_guest_addr(p); if(g) return g;
    for(u32 i=0;i<x2c_nhandles;i++) if(x2c_handles[i]==p) return X2C_HANDLES+16*i;
    if(x2c_nhandles==X2C_MAX_HANDLES) x2c_fatal("more than 64 pointers from C functions");
    x2c_handles[x2c_nhandles]=p;
    return X2C_HANDLES+16*x2c_nhandles++;
}
static u32 x2c_unstr(const char *s){                    /* a char * result: guest memory, or a copy there */
    if(!s) return 0;
    u32 g=ppy_guest_addr(s); if(g) return g;
    size_t n=strlen(s)+1;
    if(n>X2C_STRS_SIZE/2) n=X2C_STRS_SIZE/2;
    if(x2c_strs_at+n>X2C_STRS_SIZE-16) x2c_strs_at=0;   /* (its last 16 bytes: the errno cell) */
    u32 a=x2c_strs+x2c_strs_at; x2c_strs_at+=(u32)((n+15)&~(size_t)15);
    memcpy(M+a,s,n); M[a+n-1]=0;
    return a;
}
typedef struct { const char *lib, *sym; void *fn; } X2cImport;
static void *x2c_errno_cell(void){ u8 *m=M; u32 a=x2c_strs+X2C_STRS_SIZE-16; D(a)=(u32)x2c_errno; return m+a; }   /* (__errno_location: in guest memory) */
#define X2C_STR2(x) #x
#define X2C_STR(x) X2C_STR2(x)
#define X2C_ASMNAME(s) X2C_STR(__USER_LABEL_PREFIX__) s  /* a C function's symbol, whatever the C file calls it */
#define CFN(k) (x2c_imports[k].fn)

/* ---- the system calls of the runtime (Linux i386 numbers) */
typedef struct { u32 eax, ebx, ecx, edx, esi, edi, ebp; } X2cRegs;
static void x2c_put32(u8 *m, u32 a, u32 v){ if(a) D(a)=v; }
static u32 x2c_syscall(u8 *m, X2cRegs *r){
    switch(r->eax){
        case 1: case 252: x2c_status=(int)r->ebx; x2c_exited=1; return 0;      /* exit: ppy_main returns */
        case 3: if(r->ebx==0) return (u32)ppy_port_read(0,ppy_guest_ptr(r->ecx),r->edx); break;
        case 4: if(r->ebx==1 || r->ebx==2) return (u32)ppy_port_write((int)r->ebx,ppy_guest_ptr(r->ecx),r->edx); break;
        case 13:{ u32 t=(u32)(ppy_port_clock_ns(0)/1000000000u); x2c_put32(m,r->ebx,t); return t; }   /* time */
        case 45:                                                                 /* brk: none to grow */
            return x2c_brk;
        case 78:{ uint64_t ns=ppy_port_clock_ns(0);                              /* gettimeofday */
            if(r->ebx){ D(r->ebx)=(u32)(ns/1000000000u); D(r->ebx+4)=(u32)(ns%1000000000u/1000u); }
            if(r->ecx){ D(r->ecx)=0; D(r->ecx+4)=0; }
            return 0; }
        case 91: return x2c_munmap(r->ebx,r->ecx);
        case 162:{ uint64_t ns=(uint64_t)D(r->ebx)*1000000000u+D(r->ebx+4);       /* nanosleep */
            ppy_port_sleep_ns(ns); x2c_put32(m,r->ecx,0); if(r->ecx) D(r->ecx+4)=0; return 0; }
        case 192: if(!(r->esi&0x20)) return (u32)-PPY_ENODEV;                    /* mmap2: anonymous memory only */
            return x2c_mmap(r->ecx);
        case 265:{ uint64_t ns=ppy_port_clock_ns(r->ebx==0 ? 0 : 1);             /* clock_gettime */
            D(r->ecx)=(u32)(ns/1000000000u); D(r->ecx+4)=(u32)(ns%1000000000u); return 0; }
        case 355: ppy_port_random(ppy_guest_ptr(r->ebx),r->ecx); return r->ecx; /* getrandom */
    }
    uint32_t regs[7]={r->eax,r->ebx,r->ecx,r->edx,r->esi,r->edi,r->ebp};
    return (u32)ppy_port_syscall(regs);
}

/* ---- start: the arena laid out, Linux's initial stack (argc, argv, an empty environment) */
static u32 x2c_start(u32 data_end, int argc, char **argv){
    uint32_t size=PPY_ARENA_SIZE;
    u8 *a=(u8*)ppy_port_arena(&size);
    if(!a) x2c_fatal("the port gave no memory (ppy_port_arena)");
    if((uintptr_t)a&15u){ u32 k=16u-(u32)((uintptr_t)a&15u); a+=k; size=size>k?size-k:0; }
    if(size>0xE0000000u) size=0xE0000000u;              /* (guest addresses stay below the handles) */
    size&=~15u;
    M=a-X2C_DATA_BASE;
    u8 *m=M;
    x2c_arena_end=X2C_DATA_BASE+size;
    x2c_strs=(data_end+15u)&~15u; x2c_strs_at=0;
    x2c_mmap_lo=x2c_brk=(x2c_strs+X2C_STRS_SIZE+4095u)&~4095u;
    x2c_stack_size=(PPY_STACK_SIZE+15u)&~15u;
    if(x2c_mmap_lo+x2c_stack_size+4096u>x2c_arena_end || x2c_arena_end<X2C_DATA_BASE) x2c_fatal("the arena is too small for the program's data and stack (plainpy --arena, --stack)");
    x2c_mmap_hi=(x2c_arena_end-x2c_stack_size)&~4095u;
    x2c_nfree=1; x2c_free[0].a=x2c_mmap_lo; x2c_free[0].n=x2c_mmap_hi-x2c_mmap_lo;
    x2c_nhandles=0; x2c_exited=0; x2c_status=0;
    memset(M+X2C_DATA_BASE,0,x2c_mmap_lo-X2C_DATA_BASE);
    u32 sp=x2c_arena_end-16;
    u32 ptrs[16]; int n=argc<16?argc:16;
    for(int i=0;i<n;i++){ size_t l=strlen(argv[i])+1; if(l>256) l=256; sp-=(u32)l; memcpy(M+sp,argv[i],l); M[sp+l-1]=0; ptrs[i]=sp; }
    sp&=~15u; sp-=4u*(u32)(n+5); sp&=~15u;
    u32 at=sp;
    D(at)=(u32)n; at+=4;
    for(int i=0;i<n;i++){ D(at)=ptrs[i]; at+=4; }
    D(at)=0; D(at+4)=0; D(at+8)=0; D(at+12)=0;         /* argv's end, envp's end, auxv */
    return sp;
}
