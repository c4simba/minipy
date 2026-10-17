/* ========================= Utility helpers ========================= */

#include "util.h"

void die(const char *m){ fprintf(stderr, "%s\n", m); exit(1); }

void *xmalloc(size_t n){ void *p=malloc(n?n:1); if(!p) die("out of memory"); return p; }
void *xrealloc(void *p,size_t n){ p=realloc(p,n?n:1); if(!p) die("out of memory"); return p; }
char *xstrndup2(const char *s,int n){ char *r=(char*)xmalloc((size_t)n+1); memcpy(r,s,(size_t)n); r[n]=0; return r; }
char *xstrdup2(const char *s){ return xstrndup2(s,(int)strlen(s)); }

/* regions: blocks of 4 KiB (a bigger request gets a block of its own), the
   newest first; an allocation is 8-aligned */
struct PpyRegionBlock { PpyRegionBlock *next; size_t used, cap; size_t last; };
#define REGION_BLOCK 4096
#define REGION_HDR   ((sizeof(PpyRegionBlock)+7)&~(size_t)7)
PpyRegion *ppy_region;
void *ppy_ralloc(size_t n){
    PpyRegion *r=ppy_region;
    if(!r) return xmalloc(n);
    n=(n+7)&~(size_t)7; if(!n) n=8;
    PpyRegionBlock *b=r->blocks;
    if(!b || b->used+n>b->cap){
        size_t cap= n>REGION_BLOCK-REGION_HDR ? n : REGION_BLOCK-REGION_HDR;
        PpyRegionBlock *nb=(PpyRegionBlock*)xmalloc(REGION_HDR+cap); nb->used=0; nb->cap=cap; nb->last=0;
        if(b && cap>REGION_BLOCK-REGION_HDR){ nb->next=b->next; b->next=nb; b=nb; }   /* (a big one: behind the current block, which keeps its room) */
        else { nb->next=b; r->blocks=nb; b=nb; }
    }
    void *p=(char*)b+REGION_HDR+b->used; b->last=b->used; b->used+=n;
    return p;
}
void *ppy_ralloc0(size_t n){ return memset(ppy_ralloc(n),0,n); }
void *ppy_rrealloc(void *p, size_t old, size_t n){
    PpyRegion *r=ppy_region;
    if(!r) return xrealloc(p,n);
    if(!p) return ppy_ralloc(n);
    if(n<=old) return p;
    PpyRegionBlock *b=r->blocks;                          /* the newest allocation grows where it is */
    size_t n8=(n+7)&~(size_t)7;
    if(b && (char*)p==(char*)b+REGION_HDR+b->last && b->last+n8<=b->cap){ b->used=b->last+n8; return p; }
    void *q=ppy_ralloc(n); memcpy(q,p,old); return q;
}
void ppy_rfree(void *p){ if(!ppy_region) free(p); }
char *ppy_rstrndup(const char *s, int n){ char *t=(char*)ppy_ralloc((size_t)n+1); memcpy(t,s,(size_t)n); t[n]=0; return t; }
char *ppy_rstrdup(const char *s){ return ppy_rstrndup(s,(int)strlen(s)); }
void ppy_region_free(PpyRegion *r){
    for(PpyRegionBlock *b=r->blocks,*nx;b;b=nx){ nx=b->next; free(b); }
    r->blocks=NULL;
}

/* Split a raw command-line string into argv (argv[0] = program name/path).
   Tokenizes on whitespace (or "quotes") into the caller's buffer; returns argc. Used on
   platforms (KolibriOS) that hand over a single string instead of argv. */
int ppy_split_cmdline(const char *cmd,const char *exe,const char *argv0_default,char *buf,int buflen,char **argv,int maxargv){
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
