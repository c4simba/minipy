/* ========================= Interpreter: bytecode files =========================
   A module's code objects as bytes: `plainpy --bytecode name.py` writes
   name.ppyc, which the importer and `plainpy name.ppyc` run without the
   source and without compiling it (MicroPython's .mpy); `plainpy
   --bytecode-lib` turns plainpy.lib's modules into them (bytecode.c's
   callers: main.c, modules/import.c, pystdlib.c). The carets' places are
   never written (a traceback compiles the source again for them, when there
   is one); --strip leaves out the docstrings, --strip-lines the line numbers
   too (a traceback then names the functions only).

   Layout (numbers LEB128, signed ones zigzag; a str: its length, its UTF-8):
     "PPYC" version flags file code
     code: name qualname firstline flags argc posonly kwonly stacksize nblocks
           ncode, 0-3 zero bytes (the array 4-aligned from the start), ncode u32
           (little-endian), loc length, loc, carets length, ncarets, carets,
           consts (n, each a tag and its value), names varnames cellnames
           freenames (n, str...), cellarg (ncells, signed), annots (n, each
           0 or 1 and a str), annotations (n, name and text...), doc (a constant)
   Loaded from memory that stays (a mapped plainpy.lib), a code array
   4-aligned there is used where it is (CO_BORROWED): only the objects are
   made in the heap. */

#include "run.h"

#define PPYC_VERSION 1

