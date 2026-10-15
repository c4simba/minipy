/* ========================= Typed compiler, code generator: state =========================
   Text buffers, emitting instructions and labels, value kinds and their
   runtime descriptors (kd), literal pools, functions and classes used. */

#include "gen.h"

void buf_put(Buf *b, const char *s, size_t n){
    if(b->len+n+1>b->cap){ size_t c=b->cap?b->cap:4096; while(b->len+n+1>c) c*=2; b->s=(char*)xrealloc(b->s,c); b->cap=c; }
    memcpy(b->s+b->len,s,n); b->len+=n; b->s[b->len]=0;
}
static void buf_vprintf(Buf *b, const char *fmt, va_list ap){
    char tmp[512]; va_list ap2; va_copy(ap2,ap);
    int n=vsnprintf(tmp,sizeof tmp,fmt,ap);
    if(n<(int)sizeof tmp) buf_put(b,tmp,(size_t)n);
    else { char *big=(char*)xmalloc((size_t)n+1); vsnprintf(big,(size_t)n+1,fmt,ap2); buf_put(b,big,(size_t)n); free(big); }
    va_end(ap2);
}
void buf_printf(Buf *b, const char *fmt, ...){ va_list ap; va_start(ap,fmt); buf_vprintf(b,fmt,ap); va_end(ap); }
void buf_cat(Buf *b, Buf *x){ if(x->len) buf_put(b,x->s,x->len); }
void put_cb(void *ctx, const char *s, size_t n){ buf_put((Buf*)ctx,s,n); }

/* `db` operands for arbitrary bytes */
/* the length of UTF-8 text in code points, counted like the runtime (and
   the interpreter): a byte that does not start a valid sequence is one */
int u8_cplen(const char *s, int n){
    const unsigned char *u=(const unsigned char*)s; int c=0;
    for(int i=0;i<n;c++){
        unsigned b=u[i]; int len= b<0xC0 ? 1 : b<0xE0 ? 2 : b<0xF0 ? 3 : b<0xF8 ? 4 : 1;
        if(len>1 && i+len<=n){ int k=1; while(k<len && (u[i+k]&0xC0)==0x80) k++; if(k<len) len=1; }
        else len=1;
        i+=len;
    }
    return c;
}
void buf_bytes(Buf *b, const char *s, int n, int nul){
    buf_put(b,"db ",3);
    int first=1;
    for(int i=0;i<n;){
        unsigned char c=(unsigned char)s[i];
        if(!first) buf_put(b,",",1);
        first=0;
        if(c>=32 && c<127 && c!='\''){
            int j=i; while(j<n && (unsigned char)s[j]>=32 && (unsigned char)s[j]<127 && s[j]!='\'') j++;
            buf_put(b,"'",1); buf_put(b,s+i,(size_t)(j-i)); buf_put(b,"'",1); i=j;
        } else { buf_printf(b,"%u",c); i++; }
    }
    if(nul){ if(!first) buf_put(b,",",1); buf_put(b,"0",1); first=0; }
    if(first) buf_put(b,"0",1);
    buf_put(b,"\n",1);
}

/* ---------------------------------------------------------------- generator state */

G *gg;                   /* the generator (one compilation at a time) */

int new_label(void){ return ++gg->labels; }
void rt(const char *name){ aot_rt_use(gg->rt,name); }

void E(F *f, const char *fmt, ...){ va_list ap; va_start(ap,fmt); buf_put(&f->code,"        ",8); buf_vprintf(&f->code,fmt,ap); buf_put(&f->code,"\n",1); va_end(ap); }
void LBL(F *f, int l){ buf_printf(&f->code,"L%d:\n",l); }
void CALLRT(F *f, const char *name){ rt(name); E(f,"call %s",name); }

Ty *TY(Expr *e){ return ty_find(xinfo(e)->ty); }
int is_ptr(Ty *t){ return ty_is_ptr(t); }
int is_flt(Ty *t){ return ty_find(t)->k==TY_FLOAT; }
int is_lng(Ty *t){ return ty_find(t)->k==TY_INT; }     /* an int: 64 bits in edx:eax */
/* The runtime's descriptor (kd) of values of type t: their size, whether they
   are references, how they compare, hash and print (aot/runtime). Classes
   get their own (class_kd), using __eq__, __lt__, __hash__ and their repr. */
