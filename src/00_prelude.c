#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <setjmp.h>

#if defined(MPY_PLATFORM_KOLIBRI) || defined(MPY_TARGET_KOLIBRIOS) || defined(MPY_FS_KOLIBRI)
/* Replace libc setjmp/longjmp with GCC builtins - the DLL-based libc
   setjmp requires TLS/reent setup that we don't have. */
#undef setjmp
#undef longjmp
#define setjmp(buf)  __builtin_setjmp((void**)(buf))
#define longjmp(buf,val) __builtin_longjmp((void**)(buf),1)

/* KolibriOS console via shared memory.
   Forward declaration - the actual definition is in 09_console_kolibri.c
   which is included later in the same translation unit. */
int  kol_console_init(void);
void kol_console_deinit(void);
void kol_console_printf(const char *fmt, ...);
void kol_console_puts(const char *s);
void kol_console_gets(char *buf, int maxlen);
void kol_console_cls(void);

/* Dual printf: format to buffer, then send to stderr AND console.
   Handles up to 3 args: %s, %d, %lld, %.15g, %c, %p, %% */
static void _dual_printf(const char *fmt, ...){
    char buf[256]; int pos=0;
    va_list args; va_start(args, fmt);
    for(const char *p=fmt;*p&&pos<(int)sizeof(buf)-32;p++){
        if(*p!='%'){buf[pos++]=*p;continue;}
        p++; if(*p==0)break;
        if(*p=='s'){const char *s=va_arg(args,const char*);if(!s)s="(null)";
            int sl=(int)strlen(s),rm=(int)sizeof(buf)-32-pos;if(sl>rm)sl=rm;memcpy(buf+pos,s,(size_t)sl);pos+=sl;}
        else if(*p=='d'||*p=='i'){int v=va_arg(args,int);int neg=0;if(v<0){neg=1;v=-v;}
            char t[16];int ti=0;if(v==0)t[ti++]='0';while(v>0){t[ti++]=(char)('0'+v%10);v/=10;}
            if(neg)buf[pos++]='-';while(ti>0)buf[pos++]=t[--ti];}
        else if(*p=='l'){p++;if(*p=='l'){p++;long long v=va_arg(args,long long);int neg=0;if(v<0){neg=1;v=-v;}
            char t[32];int ti=0;if(v==0)t[ti++]='0';while(v>0){t[ti++]=(char)('0'+v%10);v/=10;}
            if(neg)buf[pos++]='-';while(ti>0)buf[pos++]=t[--ti];}}
        else if(*p=='.'){p++;int prec=0;while(*p>='0'&&*p<='9'){prec=prec*10+(*p-'0');p++;}
            if(*p=='g'){double d=va_arg(args,double);int neg=0;
                if(d<0){neg=1;d=-d;}long long ip=(long long)d;double fp=d-(double)ip;
                char t2[32];int ti=0;long long iv=ip;if(iv==0)t2[ti++]='0';while(iv>0){t2[ti++]=(char)('0'+iv%10);iv/=10;}
                if(neg)buf[pos++]='-';while(ti>0)buf[pos++]=t2[--ti];
                if(prec>0){buf[pos++]='.';for(int i=0;i<prec;i++){fp*=10.0;int dg=(int)fp;fp-=dg;buf[pos++]=(char)('0'+dg);}}}}
        else if(*p=='c'){buf[pos++]=(char)va_arg(args,int);}
        else if(*p=='p'){buf[pos++]='0';buf[pos++]='x';
            unsigned long long a=(unsigned long long)(uintptr_t)va_arg(args,void*);
            char t[32];int ti=0;if(a==0)t[ti++]='0';while(a>0){int d=(int)(a%16);t[ti++]=(char)(d<10?'0'+d:'a'+d-10);a/=16;}
            while(ti>0)buf[pos++]=t[--ti];}
        else if(*p=='%')buf[pos++]='%';
        else buf[pos++]='?';
    }
    buf[pos]=0;
    fprintf(stderr,"%s",buf);
    kol_console_puts(buf);
    va_end(args);
}
#undef printf
#define printf(...)  _dual_printf(__VA_ARGS__)

#endif

static void die(const char *m){ fprintf(stderr, "%s\n", m); exit(1); }

#if defined(MPY_PLATFORM_KOLIBRI) || defined(MPY_TARGET_KOLIBRIOS) || defined(MPY_FS_KOLIBRI)
/* Use f68.12 for memory allocation (libc malloc crashes without TLS).
   f68.12: eax=68, ebx=12, ecx=pages (1 page = 4096 bytes).
   Returns pointer in eax, or 0 on failure. */
static void *kos_malloc(size_t n){
    int pages = (int)((n + 4095) / 4096);
    void *p;
    __asm__ __volatile__("int $0x40" : "=a"(p) : "a"(68), "b"(12), "c"(pages) : "memory");
    if(!p || (int)p <= 0) die("out of memory");
    return p;
}
#define malloc(n)  kos_malloc(n)
#define realloc(p,n)  ({ void *_p = kos_malloc(n); if(p){ memcpy(_p, p, n); free(p); } _p; })
#define free(p)  /* no-op - KolibriOS heap manages pages */
#endif

static void *xmalloc(size_t n){ void *p=malloc(n?n:1); if(!p) die("out of memory"); return p; }
static void *xrealloc(void *p,size_t n){ p=realloc(p,n?n:1); if(!p) die("out of memory"); return p; }
static char *xstrndup2(const char *s,int n){ char *r=(char*)xmalloc((size_t)n+1); memcpy(r,s,(size_t)n); r[n]=0; return r; }
static char *xstrdup2(const char *s){ return xstrndup2(s,(int)strlen(s)); }