/* ---------------------------------------------------------------- writing */
static void w_u(SBuf *b, uint64_t v){ do{ char c=(char)((v&127)|(v>127?128:0)); sb_put(b,&c,1); v>>=7; } while(v); }
static void w_s(SBuf *b, int64_t v){ w_u(b,v<0?((uint64_t)(-(v+1))<<1)|1u:(uint64_t)v<<1); }
static void w_str(SBuf *b, const char *s, int64_t n){ w_u(b,(uint64_t)n); sb_put(b,s,n); }
static void w_cstr(SBuf *b, const char *s){ w_str(b,s,(int64_t)strlen(s)); }
static void w_code(SBuf *b, CodeObj *co);
static void w_const(SBuf *b, Value v){
    switch(v.k){
        case V_NONE: sb_putc(b,'N'); return;
        case V_BOOL: sb_putc(b,v.u.i?'T':'F'); return;
        case V_INT: sb_putc(b,'i'); w_s(b,v.u.i); return;
        case V_FLOAT:{ sb_putc(b,'f'); uint64_t u; memcpy(&u,&v.u.f,8); for(int i=0;i<8;i++){ char c=(char)(u>>(8*i)); sb_put(b,&c,1); } return; }
        default: break;
    }
    if(v.k==V_OBJ){
        Type *t=v.u.o->type;
        if(v.u.o==mp_Ellipsis.u.o){ sb_putc(b,'E'); return; }
        if(t==T_str){ sb_putc(b,'s'); w_str(b,AS_STR(v)->s,AS_STR(v)->len); return; }
        if(t==T_bytes){ BytesObj *o=(BytesObj*)v.u.o; sb_putc(b,'b'); w_str(b,(const char*)o->s,o->len); return; }
        if(t==T_complex){ ComplexObj *c=(ComplexObj*)v.u.o; sb_putc(b,'c'); double d[2]={c->re,c->im};
            for(int k=0;k<2;k++){ uint64_t u; memcpy(&u,&d[k],8); for(int i=0;i<8;i++){ char ch=(char)(u>>(8*i)); sb_put(b,&ch,1); } } return; }
        if(t==T_tuple){ TupleObj *o=AS_TUPLE(v); sb_putc(b,'t'); w_u(b,(uint64_t)o->len); for(int64_t i=0;i<o->len;i++) w_const(b,o->items[i]); return; }
        if(t==T_code){ sb_putc(b,'C'); w_code(b,(CodeObj*)v.u.o); return; }
    }
    mp_raise_t(E_ValueError,"a constant of type %s cannot be written as bytecode",TYPE(v)->name->s);
}
static void w_names(SBuf *b, StrObj **n, int k){ w_u(b,(uint64_t)k); for(int i=0;i<k;i++) w_str(b,n[i]->s,n[i]->len); }
static void w_code(SBuf *b, CodeObj *co){
    w_str(b,co->name->s,co->name->len); w_str(b,co->qualname->s,co->qualname->len);
    w_u(b,(uint64_t)co->firstline); w_u(b,(uint64_t)(co->flags&~CO_BORROWED)); w_u(b,(uint64_t)co->argc); w_u(b,(uint64_t)co->posonly);
    w_u(b,(uint64_t)co->kwonly); w_u(b,(uint64_t)co->stacksize); w_u(b,(uint64_t)co->nblocks);
    w_u(b,(uint64_t)co->ncode);
    while(b->n%4) sb_putc(b,0);
    for(int i=0;i<co->ncode;i++){ uint32_t c=co->code[i]; char q[4]={(char)c,(char)(c>>8),(char)(c>>16),(char)(c>>24)}; sb_put(b,q,4); }
    int nloc=0;                                          /* (its length: the runs walked) */
    if(co->loc && !(mp_strip&MP_STRIP_LINES)){
        const uint8_t *p=co->loc+1; int places=co->loc[0]==1, at=0;
        while(at<co->ncode){ uint64_t run=0; int sh=0; uint8_t c; do{ c=*p++; run|=(uint64_t)(c&127)<<sh; sh+=7; } while(c&128);
            for(int f=0;f<(places?4:1);f++) while(*p++&128);
            at+=(int)run; }
        nloc=(int)(p-co->loc);
    }
    w_u(b,(uint64_t)nloc); if(nloc) sb_put(b,(const char*)co->loc,nloc);
    w_u(b,0); w_u(b,0);                                  /* (the carets: never written) */
    w_u(b,(uint64_t)co->nconsts);
    for(int i=0;i<co->nconsts;i++) w_const(b,co->consts[i]);
    w_names(b,co->names,co->nnames); w_names(b,co->varnames,co->nlocals); w_names(b,co->cellnames,co->ncells); w_names(b,co->freenames,co->nfrees);
    for(int i=0;i<co->ncells;i++) w_s(b,co->cellarg[i]);
    int np= co->annots ? co->argc+co->kwonly+2 : 0;
    w_u(b,(uint64_t)np);
    for(int i=0;i<np;i++){ if(co->annots[i]){ sb_putc(b,1); w_cstr(b,co->annots[i]); } else sb_putc(b,0); }
    w_u(b,(uint64_t)co->nann);
    for(int i=0;i<co->nann;i++){ w_cstr(b,co->annname[i]); w_cstr(b,co->anntext[i]); }
    w_const(b,co->doc);
}
/* the bytes of a module's code (what is left out: mp_strip) */
Value mp_bytecode_dump(CodeObj *co, const char *file){
    SBuf b={0};
    sb_put(&b,"PPYC",4); sb_putc(&b,PPYC_VERSION); sb_putc(&b,(char)(mp_strip&(MP_STRIP_DOCS|MP_STRIP_LINES)));
    w_cstr(&b,file);
    w_code(&b,co);
    Value r=mp_bytes(b.s,b.n); free(b.s);
    return r;
}
int mp_is_bytecode(const void *data, size_t len){ return len>=6 && !memcmp(data,"PPYC",4); }

