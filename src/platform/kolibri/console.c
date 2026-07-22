/* ========================= KolibriOS console via shared memory =========================
 * The console is a named shared buffer "{PID}-SHELL" (opened with f68.22, freed
 * with f68.23). The protocol is just command + data: write a command code into
 * byte 0 (with any payload after it), then wait until the shell zeroes byte 0
 * to signal it consumed the command.
 *   SC_EXIT=1  SC_PUTC=2  SC_PUTS=3  SC_GETC=4  SC_GETS=5  SC_CLS=6
 * All output is also echoed to stderr (the debug board).
 */

#include "platform/platform.h"

static char *g_cbuf = NULL;    /* shared buffer: [0]=command byte, [1..]=payload */
static char  g_cname[32];      /* "{PID}-SHELL" */
static int   g_cok = 0;

/* ---- protocol primitive -------------------------------------------------- */

/* Issue the command already written to g_cbuf[0] and block until the shell
   consumes it (it zeroes byte 0). Bounded so a missing shell can't hang us.
   Returns 1 if consumed, 0 on timeout (and force-clears the byte). */
static int kol_wait_shell(void){
    for(int i=0; i<200 && g_cbuf[0]; i++)
        __asm__ __volatile__("int $0x40" :: "a"(5), "b"(5) : "memory");   /* fn 5: delay 50 ms */
    if(g_cbuf[0]){ g_cbuf[0]=0; return 0; }
    return 1;
}

/* ---- a tiny vsnprintf ----------------------------------------------------
   This port can't use newlib's printf (it needs the reent/TLS the loader never
   sets up, the same reason libc malloc faulted), so formatting is done by hand
   -- but in ONE place, shared by every console printf below. */

static int kutoa(unsigned long long v, int base, int upper, char *out){
    char tmp[32]; int t=0;
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do { tmp[t++]=digs[v%(unsigned)base]; v/=(unsigned)base; } while(v && t<32);
    for(int i=0;i<t;i++) out[i]=tmp[t-1-i];
    return t;
}
static int kftoa(double d, int prec, int trim, char *out){
    int n=0;
    if(d<0){ out[n++]='-'; d=-d; }
    if(prec<0) prec=6;
    long long ip=(long long)d; double fp=d-(double)ip;
    n += kutoa((unsigned long long)ip, 10, 0, out+n);
    if(prec>0){
        int dot=n; out[n++]='.';
        for(int i=0;i<prec && i<17;i++){ fp*=10.0; int dg=(int)fp; if(dg<0)dg=0; if(dg>9)dg=9; out[n++]=(char)('0'+dg); fp-=dg; }
        if(trim){ while(n>dot+1 && out[n-1]=='0') n--; if(out[n-1]=='.') n--; }   /* %g: drop trailing zeros */
    }
    return n;
}

/* Supports: %s %d %i %u %x %X %c %p %g %f %e %%, an optional l/ll length, a
   field width, a .precision, and the '-' (left) / '0' (zero-pad) flags.
   Always NUL-terminates and never writes past `cap`. */
static void kfmt(char *out, int cap, const char *fmt, va_list ap){
    int n=0;
    #define KPUT(ch) do{ if(n<cap-1) out[n++]=(char)(ch); }while(0)
    for(const char *p=fmt; *p; p++){
        if(*p!='%'){ KPUT(*p); continue; }
        p++;
        if(*p=='%'){ KPUT('%'); continue; }
        if(!*p) break;

        int left=0, zero=0;
        for(; *p=='-'||*p=='0'; p++){ if(*p=='-') left=1; else zero=1; }
        int width=0; for(; *p>='0'&&*p<='9'; p++) width=width*10+(*p-'0');
        int prec=-1; if(*p=='.'){ p++; prec=0; for(; *p>='0'&&*p<='9'; p++) prec=prec*10+(*p-'0'); }
        int lng=0; for(; *p=='l'; p++) lng++;
        char conv=*p;

        char num[64]; const char *body=num; int blen=0; char sign=0;
        switch(conv){
            case 's': body=va_arg(ap,const char*); if(!body) body="(null)"; blen=(int)strlen(body); if(prec>=0 && blen>prec) blen=prec; break;
            case 'c': num[0]=(char)va_arg(ap,int); blen=1; break;
            case 'd': case 'i': {
                long long v = (lng>=2) ? va_arg(ap,long long) : (long long)va_arg(ap,int);
                unsigned long long u; if(v<0){ sign='-'; u=(unsigned long long)(-v); } else u=(unsigned long long)v;
                blen=kutoa(u,10,0,num); break;
            }
            case 'u': blen=kutoa((lng>=2)?va_arg(ap,unsigned long long):(unsigned long long)va_arg(ap,unsigned),10,0,num); break;
            case 'x': case 'X': blen=kutoa((lng>=2)?va_arg(ap,unsigned long long):(unsigned long long)va_arg(ap,unsigned),16,conv=='X',num); break;
            case 'p': KPUT('0'); KPUT('x'); blen=kutoa((unsigned long long)(uintptr_t)va_arg(ap,void*),16,0,num); break;
            case 'g': case 'f': case 'e': blen=kftoa(va_arg(ap,double), prec, conv=='g', num); break;
            default: KPUT('?'); continue;
        }

        int total = blen + (sign?1:0);
        if(!left){
            if(zero){ if(sign){ KPUT(sign); sign=0; } for(int i=total;i<width;i++) KPUT('0'); }
            else    { for(int i=total;i<width;i++) KPUT(' '); }
        }
        if(sign) KPUT(sign);
        for(int i=0;i<blen;i++) KPUT(body[i]);
        if(left) for(int i=total;i<width;i++) KPUT(' ');
    }
    out[n]=0;
    #undef KPUT
}