const char *kd_of(Ty *t){
    t=ty_find(t);
    const char *n;
    switch(t->k){
        case TY_BOOL: n=t->opt?"rt_kd_obool":"rt_kd_bool"; break;    /* optional: None in the type's reserved value */
        case TY_INT: n=t->opt?"rt_kd_oint":"rt_kd_int"; break;
        case TY_FLOAT: n=t->opt?"rt_kd_ofloat":"rt_kd_float"; break;
        case TY_STR: n="rt_kd_str"; break;
        case TY_BYTES: n="rt_kd_bytes"; break;
        case TY_TYPE: n="rt_kd_type"; break;
        case TY_TUPLE: n=gc_type(t)?"rt_kd_tuple_c":"rt_kd_tuple"; break;   /* (_c: values that can be in a cycle) */
        case TY_LIST: n=gc_type(t)?"rt_kd_list_c":"rt_kd_list"; break;
        case TY_DICT: n=gc_type(t)?"rt_kd_dict_c":"rt_kd_dict"; break;
        case TY_SET: n=gc_type(t)?"rt_kd_set_c":"rt_kd_set"; break;
        case TY_OBJ: return class_kd(t->cls);
        case TY_VAR: case TY_VOID: n="rt_kd_word"; break;       /* never filled */
        default: n=gc_type(t)?"rt_kd_obj_c":"rt_kd_obj"; break;  /* functions, files ...: identity */
    }
    rt(n); return n;
}
int esize(Ty *t){ return ty_size(t); }
/* a new empty dict of type t -> eax (owned) */
void new_dict(F *f, Ty *t){
    E(f,"mov eax,%s",kd_of(ty_dkey(t))); E(f,"mov edx,%s",kd_of(ty_find(t)->elem)); CALLRT(f,"rt_dict_new");
}
void new_list(F *f, Ty *elem){ E(f,"mov eax,%s",kd_of(elem)); CALLRT(f,"rt_list_new"); }
void new_set(F *f, Ty *elem){ E(f,"mov eax,%s",kd_of(elem)); CALLRT(f,"rt_set_new"); }

/* ---- literal pools ---- */
static int lit_index(Lit **v, int *n, int *cap, const char *s, int len){
    for(int i=0;i<*n;i++) if((*v)[i].len==len && memcmp((*v)[i].s,s,(size_t)len)==0) return i;
    if(*n==*cap){ *cap=*cap?*cap*2:32; *v=(Lit*)xrealloc(*v,sizeof(Lit)*(size_t)*cap); }
    (*v)[*n].s=xstrndup2(s,len); (*v)[*n].len=len; return (*n)++;
}
int str_lit(const char *s){ rt("rt_static"); return lit_index(&gg->lits,&gg->nlits,&gg->clits,s,(int)strlen(s)); }
int print_lit(const char *s, int n){ return lit_index(&gg->plits,&gg->nplits,&gg->cplits,s,n); }
int str_litn(const char *s, int n){ rt("rt_static"); return lit_index(&gg->lits,&gg->nlits,&gg->clits,s,n); }
int bytes_lit(const char *s, int n){ rt("rt_static"); return lit_index(&gg->blits,&gg->nblits,&gg->cblits,s,n); }
int zlit(const char *s){ return lit_index(&gg->zlits,&gg->nzlits,&gg->czlits,s,(int)strlen(s)); }
int rlit(const char *s, int n){ return lit_index(&gg->rlits,&gg->nrlits,&gg->crlits,s,n); }
/* A float literal's source text in fasm's syntax ("1e3" -> "1.0e3"); fasm does
   the decimal -> binary conversion, correctly rounded. */
int float_const(const char *src){
    char t[128], o[140]; int k=0, j=0;
    for(const char *q=src;*q && k<127;q++) if(*q!='_') t[k++]=*q;
    t[k]=0;
    int dot=strchr(t,'.')!=NULL;
    if(t[0]=='.') o[j++]='0';
    for(int i=0;t[i];i++){
        if((t[i]=='e'||t[i]=='E') && !dot){ o[j++]='.'; o[j++]='0'; dot=1; }
        o[j++]=t[i];
        if(t[i]=='.' && !(t[i+1]>='0'&&t[i+1]<='9')) o[j++]='0';
    }
    if(!dot){ o[j++]='.'; o[j++]='0'; }
    o[j]=0;
    return lit_index(&gg->flts,&gg->nflts,&gg->cflts,o,j);
}

void use_fn(AFunc *fn){
    if(fn->used || fn->unused) return;                  /* (unused: a mixin's own method, never run as such) */
    fn->used=1;
    if(gg->nq==gg->cq){ gg->cq=gg->cq?gg->cq*2:32; gg->queue=(AFunc**)xrealloc(gg->queue,sizeof(AFunc*)*(size_t)gg->cq); }
    gg->queue[gg->nq++]=fn;
}
/* vtable slot i of class c is called through the table (a method some class overrides) or by
   the run time (a dunder method): its code is needed once the class is */
static int vt_dispatched(AClass *c, int i){
    const char *n=c->vt[i]->name; size_t l=strlen(n);
    if(l>4 && n[0]=='_' && n[1]=='_' && n[l-1]=='_' && n[l-2]=='_') return 1;
    for(AClass *b=c;b;b=b->base) if(i<b->nvt && b->vt[i]->overridden) return 1;
    return 0;
}
void use_class(AClass *c){
    for(;c;c=c->base){
        if(gg->class_used[c->id]) return;
        gg->class_used[c->id]=1;
        for(int i=0;i<c->nvt;i++) if(vt_dispatched(c,i)) use_fn(c->vt[i]);   /* (the others: when a call names them) */
        for(int i=0;i<c->nmethods;i++) if(c->methods[i]->is_static) use_fn(c->methods[i]);
        rt("rt_free"); rt("rt_decref"); rt("rt_static");
        str_lit(c->name);
    }
}

