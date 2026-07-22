/* ========================= KolibriOS Console via Shared Memory =========================
 * Uses named buffer "{PID}-SHELL" (f68.22/f68.23).
 * Shell command codes: SC_EXIT=1 SC_PUTC=2 SC_PUTS=3 SC_GETC=4 SC_GETS=5 SC_CLS=6
 * All output echoed to stderr AND written to console buffer.
 */

#include "platform/platform.h"

static char *g_cbuf = NULL;
static char  g_cname[32];
static int   g_cok = 0;

int kol_console_init(void){
    char *b = malloc(1024); if(!b) return -1;
    memset(b,0,1024);
    __asm__ __volatile__("int $0x40" : : "a"(9),"b"((int)b),"c"(-1) : "memory");
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
        : "a"(68), "b"(22), "c"((int)g_cname), "d"(1024*16), "S"(0x04|0x01)
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
    {int c=0;while(g_cbuf[0]&&c<200){__asm__ __volatile__("int $0x40"::"a"(5),"b"(5):"memory");c++;}}
    __asm__ __volatile__("int $0x40" : : "a"(68),"b"(23),"c"((int)g_cname) : "memory");
    g_cok=0; g_cbuf=NULL;
}

void kol_console_puts(const char *s){
    if(!s)return;
    /* Write to BOTH stderr and console buffer */
    fprintf(stderr,"%s",s);
    if(!g_cok||!g_cbuf)return;
    size_t n=strlen(s); if(n>1024*16-2)n=1024*16-2;
    g_cbuf[0]=3; /* SC_PUTS */
    { char *d=g_cbuf+1; const char *src=s; while(*src)*d++=*src++; *d=0; }
    /* Wait for shell - same as original: while(*buffer) kol_sleep(5) */
    {int c=0;while(g_cbuf[0]&&c<200){__asm__ __volatile__("int $0x40"::"a"(5),"b"(5):"memory");c++;}}
    if(g_cbuf[0]) g_cbuf[0]=0;
}

void kol_console_gets(char *buf,int maxlen){
    if(!g_cok||!g_cbuf||!buf||maxlen<=0){if(buf)buf[0]=0;return;}
    g_cbuf[0]=5; /* SC_GETS */
    {int c=0;while(g_cbuf[0]&&c<200){__asm__ __volatile__("int $0x40"::"a"(5),"b"(5):"memory");c++;}}
    if(g_cbuf[0]){buf[0]=0;g_cbuf[0]=0;return;}
    { char *d=buf; const char *src=g_cbuf+1; while(*src)*d++=*src++; *d=0; }
}

void kol_console_cls(void){
    if(!g_cok||!g_cbuf)return;
    g_cbuf[0]=6; /* SC_CLS */
    {int c=0;while(g_cbuf[0]&&c<200){__asm__ __volatile__("int $0x40"::"a"(5),"b"(5):"memory");c++;}}
    if(g_cbuf[0]) g_cbuf[0]=0;
}

void kol_console_printf(const char *fmt,...){
    va_list args; va_start(args,fmt);
    char buf[512]; int pos=0;
    for(const char *p=fmt;*p&&pos<(int)sizeof(buf)-32;p++){
        if(*p!='%'){buf[pos++]=*p;continue;}
        p++; if(*p==0)break;
        int w=0,pr=-1,lj=0,zp=0,ll=0;
        while(*p=='-'||*p=='0'){if(*p=='-')lj=1;if(*p=='0')zp=1;p++;}
        while(*p>='0'&&*p<='9'){w=w*10+(*p-'0');p++;}
        if(*p=='.'){p++;pr=0;while(*p>='0'&&*p<='9'){pr=pr*10+(*p-'0');p++;}}
        if(*p=='l'){p++;if(*p=='l'){ll=1;p++;}}
        if(*p=='s'){const char *s=va_arg(args,const char*);if(!s)s="(null)";
            int sl=(int)strlen(s),rm=(int)sizeof(buf)-32-pos;if(sl>rm)sl=rm;memcpy(buf+pos,s,(size_t)sl);pos+=sl;}
        else if(*p=='d'||*p=='i'){char t[32];int neg=0;long long v=ll?va_arg(args,long long):va_arg(args,int);
            if(v<0){neg=1;v=-v;}int ti=0;if(v==0)t[ti++]='0';
            while(v>0&&ti<30){t[ti++]=(char)('0'+v%10);v/=10;}int nl=ti+neg,pd=w>nl?w-nl:0;
            if(!lj&&pd>0&&!zp)while(pd-->0&&pos<(int)sizeof(buf)-1)buf[pos++]=' ';
            if(neg&&pos<(int)sizeof(buf)-1)buf[pos++]='-';
            if(!lj&&pd>0&&zp)while(pd-->0&&pos<(int)sizeof(buf)-1)buf[pos++]='0';
            while(ti>0&&pos<(int)sizeof(buf)-1)buf[pos++]=t[--ti];
            if(lj&&pd>0)while(pd-->0&&pos<(int)sizeof(buf)-1)buf[pos++]=' ';}
        else if(*p=='x'||*p=='X'){char t[32];unsigned long long v=ll?va_arg(args,unsigned long long):(unsigned long long)va_arg(args,unsigned);
            int ti=0;if(v==0)t[ti++]='0';while(v>0&&ti<30){int d=(int)(v%16);t[ti++]=(char)(d<10?'0'+d:(*p=='X'?'A':'a')+d-10);v/=16;}
            while(ti>0&&pos<(int)sizeof(buf)-1)buf[pos++]=t[--ti];}
        else if(*p=='c'){if(pos<(int)sizeof(buf)-1)buf[pos++]=(char)va_arg(args,int);}
        else if(*p=='%')buf[pos++]='%';
        else if(*p=='g'||*p=='f'||*p=='e'){double d=va_arg(args,double);char t[64];int neg=0;
            if(d<0){neg=1;d=-d;}long long ip=(long long)d;double fp=d-(double)ip;
            char t2[32];int ti=0;long long iv=ip;
            if(iv==0)t2[ti++]='0';while(iv>0&&ti<30){t2[ti++]=(char)('0'+(iv%10));iv/=10;}
            int xp=0;if(neg)t[xp++]='-';while(ti>0)t[xp++]=t2[--ti];
            if(pr!=0){t[xp++]='.';int prc=pr>=0?pr:6;for(int i=0;i<prc&&i<15;i++){fp*=10.0;int dg=(int)fp;fp-=dg;t[xp++]=(char)('0'+dg);}}
            t[xp]=0;int sl=(int)strlen(t),rm=(int)sizeof(buf)-32-pos;if(sl>rm)sl=rm;memcpy(buf+pos,t,(size_t)sl);pos+=sl;}
        else{if(pos<(int)sizeof(buf)-1)buf[pos++]='?';}
    }
    buf[pos]=0;
    kol_console_puts(buf);
    va_end(args);
}

/* Dual printf: format to a buffer, echo to stderr AND the console.
   Handles %s, %d, %lld, %.15g, %c, %p, %%. Backs the `#define printf` in
   platform.h so all core printf() output reaches the KolibriOS console. */
void _dual_printf(const char *fmt, ...){
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
