/* ========================= Utility helpers ========================= */

#include "util.h"

void die(const char *m){ fprintf(stderr, "%s\n", m); exit(1); }

void *xmalloc(size_t n){ void *p=malloc(n?n:1); if(!p) die("out of memory"); return p; }
void *xrealloc(void *p,size_t n){ p=realloc(p,n?n:1); if(!p) die("out of memory"); return p; }
char *xstrndup2(const char *s,int n){ char *r=(char*)xmalloc((size_t)n+1); memcpy(r,s,(size_t)n); r[n]=0; return r; }
char *xstrdup2(const char *s){ return xstrndup2(s,(int)strlen(s)); }

/* Split a raw command-line string into argv (argv[0] = program name/path).
   Tokenizes on whitespace (or "quotes") into the caller's buffer; returns argc. Used on
   platforms (KolibriOS) that hand over a single string instead of argv. */
int mpy_split_cmdline(const char *cmd,const char *exe,const char *argv0_default,char *buf,int buflen,char **argv,int maxargv){
    int argc=0;
    argv[argc++]=(char*)((exe && exe[0]) ? exe : argv0_default);   /* argv[0], read-only */
    int n=0; if(cmd){ while(cmd[n] && n<buflen-1){ buf[n]=cmd[n]; n++; } } buf[n]=0;
    char *p=buf;
    while(*p && argc<maxargv){
        while(*p==' '||*p=='\t') p++;
        if(!*p) break;
        if(*p=='"'){                                          /* "an argument with spaces" */
            argv[argc++]=++p;
            while(*p && *p!='"') p++;
            if(*p) *p++=0;
            continue; }
        argv[argc++]=p;
        while(*p && *p!=' ' && *p!='\t') p++;
        if(*p) *p++=0;
    }
    return argc;
}
