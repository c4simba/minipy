/* The C library plainpy's mbedtls.obj calls (build.sh): what the port's build
   imports from KolibriOS's libc.obj, here, so the library needs no other one.
   Memory: KolibriOS's heap (function 68: 11 starts it, 12 / 13 give and free
   pages), small blocks carved out of 64 KiB of it per size class; time:
   functions 3 and 29 (the CMOS clock, as local time); the 64-bit division the
   compiler calls for. */

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

int errno;

static inline int kos_call(int eax, int ebx, int ecx){
    int r;
    __asm__ volatile("int $0x40" : "=a"(r) : "a"(eax), "b"(ebx), "c"(ecx) : "memory");
    return r;
}

/* ---------------------------------------------------------------- memory, strings */
void *memcpy(void *d, const void *s, size_t n){        /* (rep movsb / stosb: the compiler cannot make a call of them) */
    void *r=d;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return r;
}
void *memmove(void *d, const void *s, size_t n){
    if((uintptr_t)d<=(uintptr_t)s || (uintptr_t)d>=(uintptr_t)s+n) return memcpy(d,s,n);
    unsigned char *dd=(unsigned char*)d+n-1; const unsigned char *ss=(const unsigned char*)s+n-1;
    __asm__ volatile("std\n\trep movsb\n\tcld" : "+D"(dd), "+S"(ss), "+c"(n) : : "memory");
    return d;
}
void *memset(void *d, int c, size_t n){
    void *r=d;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}