/* ---------------------------------------------------------------- reading */
typedef struct { const uint8_t *p, *end, *base; int borrow; const char *file; } R;
PPY_NORETURN static void bad(void){ mp_raise_t(E_ValueError,"bad bytecode data (a file of another plainpy, or damaged)"); }
static uint64_t r_u(R *r){ uint64_t v=0; int sh=0; uint8_t c; do{ if(r->p>=r->end || sh>63) bad(); c=*r->p++; v|=(uint64_t)(c&127)<<sh; sh+=7; } while(c&128); return v; }
static int64_t r_s(R *r){ uint64_t v=r_u(r); return v&1 ? -(int64_t)(v>>1)-1 : (int64_t)(v>>1); }
static int r_int(R *r){ uint64_t v=r_u(r); if(v>0x7fffffff) bad(); return (int)v; }
static const char *r_bytes(R *r, int64_t *n){ uint64_t k=r_u(r); if(k>(uint64_t)(r->end-r->p)) bad(); const char *s=(const char*)r->p; r->p+=k; *n=(int64_t)k; return s; }
static Value r_strv(R *r){ int64_t n; const char *s=r_bytes(r,&n); return mp_strn(s,n); }
static char *r_cstr(R *r){ int64_t n; const char *s=r_bytes(r,&n); return xstrndup2(s,(int)n); }
static StrObj *r_name(R *r){
    int64_t n; const char *s=r_bytes(r,&n); char buf[256];
    if(n<(int64_t)sizeof buf && !memchr(s,0,(size_t)n)){ memcpy(buf,s,(size_t)n); buf[n]=0; return AS_STR(mp_intern(buf)); }
    return AS_STR(mp_strn(s,n));
}
static double r_double(R *r){ if(r->end-r->p<8) bad(); uint64_t u=0; for(int i=0;i<8;i++) u|=(uint64_t)r->p[i]<<(8*i); r->p+=8; double d; memcpy(&d,&u,8); return d; }
static CodeObj *r_code(R *r);
static Value r_const(R *r){
    if(r->p>=r->end) bad();
    switch(*r->p++){
        case 'N': return v_none();
        case 'T': return v_bool(1);
        case 'F': return v_bool(0);
        case 'E': return mp_Ellipsis;
        case 'i': return v_int(r_s(r));
        case 'f': return v_float(r_double(r));
        case 'c':{ double re=r_double(r), im=r_double(r); return mp_complex(re,im); }
        case 's': return r_strv(r);
        case 'b':{ int64_t n; const char *s=r_bytes(r,&n); return mp_bytes(s,n); }
        case 't':{ uint64_t n=r_u(r); if(n>(uint64_t)(r->end-r->p)) bad();
            Value t=mp_tuple((int64_t)n,NULL); TupleObj *o=AS_TUPLE(t);
            for(int64_t i=0;i<(int64_t)n;i++) o->items[i]=v_none();
            for(int64_t i=0;i<(int64_t)n;i++) o->items[i]=r_const(r);
            return t; }
        case 'C': return v_obj(r_code(r));
        default: bad();
    }
}
static StrObj **r_names(R *r, int *count){
    int n=r_int(r); if(n>r->end-r->p) bad();
    StrObj **v=(StrObj**)xmalloc(sizeof(StrObj*)*(size_t)(n+1)); memset(v,0,sizeof(StrObj*)*(size_t)(n+1));
    *count=n;
    return v;
}
static void r_fill(R *r, StrObj **v, int n){ for(int i=0;i<n;i++) v[i]=r_name(r); }
static CodeObj *r_code(R *r){
    CodeObj *co=(CodeObj*)mp_alloc(T_code,sizeof(CodeObj));      /* (a local: the collector sees it on the stack) */
    co->doc=v_none(); co->file=r->file;
    co->name=AS_STR(mp_intern(mp_cstr(r_strv(r)))); co->qualname=AS_STR(r_strv(r));
    co->firstline=r_int(r); int flags=r_int(r); co->argc=r_int(r); co->posonly=r_int(r);
    co->kwonly=r_int(r); co->stacksize=r_int(r); co->nblocks=r_int(r);
    int n=r_int(r);
    while((r->p-r->base)%4){ if(r->p>=r->end) bad(); r->p++; }
    if((uint64_t)n*4>(uint64_t)(r->end-r->p)) bad();
    uint16_t one=1; int le=*(uint8_t*)&one==1;
    if(r->borrow && le && ((uintptr_t)r->p&3)==0){ co->code=(uint32_t*)(void*)r->p; flags|=CO_BORROWED; }
    else { co->code=(uint32_t*)xmalloc(sizeof(uint32_t)*(size_t)(n?n:1));
        for(int i=0;i<n;i++) co->code[i]=(uint32_t)r->p[4*i]|(uint32_t)r->p[4*i+1]<<8|(uint32_t)r->p[4*i+2]<<16|(uint32_t)r->p[4*i+3]<<24;
        flags&=~CO_BORROWED; }
    co->ncode=n; r->p+=4*(size_t)n;
    int64_t nl; const char *loc=r_bytes(r,&nl);
    if(nl){ if(flags&CO_BORROWED) co->loc=(uint8_t*)(void*)loc; else { co->loc=(uint8_t*)xmalloc((size_t)nl); memcpy(co->loc,loc,(size_t)nl); } }
    int64_t nc; (void)r_bytes(r,&nc); (void)r_u(r);       /* (carets: none written) */
    co->flags=flags;
    int k=r_int(r); if(k>r->end-r->p) bad();
    co->consts=(Value*)xmalloc(sizeof(Value)*(size_t)(k+1));
    for(int i=0;i<k;i++) co->consts[i]=v_none();
    co->nconsts=k;
    for(int i=0;i<k;i++) co->consts[i]=r_const(r);
    int cnt;
    co->names=r_names(r,&cnt); co->nnames=cnt; r_fill(r,co->names,cnt);
    co->varnames=r_names(r,&cnt); co->nlocals=cnt; r_fill(r,co->varnames,cnt);
    co->cellnames=r_names(r,&cnt); co->ncells=cnt; r_fill(r,co->cellnames,cnt);
    co->freenames=r_names(r,&cnt); co->nfrees=cnt; r_fill(r,co->freenames,cnt);
    co->cellarg=(int*)xmalloc(sizeof(int)*(size_t)(co->ncells+1));
    for(int i=0;i<co->ncells;i++){ int64_t a=r_s(r); co->cellarg[i]= a<-1||a>=co->nlocals ? -1 : (int)a; }
    int np=r_int(r);
    if(np){ if(np!=co->argc+co->kwonly+2) bad();
        co->annots=(char**)xmalloc(sizeof(char*)*(size_t)np); memset(co->annots,0,sizeof(char*)*(size_t)np);
        for(int i=0;i<np;i++){ if(r->p>=r->end) bad(); if(*r->p++) co->annots[i]=r_cstr(r); } }
    int na=r_int(r); if(na>r->end-r->p) bad();
    if(na){ co->annname=(char**)xmalloc(sizeof(char*)*(size_t)(na+2)); co->anntext=(char**)xmalloc(sizeof(char*)*(size_t)(na+2));
        for(int i=0;i<na;i++){ co->annname[i]=r_cstr(r); co->anntext[i]=r_cstr(r); co->nann=i+1; } }
    co->doc=r_const(r);
    if(co->stacksize<1 || co->nblocks<1) bad();
    return co;
}
/* a module's code from those bytes (borrow: they stay as long as the program runs - its code
   arrays are used where they are); file: what its tracebacks name (NULL: the one written) */
CodeObj *mp_bytecode_load(const void *data, size_t len, const char *file, int borrow){
    R r; r.base=r.p=(const uint8_t*)data; r.end=r.p+len; r.borrow=borrow; r.file=NULL;
    if(!mp_is_bytecode(data,len)) bad();
    if(r.p[4]!=PPYC_VERSION) mp_raise_t(E_ValueError,"bytecode of another version of plainpy (%d; this one reads %d)",r.p[4],PPYC_VERSION);
    r.p+=6;
    char *written=r_cstr(&r);
    if(file){ free(written); written=xstrdup2(file); }
    r.file=written;                                       /* (the code objects' file: kept for good, as the compiler's) */
    return r_code(&r);
}
