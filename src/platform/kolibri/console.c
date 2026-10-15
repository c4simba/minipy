/* ========================= KolibriOS console via shared memory =========================
 * The console is a named shared buffer "{PID}-SHELL" (created with f68.22, freed
 * with f68.23). The buffer is a header followed by a byte-oriented ring buffer:
 *
 *   +0    write_ptr   next byte we (client) will write   (we own)
 *   +4    read_ptr    next byte the shell will read       (shell owns)
 *   +8    resp_ready  shell sets 1 when a reply is ready
 *   +12   resp_len    length of the reply payload
 *   +16   resp[1024]  reply payload (shell -> us)
 *   +1040 ring[]       command stream (us -> shell)
 *
 * The command stream is a sequence of variable-length frames:
 *
 *   [cmd:1][len_lo:1][len_hi:1][payload:len]
 *
 * We append output frames (SC_PUTS/SC_CLS) into the ring and only publish
 * write_ptr once a whole frame is written; the shell drains every pending frame
 * on each poll. This means many puts() calls no longer block one-at-a-time -- we
 * only wait if the ring is full. Commands that need an answer (SC_GETS, SC_EXIT)
 * clear resp_ready, queue the request and wait for the shell to fill resp[].
 *   SC_EXIT=1  SC_PUTC=2  SC_PUTS=3  SC_GETC=4  SC_GETS=5  SC_CLS=6  SC_PID=7  SC_PING=8
 * All output is also echoed to stderr (the debug board).
 */

#include "platform/platform.h"

/* ---- protocol constants (must match the shell's program_console.h) -------- */
#define SC_EXIT 1
#define SC_PUTC 2
#define SC_PUTS 3
#define SC_GETC 4
#define SC_GETS 5
#define SC_CLS  6
#define SC_PID  7
#define SC_PING 8

#define SC_SHM_SIZE  (1024*16)
#define SC_RESP_MAX  1024
#define SC_OFF_WRITE 0
#define SC_OFF_READ  4
#define SC_OFF_RESP  8
#define SC_OFF_RLEN  12
#define SC_OFF_RDATA 16
#define SC_DATA_OFF  (SC_OFF_RDATA + SC_RESP_MAX)   /* 1040 */
#define SC_RING_SIZE (SC_SHM_SIZE - SC_DATA_OFF)    /* 15344 */
#define SC_FRAME_HDR 3

/* poll granularity and bounded waits. Output/exit are bounded so a shell that
   stops draining can't hang us; interactive input waits far longer for a human. */
#define POLL_MS                   50
#define CONSOLE_OP_TIMEOUT_MS     10000    /* queue room + exit ack */
#define CONSOLE_INPUT_TIMEOUT_MS  300000   /* input(): up to 5 min for the user */

/* Echoing every line to the debug board is an unbuffered, per-character syscall
   path that badly throttles bulk output, so it's off by default. Build with
   -DMPY_CONSOLE_ECHO_STDERR=1 to re-enable it for debugging. */
#ifndef MPY_CONSOLE_ECHO_STDERR
#define MPY_CONSOLE_ECHO_STDERR 0
#endif

static char *g_cbuf = NULL;    /* shared buffer base (header + ring) */
static char  g_cname[32];      /* "{PID}-SHELL" */
static int   g_cok = 0;

/* ---- ring transport primitives ------------------------------------------- */

#define HDR_U32(off) (*(volatile unsigned*)(g_cbuf + (off)))

static void kol_poll_delay(void){
    __asm__ __volatile__("int $0x40" :: "a"(5), "b"(POLL_MS/10) : "memory");   /* fn 5: delay */
}

/* free space in the ring (one byte is kept unused to tell full from empty) */
static unsigned kol_ring_free(void){
    unsigned wp = HDR_U32(SC_OFF_WRITE);
    unsigned rp = HDR_U32(SC_OFF_READ);
    unsigned used = (wp - rp + SC_RING_SIZE) % SC_RING_SIZE;
    return SC_RING_SIZE - 1 - used;
}

/* Queue one [cmd][len][payload] frame. Blocks while the ring is full, but only
   up to CONSOLE_OP_TIMEOUT_MS -- if the shell never drains we drop the frame
   instead of hanging. Returns 1 if written, 0 otherwise. */
