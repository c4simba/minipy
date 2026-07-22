/* ========================= Utility helpers ========================= */

#include "util.h"

void die(const char *m){ fprintf(stderr, "%s\n", m); exit(1); }

void *xmalloc(size_t n){ void *p=malloc(n?n:1); if(!p) die("out of memory"); return p; }
void *xrealloc(void *p,size_t n){ p=realloc(p,n?n:1); if(!p) die("out of memory"); return p; }
char *xstrndup2(const char *s,int n){ char *r=(char*)xmalloc((size_t)n+1); memcpy(r,s,(size_t)n); r[n]=0; return r; }
char *xstrdup2(const char *s){ return xstrndup2(s,(int)strlen(s)); }