/* ---- console API --------------------------------------------------------- */

int kol_console_init(void){
    if(g_cok) return 0;                 /* idempotent: safe to call more than once */
    char *b = malloc(1024); if(!b) return -1;
    memset(b,0,1024);
    __asm__ __volatile__("int $0x40" : : "a"(9),"b"((int)b),"c"(-1) : "memory");   /* fn 9: process info */
    int pid = *(int*)(b+30);
    { char t[16]; int i=0, v=pid;
      if(v==0){g_cname[0]='0';g_cname[1]=0;}
      else { if(v<0)v=-v; while(v>0){t[i++]=(char)('0'+v%10);v/=10;} int p=0; while(i>0)g_cname[p++]=t[--i]; g_cname[p]=0; }
    }
    { char *d=g_cname; while(*d)d++; const char *s="-SHELL"; while(*s)*d++=*s++; *d=0; }
    free(b);

    int ptr_val, edx_result;
    __asm__ __volatile__("int $0x40"
        : "=a"(ptr_val), "=d"(edx_result)
        : "a"(68), "b"(22), "c"((int)g_cname), "d"(1024*16), "S"(0x04|0x01)   /* fn 68.22: open shared */
        : "memory");
    if(ptr_val > 0 && (int)ptr_val > 0x1000){
        g_cbuf = (char*)ptr_val;
        g_cbuf[0] = 0;
        g_cok = 1;
    }
    return g_cok ? 0 : -1;
}

void kol_console_deinit(void){
    if(!g_cok||!g_cbuf)return;
    g_cbuf[0]=1; /* SC_EXIT */
    kol_wait_shell();
    __asm__ __volatile__("int $0x40" : : "a"(68),"b"(23),"c"((int)g_cname) : "memory");   /* fn 68.23: free shared */
    g_cok=0; g_cbuf=NULL;
}

void kol_console_puts(const char *s){
    if(!s)return;
    fprintf(stderr,"%s",s);                        /* always echo to the debug board */
    if(!g_cok||!g_cbuf)return;
    size_t max=1024*16-2, n=strlen(s); if(n>max)n=max;
    g_cbuf[0]=3; /* SC_PUTS */
    memcpy(g_cbuf+1,s,n); g_cbuf[1+n]=0;
    kol_wait_shell();
}

void kol_console_gets(char *buf,int maxlen){
    if(!g_cok||!g_cbuf||!buf||maxlen<=0){ if(buf)buf[0]=0; return; }
    g_cbuf[0]=5; /* SC_GETS */
    if(!kol_wait_shell()){ buf[0]=0; return; }
    { char *d=buf, *end=buf+maxlen-1; const char *src=g_cbuf+1; while(*src && d<end)*d++=*src++; *d=0; }
}

void kol_console_cls(void){
    if(!g_cok||!g_cbuf)return;
    g_cbuf[0]=6; /* SC_CLS */
    kol_wait_shell();
}

/* Both of these format once (via kfmt) and route through kol_console_puts, so
   there is a single formatter and a single output path (console + stderr). */
static void kol_vputs(const char *fmt, va_list ap){
    char buf[512];
    kfmt(buf,(int)sizeof buf,fmt,ap);
    kol_console_puts(buf);
}
void kol_console_printf(const char *fmt, ...){
    va_list ap; va_start(ap,fmt); kol_vputs(fmt,ap); va_end(ap);
}
/* Backs `#define printf` in platform.h, so all core printf() output lands here. */
void _dual_printf(const char *fmt, ...){
    va_list ap; va_start(ap,fmt); kol_vputs(fmt,ap); va_end(ap);
}