static int kol_send(unsigned char cmd, const void *payload, unsigned len){
    if(!g_cok || !g_cbuf) return 0;

    unsigned total = SC_FRAME_HDR + len;
    if(total > SC_RING_SIZE - 1){                 /* never fits: truncate */
        len = SC_RING_SIZE - 1 - SC_FRAME_HDR;
        total = SC_RING_SIZE - 1;
    }

    /* Wait for room. Normally the shell drains within a few scheduler yields,
       so yield (fn 68.1, ~immediate) instead of sleeping - this is what keeps
       bulk output fast. Only if the ring stays full for a long time (no shell
       draining) fall back to bounded sleeps and eventually drop the frame. */
    {
        int slow = CONSOLE_OP_TIMEOUT_MS / POLL_MS;
        while(kol_ring_free() < total){
            int k;
            for(k = 0; k < 1024 && kol_ring_free() < total; k++)
                mpy_thread_yield();
            if(kol_ring_free() >= total) break;
            if(--slow < 0) return 0;
            kol_poll_delay();
        }
    }

    unsigned char *ring = (unsigned char*)g_cbuf + SC_DATA_OFF;
    const unsigned char *p = (const unsigned char*)payload;
    unsigned wp = HDR_U32(SC_OFF_WRITE);

    ring[wp] = cmd;                                wp++; if(wp==SC_RING_SIZE) wp=0;
    ring[wp] = (unsigned char)(len & 0xff);        wp++; if(wp==SC_RING_SIZE) wp=0;
    ring[wp] = (unsigned char)((len >> 8) & 0xff); wp++; if(wp==SC_RING_SIZE) wp=0;
    for(unsigned i=0;i<len;i++){ ring[wp]=p[i];    wp++; if(wp==SC_RING_SIZE) wp=0; }

    HDR_U32(SC_OFF_WRITE) = wp;                   /* publish the whole frame */
    return 1;
}

/* Queue a request frame and wait for the shell to place a reply in resp[].
   timeout_ms <= 0 means wait indefinitely. Returns 1 on reply, 0 on timeout. */
static int kol_request(unsigned char cmd, const void *payload, unsigned len, int timeout_ms){
    if(!g_cok || !g_cbuf) return 0;

    HDR_U32(SC_OFF_RESP) = 0;
    if(!kol_send(cmd, payload, len)) return 0;

    int tries = timeout_ms > 0 ? timeout_ms / POLL_MS : -1;   /* -1 => forever */
    while(!HDR_U32(SC_OFF_RESP)){
        if(tries == 0) return 0;
        if(tries > 0) tries--;
        kol_poll_delay();
    }
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
        : "a"(68), "b"(22), "c"((int)g_cname), "d"(SC_SHM_SIZE), "S"(0x04|0x01)   /* fn 68.22: open shared */
        : "memory");
    if(ptr_val > 0 && (int)ptr_val > 0x1000){
        g_cbuf = (char*)ptr_val;
        HDR_U32(SC_OFF_WRITE) = 0;      /* start with an empty ring, no pending reply */
        HDR_U32(SC_OFF_READ)  = 0;
        HDR_U32(SC_OFF_RESP)  = 0;
        HDR_U32(SC_OFF_RLEN)  = 0;
        g_cok = 1;
    }
    return g_cok ? 0 : -1;
}

void kol_console_deinit(void){
    if(!g_cok||!g_cbuf)return;
    kol_request(SC_EXIT, NULL, 0, CONSOLE_OP_TIMEOUT_MS);   /* wait for the shell to drain & ack */
    __asm__ __volatile__("int $0x40" : : "a"(68),"b"(23),"c"((int)g_cname) : "memory");   /* fn 68.23: free shared */
    g_cok=0; g_cbuf=NULL;
}

void kol_console_puts(const char *s){
    if(!s)return;
#if MPY_CONSOLE_ECHO_STDERR
    fprintf(stderr,"%s",s);                        /* debug-board echo (slow; off by default) */
#endif
    if(!g_cok||!g_cbuf)return;
    /* send the string together with its terminating '\0' so the shell can print
       the frame payload directly */
    kol_send(SC_PUTS, s, (unsigned)strlen(s) + 1);
}

void kol_console_gets(char *buf,int maxlen){
    if(!g_cok||!g_cbuf||!buf||maxlen<=0){ if(buf)buf[0]=0; return; }
    unsigned max = (unsigned)maxlen;               /* tell the shell our capacity */
    if(!kol_request(SC_GETS, &max, sizeof(max), CONSOLE_INPUT_TIMEOUT_MS)){ buf[0]=0; return; }
    { char *d=buf, *end=buf+maxlen-1; const char *src=g_cbuf+SC_OFF_RDATA; while(*src && d<end)*d++=*src++; *d=0; }
}

void kol_console_cls(void){
    if(!g_cok||!g_cbuf)return;
    kol_send(SC_CLS, NULL, 0);
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
