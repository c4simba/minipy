/* ========================= Typed compiler: the C runtime of a hosted program =========================
   plainpy --compile --target macos turns the i386 listing into C (aot/x2c.c);
   this file is the start of every such C program (plainpy carries it in
   plainpy.lib with the files it includes). The machine is aot/x2c_machine.c;
   its memory here is a 4 GiB guest address space, guest address a at host
   address M + a:

     0x00100000  code: addresses of the labels jumped to indirectly (return
                 addresses, function values), 4 bytes apart, nothing behind them
     0x08000000  the listing's data and bss, then the brk heap
     0x40000000  mmap2 (the allocator's pages, task stacks)
     0xB8000000  the stack (grows down from 0xC0000000; its lowest 64 KiB a guard)
     0xE0000000  copies of the C strings C functions return
     0xF0000000  handles: pointers C functions return that are outside M

   Linux system calls (int 0x80) run on the host's (targets/posix/lx_emul.c),
   C functions (ctypes) are called natively with the signature the listing
   gives each call, and the few runtime routines that compute in the x87's
   64-bit precision are done in C: float text exactly (aot/x2c_float.c), exp,
   pow, log ... with the C library's. */

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


#include "aot/x2c_machine.c"
#include "aot/x2c_float.c"

#define X2C_MMAP_BASE   0x40000000u
#define X2C_MMAP_END    0xB8000000u
#define X2C_STACK_TOP   0xC0000000u
#define X2C_STRS        0xE0000000u
#define X2C_STRS_SIZE   0x00100000u
#define X2C_HANDLES     0xF0000000u

/* ---- the guest address space */
static u8 *M;
static u32 x2c_brk_start, x2c_brk;
static int x2c_errno;                                   /* errno after the last C function */
static int x2c_argc; static char **x2c_argv;

static void x2c_fatal(const char *fmt, ...){
    va_list ap; va_start(ap,fmt); fprintf(stderr,"plainpy (native code): "); vfprintf(stderr,fmt,ap); fprintf(stderr,"\n"); va_end(ap);
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
    char first[600]; { const char *e=h?NULL:dlerror(); snprintf(first,sizeof first,"%s",e?e:""); }
    if(!h){                                             /* libfoo.so.N -> libfoo.dylib */
        char name[512]; const char *dot=strstr(so,".so");
        if(dot && dot-so<480){ snprintf(name,sizeof name,"%.*s.dylib",(int)(dot-so),so); h=dlopen(name,RTLD_NOW|RTLD_GLOBAL); }
    }
    if(!h && !strchr(so,'/')){                          /* the folders of PLAINPY_LIBRARY_PATH, then Homebrew's (the interpreter's too: ctypes.c) */
        static const char *const dirs[]={"/opt/homebrew/lib","/opt/homebrew/opt/openssl@3/lib","/usr/local/lib","/usr/local/opt/openssl@3/lib",NULL};
        char path[1024]; const char *env=getenv("PLAINPY_LIBRARY_PATH");
        while(!h && env && *env){
            const char *e=strchr(env,':'); size_t n= e ? (size_t)(e-env) : strlen(env);
            if(n && n<sizeof path-strlen(so)-2){ snprintf(path,sizeof path,"%.*s/%s",(int)n,env,so); h=dlopen(path,RTLD_NOW|RTLD_GLOBAL); }
            env= e ? e+1 : NULL;
        }
        for(int i=0;!h && dirs[i];i++){ snprintf(path,sizeof path,"%s/%s",dirs[i],so); h=dlopen(path,RTLD_NOW|RTLD_GLOBAL); }
    }
    if(!h) x2c_fatal("cannot load the C library %s: %s (PLAINPY_LIBRARY_PATH names folders to look in)",so,first);
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
#include "targets/posix/lx_emul.c"
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
    if(v==-ENOSYS) fprintf(stderr,"plainpy (native code): Linux system call %u is not supported\n",r->eax);
    (void)m;
    return (u32)v;
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
