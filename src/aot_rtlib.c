/* ========================= Typed compiler: runtime routines =========================
   The runtime routines of compiled programs are fasm source (aot_rtlib.asm,
   embedded into minipy at build time). The code generator asks for routines by
   name; only those, their dependencies and their data end up in the program. */

#include "aot_model.h"

static const char rtlib_text[] =
#include "aot_rtlib.inc"
;

typedef enum { RB_CODE, RB_DATA, RB_BSS } RtKind;
typedef struct { RtKind kind; char name[48]; int target; /* -1 any */ char deps[8][48]; int ndeps; const char *text; size_t len; } RtBlock;

struct AotRt {
    AotTarget target;
    RtBlock *b; int n;
    char (*used)[48]; int nused, cap;
};

static int parse_target(const char *w){ if(!strcmp(w,"linux")) return AOT_TARGET_LINUX; if(!strcmp(w,"kolibri")) return AOT_TARGET_KOLIBRI; return -2; }

/* Split the library into its ";;; kind name [target] [: deps]" blocks. */
static void rt_parse(AotRt *rt){
    const char *p=rtlib_text; int cap=0;
    RtBlock *cur=NULL;
    while(*p){
        const char *eol=strchr(p,'\n'); if(!eol) eol=p+strlen(p);
        if(eol-p>4 && !strncmp(p,";;; ",4)){
            if(cur) cur->len=(size_t)(p-cur->text);
            if(rt->n==cap){ cap=cap?cap*2:128; rt->b=(RtBlock*)xrealloc(rt->b,sizeof(RtBlock)*(size_t)cap); }
            cur=&rt->b[rt->n++]; memset(cur,0,sizeof *cur); cur->target=-1;
            char line[512]; int ll=(int)(eol-p-4); if(ll>511) ll=511; memcpy(line,p+4,(size_t)ll); line[ll]=0;
            int field=0, deps=0;
            for(char *q=line;;){
                while(*q==' '||*q=='\t'||*q=='\r') q++;
                if(!*q) break;
                char *w=q; while(*q && *q!=' ' && *q!='\t' && *q!='\r') q++;
                if(*q) *q++=0;
                if(!strcmp(w,":")){ deps=1; continue; }
                if(deps){ if(cur->ndeps<8) snprintf(cur->deps[cur->ndeps++],48,"%s",w); continue; }
                if(field==0) cur->kind=!strcmp(w,"data")?RB_DATA:!strcmp(w,"bss")?RB_BSS:RB_CODE;
                else if(field==1) snprintf(cur->name,48,"%s",w);
                else if(parse_target(w)>=-1) cur->target=parse_target(w);
                field++;
            }
            cur->text=*eol?eol+1:eol;
        }
        p=*eol?eol+1:eol;
    }
    if(cur) cur->len=strlen(cur->text);
}

AotRt *aot_rt_new(AotTarget target){
    AotRt *rt=MPY_NEW0(AotRt);
    rt->target=target==AOT_TARGET_MACOS?AOT_TARGET_LINUX:target;          /* macos runs the Linux routines (aot_x2c.c) */
    rt_parse(rt); return rt;
}

int aot_rt_used(AotRt *rt, const char *name){ for(int i=0;i<rt->nused;i++) if(!strcmp(rt->used[i],name)) return 1; return 0; }

void aot_rt_use(AotRt *rt, const char *name){
    if(aot_rt_used(rt,name)) return;
    if(rt->nused==rt->cap){ rt->cap=rt->cap?rt->cap*2:64; rt->used=(char(*)[48])xrealloc(rt->used,48*(size_t)rt->cap); }
    snprintf(rt->used[rt->nused++],48,"%s",name);
    int found=0;
    for(int i=0;i<rt->n;i++){ RtBlock *b=&rt->b[i];
        if(strcmp(b->name,name)||(b->target>=0&&b->target!=(int)rt->target)) continue;
        found=1;
        for(int d=0;d<b->ndeps;d++) aot_rt_use(rt,b->deps[d]);
    }
    if(!found){ fprintf(stderr,"minipy: internal error: runtime routine %s is missing\n",name); exit(1); }
}

/* Append the used blocks of one kind, in library order. */
void aot_rt_emit(AotRt *rt, int kind, void (*put)(void *ctx, const char *s, size_t n), void *ctx){
    for(int i=0;i<rt->n;i++){ RtBlock *b=&rt->b[i];
        if((int)b->kind!=kind||(b->target>=0&&b->target!=(int)rt->target)||!aot_rt_used(rt,b->name)) continue;
        put(ctx,b->text,b->len);
    }
}