int memcmp(const void *a, const void *b, size_t n){
    const unsigned char *x=a, *y=b;
    for(size_t i=0;i<n;i++) if(x[i]!=y[i]) return x[i]<y[i] ? -1 : 1;
    return 0;
}
size_t strlen(const char *s){ size_t n=0; while(s[n]) n++; return n; }
int strcmp(const char *a, const char *b){
    while(*a && *a==*b){ a++; b++; }
    return (unsigned char)*a-(unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n){
    for(size_t i=0;i<n;i++){ if(a[i]!=b[i] || !a[i]) return (unsigned char)a[i]-(unsigned char)b[i]; }
    return 0;
}
char *strncpy(char *d, const char *s, size_t n){
    size_t i=0;
    for(;i<n && s[i];i++) d[i]=s[i];
    for(;i<n;i++) d[i]=0;
    return d;
}
char *strcpy(char *d, const char *s){ char *r=d; while((*d++=*s++)); return r; }
char *strchr(const char *s, int c){ for(;;s++){ if(*s==(char)c) return (char*)s; if(!*s) return NULL; } }
char *strrchr(const char *s, int c){ const char *r=NULL; for(;;s++){ if(*s==(char)c) r=s; if(!*s) return (char*)r; } }
char *strstr(const char *h, const char *n){
    size_t k=strlen(n);
    if(!k) return (char*)h;
    for(;*h;h++) if(*h==*n && !strncmp(h,n,k)) return (char*)h;
    return NULL;
}
int abs(int x){ return x<0 ? -x : x; }

/* ---------------------------------------------------------------- the heap */
/* a block: 8 bytes before it say its class (16 << class bytes) or, for a big
   one (more than 2 KiB: pages of its own), 0x80000000 | their size; free small
   blocks are listed per class */
#define NCLASS 8
static void *free_list[NCLASS];
static char *chunk_at[NCLASS], *chunk_end[NCLASS];
static int heap_ready;
static void *pages(size_t n){
    if(!heap_ready){ kos_call(68,11,0); heap_ready=1; }   /* (the program has started it: this returns its size then) */
    return (void*)kos_call(68,12,(int)n);
}
void *malloc(size_t n){
    if(n==0) n=1;
    if(n>2048){
        uint32_t *p=(uint32_t*)pages(n+8);
        if(!p) return NULL;
        p[0]=0x80000000u|(uint32_t)(n+8);
        return p+2;
    }
    int c=0; while((16u<<c)<n) c++;
    void *b=free_list[c];
    if(b){ free_list[c]=*(void**)b; return b; }
    size_t sz=(16u<<c)+8;
    if(!chunk_at[c] || chunk_at[c]+sz>chunk_end[c]){
        char *p=(char*)pages(65536);
        if(!p) return NULL;
        chunk_at[c]=p; chunk_end[c]=p+65536;
    }
    uint32_t *h=(uint32_t*)chunk_at[c]; chunk_at[c]+=sz;
    h[0]=(uint32_t)c;
    return h+2;
}
void free(void *p){
    if(!p) return;
    uint32_t *h=(uint32_t*)p-2;
    if(h[0]&0x80000000u){ kos_call(68,13,(int)(uintptr_t)h); return; }
    *(void**)p=free_list[h[0]]; free_list[h[0]]=p;
}
void *calloc(size_t n, size_t size){
    if(size && n>(size_t)-1/size) return NULL;
    void *p=malloc(n*size);
    if(p) memset(p,0,n*size);
    return p;
}
void *realloc(void *p, size_t n){
    if(!p) return malloc(n);
    uint32_t *h=(uint32_t*)p-2;
    size_t old= (h[0]&0x80000000u) ? (h[0]&0x7FFFFFFFu)-8 : (size_t)(16u<<h[0]);
    if(n<=old) return p;
    void *q=malloc(n);
    if(q){ memcpy(q,p,old); free(p); }
    return q;
}
static uint32_t rand_state=0x2545F491u;
int rand(void){ rand_state^=rand_state<<13; rand_state^=rand_state>>17; rand_state^=rand_state<<5; return (int)(rand_state&RAND_MAX); }
void srand(unsigned seed){ rand_state=seed?seed:1; }
void abort(void){ kos_call(-1,0,0); for(;;); }
void exit(int status){ (void)status; kos_call(-1,0,0); for(;;); }

/* ---------------------------------------------------------------- 64-bit division (the compiler's helpers) */
static uint64_t udivmod(uint64_t n, uint64_t d, uint64_t *rem){
    uint64_t q=0, r=0;
    if(d==0){ if(rem) *rem=0; return 0; }
    for(int i=63;i>=0;i--){
        r=(r<<1)|((n>>i)&1u);
        if(r>=d){ r-=d; q|=(uint64_t)1<<i; }
    }
    if(rem) *rem=r;
    return q;
}
uint64_t __udivdi3(uint64_t n, uint64_t d){ return udivmod(n,d,0); }
uint64_t __umoddi3(uint64_t n, uint64_t d){ uint64_t r; udivmod(n,d,&r); return r; }
int64_t __divdi3(int64_t n, int64_t d){
    int neg=(n<0)!=(d<0);
    uint64_t q=udivmod(n<0?-(uint64_t)n:(uint64_t)n, d<0?-(uint64_t)d:(uint64_t)d, 0);
    return neg ? -(int64_t)q : (int64_t)q;
}
int64_t __moddi3(int64_t n, int64_t d){
    uint64_t r; udivmod(n<0?-(uint64_t)n:(uint64_t)n, d<0?-(uint64_t)d:(uint64_t)d, &r);
    return n<0 ? -(int64_t)r : (int64_t)r;
}

/* ---------------------------------------------------------------- formatted text */
typedef struct { char *s; size_t n, at; } Out;
static void put(Out *o, char c){ if(o->at+1<o->n) o->s[o->at]=c; o->at++; }
static void put_num(Out *o, uint64_t v, int base, int upper, int neg, int width, int zero, int left, int prec){
    char tmp[24]; int k=0;
    const char *dig= upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if(v==0 && prec!=0) tmp[k++]='0';
    while(v){ tmp[k++]=dig[(int)(v%(unsigned)base)]; v/=(unsigned)base; }
    while(k<prec) tmp[k++]='0';
    int len=k+(neg?1:0);
    if(!left && !zero) for(int i=len;i<width;i++) put(o,' ');
    if(neg) put(o,'-');
    if(!left && zero) for(int i=len;i<width;i++) put(o,'0');
    while(k) put(o,tmp[--k]);
    if(left) for(int i=len;i<width;i++) put(o,' ');
}
int vsnprintf(char *s, size_t n, const char *fmt, va_list ap){
    Out o={s,n,0};
    for(;*fmt;fmt++){
        if(*fmt!='%'){ put(&o,*fmt); continue; }
        fmt++;
        int left=0, zero=0, width=0, prec=-1, lng=0;
        for(;;fmt++){ if(*fmt=='-') left=1; else if(*fmt=='0') zero=1; else if(*fmt=='+'||*fmt==' '||*fmt=='#') ; else break; }
        if(*fmt=='*'){ width=va_arg(ap,int); fmt++; } else while(*fmt>='0' && *fmt<='9') width=width*10+(*fmt++-'0');
        if(*fmt=='.'){ fmt++; prec=0; if(*fmt=='*'){ prec=va_arg(ap,int); fmt++; } else while(*fmt>='0' && *fmt<='9') prec=prec*10+(*fmt++-'0'); }
        for(;;fmt++){ if(*fmt=='l') lng++; else if(*fmt=='j') lng=2; else if(*fmt=='z' || *fmt=='t' || *fmt=='h') ; else break; }
        switch(*fmt){
            case 'd': case 'i':{ int64_t v= lng>=2 ? va_arg(ap,long long) : (int64_t)va_arg(ap,long);
                put_num(&o,v<0?-(uint64_t)v:(uint64_t)v,10,0,v<0,width,zero,left,prec); break; }
            case 'u': case 'x': case 'X': case 'o':{ uint64_t v= lng>=2 ? va_arg(ap,unsigned long long) : (uint64_t)va_arg(ap,unsigned long);
                put_num(&o,v,*fmt=='u'?10:*fmt=='o'?8:16,*fmt=='X',0,width,zero,left,prec); break; }
            case 'p':{ uintptr_t v=(uintptr_t)va_arg(ap,void*); put(&o,'0'); put(&o,'x'); put_num(&o,v,16,0,0,0,0,0,-1); break; }
            case 'c': put(&o,(char)va_arg(ap,int)); break;
            case 's':{ const char *t=va_arg(ap,const char*); if(!t) t="(null)";
                size_t len=strlen(t); if(prec>=0 && (size_t)prec<len) len=(size_t)prec;
                if(!left) for(size_t i=len;(int)i<width;i++) put(&o,' ');
                for(size_t i=0;i<len;i++) put(&o,t[i]);
                if(left) for(size_t i=len;(int)i<width;i++) put(&o,' ');
                break; }
            case '%': put(&o,'%'); break;
            case 0: fmt--; break;
            default: put(&o,'%'); put(&o,*fmt); break;
        }
    }
    if(n) s[o.at<n?o.at:n-1]=0;
    return (int)o.at;
}
int snprintf(char *s, size_t n, const char *fmt, ...){ va_list ap; va_start(ap,fmt); int r=vsnprintf(s,n,fmt,ap); va_end(ap); return r; }
int sprintf(char *s, const char *fmt, ...){ va_list ap; va_start(ap,fmt); int r=vsnprintf(s,(size_t)-1/2,fmt,ap); va_end(ap); return r; }
int printf(const char *fmt, ...){ (void)fmt; return 0; }                 /* (debug output: none) */
FILE *stdout, *stderr;
int fprintf(FILE *f, const char *fmt, ...){ (void)f; (void)fmt; return 0; }
int puts(const char *s){ (void)s; return 0; }

/* ---------------------------------------------------------------- time (the CMOS clock: local time, as KolibriOS keeps it) */
static int bcd(int x){ return (x>>4)*10+(x&15); }
static long days_from_civil(int y, int m, int d){
    y-= m<=2; long era=(y>=0?y:y-399)/400; int yoe=y-(int)era*400;
    int doy=(153*(m+(m>2?-3:9))+2)/5+d-1; int doe=yoe*365+yoe/4-yoe/100+doy;
    return era*146097+doe-719468;
}
time_t time(time_t *t){
    int d=kos_call(29,0,0), c=kos_call(3,0,0);
    if(kos_call(29,0,0)!=d){ d=kos_call(29,0,0); c=kos_call(3,0,0); }    /* (midnight between the two) */
    long days=days_from_civil(2000+bcd(d&255),bcd((d>>8)&255),bcd((d>>16)&255));
    time_t r=(time_t)(((days*24+bcd(c&255))*60+bcd((c>>8)&255))*60+bcd((c>>16)&255));
    if(t) *t=r;
    return r;
}
struct tm *gmtime_r(const time_t *t, struct tm *out){
    long s=(long)*t, days=s/86400, rem=s%86400;
    if(rem<0){ rem+=86400; days--; }
    out->tm_hour=(int)(rem/3600); out->tm_min=(int)(rem/60%60); out->tm_sec=(int)(rem%60);
    out->tm_wday=(int)((days+4)%7); if(out->tm_wday<0) out->tm_wday+=7;
    long z=days+719468, era=(z>=0?z:z-146096)/146097; long doe=z-era*146097;
    long yoe=(doe-doe/1460+doe/36524-doe/146096)/365; long y=yoe+era*400;
    long doy=doe-(365*yoe+yoe/4-yoe/100); long mp=(5*doy+2)/153;
    int d=(int)(doy-(153*mp+2)/5+1), m=(int)(mp<10?mp+3:mp-9);
    if(m<=2) y++;
    out->tm_year=(int)y-1900; out->tm_mon=m-1; out->tm_mday=d;
    out->tm_yday=(int)(days-days_from_civil((int)y,1,1)); out->tm_isdst=0;
    return out;
}
struct tm *gmtime(const time_t *t){ static struct tm tm; return gmtime_r(t,&tm); }
struct tm *localtime(const time_t *t){ return gmtime(t); }
clock_t clock(void){ return (clock_t)kos_call(26,9,0); }
