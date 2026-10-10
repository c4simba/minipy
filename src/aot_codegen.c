/* ========================= Typed compiler: code generator =========================
   Turns a checked program (aot_types.c) into one fasm listing for i386 that
   fasm assembles straight into an executable - a Linux ELF using int 0x80 or a
   KolibriOS MENUET01 application using int 0x40.

   Values: int/bool/pointers in eax, floats on the x87 stack (st0). Locals live
   in the machine frame ([ebp-..]); module variables in static storage.
   Strings, lists, dicts, sets, buffers and objects are reference counted: an
   expression result is either borrowed (a variable, a field, an element) or
   owned (a fresh object); owned temporaries are parked in frame slots and
   released at the end of the statement that made them.

   Functions use cdecl with all arguments on the stack (floats take 8 bytes),
   keep ebx/esi/edi/ebp, and return in eax/st0; a returned reference is owned
   by the caller. A function owns its reference parameters for its duration.

   Only the runtime routines a program uses are emitted (aot_rtlib.asm). */

#include "aot_model.h"

/* ---------------------------------------------------------------- text buffers */

typedef struct { char *s; size_t len, cap; } Buf;
static void buf_put(Buf *b, const char *s, size_t n){
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
static void buf_printf(Buf *b, const char *fmt, ...){ va_list ap; va_start(ap,fmt); buf_vprintf(b,fmt,ap); va_end(ap); }
static void buf_cat(Buf *b, Buf *x){ if(x->len) buf_put(b,x->s,x->len); }
static void put_cb(void *ctx, const char *s, size_t n){ buf_put((Buf*)ctx,s,n); }

/* `db` operands for arbitrary bytes */
/* the length of UTF-8 text in code points, counted like the runtime (and
   the interpreter): a byte that does not start a valid sequence is one */
static int u8_cplen(const char *s, int n){
    const unsigned char *u=(const unsigned char*)s; int c=0;
    for(int i=0;i<n;c++){
        unsigned b=u[i]; int len= b<0xC0 ? 1 : b<0xE0 ? 2 : b<0xF0 ? 3 : b<0xF8 ? 4 : 1;
        if(len>1 && i+len<=n){ int k=1; while(k<len && (u[i+k]&0xC0)==0x80) k++; if(k<len) len=1; }
        else len=1;
        i+=len;
    }
    return c;
}
static void buf_bytes(Buf *b, const char *s, int n, int nul){
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

typedef struct { char *s; int len; } Lit;
typedef struct { Ty *ty; int repr; } Fmt;

typedef struct G {
    AProg *p;
    AotTarget target;
    unsigned stack;
    AotRt *rt;
    Buf text, data, bss;
    int labels;
    Lit *lits; int nlits, clits;            /* static str objects S<i> */
    Lit *blits; int nblits, cblits;         /* static bytes objects B<i> */
    char *btypes[32]; int nbtypes;          /* built-in types used as values: descriptors TYD_<name> */
    Lit *plits; int nplits, cplits;         /* print literals P<i>: text + newline */
    Lit *flts; int nflts, cflts;            /* float constants FC<i>, as decimal text fasm converts */
    Lit *zlits; int nzlits, czlits;         /* NUL-terminated strings Z<i> */
    Lit *rlits; int nrlits, crlits;         /* raw bytes RL<i> (format string pieces) */
    AFunc **queue; int nq, cq;              /* functions to emit */
    Fmt *fmts; int nfmts, cfmts;            /* generated formatters FMT<i> (repr 2+v: JSON, variant v) */
    struct JVar { const char *isep, *ksep; int ascii; } *jvars; int njvars, cjvars;   /* json.dumps options */
    struct { AFunc *fn; int virt; } *stubs; int nstubs, cstubs;   /* task entries TS<i> */
    Ty **tdescs; int ntdescs, ctdescs;      /* tuple descriptors TD<i> */
    int *class_used;
    char *fv_used, *cd_used;                /* per function: static closure FV<id>, closure destroy routine CD<id> */
    struct { Ty *to, *from; } *adapters; int nadapters, cadapters;   /* AD<k> */
    char *bm_used;                          /* per method: BM<id>, the code of its bound values */
    char **kdlabels;                        /* per class: KDC<id>, the descriptor of its objects (class_kd) */
} G;

typedef struct Loop { int lcont, lbreak; } Loop;
/* an enclosing try statement: its handler record, whether that record is
   pushed right now, its finally block, the loop depth it started at */
typedef struct TryCtx { int rec; int active; Stmt **fin; int nfin; int nloops; } TryCtx;

typedef struct F {
    AFunc *fn;
    Buf code;
    int frame;                  /* bytes of locals below the saved registers */
    int *tmp; int ntmp, ctmp;   /* temporary reference slots (offsets) */
    int tmp_used;
    int *refs; int nrefs, crefs;/* every reference slot: released when the function returns */
    Loop loops[64]; int nloops;
    int lret;
    char param_stored[16];      /* parameter assigned in the body: the function takes its own reference */
    TryCtx tries[32]; int ntries;
    int excs[32]; int nexcs;    /* slots of the exceptions being handled (bare raise) */
    int env;                    /* frame slot of its closure (edx at entry); 0 without captures */
    struct LiveTab *live;       /* liveness of its variables after each simple statement (moves) */
    unsigned *live_after;       /* the statement being generated: variables still needed after it */
    unsigned char *occ;         /* ... and how often each occurs in it (2: more, or evaluated repeatedly) */
    Ty *ret;                    /* what `return` gives (None in a generator's function) */
} F;

static G *gg;                   /* the generator (one compilation at a time) */

/* a format spec, and where a formatted value comes from (an expression or a tuple field) */
typedef struct { int fill, align, sign, alt, zero, width, comma, prec; char type; int wslot, pslot; } Spec;   /* wslot / pslot: the width / precision is a value in that frame slot */
typedef struct { Expr *e; Ty *tt; int tslot, idx; } ArgSrc;
static int parse_spec(const char *s, int n, Spec *sp);
static void emit_field(F *f, Spec *sp, Ty *t, ArgSrc *src, int repr, int pystyle, int line);
static int gen_str_format(F *f, Expr *call);

static int new_label(void){ return ++gg->labels; }
static void rt(const char *name){ aot_rt_use(gg->rt,name); }

static void E(F *f, const char *fmt, ...){ va_list ap; va_start(ap,fmt); buf_put(&f->code,"        ",8); buf_vprintf(&f->code,fmt,ap); buf_put(&f->code,"\n",1); va_end(ap); }
static void LBL(F *f, int l){ buf_printf(&f->code,"L%d:\n",l); }
static void CALLRT(F *f, const char *name){ rt(name); E(f,"call %s",name); }

static Ty *TY(Expr *e){ return ty_find(xinfo(e)->ty); }
static int is_ptr(Ty *t){ return ty_is_ptr(t); }
static int is_flt(Ty *t){ return ty_find(t)->k==TY_FLOAT; }
static int is_lng(Ty *t){ return ty_find(t)->k==TY_INT; }     /* an int: 64 bits in edx:eax */
/* The runtime's descriptor (kd) of values of type t: their size, whether they
   are references, how they compare, hash and print (aot_rtlib.asm). Classes
   get their own (class_kd), using __eq__, __lt__, __hash__ and their repr. */
static void rt(const char *name);
static const char *class_kd(AClass *c);
static const char *kd_of(Ty *t){
    t=ty_find(t);
    const char *n;
    switch(t->k){
        case TY_BOOL: n=t->opt?"rt_kd_obool":"rt_kd_bool"; break;    /* optional: None in the type's reserved value */
        case TY_INT: n=t->opt?"rt_kd_oint":"rt_kd_int"; break;
        case TY_FLOAT: n=t->opt?"rt_kd_ofloat":"rt_kd_float"; break;
        case TY_STR: n="rt_kd_str"; break;
        case TY_BYTES: n="rt_kd_bytes"; break;
        case TY_TYPE: n="rt_kd_type"; break;
        case TY_TUPLE: n="rt_kd_tuple"; break;
        case TY_LIST: n="rt_kd_list"; break;
        case TY_DICT: n="rt_kd_dict"; break;
        case TY_SET: n="rt_kd_set"; break;
        case TY_OBJ: return class_kd(t->cls);
        case TY_VAR: case TY_VOID: n="rt_kd_word"; break;       /* never filled */
        default: n="rt_kd_obj"; break;                          /* functions, files ...: identity */
    }
    rt(n); return n;
}
static int esize(Ty *t){ return ty_size(t); }
static void cg_fail(int line, const char *msg);
/* a new empty dict of type t -> eax (owned) */
static void new_dict(F *f, Ty *t){
    E(f,"mov eax,%s",kd_of(ty_dkey(t))); E(f,"mov edx,%s",kd_of(ty_find(t)->elem)); CALLRT(f,"rt_dict_new");
}
static void new_list(F *f, Ty *elem){ E(f,"mov eax,%s",kd_of(elem)); CALLRT(f,"rt_list_new"); }
static void new_set(F *f, Ty *elem){ E(f,"mov eax,%s",kd_of(elem)); CALLRT(f,"rt_set_new"); }

/* ---- literal pools ---- */
static int lit_index(Lit **v, int *n, int *cap, const char *s, int len){
    for(int i=0;i<*n;i++) if((*v)[i].len==len && memcmp((*v)[i].s,s,(size_t)len)==0) return i;
    if(*n==*cap){ *cap=*cap?*cap*2:32; *v=(Lit*)xrealloc(*v,sizeof(Lit)*(size_t)*cap); }
    (*v)[*n].s=xstrndup2(s,len); (*v)[*n].len=len; return (*n)++;
}
static int str_lit(const char *s){ rt("rt_static"); return lit_index(&gg->lits,&gg->nlits,&gg->clits,s,(int)strlen(s)); }
static int print_lit(const char *s, int n){ return lit_index(&gg->plits,&gg->nplits,&gg->cplits,s,n); }
static int str_litn(const char *s, int n){ rt("rt_static"); return lit_index(&gg->lits,&gg->nlits,&gg->clits,s,n); }
static int bytes_lit(const char *s, int n){ rt("rt_static"); return lit_index(&gg->blits,&gg->nblits,&gg->cblits,s,n); }
static int zlit(const char *s){ return lit_index(&gg->zlits,&gg->nzlits,&gg->czlits,s,(int)strlen(s)); }
static int rlit(const char *s, int n){ return lit_index(&gg->rlits,&gg->nrlits,&gg->crlits,s,n); }
/* A float literal's source text in fasm's syntax ("1e3" -> "1.0e3"); fasm does
   the decimal -> binary conversion, correctly rounded. */
static int float_const(const char *src){
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

static void use_fn(AFunc *fn){
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
static void use_class(AClass *c){
    for(;c;c=c->base){
        if(gg->class_used[c->id]) return;
        gg->class_used[c->id]=1;
        for(int i=0;i<c->nvt;i++) if(vt_dispatched(c,i)) use_fn(c->vt[i]);   /* (the others: when a call names them) */
        for(int i=0;i<c->nmethods;i++) if(c->methods[i]->is_static) use_fn(c->methods[i]);
        rt("rt_free"); rt("rt_decref"); rt("rt_static");
        str_lit(c->name);
    }
}

/* ---- readable labels: module-qualified names (main.read_item,
   fastapi.FastAPI.get, main.outer.inner) made unique with _2, _3 ... ---- */
typedef struct { char **v; int n, cap; } LabelSet;
static LabelSet g_labels;
static int label_reserved(const char *s){
    static const char *fixed[]={"start","i_end","mem_end","stack_top","ADFREE",NULL};
    for(int i=0;fixed[i];i++) if(!strcmp(s,fixed[i])) return 1;
    if(!strncmp(s,"rt_",3)||!strncmp(s,"RT_",3)||!strncmp(s,"DYN_",4)||!strncmp(s,"VTX_",4)||!strncmp(s,"DTX_",4)||!strncmp(s,"CI_",3)||!strncmp(s,"TYD_",4)||!strcmp(s,"TYQ")||!strcmp(s,"TYQN")) return 1;
    static const char *families[]={"L","S","B","Z","P","FC","RL","TD","FMT","AD","CI",NULL};   /* numbered internal labels */
    for(int i=0;families[i];i++){ size_t n=strlen(families[i]);
        if(!strncmp(s,families[i],n) && s[n]){ const char *q=s+n; while(*q>='0'&&*q<='9') q++; if(!*q) return 1; } }
    for(int i=0;i<g_labels.n;i++) if(!strcmp(g_labels.v[i],s)) return 1;
    return 0;
}
static const char *unique_label(const char *raw){
    char base[256]; int k=0;
    for(const char *p=raw;*p && k<200;p++){
        char c=*p;
        if(c=='>') continue;                                          /* f<endpoint> -> f.endpoint */
        if(c=='<') c='.';
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='.')) c='_';
        if(c=='.' && (k==0 || base[k-1]=='.')) continue;          /* no leading or double dots */
        if(k==0 && c>='0' && c<='9') base[k++]='_';
        base[k++]=c;
    }
    while(k>0 && base[k-1]=='.') k--;
    if(!k) base[k++]='_';
    base[k]=0;
    char name[280]; snprintf(name,sizeof name,"%s",base);
    for(int n=2;label_reserved(name);n++) snprintf(name,sizeof name,"%s_%d",base,n);
    if(g_labels.n==g_labels.cap){ g_labels.cap=g_labels.cap?g_labels.cap*2:256; g_labels.v=(char**)xrealloc(g_labels.v,sizeof(char*)*(size_t)g_labels.cap); }
    return g_labels.v[g_labels.n++]=xstrdup2(name);
}
/* the label prefix of a module: its dotted name, the script's file name for __main__ */
static const char *mod_prefix(AModule *m){
    if(strcmp(m->name,"__main__")) return m->name;
    static char main_name[128];
    const char *path=m->unit && m->unit->path ? m->unit->path : "main";
    const char *b=strrchr(path,'/'); b=b?b+1:path;
    snprintf(main_name,sizeof main_name,"%s",b);
    char *dot=strrchr(main_name,'.'); if(dot && dot!=main_name) *dot=0;
    return main_name;
}
enum { LB_CODE, LB_VALUE, LB_FREE, LB_BOUND, LB_TASK, LB_VTASK, LB_BODY, LB_GEN, LB_KINDS };
static const char **g_fnlabels[LB_KINDS];      /* per function id */
static const char **g_vtlabels, **g_dtlabels, **g_glabels, **g_mlabels;
static const char *fnl(AFunc *fn, int kind);
static void fn_qualname(AFunc *fn, char *out, size_t n){
    if(fn->outer){ snprintf(out,n,"%s.%s",fnl(fn->outer,LB_CODE),fn->lam?"lambda":fn->genexp?"genexpr":fn->name); return; }   /* under the outer function's label */
    if(!fn->def){ snprintf(out,n,"%s.module_body",mod_prefix(fn->mod)); return; }
    if(fn->cls){ snprintf(out,n,"%s.%s.%s",mod_prefix(fn->cls->mod),fn->cls->name,fn->name); return; }   /* (a mixin's copy: in its own module) */
    ASym *s=symtab_find(&fn->mod->syms,fn->name);
    snprintf(out,n,"%s.%s%s",mod_prefix(fn->mod),fn->name,s && s->kind==AS_VAR?".def":"");   /* decorated: the name is the decorated value */
}
/* a function's labels: its code, and what is made of it (static value, closure
   destroy, bound-method code, task entries, generator body and descriptor) */
static const char *fnl(AFunc *fn, int kind){
    if(!g_fnlabels[kind]){ g_fnlabels[kind]=(const char**)xmalloc(sizeof(char*)*(size_t)(gg->p->nfuncs+1)); memset(g_fnlabels[kind],0,sizeof(char*)*(size_t)(gg->p->nfuncs+1)); }
    if(fn->id>gg->p->nfuncs) cg_fail(0,"internal error: function id");
    if(!g_fnlabels[kind][fn->id]){
        static const char *suffix[]={"","value","free","bound","task","vtask","body","gen"};
        char q[256], full[300]; fn_qualname(fn,q,sizeof q);
        if(kind==LB_CODE) snprintf(full,sizeof full,"%s",q); else snprintf(full,sizeof full,"%s.%s",fnl(fn,LB_CODE),suffix[kind]);
        g_fnlabels[kind][fn->id]=unique_label(full);
    }
    return g_fnlabels[kind][fn->id];
}
static const char *class_label(AClass *c, int destroy){
    const char ***tab=destroy?&g_dtlabels:&g_vtlabels;
    if(!*tab){ *tab=(const char**)xmalloc(sizeof(char*)*(size_t)(gg->p->nclasses+1)); memset(*tab,0,sizeof(char*)*(size_t)(gg->p->nclasses+1)); }
    if(!(*tab)[c->id]){ char full[300];
        if(c->builtin) snprintf(full,sizeof full,"%s.%s",c->name,destroy?"destroy":"vtable");
        else snprintf(full,sizeof full,"%s.%s.%s",mod_prefix(c->mod),c->name,destroy?"destroy":"vtable");
        (*tab)[c->id]=unique_label(full); }
    return (*tab)[c->id];
}
static const char *global_label(AVar *v){
    if(!g_glabels){ g_glabels=(const char**)xmalloc(sizeof(char*)*(size_t)(gg->p->nglobals+1)); memset(g_glabels,0,sizeof(char*)*(size_t)(gg->p->nglobals+1)); }
    if(!g_glabels[v->id]){ char full[300]; snprintf(full,sizeof full,"%s.%s",mod_prefix(v->mod),v->name); g_glabels[v->id]=unique_label(full); }
    return g_glabels[v->id];
}
static const char *module_label(AModule *m){
    if(!g_mlabels){ g_mlabels=(const char**)xmalloc(sizeof(char*)*(size_t)(gg->p->nmods+1)); memset(g_mlabels,0,sizeof(char*)*(size_t)(gg->p->nmods+1)); }
    if(!g_mlabels[m->index]){ char full[300]; snprintf(full,sizeof full,"%s.imported",mod_prefix(m)); g_mlabels[m->index]=unique_label(full); }
    return g_mlabels[m->index];
}

/* ---- frame ---- */
static int frame_slot(F *f, int size){ f->frame+=size; return -(12+f->frame); }
static void add_ref_slot(F *f, int off){
    if(f->nrefs==f->crefs){ f->crefs=f->crefs?f->crefs*2:16; f->refs=(int*)xrealloc(f->refs,sizeof(int)*(size_t)f->crefs); }
    f->refs[f->nrefs++]=off;
}
/* Park the owned reference in eax until the end of the current statement. */
static void hold(F *f){
    if(f->tmp_used==f->ntmp){
        if(f->ntmp==f->ctmp){ f->ctmp=f->ctmp?f->ctmp*2:8; f->tmp=(int*)xrealloc(f->tmp,sizeof(int)*(size_t)f->ctmp); }
        int off=frame_slot(f,4); f->tmp[f->ntmp++]=off; add_ref_slot(f,off);
    }
    E(f,"mov [ebp%+d],eax",f->tmp[f->tmp_used++]);
}
static int scope_open(F *f){ return f->tmp_used; }
static void scope_close(F *f, int mark){
    while(f->tmp_used>mark){
        int off=f->tmp[--f->tmp_used];
        rt("rt_decref");
        E(f,"mov eax,[ebp%+d]",off); E(f,"mov dword [ebp%+d],0",off); E(f,"call rt_decref");
    }
}
/* A reference slot of its own (loop iterables, comprehension results). */
static int ref_slot(F *f){ int off=frame_slot(f,4); add_ref_slot(f,off); return off; }

/* ---- variables and memory ---- */
static int is_fn_body(AFunc *f){ return f->def||f->lam||f->genexp; }   /* not a module body */
static AVar *root_var(AVar *v){ while(v->src) v=v->src; return v; }
/* A function value: +0 refcount, +4 destroy, +8 code, +12 its name (str),
   captured values from +16: a copy, or the pointer of a shared cell. */
/* the defaults of fn's function objects: at 16 on, in parameter order (positional ones first: a value call fills them from there) */
static void layout_defaults(AFunc *fn){
    if(fn->deflaid) return;
    fn->deflaid=1; int off=0;
    for(int i=0;i<fn->nparams && i<16;i++){ if(!fn->defaults[i] || i==fn->star || i==fn->dstar) continue; fn->defoff[i]=off; off+=ty_size(fn->params[i]->ty); }
    fn->defsize=off;
}
/* a default evaluated where its function is defined (not a constant put in at the call) */
static int dyn_defaults(AFunc *fn){
    for(int i=0;i<fn->nparams;i++) if(fn->defaults[i] && i!=fn->star && i!=fn->dstar && !const_default(fn->defaults[i])) return 1;
    return 0;
}
static int has_defaults(AFunc *fn){ for(int i=0;i<fn->nparams;i++) if(fn->defaults[i] && i!=fn->star && i!=fn->dstar) return 1; return 0; }
static void layout_caps(AFunc *fn){
    layout_defaults(fn);
    if(fn->capsize || !fn->ncaps) return;
    int off=16+fn->defsize;
    for(int i=0;i<fn->ncaps;i++){ AVar *v=fn->caps[i]; v->capoff=off; off+=root_var(v)->cell?4:ty_size(v->ty); }
    fn->capsize=off-16;
}
static const char *fn_display_name(AFunc *g){ return g->shown?g->shown:g->lam?"<lambda>":g->genexp?"<genexpr>":g->name; }
static void var_addr_at(char *buf, size_t n, AVar *v, int extra){
    if(v->global){ if(extra) snprintf(buf,n,"[%s+%d]",global_label(v),extra); else snprintf(buf,n,"[%s]",global_label(v)); }
    else snprintf(buf,n,"[ebp%+d]",v->offset+extra);
}
static void var_addr(char *buf, size_t n, AVar *v){ var_addr_at(buf,n,v,0); }
static void load_mem(F *f, Ty *t, const char *reg, int off);
static void store_mem(F *f, Ty *t, const char *reg, int off);
/* v of `del v` somewhere: UnboundLocalError / NameError unless it holds a value */
static void check_bound(F *f, AVar *v){
    char a[320]; var_addr(a,sizeof a,v->bound); int l=new_label(); char msg[400];
    E(f,"cmp dword %s,0",a); E(f,"jne L%d",l);
    if(v->global){ snprintf(msg,sizeof msg,"name '%s' is not defined",v->name); E(f,"mov esi,Z%d",zlit(msg)); CALLRT(f,"rt_nameerr_text"); }
    else { snprintf(msg,sizeof msg,"cannot access local variable '%s' where it is not associated with a value",v->name); E(f,"mov esi,Z%d",zlit(msg)); CALLRT(f,"rt_unbound_text"); }
    LBL(f,l);
}
static void load_var(F *f, AVar *v){
    if(v->bound) check_bound(f,v);
    if(v->src){                                         /* captured: in the closure (or its cell) */
        layout_caps(v->owner);
        E(f,"mov ecx,[ebp%+d]",f->env);
        if(root_var(v)->cell){ E(f,"mov ecx,[ecx+%d]",v->capoff); load_mem(f,v->ty,"ecx",8); }
        else load_mem(f,v->ty,"ecx",v->capoff);
        return;
    }
    if(v->cell){ E(f,"mov ecx,[ebp%+d]",v->offset); load_mem(f,v->ty,"ecx",8); return; }
    char a[320]; var_addr(a,sizeof a,v);
    if(is_flt(v->ty)) E(f,"fld qword %s",a);
    else if(is_lng(v->ty)){ char h[320]; var_addr_at(h,sizeof h,v,4); E(f,"mov eax,%s",a); E(f,"mov edx,%s",h); }
    else E(f,"mov eax,%s",a);
}
/* Store eax/st0 (an owned reference for reference types) into the variable. */
static void store_var(F *f, AVar *v){
    if(v->src){ layout_caps(v->owner); E(f,"mov ecx,[ebp%+d]",f->env); E(f,"mov ecx,[ecx+%d]",v->capoff); store_mem(f,v->ty,"ecx",8); return; }   /* nonlocal: a cell */
    if(v->cell){ E(f,"mov ecx,[ebp%+d]",v->offset); store_mem(f,v->ty,"ecx",8); return; }
    char a[320]; var_addr(a,sizeof a,v);
    if(!v->global && v->id>=0 && v->id<f->fn->nparams && f->fn->params[v->id]==v) f->param_stored[v->id]=1;
    if(is_flt(v->ty)) E(f,"fstp qword %s",a);
    else if(is_ptr(v->ty)){ rt("rt_decref"); E(f,"xchg eax,%s",a); E(f,"call rt_decref"); }
    else if(is_lng(v->ty)){ char h[320]; var_addr_at(h,sizeof h,v,4); E(f,"mov %s,eax",a); E(f,"mov %s,edx",h); }
    else E(f,"mov %s,eax",a);
    if(v->bound){ char b[320]; var_addr(b,sizeof b,v->bound); E(f,"mov dword %s,1",b); }   /* it holds a value again */
}
/* Store eax/st0 (owned) at [reg+off], releasing the previous reference. */
static void store_mem(F *f, Ty *t, const char *reg, int off){
    if(is_flt(t)) E(f,"fstp qword [%s%+d]",reg,off);
    else if(is_ptr(t)){ rt("rt_decref"); E(f,"xchg eax,[%s%+d]",reg,off); E(f,"call rt_decref"); }
    else if(is_lng(t)){ E(f,"mov [%s%+d],eax",reg,off); E(f,"mov [%s%+d],edx",reg,off+4); }
    else E(f,"mov [%s%+d],eax",reg,off);
}
/* Store into a fresh (zeroed or garbage) slot: nothing to release. */
static void store_new(F *f, Ty *t, const char *reg, int off){
    if(is_flt(t)) E(f,"fstp qword [%s%+d]",reg,off);
    else if(is_lng(t)){ E(f,"mov [%s%+d],eax",reg,off); E(f,"mov [%s%+d],edx",reg,off+4); }
    else E(f,"mov [%s%+d],eax",reg,off);
}
static void load_mem(F *f, Ty *t, const char *reg, int off){
    if(is_flt(t)) E(f,"fld qword [%s%+d]",reg,off);
    else if(is_lng(t)){
        if(!strcmp(reg,"edx")){ E(f,"mov eax,[edx%+d]",off); E(f,"mov edx,[edx%+d]",off+4); }
        else { E(f,"mov edx,[%s%+d]",reg,off+4); E(f,"mov eax,[%s%+d]",reg,off); } }
    else E(f,"mov eax,[%s%+d]",reg,off);
}
/* eax/st0 = the element at [base+idx*scale] (scale: the slot size) */
static void load_at(F *f, Ty *t, const char *base, const char *idx, int scale){
    if(is_flt(t)) E(f,"fld qword [%s+%s*%d]",base,idx,scale);
    else if(is_lng(t)){ E(f,"mov edx,[%s+%s*%d+4]",base,idx,scale); E(f,"mov eax,[%s+%s*%d]",base,idx,scale); }
    else E(f,"mov eax,[%s+%s*%d]",base,idx,scale);
}
/* ---- None: a null reference; an int -2**63, a float a NaN of its own, a bool 2 ---- */
static int is_opt(Ty *t){ return ty_opt(t); }
#define NONE_HI_INT "0x80000000"
static void gen_none(F *f, Ty *t){
    t=ty_find(t);
    if(is_flt(t)){ rt("rt_fnone"); E(f,"fld qword [rt_fnone]"); }
    else if(is_lng(t)){ E(f,"xor eax,eax"); E(f,"mov edx," NONE_HI_INT); }
    else if(t->k==TY_BOOL) E(f,"mov eax,2");
    else if(t->k!=TY_VOID) E(f,"xor eax,eax");
}
/* jump to `label` when the value in eax / edx:eax / st0 (of type t) is None; the value stays */
static int new_label(void);
static void LBL(F *f, int l);
static void jump_if_none(F *f, Ty *t, int label){
    t=ty_find(t);
    if(is_flt(t)){ int l=new_label(); rt("rt_fnone");
        E(f,"sub esp,8"); E(f,"fst qword [esp]"); E(f,"mov ecx,[esp+4]"); E(f,"cmp ecx,[rt_fnone+4]"); E(f,"jne L%d",l);
        E(f,"mov ecx,[esp]"); E(f,"cmp ecx,[rt_fnone]"); LBL(f,l); E(f,"lea esp,[esp+8]"); E(f,"je L%d",label); }
    else if(is_lng(t)){ int l=new_label(); E(f,"cmp edx," NONE_HI_INT); E(f,"jne L%d",l); E(f,"test eax,eax"); E(f,"jz L%d",label); LBL(f,l); }
    else if(t->k==TY_BOOL){ E(f,"cmp eax,2"); E(f,"je L%d",label); }
    else { E(f,"test eax,eax"); E(f,"jz L%d",label); }
}
/* exc(msg) when the value of type t is None (only for optional types) */
static void CALLRT(F *f, const char *name);
static int zlit(const char *s);
static void check_none(F *f, Ty *t, const char *exc, const char *msg){
    if(!is_opt(t)) return;
    if(!msg) msg="unsupported operand type: 'NoneType'";
    int lnone=new_label(), lok=new_label();
    jump_if_none(f,t,lnone); E(f,"jmp L%d",lok);
    LBL(f,lnone);
    E(f,"mov esi,Z%d",zlit(msg));
    CALLRT(f,!strcmp(exc,"AttributeError")?"rt_attrerr_text":"rt_typeerr_text");
    LBL(f,lok);
}
static const char *py_type_name(Ty *t);
static void use_class(AClass *c);
/* the descriptor of a built-in type (name, base): TYD_<name>, emitted at the end */
static const char *btype_label(const char *name){
    static char buf[32][48];
    int i=0;
    for(;i<gg->nbtypes;i++) if(!strcmp(gg->btypes[i],name)) break;
    if(i==gg->nbtypes){ if(i==32) cg_fail(0,"too many built-in types as values"); gg->btypes[gg->nbtypes++]=xstrdup2(name); }
    if(!strcmp(name,"bool")) btype_label("int");
    snprintf(buf[i],sizeof buf[i],"TYD_%s",name); return buf[i];
}
/* a class as a value (X_TYPEVAL): its descriptor */
static const char *typeval_label(XInfo *xi){
    if(xi->cls){ use_class(xi->cls); return class_label(xi->cls,0); }
    return btype_label(xi->name);
}
static void gen_type_of(F *f, Expr *x);
/* whether values of static type t are instances of the built-in type name (None aside) */
static int btype_match(Ty *t, const char *name){
    t=ty_find(t);
    if(!strcmp(name,"object")) return 1;
    switch(t->k){
        case TY_INT: return !strcmp(name,"int");
        case TY_BOOL: return !strcmp(name,"bool")||!strcmp(name,"int");
        case TY_FLOAT: return !strcmp(name,"float");
        case TY_STR: return !strcmp(name,"str");
        case TY_BYTES: return !strcmp(name,"bytes");
        case TY_LIST: return !strcmp(name,t->tup?"tuple":"list");
        case TY_TUPLE: return !strcmp(name,t->names?"dict":"tuple");
        case TY_DICT: return !strcmp(name,"dict");
        case TY_SET: return !strcmp(name,t->tup?"frozenset":"set");
        case TY_TYPE: return !strcmp(name,"type");
        default: return 0;
    }
}
/* the Python name of t's values, for messages */
static const char *py_type_name(Ty *t){
    t=ty_find(t);
    switch(t->k){ case TY_INT: return "int"; case TY_BOOL: return "bool"; case TY_FLOAT: return "float"; case TY_STR: return "str"; case TY_BYTES: return "bytes"; case TY_TYPE: return "type";
        case TY_LIST: return t->tup?"tuple":"list"; case TY_DICT: return "dict"; case TY_SET: return t->tup?"frozenset":"set"; case TY_TUPLE: return t->names?"dict":"tuple";
        case TY_OBJ: return t->cls->name; case TY_FUNC: return "function"; case TY_GEN: return "generator"; case TY_FILE: return "TextIOWrapper";
        case TY_BUF: return "buffer"; case TY_TASK: return "Task"; default: return "NoneType"; }
}
static char none_msg_buf[8][256]; static int none_msg_k;
static const char *none_msg(const char *fmt, ...){
    char *b=none_msg_buf[none_msg_k++&7]; va_list ap; va_start(ap,fmt); vsnprintf(b,256,fmt,ap); va_end(ap); return b;
}
/* what a check of a value going where None is not allowed says (set around the operations below) */
static const char *g_none_exc="TypeError", *g_none_text=NULL;
static void push_value(F *f, Ty *t){ if(is_flt(t)){ E(f,"sub esp,8"); E(f,"fstp qword [esp]"); } else { if(is_lng(t)) E(f,"push edx"); E(f,"push eax"); } }
static void pop_value(F *f, Ty *t){ if(is_flt(t)){ E(f,"fld qword [esp]"); E(f,"add esp,8"); } else { E(f,"pop eax"); if(is_lng(t)) E(f,"pop edx"); } }
/* an int in edx:eax as 32 bits (an index, a count): the nearest such */
static void to_i32(F *f, Ty *t){ if(is_lng(t)) CALLRT(f,"rt_i32sat"); }
/* a 32-bit result (eax) as an int: sign- or zero-extended */
static void widen(F *f, int is_signed){ if(is_signed) E(f,"cdq"); else E(f,"xor edx,edx"); }
static void drop_value(F *f, Ty *t, int owned){
    t=ty_find(t);
    if(t->k==TY_VOID||t->k==TY_VAR) return;
    if(is_flt(t)) E(f,"fstp st0");
    else if(is_ptr(t) && owned){ rt("rt_decref"); E(f,"call rt_decref"); }
}
static void list_append_top(F *f, Ty *el);
static void set_add_top(F *f, Ty *el);
static void gen_int_const(F *f, Ty *t, int64_t n);
static void put_arg(F *f, Ty *pt, int off);
static void gen_list_of(F *f, Expr *x, Ty *elem);
static void gen_codec_call(F *f, Expr *obj, Expr *enc, Expr *errs, const char *m, const char *routine);
static void gen_byteorder(F *f, Expr *x);
/* AttributeError for x.name when x (in eax) is None */
static void attr_check(F *f, const char *name){
    int l=new_label(); E(f,"test eax,eax"); E(f,"jnz L%d",l);
    E(f,"mov esi,Z%d",zlit(none_msg("'NoneType' object has no attribute '%s'",name))); CALLRT(f,"rt_attrerr_text"); LBL(f,l);
}
static void panic_if_null(F *f){ int l=new_label(); rt("rt_panic_none"); E(f,"test eax,eax"); E(f,"jnz L%d",l); E(f,"call rt_panic_none"); LBL(f,l); }
static void incref(F *f){ CALLRT(f,"rt_incref"); }

/* ---------------------------------------------------------------- expressions */

static int  gen(F *f, Expr *e);
static void gen_bool(F *f, Expr *e);
static void gen_jump(F *f, Expr *e, int label, int want);
static void gen_yield_value(F *f, Ty *yt);
static Ty *gpart(Ty *g, int i){ Ty *t=aot_gen_part(g,i); return t->k==TY_VAR ? TY_VOID_T : t; }   /* a generator's send / return type (void: None) */
static void drop_sent(F *f);
static int take_sent(F *f, Ty *st);
static int gen_yield_from(F *f, Expr *src, int want);
static void call_method_on_top(F *f, AFunc *m);
static void gen_fmt(F *f, Ty *t, int repr);
static void gen_stmts(F *f, Stmt **b, int n);

static const char *cg_path="?";        /* source of the function being generated (messages) */
MPY_NORETURN static void cg_fail(int line, const char *msg){
    fprintf(stderr,"%s:%d: error: %s\n",cg_path,line,msg); exit(1);
}
/* The variable a name stores to inside the current function. */
static AVar *find_var(F *f, const char *name, int line){
    AFunc *fn=f->fn; ASym *s=NULL;
    if(is_fn_body(fn)){
        int global=0;
        for(int i=0;i<fn->nglobals_decl;i++) if(!strcmp(fn->globals_decl[i],name)) global=1;
        if(!global) s=symtab_find(&fn->locals,name);
        if(!global && !s) for(int i=0;i<fn->ncaps;i++) if(!strcmp(fn->caps[i]->name,name)) return fn->caps[i];   /* nonlocal */
    }
    if(!s) s=symtab_find(&fn->mod->syms,name);
    if(!s || s->kind!=AS_VAR) cg_fail(line,"internal error: unknown variable");
    return (AVar*)s->p;
}

/* reference results: make owned / borrowed (owned temporaries are held) */
static int try_move(F *f, Expr *e);
static void gen_owned(F *f, Expr *e){ if(try_move(f,e)) return; int o=gen(f,e); if(is_ptr(TY(e)) && !o) incref(f); }
static void gen_borrow(F *f, Expr *e){ int o=gen(f,e); if(is_ptr(TY(e)) && o) hold(f); }
/* convert eax (int/bool) to st0 when a float is wanted */
static void conv_num(F *f, Ty *from, Ty *to){
    from=ty_find(from); to=ty_find(to);
    if(to->k==TY_FLOAT && from->k==TY_INT){ E(f,"push edx"); E(f,"push eax"); E(f,"fild qword [esp]"); E(f,"add esp,8"); }
    else if(to->k==TY_FLOAT && from->k==TY_BOOL){ E(f,"push eax"); E(f,"fild dword [esp]"); E(f,"add esp,4"); }
    else if(to->k==TY_INT && from->k==TY_BOOL) E(f,"xor edx,edx");
}
/* A function taking *args where a fixed signature is wanted: an adapter
   closure AD<k> (+16 the function) packs the extra arguments into the tuple. */
static int adapt_needed(Ty *from, Ty *to){ from=ty_find(from); to=ty_find(to); return from->k==TY_FUNC && to->k==TY_FUNC && !to->tup && (from->tup&1); }
static int adapter_index(Ty *to, Ty *from);
static void conv_func(F *f, Ty *from, Ty *to, int owned){
    if(!adapt_needed(from,to)) return;
    int k=adapter_index(ty_find(to),ty_find(from)), l=new_label();
    E(f,"test eax,eax"); E(f,"jz L%d",l);
    E(f,"push eax"); E(f,"mov eax,20"); CALLRT(f,"rt_alloc");
    E(f,"mov dword [eax],1"); E(f,"mov dword [eax+4],ADFREE"); E(f,"mov dword [eax+8],AD%d",k);
    E(f,"pop ecx"); E(f,"mov [eax+16],ecx"); E(f,"mov edx,[ecx+12]"); E(f,"mov [eax+12],edx");
    if(!owned){ E(f,"push eax"); E(f,"mov eax,ecx"); CALLRT(f,"rt_incref"); E(f,"pop eax"); }   /* an owned one moves into the adapter */
    LBL(f,l);
    if(!owned) hold(f);
}
/* the value in eax/st0 (owned) as a `to` */
/* an int / float (eax:edx / st0) where a complex is wanted: complex(x, 0.0), owned */
static void conv_complex(F *f, Ty *from, Ty *to){
    from=ty_find(from); to=ty_find(to);
    if(!aot_is_complex(to) || (from->k!=TY_INT && from->k!=TY_BOOL && from->k!=TY_FLOAT)) return;
    conv_num(f,from,TY_FLOAT_T);
    AClass *k=ty_find(to)->cls; use_class(k);
    AField *re=aot_find_field(k,"real"), *im=aot_find_field(k,"imag");
    E(f,"sub esp,8"); E(f,"fstp qword [esp]");
    E(f,"mov eax,%d",k->size); E(f,"mov edx,%s",class_label(k,0)); E(f,"mov ecx,%s",class_label(k,1)); CALLRT(f,"rt_obj_new");
    E(f,"fld qword [esp]"); E(f,"fstp qword [eax+%d]",re->offset); E(f,"add esp,8");
    E(f,"mov dword [eax+%d],0",im->offset); E(f,"mov dword [eax+%d],0",im->offset+4);
}
static void conv(F *f, Ty *from, Ty *to){ if(aot_is_complex(to) && !aot_is_complex(from)){ conv_complex(f,from,to); return; } conv_num(f,from,to); conv_func(f,from,to,1); }
static int gen_zero(F *f, Ty *t);
static int is_endpoint_ty(Ty *t){ t=ty_find(t); return t->k==TY_FUNC && (t->tup&4); }
static void gen_as(F *f, Expr *e, Ty *want, int owned){
    if(e->kind==EXPR_NONE){ gen_none(f,want); return; }
    if(is_endpoint_ty(want) && !is_endpoint_ty(TY(e))){                /* a function as a minipy.Endpoint: its adapter */
        XInfo *x=xinfo(e);
        if(x->kind!=X_FUNCREF || !x->fn || !x->fn->ep_adapter) cg_fail(e->line,"a minipy.Endpoint is made from a function given by its name");
        AFunc *ad=x->fn->ep_adapter; use_fn(ad); gg->fv_used[ad->id]=1;
        E(f,"mov eax,%s",fnl(ad,LB_VALUE));
        return;
    }
    const char *given=g_none_text;                                           /* (e's own code may set it) */
    char *keep= given && is_opt(TY(e)) ? xstrdup2(given) : NULL;
    if(owned) gen_owned(f,e); else gen_borrow(f,e);
    Ty *w=ty_find(want);
    if(is_opt(TY(e)) && !w->opt && w->k!=TY_VAR && w->k!=TY_VOID){          /* None where a value is needed */
        const char *msg=keep;
        if(!msg) msg = w->k==TY_INT||w->k==TY_BOOL ? "'NoneType' object cannot be interpreted as an integer"
                     : w->k==TY_FLOAT ? "must be real number, not NoneType"
                     : none_msg("expected %s, got NoneType",py_type_name(w));
        check_none(f,TY(e),g_none_exc,msg);
    }
    if(aot_is_complex(want) && !aot_is_complex(TY(e)) && ty_find(TY(e))->k!=TY_OBJ){   /* 1 as a complex: a new object */
        conv_complex(f,TY(e),want); if(!owned) hold(f); return; }
    conv_num(f,TY(e),want); conv_func(f,TY(e),want,owned);
}

/* ---- binary operators: left operand on the machine stack, right in eax/st0 ---- */
static int numeric_ty(Ty *t){ t=ty_find(t); return t->k==TY_INT||t->k==TY_BOOL||t->k==TY_FLOAT; }
static int apply_binop(F *f, TokKind op, Ty *t, Ty *ta, Ty *tb, int line){
    t=ty_find(t); ta=ty_find(ta); tb=ty_find(tb);
    switch(op){ case T_PLUS_ASSIGN: op=T_PLUS; break; case T_MINUS_ASSIGN: op=T_MINUS; break; case T_STAR_ASSIGN: op=T_STAR; break;
        case T_SLASH_ASSIGN: op=T_SLASH; break; case T_FLOOR_DIV_ASSIGN: op=T_FLOOR_DIV; break; case T_PERCENT_ASSIGN: op=T_PERCENT; break;
        case T_POWER_ASSIGN: op=T_POWER; break; case T_AMP_ASSIGN: op=T_AMP; break; case T_PIPE_ASSIGN: op=T_PIPE; break;
        case T_CARET_ASSIGN: op=T_CARET; break; case T_SHL_ASSIGN: op=T_SHL; break; case T_SHR_ASSIGN: op=T_SHR; break; default: break; }
    if(t->k==TY_FLOAT){
        E(f,"fld qword [esp]"); E(f,"add esp,8");               /* st0 = a, st1 = b */
        switch(op){
            case T_PLUS: E(f,"faddp st1,st0"); break;
            case T_MINUS: E(f,"fsubrp st1,st0"); break;
            case T_STAR: E(f,"fmulp st1,st0"); break;
            case T_SLASH: CALLRT(f,"rt_fdiv"); break;
            case T_FLOOR_DIV: CALLRT(f,"rt_ffloordiv"); break;
            case T_PERCENT: CALLRT(f,"rt_fmod"); break;
            case T_POWER: CALLRT(f,"rt_fpow"); break;
            default: cg_fail(line,"unsupported float operator");
        }
        return 0;
    }
    if(t->k==TY_INT){                                         /* 64 bits: the left operand on the stack, the right in edx:eax */
        rt("rt_int_overflow");
        switch(op){
            case T_PLUS: E(f,"add eax,[esp]"); E(f,"adc edx,[esp+4]"); E(f,"jo rt_int_overflow"); break;
            case T_MINUS: E(f,"mov ecx,eax"); E(f,"mov eax,[esp]"); E(f,"sub eax,ecx"); E(f,"mov ecx,edx"); E(f,"mov edx,[esp+4]"); E(f,"sbb edx,ecx"); E(f,"jo rt_int_overflow"); break;
            case T_STAR: CALLRT(f,"rt_imul"); break;
            case T_AMP: E(f,"and eax,[esp]"); E(f,"and edx,[esp+4]"); break;
            case T_PIPE: E(f,"or eax,[esp]"); E(f,"or edx,[esp+4]"); break;
            case T_CARET: E(f,"xor eax,[esp]"); E(f,"xor edx,[esp+4]"); break;
            case T_SHL: CALLRT(f,"rt_ishl"); break;
            case T_SHR: CALLRT(f,"rt_isar"); break;
            case T_FLOOR_DIV: CALLRT(f,"rt_idivmod"); break;
            case T_PERCENT: CALLRT(f,"rt_idivmod"); E(f,"mov eax,[rt_scratch]"); E(f,"mov edx,[rt_scratch+4]"); break;
            case T_POWER: CALLRT(f,"rt_ipow"); break;
            default: cg_fail(line,"unsupported int operator");
        }
        E(f,"add esp,8");
        return 0;
    }
    if(t->k==TY_BOOL){                                        /* bool & | ^ bool */
        E(f,"mov ecx,eax"); E(f,"pop eax");
        switch(op){
            case T_AMP: E(f,"and eax,ecx"); break;
            case T_PIPE: E(f,"or eax,ecx"); break;
            case T_CARET: E(f,"xor eax,ecx"); break;
            default: cg_fail(line,"unsupported bool operator");
        }
        return 0;
    }
    if(t->k==TY_STR||t->k==TY_BYTES||t->k==TY_LIST){
        if(op==T_STAR){                                       /* x * n, n * x: n as a 32-bit count */
            if(ta->k==t->k){ to_i32(f,tb); E(f,"mov edx,eax"); E(f,"pop eax"); }
            else { E(f,"mov ecx,eax"); E(f,"pop eax"); if(is_lng(ta)){ E(f,"pop edx"); to_i32(f,ta); } E(f,"mov edx,eax"); E(f,"mov eax,ecx"); }
            if(t->k==TY_STR||t->k==TY_BYTES){ CALLRT(f,"rt_str_mul"); return 1; }
            E(f,"mov ecx,%s",kd_of(t->elem)); CALLRT(f,"rt_list_mul"); return 1;
        }
        E(f,"mov edx,eax"); E(f,"pop eax");
        if(t->k==TY_STR||t->k==TY_BYTES){ if(op==T_PLUS){ CALLRT(f,"rt_str_concat"); return 1; } }
        else if(op==T_PLUS){ E(f,"mov ecx,%s",kd_of(t->elem)); CALLRT(f,"rt_list_concat"); return 1; }
    }
    if(t->k==TY_SET){
        int code=op==T_PIPE?0:op==T_AMP?1:op==T_MINUS?2:3;
        E(f,"mov edx,eax"); E(f,"pop eax"); E(f,"mov ecx,%d",code); CALLRT(f,"rt_set_op"); return 1;
    }
    (void)tb;
    cg_fail(line,"unsupported operator");
}

/* self.m(arg): an operator implemented by a method -> its result (owned if a reference) */
static void call_on_top(F *f, AFunc *m){
    use_fn(m);
    E(f,"mov eax,[esp]"); attr_check(f,m->name);
    if(m->overridden){ E(f,"mov eax,[eax+8]"); E(f,"call dword [eax+%d]",8+4*m->vslot); }
    else E(f,"call %s",fnl(m,LB_CODE));
}
/* the size of the arguments of an operator method's call: self, then the rest (the operand, defaults) */
static int op_frame(AFunc *m){ int t=4; for(int i=1;i<m->nparams;i++) t+=esize(m->params[i]->ty); return t; }
/* the defaults of its parameters after the used ones (constants: see op_method_of) */
static void op_defaults(F *f, AFunc *m, int used){
    int off=4; for(int i=1;i<used && i<m->nparams;i++) off+=esize(m->params[i]->ty);
    for(int i=used;i<m->nparams;i++){ Ty *pt=m->params[i]->ty; gen_as(f,m->defaults[i],pt,m->params[i]->consumed); put_arg(f,pt,off); off+=esize(pt); }
}
/* self.__r<op>__(arg) for `arg <op> self`: arg (the left operand) evaluated first */
static int gen_op_call_r(F *f, AFunc *m, Expr *self, Expr *arg){
    Ty *pt=m->params[1]->ty; int total=op_frame(m);
    E(f,"sub esp,%d",total);
    gen_as(f,arg,pt,0); put_arg(f,pt,4);
    op_defaults(f,m,2);
    gen_borrow(f,self); E(f,"mov [esp],eax");
    call_on_top(f,m);
    E(f,"add esp,%d",total);
    return is_ptr(m->ret);
}
static int gen_op_call(F *f, AFunc *m, Expr *self, Expr *arg){
    int total=op_frame(m);
    E(f,"sub esp,%d",total);
    gen_borrow(f,self); E(f,"mov [esp],eax");
    if(arg){ Ty *pt=m->params[1]->ty; gen_as(f,arg,pt,0); put_arg(f,pt,4); }
    op_defaults(f,m,arg?2:1);
    call_on_top(f,m);
    E(f,"add esp,%d",total);
    return is_ptr(m->ret);
}
static int gen_format(F *f, Expr *e);
static int concat_worth(Expr *e);
static int gen_concat(F *f, Expr *e);
/* Python's spelling of a binary operator (for messages) */
static const char *op_symbol(TokKind op){
    switch(op){
        case T_PLUS: case T_PLUS_ASSIGN: return "+"; case T_MINUS: case T_MINUS_ASSIGN: return "-";
        case T_STAR: case T_STAR_ASSIGN: return "*"; case T_SLASH: case T_SLASH_ASSIGN: return "/";
        case T_FLOOR_DIV: case T_FLOOR_DIV_ASSIGN: return "//"; case T_PERCENT: case T_PERCENT_ASSIGN: return "%";
        case T_POWER: case T_POWER_ASSIGN: return "** or pow()"; case T_AMP: case T_AMP_ASSIGN: return "&";
        case T_PIPE: case T_PIPE_ASSIGN: return "|"; case T_CARET: case T_CARET_ASSIGN: return "^";
        case T_SHL: case T_SHL_ASSIGN: return "<<"; case T_SHR: case T_SHR_ASSIGN: return ">>"; default: return "?";
    }
}
static int is_aug_op(TokKind op){ return op==T_PLUS_ASSIGN||op==T_MINUS_ASSIGN||op==T_STAR_ASSIGN||op==T_SLASH_ASSIGN||op==T_FLOOR_DIV_ASSIGN||op==T_PERCENT_ASSIGN||op==T_POWER_ASSIGN||op==T_AMP_ASSIGN||op==T_PIPE_ASSIGN||op==T_CARET_ASSIGN||op==T_SHL_ASSIGN||op==T_SHR_ASSIGN; }
/* the TypeError of `None op b` (left) / `a op None` (right) */
static const char *binop_none_msg(TokKind op, Ty *ta, Ty *tb, int left){
    const char *sym=op_symbol(op); char aug[24]; snprintf(aug,sizeof aug,"%s%s",sym,is_aug_op(op)&&op!=T_POWER_ASSIGN?"=":"");
    if(op==T_POWER_ASSIGN) snprintf(aug,sizeof aug,"**=");
    Ty *x=ty_find(left?tb:ta);
    if(!left && (op==T_PLUS||op==T_PLUS_ASSIGN) && (x->k==TY_STR||(x->k==TY_LIST&&!x->tup))) return none_msg("can only concatenate %s (not \"NoneType\") to %s",py_type_name(x),py_type_name(x));
    if((op==T_STAR||op==T_STAR_ASSIGN) && !left && (x->k==TY_STR||x->k==TY_LIST)) return none_msg("can't multiply sequence by non-int of type 'NoneType'");
    return left ? none_msg("unsupported operand type(s) for %s: 'NoneType' and '%s'",aug,py_type_name(x))
                : none_msg("unsupported operand type(s) for %s: '%s' and 'NoneType'",aug,py_type_name(x));
}
static int gen_binary(F *f, Expr *e){
    Ty *t=TY(e), *ta=TY(e->a), *tb=TY(e->b);
    if(xinfo(e)->kind==X_OPMETHOD) return xinfo(e)->reflected ? gen_op_call_r(f,xinfo(e)->fn,e->b,e->a) : gen_op_call(f,xinfo(e)->fn,e->a,e->b);
    if(e->op==T_POWER && t->k==TY_FLOAT && e->b->kind==EXPR_LITERAL && e->b->tok->is_float && e->b->tok->f==0.5){
        g_none_text=binop_none_msg(e->op,ta,tb,1); gen_as(f,e->a,TY_FLOAT_T,0); g_none_text=NULL; E(f,"fsqrt"); return 0; }   /* x ** 0.5: exact */
    if(e->op==T_PERCENT && ta->k==TY_STR) return gen_format(f,e);
    if(e->op==T_PLUS && t->k==TY_STR && concat_worth(e) && !is_opt(ta) && !is_opt(tb)) return gen_concat(f,e);
    int num=numeric_ty(t);
    char *ma=xstrdup2(binop_none_msg(e->op,ta,tb,1)), *mb=xstrdup2(binop_none_msg(e->op,ta,tb,0));   /* (an operand's own code may set g_none_text) */
    g_none_text=ma;
    gen_as(f,e->a,num?t:ta,0); if(!num) check_none(f,ta,"TypeError",ma);
    push_value(f,num?t:ta);
    g_none_text=mb;
    gen_as(f,e->b,num?t:tb,0); if(!num) check_none(f,tb,"TypeError",mb);
    g_none_text=NULL;
    return apply_binop(f,e->op,t,ta,tb,e->line);
}

/* ---- truth values ---- */
static int has_length(Ty *t){ t=ty_find(t); return t->k==TY_STR||t->k==TY_BYTES||t->k==TY_LIST||t->k==TY_DICT||t->k==TY_SET||t->k==TY_BUF; }
/* eax = truth of the value in eax/st0 (references stay put) */
static void truth_of(F *f, Ty *t);
static void truth(F *f, Ty *t){
    t=ty_find(t);
    if(t->opt && !is_ptr(t)){                              /* None is false */
        int lnone=new_label(), lend=new_label();
        jump_if_none(f,t,lnone); truth_of(f,t); E(f,"jmp L%d",lend);
        LBL(f,lnone); drop_value(f,t,0); E(f,"xor eax,eax"); LBL(f,lend); return; }
    truth_of(f,t);
}
/* the method giving an object's truth: __bool__, else __len__ (NULL: always true) */
static AFunc *truth_method(Ty *t){
    t=ty_find(t); if(t->k!=TY_OBJ || !t->cls) return NULL;
    AFunc *m=aot_find_method(t->cls,"__bool__"); if(!m) m=aot_find_method(t->cls,"__len__");
    return m && m->nparams==1 ? m : NULL;
}
/* eax = the truth of the object in eax (kept in place) by its method; a null reference is false */
static void truth_call(F *f, AFunc *m){
    int l=new_label();
    E(f,"test eax,eax"); E(f,"jz L%d",l);
    E(f,"push eax"); call_method_on_top(f,m); E(f,"add esp,4");
    if(ty_find(m->ret)->k!=TY_BOOL){ E(f,"or eax,edx"); E(f,"setne al"); E(f,"movzx eax,al"); }
    LBL(f,l);
}
static void truth_of(F *f, Ty *t){
    t=ty_find(t);
    if(t->k==TY_BOOL) return;
    { AFunc *m=truth_method(t); if(m){ truth_call(f,m); return; } }
    if(t->k==TY_FLOAT){ E(f,"ftst"); E(f,"fnstsw ax"); E(f,"fstp st0"); E(f,"sahf"); E(f,"setne al"); E(f,"movzx eax,al"); return; }
    if(t->k==TY_INT){ E(f,"or eax,edx"); E(f,"setne al"); E(f,"movzx eax,al"); return; }
    if(has_length(t)){ int l=new_label(); E(f,"xor ecx,ecx"); E(f,"test eax,eax"); E(f,"jz L%d",l); E(f,"cmp dword [eax+8],0"); E(f,"setne cl"); LBL(f,l); E(f,"mov eax,ecx"); return; }
    E(f,"test eax,eax"); E(f,"setne al"); E(f,"movzx eax,al");
}
/* jump to `label` when the truth of eax/st0 equals want, keeping the value */
static void truth_jump(F *f, Ty *t, int label, int want){
    t=ty_find(t);
    if(t->opt && !is_ptr(t)){                              /* None is false */
        int lnone=new_label(), lend=new_label();
        jump_if_none(f,t,lnone);
        Ty plain=*t; plain.opt=0; truth_jump(f,&plain,label,want); E(f,"jmp L%d",lend);
        LBL(f,lnone); if(!want) E(f,"jmp L%d",label);
        LBL(f,lend); return; }
    if(t->k==TY_FLOAT){ E(f,"ftst"); E(f,"fnstsw ax"); E(f,"sahf"); E(f,"j%s L%d",want?"ne":"e",label); return; }
    if(t->k==TY_INT){ E(f,"mov ecx,eax"); E(f,"or ecx,edx"); E(f,"j%s L%d",want?"nz":"z",label); return; }
    { AFunc *m=truth_method(t);
      if(m){ E(f,"push eax"); truth_call(f,m); E(f,"test eax,eax"); E(f,"pop eax"); E(f,"j%s L%d",want?"nz":"z",label); return; } }
    if(has_length(t)){
        if(want){ int skip=new_label(); E(f,"test eax,eax"); E(f,"jz L%d",skip); E(f,"cmp dword [eax+8],0"); E(f,"jne L%d",label); LBL(f,skip); }
        else { E(f,"test eax,eax"); E(f,"jz L%d",label); E(f,"cmp dword [eax+8],0"); E(f,"je L%d",label); }
        return;
    }
    E(f,"test eax,eax"); E(f,"j%s L%d",want?"nz":"z",label);
}

/* and/or: the deciding operand's value */
static int gen_boolop(F *f, Expr *e){
    Ty *t=TY(e); int lend=new_label();
    gen_as(f,e->a,t,1);
    truth_jump(f,t,lend,e->op==T_OR);
    drop_value(f,t,1);
    gen_as(f,e->b,t,1);
    LBL(f,lend);
    return 1;
}

/* ---- comparisons ---- */
static int is_none_lit(Expr *e){ return e->kind==EXPR_NONE; }
static const char *cc_of(int code, int is_float){
    switch(code){
        case CMP_LT: return is_float?"b":"l";
        case CMP_LE: return is_float?"be":"le";
        case CMP_GT: return is_float?"a":"g";
        case CMP_GE: return is_float?"ae":"ge";
        case CMP_EQ: case CMP_IS: case CMP_IN: return "e";
        default: return "ne";
    }
}
/* the type two numbers are compared as: float if either is, int if either is, else bool */
static Ty *num_common(Ty *a, Ty *b){
    a=ty_find(a); b=ty_find(b);
    if(a->k==TY_FLOAT||b->k==TY_FLOAT) return TY_FLOAT_T;
    if(a->k==TY_INT||b->k==TY_INT) return TY_INT_T;
    return TY_BOOL_T;
}
/* the int on the stack (dropped) <code> the one in edx:eax -> eax = bool */
static void cmp64(F *f, int code){
    switch(code){
        case CMP_EQ: case CMP_NE: case CMP_IS: case CMP_ISNOT:
            E(f,"xor eax,[esp]"); E(f,"xor edx,[esp+4]"); E(f,"or eax,edx"); E(f,"set%s al",(code==CMP_EQ||code==CMP_IS)?"e":"ne"); break;
        case CMP_LT: case CMP_GE:                       /* left - right */
            E(f,"mov ecx,[esp]"); E(f,"sub ecx,eax"); E(f,"mov ecx,[esp+4]"); E(f,"sbb ecx,edx"); E(f,"set%s al",code==CMP_LT?"l":"ge"); break;
        default:                                        /* >, <=: right - left */
            E(f,"sub eax,[esp]"); E(f,"sbb edx,[esp+4]"); E(f,"set%s al",code==CMP_GT?"l":"ge"); break;
    }
    E(f,"movzx eax,al"); E(f,"add esp,8");
}
static const char *neg_cc(const char *cc);
/* jump to `label` when (the int on the stack <code> the one in edx:eax) == want; the stack int is dropped */
static void jcc64(F *f, int code, int want, int label){
    const char *cc;
    switch(code){
        case CMP_EQ: case CMP_NE: E(f,"xor eax,[esp]"); E(f,"xor edx,[esp+4]"); E(f,"or eax,edx"); cc=code==CMP_EQ?"e":"ne"; break;
        case CMP_LT: case CMP_GE: E(f,"mov ecx,[esp]"); E(f,"sub ecx,eax"); E(f,"mov ecx,[esp+4]"); E(f,"sbb ecx,edx"); cc=code==CMP_LT?"l":"ge"; break;
        default: E(f,"sub eax,[esp]"); E(f,"sbb edx,[esp+4]"); cc=code==CMP_GT?"l":"ge"; break;
    }
    E(f,"lea esp,[esp+8]");                             /* keeps the flags */
    E(f,"j%s L%d",want?cc:neg_cc(cc),label);
}
/* Left value pushed on the machine stack, right in eax/st0 -> eax = bool. */
static void cmp_finish(F *f, int code, Ty *ta, Ty *tb, int line){
    ta=ty_find(ta); tb=ty_find(tb);
    if(code==CMP_IN||code==CMP_NOTIN){
        if(tb->k==TY_STR||(tb->k==TY_BYTES&&ta->k==TY_BYTES)){ E(f,"pop edx"); CALLRT(f,"rt_str_contains"); if(code==CMP_NOTIN) E(f,"xor eax,1"); return; }
        if(tb->k==TY_BYTES){                                    /* a byte value in bytes */
            if(is_lng(ta)){ E(f,"pop ecx"); E(f,"pop edx"); } else { E(f,"pop ecx"); E(f,"xor edx,edx"); }
            CALLRT(f,"rt_bytes_has"); if(code==CMP_NOTIN) E(f,"xor eax,1"); return; }
        /* the value is on the machine stack: its address goes to the runtime */
        int sz=esize(ta);
        E(f,"mov edx,esp");
        if(tb->k==TY_SET){ CALLRT(f,"rt_set_contains"); E(f,"add esp,%d",sz); if(code==CMP_NOTIN) E(f,"xor eax,1"); return; }
        CALLRT(f,tb->k==TY_DICT?"rt_dict_find":"rt_list_find"); E(f,"add esp,%d",sz);
        E(f,"cmp eax,-1"); E(f,"set%s al",code==CMP_IN?"ne":"e"); E(f,"movzx eax,al"); return;
    }
    if(numeric_ty(ta)&&numeric_ty(tb)){
        if(ta->k==TY_FLOAT||tb->k==TY_FLOAT){
            conv_num(f,tb,TY_FLOAT_T);
            if(ta->k==TY_FLOAT){ E(f,"fld qword [esp]"); E(f,"add esp,8"); }
            else if(ta->k==TY_INT){ E(f,"fild qword [esp]"); E(f,"add esp,8"); }
            else { E(f,"fild dword [esp]"); E(f,"add esp,4"); }
            E(f,"fcompp"); E(f,"fnstsw ax"); E(f,"sahf");
            E(f,"set%s al",cc_of(code,1));                     /* a NaN (unordered: PF) is unequal to everything */
            if(code==CMP_NE){ E(f,"setp cl"); E(f,"or al,cl"); } else { E(f,"setnp cl"); E(f,"and al,cl"); }
            E(f,"movzx eax,al"); return;
        }
        if(ta->k==TY_INT||tb->k==TY_INT){ conv_num(f,tb,TY_INT_T); cmp64(f,code); return; }     /* (both int: see num_common) */
        E(f,"mov ecx,eax"); E(f,"pop eax"); E(f,"cmp eax,ecx"); E(f,"set%s al",cc_of(code,0)); E(f,"movzx eax,al"); return;
    }
    if((ta->k==TY_STR&&tb->k==TY_STR)||(ta->k==TY_BYTES&&tb->k==TY_BYTES)){
        E(f,"mov edx,eax"); E(f,"pop eax");
        if(code==CMP_EQ||code==CMP_NE){ CALLRT(f,"rt_str_eq"); if(code==CMP_NE) E(f,"xor eax,1"); return; }
        CALLRT(f,"rt_str_cmp"); E(f,"cmp eax,0"); E(f,"set%s al",cc_of(code,0)); E(f,"movzx eax,al"); return;
    }
    if(ta->k==TY_TUPLE&&tb->k==TY_TUPLE){
        E(f,"mov edx,eax"); E(f,"pop eax");
        if((code==CMP_EQ||code==CMP_NE) && aot_tuple_prefix(ta,tb)){ E(f,"mov eax,%d",code==CMP_NE); return; }   /* different lengths */
        if(code==CMP_EQ||code==CMP_NE){ CALLRT(f,"rt_tuple_eq"); if(code==CMP_NE) E(f,"xor eax,1"); return; }
        CALLRT(f,"rt_tuple_cmp"); E(f,"cmp eax,0"); E(f,"set%s al",cc_of(code,0)); E(f,"movzx eax,al"); return;
    }
    if(ta->k==TY_DICT&&(code==CMP_EQ||code==CMP_NE)){
        E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_dict_eq");
        if(code==CMP_NE) E(f,"xor eax,1"); return;
    }
    if((ta->k==TY_LIST||ta->k==TY_SET)&&(code==CMP_EQ||code==CMP_NE)){
        E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,ta->k==TY_LIST?"rt_list_eq":"rt_set_eq");
        if(code==CMP_NE) E(f,"xor eax,1"); return;
    }
    if(ta->k==TY_LIST&&tb->k==TY_LIST&&code<=CMP_GE){         /* lists: the first unequal elements decide */
        E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_list_cmp"); E(f,"cmp eax,0"); E(f,"set%s al",cc_of(code,0)); E(f,"movzx eax,al"); return;
    }
    if(ta->k==TY_SET&&tb->k==TY_SET&&code<=CMP_GE){          /* sets: subset / superset */
        int sup=code==CMP_GT||code==CMP_GE, strict=code==CMP_LT||code==CMP_GT;
        E(f,"mov edx,eax"); E(f,"pop eax");
        if(sup) E(f,"xchg eax,edx");                          /* a >= b: b <= a */
        if(strict){ int l=new_label(); E(f,"push eax"); E(f,"push edx"); CALLRT(f,"rt_set_le");
            E(f,"pop edx"); E(f,"pop ecx"); E(f,"test eax,eax"); E(f,"jz L%d",l);
            E(f,"mov eax,[ecx+8]"); E(f,"cmp eax,[edx+8]"); E(f,"setne al"); E(f,"movzx eax,al"); LBL(f,l); return; }
        CALLRT(f,"rt_set_le"); return;
    }
    if(code==CMP_EQ||code==CMP_NE||code==CMP_IS||code==CMP_ISNOT){     /* identity */
        E(f,"mov ecx,eax"); E(f,"pop eax"); E(f,"cmp eax,ecx"); E(f,"set%s al",cc_of(code,0)); E(f,"movzx eax,al"); return;
    }
    cg_fail(line,"unsupported comparison");
}
static void gen_cmp2_m(F *f, int code, Expr *a, Expr *b, int line, AFunc *m, int neg, int swap){
    (void)line;
    Ty *r=ty_find(m->ret);
    if(swap){ gen_op_call_r(f,m,b,a); if(r->k!=TY_BOOL) truth(f,r); if(neg) E(f,"xor eax,1"); return; }   /* 1 < obj: obj.__gt__(1) */
    if((code==CMP_EQ||code==CMP_NE) && is_opt(TY(a))){       /* a maybe None: None == b is b is None */
        Ty *pt=m->params[1]->ty; int total=op_frame(m), lcall=new_label(), ldone=new_label();
        E(f,"sub esp,%d",total);
        gen_borrow(f,a); E(f,"mov [esp],eax");
        gen_as(f,b,pt,0); put_arg(f,pt,4);
        op_defaults(f,m,2);
        E(f,"mov eax,[esp]"); E(f,"test eax,eax"); E(f,"jnz L%d",lcall);
        if(is_ptr(pt)){ E(f,"mov eax,[esp+4]"); E(f,"test eax,eax"); E(f,"sete al"); E(f,"movzx eax,al"); }
        else E(f,"xor eax,eax");
        E(f,"jmp L%d",ldone);
        LBL(f,lcall);
        call_on_top(f,m);
        if(r->k!=TY_BOOL) truth(f,r);
        LBL(f,ldone);
        E(f,"add esp,%d",total);
        if(neg) E(f,"xor eax,1");
        return; }
    if(code==CMP_IN||code==CMP_NOTIN){ int o=gen_op_call(f,m,b,a); (void)o; }     /* b.__contains__(a) */
    else gen_op_call(f,m,a,b);
    if(r->k!=TY_BOOL) truth(f,r);
    if(neg || code==CMP_NOTIN) E(f,"xor eax,1");
}
static void gen_cmp2(F *f, int code, Expr *a, Expr *b, int line){
    if(is_none_lit(a)||is_none_lit(b)){                 /* x is None / x == None */
        Expr *x=is_none_lit(a)?b:a; int eq=code==CMP_EQ||code==CMP_IS;
        if(is_none_lit(x)){ E(f,"mov eax,%d",eq); return; }
        Ty *t=TY(x); int o=gen(f,x);
        if(!is_opt(t) && !is_ptr(t)){ drop_value(f,t,o); E(f,"mov eax,%d",!eq); return; }   /* never None */
        if(is_ptr(t)){ if(o) hold(f); E(f,"test eax,eax"); E(f,"set%s al",eq?"z":"nz"); E(f,"movzx eax,al"); return; }
        int lnone=new_label(), lend=new_label();
        jump_if_none(f,t,lnone); drop_value(f,t,0); E(f,"mov eax,%d",!eq); E(f,"jmp L%d",lend);
        LBL(f,lnone); drop_value(f,t,0); E(f,"mov eax,%d",eq); LBL(f,lend);
        return;
    }
    Ty *ta=TY(a), *tb=TY(b);
    if((code==CMP_IN||code==CMP_NOTIN) && (tb->k==TY_LIST||tb->k==TY_SET)) ta=tb->elem;           /* as the elements are */
    if((code==CMP_IN||code==CMP_NOTIN) && tb->k==TY_DICT) ta=ty_dkey(tb);
    if(code!=CMP_IN && code!=CMP_NOTIN && numeric_ty(ta) && numeric_ty(tb)){                      /* numbers: as one type */
        Ty *ct=num_common(ta,tb);
        if((is_opt(ta)||is_opt(tb)) && (code==CMP_EQ||code==CMP_NE||code==CMP_IS||code==CMP_ISNOT)){   /* None == x: no error */
            int sa=frame_slot(f,8), sb=frame_slot(f,8), cnt=frame_slot(f,4), lnum=new_label(), lend=new_label();
            gen_borrow(f,a); store_new(f,ta,"ebp",sa); gen_borrow(f,b); store_new(f,tb,"ebp",sb);
            E(f,"mov dword [ebp%+d],0",cnt);
            for(int k=0;k<2;k++){ Ty *tk=k?tb:ta; if(!is_opt(tk)) continue; int ln=new_label(), lx=new_label();
                load_mem(f,tk,"ebp",k?sb:sa); jump_if_none(f,tk,ln); drop_value(f,tk,0); E(f,"jmp L%d",lx);
                LBL(f,ln); drop_value(f,tk,0); E(f,"add dword [ebp%+d],%d",cnt,k?2:1); LBL(f,lx); }
            E(f,"cmp dword [ebp%+d],0",cnt); E(f,"je L%d",lnum);
            E(f,"cmp dword [ebp%+d],3",cnt); E(f,"set%s al",(code==CMP_EQ||code==CMP_IS)?"e":"ne"); E(f,"movzx eax,al"); E(f,"jmp L%d",lend);
            LBL(f,lnum);
            load_mem(f,ta,"ebp",sa); conv_num(f,ta,ct); push_value(f,ct); load_mem(f,tb,"ebp",sb); conv_num(f,tb,ct);
            cmp_finish(f,code,ct,ct,line); LBL(f,lend); return;
        }
        static const char *sym[]={"<","<=",">",">="};
        if(code<=CMP_GE) g_none_text=none_msg("'%s' not supported between instances of 'NoneType' and '%s'",sym[code],py_type_name(tb));
        gen_as(f,a,ct,0); push_value(f,ct);
        if(code<=CMP_GE) g_none_text=none_msg("'%s' not supported between instances of '%s' and 'NoneType'",sym[code],py_type_name(ta));
        gen_as(f,b,ct,0); g_none_text=NULL;
        cmp_finish(f,code,ct,ct,line); return;
    }
    if(code<=CMP_GE && (is_opt(ta)||is_opt(tb))){                  /* ordering None: TypeError */
        static const char *sym[]={"<","<=",">",">="};
        gen_as(f,a,ta,0); check_none(f,ta,"TypeError",none_msg("'%s' not supported between instances of 'NoneType' and '%s'",sym[code],py_type_name(tb)));
        push_value(f,ta); gen_borrow(f,b);
        check_none(f,tb,"TypeError",none_msg("'%s' not supported between instances of '%s' and 'NoneType'",sym[code],py_type_name(ta)));
        cmp_finish(f,code,ta,tb,line); return;
    }
    gen_as(f,a,ta,0); push_value(f,ta);
    gen_borrow(f,b);
    if(code==CMP_IN||code==CMP_NOTIN) check_none(f,tb,"TypeError","argument of type 'NoneType' is not a container or iterable");
    cmp_finish(f,code,ta,tb,line);
}
/* a < b < c where some comparison is an object's method: each operand once, kept in a slot */
static void gen_compare_chain_m(F *f, Expr *e){
    XInfo *xi=xinfo(e); int lend=new_label(), ps=frame_slot(f,8), cs=frame_slot(f,8);
    Ty *prev=TY(e->items[0]);
    gen_borrow(f,e->items[0]); store_new(f,prev,"ebp",ps);
    for(int i=1;i<e->count;i++){
        Ty *t=TY(e->items[i]); int code=e->items[i]->akind; AFunc *m=xi->cmpfn[i];
        gen_borrow(f,e->items[i]); store_new(f,t,"ebp",cs);
        if(m){
            int contains= code==CMP_IN||code==CMP_NOTIN || ((xi->cmpswap>>i)&1);   /* b.__contains__(a), b.__gt__(a) */
            Ty *at=contains?prev:t, *pt=m->params[1]->ty; int sslot=contains?cs:ps, aslot=contains?ps:cs, total=op_frame(m);
            E(f,"sub esp,%d",total);
            E(f,"mov eax,[ebp%+d]",sslot); E(f,"mov [esp],eax");
            load_mem(f,at,"ebp",aslot); conv_num(f,at,pt); put_arg(f,pt,4);
            op_defaults(f,m,2);
            call_on_top(f,m); E(f,"add esp,%d",total);
            Ty *r=ty_find(m->ret); if(r->k!=TY_BOOL) truth(f,r);
            if(((xi->cmpneg>>i)&1) || code==CMP_NOTIN) E(f,"xor eax,1");
        } else {
            Ty *ct=(code!=CMP_IN && code!=CMP_NOTIN && numeric_ty(prev) && numeric_ty(t)) ? num_common(prev,t) : NULL;
            Ty *lt=ct?ct:prev, *rtt=ct?ct:t;
            load_mem(f,prev,"ebp",ps); conv_num(f,prev,lt); push_value(f,lt);
            load_mem(f,t,"ebp",cs); conv_num(f,t,rtt);
            cmp_finish(f,code,lt,rtt,e->line);
        }
        if(i<e->count-1){ E(f,"test eax,eax"); E(f,"jz L%d",lend); load_mem(f,t,"ebp",cs); store_new(f,t,"ebp",ps); }
        prev=t;
    }
    LBL(f,lend);
}
static void gen_compare(F *f, Expr *e){
    XInfo *xi=xinfo(e);
    if(e->count==2 && xi->cmpfn && xi->cmpfn[1]){ gen_cmp2_m(f,e->items[1]->akind,e->items[0],e->items[1],e->line,xi->cmpfn[1],(xi->cmpneg>>1)&1,(xi->cmpswap>>1)&1); return; }
    if(e->count==2){ gen_cmp2(f,e->items[1]->akind,e->items[0],e->items[1],e->line); return; }
    if(xi->cmpfn) for(int i=1;i<e->count;i++) if(xi->cmpfn[i]){ gen_compare_chain_m(f,e); return; }
    int lend=new_label(), slot=frame_slot(f,8);           /* a < b < c: each middle operand once */
    Ty *prev=TY(e->items[0]);
    gen_borrow(f,e->items[0]);
    store_new(f,prev,"ebp",slot);
    for(int i=1;i<e->count;i++){
        Ty *t=TY(e->items[i]); int code=e->items[i]->akind;
        Ty *ct=(code!=CMP_IN && code!=CMP_NOTIN && numeric_ty(prev) && numeric_ty(t)) ? num_common(prev,t) : NULL;
        Ty *lt=ct?ct:prev, *rtt=ct?ct:t;
        load_mem(f,prev,"ebp",slot); conv_num(f,prev,lt); push_value(f,lt);
        gen_borrow(f,e->items[i]);
        if(i<e->count-1){ if(is_flt(t)) E(f,"fst qword [ebp%+d]",slot); else store_new(f,t,"ebp",slot); }
        conv_num(f,t,rtt);
        cmp_finish(f,code,lt,rtt,e->line);
        if(i<e->count-1){ E(f,"test eax,eax"); E(f,"jz L%d",lend); }
        prev=t;
    }
    LBL(f,lend);
}

/* eax = truth of e */
static void gen_bool(F *f, Expr *e){
    if(e->kind==EXPR_COMPARE){ gen_compare(f,e); return; }
    if(e->kind==EXPR_UNARY && e->op==T_NOT){ gen_bool(f,e->a); E(f,"xor eax,1"); return; }
    if(e->kind==EXPR_BOOL){ int yes=new_label(), done=new_label();       /* (its operands may differ in type) */
        gen_jump(f,e,yes,1); E(f,"xor eax,eax"); E(f,"jmp L%d",done); LBL(f,yes); E(f,"mov eax,1"); LBL(f,done); return; }
    if(e->kind==EXPR_TRUE){ E(f,"mov eax,1"); return; }
    if(e->kind==EXPR_FALSE||e->kind==EXPR_NONE){ E(f,"xor eax,eax"); return; }
    gen_borrow(f,e);
    truth(f,TY(e));
}
static const char *neg_cc(const char *cc){
    static const char *pairs[][2]={{"l","ge"},{"ge","l"},{"le","g"},{"g","le"},{"e","ne"},{"ne","e"}};
    for(int i=0;i<6;i++) if(!strcmp(cc,pairs[i][0])) return pairs[i][1];
    return "ne";
}
static int int_like(Ty *t){ t=ty_find(t); return t->k==TY_INT||t->k==TY_BOOL; }
/* Jump to `label` when the truth of e equals want. Temporaries are released first. */
static void gen_jump(F *f, Expr *e, int label, int want){
    if(e->kind==EXPR_UNARY && e->op==T_NOT){ gen_jump(f,e->a,label,!want); return; }
    if(e->kind==EXPR_TRUE||e->kind==EXPR_FALSE){ if((e->kind==EXPR_TRUE)==want) E(f,"jmp L%d",label); return; }
    if(e->kind==EXPR_BOOL){
        int both=(e->op==T_OR);
        if(want==both){ gen_jump(f,e->a,label,want); gen_jump(f,e->b,label,want); }
        else { int skip=new_label(); gen_jump(f,e->a,skip,!want); gen_jump(f,e->b,label,want); LBL(f,skip); }
        return;
    }
    int mark=scope_open(f);
    if(e->kind==EXPR_COMPARE && e->count==2 && int_like(TY(e->items[0])) && int_like(TY(e->items[1]))
       && !is_opt(TY(e->items[0])) && !is_opt(TY(e->items[1]))                    /* (None == 5: gen_cmp2's way) */
       && !is_none_lit(e->items[0]) && !is_none_lit(e->items[1]) && e->items[1]->akind<=CMP_NE){
        Ty *ct=num_common(TY(e->items[0]),TY(e->items[1])); int code=e->items[1]->akind;
        gen_as(f,e->items[0],ct,0); push_value(f,ct); gen_as(f,e->items[1],ct,0);
        if(is_lng(ct)){
            if(f->tmp_used==mark){ jcc64(f,code,want,label); return; }
            cmp64(f,code);
        } else {
            E(f,"mov ecx,eax"); E(f,"pop eax");
            const char *cc=cc_of(code,0);
            if(f->tmp_used==mark){ E(f,"cmp eax,ecx"); E(f,"j%s L%d",want?cc:neg_cc(cc),label); return; }
            E(f,"cmp eax,ecx"); E(f,"set%s al",cc); E(f,"movzx eax,al");
        }
    } else gen_bool(f,e);
    if(f->tmp_used>mark){ E(f,"push eax"); scope_close(f,mark); E(f,"pop eax"); }
    E(f,"test eax,eax"); E(f,"j%s L%d",want?"nz":"z",label);
}

/* ---- iteration (for loops and comprehensions) ---- */
typedef enum { IT_RANGE, IT_SEQ, IT_STR, IT_KEYS, IT_ITEMS, IT_ENUM, IT_ZIP, IT_TUP, IT_GEN, IT_BYTES } ItKind;
typedef struct {
    ItKind kind;
    int top, cont, exit;
    int idx, end, step, start;      /* frame slots */
    int step_const, has_step_const;
    int src;                        /* reference slot of the iterated object (zip: srcs) */
    Ty *elem, *elem2;               /* element types (enumerate: elem2 = the items) */
    ItKind sub;                     /* enumerate: what the underlying object is */
    int nsrc, srcs[8]; ItKind subs[8]; Ty *elems[8];   /* zip: its iterables */
} Iter;
static ItKind seq_kind(Ty *t){ t=ty_find(t); return t->k==TY_STR?IT_STR:t->k==TY_BYTES?IT_BYTES:t->k==TY_DICT?IT_KEYS:t->k==TY_TUPLE?IT_TUP:t->k==TY_GEN?IT_GEN:IT_SEQ; }
static Ty *seq_elem(Ty *t){ t=ty_find(t); return t->k==TY_STR?TY_STR_T:t->k==TY_BYTES?TY_INT_T:t->k==TY_DICT?ty_dkey(t):t->k==TY_TUPLE?(t->nelems?t->elems[0]:TY_INT_T):t->elem; }   /* (an empty tuple: no items) */
/* the object a loop goes over, kept in a slot of its own: a sequence, a
   generator, obj.__iter__()'s result, a file's lines -> its kind and items */
static ItKind iter_source(F *f, Expr *x, int *slot, Ty **elem){
    Ty *t=TY(x);
    *slot=ref_slot(f);
    if(is_opt(t)){ int o=gen(f,x); if(o) hold(f); check_none(f,t,"TypeError","'NoneType' object is not iterable"); drop_value(f,t,0); }
    if(t->k==TY_OBJ){
        AFunc *m=aot_find_method(t->cls,"__iter__"); Ty *r=ty_find(m->ret);
        E(f,"sub esp,4"); gen_borrow(f,x); E(f,"mov [esp],eax"); call_method_on_top(f,m); E(f,"add esp,4");
        E(f,"mov [ebp%+d],eax",*slot); *elem=r->elem; return r->k==TY_GEN?IT_GEN:IT_SEQ;
    }
    if(t->k==TY_FILE){ gen_borrow(f,x); CALLRT(f,"rt_file_readlines"); E(f,"mov [ebp%+d],eax",*slot); *elem=t->elem?t->elem:TY_STR_T; return IT_SEQ; }
    if(t->k==TY_SET){ gen_borrow(f,x); E(f,"mov edx,%s",kd_of(t->elem)); CALLRT(f,"rt_set_to_list"); E(f,"mov [ebp%+d],eax",*slot); *elem=t->elem; return IT_SEQ; }   /* in its order */
    gen_owned(f,x); E(f,"mov [ebp%+d],eax",*slot);
    *elem=seq_elem(t); return seq_kind(t);
}
static void iter_begin(F *f, Expr *it, Iter *I){
    memset(I,0,sizeof *I);
    I->top=new_label(); I->cont=new_label(); I->exit=new_label();
    XInfo *xi=xinfo(it);
    int ranged=xi->kind==X_BUILTIN && (!strcmp(xi->name,"range")||!strcmp(xi->name,"reversed_range"));
    I->idx=frame_slot(f,ranged?8:4);                    /* a range's own int (64 bits); else a position */
    if(ranged){
        Expr *r=it; int rev=xi->name[0]=='r'&&xi->name[1]=='e';
        if(rev) r=it->items[0];
        I->kind=IT_RANGE; I->elem=TY_INT_T; I->end=frame_slot(f,8);
        Expr *lo=r->count>=2?r->items[0]:NULL, *hi=r->count>=2?r->items[1]:r->items[0];
        if(rev){                                        /* stop-1 down to start */
            gen_as(f,hi,TY_INT_T,0); E(f,"sub eax,1"); E(f,"sbb edx,0"); store_new(f,TY_INT_T,"ebp",I->idx);
            if(lo){ gen_as(f,lo,TY_INT_T,0); E(f,"sub eax,1"); E(f,"sbb edx,0"); } else gen_int_const(f,TY_INT_T,-1);
            store_new(f,TY_INT_T,"ebp",I->end);
            I->has_step_const=1; I->step_const=-1;
        } else {
            if(lo) gen_as(f,lo,TY_INT_T,0); else gen_int_const(f,TY_INT_T,0);
            store_new(f,TY_INT_T,"ebp",I->idx);
            gen_as(f,hi,TY_INT_T,0); store_new(f,TY_INT_T,"ebp",I->end);
            I->has_step_const=1; I->step_const=1;
            if(r->count==3){
                Expr *s=r->items[2]; int neg=0;
                if(s->kind==EXPR_UNARY&&s->op==T_MINUS&&s->a->kind==EXPR_LITERAL){ neg=1; s=s->a; }
                if(s->kind==EXPR_LITERAL && s->tok->kind==T_NUMBER && s->tok->i<=0x7FFFFFFF){ I->step_const=(int)(neg?-s->tok->i:s->tok->i); if(!I->step_const) cg_fail(it->line,"range() step must not be zero"); }
                else { I->has_step_const=0; I->step=frame_slot(f,8); gen_as(f,r->items[2],TY_INT_T,0); store_new(f,TY_INT_T,"ebp",I->step);
                    int ok=new_label(); E(f,"or eax,edx"); E(f,"jnz L%d",ok); E(f,"mov esi,Z%d",zlit("range() arg 3 must not be zero")); CALLRT(f,"rt_panic_value"); LBL(f,ok); }
            }
        }
        LBL(f,I->top);
        int up=new_label(), go=new_label();
        if(!I->has_step_const){ E(f,"cmp dword [ebp%+d],0",I->step+4); E(f,"jge L%d",up); }
        if(!I->has_step_const || I->step_const<0){           /* down: stop once idx <= end */
            E(f,"mov eax,[ebp%+d]",I->end); E(f,"cmp eax,[ebp%+d]",I->idx); E(f,"mov eax,[ebp%+d]",I->end+4); E(f,"sbb eax,[ebp%+d]",I->idx+4); E(f,"jge L%d",I->exit);
            if(!I->has_step_const) E(f,"jmp L%d",go); }
        LBL(f,up);
        if(!I->has_step_const || I->step_const>0){           /* up: stop once idx >= end */
            E(f,"mov eax,[ebp%+d]",I->idx); E(f,"cmp eax,[ebp%+d]",I->end); E(f,"mov eax,[ebp%+d]",I->idx+4); E(f,"sbb eax,[ebp%+d]",I->end+4); E(f,"jge L%d",I->exit); }
        LBL(f,go);
        return;
    }
    E(f,"mov dword [ebp%+d],0",I->idx);
    if(xi->kind==X_BUILTIN && (!strcmp(xi->name,"enumerate")||!strcmp(xi->name,"zip"))){
        int isenum=xi->name[0]=='e'; Ty *e0;
        I->kind=isenum?IT_ENUM:IT_ZIP;
        if(isenum){ I->sub=iter_source(f,it->items[0],&I->src,&e0);
            I->elem=TY_INT_T; I->elem2=e0; I->start=frame_slot(f,8);
            if(it->count==2) gen_as(f,it->items[1],TY_INT_T,0); else gen_int_const(f,TY_INT_T,0);
            store_new(f,TY_INT_T,"ebp",I->start); }
        else { if(it->count>8) cg_fail(it->line,"zip() of more than 8 iterables");
            I->nsrc=it->count;
            for(int k=0;k<it->count;k++) I->subs[k]=iter_source(f,it->items[k],&I->srcs[k],&I->elems[k]);
            I->src=I->srcs[0]; I->elem=I->elems[0]; I->elem2=it->count>1?I->elems[1]:NULL; }
    } else if(xi->kind==X_TMETHOD && !strcmp(xi->name,"items")){
        Ty *e0; iter_source(f,it->a->a,&I->src,&e0); I->kind=IT_ITEMS; I->elem=ty_dkey(TY(it->a->a)); I->elem2=TY(it->a->a)->elem;
    } else {
        Ty *el; I->kind=iter_source(f,it,&I->src,&el); I->elem=el;
    }
    LBL(f,I->top);
    for(int k=0;k<(I->kind==IT_ZIP?I->nsrc:1);k++){
        ItKind sk= I->kind==IT_ZIP ? I->subs[k] : I->kind==IT_ENUM ? I->sub : I->kind;
        E(f,"mov eax,[ebp%+d]",I->kind==IT_ZIP?I->srcs[k]:I->src); E(f,"test eax,eax"); E(f,"jz L%d",I->exit);
        if(sk==IT_GEN){ CALLRT(f,"rt_gen_next"); E(f,"test eax,eax"); E(f,"jz L%d",I->exit); continue; }
        E(f,"mov ecx,[ebp%+d]",I->idx);
        if(sk==IT_TUP){ E(f,"mov edx,[eax+8]"); E(f,"cmp ecx,[edx]"); }
        else if(sk==IT_STR){ E(f,"push ecx"); CALLRT(f,"rt_str_cplen"); E(f,"pop ecx"); E(f,"cmp ecx,eax"); }
        else E(f,"cmp ecx,[eax+8]");
        E(f,"jae L%d",I->exit);
    }
}
/* value number k of the current iteration (enumerate, items: 0 or 1; zip: which iterable) -> eax/st0 (borrowed) */
static Ty *iter_elem(Iter *I, int k){ return I->kind==IT_ZIP ? I->elems[k] : k ? I->elem2 : I->elem; }
static void iter_value(F *f, Iter *I, int k){
    ItKind kind=I->kind; int src=I->src; Ty *el=iter_elem(I,k);
    if(kind==IT_RANGE){ load_mem(f,TY_INT_T,"ebp",I->idx); return; }
    if(kind==IT_ENUM){ if(k==0){ E(f,"mov eax,[ebp%+d]",I->idx); E(f,"xor edx,edx"); E(f,"add eax,[ebp%+d]",I->start); E(f,"adc edx,[ebp%+d]",I->start+4); return; } kind=I->sub; }
    else if(kind==IT_ZIP){ kind=I->subs[k]; src=I->srcs[k]; }
    E(f,"mov eax,[ebp%+d]",src);
    if(kind==IT_GEN){ load_mem(f,el,"eax",24); return; }
    E(f,"mov ecx,[ebp%+d]",I->idx);
    if(kind==IT_STR){ E(f,"mov edx,ecx"); CALLRT(f,"rt_str_char"); return; }
    if(kind==IT_BYTES){ E(f,"movzx eax,byte [eax+12+ecx]"); E(f,"xor edx,edx"); return; }
    if(kind==IT_TUP){ E(f,"add eax,12"); load_at(f,el,"eax","ecx",esize(el)); return; }
    if(kind==IT_KEYS||(kind==IT_ITEMS&&k==0)){ E(f,"mov eax,[eax+16]"); E(f,"lea eax,[eax+ecx*8]"); load_mem(f,el,"eax",0); return; }   /* keys: 8 bytes each */
    if(kind==IT_ITEMS){ E(f,"mov eax,[eax+20]"); load_at(f,el,"eax","ecx",esize(el)); return; }
    E(f,"mov eax,[eax+16]");
    load_at(f,el,"eax","ecx",esize(el));
}
static void iter_end(F *f, Iter *I){
    LBL(f,I->cont);
    if(I->kind==IT_RANGE){
        if(I->has_step_const){ E(f,"add dword [ebp%+d],%d",I->idx,I->step_const); E(f,"adc dword [ebp%+d],%d",I->idx+4,I->step_const<0?-1:0); }
        else { E(f,"mov eax,[ebp%+d]",I->step); E(f,"add [ebp%+d],eax",I->idx); E(f,"mov eax,[ebp%+d]",I->step+4); E(f,"adc [ebp%+d],eax",I->idx+4); } }
    else E(f,"inc dword [ebp%+d]",I->idx);
    E(f,"jmp L%d",I->top);
}
static void iter_release(F *f, Iter *I){
    int n= I->kind==IT_ZIP ? I->nsrc : 1;
    for(int k=0;k<n;k++){ int sl= I->kind==IT_ZIP ? I->srcs[k] : I->src;
        if(sl){ rt("rt_decref"); E(f,"mov eax,[ebp%+d]",sl); E(f,"mov dword [ebp%+d],0",sl); E(f,"call rt_decref"); } }
}
/* store the iteration value (borrowed in eax/st0) into a variable */
static void store_borrowed(F *f, AVar *v, Ty *from){
    if(v->borrowed){ conv_num(f,from,v->ty); E(f,"mov [ebp%+d],eax",v->offset); return; }   /* the item stays the container's */
    if(is_ptr(v->ty)) incref(f);
    conv(f,from,v->ty);
    store_var(f,v);
}
static int tuple_off(Ty *t, int i);
static int tuple_size(Ty *t);
static int tdesc(Ty *t);
/* The loop variables of one iteration: `x`, `i, x` (enumerate/zip/items), or
   `a, b` unpacking each element (a list or a tuple). */
static void iter_store_vars(F *f, Iter *I, AVar **vars, int n){
    if(n==1 && (I->kind==IT_ENUM || I->kind==IT_ZIP || I->kind==IT_ITEMS)){     /* for t in zip(a, b): a new tuple each time */
        Ty *tt=ty_find(vars[0]->ty); int parts= I->kind==IT_ZIP ? I->nsrc : 2;
        E(f,"mov eax,TD%d",tdesc(tt)); E(f,"mov edx,%d",tuple_size(tt)); CALLRT(f,"rt_tuple_new"); E(f,"push eax");
        for(int k=0;k<parts;k++){ Ty *et=tt->elems[k];
            iter_value(f,I,k); conv_num(f,iter_elem(I,k),et); if(is_ptr(et)) incref(f);
            E(f,"mov ecx,[esp]"); store_mem(f,et,"ecx",tuple_off(tt,k)); }
        E(f,"pop eax"); conv(f,tt,vars[0]->ty); store_var(f,vars[0]);
        return;
    }
    if(n==1 || I->kind==IT_ENUM || I->kind==IT_ZIP || I->kind==IT_ITEMS){
        for(int k=0;k<n;k++){ iter_value(f,I,k); store_borrowed(f,vars[k],iter_elem(I,k)); }
        return;
    }
    Ty *et=ty_find(I->elem);
    iter_value(f,I,0); E(f,"push eax");
    if(et->k==TY_TUPLE){
        for(int k=0;k<n;k++){ E(f,"mov eax,[esp]"); panic_if_null(f); load_mem(f,et->elems[k],"eax",tuple_off(et,k)); store_borrowed(f,vars[k],et->elems[k]); }
    } else {
        Ty *el=et->elem;
        E(f,"mov edx,%d",n); CALLRT(f,"rt_unpack_check");
        for(int k=0;k<n;k++){
            E(f,"mov eax,[esp]"); E(f,"mov eax,[eax+16]");
            load_mem(f,el,"eax",esize(el)*k);
            store_borrowed(f,vars[k],el);
        }
    }
    E(f,"add esp,4");
}

static void store_target(F *f, Expr *t, Ty *vt, int line);
/* a comprehension clause with a target: its loop variable unpacked into it */
static void comp_unpack(F *f, Expr *e, int i){
    CompClause *cl=&e->clauses[i];
    for(int k=0;k<2;k++){ Expr *t=k?cl->target2:cl->target; if(!t) continue;
        AVar *v=xinfo(e)->cvars[2*i+k];
        load_var(f,v); if(is_ptr(v->ty)) incref(f);
        store_target(f,t,v->ty,e->line); }
}
static int gen_comprehension(F *f, Expr *e){
    Ty *t=TY(e);
    int res=ref_slot(f);
    if(e->comp_kind=='D') new_dict(f,t);
    else if(e->comp_kind=='S') new_set(f,t->elem);
    else new_list(f,t->elem);
    E(f,"mov [ebp%+d],eax",res);
    Iter its[8]; int n=e->nclause;
    if(n>8) cg_fail(e->line,"too many for clauses");
    for(int i=0;i<n;i++){
        CompClause *cl=&e->clauses[i];
        iter_begin(f,cl->iter,&its[i]);
        iter_store_vars(f,&its[i],&xinfo(e)->cvars[2*i],cl->nvars); comp_unpack(f,e,i);
        for(int k=0;k<cl->ncond;k++) gen_jump(f,cl->conds[k],its[i].cont,0);
    }
    int mark=scope_open(f);
    Ty *el=t->elem;
    if(e->comp_kind=='L'||e->comp_kind=='g'){
        gen_as(f,e->a,el,1);
        E(f,"push dword [ebp%+d]",res); list_append_top(f,el); E(f,"add esp,4");
    } else if(e->comp_kind=='S'){
        gen_as(f,e->a,el,0);
        E(f,"push dword [ebp%+d]",res); set_add_top(f,el); E(f,"add esp,4");
    } else {
        Ty *kt=ty_dkey(t);
        gen_as(f,e->b,el,1); push_value(f,el);
        gen_as(f,e->a,kt,0); push_value(f,kt);
        E(f,"mov edx,esp"); E(f,"mov eax,[ebp%+d]",res); CALLRT(f,"rt_dict_slot"); E(f,"add esp,%d",esize(kt));
        E(f,"mov ecx,eax"); pop_value(f,el); store_mem(f,el,"ecx",0);
    }
    scope_close(f,mark);
    for(int i=n-1;i>=0;i--){ iter_end(f,&its[i]); LBL(f,its[i].exit); iter_release(f,&its[i]); }
    E(f,"mov eax,[ebp%+d]",res); E(f,"mov dword [ebp%+d],0",res);
    return 1;
}

/* ---- tuples: +8 descriptor TD<i> (count, element kinds), items from +12 ---- */
static int tuple_off(Ty *t, int i){ t=ty_find(t); int off=12; for(int k=0;k<i;k++) off+=esize(t->elems[k]); return off; }
static int tuple_size(Ty *t){ t=ty_find(t); return tuple_off(t,t->nelems)-12; }
static const char *kd_of(Ty *t);
static int tdesc(Ty *t){
    t=ty_find(t);
    for(int i=0;i<gg->ntdescs;i++) if(ty_same_exact(gg->tdescs[i],t)) return i;
    for(int k=0;k<t->nelems;k++) kd_of(t->elems[k]);        /* now, while functions are still being emitted */
    if(gg->ntdescs==gg->ctdescs){ gg->ctdescs=gg->ctdescs?gg->ctdescs*2:8; gg->tdescs=(Ty**)xrealloc(gg->tdescs,sizeof(Ty*)*(size_t)gg->ctdescs); }
    gg->tdescs[gg->ntdescs]=t;
    return gg->ntdescs++;
}
static int gen_tuple(F *f, Expr *e){
    Ty *t=TY(e);
    Expr **items=e->kind==EXPR_DICT ? e->vals : e->items;          /* a record: {"key": value, ...} */
    E(f,"mov eax,TD%d",tdesc(t)); E(f,"mov edx,%d",tuple_size(t)); CALLRT(f,"rt_tuple_new");
    E(f,"push eax");
    for(int i=0;i<e->count;i++){ Ty *et=t->elems[i]; gen_as(f,items[i],et,1); E(f,"mov ecx,[esp]"); store_new(f,et,"ecx",tuple_off(t,i)); }
    E(f,"pop eax");
    return 1;
}
/* The fields of the tuple at [ebp+slot] (borrowed) into the targets. */
static void store_target(F *f, Expr *t, Ty *vt, int line);
static void unpack_tuple_slot(F *f, int slot, Ty *tt, Expr **targets, int n, int line){
    tt=ty_find(tt);
    for(int i=0;i<n;i++){
        Ty *et=tt->elems[i];
        E(f,"mov eax,[ebp%+d]",slot); panic_if_null(f);
        load_mem(f,et,"eax",tuple_off(tt,i));
        if(is_ptr(et)) incref(f);
        store_target(f,targets[i],et,line);
    }
}

/* ---- indexing ---- */
/* the registers of sys.syscall(...): eax..edi, ebp (0 unless given; ebp itself
   is kept around the call) -> after the call [esp] eax .. [esp+20] edi; 28 bytes */
static void gen_syscall_regs(F *f, Expr *e){
    E(f,"sub esp,28");
    for(int i=e->count;i<7;i++) E(f,"mov dword [esp+%d],0",4*i);      /* registers no argument sets */
    for(int i=0;i<e->count;i++){
        Ty *t=TY(e->items[i]);
        gen_borrow(f,e->items[i]);
        if(t->k==TY_STR||t->k==TY_BYTES){ int l=new_label(), l2=new_label();            /* its bytes, NUL-terminated ("" too) */
            E(f,"test eax,eax"); E(f,"jnz L%d",l); E(f,"mov eax,Z%d",zlit("")); E(f,"jmp L%d",l2); LBL(f,l); E(f,"add eax,12"); LBL(f,l2); }
        else if(t->k==TY_BUF){ int l=new_label(); E(f,"test eax,eax"); E(f,"jz L%d",l); E(f,"add eax,12"); LBL(f,l); }
        E(f,"mov [esp+%d],eax",4*i);
    }
    static const char *regs[6]={"eax","ebx","ecx","edx","esi","edi"};
    E(f,"push ebp"); E(f,"mov ebp,[esp+28]");                         /* (on Linux: the 6th argument) */
    for(int i=5;i>=0;i--) E(f,"mov %s,[esp+%d]",regs[i],4+4*i);
    E(f,gg->target==AOT_TARGET_KOLIBRI?"int 0x40":"int 0x80");
    for(int i=0;i<6;i++) E(f,"mov [esp+%d],%s",4+4*i,regs[i]);
    E(f,"pop ebp");
}
static int gen_index(F *f, Expr *e){
    if(e->a->kind==EXPR_CALL && xinfo(e->a)->kind==X_SYSCALL && e->b->kind==EXPR_LITERAL && e->b->tok->kind==T_NUMBER
       && !e->b->tok->is_float && e->b->tok->i>=0 && e->b->tok->i<6){        /* sys.syscall(...)[k]: just register k */
        gen_syscall_regs(f,e->a); E(f,"mov eax,[esp+%d]",4*(int)e->b->tok->i); E(f,"add esp,28"); widen(f,1); return 0;
    }
    Ty *ct=TY(e->a);
    if(xinfo(e)->kind==X_TYPEVAL){ E(f,"mov eax,%s",typeval_label(xinfo(e))); return 0; }      /* Box[int]: the class */
    if(ct->k==TY_OBJ) return gen_op_call(f,xinfo(e)->kind==X_OPMETHOD && xinfo(e)->fn ? xinfo(e)->fn : aot_find_method(ct->cls,"__getitem__"),e->a,e->b);   /* obj[key] (a generic one: its copy) */
    if(is_opt(ct)){ int o=gen(f,e->a); if(o) hold(f); check_none(f,ct,"TypeError","'NoneType' object is not subscriptable"); drop_value(f,ct,0); }
    if(ct->k==TY_TUPLE){
        XInfo *xi=xinfo(e);
        if(xi->argmap[1]){ gen_borrow(f,e->a); panic_if_null(f); load_mem(f,ct->elems[xi->argmap[0]],"eax",tuple_off(ct,xi->argmap[0])); return 0; }
        Ty *et=ct->elems[0];                                     /* all items of one type */
        gen_borrow(f,e->a); E(f,"push eax"); gen(f,e->b); to_i32(f,TY(e->b)); E(f,"mov edx,eax"); E(f,"pop eax");
        CALLRT(f,"rt_tuple_at"); load_mem(f,et,"eax",0); return 0;
    }
    gen_borrow(f,e->a); E(f,"push eax");
    if(ct->k==TY_DICT){                                          /* the key goes by address */
        Ty *kt=ty_dkey(ct); gen_as(f,e->b,kt,0); push_value(f,kt);
        E(f,"mov edx,esp"); E(f,"mov eax,[esp+%d]",esize(kt)); CALLRT(f,"rt_dict_get"); E(f,"add esp,%d",esize(kt)+4);
        load_mem(f,ct->elem,"eax",0); return 0;
    }
    gen(f,e->b); to_i32(f,TY(e->b));
    E(f,"mov edx,eax"); E(f,"pop eax");
    if(ct->k==TY_STR){ CALLRT(f,"rt_str_char"); return 0; }
    if(ct->k==TY_BYTES){ CALLRT(f,"rt_bytes_at"); widen(f,0); return 0; }
    CALLRT(f,"rt_list_at"); load_mem(f,ct->elem,"eax",0); return 0;
}
static int gen_slice(F *f, Expr *e){
    Ty *t=TY(e->a); int list=t->k==TY_LIST, n=list?24:20, flags=0;
    E(f,"sub esp,%d",n);
    gen_borrow(f,e->a); check_none(f,t,"TypeError","'NoneType' object is not subscriptable"); E(f,"mov [esp],eax");
    if(t->k==TY_BYTES) CALLRT(f,"rt_bytes_cp");                 /* (a byte a code point) */
    Expr *parts[3]={e->b,e->c,e->d};
    for(int k=0;k<3;k++){
        if(parts[k] && parts[k]->kind!=EXPR_NONE){ gen(f,parts[k]); to_i32(f,TY(parts[k])); E(f,"mov [esp+%d],eax",4+4*k); }
        else { flags|=1<<k; E(f,"mov dword [esp+%d],0",4+4*k); }
    }
    E(f,"mov dword [esp+16],%d",flags);
    if(list) E(f,"mov dword [esp+20],%s",kd_of(t->elem));
    CALLRT(f,list?"rt_list_slice":"rt_str_slice");
    E(f,"add esp,%d",n);
    return 1;
}

/* ---- container literals ---- */
static void list_append_top(F *f, Ty *el){         /* list at [esp], value in eax/st0 (owned) */
    push_value(f,el);
    E(f,"mov eax,[esp+%d]",esize(el)); CALLRT(f,"rt_list_push");
    E(f,"mov ecx,eax"); pop_value(f,el); store_new(f,el,"ecx",0);
}
static void set_add_top(F *f, Ty *el){             /* set at [esp], value in eax/st0 (borrowed) */
    push_value(f,el);
    E(f,"mov edx,esp"); E(f,"mov eax,[esp+%d]",esize(el)); CALLRT(f,"rt_set_add"); E(f,"add esp,%d",esize(el));
}
/* an expression CPython's optimizer folds into a constant (see set displays) */
static int folds_const(Expr *e){
    switch(e->kind){
        case EXPR_LITERAL: case EXPR_TRUE: case EXPR_FALSE: case EXPR_NONE: return 1;
        case EXPR_UNARY: return e->op!=T_NOT && folds_const(e->a);
        case EXPR_BINARY: return folds_const(e->a) && folds_const(e->b);
        case EXPR_TUPLE: for(int i=0;i<e->count;i++) if(!folds_const(e->items[i])) return 0; return 1;
        default: return 0;
    }
}
static int gen_literal_container(F *f, Expr *e){
    Ty *t=TY(e), *el=t->elem;
    if(e->kind==EXPR_DICT){
        Ty *kt=ty_dkey(t);
        new_dict(f,t); E(f,"push eax");
        for(int i=0;i<e->count;i++){
            if(e->items[i]->akind==2){ gen_borrow(f,e->items[i]); E(f,"mov edx,eax"); E(f,"mov eax,[esp]"); CALLRT(f,"rt_dict_update"); continue; }   /* **d */
            gen_as(f,e->vals[i],el,1); push_value(f,el);
            gen_as(f,e->items[i],kt,0); push_value(f,kt);
            E(f,"mov edx,esp"); E(f,"mov eax,[esp+%d]",esize(kt)+esize(el)); CALLRT(f,"rt_dict_slot"); E(f,"add esp,%d",esize(kt));
            E(f,"mov ecx,eax"); pop_value(f,el); store_mem(f,el,"ecx",0);
        }
        E(f,"pop eax"); return 1;
    }
    if(e->kind==EXPR_SET){
        int consts=e->count>=3;
        new_set(f,el); E(f,"push eax");
        for(int i=0;i<e->count;i++){
            if(e->items[i]->akind==1){ consts=0; gen_list_of(f,e->items[i],el); hold(f); E(f,"mov edx,eax"); E(f,"mov eax,[esp]"); CALLRT(f,"rt_set_add_list"); continue; }   /* *xs */
            if(!folds_const(e->items[i])) consts=0; gen_as(f,e->items[i],el,0); set_add_top(f,el); }
        if(consts){                                 /* CPython: a frozenset constant merged into a new set (its order) */
            new_set(f,el); E(f,"push eax"); E(f,"mov edx,[esp+4]"); CALLRT(f,"rt_set_merge");
            E(f,"pop eax"); E(f,"xchg eax,[esp]"); CALLRT(f,"rt_decref"); }
        E(f,"pop eax"); return 1;
    }
    new_list(f,el); E(f,"push eax");
    for(int i=0;i<e->count;i++){
        if(e->items[i]->akind==1){ gen_list_of(f,e->items[i],el); hold(f); E(f,"mov edx,eax"); E(f,"mov eax,[esp]"); CALLRT(f,"rt_list_extend"); continue; }   /* *xs */
        gen_as(f,e->items[i],el,1); list_append_top(f,el); }
    E(f,"pop eax"); return 1;
}

/* ---- calls ---- */
static void put_arg(F *f, Ty *pt, int off){ if(is_flt(pt)) E(f,"fstp qword [esp+%d]",off); else { E(f,"mov [esp+%d],eax",off); if(is_lng(pt)) E(f,"mov [esp+%d],edx",off+4); } }
static int tuple_off(Ty *t, int i);
static int in_list(int *v, int n, int x){ for(int i=0;i<n;i++) if(v[i]==x) return 1; return 0; }
/* *args of a call: a new tuple (list) of the extra positionals and f(*xs) -> eax (held) */
static void gen_star_args(F *f, AFunc *fn, Expr *e, XInfo *xi){
    Ty *el=ty_find(fn->params[fn->star]->ty)->elem;
    new_list(f,el); E(f,"push eax");
    for(int j=0;j<xi->nxargs;j++){ gen_as(f,e->items[xi->xargs[j]],el,1); list_append_top(f,el); }
    if(xi->splat){
        Expr *sa=e->items[xi->splat-1]; Ty *st=TY(sa);
        if(st->k==TY_LIST && kd_of(el)==kd_of(st->elem)){ gen_borrow(f,sa); E(f,"mov edx,eax"); E(f,"mov eax,[esp]"); CALLRT(f,"rt_list_extend"); }
        else if(st->k==TY_LIST) cg_fail(e->line,"f(*list of int) into *args of float is not supported");
        else {
            int slot=frame_slot(f,4); gen_borrow(f,sa); panic_if_null(f); E(f,"mov [ebp%+d],eax",slot);
            for(int k=0;k<st->nelems;k++){ Ty *et=st->elems[k];
                E(f,"mov eax,[ebp%+d]",slot); load_mem(f,et,"eax",tuple_off(st,k)); if(is_ptr(et)) incref(f); conv(f,et,el); list_append_top(f,el); }
        }
    }
    E(f,"pop eax"); hold(f);
}
/* **kwargs of a call: a new dict of the extra keywords and f(**d) -> eax (held) */
static void gen_kwargs(F *f, AFunc *fn, Expr *e, XInfo *xi){
    Ty *dt=ty_find(fn->params[fn->dstar]->ty), *el=dt->elem;
    new_dict(f,dt); E(f,"push eax");
    for(int j=0;j<xi->nkwargs;j++){ Expr *a=e->items[xi->kwargs[j]];
        gen_as(f,a,el,1); push_value(f,el);
        E(f,"push S%d",str_lit(a->kw)); E(f,"mov edx,esp"); E(f,"mov eax,[esp+%d]",4+esize(el)); CALLRT(f,"rt_dict_slot"); E(f,"add esp,4");
        E(f,"mov ecx,eax"); pop_value(f,el); store_mem(f,el,"ecx",0); }
    if(xi->dsplat){ gen_borrow(f,e->items[xi->dsplat-1]); E(f,"mov edx,eax"); E(f,"mov eax,[esp]"); CALLRT(f,"rt_dict_update"); }
    E(f,"pop eax"); hold(f);
}
/* The arguments of a call of fn (parameters from `first` on) into [esp+off[i]],
   in source order: positionals, keywords, f(*seq) spread, *args, **kwargs, defaults. */
static void gen_call_args(F *f, AFunc *fn, Expr *e, XInfo *xi, int *off, int first){
    int done_star=fn->star<0, done_dstar=fn->dstar<0;
    for(int ai=0;ai<e->count;ai++){
        Expr *a=e->items[ai]; int mapped=0;
        if(xi->emptysplat==ai+1){ gen_borrow(f,a); E(f,"xor edx,edx"); CALLRT(f,"rt_unpack_check"); continue; }   /* f(*xs) with nothing left to fill */
        for(int i=first;i<fn->nparams;i++) if(xi->argmap[i]==ai) mapped++;
        if(mapped && a->akind==1){                      /* f(*seq) over parameters */
            Ty *st=TY(a); int slot=frame_slot(f,4);
            gen_borrow(f,a);
            if(st->k==TY_LIST){ E(f,"mov edx,%d",mapped); CALLRT(f,"rt_unpack_check"); } else panic_if_null(f);
            E(f,"mov [ebp%+d],eax",slot);
            for(int i=first;i<fn->nparams;i++) if(xi->argmap[i]==ai){
                Ty *pt=fn->params[i]->ty; int k=xi->argelem[i]-1;
                E(f,"mov eax,[ebp%+d]",slot);
                if(st->k==TY_TUPLE){ load_mem(f,st->elems[k],"eax",tuple_off(st,k)); conv_num(f,st->elems[k],pt); }
                else { E(f,"mov eax,[eax+16]"); load_mem(f,st->elem,"eax",k*esize(st->elem)); conv_num(f,st->elem,pt); }
                if(fn->params[i]->consumed) incref(f);
                put_arg(f,pt,off[i]);
            }
            continue;
        }
        if(mapped){ for(int i=first;i<fn->nparams;i++) if(xi->argmap[i]==ai){ Ty *pt=fn->params[i]->ty; gen_as(f,a,pt,fn->params[i]->consumed); put_arg(f,pt,off[i]); } continue; }
        if(!done_star && (in_list(xi->xargs,xi->nxargs,ai) || xi->splat==ai+1)){ done_star=1; gen_star_args(f,fn,e,xi); put_arg(f,fn->params[fn->star]->ty,off[fn->star]); }
        if(!done_dstar && (in_list(xi->kwargs,xi->nkwargs,ai) || xi->dsplat==ai+1)){ done_dstar=1; gen_kwargs(f,fn,e,xi); put_arg(f,fn->params[fn->dstar]->ty,off[fn->dstar]); }
        else if(fn->dstar<0 && xi->dsplat==ai+1){          /* f(**d) without **kwargs: d must be empty */
            int l=new_label(); gen_borrow(f,a); E(f,"test eax,eax"); E(f,"jz L%d",l); E(f,"cmp dword [eax+8],0"); E(f,"je L%d",l);
            E(f,"mov esi,Z%d",zlit("got unexpected keyword arguments")); CALLRT(f,"rt_panic_type"); LBL(f,l); }
    }
    if(!done_star){ gen_star_args(f,fn,e,xi); put_arg(f,fn->params[fn->star]->ty,off[fn->star]); }
    if(!done_dstar){ gen_kwargs(f,fn,e,xi); put_arg(f,fn->params[fn->dstar]->ty,off[fn->dstar]); }
    for(int i=first;i<fn->nparams;i++) if(xi->argmap[i]<0 && fn->defaults[i] && i!=fn->star && i!=fn->dstar){
        Ty *pt=fn->params[i]->ty;
        if(const_default(fn->defaults[i])){ gen_as(f,fn->defaults[i],pt,fn->params[i]->consumed); put_arg(f,pt,off[i]); continue; }
        layout_defaults(fn);                                 /* evaluated where it was defined: in its function object */
        if(xi->kind==X_CALLNEST && fn->outer){ if(xi->var) load_var(f,xi->var); else E(f,"mov eax,[ebp%+d]",f->env); }
        else { use_fn(fn); gg->fv_used[fn->id]=1; rt("rt_static"); E(f,"mov eax,%s",fnl(fn,LB_VALUE)); }
        load_mem(f,pt,"eax",16+fn->defoff[i]);
        if(fn->params[i]->consumed && is_ptr(pt)) incref(f);
        put_arg(f,pt,off[i]);
    }
}
/* self_kind: 0 none, 1 the object of obj.method(), 2 the current method's self (super());
   env: the closure of a nested function (NULL: none, or the current one if it calls itself) */
static void call_user(F *f, AFunc *fn, Expr *e, XInfo *xi, int self_kind){
    use_fn(fn);
    int off[16], total=0;
    for(int i=0;i<fn->nparams;i++){ off[i]=total; total+=esize(fn->params[i]->ty); }
    if(total) E(f,"sub esp,%d",total);
    if(self_kind==1){ gen_borrow(f,e->a->a); E(f,"mov [esp],eax"); }
    if(self_kind==2){ E(f,"mov eax,[ebp+8]"); E(f,"mov [esp],eax"); }
    gen_call_args(f,fn,e,xi,off,self_kind?1:0);
    if(xi->kind==X_CALLNEST && (fn->ncaps || dyn_defaults(fn))){   /* its closure in edx */
        if(xi->var){ load_var(f,xi->var); panic_if_null(f); E(f,"mov edx,eax"); }
        else E(f,"mov edx,[ebp%+d]",f->env);
    }
    if(self_kind==1 && fn->cls && !fn->is_static){
        E(f,"mov eax,[esp]"); attr_check(f,fn->name);
        if(fn->overridden){ E(f,"mov eax,[eax+8]"); E(f,"call dword [eax+%d]",8+4*fn->vslot); }
        else E(f,"call %s",fnl(fn,LB_CODE));
    } else E(f,"call %s",fnl(fn,LB_CODE));
    if(total) E(f,"add esp,%d",total);
}
/* fv(args): a function value -> its result (owned if a reference). Its
   parameters: the regular ones, then *args (a tuple) and **kwargs (a dict).
   With dv: a decorated method obj.m(args) -> dv(obj, args). */
static int call_value_of(F *f, Expr *e, AVar *dv){
    Ty *ft=dv?ty_find(dv->ty):TY(e->a); int cs=frame_slot(f,4), off[17], total=0;
    int star=ft->tup&1, dstar=(ft->tup>>1)&1, nreg=ft->nelems-star-dstar, skip=dv?1:0;
    AFunc pseudo; memset(&pseudo,0,sizeof pseudo); AVar pv[16], *pp[16]; Expr *nodef[16]={NULL};
    for(int i=0;i<ft->nelems;i++){
        Ty *pt;
        if(i<nreg) pt=ft->elems[i];
        else if(star && i==nreg){ pt=ty_new(TY_LIST,ft->elems[i],NULL); pt->tup=1; }
        else pt=ty_dict(TY_STR_T,ft->elems[i]);
        off[i]=total; total+=esize(pt);
        if(i>=skip){ memset(&pv[i-skip],0,sizeof pv[0]); pv[i-skip].ty=pt; pp[i-skip]=&pv[i-skip]; }
    }
    pseudo.name="function"; pseudo.nparams=ft->nelems-skip; pseudo.params=pp; pseudo.defaults=nodef;
    pseudo.star=star?nreg-skip:-1; pseudo.dstar=dstar?nreg+star-skip:-1; pseudo.kwonly=nreg-skip;
    if(dv) load_var(f,dv); else gen_borrow(f,e->a);
    E(f,"mov [ebp%+d],eax",cs);
    if(total) E(f,"sub esp,%d",total);
    if(dv){ gen_borrow(f,e->a->a); E(f,"mov [esp],eax"); }
    gen_call_args(f,&pseudo,e,xinfo(e),off+skip,0);
    if(ft->ndef){ int doff=16;                              /* omitted ones: the defaults kept in the function object */
        for(int i=nreg-ft->ndef;i<nreg;i++){ Ty *pt=ft->elems[i];
            if(i>=skip && xinfo(e)->argmap[i-skip]<0){ E(f,"mov eax,[ebp%+d]",cs); load_mem(f,pt,"eax",doff); put_arg(f,pt,off[i]); }
            doff+=esize(pt); } }
    E(f,"mov eax,[ebp%+d]",cs);
    { int l=new_label(); E(f,"test eax,eax"); E(f,"jnz L%d",l); E(f,"mov esi,Z%d",zlit("'NoneType' object is not callable")); CALLRT(f,"rt_typeerr_text"); LBL(f,l); }
    E(f,"mov edx,eax"); E(f,"call dword [edx+8]");
    if(total) E(f,"add esp,%d",total);
    return is_ptr(ft->elem);
}
static int call_value(F *f, Expr *e){ return call_value_of(f,e,NULL); }
static int gen_str_of(F *f, Expr *x);
/* the message of an exception: str(x), owned, in eax; a KeyError keeps repr(x), which str(e) gives */
static int is_keyerror(AClass *c){ for(;c;c=c->base) if(c->builtin && !strcmp(c->name,"KeyError")) return 1; return 0; }
/* an exception's message shown as is by repr() (KeyError: the key's repr; a non-str argument: str(1) == repr(1)) */
static int raw_message(Expr *x, AClass *cls){ if(!x) return 0; if(cls && is_keyerror(cls)) return 1; return ty_find(TY(x))->k!=TY_STR; }
static int exc_rawoff(AClass *cls){ AField *fd=aot_find_field(cls,"_rawrepr"); return fd?fd->offset:0; }
static void gen_exc_message_of(F *f, Expr *x, AClass *cls){
    if(cls && is_keyerror(cls)){ rt("rt_sb_need"); E(f,"push dword [rt_sb_len]"); gen_borrow(f,x); gen_fmt(f,TY(x),1); E(f,"pop eax"); CALLRT(f,"rt_sb_take"); return; }
    int o=gen_str_of(f,x); if(!o) incref(f);
}
/* A new object (eax, kept): the slots of class attributes an object may set start as the class's
   value (the most derived class's, for one a subclass redefines). */
/* eax/st0 = the attribute of the object in eax (borrowed) */
static void load_field(F *f, AField *fd){
    if(aot_field_shared(fd)){ load_var(f,fd->cvar); return; }        /* a class attribute no object sets */
    if(fd->setoff){ int own=new_label(), done=new_label();          /* the object's own value once set */
        E(f,"cmp dword [eax+%d],0",fd->setoff); E(f,"jne L%d",own); load_var(f,fd->cvar); E(f,"jmp L%d",done);
        LBL(f,own); load_mem(f,fd->ty,"eax",fd->offset); LBL(f,done); return; }
    load_mem(f,fd->ty,"eax",fd->offset);
}
static void seed_fields(F *f, AClass *cls){
    AClass *chain[32]; int n=0; for(AClass *c=cls;c&&n<32;c=c->base) chain[n++]=c;
    for(int k=n-1;k>=0;k--) for(int i=0;i<chain[k]->nfields;i++){ AField *fd=chain[k]->fields[i], *src=NULL;
        if(fd->over || aot_field_shared(fd) || fd->setoff) continue;
        for(int j=0;j<=k && !src;j++) for(int q=0;q<chain[j]->nfields;q++){ AField *g=chain[j]->fields[q]; if(aot_field_root(g)==fd && g->cvar){ src=g; break; } }
        if(!src && !fd->init) continue;
        E(f,"push eax");
        if(src){ load_var(f,src->cvar); if(is_ptr(fd->ty)) incref(f); }
        else gen_as(f,fd->init,fd->ty,1);
        E(f,"mov ecx,[esp]");
        store_new(f,fd->ty,"ecx",fd->offset);
        E(f,"pop eax");
    }
}
static int gen_ctor(F *f, Expr *e, XInfo *xi){
    AClass *cls=xi->cls; AFunc *init=xi->fn;
    if(xi->abstract_msg){                                  /* an abstract class: TypeError (after the arguments) */
        for(int i=0;i<e->count;i++){ int o=gen(f,e->items[i]); drop_value(f,TY(e->items[i]),o); }
        E(f,"mov esi,Z%d",zlit(xi->abstract_msg)); CALLRT(f,"rt_typeerr_text"); E(f,"xor eax,eax"); return 1; }
    use_class(cls);
    if(!init && aot_is_exception(cls)){                  /* ValueError("message") */
        if(e->kind==EXPR_CALL && e->count){ gen_exc_message_of(f,e->items[0],cls); E(f,"push eax"); } else E(f,"push 0");
        E(f,"mov eax,%d",cls->size); E(f,"mov edx,%s",class_label(cls,0)); E(f,"mov ecx,%s",class_label(cls,1)); CALLRT(f,"rt_obj_new");
        E(f,"pop ecx"); E(f,"mov [eax+12],ecx");
        seed_fields(f,cls);
        if(raw_message(e->count?e->items[0]:NULL,cls)) E(f,"mov dword [eax+%d],1",exc_rawoff(cls));
        return 1;
    }
    int off[16], total=0;
    if(init){ use_fn(init); for(int i=0;i<init->nparams;i++){ off[i]=total; total+=esize(init->params[i]->ty); } }
    if(total) E(f,"sub esp,%d",total);
    if(init) gen_call_args(f,init,e,xi,off,1);
    E(f,"mov eax,%d",cls->size); E(f,"mov edx,%s",class_label(cls,0)); E(f,"mov ecx,%s",class_label(cls,1)); CALLRT(f,"rt_obj_new");
    seed_fields(f,cls);
    if(init){ E(f,"mov [esp],eax"); E(f,"call %s",fnl(init,LB_CODE)); E(f,"mov eax,[esp]"); }
    if(total) E(f,"add esp,%d",total);
    return 1;
}
static void call_method_on_top(F *f, AFunc *m){      /* object at [esp], one 4-byte argument area */
    use_fn(m);
    E(f,"mov eax,[esp]"); attr_check(f,m->name);
    if(m->overridden){ E(f,"mov eax,[eax+8]"); E(f,"call dword [eax+%d]",8+4*m->vslot); }
    else E(f,"call %s",fnl(m,LB_CODE));
}

/* ---- key= functions ---- */
/* key(element): the element (borrowed) in eax/st0 -> the key in eax/st0 (borrowed; an owned one is held) */
static void gen_key_call(F *f, Expr *key, Ty *elem){
    XInfo *ki=xinfo(key);
    if(key->kind==EXPR_LAMBDA){ store_borrowed(f,ki->var,elem); gen_borrow(f,key->a); return; }
    if(ki->kind!=X_FUNC){                                   /* a function value */
        Ty *ft=TY(key), *pt=ft->elems[0];
        conv_num(f,elem,pt); push_value(f,pt);
        gen_borrow(f,key); panic_if_null(f); E(f,"mov edx,eax"); E(f,"call dword [edx+8]"); E(f,"add esp,%d",esize(pt));
        if(is_ptr(ft->elem)) hold(f);
        return;
    }
    AFunc *fn=ki->fn; Ty *pt=fn->params[0]->ty; use_fn(fn);
    conv_num(f,elem,pt); push_value(f,pt);
    E(f,"call %s",fnl(fn,LB_CODE)); E(f,"add esp,%d",esize(pt));
    if(is_ptr(fn->ret)) hold(f);
}
static Ty *key_type(Expr *key){
    Ty *t=ty_find(xinfo(key)->ty);
    if(key->kind!=EXPR_LAMBDA && xinfo(key)->kind!=X_FUNC && t->k==TY_FUNC) return ty_find(t->elem);   /* a function value: its result */
    return t;
}
/* reverse= evaluated once -> a frame slot holding 0/1 (0 when absent) */
static int gen_rev_flag(F *f, Expr *rev){
    int slot=frame_slot(f,4);
    if(rev){ gen_bool(f,rev); E(f,"mov [ebp%+d],eax",slot); } else E(f,"mov dword [ebp%+d],0",slot);
    return slot;
}
/* Sort the list at [ebp+src] by key: (key, index) pairs sorted, then the
   elements in that order -> eax = new list (owned); in place: swapped into src. */
static int gen_sort_by_key(F *f, int src, Ty *el, Expr *key, int revslot, int inplace){
    Ty *kt=key_type(key); Ty *pe[2]={kt,TY_INT_T}; Ty *pt=ty_tuple(pe,2);
    int td=tdesc(pt), koff=12, ioff=12+esize(kt);
    int pairs=ref_slot(f), idx=frame_slot(f,4), res=ref_slot(f);
    int ltop=new_label(), lend=new_label(), l2=new_label(), l2end=new_label();
    new_list(f,pt); E(f,"mov [ebp%+d],eax",pairs);
    E(f,"mov dword [ebp%+d],0",idx);
    LBL(f,ltop);
    E(f,"mov eax,[ebp%+d]",src); E(f,"test eax,eax"); E(f,"jz L%d",lend);
    E(f,"mov ecx,[ebp%+d]",idx); E(f,"cmp ecx,[eax+8]"); E(f,"jae L%d",lend);
    int mark=scope_open(f);
    E(f,"mov eax,[eax+16]"); load_at(f,el,"eax","ecx",esize(el));
    gen_key_call(f,key,el); push_value(f,kt);
    E(f,"mov eax,TD%d",td); E(f,"mov edx,%d",tuple_size(pt)); CALLRT(f,"rt_tuple_new");
    E(f,"mov ecx,eax"); pop_value(f,kt);
    if(is_ptr(kt)){ E(f,"push ecx"); incref(f); E(f,"pop ecx"); }
    store_new(f,kt,"ecx",koff);
    { int l=new_label(); E(f,"mov eax,[ebp%+d]",idx); E(f,"cmp dword [ebp%+d],0",revslot); E(f,"je L%d",l); E(f,"neg eax"); LBL(f,l); }  /* reverse keeps equal keys in order */
    E(f,"cdq"); E(f,"mov [ecx+%d],eax",ioff); E(f,"mov [ecx+%d],edx",ioff+4);
    E(f,"push ecx"); E(f,"mov eax,[ebp%+d]",pairs); CALLRT(f,"rt_list_push"); E(f,"pop ecx"); E(f,"mov [eax],ecx");
    scope_close(f,mark);
    E(f,"inc dword [ebp%+d]",idx); E(f,"jmp L%d",ltop);
    LBL(f,lend);
    E(f,"mov eax,[ebp%+d]",pairs); CALLRT(f,"rt_list_sort");
    { int l=new_label(); E(f,"cmp dword [ebp%+d],0",revslot); E(f,"je L%d",l); E(f,"mov eax,[ebp%+d]",pairs); CALLRT(f,"rt_list_reverse"); LBL(f,l); }
    new_list(f,el); E(f,"mov [ebp%+d],eax",res);
    E(f,"mov dword [ebp%+d],0",idx);
    LBL(f,l2);
    E(f,"mov eax,[ebp%+d]",pairs); E(f,"mov ecx,[ebp%+d]",idx); E(f,"cmp ecx,[eax+8]"); E(f,"jae L%d",l2end);
    E(f,"mov eax,[eax+16]"); E(f,"mov eax,[eax+ecx*4]"); E(f,"mov ecx,[eax+%d]",ioff);
    { int l=new_label(); E(f,"test ecx,ecx"); E(f,"jns L%d",l); E(f,"neg ecx"); LBL(f,l); }
    E(f,"mov eax,[ebp%+d]",src); E(f,"mov eax,[eax+16]");
    load_at(f,el,"eax","ecx",esize(el)); if(is_ptr(el)) incref(f);
    E(f,"push dword [ebp%+d]",res);
    list_append_top(f,el);
    E(f,"add esp,4");
    E(f,"inc dword [ebp%+d]",idx); E(f,"jmp L%d",l2);
    LBL(f,l2end);
    E(f,"mov eax,[ebp%+d]",pairs); E(f,"mov dword [ebp%+d],0",pairs); CALLRT(f,"rt_decref");
    if(inplace){                                         /* the new order into the list itself */
        E(f,"mov eax,[ebp%+d]",src); E(f,"mov ecx,[ebp%+d]",res);
        E(f,"test eax,eax"); { int l=new_label(); E(f,"jz L%d",l);
        for(int k=8;k<=16;k+=4){ E(f,"mov edx,[eax+%d]",k); E(f,"xchg edx,[ecx+%d]",k); E(f,"mov [eax+%d],edx",k); }
        LBL(f,l); }
        E(f,"mov eax,[ebp%+d]",res); E(f,"mov dword [ebp%+d],0",res); CALLRT(f,"rt_decref");
        return 0;
    }
    E(f,"mov eax,[ebp%+d]",src); E(f,"mov dword [ebp%+d],0",src); CALLRT(f,"rt_decref");   /* the copy sorted */
    E(f,"mov eax,[ebp%+d]",res); E(f,"mov dword [ebp%+d],0",res);
    return 1;
}
/* min/max(xs, key=f): the first element with the smallest/largest key -> eax/st0 (owned) */
static int gen_minmax_key(F *f, Expr *e, int is_max){
    Expr *key=xinfo(e)->key; Ty *lt=TY(e->items[0]), *el=lt->k==TY_DICT?ty_dkey(lt):lt->elem, *kt=key_type(key);
    int src=ref_slot(f), idx=frame_slot(f,4), best=frame_slot(f,4), bkey=is_ptr(kt)?ref_slot(f):frame_slot(f,8);
    int ltop=new_label(), lend=new_label(), ltake=new_label(), lnext=new_label();
    if(lt->k==TY_LIST) gen_owned(f,e->items[0]); else gen_list_of(f,e->items[0],el);
    E(f,"mov [ebp%+d],eax",src);
    { int l=new_label(), lok=new_label(); E(f,"test eax,eax"); E(f,"jz L%d",l); E(f,"cmp dword [eax+8],0"); E(f,"jne L%d",lok); LBL(f,l);
      E(f,"mov esi,Z%d",zlit(is_max?"max() iterable argument is empty":"min() iterable argument is empty")); CALLRT(f,"rt_panic_value"); LBL(f,lok); }
    E(f,"mov dword [ebp%+d],0",idx); E(f,"mov dword [ebp%+d],-1",best);
    LBL(f,ltop);
    E(f,"mov eax,[ebp%+d]",src); E(f,"mov ecx,[ebp%+d]",idx); E(f,"cmp ecx,[eax+8]"); E(f,"jae L%d",lend);
    int mark=scope_open(f);
    E(f,"mov eax,[eax+16]"); load_at(f,el,"eax","ecx",esize(el));
    gen_key_call(f,key,el);
    E(f,"cmp dword [ebp%+d],-1",best); E(f,"je L%d",ltake);
    if(is_flt(kt)){ E(f,"fld qword [ebp%+d]",bkey); E(f,"fcomp st1"); E(f,"fnstsw ax"); E(f,"sahf"); E(f,"j%s L%d",is_max?"b":"a",ltake); E(f,"fstp st0"); E(f,"jmp L%d",lnext); }
    else {                                                  /* the key type's ordering: cmp(key, best) */
        push_value(f,kt); E(f,"mov eax,esp"); E(f,"lea edx,[ebp%+d]",bkey); E(f,"mov ecx,%s",kd_of(kt)); E(f,"call dword [ecx+12]");
        E(f,"mov ecx,eax"); pop_value(f,kt); E(f,"cmp ecx,0");
        E(f,"j%s L%d",is_max?"g":"l",ltake); E(f,"jmp L%d",lnext);
    }
    LBL(f,ltake);
    if(is_ptr(kt)){ incref(f); E(f,"xchg eax,[ebp%+d]",bkey); CALLRT(f,"rt_decref"); }
    else store_new(f,kt,"ebp",bkey);
    E(f,"mov eax,[ebp%+d]",idx); E(f,"mov [ebp%+d],eax",best);
    LBL(f,lnext);
    scope_close(f,mark);
    E(f,"inc dword [ebp%+d]",idx); E(f,"jmp L%d",ltop);
    LBL(f,lend);
    E(f,"mov eax,[ebp%+d]",src); E(f,"mov ecx,[ebp%+d]",best); E(f,"mov eax,[eax+16]");
    load_at(f,el,"eax","ecx",esize(el)); if(is_ptr(el)) incref(f);
    if(!is_flt(el)) push_value(f,el);                                         /* the sequence is released */
    E(f,"mov eax,[ebp%+d]",src); E(f,"mov dword [ebp%+d],0",src); CALLRT(f,"rt_decref");
    if(!is_flt(el)) pop_value(f,el);
    return is_ptr(el);
}

/* any/all of a generator expression: its loops, stopping at the first deciding item */
static int gen_comp_anyall(F *f, Expr *e, int isall){
    int res=frame_slot(f,4), lfound=new_label(), ldone=new_label();
    E(f,"mov dword [ebp%+d],%d",res,isall);
    Iter its[8]; int n=e->nclause;
    if(n>8) cg_fail(e->line,"too many for clauses");
    for(int i=0;i<n;i++){
        CompClause *cl=&e->clauses[i];
        iter_begin(f,cl->iter,&its[i]);
        iter_store_vars(f,&its[i],&xinfo(e)->cvars[2*i],cl->nvars); comp_unpack(f,e,i);
        for(int k=0;k<cl->ncond;k++) gen_jump(f,cl->conds[k],its[i].cont,0);
    }
    gen_jump(f,e->a,lfound,!isall);
    for(int i=n-1;i>=0;i--){ iter_end(f,&its[i]); LBL(f,its[i].exit); iter_release(f,&its[i]); }
    E(f,"jmp L%d",ldone);
    LBL(f,lfound);
    E(f,"mov dword [ebp%+d],%d",res,!isall);
    for(int i=0;i<n;i++) iter_release(f,&its[i]);
    LBL(f,ldone);
    E(f,"mov eax,[ebp%+d]",res);
    return 0;
}
/* functools.reduce(f, xs[, init]) */
static int gen_reduce(F *f, Expr *e, XInfo *xi){
    Ty *lt=TY(e->items[1]), *el=lt->elem; AVar *acc=xi->cvars[0], *x=xi->cvars[1]; Ty *at=acc->ty;
    int src=ref_slot(f), idx=frame_slot(f,4), ltop=new_label(), lend=new_label();
    gen_owned(f,e->items[1]); E(f,"mov [ebp%+d],eax",src);
    if(e->count==3){ gen_as(f,e->items[2],at,1); store_var(f,acc); E(f,"mov dword [ebp%+d],0",idx); }
    else {
        int lok=new_label(), lempty=new_label();
        E(f,"mov eax,[ebp%+d]",src); E(f,"test eax,eax"); E(f,"jz L%d",lempty); E(f,"cmp dword [eax+8],0"); E(f,"jne L%d",lok);
        LBL(f,lempty); E(f,"mov esi,Z%d",zlit("reduce() of empty iterable with no initial value")); CALLRT(f,"rt_panic_type");
        LBL(f,lok); E(f,"mov eax,[eax+16]"); load_mem(f,el,"eax",0); store_borrowed(f,acc,el); E(f,"mov dword [ebp%+d],1",idx);
    }
    LBL(f,ltop);
    E(f,"mov eax,[ebp%+d]",src); E(f,"mov ecx,[ebp%+d]",idx); E(f,"cmp ecx,[eax+8]"); E(f,"jae L%d",lend);
    E(f,"mov eax,[eax+16]"); load_at(f,el,"eax","ecx",esize(el));
    store_borrowed(f,x,el);
    { int mark=scope_open(f); gen_as(f,xi->key,at,1); store_var(f,acc); scope_close(f,mark); }
    E(f,"inc dword [ebp%+d]",idx); E(f,"jmp L%d",ltop);
    LBL(f,lend);
    E(f,"mov eax,[ebp%+d]",src); E(f,"mov dword [ebp%+d],0",src); CALLRT(f,"rt_decref");
    load_var(f,acc); if(is_ptr(at)) incref(f);
    return is_ptr(at);
}
/* list(range(...)) of the range call r -> eax (owned) */
static void gen_range_list(F *f, Expr *r){
    E(f,"sub esp,24");                                   /* start, stop, step: 8 bytes each */
    Expr *lo=r->count>=2?r->items[0]:NULL, *hi=r->count>=2?r->items[1]:r->items[0], *st=r->count==3?r->items[2]:NULL;
    if(lo){ gen_as(f,lo,TY_INT_T,0); put_arg(f,TY_INT_T,0); } else { E(f,"mov dword [esp],0"); E(f,"mov dword [esp+4],0"); }
    gen_as(f,hi,TY_INT_T,0); put_arg(f,TY_INT_T,8);
    if(st){ gen_as(f,st,TY_INT_T,0); put_arg(f,TY_INT_T,16); } else { E(f,"mov dword [esp+16],1"); E(f,"mov dword [esp+20],0"); }
    CALLRT(f,"rt_range_list"); E(f,"add esp,24");
}
/* eax = a new list of x's items: a list copied, a dict's keys, a str's
   characters, a set's elements (in its order), what a generator yields */
static void gen_list_of(F *f, Expr *x, Ty *elem){
    Ty *t=TY(x);
    gen_borrow(f,x);
    switch(t->k){
        case TY_DICT: CALLRT(f,"rt_dict_keys"); break;
        case TY_STR: CALLRT(f,"rt_str_chars"); break;
        case TY_BYTES: CALLRT(f,"rt_bytes_list"); break;
        case TY_SET: E(f,"mov edx,%s",kd_of(t->elem)); CALLRT(f,"rt_set_to_list"); break;
        case TY_GEN: E(f,"mov edx,%s",kd_of(elem)); CALLRT(f,"rt_gen_drain"); break;
        case TY_TUPLE:{                                     /* a tuple's items, one by one */
            E(f,"push eax"); new_list(f,elem); E(f,"push eax");
            for(int i=0;i<t->nelems;i++){ Ty *et=t->elems[i];
                E(f,"mov eax,[esp+4]"); load_mem(f,et,"eax",tuple_off(t,i)); if(is_ptr(et)) incref(f);
                conv_num(f,et,elem); list_append_top(f,elem); }
            E(f,"pop eax"); E(f,"add esp,4"); break; }
        default: E(f,"mov edx,%s",kd_of(t->elem)); CALLRT(f,"rt_list_copy"); break;
    }
}
/* type(x) -> eax: the class of an object, else the descriptor of x's built-in type */
static void gen_type_of(F *f, Expr *x){
    Ty *t=TY(x);
    if(is_none_lit(x)){ E(f,"mov eax,%s",btype_label("NoneType")); return; }
    if(t->k==TY_OBJ){
        gen_borrow(f,x);
        if(is_opt(t)){ int l=new_label(), l2=new_label(); E(f,"test eax,eax"); E(f,"jnz L%d",l); E(f,"mov eax,%s",btype_label("NoneType")); E(f,"jmp L%d",l2); LBL(f,l); E(f,"mov eax,[eax+8]"); LBL(f,l2); }
        else E(f,"mov eax,[eax+8]");
        return; }
    int o=gen(f,x);
    Ty plain=*ty_find(t); plain.opt=0;
    const char *lbl=btype_label(py_type_name(&plain));
    if(is_opt(t)){ int lnone=new_label(), lend=new_label();
        jump_if_none(f,t,lnone); drop_value(f,t,o); E(f,"mov eax,%s",lbl); E(f,"jmp L%d",lend);
        LBL(f,lnone); drop_value(f,t,o); E(f,"mov eax,%s",btype_label("NoneType")); LBL(f,lend); return; }
    drop_value(f,t,o); E(f,"mov eax,%s",lbl);
}
/* the generator in [ebp+gslot] finished: StopIteration, its return value as .value (and the message) */
static void gen_raise_stop(F *f, int gslot, Ty *gt){
    if(ty_find(gt)->tup){                                  /* an async generator: StopAsyncIteration */
        AClass *sa=NULL; for(int i=0;i<gg->p->nclasses;i++) if(gg->p->classes[i]->builtin && !strcmp(gg->p->classes[i]->name,"StopAsyncIteration")) sa=gg->p->classes[i];
        use_class(sa); E(f,"push 0");
        E(f,"mov eax,%d",sa->size); E(f,"mov edx,%s",class_label(sa,0)); E(f,"mov ecx,%s",class_label(sa,1)); CALLRT(f,"rt_obj_new");
        E(f,"pop ecx"); E(f,"mov [eax+12],ecx"); CALLRT(f,"rt_throw"); return; }
    AClass *si=NULL; for(int i=0;i<gg->p->nclasses;i++) if(gg->p->classes[i]->builtin && !strcmp(gg->p->classes[i]->name,"StopIteration")) si=gg->p->classes[i];
    use_class(si);
    AField *vf=aot_find_field(si,"value"); Ty *vt=ty_find(vf->ty), *gr=gpart(gt,1);
    int lnone=new_label(), lgo=new_label();
    if(gr->k!=TY_VOID){
        E(f,"mov ecx,[ebp%+d]",gslot); E(f,"cmp dword [ecx+108],0"); E(f,"je L%d",lnone);
        rt("rt_sb_need"); E(f,"push dword [rt_sb_len]"); E(f,"mov ecx,[ebp%+d]",gslot); load_mem(f,gr,"ecx",100); gen_fmt(f,gr,0); E(f,"pop eax"); CALLRT(f,"rt_sb_take");
        E(f,"jmp L%d",lgo); }
    LBL(f,lnone); E(f,"xor eax,eax");
    LBL(f,lgo); E(f,"push eax");
    E(f,"mov eax,%d",si->size); E(f,"mov edx,%s",class_label(si,0)); E(f,"mov ecx,%s",class_label(si,1)); CALLRT(f,"rt_obj_new");
    E(f,"pop ecx"); E(f,"mov [eax+12],ecx"); E(f,"push eax");
    int l2=new_label(), l3=new_label();
    if(gr->k!=TY_VOID){ E(f,"mov ecx,[ebp%+d]",gslot); E(f,"cmp dword [ecx+108],0"); E(f,"je L%d",l2);
        load_mem(f,gr,"ecx",100); if(is_ptr(gr)) incref(f); conv(f,gr,vt); E(f,"jmp L%d",l3); }
    LBL(f,l2); gen_none(f,vt);
    LBL(f,l3); E(f,"mov ecx,[esp]"); store_new(f,vt,"ecx",vf->offset); E(f,"pop eax");
    CALLRT(f,"rt_throw");
}
static int gen_builtin_in(F *f, Expr *e, XInfo *xi);
static void gen_task_new(F *f, Expr *call);
static int gen_builtin(F *f, Expr *e, XInfo *xi){
    const char *save=g_none_text; int o=gen_builtin_in(f,e,xi); g_none_text=save; return o;
}
static int gen_builtin_in(F *f, Expr *e, XInfo *xi){
    const char *n=xi->name; Ty *t=TY(e);
    Expr *a0=e->count?e->items[0]:NULL; Ty *t0=a0?TY(a0):NULL;
    if(!strcmp(n,"reduce")) return gen_reduce(f,e,xi);
    if(!strcmp(n,"downcast")) return gen(f,e->items[1]);                  /* (the same object, known to be of a subclass) */
    if(!strcmp(n,"aiter")) return gen(f,a0);
    if(!strncmp(n,"thread.",7)){ const char *m=n+7;                       /* threads: tasks */
        if(!strcmp(m,"start")){ gen_task_new(f,xi->key); rt("rt_time_sleep"); rt("rt_thread_sleep");
            E(f,"mov dword [rt_sleep_hook],rt_thread_sleep"); return 1; }          /* (time.sleep() lets the threads run from now on) */      /* (at the end the program does not wait for them, as the interpreter) */
        if(!strcmp(m,"join")){ gen_borrow(f,a0); CALLRT(f,"rt_thread_join"); return 0; }
        if(!strcmp(m,"lock")){ CALLRT(f,"rt_thread_lock"); return 0; }
        if(!strcmp(m,"acquire")||!strcmp(m,"release")){ gen(f,a0); to_i32(f,TY(a0)); CALLRT(f,m[0]=='a'?"rt_thread_acquire":"rt_thread_release"); return 0; }
        if(!strcmp(m,"sleep")){ gen_as(f,a0,TY_FLOAT_T,0); CALLRT(f,"rt_thread_sleep"); return 0; }
        if(!strcmp(m,"current")){ CALLRT(f,"rt_thread_current"); return 0; } }
    if(!strcmp(n,"next")||!strcmp(n,"anext")){
        Ty *el=t; int slot=frame_slot(f,4), lmiss=new_label(), lend=new_label();
        gen_borrow(f,a0); panic_if_null(f); E(f,"mov [ebp%+d],eax",slot);
        CALLRT(f,"rt_gen_next"); E(f,"test eax,eax"); E(f,"jz L%d",lmiss);
        E(f,"mov eax,[ebp%+d]",slot); load_mem(f,el,"eax",24); if(is_ptr(el)) incref(f);
        E(f,"jmp L%d",lend);
        LBL(f,lmiss);
        if(e->count==2){ gen_as(f,e->items[1],el,1); }
        else gen_raise_stop(f,slot,TY(a0));
        LBL(f,lend);
        return is_ptr(el);
    }
    if((!strcmp(n,"any")||!strcmp(n,"all")) && a0->kind==EXPR_COMPREHENSION && a0->comp_kind=='g') return gen_comp_anyall(f,a0,n[1]=='l');
    if(!strcmp(n,"len") && t0->k==TY_TUPLE){ int o=gen(f,a0); drop_value(f,t0,o); E(f,"mov eax,%d",t0->nelems); widen(f,0); return 0; }
    if(!strcmp(n,"divmod")){
        Ty *qt=t->elems[0];
        E(f,"mov eax,TD%d",tdesc(t)); E(f,"mov edx,%d",tuple_size(t)); CALLRT(f,"rt_tuple_new"); E(f,"push eax");
        gen_as(f,a0,qt,0); push_value(f,qt); gen_as(f,e->items[1],qt,0); push_value(f,qt);   /* [esp] b, then a, then the tuple */
        int sz=esize(qt);
        if(is_flt(qt)){
            E(f,"fld qword [esp]"); E(f,"fld qword [esp+8]"); CALLRT(f,"rt_ffloordiv"); E(f,"mov ecx,[esp+16]"); E(f,"fstp qword [ecx+12]");
            E(f,"fld qword [esp+8]"); E(f,"fld qword [esp]"); E(f,"fxch"); CALLRT(f,"rt_fmod"); E(f,"mov ecx,[esp+16]"); E(f,"fstp qword [ecx+20]");
        } else {                                            /* [esp] b, [esp+8] a, [esp+16] the tuple */
            E(f,"push dword [esp+12]"); E(f,"push dword [esp+12]"); E(f,"mov eax,[esp+8]"); E(f,"mov edx,[esp+12]");
            CALLRT(f,"rt_idivmod"); E(f,"add esp,8");
            E(f,"mov ecx,[esp+16]"); E(f,"mov [ecx+12],eax"); E(f,"mov [ecx+16],edx");
            E(f,"mov eax,[rt_scratch]"); E(f,"mov [ecx+20],eax"); E(f,"mov eax,[rt_scratch+4]"); E(f,"mov [ecx+24],eax");
        }
        E(f,"add esp,%d",2*sz); E(f,"pop eax");
        return 1;
    }
    if(!strcmp(n,"len")){
        if(xi->fn){ E(f,"sub esp,4"); gen_borrow(f,a0); E(f,"mov [esp],eax"); call_method_on_top(f,xi->fn); E(f,"add esp,4"); return 0; }
        int l=new_label(); gen_borrow(f,a0); check_none(f,t0,"TypeError","object of type 'NoneType' has no len()");
        if(t0->k==TY_STR){ CALLRT(f,"rt_str_cplen"); widen(f,0); return 0; }
        E(f,"test eax,eax"); E(f,"jz L%d",l); E(f,"mov eax,[eax+8]"); LBL(f,l); widen(f,0); return 0;
    }
    if(!strcmp(n,"str_decode")){ gen_codec_call(f,a0,e->items[1],e->count>2?e->items[2]:NULL,NULL,"rt_bytes_decode"); return 1; }
    if(!strcmp(n,"bytes_empty")){ E(f,"xor eax,eax"); CALLRT(f,"rt_bytes_new"); return 1; }
    if(!strcmp(n,"bytes_encode")){ gen_codec_call(f,a0,e->items[1],e->count>2?e->items[2]:NULL,NULL,"rt_str_encode"); return 1; }
    if(!strcmp(n,"bytes_zeros")){ gen(f,a0); to_i32(f,t0); CALLRT(f,"rt_bytes_new"); return 1; }
    if(!strcmp(n,"bytes_same")) return gen(f,a0);
    if(!strcmp(n,"bytes_list")){ gen_borrow(f,a0);
        if(t0->k==TY_SET){ E(f,"mov edx,%s",kd_of(t0->elem)); CALLRT(f,"rt_set_to_list"); hold(f); }
        CALLRT(f,"rt_bytes_from_list"); return 1; }
    if(!strcmp(n,"bytes.fromhex")){ gen_borrow(f,a0); CALLRT(f,"rt_bytes_fromhex"); return 1; }
    if(!strcmp(n,"int.from_bytes")){
        gen_borrow(f,a0); E(f,"push eax");
        gen_byteorder(f,e->count>1?e->items[1]:NULL); E(f,"push edx");
        if(e->count>2) gen(f,e->items[2]); else E(f,"xor eax,eax");
        E(f,"mov ecx,eax"); E(f,"pop edx"); E(f,"pop eax"); CALLRT(f,"rt_int_from_bytes"); return 0; }
    if(!strcmp(n,"str")||!strcmp(n,"repr")){
        if(!a0){ E(f,"xor eax,eax"); return 0; }
        if(t0->k==TY_STR && !t0->opt && n[0]=='s') return gen(f,a0);
        rt("rt_sb_need"); E(f,"push dword [rt_sb_len]");
        gen_borrow(f,a0); gen_fmt(f,t0,n[0]=='r');
        E(f,"pop eax"); CALLRT(f,"rt_sb_take"); return 1;
    }
    if(!strcmp(n,"int")||!strcmp(n,"float")) g_none_text=n[0]=='i'?"int() argument must be a string, a bytes-like object or a real number, not 'NoneType'":"float() argument must be a string or a real number, not 'NoneType'";
    if(!strcmp(n,"abs")) g_none_text="bad operand type for abs(): 'NoneType'";
    if(!strcmp(n,"round")) g_none_text="type NoneType doesn't define __round__ method";
    if(!strcmp(n,"int")){
        if(!a0){ gen_int_const(f,TY_INT_T,0); return 0; }
        if(!g_none_text) g_none_text="int() argument must be a string, a bytes-like object or a real number, not 'NoneType'";
        if(t0->k==TY_FLOAT){ gen(f,a0); CALLRT(f,"rt_ftoi"); return 0; }
        if(e->count==2 || t0->k==TY_BYTES){                                       /* int(s, base), int(b"12") */
            if(e->count==2){ gen_as(f,e->items[1],TY_INT_T,0); to_i32(f,TY_INT_T); E(f,"push eax"); } else E(f,"push 10");
            gen_borrow(f,a0); check_none(f,t0,"TypeError",g_none_text);
            E(f,"pop ecx"); E(f,"mov edx,%d",t0->k==TY_BYTES); CALLRT(f,"rt_int_parse_base"); return 0; }
        if(t0->k==TY_STR){ gen_borrow(f,a0); check_none(f,t0,"TypeError",g_none_text); CALLRT(f,"rt_int_parse"); return 0; }
        gen_as(f,a0,TY_INT_T,0); return 0;
    }
    if(!strcmp(n,"float")){
        if(!a0){ E(f,"fldz"); return 0; }
        if(t0->k==TY_STR){ gen_borrow(f,a0); check_none(f,t0,"TypeError",g_none_text); CALLRT(f,"rt_float_parse"); return 0; }
        gen_as(f,a0,TY_FLOAT_T,0); return 0;
    }
    if(!strcmp(n,"bool")){ if(!a0) E(f,"xor eax,eax"); else gen_bool(f,a0); return 0; }
    if(!strcmp(n,"abs")){ if(t0->k==TY_FLOAT){ gen(f,a0); E(f,"fabs"); } else { gen_as(f,a0,TY_INT_T,0); CALLRT(f,"rt_iabs"); } return 0; }
    if((!strcmp(n,"min")||!strcmp(n,"max")) && xi->key) return gen_minmax_key(f,e,n[1]=='a');
    if(!strcmp(n,"min")||!strcmp(n,"max")){
        int mx=n[1]=='a';
        if(e->count==1){                                   /* of a container: the address of the element */
            gen_borrow(f,a0);
            if(t0->k==TY_DICT){ CALLRT(f,"rt_dict_keys"); hold(f); }
            else if(t0->k==TY_SET){ E(f,"mov edx,%s",kd_of(t0->elem)); CALLRT(f,"rt_set_to_list"); hold(f); }
            else if(t0->k==TY_STR){ CALLRT(f,"rt_str_chars"); hold(f); }
            E(f,"mov edx,%d",mx); CALLRT(f,"rt_list_minmax"); load_mem(f,t,"eax",0); return 0; }
        int slot=frame_slot(f,8);
        gen_as(f,a0,t,0); store_new(f,t,"ebp",slot);
        for(int i=1;i<e->count;i++){
            int skip=new_label();
            gen_as(f,e->items[i],t,0);
            if(is_flt(t)){
                int take=new_label();
                E(f,"fld qword [ebp%+d]",slot); E(f,"fcomp st1"); E(f,"fnstsw ax"); E(f,"sahf");   /* best vs x */
                E(f,"j%s L%d",mx?"b":"a",take); E(f,"fstp st0"); E(f,"jmp L%d",skip);
                LBL(f,take); E(f,"fstp qword [ebp%+d]",slot);
            } else if(is_lng(t)){                               /* the first of equals stays */
                if(mx){ E(f,"mov ecx,[ebp%+d]",slot); E(f,"sub ecx,eax"); E(f,"mov ecx,[ebp%+d]",slot+4); E(f,"sbb ecx,edx"); E(f,"jge L%d",skip); }
                else { E(f,"cmp eax,[ebp%+d]",slot); E(f,"mov ecx,edx"); E(f,"sbb ecx,[ebp%+d]",slot+4); E(f,"jge L%d",skip); }
                store_new(f,t,"ebp",slot);
            } else if(ty_find(t)->k==TY_BOOL){ E(f,"cmp eax,[ebp%+d]",slot); E(f,"j%s L%d",mx?"le":"ge",skip); E(f,"mov [ebp%+d],eax",slot); }
            else {                                              /* others: the type's ordering */
                E(f,"push eax"); E(f,"mov eax,esp"); E(f,"lea edx,[ebp%+d]",slot); E(f,"mov ecx,%s",kd_of(t)); E(f,"call dword [ecx+12]");
                E(f,"pop ecx"); E(f,"cmp eax,0"); E(f,"j%s L%d",mx?"le":"ge",skip); E(f,"mov [ebp%+d],ecx",slot);
            }
            LBL(f,skip);
        }
        load_mem(f,t,"ebp",slot);
        return 0;
    }
    if(!strcmp(n,"ord")){ gen_borrow(f,a0); CALLRT(f,"rt_ord"); widen(f,0); return 0; }
    if(!strcmp(n,"id")){                                    /* an object's address (an int, a bool: its value) */
        if(is_ptr(t0)){ gen_borrow(f,a0); widen(f,0); return 0; }
        gen_borrow(f,a0); if(esize(t0)<8) widen(f,1); return 0; }
    if(!strcmp(n,"hash")){                                  /* the type's hash (the interpreter's numbers) */
        gen_borrow(f,a0); push_value(f,t0); E(f,"mov eax,esp"); E(f,"mov ecx,%s",kd_of(t0)); E(f,"call dword [ecx+16]"); E(f,"add esp,%d",esize(t0)); return 0; }
    if(!strcmp(n,"open")){
        gen_borrow(f,a0); E(f,"push eax");
        if(e->count==2) gen_borrow(f,e->items[1]); else E(f,"xor eax,eax");
        E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_file_open"); return 1;
    }
    if(!strcmp(n,"any")||!strcmp(n,"all")){
        int isall=n[1]=='l', res=frame_slot(f,4), ldone=new_label();
        E(f,"mov dword [ebp%+d],%d",res,isall);
        Iter I; iter_begin(f,a0,&I);
        iter_value(f,&I,0); truth(f,I.elem);
        E(f,"test eax,eax"); E(f,"j%s L%d",isall?"nz":"z",I.cont);
        E(f,"mov dword [ebp%+d],%d",res,!isall); E(f,"jmp L%d",ldone);
        iter_end(f,&I); LBL(f,I.exit); LBL(f,ldone); iter_release(f,&I);
        E(f,"mov eax,[ebp%+d]",res); return 0;
    }
    if(!strcmp(n,"reversed")){
        if(t0->k==TY_TUPLE) cg_fail(e->line,"reversed() of a tuple: use a list");
        gen_list_of(f,a0,t->elem);
        E(f,"push eax"); CALLRT(f,"rt_list_reverse"); E(f,"pop eax");
        return 1;
    }
    if(!strcmp(n,"hex")||!strcmp(n,"bin")||!strcmp(n,"oct")||!strcmp(n,"format")){
        Spec sp; memset(&sp,0,sizeof sp); sp.prec=-1;
        if(n[0]=='f'){ if(e->count==2){ const char *ss=e->items[1]->tok->text; if(parse_spec(ss,(int)strlen(ss),&sp)!=(int)strlen(ss)) cg_fail(e->line,"invalid format spec"); } }
        else { sp.alt=1; sp.type=n[0]=='h'?'x':n[0]=='b'?'b':'o'; }
        rt("rt_sb_need"); E(f,"push dword [rt_sb_len]");
        ArgSrc src={a0,NULL,0,0}; emit_field(f,&sp,t0,&src,0,1,e->line);
        E(f,"pop eax"); CALLRT(f,"rt_sb_take"); return 1;
    }
    if(!strcmp(n,"pow")){
        if(e->count==3){                                    /* [esp] base, [esp+8] exp, the modulus in edx:eax */
            gen_as(f,e->items[1],TY_INT_T,0); push_value(f,TY_INT_T); gen_as(f,a0,TY_INT_T,0); push_value(f,TY_INT_T);
            gen_as(f,e->items[2],TY_INT_T,0); CALLRT(f,"rt_ipowmod"); E(f,"add esp,16"); return 0; }
        int num=1; (void)num;
        gen_as(f,a0,t,0); push_value(f,t); gen_as(f,e->items[1],t,0);
        return apply_binop(f,T_POWER,t,t,TY(e->items[1]),e->line);
    }
    if(!strcmp(n,"chr")){ gen(f,a0); to_i32(f,t0); CALLRT(f,"rt_chr"); return 0; }
    if(!strcmp(n,"sum")){
        gen_borrow(f,a0); if(t0->k==TY_SET){ E(f,"mov edx,%s",kd_of(t0->elem)); CALLRT(f,"rt_set_to_list"); hold(f); }
        if(is_lng(t0->elem)) CALLRT(f,"rt_list_isum");
        else { CALLRT(f,"rt_list_sum"); if(!is_flt(t0->elem)) widen(f,0); }     /* floats; bools counted */
        return 0; }
    if(!strcmp(n,"sorted") && (xi->key||xi->rev)){
        int rev=gen_rev_flag(f,xi->rev), src=ref_slot(f);
        if(xi->key && t0->k==TY_LIST){ gen_borrow(f,a0); incref(f); }     /* sorting by key makes a new list anyway */
        else gen_list_of(f,a0,t->elem);
        E(f,"mov [ebp%+d],eax",src);
        if(xi->key) return gen_sort_by_key(f,src,t->elem,xi->key,rev,0);
        CALLRT(f,"rt_list_sort");
        { int l=new_label(); E(f,"cmp dword [ebp%+d],0",rev); E(f,"je L%d",l); E(f,"mov eax,[ebp%+d]",src); CALLRT(f,"rt_list_reverse"); LBL(f,l); }
        E(f,"mov eax,[ebp%+d]",src); E(f,"mov dword [ebp%+d],0",src); return 1;
    }
    if(!strcmp(n,"sorted")){
        gen_list_of(f,a0,t->elem);
        E(f,"push eax"); CALLRT(f,"rt_list_sort"); E(f,"pop eax");
        return 1;
    }
    if(!strcmp(n,"type")){ gen_type_of(f,a0); return 0; }
    if(!strcmp(n,"issubclass")){
        Expr *k=e->items[1]; int nk=k->kind==EXPR_TUPLE?k->count:1;
        gen(f,a0); E(f,"push eax"); E(f,"push 0");
        for(int i=0;i<nk;i++){ XInfo *ki=xinfo(k->kind==EXPR_TUPLE?k->items[i]:k);
            if(ki->kind!=X_TYPEVAL){                         /* a class value: when running */
                gen(f,k); E(f,"mov edx,eax"); E(f,"mov eax,[esp+4]"); CALLRT(f,"rt_issubclass"); E(f,"or [esp],eax"); continue; }
            if(!ki->cls && !strcmp(ki->name,"object")){ E(f,"mov dword [esp],1"); continue; }
            E(f,"mov eax,[esp+4]"); E(f,"mov edx,%s",typeval_label(ki)); CALLRT(f,"rt_issubclass"); E(f,"or [esp],eax"); }
        E(f,"pop eax"); E(f,"add esp,4"); return 0;
    }
    if(!strcmp(n,"isinstance")){
        Expr *k=e->items[1]; int nk=k->kind==EXPR_TUPLE?k->count:1;
        if(t0->k==TY_OBJ){                                     /* an object: its class (None: an instance of nothing here) */
            gen_borrow(f,a0); E(f,"push eax"); E(f,"push 0");
            for(int i=0;i<nk;i++){ XInfo *ki=xinfo(k->kind==EXPR_TUPLE?k->items[i]:k);
                if(ki->kind!=X_TYPEVAL){                         /* a class value: its descriptor, when running */
                    gen(f,k->kind==EXPR_TUPLE?k->items[i]:k); E(f,"mov edx,eax"); E(f,"mov eax,[esp+4]"); CALLRT(f,"rt_isinstance"); E(f,"or [esp],eax");
                    continue; }
                if(!ki->cls){ if(!strcmp(ki->name,"object")){ int l=new_label(); E(f,"cmp dword [esp+4],0"); E(f,"je L%d",l); E(f,"mov dword [esp],1"); LBL(f,l); }
                    else if(!strcmp(ki->name,"NoneType")){ int l=new_label(); E(f,"cmp dword [esp+4],0"); E(f,"jne L%d",l); E(f,"mov dword [esp],1"); LBL(f,l); }
                    continue; }
                for(int q=-1;q<ki->cls->ninsts;q++){ AClass *kc=q<0?ki->cls:ki->cls->insts[q];   /* (a generic class: any of its instances) */
                    use_class(kc);
                    E(f,"mov eax,[esp+4]"); E(f,"mov edx,%s",class_label(kc,0)); CALLRT(f,"rt_isinstance"); E(f,"or [esp],eax"); }
                for(int q=0;q<gg->p->nclasses;q++){ AClass *kc=gg->p->classes[q];       /* a mixin: the classes it was copied into */
                    for(int v=0;v<kc->nvbases;v++) if(kc->vbases[v]==ki->cls){ use_class(kc);
                        E(f,"mov eax,[esp+4]"); E(f,"mov edx,%s",class_label(kc,0)); CALLRT(f,"rt_isinstance"); E(f,"or [esp],eax"); } } }
            E(f,"pop eax"); E(f,"add esp,4"); return 0;
        }
        int match=0, none_ok=0;                                 /* a value of a built-in type: known but for None */
        for(int i=0;i<nk;i++){ XInfo *ki=xinfo(k->kind==EXPR_TUPLE?k->items[i]:k);
            if(ki->cls) continue;
            if(!strcmp(ki->name,"NoneType")) none_ok=1; else if(btype_match(t0,ki->name)) match=1; }
        if(!is_opt(t0) && !is_none_lit(a0)){ int o=gen(f,a0); drop_value(f,t0,o); E(f,"mov eax,%d",match); return 0; }
        if(is_none_lit(a0)){ E(f,"mov eax,%d",none_ok); return 0; }
        int o=gen(f,a0); int lnone=new_label(), lend=new_label();
        jump_if_none(f,t0,lnone); drop_value(f,t0,o); E(f,"mov eax,%d",match); E(f,"jmp L%d",lend);
        LBL(f,lnone); drop_value(f,t0,o); E(f,"mov eax,%d",none_ok); LBL(f,lend); return 0;
    }
    if(!strcmp(n,"input")){ if(a0) gen_borrow(f,a0); else E(f,"xor eax,eax"); CALLRT(f,"rt_input"); return 1; }
    if(!strcmp(n,"round")){
        if(e->count==1){ gen_as(f,a0,TY_FLOAT_T,0); CALLRT(f,"rt_fround"); return 0; }
        gen_as(f,a0,TY_FLOAT_T,0); E(f,"sub esp,8"); E(f,"fstp qword [esp]");
        gen(f,e->items[1]); to_i32(f,TY(e->items[1])); E(f,"fld qword [esp]"); E(f,"add esp,8"); CALLRT(f,"rt_fround_n"); return 0;
    }
    if(!strcmp(n,"list")){
        if(!a0){ new_list(f,t->elem); return 1; }
        XInfo *ai=xinfo(a0);
        if(ai->kind==X_BUILTIN && ai->name && !strcmp(ai->name,"range")){ gen_range_list(f,a0); return 1; }
        gen_list_of(f,a0,t->elem);
        return 1;
    }
    if(!strcmp(n,"set")){
        if(!a0){ new_set(f,t->elem); return 1; }
        if(t0->k==TY_SET){ gen_borrow(f,a0); E(f,"mov edx,%s",kd_of(t->elem)); CALLRT(f,"rt_set_copy"); return 1; }   /* CPython: merged */
        if(t0->k==TY_LIST) gen_borrow(f,a0);
        else { gen_list_of(f,a0,t->elem); hold(f); }
        E(f,"mov edx,%s",kd_of(t->elem)); CALLRT(f,"rt_set_from_list"); return 1;
    }
    if(!strcmp(n,"range_list")){ gen_range_list(f,e); return 1; }  /* range(...) as a value */
    if(!strcmp(n,"dict_copy")){ gen_borrow(f,a0); CALLRT(f,"rt_dict_copy"); return 1; }
    if(!strcmp(n,"dict")){ new_dict(f,t); return 1; }
    cg_fail(e->line,"unsupported builtin");
}

/* the receiver of a method of a built-in type (AttributeError if None) */
static void gen_recv(F *f, Expr *obj, const char *m){
    gen_borrow(f,obj); check_none(f,TY(obj),"AttributeError",none_msg("'NoneType' object has no attribute '%s'",m));
}
/* s.find(sub[, start[, end]]) and the like: {s, sub, start, end, flags} on
   the stack for the runtime (flags: 1 no start, 2 no end, 4 sub is a tuple) */
static void gen_str_ranged(F *f, Expr *e, Expr *obj, const char *m, const char *routine, int mode){
    int flags=0, isbytes=TY(obj)->k==TY_BYTES;
    E(f,"sub esp,20");
    gen_recv(f,obj,m); if(isbytes) CALLRT(f,"rt_bytes_cp"); E(f,"mov [esp],eax");
    Ty *nt=TY(e->items[0]);
    if(isbytes && (nt->k==TY_INT||nt->k==TY_BOOL)){ gen(f,e->items[0]); if(!is_lng(nt)) widen(f,0); CALLRT(f,"rt_bytes_byte"); }   /* b.find(65): one byte */
    else gen_borrow(f,e->items[0]);
    E(f,"mov [esp+4],eax");
    if(TY(e->items[0])->k==TY_TUPLE) flags|=4;
    for(int k=1;k<3;k++){
        if(k<e->count && !is_none_lit(e->items[k])){ gen(f,e->items[k]); to_i32(f,TY(e->items[k])); E(f,"mov [esp+%d],eax",4+4*k); }
        else { flags|=k; E(f,"mov dword [esp+%d],0",4+4*k); }
    }
    E(f,"mov dword [esp+16],%d",flags);
    E(f,"mov eax,%d",mode); CALLRT(f,routine); E(f,"add esp,20");
}
/* an encoding's name -> rt_codec's number for it (-1: unknown, found out when the program runs) */
static int codec_number(const char *name){
    char n[40]; int k=0;
    if(strlen(name)>=sizeof n) return -1;
    for(const char *p=name;*p;p++){ char c=*p; if(c>='A'&&c<='Z') c=(char)(c+32); if(c=='_'||c==' ') c='-'; n[k++]=c; }
    n[k]=0;
    if(!strcmp(n,"utf-8")||!strcmp(n,"utf8")||!strcmp(n,"u8")) return 0;
    if(!strcmp(n,"ascii")||!strcmp(n,"us-ascii")) return 1;
    if(!strcmp(n,"latin-1")||!strcmp(n,"latin1")||!strcmp(n,"iso-8859-1")||!strcmp(n,"iso8859-1")||!strcmp(n,"l1")) return 2;
    return -1;
}
/* s.encode(encoding, errors), b.decode(...): routine gets eax = the object, edx = the codec, ecx = errors (0: strict) */
static void gen_codec_call(F *f, Expr *obj, Expr *enc, Expr *errs, const char *m, const char *routine){
    if(m) gen_recv(f,obj,m); else gen_borrow(f,obj);
    E(f,"push eax");
    if(enc && !is_none_lit(enc)){
        int k= enc->kind==EXPR_LITERAL && enc->tok->kind==T_STRING ? codec_number(enc->tok->text) : -1;
        if(k>=0) E(f,"push %d",k); else { gen_borrow(f,enc); CALLRT(f,"rt_codec"); E(f,"push eax"); }
    } else E(f,"push 0");
    if(errs && !is_none_lit(errs)) gen_borrow(f,errs); else E(f,"xor eax,eax");
    E(f,"mov ecx,eax"); E(f,"pop edx"); E(f,"pop eax"); CALLRT(f,routine);
}
/* a byte order ("big" / "little") -> edx = 1 when little; checked when the program runs unless a literal */
static void gen_byteorder(F *f, Expr *x){
    if(!x || is_none_lit(x)){ E(f,"xor edx,edx"); return; }
    if(x->kind==EXPR_LITERAL && x->tok->kind==T_STRING && (!strcmp(x->tok->text,"big")||!strcmp(x->tok->text,"little"))){ E(f,"mov edx,%d",x->tok->text[0]=='l'); return; }
    gen_borrow(f,x); CALLRT(f,"rt_byteorder"); E(f,"mov edx,eax");
}
static int gen_tmethod(F *f, Expr *e, XInfo *xi){
    const char *m=xi->name; Expr *obj=e->a->a; Ty *t0=TY(obj);

    Expr *a0=e->count?e->items[0]:NULL, *a1=e->count>1?e->items[1]:NULL;
    if(t0->k==TY_STR){
        if(!strcmp(m,"format")) return gen_str_format(f,e);
        if(!strcmp(m,"ljust")||!strcmp(m,"rjust")||!strcmp(m,"center")||!strcmp(m,"zfill")){
            int mode= m[0]=='l' ? 1 : m[0]=='c' ? 3 : m[0]=='z' ? 4|'0'<<8 : 0;           /* see rt_sb_pad */
            rt("rt_sb_need"); E(f,"push dword [rt_sb_len]");
            gen_recv(f,obj,m); CALLRT(f,"rt_sb_str");
            gen(f,a0); to_i32(f,TY(a0)); E(f,"push eax");
            if(a1){ gen_borrow(f,a1); CALLRT(f,"rt_fillchar"); E(f,"shl eax,8"); E(f,"or eax,%d",mode); E(f,"mov edx,eax"); } else E(f,"mov edx,%d",mode);
            E(f,"pop ecx"); E(f,"mov eax,[esp]"); CALLRT(f,"rt_sb_pad");
            E(f,"pop eax"); CALLRT(f,"rt_sb_take"); return 1;
        }
        if(!strcmp(m,"strip")||!strcmp(m,"lstrip")||!strcmp(m,"rstrip")){
            gen_recv(f,obj,m);
            if(a0 && !is_none_lit(a0)){ E(f,"push eax"); gen_borrow(f,a0); E(f,"mov ecx,eax"); E(f,"pop eax"); } else E(f,"xor ecx,ecx");
            E(f,"mov edx,%d",m[0]=='s'?3:m[0]=='l'?1:2); CALLRT(f,"rt_str_strip"); return 1;
        }
        if(!strcmp(m,"encode")){ gen_codec_call(f,obj,a0,a1,m,"rt_str_encode"); return 1; }
        if(!strcmp(m,"startswith")||!strcmp(m,"endswith")){ gen_str_ranged(f,e,obj,m,"rt_str_affix",m[0]=='e'); return 0; }
        if(!strcmp(m,"find")||!strcmp(m,"rfind")||!strcmp(m,"index")||!strcmp(m,"rindex")){
            gen_str_ranged(f,e,obj,m,"rt_str_find",(m[0]=='r')|(strstr(m,"index")?2:0)); widen(f,1); return 0; }
        if(!strcmp(m,"count")){ gen_str_ranged(f,e,obj,m,"rt_str_count",0); widen(f,1); return 0; }
        if(!strcmp(m,"replace")){
            E(f,"sub esp,16"); gen_recv(f,obj,m); E(f,"mov [esp],eax");
            gen_borrow(f,a0); E(f,"mov [esp+4],eax"); gen_borrow(f,a1); E(f,"mov [esp+8],eax");
            if(e->count==3){ gen(f,e->items[2]); to_i32(f,TY(e->items[2])); E(f,"mov [esp+12],eax"); } else E(f,"mov dword [esp+12],-1");
            CALLRT(f,"rt_str_replace"); E(f,"add esp,16"); return 1;
        }
        if(!strcmp(m,"split")||!strcmp(m,"rsplit")){
            gen_recv(f,obj,m); E(f,"push eax");
            if(a0 && !is_none_lit(a0)) gen_borrow(f,a0); else E(f,"xor eax,eax");
            E(f,"push eax");
            if(a1){ gen(f,a1); to_i32(f,TY(a1)); E(f,"mov ecx,eax"); } else E(f,"or ecx,-1");
            E(f,"pop edx"); E(f,"pop eax"); rt("rt_str_split"); E(f,"call %s",m[0]=='r'?"rt_str_rsplit":"rt_str_split"); return 1;
        }
        if(!strcmp(m,"partition")||!strcmp(m,"rpartition")){
            gen_recv(f,obj,m); E(f,"push eax"); gen_borrow(f,a0); E(f,"mov edx,eax"); E(f,"pop eax");
            E(f,"mov ecx,TD%d",tdesc(TY(e))); rt("rt_str_partition"); E(f,"call %s",m[0]=='r'?"rt_str_rpartition":"rt_str_partition"); return 1;
        }
        if(!strcmp(m,"removeprefix")||!strcmp(m,"removesuffix")){
            gen_recv(f,obj,m); E(f,"push eax"); gen_borrow(f,a0); E(f,"mov edx,eax"); E(f,"pop eax");
            E(f,"mov ecx,%d",m[6]=='s'); CALLRT(f,"rt_str_remove"); return 1;
        }
        if(!strcmp(m,"expandtabs")){
            gen_recv(f,obj,m);
            if(a0){ E(f,"push eax"); gen(f,a0); to_i32(f,TY(a0)); E(f,"mov edx,eax"); E(f,"pop eax"); } else E(f,"mov edx,8");
            CALLRT(f,"rt_str_expandtabs"); return 1;
        }
        if(!strcmp(m,"splitlines")){
            gen_recv(f,obj,m);
            if(a0){ E(f,"push eax"); gen(f,a0); E(f,"mov edx,eax"); E(f,"pop eax"); } else E(f,"xor edx,edx");
            CALLRT(f,"rt_str_splitlines"); return 1;
        }
        gen_recv(f,obj,m);
        if(!strcmp(m,"upper")||!strcmp(m,"lower")||!strcmp(m,"swapcase")||!strcmp(m,"title")||!strcmp(m,"capitalize")||!strcmp(m,"casefold")){
            static const char *const modes[]={"upper","lower","swapcase","title","capitalize","casefold"};
            int k=0; for(int i=0;i<6;i++) if(!strcmp(m,modes[i])) k=i;
            E(f,"mov edx,%d",k); CALLRT(f,"rt_str_case"); return 1; }
        if(str_is_mode(m)>=0){ E(f,"mov edx,%d",str_is_mode(m)); CALLRT(f,"rt_str_is"); return 0; }
        E(f,"push eax");
        if(a0) gen_borrow(f,a0); else E(f,"xor eax,eax");
        if(a0 && !strcmp(m,"join") && TY(a0)->k==TY_STR){ CALLRT(f,"rt_str_chars"); hold(f); }    /* sep.join("abc") */
        if(a0 && !strcmp(m,"join") && TY(a0)->k==TY_SET){ E(f,"mov edx,%s",kd_of(TY(a0)->elem)); CALLRT(f,"rt_set_to_list"); hold(f); }
        E(f,"mov edx,eax"); E(f,"pop eax");
        if(!strcmp(m,"join")){ CALLRT(f,"rt_str_join"); return 1; }
    }
    if(t0->k==TY_BYTES){
        if(!strcmp(m,"decode")){ gen_codec_call(f,obj,a0,a1,m,"rt_bytes_decode"); return 1; }
        if(!strcmp(m,"startswith")||!strcmp(m,"endswith")){ gen_str_ranged(f,e,obj,m,"rt_str_affix",m[0]=='e'); return 0; }
        if(!strcmp(m,"find")||!strcmp(m,"rfind")||!strcmp(m,"index")||!strcmp(m,"rindex")){
            gen_str_ranged(f,e,obj,m,"rt_str_find",4|(m[0]=='r')|(strstr(m,"index")?2:0)); widen(f,1); return 0; }
        if(!strcmp(m,"count")){ gen_str_ranged(f,e,obj,m,"rt_str_count",0); widen(f,1); return 0; }
        if(!strcmp(m,"replace")){
            E(f,"sub esp,16"); gen_recv(f,obj,m); E(f,"mov [esp],eax");
            gen_borrow(f,a0); E(f,"mov [esp+4],eax"); gen_borrow(f,a1); E(f,"mov [esp+8],eax");
            if(e->count==3){ gen(f,e->items[2]); to_i32(f,TY(e->items[2])); E(f,"mov [esp+12],eax"); } else E(f,"mov dword [esp+12],-1");
            CALLRT(f,"rt_bytes_replace"); E(f,"add esp,16"); return 1;
        }
        if(!strcmp(m,"split")||!strcmp(m,"rsplit")){
            gen_recv(f,obj,m); E(f,"push eax");
            if(a0 && !is_none_lit(a0)) gen_borrow(f,a0); else E(f,"xor eax,eax");
            E(f,"push eax");
            if(a1){ gen(f,a1); to_i32(f,TY(a1)); E(f,"mov ecx,eax"); } else E(f,"or ecx,-1");
            E(f,"pop edx"); E(f,"pop eax"); rt("rt_bytes_split"); E(f,"call %s",m[0]=='r'?"rt_bytes_rsplit":"rt_bytes_split"); return 1;
        }
        if(!strcmp(m,"strip")||!strcmp(m,"lstrip")||!strcmp(m,"rstrip")){
            gen_recv(f,obj,m);
            if(a0 && !is_none_lit(a0)){ E(f,"push eax"); gen_borrow(f,a0); E(f,"mov ecx,eax"); E(f,"pop eax"); } else E(f,"xor ecx,ecx");
            E(f,"mov edx,%d",m[0]=='s'?3:m[0]=='l'?1:2); CALLRT(f,"rt_bytes_strip"); return 1;
        }
        if(!strcmp(m,"partition")||!strcmp(m,"rpartition")){
            gen_recv(f,obj,m); E(f,"push eax"); gen_borrow(f,a0); E(f,"mov edx,eax"); E(f,"pop eax");
            E(f,"mov ecx,TD%d",tdesc(TY(e))); rt("rt_str_partition"); E(f,"call %s",m[0]=='r'?"rt_str_rpartition":"rt_str_partition"); return 1;
        }
        if(!strcmp(m,"removeprefix")||!strcmp(m,"removesuffix")){
            gen_recv(f,obj,m); E(f,"push eax"); gen_borrow(f,a0); E(f,"mov edx,eax"); E(f,"pop eax");
            E(f,"mov ecx,%d",m[6]=='s'); CALLRT(f,"rt_str_remove"); return 1;
        }
        if(!strcmp(m,"splitlines")){
            gen_recv(f,obj,m);
            if(a0){ E(f,"push eax"); gen(f,a0); E(f,"mov edx,eax"); E(f,"pop eax"); } else E(f,"xor edx,edx");
            CALLRT(f,"rt_bytes_splitlines"); return 1;
        }
        if(!strcmp(m,"hex") && a0){
            gen_recv(f,obj,m); E(f,"push eax"); gen_borrow(f,a0); E(f,"push eax");
            if(a1){ gen_as(f,a1,TY_INT_T,0); to_i32(f,TY_INT_T); E(f,"mov ecx,eax"); } else E(f,"mov ecx,1");
            E(f,"pop edx"); E(f,"pop eax"); CALLRT(f,"rt_bytes_hex_sep"); return 1; }
        gen_recv(f,obj,m);
        if(!strcmp(m,"hex")){ CALLRT(f,"rt_bytes_hex"); return 1; }
        if(bytes_case_mode(m)>=0){ E(f,"mov edx,%d",bytes_case_mode(m)); CALLRT(f,"rt_bytes_case"); return 1; }
        if(bytes_is_mode(m)>=0){ E(f,"mov edx,%d",bytes_is_mode(m)); CALLRT(f,"rt_bytes_is"); return 0; }
        if(!strcmp(m,"join")){
            E(f,"push eax"); gen_borrow(f,a0);
            if(TY(a0)->k==TY_SET){ E(f,"mov edx,%s",kd_of(TY(a0)->elem)); CALLRT(f,"rt_set_to_list"); hold(f); }
            E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_str_join"); return 1; }
    }
    if(t0->k==TY_INT||t0->k==TY_BOOL){
        if(!strcmp(m,"to_bytes")){                              /* n.to_bytes(length=1, byteorder="big", signed=False) */
            gen_as(f,obj,TY_INT_T,0); push_value(f,TY_INT_T);
            if(a0 && !is_none_lit(a0)){ gen(f,a0); to_i32(f,TY(a0)); E(f,"push eax"); } else E(f,"push 1");
            gen_byteorder(f,a1); E(f,"push edx");
            if(e->count>2) gen(f,e->items[2]); else E(f,"xor eax,eax");
            E(f,"push eax"); CALLRT(f,"rt_int_to_bytes"); E(f,"add esp,20"); return 1; }
        gen_as(f,obj,TY_INT_T,0);
        if(!strcmp(m,"bit_length")){ CALLRT(f,"rt_int_bitlen"); widen(f,0); return 0; }
        if(!strcmp(m,"bit_count")){ CALLRT(f,"rt_int_bitcount"); widen(f,0); return 0; }
    }
    if(t0->k==TY_FLOAT && !strcmp(m,"is_integer")){             /* x - round(x) == 0 (a NaN for inf and nan) */
        gen_as(f,obj,TY_FLOAT_T,0); E(f,"fld st0"); E(f,"frndint"); E(f,"fsubp st1,st0"); E(f,"ftst"); E(f,"fnstsw ax"); E(f,"fstp st0"); E(f,"sahf");
        E(f,"sete al"); E(f,"setnp cl"); E(f,"and al,cl"); E(f,"movzx eax,al"); return 0; }
    if(t0->k==TY_LIST){
        Ty *el=t0->elem; int es=esize(el);
        gen_recv(f,obj,m);
        if(!strcmp(m,"reverse")){ CALLRT(f,"rt_list_reverse"); return 0; }
        if(!strcmp(m,"sort") && (xi->key||xi->rev)){
            int src=ref_slot(f); incref(f); E(f,"mov [ebp%+d],eax",src);
            int rev=gen_rev_flag(f,xi->rev);
            if(xi->key){ gen_sort_by_key(f,src,el,xi->key,rev,1); E(f,"mov eax,[ebp%+d]",src); E(f,"mov dword [ebp%+d],0",src); CALLRT(f,"rt_decref"); return 0; }
            E(f,"mov eax,[ebp%+d]",src); CALLRT(f,"rt_list_sort");
            { int l=new_label(); E(f,"cmp dword [ebp%+d],0",rev); E(f,"je L%d",l); E(f,"mov eax,[ebp%+d]",src); CALLRT(f,"rt_list_reverse"); LBL(f,l); }
            E(f,"mov eax,[ebp%+d]",src); E(f,"mov dword [ebp%+d],0",src); CALLRT(f,"rt_decref"); return 0;
        }
        if(!strcmp(m,"sort")){ CALLRT(f,"rt_list_sort"); return 0; }
        if(!strcmp(m,"clear")){ CALLRT(f,"rt_list_clear"); return 0; }
        if(!strcmp(m,"copy")){ E(f,"mov edx,%s",kd_of(el)); CALLRT(f,"rt_list_copy"); return 1; }
        E(f,"push eax");
        if(!strcmp(m,"append")){ gen_as(f,a0,el,1); list_append_top(f,el); E(f,"add esp,4"); return 0; }
        if(!strcmp(m,"pop")){ if(a0){ gen(f,a0); to_i32(f,TY(a0)); } else E(f,"or eax,-1");
            E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_list_pop"); load_mem(f,el,"eax",0); return is_ptr(el); }
        if(!strcmp(m,"insert")){ gen(f,a0); to_i32(f,TY(a0)); E(f,"push eax"); gen_as(f,a1,el,1); push_value(f,el);
            E(f,"mov eax,[esp+%d]",es+4); E(f,"mov edx,[esp+%d]",es); CALLRT(f,"rt_list_insert");
            E(f,"mov ecx,eax"); pop_value(f,el); store_new(f,el,"ecx",0);
            E(f,"add esp,8"); return 0; }
        if(!strcmp(m,"extend")){
            if(TY(a0)->k==TY_LIST) gen_borrow(f,a0); else { gen_list_of(f,a0,el); hold(f); }
            E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_list_extend"); return 0; }
        gen_as(f,a0,el,0); push_value(f,el);                     /* remove / index / count: the value by address */
        E(f,"mov edx,esp"); E(f,"mov eax,[esp+%d]",es);
        CALLRT(f,!strcmp(m,"remove")?"rt_list_remove":!strcmp(m,"index")?"rt_list_index":"rt_list_count");
        E(f,"add esp,%d",es+4);
        if(m[0]!='r') widen(f,0);
        return 0;
    }
    if(t0->k==TY_DICT){
        Ty *v=t0->elem, *kt=ty_dkey(t0); int vs=esize(v), ks=esize(kt);
        gen_recv(f,obj,m);
        if(!strcmp(m,"keys")){ CALLRT(f,"rt_dict_keys"); return 1; }
        if(!strcmp(m,"items")){ E(f,"mov edx,TD%d",tdesc(ty_find(TY(e))->elem)); CALLRT(f,"rt_dict_items"); return 1; }
        if(!strcmp(m,"values")){ CALLRT(f,"rt_dict_values"); return 1; }
        if(!strcmp(m,"clear")){ CALLRT(f,"rt_dict_clear"); return 0; }
        if(!strcmp(m,"copy")){ CALLRT(f,"rt_dict_copy"); return 1; }
        E(f,"push eax");
        if(!strcmp(m,"update")){ gen_borrow(f,a0); E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_dict_update"); return 0; }
        gen_as(f,a0,kt,0); push_value(f,kt);                     /* [esp] key, [esp+ks] the dict */
        E(f,"mov edx,esp"); E(f,"mov eax,[esp+%d]",ks); CALLRT(f,"rt_dict_find");
        int lmiss=new_label(), lend=new_label(), owned=0;
        E(f,"cmp eax,-1"); E(f,"je L%d",lmiss);
        if(!strcmp(m,"get")||!strcmp(m,"setdefault")){
            E(f,"mov ecx,[esp+%d]",ks); E(f,"mov ecx,[ecx+20]");
            if(vs==8) E(f,"lea ecx,[ecx+eax*8]"); else E(f,"lea ecx,[ecx+eax*4]");
            load_mem(f,v,"ecx",0);
            E(f,"jmp L%d",lend); LBL(f,lmiss);
            if(m[0]=='g'){ if(a1) gen_as(f,a1,v,0); else gen_none(f,v); }
            else { gen_as(f,a1,v,1); push_value(f,v);
                E(f,"lea edx,[esp+%d]",vs); E(f,"mov eax,[esp+%d]",vs+ks); CALLRT(f,"rt_dict_slot");
                E(f,"mov ecx,eax"); pop_value(f,v); store_new(f,v,"ecx",0); load_mem(f,v,"ecx",0); }
        } else {                                                    /* pop */
            E(f,"mov edx,esp"); E(f,"mov eax,[esp+%d]",ks); CALLRT(f,"rt_dict_del"); load_mem(f,v,"eax",0);
            E(f,"jmp L%d",lend); LBL(f,lmiss);
            if(a1) gen_as(f,a1,v,1); else { E(f,"mov eax,esp"); E(f,"mov ecx,%s",kd_of(kt)); CALLRT(f,"rt_raise_key"); }
            owned=1;
        }
        LBL(f,lend); E(f,"add esp,%d",ks+4);
        return owned && is_ptr(v);
    }
    if(t0->k==TY_FILE){
        gen_recv(f,obj,m);
        if(!strcmp(m,"read")){ CALLRT(f,"rt_file_read"); return 1; }
        if(!strcmp(m,"readline")){ CALLRT(f,"rt_file_readline"); return 1; }
        if(!strcmp(m,"readlines")){ CALLRT(f,"rt_file_readlines"); return 1; }
        if(!strcmp(m,"close")){ CALLRT(f,"rt_file_close"); return 0; }
        E(f,"push eax"); gen_borrow(f,a0); E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_file_write"); widen(f,0); return 0;
    }
    if(t0->k==TY_GEN){                                     /* g.send(v), g.throw(exc), g.close() */
        Ty *el=ty_find(t0->elem); int slot=frame_slot(f,4), lok=new_label();
        gen_borrow(f,obj); panic_if_null(f); E(f,"mov [ebp%+d],eax",slot);
        if(!strcmp(m,"close")||!strcmp(m,"aclose")){ E(f,"mov edx,1"); CALLRT(f,"rt_gen_close"); E(f,"test eax,eax"); E(f,"jz L%d",lok);
            rt("rt_raise_builtin"); E(f,"mov eax,VTX_RuntimeError"); E(f,"mov edx,DTX_RuntimeError"); E(f,"xor ecx,ecx");
            E(f,"mov esi,Z%d",zlit(m[0]=='a'?"async generator ignored GeneratorExit":"generator ignored GeneratorExit")); E(f,"call rt_raise_builtin");
            LBL(f,lok); return 0; }
        if(!strcmp(m,"throw")||!strcmp(m,"athrow")){ gen_owned(f,a0); E(f,"mov edx,eax"); E(f,"mov eax,[ebp%+d]",slot); CALLRT(f,"rt_gen_throw"); }
        else {
            Ty *st=gpart(t0,0);
            if(a0 && !is_none_lit(a0) && st->k!=TY_VOID){
                gen_as(f,a0,st,1); push_value(f,st);
                int ok=new_label(); E(f,"mov ecx,[ebp%+d]",slot); E(f,"cmp dword [ecx+8],0"); E(f,"jne L%d",ok);
                E(f,"mov esi,Z%d",zlit("can't send non-None value to a just-started generator")); CALLRT(f,"rt_typeerr_text");
                LBL(f,ok); pop_value(f,st);
                E(f,"mov ecx,[ebp%+d]",slot); store_mem(f,st,"ecx",88); E(f,"mov ecx,[ebp%+d]",slot); E(f,"mov dword [ecx+96],1"); }
            E(f,"mov eax,[ebp%+d]",slot); CALLRT(f,"rt_gen_next"); }
        E(f,"test eax,eax"); E(f,"jnz L%d",lok); gen_raise_stop(f,slot,t0);
        LBL(f,lok); E(f,"mov eax,[ebp%+d]",slot); load_mem(f,el,"eax",24); if(is_ptr(el)) incref(f);
        return is_ptr(el);
    }
    if(t0->k==TY_TASK){
        gen_recv(f,obj,m);
        if(!strcmp(m,"done")){ E(f,"cmp dword [eax+8],4"); E(f,"sete al"); E(f,"movzx eax,al"); return 0; }
        CALLRT(f,"rt_task_result"); if(ty_find(t0->elem)->k!=TY_VOID) load_mem(f,t0->elem,"eax",24); return 0;
    }
    if(t0->k==TY_SET){
        Ty *el=t0->elem; int es=esize(el);
        gen_recv(f,obj,m);
        if(!strcmp(m,"clear")){ CALLRT(f,"rt_set_clear"); return 0; }
        if(!strcmp(m,"copy")){ E(f,"mov edx,%s",kd_of(el)); CALLRT(f,"rt_set_copy"); return 1; }
        if(!strcmp(m,"pop")){ CALLRT(f,"rt_set_pop"); load_mem(f,el,"eax",0); return is_ptr(el); }
        E(f,"push eax");
        static const char *ops[]={"union","intersection","difference","symmetric_difference",NULL};
        static const char *iops[]={"update","intersection_update","difference_update","symmetric_difference_update",NULL};
        for(int k=0;ops[k];k++) if(!strcmp(m,ops[k])||!strcmp(m,iops[k])){
            if(TY(a0)->k!=TY_SET) cg_fail(e->line,"set methods take sets in compiled code");
            gen_borrow(f,a0);
            E(f,"mov edx,eax"); E(f,"pop eax"); E(f,"mov ecx,%d",k);
            if(!strcmp(m,ops[k])){ CALLRT(f,"rt_set_op"); return 1; }
            CALLRT(f,"rt_set_iop"); return 0; }
        if(!strcmp(m,"issubset")||!strcmp(m,"issuperset")||!strcmp(m,"isdisjoint")){
            gen_borrow(f,a0); E(f,"mov edx,eax"); E(f,"pop eax");
            if(!strcmp(m,"issuperset")) E(f,"xchg eax,edx");
            CALLRT(f,m[2]=='d'?"rt_set_disjoint":"rt_set_le"); return 0; }
        gen_as(f,a0,el,0);
        if(!strcmp(m,"add")){ set_add_top(f,el); E(f,"add esp,4"); return 0; }
        push_value(f,el);                                        /* remove / discard: the value by address */
        E(f,"mov edx,esp"); E(f,"mov eax,[esp+%d]",es); CALLRT(f,m[0]=='d'?"rt_set_discard":"rt_set_remove");
        E(f,"add esp,%d",es+4); return 0;
    }
    cg_fail(e->line,"unsupported method");
}

static int gen_sys(F *f, Expr *e, XInfo *xi){
    const char *m=xi->name;
    if(!strcmp(m,"buffer")){ Ty *t0=TY(e->items[0]); gen_borrow(f,e->items[0]); to_i32(f,t0); CALLRT(f,t0->k==TY_STR||t0->k==TY_BYTES?"rt_buf_from_str":"rt_buf_new"); return 1; }
    if(!strcmp(m,"addr")){ gen_borrow(f,e->items[0]); E(f,"add eax,12"); widen(f,0); return 0; }
    if(!strcmp(m,"_rawargs")){ gen(f,e->items[0]); CALLRT(f,"rt_sys_args"); return 1; }
    if(!strcmp(m,"_sleep")){ gen_as(f,e->items[0],TY_FLOAT_T,0); CALLRT(f,"rt_time_sleep"); return 0; }
    if(!strcmp(m,"exit")){ if(e->count) gen(f,e->items[0]); else E(f,"xor eax,eax"); E(f,"mov ebx,eax"); CALLRT(f,"rt_exit"); return 0; }
    if(!strcmp(m,"peek_at")||!strcmp(m,"peek_str_at")||!strcmp(m,"cstr_at")||!strcmp(m,"poke_str_at")){   /* raw memory: (address, n | str) */
        gen(f,e->items[0]); E(f,"push eax");
        if(m[1]=='o') gen_borrow(f,e->items[1]); else gen(f,e->items[1]);
        E(f,"mov edx,eax"); E(f,"pop eax");
        if(!strcmp(m,"peek_at")){ CALLRT(f,"rt_mem_peek"); widen(f,0); return 0; }
        if(!strcmp(m,"poke_str_at")){ CALLRT(f,"rt_mem_poke_str"); return 0; }
        CALLRT(f,m[0]=='c'?"rt_mem_cstr":"rt_mem_peek_str"); return 1;
    }
    if(!strcmp(m,"poke_at")){ gen(f,e->items[0]); E(f,"push eax"); gen(f,e->items[1]); E(f,"push eax"); gen(f,e->items[2]);
        E(f,"mov ecx,eax"); E(f,"pop edx"); E(f,"pop eax"); CALLRT(f,"rt_mem_poke"); return 0; }
    int n=e->count;
    E(f,"sub esp,%d",4*n);
    for(int i=0;i<n;i++){ gen_borrow(f,e->items[i]); E(f,"mov [esp+%d],eax",4*i); }
    if(!strcmp(m,"poke")){ CALLRT(f,"rt_buf_poke"); E(f,"add esp,16"); return 0; }
    if(!strcmp(m,"peek")){ CALLRT(f,"rt_buf_peek"); E(f,"add esp,12"); widen(f,0); return 0; }
    E(f,"pop eax"); E(f,"pop edx"); E(f,"pop ecx");
    if(!strcmp(m,"poke_str")||!strcmp(m,"poke_bytes")){ CALLRT(f,"rt_buf_poke_str"); return 0; }   /* (bytes: the same layout) */
    CALLRT(f,!strcmp(m,"peek_bytes")?"rt_buf_peek_bytes":"rt_buf_peek_str"); return 1;
}

/* A C function through ctypes (cdecl, i386 System V): the arguments are
   built in a block the call finds 16-byte aligned, the old esp saved above
   it; the function is reached through its slot CI<id>, which the dynamic
   linker fills in. Results: eax (edx:eax for 64 bits: the low half), st0 for
   floating point, a char * copied into a new str. */
static int csize(int k){ return k==CT_DOUBLE||k==CT_LONGLONG||k==CT_ULONGLONG ? 8 : 4; }
/* macos: the C translation (aot_x2c.c) calls the function natively, so each
   call carries its C signature in a comment: ";@ccall R:ARGS[|VARARGS]", one
   letter per type (i int, u unsigned, h/H short, b/B char, o bool, l/L long,
   q/Q long long, d double, f float, s char *, p void *, v void). */
static char ct_letter(int k){
    switch(k){
        case CT_UINT: return 'u'; case CT_SHORT: return 'h'; case CT_USHORT: return 'H'; case CT_BYTE: return 'b';
        case CT_UBYTE: return 'B'; case CT_BOOL: return 'o'; case CT_LONG: return 'l'; case CT_ULONG: return 'L';
        case CT_LONGLONG: return 'q'; case CT_ULONGLONG: return 'Q'; case CT_DOUBLE: return 'd'; case CT_FLOAT: return 'f';
        case CT_CHARP: return 's'; case CT_VOIDP: return 'p'; case CT_VOID: return 'v'; default: return 'i';
    }
}
/* The fixed parameters of the C library's variadic functions: without
   argtypes the rest of their arguments are the variadic ones. */
static int c_variadic_fixed(const char *sym){
    static const struct { const char *name; int fixed; } v[]={{"printf",1},{"fprintf",2},{"dprintf",2},{"sprintf",2},{"snprintf",3},
        {"scanf",1},{"fscanf",2},{"sscanf",2},{"open",2},{"openat",3},{"fcntl",2},{"ioctl",2},{"syslog",2},{"execl",2},{"execlp",2},{NULL,0}};
    for(int i=0;v[i].name;i++) if(!strcmp(v[i].name,sym)) return v[i].fixed;
    return -1;
}
static void ccall_marker(F *f, ACFunc *cf, const int *kinds, int n){
    if(gg->target!=AOT_TARGET_MACOS) return;
    char sig[64]; int k=0, fixed=cf->nargtypes>=0 ? cf->nargtypes : c_variadic_fixed(cf->sym);
    sig[k++]=ct_letter(cf->restype); sig[k++]=':';
    for(int i=0;i<n;i++){ if(i==fixed) sig[k++]='|'; sig[k++]=ct_letter(kinds[i]); }
    sig[k]=0;
    buf_printf(&f->code,"        ;@ccall %s\n",sig);
}
static int gen_ccall(F *f, Expr *e, XInfo *xi){
    ACFunc *cf=xi->cfn; int off[16], size=0;
    for(int i=0;i<e->count;i++){ off[i]=size; size+=csize(xi->argmap[i]); }
    E(f,"mov eax,esp"); E(f,"sub esp,%d",size+4); E(f,"and esp,-16"); E(f,"mov [esp+%d],eax",size);
    for(int i=0;i<e->count;i++){ Expr *a=e->items[i]; int k=xi->argmap[i]; Ty *t=TY(a);
        if(k==CT_DOUBLE||k==CT_FLOAT){ gen_as(f,a,TY_FLOAT_T,0); E(f,k==CT_DOUBLE?"fstp qword [esp+%d]":"fstp dword [esp+%d]",off[i]); continue; }
        gen_borrow(f,a);
        if(t->k==TY_STR||t->k==TY_BYTES){ int l=new_label(), l2=new_label();                 /* the bytes, NUL-terminated ("" too) */
            E(f,"test eax,eax"); E(f,"jnz L%d",l); E(f,"mov eax,Z%d",zlit("")); E(f,"jmp L%d",l2); LBL(f,l); E(f,"add eax,12"); LBL(f,l2); }
        else if(t->k==TY_BUF){ int l=new_label(); E(f,"test eax,eax"); E(f,"jz L%d",l); E(f,"add eax,12"); LBL(f,l); }
        E(f,"mov [esp+%d],eax",off[i]);
        if(k==CT_LONGLONG||k==CT_ULONGLONG){ if(!is_lng(t)) widen(f,k==CT_LONGLONG); E(f,"mov [esp+%d],edx",off[i]+4); }
    }
    int kinds[16]; for(int i=0;i<e->count;i++) kinds[i]=xi->argmap[i];
    ccall_marker(f,cf,kinds,e->count);
    E(f,"call [CI%d]   ; %s",cf->id,cf->sym);
    E(f,"mov esp,[esp+%d]",size);
    switch(cf->restype){
        case CT_SHORT: E(f,"movsx eax,ax"); break;
        case CT_USHORT: E(f,"movzx eax,ax"); break;
        case CT_BYTE: E(f,"movsx eax,al"); break;
        case CT_UBYTE: E(f,"movzx eax,al"); break;
        case CT_BOOL: E(f,"test al,al"); E(f,"setnz al"); E(f,"movzx eax,al"); break;
        case CT_CHARP:{ int l=new_label(); E(f,"test eax,eax"); E(f,"jz L%d",l); E(f,"mov edx,0x7FFFFFFF"); CALLRT(f,"rt_mem_cstr"); LBL(f,l); return 1; }
        default: break;
    }
    if(is_lng(TY(e)) && cf->restype!=CT_LONGLONG && cf->restype!=CT_ULONGLONG)      /* as an int (64 bits) */
        widen(f,cf->restype==CT_DEFAULT||cf->restype==CT_INT||cf->restype==CT_SHORT||cf->restype==CT_BYTE||cf->restype==CT_LONG);
    return 0;
}
/* ctypes.create_string_buffer / string_at / addressof / get_errno */
static int gen_ctypes_fn(F *f, Expr *e, XInfo *xi){
    const char *m=xi->name+7;
    if(!strcmp(m,"create_string_buffer")){ Ty *t0=TY(e->items[0]); gen_borrow(f,e->items[0]);
        if(t0->k==TY_STR){ CALLRT(f,"rt_buf_from_str"); E(f,"inc dword [eax+8]"); }   /* and its 0 byte (a buffer has one more) */
        else CALLRT(f,"rt_buf_new");
        return 1; }
    if(!strcmp(m,"addressof")){ gen_borrow(f,e->items[0]); E(f,"add eax,12"); widen(f,0); return 0; }
    if(!strcmp(m,"string_at")){ gen(f,e->items[0]); E(f,"push eax");
        if(e->count==2){ gen(f,e->items[1]); to_i32(f,TY(e->items[1])); E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_mem_peek_str"); return 1; }
        E(f,"pop eax"); E(f,"mov edx,0x7FFFFFFF"); CALLRT(f,"rt_mem_cstr"); return 1; }
    if(!strcmp(m,"get_errno")){ E(f,"mov eax,esp"); E(f,"and esp,-16"); E(f,"sub esp,12"); E(f,"push eax");
        ccall_marker(f,xi->cfn,NULL,0);
        E(f,"call [CI%d]   ; __errno_location",xi->cfn->id); E(f,"pop esp"); E(f,"mov eax,[eax]"); widen(f,1); return 0; }
    cg_fail(e->line,"unsupported ctypes function");
    return 0;
}

static int gen_asyncio(F *f, Expr *e, XInfo *xi);
static int gen_bmod(F *f, Expr *e, XInfo *xi);
static int gen_json_dumps(F *f, Expr *e, XInfo *xi);
static int gen_call(F *f, Expr *e){
    XInfo *xi=xinfo(e); Ty *t=TY(e);
    switch(xi->kind){
        case X_FUNC: case X_STATIC: case X_CALLNEST: call_user(f,xi->fn,e,xi,0); return is_ptr(t);
        case X_CALLVAL: return call_value(f,e);
        case X_CALLDECO: return call_value_of(f,e,xi->var);
        case X_METHOD: call_user(f,xi->fn,e,xi,1); return is_ptr(t);
        case X_SUPER:
            if(!xi->fn){                                         /* super().__init__(message) of an exception */
                if(e->count) gen_exc_message_of(f,e->items[0],f->fn->cls); else E(f,"xor eax,eax");
                E(f,"mov ecx,[ebp+8]"); store_mem(f,TY_STR_T,"ecx",12);
                if(raw_message(e->count?e->items[0]:NULL,f->fn->cls)){ E(f,"mov ecx,[ebp+8]"); E(f,"mov dword [ecx+%d],1",exc_rawoff(f->fn->cls)); }
                return 0;
            }
            call_user(f,xi->fn,e,xi,xi->fn->is_static?0:2); return is_ptr(t);
        case X_CTOR: return gen_ctor(f,e,xi);
        case X_TYPECALL:{                                 /* cls_value(args): the constructor of the class it is */
            int slot=frame_slot(f,4), lend=new_label();
            gen(f,e->a); E(f,"mov [ebp%+d],eax",slot);
            for(int i=0;i<xi->ncands;i++){ int lnext=new_label(); AClass *k=xi->cands[i]; use_class(k);
                E(f,"cmp dword [ebp%+d],%s",slot,class_label(k,0)); E(f,"jne L%d",lnext);
                gen_ctor(f,e,xi->cxi[i]); E(f,"jmp L%d",lend);
                LBL(f,lnext); }
            E(f,"mov esi,Z%d",zlit("'NoneType' object is not callable")); CALLRT(f,"rt_typeerr_text");
            LBL(f,lend); return 1; }
        case X_BUILTIN: return gen_builtin(f,e,xi);
        case X_TMETHOD: return gen_tmethod(f,e,xi);
        case X_SYSCALL: gen_syscall_regs(f,e); E(f,"mov eax,esp"); CALLRT(f,"rt_syscall_list"); E(f,"add esp,28"); return 1;
        case X_SYS: return gen_sys(f,e,xi);
        case X_ASYNC: return gen_asyncio(f,e,xi);
        case X_BMOD: if(!strncmp(xi->name,"ctypes.",7)) return gen_ctypes_fn(f,e,xi);
            if(!strcmp(xi->name,"json.dumps")) return gen_json_dumps(f,e,xi);
            if(!strcmp(xi->name,"minipy.endpoint")){ gen_as(f,e->items[0],TY(e),1); return 1; }
            return gen_bmod(f,e,xi);
        case X_CCALL: return gen_ccall(f,e,xi);
        default: cg_fail(e->line,"unsupported call");
    }
}

/* ---- "%" formatting with a literal format string (also what f-strings with a spec become) ---- */
static void sb_bytes_lit(F *f, const char *s, int n){
    if(n<=0) return;
    if(n==1){ E(f,"mov al,%d",(unsigned char)s[0]); CALLRT(f,"rt_sb_char"); return; }
    E(f,"mov eax,RL%d",rlit(s,n)); E(f,"mov edx,%d",n); CALLRT(f,"rt_sb_bytes");
}
static void fmt_arg(F *f, Expr *a, Ty *tt, int tslot, int i, int as_float){
    if(tslot){ Ty *et=ty_find(tt)->elems[i]; E(f,"mov eax,[ebp%+d]",tslot); load_mem(f,et,"eax",tuple_off(tt,i)); if(as_float) conv_num(f,et,TY_FLOAT_T); return; }
    if(as_float) gen_as(f,a,TY_FLOAT_T,0); else gen_borrow(f,a);
}

/* ---- format specs: printf ("%-8.2f") and Python ("{:>8,.2f}", f-strings) ---- */
/* [[fill]align][sign][#][0][width][,][.precision][type], printf flags (- + space # 0) accepted too */
static int parse_spec(const char *s, int n, Spec *sp){
    memset(sp,0,sizeof *sp); sp->prec=-1;
    int i=0;
    unsigned b0=n?(unsigned char)s[0]:0; int fl= b0>=0xF0 ? 4 : b0>=0xE0 ? 3 : b0>=0xC0 ? 2 : 1;   /* the fill: one UTF-8 character */
    if(n>fl && s[fl] && strchr("<>^=",s[fl])){
        unsigned c= fl==1 ? b0 : b0&(0xFF>>(fl+1));
        for(int k=1;k<fl;k++) c=(c<<6)|((unsigned char)s[k]&0x3F);
        sp->fill=(int)c; sp->align=s[fl]; i=fl+1; }
    else if(n>=1 && s[0] && strchr("<>^=",s[0])){ sp->align=s[0]; i=1; }
    for(;i<n;i++){ char c=s[i];
        if(c=='-') sp->align='<'; else if(c=='+'||c==' ') sp->sign=c; else if(c=='#') sp->alt=1; else if(c=='0') sp->zero=1; else break; }
    if(i<n && s[i]=='\001'){ sp->wslot=-1; i++; }                        /* {w}: filled in by the caller */
    else for(;i<n && s[i]>='0' && s[i]<='9';i++) sp->width=sp->width*10+(s[i]-'0');
    if(i<n && (s[i]==','||s[i]=='_')){ sp->comma=s[i]; i++; }
    if(i<n && s[i]=='.'){ i++; sp->prec=0; if(i<n && s[i]=='\001'){ sp->pslot=-1; i++; } else for(;i<n && s[i]>='0' && s[i]<='9';i++) sp->prec=sp->prec*10+(s[i]-'0'); }
    if(i<n) sp->type=s[i++];
    return i;
}
static void src_load(F *f, ArgSrc *a, int as_float){ fmt_arg(f,a->e,a->tt,a->tslot,a->idx,as_float); }
/* Append one formatted value. pystyle: Python's defaults (strings left-aligned). */
static void emit_field(F *f, Spec *sp, Ty *t, ArgSrc *src, int repr, int pystyle, int line){
    t=ty_find(t);
    int num=t->k==TY_INT||t->k==TY_BOOL||t->k==TY_FLOAT;
    char ty=sp->type;
    if(!ty||ty=='n') ty = t->k==TY_FLOAT ? (sp->prec>=0?1:2) : (t->k==TY_INT||(t->k==TY_BOOL&&sp->width)) ? 'd' : 's';   /* 1: like g, keeps ".0"; 2: repr */
    int align=sp->align;
    if(!align) align = (pystyle && !num && ty=='s') ? '<' : '>';
    int padded=sp->width>0 || sp->wslot;
    if(padded) E(f,"push dword [rt_sb_len]");
    if(sp->sign && num && ty!='s') E(f,"push dword [rt_sb_len]");
    int prec=sp->prec, ps=sp->pslot;
    #define PREC(dflt,lo,hi,add) do{ if(ps){ int l1_=new_label(), l2_=new_label(); E(f,"mov edx,[ebp%+d]",ps); E(f,"cmp edx,%d",lo); E(f,"jge L%d",l1_); E(f,"mov edx,%d",lo); LBL(f,l1_); \
        E(f,"cmp edx,%d",hi); E(f,"jle L%d",l2_); E(f,"mov edx,%d",hi); LBL(f,l2_); if(add) E(f,"add edx,%d",add); } \
        else E(f,"mov edx,%d",(prec<0?(dflt):prec<(lo)?(lo):prec>(hi)?(hi):prec)+(add)); }while(0)
    int grp= sp->comma && num && ty!='s' && ty!='c';             /* 1,234,567 / 1_2345_6789 */
    if(grp) E(f,"push dword [rt_sb_len]");
    switch(ty){
        case 's': case 'r':
            if(prec>=0||ps) E(f,"push dword [rt_sb_len]");
            src_load(f,src,0); gen_fmt(f,t,repr||ty=='r');
            if(prec>=0||ps){ E(f,"pop eax"); if(ps) E(f,"mov edx,[ebp%+d]",ps); else E(f,"mov edx,%d",prec); CALLRT(f,"rt_sb_trunc"); }       /* "%.3s": the first 3 characters */
            break;
        case 'd': case 'i': case 'u':
            src_load(f,src,0); if(is_flt(t)) CALLRT(f,"rt_ftoi"); else if(!is_lng(t)) widen(f,0);
            CALLRT(f,"rt_sb_int"); break;
        case 'x': case 'X': case 'o': case 'b':{
            src_load(f,src,0); if(is_flt(t)) CALLRT(f,"rt_ftoi"); else if(!is_lng(t)) widen(f,0);
            if(sp->alt){ int l=new_label(); E(f,"test edx,edx"); E(f,"jns L%d",l); E(f,"push edx"); E(f,"push eax"); E(f,"mov al,'-'"); CALLRT(f,"rt_sb_char"); E(f,"pop eax"); E(f,"pop edx");
                E(f,"neg eax"); E(f,"adc edx,0"); E(f,"neg edx"); LBL(f,l);
                E(f,"push edx"); E(f,"push eax"); E(f,"mov al,'0'"); CALLRT(f,"rt_sb_char"); E(f,"mov al,'%c'",ty=='o'?'o':ty=='b'?'b':ty); CALLRT(f,"rt_sb_char"); E(f,"pop eax"); E(f,"pop edx"); }
            E(f,"mov ecx,%d",(ty=='o'?8:ty=='b'?2:16)|(ty=='X'?0x100:0)); CALLRT(f,"rt_sb_radix"); break; }
        case 'f': case 'F': case '%':
            src_load(f,src,1);
            if(ty=='%'){ E(f,"push 100"); E(f,"fimul dword [esp]"); E(f,"add esp,4"); }
            PREC(6,0,40,0); CALLRT(f,"rt_sb_fixed");
            if(ty=='%'){ E(f,"mov al,'%%'"); CALLRT(f,"rt_sb_char"); }
            break;
        case 'e': case 'E':
            src_load(f,src,1); PREC(6,0,16,1); E(f,"mov ecx,4"); CALLRT(f,"rt_sb_gen"); break;
        case 'g': case 'G': case 1:
            src_load(f,src,1); PREC(6,1,17,0); E(f,"mov ecx,%d",ty==1?1:0); CALLRT(f,"rt_sb_gen"); break;
        case 2: src_load(f,src,1); CALLRT(f,"rt_sb_float"); break;
        case 'c': src_load(f,src,0); to_i32(f,t); CALLRT(f,"rt_sb_cp"); break;
        default: cg_fail(line,"unsupported format type");
    }
    if(grp){ E(f,"pop eax"); E(f,"mov dl,'%c'",sp->comma); E(f,"mov ecx,%d",(ty=='x'||ty=='X'||ty=='o'||ty=='b')?4:3); CALLRT(f,"rt_sb_group"); }
    if(sp->sign && num && ty!='s'){ E(f,"pop eax"); E(f,"mov dl,'%c'",sp->sign); CALLRT(f,"rt_sb_sign"); }
    if(padded){                                                  /* see rt_sb_pad */
        unsigned mode= align=='<' ? 1 : align=='^' ? 2 : 0, fill=(unsigned)sp->fill;
        if(sp->zero && !sp->align && num){ mode|=4; fill='0'; }
        if(align=='=' && num) mode|=4;
        E(f,"pop eax"); if(sp->wslot) E(f,"mov ecx,[ebp%+d]",sp->wslot); else E(f,"mov ecx,%d",sp->width); E(f,"mov edx,0x%x",mode|fill<<8); CALLRT(f,"rt_sb_pad");
    }
}
static void gen_format_body(F *f, Expr *e){
    const char *fmt=e->a->tok->text; int pystyle=e->a->tok->i==1;
    if(pystyle) fmt+=strlen(fmt)+1;                          /* an f-string: the spec as written (see lex_fstring) */
    int len=(int)strlen(fmt);
    Expr **args=e->b->kind==EXPR_TUPLE?e->b->items:&e->b; int nargs=e->b->kind==EXPR_TUPLE?e->b->count:1, ai=0;
    Ty *tt=NULL; int tslot=0;
    if(e->b->kind!=EXPR_TUPLE && TY(e->b)->k==TY_TUPLE){          /* "%s=%d" % pair */
        tt=TY(e->b); nargs=tt->nelems; gen_borrow(f,e->b); tslot=frame_slot(f,4); E(f,"mov [ebp%+d],eax",tslot);
    }
    for(int i=0;i<len;){
        if(fmt[i]!='%'){ int j=i; while(j<len && fmt[j]!='%') j++; sb_bytes_lit(f,fmt+i,j-i); i=j; continue; }
        i++;
        if(i<len && fmt[i]=='%'){ E(f,"mov al,'%%'"); CALLRT(f,"rt_sb_char"); i++; continue; }
        Spec sp;
        if(pystyle){                                         /* f-string: "%" + the whole Python spec */
            if(parse_spec(fmt+i,len-i,&sp)!=len-i) cg_fail(e->line,"invalid format spec");
            i=len;
            int nx=0;                                          /* {w} / {p}: their values, now (into slots) */
            if(sp.wslot<0 || sp.pslot<0){ int *slots[2]; int ns=0; if(sp.wslot<0) slots[ns++]=&sp.wslot; if(sp.pslot<0) slots[ns++]=&sp.pslot;
                if(ns!=e->a->count) cg_fail(e->line,"a replacement field there is not supported in compiled code");
                for(int k=0;k<ns;k++){ *slots[k]=frame_slot(f,4); gen(f,e->a->items[k]); to_i32(f,TY(e->a->items[k])); E(f,"mov [ebp%+d],eax",*slots[k]); nx++; } }
            (void)nx;
        } else {
            int j=i;                                            /* printf: flags, width, precision, then the type letter */
            while(j<len && !((fmt[j]>='a'&&fmt[j]<='z')||(fmt[j]>='A'&&fmt[j]<='Z')||fmt[j]=='%')) j++;
            if(j>=len) cg_fail(e->line,"incomplete format");
            parse_spec(fmt+i,j+1-i,&sp); i=j+1;
        }
        if(ai>=nargs) cg_fail(e->line,"not enough arguments for the format string");
        int ix=ai++; ArgSrc src={tslot?NULL:args[ix],tt,tslot,ix};
        emit_field(f,&sp,tslot?ty_find(tt->elems[ix]):TY(args[ix]),&src,0,pystyle,e->line);
    }
    if(ai<nargs) cg_fail(e->line,"not all arguments converted during string formatting");
}
/* "{} and {name:>8}".format(a, name=b): the literal is parsed here */
static int gen_str_format(F *f, Expr *call){
    Expr *lit=call->a->a; const char *fmt=lit->tok->text; int len=(int)strlen(fmt), auto_ix=0;
    rt("rt_sb_need"); E(f,"push dword [rt_sb_len]");
    for(int i=0;i<len;){
        if(fmt[i]=='{' && i+1<len && fmt[i+1]=='{'){ sb_bytes_lit(f,"{",1); i+=2; continue; }
        if(fmt[i]=='}' && i+1<len && fmt[i+1]=='}'){ sb_bytes_lit(f,"}",1); i+=2; continue; }
        if(fmt[i]!='{'){ int j=i; while(j<len && fmt[j]!='{' && fmt[j]!='}') j++; if(j==i) j++; sb_bytes_lit(f,fmt+i,j-i); i=j; continue; }
        int j=i+1; while(j<len && fmt[j]!='}') j++;
        if(j>=len) cg_fail(call->line,"unmatched '{' in the format string");
        const char *fld=fmt+i+1; int flen=j-i-1; i=j+1;
        int nl=0; while(nl<flen && fld[nl]!='!' && fld[nl]!=':') nl++;
        int repr=0; const char *spec=""; int slen=0;
        if(nl<flen && fld[nl]=='!'){ repr=fld[nl+1]=='r'; nl+=0; }
        { const char *c=memchr(fld,':',(size_t)flen); if(c){ spec=c+1; slen=flen-(int)(c+1-fld); } }
        Expr *arg=NULL;
        if(nl==0) arg = auto_ix<call->count && !call->items[auto_ix]->akind ? call->items[auto_ix] : NULL, auto_ix++;
        else if(fld[0]>='0'&&fld[0]<='9'){ int k=atoi(fld); if(k<call->count && !call->items[k]->akind) arg=call->items[k]; }
        else for(int k=0;k<call->count;k++) if(call->items[k]->akind==3 && (int)strlen(call->items[k]->kw)==nl && !strncmp(call->items[k]->kw,fld,(size_t)nl)) arg=call->items[k];
        if(!arg) cg_fail(call->line,"format() field without a matching argument");
        Spec sp; if(parse_spec(spec,slen,&sp)!=slen) cg_fail(call->line,"invalid format spec");
        ArgSrc src={arg,NULL,0,0};
        emit_field(f,&sp,TY(arg),&src,repr,1,call->line);
    }
    E(f,"pop eax"); CALLRT(f,"rt_sb_take");
    return 1;
}
static int gen_format(F *f, Expr *e){
    rt("rt_sb_need"); E(f,"push dword [rt_sb_len]");
    gen_format_body(f,e);
    E(f,"pop eax"); CALLRT(f,"rt_sb_take");
    return 1;
}

static int gen_await(F *f, Expr *e);
/* a closure of g made here: its captured values (or cells) copied from this function -> eax (owned) */
static void fill_defaults(F *f, AFunc *g, const char *lbl);
static int gen_closure(F *f, AFunc *g){
    layout_caps(g); use_fn(g); gg->cd_used[g->id]=1;
    E(f,"mov eax,%d",16+g->defsize+g->capsize); CALLRT(f,"rt_alloc");
    E(f,"mov dword [eax],1"); E(f,"mov dword [eax+4],%s",fnl(g,LB_FREE)); E(f,"mov dword [eax+8],%s",fnl(g,LB_CODE)); E(f,"mov dword [eax+12],S%d",str_lit(fn_display_name(g)));
    E(f,"push eax");
    fill_defaults(f,g,NULL);                                /* its defaults, evaluated now */
    for(int k=0;k<g->ncaps;k++){ AVar *cv=g->caps[k], *src=cv->src;
        if(root_var(cv)->cell){                             /* share the cell */
            if(src->src){ layout_caps(src->owner); E(f,"mov ecx,[ebp%+d]",f->env); E(f,"mov eax,[ecx+%d]",src->capoff); }
            else E(f,"mov eax,[ebp%+d]",src->offset);
            incref(f); E(f,"mov ecx,[esp]"); E(f,"mov [ecx+%d],eax",cv->capoff);
        } else {
            load_var(f,src); if(is_ptr(cv->ty)) incref(f);
            E(f,"mov ecx,[esp]"); store_new(f,cv->ty,"ecx",cv->capoff);
        }
    }
    E(f,"pop eax");
    return 1;
}
/* g's default slots in its function object ([esp], or the static object lbl): its defaults, evaluated here */
static void fill_defaults(F *f, AFunc *g, const char *lbl){
    layout_defaults(g);
    for(int i=0;i<g->nparams;i++){ Expr *d=g->defaults[i]; if(!d || i==g->star || i==g->dstar) continue;
        if(i<32 && (g->defaults_mismatch>>i&1)) continue;        /* (a generic copy's constant default of another type: calls put it in) */
        if(!d->ty && g->origin && i<g->origin->nparams && g->origin->defaults[i] && g->origin->defaults[i]->ty) d=g->origin->defaults[i];   /* (a copy's unchecked one: its origin's) */
        if(!d->ty) continue;
        Ty *pt=g->params[i]->ty;
        gen_as(f,d,pt,1);
        if(lbl) E(f,"mov ecx,%s",lbl); else E(f,"mov ecx,[esp]");
        store_mem(f,pt,"ecx",16+g->defoff[i]); }
}
/* function g as a value -> eax */
static int gen_funcref(F *f, AFunc *g){
    use_fn(g);
    if(!g->ncaps && !dyn_defaults(g)){ gg->fv_used[g->id]=1; rt("rt_static");    /* nothing captured: a static object */
        if(has_defaults(g)) fill_defaults(f,g,fnl(g,LB_VALUE));
        E(f,"mov eax,%s",fnl(g,LB_VALUE)); return 0; }
    if(g==f->fn){ E(f,"mov eax,[ebp%+d]",f->env); return 0; }                                    /* a nested def naming itself */
    return gen_closure(f,g);
}
/* the constant n as a value of type t (int: edx:eax) */
static void gen_int_const(F *f, Ty *t, int64_t n){
    uint32_t lo=(uint32_t)n, hi=(uint32_t)((uint64_t)n>>32);
    if(lo==0) E(f,"xor eax,eax"); else E(f,"mov eax,%u",lo);
    if(is_lng(t)){ if(hi==0) E(f,"xor edx,edx"); else if(hi==0xFFFFFFFFu) E(f,"or edx,-1"); else E(f,"mov edx,%u",hi); }
}
static int gen(F *f, Expr *e){
    XInfo *xi=xinfo(e); Ty *t=TY(e);
    switch(e->kind){
        case EXPR_LITERAL:{
            Tok *tk=e->tok;
            if(tk->kind==T_STRING && tk->i==2){ E(f,"mov eax,B%d",bytes_lit(tk->text,(int)tk->len)); return 0; }
            if(tk->kind==T_STRING){ E(f,"mov eax,S%d",str_litn(tk->text,(int)tk->len)); return 0; }      /* "" too: a str (0 is None) */
            if(tk->is_float){ double d=tk->f; if(d==0.0) E(f,"fldz"); else if(d==1.0) E(f,"fld1"); else E(f,"fld qword [FC%d]",float_const(tk->text)); return 0; }
            if(t->k==TY_FLOAT){ char num[32]; snprintf(num,sizeof num,"%lld",(long long)tk->i);
                if(tk->i==0) E(f,"fldz"); else if(tk->i==1) E(f,"fld1"); else E(f,"fld qword [FC%d]",float_const(num)); return 0; }
            gen_int_const(f,t,tk->i);
            return 0; }
        case EXPR_TRUE: E(f,"mov eax,1"); return 0;
        case EXPR_FALSE: E(f,"xor eax,eax"); return 0;
        case EXPR_NONE: gen_none(f,t); return 0;
        case EXPR_NAME:
            if(xi->kind==X_CONST_STR){ E(f,"mov eax,S%d",str_lit(xi->name)); return 0; }
            if(xi->kind==X_FUNCREF) return gen_funcref(f,xi->fn);
            if(xi->kind==X_TYPEVAL){ E(f,"mov eax,%s",typeval_label(xi)); return 0; }
            load_var(f,xi->var); return 0;
        case EXPR_LAMBDA: return gen_funcref(f,xi->fn);
        case EXPR_YIELD:{                                 /* (yield v): what send() gives; (yield from g): what g returns */
            Ty *yt=f->fn->yield_ty;
            if(e->akind==7){
                if(ty_find(TY(e->a))->k==TY_GEN) return gen_yield_from(f,e->a,ty_find(t)->k!=TY_VOID);
                Iter I; iter_begin(f,e->a,&I);
                iter_value(f,&I,0); if(is_ptr(I.elem)) incref(f); conv(f,I.elem,yt);
                gen_yield_value(f,yt); drop_sent(f);
                iter_end(f,&I); LBL(f,I.exit); iter_release(f,&I);
                return 0; }
            if(e->a) gen_as(f,e->a,yt,1); else { int o=gen_zero(f,yt); if(is_ptr(yt)&&!o) incref(f); }
            gen_yield_value(f,yt);
            return take_sent(f,gpart(f->fn->ret,0)); }
        case EXPR_UNARY:
            if(e->op==T_NOT){ gen_bool(f,e->a); E(f,"xor eax,1"); return 0; }
            if(xi->kind==X_OPMETHOD) return gen_op_call(f,xi->fn,e->a,NULL);
            if(e->op==T_MINUS && is_lng(t) && e->a->kind==EXPR_LITERAL && !e->a->tok->is_float && e->a->tok->kind==T_NUMBER){   /* -5 */
                gen_int_const(f,t,-e->a->tok->i); return 0; }
            g_none_text=none_msg("bad operand type for unary %s: 'NoneType'",e->op==T_MINUS?"-":e->op==T_PLUS?"+":"~");
            gen_as(f,e->a,t,0); g_none_text=NULL;
            if(e->op==T_PLUS) return 0;
            if(e->op==T_MINUS){ if(is_flt(t)) E(f,"fchs"); else if(is_lng(t)) CALLRT(f,"rt_ineg"); else E(f,"neg eax"); }
            else { E(f,"not eax"); if(is_lng(t)) E(f,"not edx"); }
            return 0;
        case EXPR_BINARY: return gen_binary(f,e);
        case EXPR_BOOL: return gen_boolop(f,e);
        case EXPR_COMPARE: gen_compare(f,e); return 0;
        case EXPR_TERNARY:{
            int lelse=new_label(), lend=new_label();
            gen_jump(f,e->a,lelse,0);
            gen_as(f,e->b,t,1); E(f,"jmp L%d",lend);
            LBL(f,lelse); gen_as(f,e->c,t,1);
            LBL(f,lend);
            return 1; }
        case EXPR_CALL: return gen_call(f,e);
        case EXPR_ATTRIBUTE:
            if(xi->kind==X_CONST_STR){ E(f,"mov eax,S%d",str_lit(xi->name)); return 0; }
            if(xi->kind==X_BUILTIN && !strcmp(xi->name,"type")){ gen_type_of(f,e->a); return 0; }        /* x.__class__ */
            if(xi->kind==X_BUILTIN && !strcmp(xi->name,"class_mro")){            /* C.__mro__ */
                AClass *k=xi->cls, *ks[64]; int n=0;
                if(k->mro) for(int i=0;i<k->nmro && n<64;i++) ks[n++]=k->mro[i]; else for(AClass *x=k;x && n<64;x=x->base) ks[n++]=x;
                Ty *el=ty_find(t)->elem; new_list(f,el); E(f,"push eax");
                for(int i=0;i<=n;i++){ if(i<n) use_class(ks[i]); E(f,"mov eax,%s",i<n?class_label(ks[i],0):btype_label("object")); list_append_top(f,el); }
                E(f,"pop eax"); return 1; }
            if(xi->kind==X_BUILTIN && !strcmp(xi->name,"type_qualname") && xinfo(e->a)->kind==X_TYPEVAL && xinfo(e->a)->cls){   /* C.__qualname__: Outer.Inner */
                E(f,"mov eax,S%d",str_lit(xinfo(e->a)->cls->qualname)); return 0; }
            if(xi->kind==X_BUILTIN && !strcmp(xi->name,"type_qualname")){ gen(f,e->a); CALLRT(f,"rt_type_qualname"); return 0; }   /* type(x).__qualname__ */
            if(xi->kind==X_BUILTIN && !strcmp(xi->name,"type_name")){ gen(f,e->a); E(f,"mov eax,[eax]"); return 0; }   /* C.__name__ (static) */
            if(xi->kind==X_BUILTIN && !strcmp(xi->name,"type_module")){ gen(f,e->a); E(f,"mov eax,[eax-8]"); return 0; }   /* C.__module__ (static) */
            if(xi->kind==X_BUILTIN && !strcmp(xi->name,"exc_args")){            /* e.args: (message,) or () */
                AField *fm=aot_find_field(ty_find(TY(e->a))->cls,"_msg"); int l=new_label();
                gen_borrow(f,e->a); E(f,"push dword [eax+%d]",fm->offset);
                new_list(f,TY_STR_T); E(f,"pop ecx"); E(f,"test ecx,ecx"); E(f,"jz L%d",l);
                E(f,"push eax");
                if(is_keyerror(ty_find(TY(e->a))->cls)){ E(f,"mov eax,ecx"); CALLRT(f,"rt_str_unquote"); E(f,"mov ecx,eax"); }   /* (a KeyError keeps repr(key)) */
                else E(f,"inc dword [ecx]");
                E(f,"push ecx"); E(f,"mov eax,[esp+4]"); E(f,"call rt_list_push"); E(f,"pop ecx"); E(f,"mov [eax],ecx"); E(f,"pop eax"); rt("rt_list_push");
                LBL(f,l); return 1; }
            if(xi->kind==X_BMOD && (!strcmp(xi->name,"inf")||!strcmp(xi->name,"nan"))){ rt("rt_finf"); E(f,"fld qword [%s]",xi->name[0]=='i'?"rt_finf":"rt_fnan"); return 0; }
            if(xi->kind==X_BMOD){                         /* math.pi / math.e / math.tau */
                E(f,"fld qword [FC%d]",float_const(!strcmp(xi->name,"pi")?"3.141592653589793":!strcmp(xi->name,"tau")?"6.283185307179586":"2.718281828459045"));
                return 0; }
            if(xi->kind==X_VAR){ load_var(f,xi->var); return 0; }
            if(xi->kind==X_FUNCREF) return gen_funcref(f,xi->fn);
            if(xi->kind==X_TYPEVAL){ E(f,"mov eax,%s",typeval_label(xi)); return 0; }      /* Outer.Inner */
            if(xi->kind==X_BOUND){                        /* obj.method: a closure of obj (+16) */
                AFunc *m=xi->fn; use_fn(m); gg->bm_used[m->id]=1;
                gen_owned(f,e->a); attr_check(f,e->name); E(f,"push eax");
                E(f,"mov eax,20"); CALLRT(f,"rt_alloc");
                E(f,"mov dword [eax],1"); E(f,"mov dword [eax+4],ADFREE"); E(f,"mov dword [eax+8],%s",fnl(m,LB_BOUND)); E(f,"mov dword [eax+12],S%d",str_lit(m->name));
                E(f,"pop ecx"); E(f,"mov [eax+16],ecx");
                return 1; }
            if(xi->kind==X_PROP){ E(f,"sub esp,4"); gen_borrow(f,e->a); E(f,"mov [esp],eax"); call_on_top(f,xi->fn); E(f,"add esp,4"); return is_ptr(xi->fn->ret); }
            if(xi->kind==X_CLASSCONST){ gen_as(f,xi->field->init,xi->field->ty,0); return 0; }
            gen_borrow(f,e->a); attr_check(f,e->name);
            load_field(f,xi->field);
            return 0;
        case EXPR_INDEX: return gen_index(f,e);
        case EXPR_SLICE: return gen_slice(f,e);
        case EXPR_DICT: if(t->k==TY_TUPLE) return gen_tuple(f,e); return gen_literal_container(f,e);
        case EXPR_LIST: case EXPR_SET: return gen_literal_container(f,e);
        case EXPR_TUPLE: return t->k==TY_TUPLE ? gen_tuple(f,e) : gen_literal_container(f,e);
        case EXPR_COMPREHENSION:
            if(e->comp_kind=='G'){                       /* a generator expression: call its generator function with the first iterable */
                AFunc *g=xi->fn; use_fn(g);
                if(g->nparams){ Ty *pt=g->params[0]->ty; E(f,"sub esp,%d",esize(pt)); gen_as(f,e->clauses[0].iter,pt,0); put_arg(f,pt,0); }
                if(g->ncaps){ gen_closure(f,g); hold(f); E(f,"mov edx,eax"); }
                E(f,"call %s",fnl(g,LB_CODE));
                if(g->nparams) E(f,"add esp,%d",esize(g->params[0]->ty));
                return 1;
            }
            return gen_comprehension(f,e);
        case EXPR_AWAIT: return gen_await(f,e);
        case EXPR_WALRUS:{                                       /* name := value: stored, and a reference kept as the result */
            AVar *v=xi->var;
            gen_as(f,e->a,v->ty,1);
            if(is_flt(v->ty)){ E(f,"fld st0"); store_var(f,v); return 0; }
            if(is_ptr(v->ty)){ incref(f); E(f,"push eax"); store_var(f,v); E(f,"pop eax"); return 1; }
            store_var(f,v); return 0; }
        default: cg_fail(e->line,"unsupported expression");
    }
}

/* None as a value of type t (returns 0: nothing owned) */
static int gen_zero(F *f, Ty *t){ gen_none(f,t); return 0; }

/* ---------------------------------------------------------------- string building */

static int is_str_lit(Expr *e){ return e->kind==EXPR_LITERAL && e->tok->kind==T_STRING && e->tok->i!=2; }   /* (not b"...") */
static int is_str_call(Expr *e){ if(e->kind!=EXPR_CALL||e->count!=1) return 0; XInfo *xi=xinfo(e); return xi->kind==X_BUILTIN && !strcmp(xi->name,"str"); }
static int is_fmt_expr(Expr *e){ return e->kind==EXPR_BINARY && e->op==T_PERCENT && is_str_lit(e->a) && TY(e)->k==TY_STR; }
static int is_concat(Expr *e){ return e->kind==EXPR_BINARY && e->op==T_PLUS && TY(e)->k==TY_STR && !is_opt(TY(e->a)) && !is_opt(TY(e->b)); }
static int concat_parts(Expr *e, int *special){
    if(is_concat(e)) return concat_parts(e->a,special)+concat_parts(e->b,special);
    if(is_str_call(e)||is_fmt_expr(e)) *special=1;
    return 1;
}
static int concat_worth(Expr *e){ int sp=0; int n=concat_parts(e,&sp); return n>=3||sp; }

/* append str(x) to the string builder */
static void gen_into_sb(F *f, Expr *x){
    if(is_str_lit(x)){ sb_bytes_lit(f,x->tok->text,(int)x->tok->len); return; }
    if(is_concat(x)){ gen_into_sb(f,x->a); gen_into_sb(f,x->b); return; }
    if(is_fmt_expr(x)){ gen_format_body(f,x); return; }
    if(is_str_call(x)){ gen_into_sb(f,x->items[0]); return; }
    gen_borrow(f,x); gen_fmt(f,TY(x),0);
}
static int gen_concat(F *f, Expr *e){
    rt("rt_sb_need"); E(f,"push dword [rt_sb_len]");
    gen_into_sb(f,e);
    E(f,"pop eax"); CALLRT(f,"rt_sb_take");
    return 1;
}
/* eax = str(x) (owned or borrowed: returns owned) */
static int gen_str_of(F *f, Expr *x){
    if(TY(x)->k==TY_STR && !TY(x)->opt) return gen(f,x);
    return gen_concat(f,x);
}

/* formatters of containers and objects: FMT<i>, eax = value */
static int fmt_index(Ty *t, int repr){
    t=ty_find(t);
    if(t->k!=TY_OBJ && repr<2) repr=1;
    for(int i=0;i<gg->nfmts;i++) if(gg->fmts[i].repr==repr && ty_same_exact(gg->fmts[i].ty,t)) return i;
    if(gg->nfmts==gg->cfmts){ gg->cfmts=gg->cfmts?gg->cfmts*2:16; gg->fmts=(Fmt*)xrealloc(gg->fmts,sizeof(Fmt)*(size_t)gg->cfmts); }
    gg->fmts[gg->nfmts].ty=t; gg->fmts[gg->nfmts].repr=repr;
    return gg->nfmts++;
}
/* KDC<id>: the descriptor of a class's objects in containers (see kd_of):
   ==, ordering and hash through __eq__, __lt__ and __hash__ when the class has
   them (as CPython: __eq__ without __hash__ makes it unhashable), else identity */
static void kd_call_method(Buf *o, AFunc *m){         /* the arguments are on the stack, self first */
    if(m->overridden) buf_printf(o,"        mov eax,[esp]\n        mov eax,[eax+8]\n        call dword [eax+%d]\n",8+4*m->vslot);
    else buf_printf(o,"        call %s\n",fnl(m,LB_CODE));
}
static void kd_truth(Buf *o, Ty *r){                  /* eax = truth of a method's result */
    if(ty_find(r)->k!=TY_BOOL) buf_printf(o,"        test eax,eax\n        setne al\n        movzx eax,al\n");
}
static int kd_method_ok(AFunc *m, int nparams){
    if(!m || m->is_static || m->nparams<nparams) return 0;
    for(int i=1;i<nparams;i++) if(!is_ptr(m->params[i]->ty) || is_flt(m->params[i]->ty)) return 0;
    for(int i=nparams;i<m->nparams;i++)                  /* (more parameters: pointers left None - __lt__(self, other, context=None)) */
        if(!m->defaults[i] || m->defaults[i]->kind!=EXPR_NONE || !is_ptr(m->params[i]->ty) || i==m->star || i==m->dstar) return 0;
    return 1;
}
/* the None arguments of a kind method's parameters after the first n (pushed before the others) */
static const char *kd_nones(AFunc *m, int n){
    static char b[4][160]; static int k; char *o=b[k++&3]; o[0]=0;
    for(int i=n;i<m->nparams && i<16;i++) strcat(o,"        push 0\n");
    return o;
}
static const char *class_kd(AClass *c){
    if(!gg->kdlabels){ gg->kdlabels=(char**)xmalloc(sizeof(char*)*(size_t)(gg->p->nclasses+1)); memset(gg->kdlabels,0,sizeof(char*)*(size_t)(gg->p->nclasses+1)); }
    if(gg->kdlabels[c->id]) return gg->kdlabels[c->id];
    char lab[64]; snprintf(lab,sizeof lab,"KDC%d",c->id);
    gg->kdlabels[c->id]=xstrdup2(lab);
    use_class(c);
    Buf *o=&gg->text;
    AFunc *eq=c->eq_inst?c->eq_inst:aot_find_method(c,"__eq__"), *lt=c->lt_inst?c->lt_inst:aot_find_method(c,"__lt__"), *hs=aot_find_method(c,"__hash__");
    if(!kd_method_ok(eq,2)) eq=NULL;
    if(!kd_method_ok(lt,2)) lt=NULL;
    if(!kd_method_ok(hs,1) || !(ty_find(hs->ret)->k==TY_INT||ty_find(hs->ret)->k==TY_BOOL)) hs=NULL;
    char eqf[64]="rt_eq_w", cmpf[64]="rt_cmp_no", hashf[64]="rt_hash_p";
    if(eq){ use_fn(eq); snprintf(eqf,sizeof eqf,"KDC%d_eq",c->id);
        int lid=++gg->labels;
        buf_printf(o,"\n%s:                       ; %s.__eq__ for containers (None: identity)\n        mov ecx,[eax]\n        mov eax,[edx]\n        test ecx,ecx\n        jz L%d\n        test eax,eax\n        jz L%d\n%s        push eax\n        push ecx\n",eqf,c->name,lid,lid,kd_nones(eq,2));
        kd_call_method(o,eq); buf_printf(o,"        add esp,%d\n",4*eq->nparams); kd_truth(o,eq->ret);
        buf_printf(o,"        ret\nL%d:\n        cmp ecx,eax\n        sete al\n        movzx eax,al\n        ret\n",lid); }
    if(lt){ use_fn(lt); snprintf(cmpf,sizeof cmpf,"KDC%d_cmp",c->id);
        int l1=++gg->labels, l2=++gg->labels;
        buf_printf(o,"\n%s:                       ; ordering by %s.__lt__\n        push ebx\n        push esi\n        mov ebx,[eax]\n        mov esi,[edx]\n%s        push esi\n        push ebx\n",cmpf,c->name,kd_nones(lt,2));
        kd_call_method(o,lt); buf_printf(o,"        add esp,%d\n",4*lt->nparams); kd_truth(o,lt->ret);
        buf_printf(o,"        test eax,eax\n        jz L%d\n        or eax,-1\n        jmp L%d\nL%d:\n%s        push ebx\n        push esi\n",l1,l2,l1,kd_nones(lt,2));
        kd_call_method(o,lt); buf_printf(o,"        add esp,%d\n",4*lt->nparams); kd_truth(o,lt->ret);
        buf_printf(o,"L%d:\n        pop esi\n        pop ebx\n        ret\n",l2); }
    if(hs){ use_fn(hs); snprintf(hashf,sizeof hashf,"KDC%d_hash",c->id);
        buf_printf(o,"\n%s:                       ; %s.__hash__\n%s        push dword [eax]\n",hashf,c->name,kd_nones(hs,1));
        kd_call_method(o,hs); buf_printf(o,"        add esp,%d\n%s        jmp rt_hash_int\n",4*hs->nparams,ty_find(hs->ret)->k==TY_BOOL?"        cdq\n":""); rt("rt_hash_int"); }   /* (an int: all 64 bits in edx:eax) */
    else if(eq) snprintf(hashf,sizeof hashf,"rt_hash_no");
    if(!eq) rt("rt_eq_w");
    if(!lt) rt("rt_cmp_no");
    if(!hs) rt(hashf);
    int fi=fmt_index(ty_new(TY_OBJ,NULL,c),1); rt("rt_sb_need");
    buf_printf(o,"\nKDC%d_repr:                     ; repr of a %s\n        mov eax,[eax]\n        jmp FMT%d\n",c->id,c->name,fi);
    buf_printf(&gg->data,"align 4\nKDC%d dd 4,1,%s,%s,%s,KDC%d_repr,Z%d   ; objects of %s\n",c->id,eqf,cmpf,hashf,c->id,zlit(c->name),c->name);
    return gg->kdlabels[c->id];
}
/* append str()/repr() of the value in eax/st0 (borrowed) to the string builder */
static void gen_fmt(F *f, Ty *t, int repr){
    t=ty_find(t);
    if(t->opt && (!is_ptr(t) || t->k==TY_STR || t->k==TY_BYTES)){             /* None prints as None (other references: their formatters) */
        int lnone=new_label(), lend=new_label();
        jump_if_none(f,t,lnone);
        Ty plain=*t; plain.opt=0; gen_fmt(f,&plain,repr); E(f,"jmp L%d",lend);
        LBL(f,lnone); drop_value(f,t,0); E(f,"mov eax,Z%d",zlit("None")); CALLRT(f,"rt_sb_cstr");
        LBL(f,lend); return; }
    switch(t->k){
        case TY_INT: CALLRT(f,"rt_sb_int"); return;
        case TY_BOOL: CALLRT(f,"rt_sb_bool"); return;
        case TY_FLOAT: CALLRT(f,"rt_sb_float"); return;
        case TY_STR: CALLRT(f,repr?"rt_sb_repr_str":"rt_sb_str"); return;
        case TY_BYTES: CALLRT(f,"rt_sb_repr_bytes"); return;
        case TY_TYPE: CALLRT(f,"rt_sb_type"); return;
        case TY_BUF: E(f,"mov eax,Z%d",zlit("<buffer>")); CALLRT(f,"rt_sb_cstr"); return;
        case TY_TASK: E(f,"mov eax,Z%d",zlit("<Task>")); CALLRT(f,"rt_sb_cstr"); return;
        case TY_FILE: E(f,"mov eax,Z%d",zlit("<file>")); CALLRT(f,"rt_sb_cstr"); return;
        case TY_FUNC: CALLRT(f,"rt_sb_funcval"); return;
        case TY_GEN: E(f,"mov eax,Z%d",zlit("<generator object>")); CALLRT(f,"rt_sb_cstr"); return;
        case TY_LIST: case TY_SET: case TY_DICT: case TY_OBJ: case TY_TUPLE:
            if(t->k==TY_LIST && t->view){                                  /* a dict's view: dict_items([...]) */
                static const char *const pre[]={"","dict_keys(","dict_values(","dict_items("};
                Ty *plain=(Ty*)xmalloc(sizeof(Ty)); *plain=*t; plain->view=0;
                E(f,"push eax"); E(f,"mov eax,Z%d",zlit(pre[t->view])); CALLRT(f,"rt_sb_cstr"); E(f,"pop eax");
                rt("rt_sb_need"); E(f,"call FMT%d",fmt_index(plain,repr)); E(f,"mov eax,Z%d",zlit(")")); CALLRT(f,"rt_sb_cstr"); return; }
            if(t->k==TY_SET && t->tup){                                    /* frozenset({...}), frozenset() */
                Ty *plain=(Ty*)xmalloc(sizeof(Ty)); *plain=*t; plain->tup=0; int le=new_label(), ld=new_label();
                E(f,"cmp dword [eax+8],0"); E(f,"je L%d",le);
                E(f,"push eax"); E(f,"mov eax,Z%d",zlit("frozenset(")); CALLRT(f,"rt_sb_cstr"); E(f,"pop eax");
                rt("rt_sb_need"); E(f,"call FMT%d",fmt_index(plain,repr)); E(f,"mov eax,Z%d",zlit(")")); CALLRT(f,"rt_sb_cstr"); E(f,"jmp L%d",ld);
                LBL(f,le); E(f,"mov eax,Z%d",zlit("frozenset()")); CALLRT(f,"rt_sb_cstr"); LBL(f,ld); return; }
            rt("rt_sb_need"); E(f,"call FMT%d",fmt_index(t,repr)); return;
        default: E(f,"mov eax,Z%d",zlit("None")); CALLRT(f,"rt_sb_cstr"); return;     /* a bare None */
    }
}
/* ---- json.dumps: JSON writers by type (JSON formatters FMT<i> have repr 2+variant) */
static int json_variant(const char *isep, const char *ksep, int ascii){
    for(int i=0;i<gg->njvars;i++) if(!strcmp(gg->jvars[i].isep,isep) && !strcmp(gg->jvars[i].ksep,ksep) && gg->jvars[i].ascii==ascii) return i;
    if(gg->njvars==gg->cjvars){ gg->cjvars=gg->cjvars?gg->cjvars*2:4; gg->jvars=xrealloc(gg->jvars,sizeof(*gg->jvars)*(size_t)gg->cjvars); }
    gg->jvars[gg->njvars].isep=isep; gg->jvars[gg->njvars].ksep=ksep; gg->jvars[gg->njvars].ascii=ascii;
    return gg->njvars++;
}
static void sb_cstr_lit(F *f, const char *s){ E(f,"mov eax,Z%d",zlit(s)); CALLRT(f,"rt_sb_cstr"); }
/* append the JSON text of the value in eax/st0 (borrowed) */
static void gen_json(F *f, Ty *t, int v){
    t=ty_find(t);
    if(t->opt && (!is_ptr(t) || t->k==TY_STR)){             /* None: null */
        int lnone=new_label(), lend=new_label();
        jump_if_none(f,t,lnone);
        Ty plain=*t; plain.opt=0; gen_json(f,&plain,v); E(f,"jmp L%d",lend);
        LBL(f,lnone); drop_value(f,t,0); sb_cstr_lit(f,"null");
        LBL(f,lend); return; }
    switch(t->k){
        case TY_INT: CALLRT(f,"rt_sb_int"); return;
        case TY_BOOL:{ int l=new_label(); E(f,"mov ecx,Z%d",zlit("true")); E(f,"test eax,eax"); E(f,"jnz L%d",l); E(f,"mov ecx,Z%d",zlit("false")); LBL(f,l); E(f,"mov eax,ecx"); CALLRT(f,"rt_sb_cstr"); return; }
        case TY_FLOAT: CALLRT(f,"rt_sb_jfloat"); return;
        case TY_STR: E(f,"mov edx,%d",gg->jvars[v].ascii); CALLRT(f,"rt_sb_json_str"); return;
        case TY_LIST: case TY_DICT: case TY_OBJ: case TY_TUPLE: rt("rt_sb_need"); E(f,"call FMT%d",fmt_index(t,2+v)); return;
        default: sb_cstr_lit(f,"null"); return;
    }
}
static void emit_json_formatter(int i){
    F ff; memset(&ff,0,sizeof ff); F *f=&ff;
    Ty *t=gg->fmts[i].ty; int v=gg->fmts[i].repr-2;
    const char *isep=gg->jvars[v].isep, *ksep=gg->jvars[v].ksep;
    buf_printf(&f->code,"\nFMT%d:                         ; JSON of %s\n",i,ty_name(t));
    int lnull=new_label();
    E(f,"test eax,eax"); E(f,"jz L%d",lnull);
    if(t->k==TY_OBJ || (t->k==TY_TUPLE && t->names)){        /* {"field": value, ...} */
        E(f,"push eax"); E(f,"mov al,'{'"); CALLRT(f,"rt_sb_char");
        AField *fl[256]; int nf=0;
        if(t->k==TY_OBJ){ AClass *chain[32]; int nc=0; for(AClass *k=t->cls;k && nc<32;k=k->base) chain[nc++]=k;
            for(int c=nc-1;c>=0;c--) for(int k=0;k<chain[c]->nfields && nf<256;k++){ AField *fd=chain[c]->fields[k];
                if(fd->name[0]!='_' && !fd->over && !aot_field_shared(fd) && !(fd->cvar && !aot_field_root(fd)->inst_set)) fl[nf++]=fd; } }
        int n=t->k==TY_OBJ?nf:t->nelems;
        for(int k=0;k<n;k++){
            const char *name=t->k==TY_OBJ?fl[k]->name:t->names[k];
            Ty *ft=t->k==TY_OBJ?fl[k]->ty:t->elems[k]; int off=t->k==TY_OBJ?fl[k]->offset:tuple_off(t,k);
            char key[400]; snprintf(key,sizeof key,"%s\"%s\"%s",k?isep:"",name,ksep);
            sb_cstr_lit(f,key);
            E(f,"mov eax,[esp]"); load_mem(f,ft,"eax",off); gen_json(f,ft,v);
        }
        E(f,"pop eax"); E(f,"mov al,'}'"); E(f,"jmp rt_sb_char");
    } else if(t->k==TY_TUPLE){                                 /* a tuple: a JSON array */
        E(f,"push eax"); E(f,"mov al,'['"); CALLRT(f,"rt_sb_char");
        for(int k=0;k<t->nelems;k++){
            if(k) sb_cstr_lit(f,isep);
            E(f,"mov eax,[esp]"); load_mem(f,t->elems[k],"eax",tuple_off(t,k)); gen_json(f,t->elems[k],v);
        }
        E(f,"pop eax"); E(f,"mov al,']'"); E(f,"jmp rt_sb_char");
    } else {                                                   /* list / dict */
        int ltop=new_label(), lend=new_label(), lskip=new_label(), isdict=t->k==TY_DICT;
        Ty *el=t->elem;
        E(f,"push ebx"); E(f,"push esi"); E(f,"mov ebx,eax");
        E(f,"mov al,'%c'",isdict?'{':'['); CALLRT(f,"rt_sb_char");
        E(f,"xor esi,esi");
        LBL(f,ltop);
        E(f,"cmp esi,[ebx+8]"); E(f,"jae L%d",lend);
        E(f,"test esi,esi"); E(f,"jz L%d",lskip); sb_cstr_lit(f,isep); LBL(f,lskip);
        if(isdict){
            Ty *kt=ty_find(ty_dkey(t));
            if(kt->k==TY_STR){ E(f,"mov eax,[ebx+16]"); load_at(f,kt,"eax","esi",8); E(f,"mov edx,%d",gg->jvars[v].ascii); CALLRT(f,"rt_sb_json_str"); }
            else { E(f,"mov al,'\"'"); CALLRT(f,"rt_sb_char");
                E(f,"mov eax,[ebx+16]"); load_at(f,kt,"eax","esi",8);
                gen_json(f,kt,v); E(f,"mov al,'\"'"); CALLRT(f,"rt_sb_char"); }
            sb_cstr_lit(f,ksep);
            E(f,"mov eax,[ebx+20]");
        } else E(f,"mov eax,[ebx+16]");
        load_at(f,el,"eax","esi",esize(el));
        gen_json(f,el,v);
        E(f,"inc esi"); E(f,"jmp L%d",ltop);
        LBL(f,lend); E(f,"mov al,'%c'",isdict?'}':']'); CALLRT(f,"rt_sb_char");
        E(f,"pop esi"); E(f,"pop ebx"); E(f,"ret");
    }
    LBL(f,lnull); E(f,"mov eax,Z%d",zlit("null")); E(f,"jmp rt_sb_cstr"); rt("rt_sb_cstr");
    buf_cat(&gg->text,&f->code); free(f->code.s);
}
static int gen_json_dumps(F *f, Expr *e, XInfo *xi){
    const char *isep=", ", *ksep=": ";
    if(xi->key){ isep=xi->key->items[0]->tok->text; ksep=xi->key->items[1]->tok->text; }
    int v=json_variant(isep,ksep,xi->argmap[0]);
    Expr *x=e->items[0];
    rt("rt_sb_need"); E(f,"push dword [rt_sb_len]");
    if(x->kind==EXPR_NONE) sb_cstr_lit(f,"null");
    else { gen_borrow(f,x); gen_json(f,TY(x),v); }
    E(f,"pop eax"); CALLRT(f,"rt_sb_take");
    return 1;
}

/* what str() / repr() of an object of class k calls: __str__ (an exception: none, its message), else __repr__ */
static AFunc *fmt_method(AClass *k, int repr){
    AFunc *m=repr?NULL:aot_find_method(k,"__str__");
    if(!m && (repr || !aot_is_exception(k))) m=aot_find_method(k,"__repr__");
    if(m && (m->is_static || ty_find(m->ret)->k!=TY_STR)) m=NULL;
    return m;
}
static void emit_formatter(int i){
    if(gg->fmts[i].repr>=2){ emit_json_formatter(i); return; }
    F ff; memset(&ff,0,sizeof ff); F *f=&ff;
    Ty *t=gg->fmts[i].ty; int repr=gg->fmts[i].repr;
    buf_printf(&f->code,"\nFMT%d:                         ; %s of %s\n",i,repr?"repr":"str",ty_name(t));
    if(t->k==TY_OBJ){
        AClass *cls=t->cls;
        AFunc *m=fmt_method(cls,repr);
        int lnone=new_label();
        E(f,"test eax,eax"); E(f,"jz L%d",lnone);
        {   /* an object of a subclass with a __str__ / __repr__ of its own (not an override of this one's) */
            AClass *ds[64]; int nd=0;
            for(int i=0;i<gg->p->nclasses && nd<64;i++){ AClass *d=gg->p->classes[i]; AFunc *dm;
                if(d==cls || !aot_subclass(d,cls) || !(dm=fmt_method(d,repr)) || dm->cls!=d || dm==m) continue;
                if(m && !strcmp(m->name,dm->name)) continue;          /* (the vtable call reaches it) */
                ds[nd++]=d; }
            for(int a=0;a<nd;a++) for(int b=a+1;b<nd;b++){ int da=0, db=0; for(AClass *x=ds[a];x;x=x->base) da++; for(AClass *x=ds[b];x;x=x->base) db++; if(db>da){ AClass *tmp=ds[a]; ds[a]=ds[b]; ds[b]=tmp; } }
            for(int i=0;i<nd;i++){ AFunc *dm=fmt_method(ds[i],repr); int lnext=new_label();
                use_class(ds[i]); use_fn(dm);
                E(f,"push eax"); E(f,"mov edx,%s",class_label(ds[i],0)); CALLRT(f,"rt_isinstance"); E(f,"test eax,eax"); E(f,"pop eax"); E(f,"jz L%d",lnext);
                E(f,"push eax");
                if(dm->overridden){ E(f,"mov eax,[eax+8]"); E(f,"call dword [eax+%d]",8+4*dm->vslot); }
                else E(f,"call %s",fnl(dm,LB_CODE));
                E(f,"mov [esp],eax"); CALLRT(f,"rt_sb_str"); E(f,"pop eax"); CALLRT(f,"rt_decref"); E(f,"ret");
                LBL(f,lnext); }
        }
        if(m){
            use_fn(m);
            E(f,"push eax");
            if(m->overridden){ E(f,"mov eax,[eax+8]"); E(f,"call dword [eax+%d]",8+4*m->vslot); }
            else E(f,"call %s",fnl(m,LB_CODE));
            E(f,"mov [esp],eax"); CALLRT(f,"rt_sb_str"); E(f,"pop eax"); CALLRT(f,"rt_decref"); E(f,"ret");
        } else if(aot_is_exception(cls)){                   /* str(e): the message; repr(e): Name('message') */
            if(!repr){ E(f,"mov eax,[eax+12]"); E(f,"jmp rt_sb_str"); rt("rt_sb_str"); }
            else {
                int lno=new_label(), lstr=new_label(), ldone=new_label();
                E(f,"push eax"); E(f,"mov eax,[eax+8]"); E(f,"mov eax,[eax]"); CALLRT(f,"rt_sb_str");
                E(f,"mov al,'('"); CALLRT(f,"rt_sb_char");
                AClass *ke=NULL; for(int i=0;i<gg->p->nclasses;i++) if(gg->p->classes[i]->builtin && !strcmp(gg->p->classes[i]->name,"KeyError")) ke=gg->p->classes[i];
                { AField *ar=aot_find_field(cls,"_argrepr");          /* OSError(errno, strerror): repr shows those two */
                  if(ar){ int lnoar=new_label(); E(f,"mov eax,[esp]"); E(f,"mov eax,[eax+%d]",ar->offset); E(f,"test eax,eax"); E(f,"jz L%d",lnoar); CALLRT(f,"rt_sb_str"); E(f,"jmp L%d",ldone); LBL(f,lnoar); } }
                E(f,"mov eax,[esp]"); E(f,"cmp dword [eax+12],0"); E(f,"je L%d",lno);
                E(f,"cmp dword [eax+%d],0",exc_rawoff(cls)); E(f,"jne L%d",lstr);          /* the message is the repr already */
                if(is_keyerror(cls)) E(f,"jmp L%d",lstr);                                  /* (a KeyError the run time raised) */
                else if(ke && aot_subclass(ke,cls)){ use_class(ke); E(f,"mov edx,%s",class_label(ke,0)); CALLRT(f,"rt_isinstance"); E(f,"test eax,eax"); E(f,"jnz L%d",lstr); E(f,"mov eax,[esp]"); }
                E(f,"mov eax,[eax+12]"); CALLRT(f,"rt_sb_repr_str"); E(f,"jmp L%d",ldone);
                LBL(f,lstr); E(f,"mov eax,[esp]"); E(f,"mov eax,[eax+12]"); CALLRT(f,"rt_sb_str");
                LBL(f,ldone); LBL(f,lno); E(f,"pop eax"); E(f,"mov al,')'"); E(f,"jmp rt_sb_char");
            }
        } else {
            E(f,"push eax"); E(f,"mov al,'<'"); CALLRT(f,"rt_sb_char");
            E(f,"pop eax"); E(f,"mov eax,[eax+8]"); E(f,"mov eax,[eax]"); CALLRT(f,"rt_sb_str");
            E(f,"mov eax,Z%d",zlit(" object>")); CALLRT(f,"rt_sb_cstr"); E(f,"ret");
        }
        LBL(f,lnone); E(f,"mov eax,Z%d",zlit("None")); E(f,"jmp rt_sb_cstr"); rt("rt_sb_cstr");
        buf_cat(&gg->text,&f->code); free(f->code.s);
        return;
    }
    if(t->k==TY_TUPLE && t->names){                       /* a record: {'key': value, ...} */
        int lnone=new_label();
        E(f,"test eax,eax"); E(f,"jz L%d",lnone);
        E(f,"push eax"); E(f,"mov al,'{'"); CALLRT(f,"rt_sb_char");
        for(int i=0;i<t->nelems;i++){
            char key[300]; snprintf(key,sizeof key,"%s'%s': ",i?", ":"",t->names[i]);
            E(f,"mov eax,Z%d",zlit(key)); CALLRT(f,"rt_sb_cstr");
            E(f,"mov eax,[esp]"); load_mem(f,t->elems[i],"eax",tuple_off(t,i)); gen_fmt(f,t->elems[i],1);
        }
        E(f,"pop eax"); E(f,"mov al,'}'"); E(f,"jmp rt_sb_char");
        LBL(f,lnone); E(f,"mov eax,Z%d",zlit("None")); E(f,"jmp rt_sb_cstr"); rt("rt_sb_cstr");
        buf_cat(&gg->text,&f->code); free(f->code.s);
        return;
    }
    if(t->k==TY_TUPLE){                                   /* (a, b) / (a,) */
        int lnone=new_label();
        E(f,"test eax,eax"); E(f,"jz L%d",lnone);
        E(f,"push eax"); E(f,"mov al,'('"); CALLRT(f,"rt_sb_char");
        for(int i=0;i<t->nelems;i++){
            if(i){ E(f,"mov eax,Z%d",zlit(", ")); CALLRT(f,"rt_sb_cstr"); }
            E(f,"mov eax,[esp]"); load_mem(f,t->elems[i],"eax",tuple_off(t,i)); gen_fmt(f,t->elems[i],1);
        }
        if(t->nelems==1){ E(f,"mov al,','"); CALLRT(f,"rt_sb_char"); }
        E(f,"pop eax"); E(f,"mov al,')'"); E(f,"jmp rt_sb_char");
        LBL(f,lnone); E(f,"mov eax,Z%d",zlit("None")); E(f,"jmp rt_sb_cstr"); rt("rt_sb_cstr");
        buf_cat(&gg->text,&f->code); free(f->code.s);
        return;
    }
    int ltop=new_label(), lend=new_label(), lskip=new_label(), lout=new_label();
    int isdict=t->k==TY_DICT, isset=t->k==TY_SET;
    Ty *el=t->elem;
    E(f,"push ebx"); E(f,"push esi"); E(f,"push 0"); E(f,"mov ebx,eax");
    { int l=new_label(); E(f,"test ebx,ebx"); E(f,"jnz L%d",l); E(f,"mov eax,Z%d",zlit("None")); CALLRT(f,"rt_sb_cstr"); E(f,"jmp L%d",lout); LBL(f,l); }
    if(isset){ int lne=new_label(), lgo=new_label();               /* the empty set prints as set() */
        E(f,"test ebx,ebx"); E(f,"jz L%d",lne); E(f,"cmp dword [ebx+8],0"); E(f,"jne L%d",lgo);
        LBL(f,lne); E(f,"mov eax,Z%d",zlit("set()")); CALLRT(f,"rt_sb_cstr"); E(f,"jmp L%d",lout);
        LBL(f,lgo);                                                 /* its elements in its order, as a list */
        E(f,"mov eax,ebx"); E(f,"mov edx,%s",kd_of(el)); CALLRT(f,"rt_set_to_list"); E(f,"mov ebx,eax"); E(f,"mov [esp],eax"); }
    E(f,"mov al,'%c'",t->k==TY_LIST?(t->tup?'(':'['):'{'); CALLRT(f,"rt_sb_char");
    E(f,"xor esi,esi");
    LBL(f,ltop);
    E(f,"test ebx,ebx"); E(f,"jz L%d",lend); E(f,"cmp esi,[ebx+8]"); E(f,"jae L%d",lend);
    E(f,"test esi,esi"); E(f,"jz L%d",lskip); E(f,"mov eax,Z%d",zlit(", ")); CALLRT(f,"rt_sb_cstr"); LBL(f,lskip);
    if(isdict){
        E(f,"mov eax,[ebx+16]"); load_at(f,ty_dkey(t),"eax","esi",8); gen_fmt(f,ty_dkey(t),1);
        E(f,"mov eax,Z%d",zlit(": ")); CALLRT(f,"rt_sb_cstr");
        E(f,"mov eax,[ebx+20]");
    } else E(f,"mov eax,[ebx+16]");
    load_at(f,el,"eax","esi",esize(el));
    gen_fmt(f,el,1);
    E(f,"inc esi"); E(f,"jmp L%d",ltop);
    LBL(f,lend);
    if(t->k==TY_LIST && t->tup){ int l=new_label(); E(f,"cmp esi,1"); E(f,"jne L%d",l); E(f,"mov al,','"); CALLRT(f,"rt_sb_char"); LBL(f,l); }   /* (x,) */
    E(f,"mov al,'%c'",t->k==TY_LIST?(t->tup?')':']'):'}'); CALLRT(f,"rt_sb_char");
    LBL(f,lout); E(f,"pop eax"); CALLRT(f,"rt_decref"); E(f,"pop esi"); E(f,"pop ebx"); E(f,"ret");
    buf_cat(&gg->text,&f->code); free(f->code.s);
}

/* ---------------------------------------------------------------- asyncio */

/* TS<i>: where a task starts: calls the coroutine with the arguments above it,
   keeps the result in the task, drops the arguments, ends the task. */
static int task_stub(AFunc *fn, int virt){
    for(int i=0;i<gg->nstubs;i++) if(gg->stubs[i].fn==fn && gg->stubs[i].virt==virt) return i;
    if(gg->nstubs==gg->cstubs){ gg->cstubs=gg->cstubs?gg->cstubs*2:8; gg->stubs=xrealloc(gg->stubs,sizeof(*gg->stubs)*(size_t)gg->cstubs); }
    gg->stubs[gg->nstubs].fn=fn; gg->stubs[gg->nstubs].virt=virt;
    use_fn(fn); rt("rt_task_exit"); rt("rt_task_new");
    return gg->nstubs++;
}
static void emit_task_stub(int i){
    AFunc *fn=gg->stubs[i].fn; Buf *o=&gg->text; Ty *ret=ty_find(fn->ret);
    buf_printf(o,"\n%s:                          ; task: %s%s%s()\n",fnl(fn,gg->stubs[i].virt?LB_VTASK:LB_TASK),fn->cls?fn->cls->name:"",fn->cls?".":"",fn->name);
    if(gg->stubs[i].virt) buf_printf(o,"        mov eax,[esp]\n        mov eax,[eax+8]\n        call dword [eax+%d]\n",8+4*fn->vslot);
    else buf_printf(o,"        call %s\n",fnl(fn,LB_CODE));
    buf_printf(o,"        mov ecx,[rt_cur_task]\n");
    if(is_flt(ret)) buf_printf(o,"        fstp qword [ecx+24]\n");
    else if(ret->k!=TY_VOID){ buf_printf(o,"        mov [ecx+24],eax\n"); if(is_lng(ret)) buf_printf(o,"        mov [ecx+28],edx\n"); if(is_ptr(ret)) buf_printf(o,"        mov dword [ecx+44],1\n"); }
    int off=0;
    for(int k=0;k<fn->nparams;k++){ Ty *pt=fn->params[k]->ty;
        if(is_ptr(pt)){ rt("rt_decref"); buf_printf(o,"        mov eax,[esp+%d]\n        call rt_decref\n",off); }
        off+=esize(pt); }
    buf_printf(o,"        jmp rt_task_exit\n");
}
/* A task running the coroutine call `call` -> eax (owned). Its arguments are
   evaluated now (and owned by the task). */
static void gen_task_new(F *f, Expr *call){
    XInfo *xi=xinfo(call); AFunc *fn=xi->fn;
    if(fn->star>=0||fn->dstar>=0) cg_fail(call->line,"coroutines with *args/**kwargs are not supported in compiled code");
    for(int i=0;i<16;i++) if(xi->argelem[i]) cg_fail(call->line,"f(*seq) of a coroutine is not supported in compiled code");
    int self_kind=xi->kind==X_METHOD&&!fn->is_static ? 1 : xi->kind==X_SUPER&&!fn->is_static ? 2 : 0;
    int off[16], total=0;
    for(int i=0;i<fn->nparams;i++){ off[i]=total; total+=esize(fn->params[i]->ty); }
    int ts=task_stub(fn,self_kind==1&&fn->overridden);
    E(f,"mov eax,%d",total); E(f,"mov edx,%s",fnl(gg->stubs[ts].fn,gg->stubs[ts].virt?LB_VTASK:LB_TASK)); CALLRT(f,"rt_task_new");
    E(f,"push eax");
    #define TASK_ARG(pt,o_) do{ E(f,"mov ecx,[esp]"); E(f,"mov ecx,[ecx+12]"); store_new(f,pt,"ecx",20+(o_)); }while(0)
    if(self_kind==1){ gen_owned(f,call->a->a); panic_if_null(f); TASK_ARG(fn->params[0]->ty,off[0]); }
    if(self_kind==2){ E(f,"mov eax,[ebp+8]"); incref(f); TASK_ARG(fn->params[0]->ty,off[0]); }
    for(int ai=0;ai<call->count;ai++)
        for(int i=0;i<fn->nparams;i++) if(xi->argmap[i]==ai){ Ty *pt=fn->params[i]->ty; gen_as(f,call->items[ai],pt,1); TASK_ARG(pt,off[i]); }
    for(int i=(self_kind?1:0);i<fn->nparams;i++) if(xi->argmap[i]<0 && fn->defaults[i]){ Ty *pt=fn->params[i]->ty; gen_as(f,fn->defaults[i],pt,1); TASK_ARG(pt,off[i]); }
    #undef TASK_ARG
    E(f,"pop eax");
}
static int gen_asyncio(F *f, Expr *e, XInfo *xi){
    if(!strcmp(xi->name,"create_task")){ gen_task_new(f,e->items[0]); return 1; }
    /* asyncio.run(main()): the event loop runs until that task is done */
    Ty *t=TY(e);
    gen_task_new(f,e->items[0]); hold(f);
    CALLRT(f,"rt_async_run");
    if(ty_find(t)->k!=TY_VOID) load_mem(f,t,"eax",24);
    return 0;
}
static int gen_await(F *f, Expr *e){
    XInfo *xi=xinfo(e); Ty *t=TY(e); Expr *x=e->a;
    if(!strcmp(xi->name,"call")||!strcmp(xi->name,"plain")) return gen(f,x);
    if(!strcmp(xi->name,"sleep")){ gen_as(f,x->items[0],TY_FLOAT_T,0); CALLRT(f,"rt_async_sleep"); return 0; }
    if(!strcmp(xi->name,"task")){
        gen_borrow(f,x); CALLRT(f,"rt_task_wait");
        if(ty_find(t)->k!=TY_VOID) load_mem(f,t,"eax",24);
        return 0;
    }
    /* gather: start every task, then collect the results in order */
    int n=x->count, slots[32];
    if(n>32) cg_fail(e->line,"too many coroutines for gather()");
    for(int i=0;i<n;i++){ gen_task_new(f,x->items[i]); hold(f); slots[i]=f->tmp[f->tmp_used-1]; }
    if(ty_find(t)->k==TY_VOID){ for(int i=0;i<n;i++){ E(f,"mov eax,[ebp%+d]",slots[i]); CALLRT(f,"rt_task_wait"); } return 0; }
    Ty *el=ty_find(t)->elem;
    new_list(f,el); E(f,"push eax");
    for(int i=0;i<n;i++){
        E(f,"mov eax,[ebp%+d]",slots[i]); CALLRT(f,"rt_task_wait");
        load_mem(f,el,"eax",24); if(is_ptr(el)) incref(f);
        list_append_top(f,el);
    }
    E(f,"pop eax");
    return 1;
}

/* ---------------------------------------------------------------- math, time, random */

static void farg(F *f, Expr *a){ gen_as(f,a,TY_FLOAT_T,0); }
static int gen_bmod(F *f, Expr *e, XInfo *xi){
    const char *m=xi->name; Expr **a=e->items;
    Expr *call=e->a; const char *mod="?";
    if(call->kind==EXPR_ATTRIBUTE && call->a->kind==EXPR_NAME){
        ASym *s=NULL; AFunc *fn=f->fn;
        if(fn->def) s=symtab_find(&fn->locals,call->a->name);
        if(!s) s=symtab_find(&fn->mod->syms,call->a->name);
        if(s && s->kind==AS_SYS) mod=(const char*)s->p;
    }
    if(!strcmp(mod,"math")){
        if(!strcmp(m,"floor")||!strcmp(m,"ceil")){ farg(f,a[0]); if(m[0]=='c') E(f,"fchs"); CALLRT(f,"rt_ffloor"); if(m[0]=='c') E(f,"fchs"); CALLRT(f,"rt_ftoi"); return 0; }
        if(!strcmp(m,"trunc")){ farg(f,a[0]); CALLRT(f,"rt_ftoi"); return 0; }
        if(!strcmp(m,"gcd")){ gen_as(f,a[0],TY_INT_T,0); push_value(f,TY_INT_T); gen_as(f,a[1],TY_INT_T,0); CALLRT(f,"rt_gcd"); E(f,"add esp,8"); return 0; }
        if(!strcmp(m,"isqrt")){ gen_as(f,a[0],TY_INT_T,0); CALLRT(f,"rt_isqrt"); return 0; }
        if(!strcmp(m,"frexp")){ Ty *t=TY(e); farg(f,a[0]); CALLRT(f,"rt_frexp");        /* (m, e): a new tuple */
            E(f,"push eax"); E(f,"sub esp,8"); E(f,"fstp qword [esp]");
            E(f,"mov eax,TD%d",tdesc(t)); E(f,"mov edx,%d",tuple_size(t)); CALLRT(f,"rt_tuple_new");
            E(f,"pop ecx"); E(f,"mov [eax+%d],ecx",tuple_off(t,0)); E(f,"pop ecx"); E(f,"mov [eax+%d],ecx",tuple_off(t,0)+4);
            E(f,"pop ecx"); E(f,"mov [eax+%d],ecx",tuple_off(t,1)); E(f,"sar ecx,31"); E(f,"mov [eax+%d],ecx",tuple_off(t,1)+4);
            return 1; }
        if(!strcmp(m,"ldexp")){ gen_as(f,a[1],TY_INT_T,0); push_value(f,TY_INT_T); farg(f,a[0]); E(f,"pop eax"); E(f,"pop edx"); CALLRT(f,"rt_ldexp"); return 0; }
        if(e->count==2){                                                   /* two floats */
            farg(f,a[0]); E(f,"sub esp,8"); E(f,"fstp qword [esp]");
            farg(f,a[1]); E(f,"fld qword [esp]"); E(f,"add esp,8");          /* st0 = x, st1 = y */
            if(!strcmp(m,"atan2")){ E(f,"fxch"); CALLRT(f,"rt_fatan2"); }      /* atan(st1/st0): st1 = x(=y arg), st0 = y(=x arg) */
            else if(!strcmp(m,"pow")) CALLRT(f,"rt_fpow");
            else if(!strcmp(m,"hypot")){ E(f,"fmul st0,st0"); E(f,"fxch"); E(f,"fmul st0,st0"); E(f,"faddp st1,st0"); E(f,"fsqrt"); }
            else if(!strcmp(m,"fmod")) CALLRT(f,"rt_fmod_c");
            else if(!strcmp(m,"copysign")){ int l=new_label(); E(f,"fabs"); E(f,"fxch"); E(f,"sub esp,8"); E(f,"fstp qword [esp]");   /* |x|, y's sign */
                E(f,"mov eax,[esp+4]"); E(f,"add esp,8"); E(f,"test eax,eax"); E(f,"jns L%d",l); E(f,"fchs"); LBL(f,l); }
            else { E(f,"fldln2"); E(f,"fxch"); E(f,"fyl2x"); E(f,"fxch"); E(f,"fldln2"); E(f,"fxch"); E(f,"fyl2x"); E(f,"fdivp st1,st0"); }  /* log(x, base) */
            return 0;
        }
        if(!strcmp(m,"isnan")||!strcmp(m,"isinf")||!strcmp(m,"isfinite")){         /* (comparisons: unordered for a NaN) */
            farg(f,a[0]);
            if(m[2]=='n' && m[3]=='a'){ E(f,"fld st0"); E(f,"fcompp"); E(f,"fnstsw ax"); E(f,"sahf"); E(f,"setp al"); }
            else { rt("rt_finf"); E(f,"fabs"); E(f,"fld qword [rt_finf]"); E(f,"fcompp"); E(f,"fnstsw ax"); E(f,"sahf");
                if(m[2]=='i') E(f,"sete al"); else E(f,"seta al");
                E(f,"setnp cl"); E(f,"and al,cl"); }
            E(f,"movzx eax,al"); return 0; }
        farg(f,a[0]);
        if(!strcmp(m,"sqrt")) E(f,"fsqrt");
        else if(!strcmp(m,"sin")) CALLRT(f,"rt_fsin");
        else if(!strcmp(m,"cos")) CALLRT(f,"rt_fcos");
        else if(!strcmp(m,"tan")) CALLRT(f,"rt_ftan");
        else if(!strcmp(m,"atan")) CALLRT(f,"rt_fatan");
        else if(!strcmp(m,"asin")) CALLRT(f,"rt_fasin");
        else if(!strcmp(m,"acos")) CALLRT(f,"rt_facos");
        else if(!strcmp(m,"exp")) CALLRT(f,"rt_fexp");
        else if(!strcmp(m,"log")) CALLRT(f,"rt_flog");
        else if(!strcmp(m,"log10")) CALLRT(f,"rt_flog10");
        else if(!strcmp(m,"log2")) CALLRT(f,"rt_flog2");
        else if(!strcmp(m,"fabs")) E(f,"fabs");
        else if(!strcmp(m,"degrees")) E(f,"fmul qword [FC%d]",float_const("57.29577951308232"));      /* 180/pi, as Python */
        else if(!strcmp(m,"radians")) E(f,"fmul qword [FC%d]",float_const("0.017453292519943295"));
        else cg_fail(e->line,"unsupported math function");
        return 0;
    }
    if(!strcmp(mod,"time")){
        if(!strcmp(m,"sleep")){ farg(f,a[0]); CALLRT(f,"rt_time_sleep"); return 0; }
        CALLRT(f,"rt_time_now"); return 0;
    }
    if(!strcmp(mod,"random")){
        if(!strcmp(m,"random")){ CALLRT(f,"rt_rand_float"); return 0; }
        if(!strcmp(m,"seed")){ gen(f,a[0]); CALLRT(f,"rt_rand_seed"); return 0; }
        if(!strcmp(m,"uniform")){ farg(f,a[0]); E(f,"sub esp,8"); E(f,"fstp qword [esp]"); farg(f,a[1]); E(f,"fsub qword [esp]");
            CALLRT(f,"rt_rand_float"); E(f,"fmulp st1,st0"); E(f,"fadd qword [esp]"); E(f,"add esp,8"); return 0; }
        if(!strcmp(m,"randint")||!strcmp(m,"randrange")){        /* [esp] lo, edx:eax hi (exclusive), ecx: 0 randrange(n), 1 randrange(a, b), 2 randint */
            if(e->count==1){ E(f,"push 0"); E(f,"push 0"); gen_as(f,a[0],TY_INT_T,0); E(f,"xor ecx,ecx"); }
            else { gen_as(f,a[0],TY_INT_T,0); push_value(f,TY_INT_T); gen_as(f,a[1],TY_INT_T,0); E(f,"mov ecx,%d",m[4]=='i'?2:1); }
            CALLRT(f,"rt_rand_range"); E(f,"add esp,8"); return 0; }
        if(!strcmp(m,"choice")){
            Ty *t=TY(a[0]);
            gen_borrow(f,a[0]); E(f,"push eax"); E(f,"push 0"); E(f,"push 0");
            E(f,"xor eax,eax"); E(f,"xor edx,edx"); E(f,"mov ecx,[esp+8]"); E(f,"test ecx,ecx"); int l=new_label(); E(f,"jz L%d",l);
            if(t->k==TY_STR){ E(f,"mov eax,ecx"); CALLRT(f,"rt_str_cplen"); E(f,"xor edx,edx"); } else E(f,"mov eax,[ecx+8]");
            LBL(f,l);
            E(f,"mov ecx,3"); CALLRT(f,"rt_rand_range"); E(f,"add esp,8"); E(f,"mov edx,eax"); E(f,"pop eax");
            if(t->k==TY_STR){ CALLRT(f,"rt_str_char"); return 0; }
            E(f,"mov ecx,%d",esize(t->elem)); CALLRT(f,"rt_list_at"); load_mem(f,t->elem,"eax",0); return 0; }
        if(!strcmp(m,"shuffle")){ Ty *t=TY(a[0]); gen_borrow(f,a[0]); E(f,"mov edx,%d",esize(t->elem)); CALLRT(f,"rt_shuffle"); return 0; }
    }
    cg_fail(e->line,"unsupported built-in module function");
}

/* ---------------------------------------------------------------- statements */

static int has_call(Expr *e){
    if(!e) return 0;
    if(e->kind==EXPR_CALL||e->kind==EXPR_COMPREHENSION) return 1;
    if(has_call(e->a)||has_call(e->b)||has_call(e->c)||has_call(e->d)) return 1;
    for(int i=0;i<e->count;i++) if(has_call(e->items[i])) return 1;
    for(int i=0;i<e->vcount;i++) if(has_call(e->vals[i])) return 1;
    return 0;
}
static int park_value(F *f, Ty *t, int owned);
/* print(..., *xs, ...): separators only between the items actually printed */
static void gen_print_star(F *f, APrint *pr){
    int first=frame_slot(f,4);
    rt("rt_sb_need"); E(f,"push dword [rt_sb_len]");
    E(f,"mov dword [ebp%+d],1",first);
    for(int i=0;i<pr->n;i++){
        Iter I; int star=pr->star[i];
        if(star){ iter_begin(f,pr->args[i],&I); }
        int l=new_label();
        E(f,"cmp dword [ebp%+d],0",first); E(f,"jne L%d",l);
        if(pr->sep) gen_into_sb(f,pr->sep); else { E(f,"mov al,' '"); CALLRT(f,"rt_sb_char"); }
        LBL(f,l); E(f,"mov dword [ebp%+d],0",first);
        if(star){ iter_value(f,&I,0); gen_fmt(f,I.elem,0); iter_end(f,&I); LBL(f,I.exit); iter_release(f,&I); }
        else gen_into_sb(f,pr->args[i]);
    }
    if(pr->end) gen_into_sb(f,pr->end); else { E(f,"mov al,10"); CALLRT(f,"rt_sb_char"); }
    E(f,"pop eax"); CALLRT(f,pr->fd==2?"rt_sb_flush_err":"rt_sb_flush");
}
static void gen_print(F *f, APrint *pr){
    int n=pr->n;
    if(pr->flush){ int o=gen(f,pr->flush); drop_value(f,TY(pr->flush),o); }   /* (writes are not buffered: nothing to flush) */
    for(int i=0;i<n;i++) if(pr->star[i]){ gen_print_star(f,pr); return; }
    if(!pr->sep && !pr->end && pr->fd!=2){
        if(n==0){ E(f,"mov ecx,P%d",print_lit("",0)); E(f,"mov edx,1"); CALLRT(f,"rt_write"); return; }
        Expr *a=pr->args[0]; Ty *t=TY(a);
        if(n==1 && is_str_lit(a) && a->tok->len){
            E(f,"mov ecx,P%d",print_lit(a->tok->text,(int)a->tok->len)); E(f,"mov edx,%d",(int)a->tok->len+1); CALLRT(f,"rt_write"); return; }
        if(n==1 && t->k==TY_STR && !t->opt && !is_concat(a) && !is_fmt_expr(a) && !is_str_call(a)){ gen_borrow(f,a); CALLRT(f,"rt_print_str"); return; }
        if(n==1 && t->k==TY_INT && !t->opt){ gen(f,a); CALLRT(f,"rt_print_int"); return; }      /* edx:eax */
    }
    /* print(xs, xs.pop()): every argument is evaluated before any is formatted */
    int early=0, slots[32];
    for(int i=1;i<n && !early;i++) if(has_call(pr->args[i])) for(int k=0;k<i;k++){ Ty *t=TY(pr->args[k]); if(t->k==TY_LIST||t->k==TY_DICT||t->k==TY_SET||t->k==TY_OBJ) early=1; }
    if(early) for(int i=0;i<n;i++){ Expr *a=pr->args[i]; int o=gen(f,a); Ty *t=TY(a);
        slots[i]=(t->k==TY_VAR||t->k==TY_VOID) ? 0 : park_value(f,t,o); }
    rt("rt_sb_need"); E(f,"push dword [rt_sb_len]");
    for(int i=0;i<n;i++){
        if(i){ if(pr->sep) gen_into_sb(f,pr->sep); else { E(f,"mov al,' '"); CALLRT(f,"rt_sb_char"); } }
        if(early){ Ty *t=TY(pr->args[i]);
            if(slots[i]) load_mem(f,t,"ebp",slots[i]);
            gen_fmt(f,t,0); }
        else gen_into_sb(f,pr->args[i]);
    }
    if(pr->end) gen_into_sb(f,pr->end); else { E(f,"mov al,10"); CALLRT(f,"rt_sb_char"); }
    E(f,"pop eax"); CALLRT(f,pr->fd==2?"rt_sb_flush_err":"rt_sb_flush");
}

/* Store the owned value in eax/st0 (of type vt) into an assignment target. */
/* a, *b, c = value (in eax, owned): b gets a new list of the middle items */
static void gen_star_unpack(F *f, Expr *t, Ty *lt, int star, int line){
    int n=t->count, after=n-1-star;
    hold(f); int slot=f->tmp[f->tmp_used-1];
    if(lt->k==TY_TUPLE){                                     /* a fixed tuple: its items by position */
        int rest=lt->nelems-(n-1);
        for(int i=0;i<star;i++){ Ty *et=lt->elems[i]; E(f,"mov eax,[ebp%+d]",slot); load_mem(f,et,"eax",tuple_off(lt,i)); if(is_ptr(et)) incref(f); store_target(f,t->items[i],et,line); }
        Ty *mt=ty_find(TY(t->items[star])), *el=mt->elem;
        new_list(f,el); E(f,"push eax");
        for(int i=star;i<star+rest;i++){ E(f,"mov eax,[ebp%+d]",slot); load_mem(f,lt->elems[i],"eax",tuple_off(lt,i)); conv_num(f,lt->elems[i],el); if(is_ptr(el)) incref(f); list_append_top(f,el); }
        E(f,"pop eax"); store_target(f,t->items[star],mt,line);
        for(int i=star+1;i<n;i++){ int k=lt->nelems-(n-i); Ty *et=lt->elems[k]; E(f,"mov eax,[ebp%+d]",slot); load_mem(f,et,"eax",tuple_off(lt,k)); if(is_ptr(et)) incref(f); store_target(f,t->items[i],et,line); }
        return;
    }
    Ty *el= lt->k==TY_STR ? TY_STR_T : lt->elem;
    if(lt->k==TY_STR){ E(f,"mov eax,[ebp%+d]",slot); CALLRT(f,"rt_str_chars"); hold(f); slot=f->tmp[f->tmp_used-1]; }
    E(f,"mov eax,[ebp%+d]",slot); E(f,"mov edx,%d",n-1); CALLRT(f,"rt_unpack_atleast");
    for(int i=0;i<star;i++){ E(f,"mov eax,[ebp%+d]",slot); E(f,"mov eax,[eax+16]"); load_mem(f,el,"eax",esize(el)*i); if(is_ptr(el)) incref(f); store_target(f,t->items[i],el,line); }
    E(f,"sub esp,24"); E(f,"mov eax,[ebp%+d]",slot); E(f,"mov [esp],eax");          /* the middle: value[star:len-after] */
    E(f,"mov dword [esp+4],%d",star); E(f,"mov dword [esp+8],%d",-after); E(f,"mov dword [esp+12],0");
    E(f,"mov dword [esp+16],%d",after?4:6); E(f,"mov dword [esp+20],%s",kd_of(el));
    CALLRT(f,"rt_list_slice"); E(f,"add esp,24");
    store_target(f,t->items[star],TY(t->items[star]),line);
    for(int i=star+1;i<n;i++){ E(f,"mov eax,[ebp%+d]",slot); E(f,"mov ecx,[eax+8]"); E(f,"sub ecx,%d",n-i); E(f,"mov eax,[eax+16]");
        if(esize(el)==8) E(f,"lea eax,[eax+ecx*8]"); else E(f,"lea eax,[eax+ecx*4]");
        load_mem(f,el,"eax",0); if(is_ptr(el)) incref(f); store_target(f,t->items[i],el,line); }
}
/* obj.name = value on a frozen dataclass: FrozenInstanceError("cannot assign to field 'name'") */
static void gen_frozen_raise(F *f, Expr *t, const char *what){
    gen_borrow(f,t->a); attr_check(f,t->name);
    AClass *cls=NULL; for(int i=0;i<gg->p->nclasses;i++) if(gg->p->classes[i]->builtin && !strcmp(gg->p->classes[i]->name,"FrozenInstanceError")) cls=gg->p->classes[i];
    char msg[300]; snprintf(msg,sizeof msg,"cannot %s field '%s'",what,t->name);
    use_class(cls);
    E(f,"mov eax,S%d",str_lit(msg)); incref(f); E(f,"push eax");
    E(f,"mov eax,%d",cls->size); E(f,"mov edx,%s",class_label(cls,0)); E(f,"mov ecx,%s",class_label(cls,1)); CALLRT(f,"rt_obj_new");
    E(f,"pop ecx"); E(f,"mov [eax+12],ecx");
    seed_fields(f,cls);
    CALLRT(f,"rt_throw");
}
static void store_target(F *f, Expr *t, Ty *vt, int line){
    XInfo *xi=xinfo(t);
    switch(t->kind){
        case EXPR_NAME:{
            if(xi->name && !strcmp(xi->name,"_discard")){ drop_value(f,vt,1); return; }      /* _ = a value of another type */
            AVar *v=xi->kind==X_VAR&&xi->var?xi->var:find_var(f,t->name,line); conv(f,vt,v->ty); store_var(f,v); return; }
        case EXPR_ATTRIBUTE:
            if(xi->kind==X_VAR){ conv(f,vt,xi->var->ty); store_var(f,xi->var); return; }
            if(xi->name && !strcmp(xi->name,"frozen")){ drop_value(f,vt,1); gen_frozen_raise(f,t,"assign to"); return; }
            { AField *fd=xi->field;
              conv(f,vt,fd->ty); push_value(f,fd->ty);
              gen_borrow(f,t->a); attr_check(f,t->name); E(f,"mov ecx,eax");
              if(fd->setoff) E(f,"mov dword [ecx+%d],1",fd->setoff);
              pop_value(f,fd->ty); store_mem(f,fd->ty,"ecx",fd->offset); return; }
        case EXPR_SLICE:{                                     /* xs[a:b:c] = items: the items as a list */
            Ty *v=ty_find(vt); Expr *parts[3]={t->b,t->c,t->d}; int flags=0;
            if(v->k==TY_STR) CALLRT(f,"rt_str_chars");
            else if(v->k==TY_SET){ E(f,"push eax"); E(f,"mov edx,%s",kd_of(v->elem)); CALLRT(f,"rt_set_to_list"); E(f,"xchg eax,[esp]"); CALLRT(f,"rt_decref"); E(f,"pop eax"); }
            else if(v->k==TY_GEN){ E(f,"mov edx,%s",kd_of(v->elem)); CALLRT(f,"rt_gen_drain"); }
            hold(f); int vs=f->tmp[f->tmp_used-1];
            E(f,"sub esp,24"); gen_borrow(f,t->a); check_none(f,TY(t->a),"TypeError","'NoneType' object does not support item assignment"); E(f,"mov [esp],eax");
            for(int k=0;k<3;k++){
                if(parts[k] && parts[k]->kind!=EXPR_NONE){ gen(f,parts[k]); to_i32(f,TY(parts[k])); E(f,"mov [esp+%d],eax",4+4*k); }
                else { flags|=1<<k; E(f,"mov dword [esp+%d],0",4+4*k); } }
            E(f,"mov dword [esp+16],%d",flags); E(f,"mov eax,[ebp%+d]",vs); E(f,"mov [esp+20],eax");
            CALLRT(f,"rt_list_set_slice"); E(f,"add esp,24");
            return; }
        case EXPR_INDEX:{
            Ty *ct=TY(t->a), *el=ct->elem; int dict=ct->k==TY_DICT;
            if(ct->k==TY_OBJ){                                    /* obj[key] = value: obj.__setitem__(key, value) */
                AFunc *m=aot_find_method(ct->cls,"__setitem__"); Ty *kt=m->params[1]->ty, *pv=m->params[2]->ty;
                conv(f,vt,pv); int slot=park_value(f,pv,1);          /* the owned value, released with the statement's temporaries */
                int total=4+esize(kt)+esize(pv);
                E(f,"sub esp,%d",total);
                gen_borrow(f,t->a); E(f,"mov [esp],eax");
                gen_as(f,t->b,kt,0); put_arg(f,kt,4);
                load_mem(f,pv,"ebp",slot); put_arg(f,pv,4+esize(kt));
                call_on_top(f,m); E(f,"add esp,%d",total);
                drop_value(f,m->ret,1);
                return;
            }
            conv(f,vt,el); push_value(f,el);
            gen_borrow(f,t->a); check_none(f,ct,"TypeError","'NoneType' object does not support item assignment"); E(f,"push eax");
            if(dict){ Ty *kt=ty_dkey(ct); gen_as(f,t->b,kt,0); push_value(f,kt);
                E(f,"mov edx,esp"); E(f,"mov eax,[esp+%d]",esize(kt)); CALLRT(f,"rt_dict_slot"); E(f,"add esp,%d",esize(kt)+4); }
            else { gen(f,t->b); to_i32(f,TY(t->b)); E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_list_set_at"); }
            E(f,"mov ecx,eax"); pop_value(f,el); store_mem(f,el,"ecx",0);
            return; }
        case EXPR_TUPLE: case EXPR_LIST:{                    /* a, b = some_list / some_tuple */
            Ty *lt=ty_find(vt), *el=lt->elem;
            int star=-1; for(int i=0;i<t->count;i++) if(t->items[i]->akind==1) star=i;
            if(star>=0){ gen_star_unpack(f,t,lt,star,line); return; }
            hold(f); int slot=f->tmp[f->tmp_used-1];
            if(lt->k==TY_TUPLE){ unpack_tuple_slot(f,slot,lt,t->items,t->count,line); return; }
            if(lt->k==TY_STR){ el=TY_STR_T; CALLRT(f,"rt_str_chars"); hold(f); slot=f->tmp[f->tmp_used-1]; }   /* a, b = "xy" */
            E(f,"mov eax,[ebp%+d]",slot); E(f,"mov edx,0x%x",t->count|(lt->k==TY_STR?0x80000000u:0)); CALLRT(f,"rt_unpack_check");
            for(int i=0;i<t->count;i++){
                E(f,"mov eax,[ebp%+d]",slot); E(f,"mov eax,[eax+16]");
                load_mem(f,el,"eax",esize(el)*i);
                if(is_ptr(el)) incref(f);
                store_target(f,t->items[i],el,line);
            }
            return; }
        default: cg_fail(line,"cannot assign to this expression");
    }
}
/* a borrowed copy of the value in eax/st0 kept until the end of the statement */
static int park_value(F *f, Ty *t, int owned){
    if(is_ptr(t)){ if(!owned) incref(f); hold(f); return f->tmp[f->tmp_used-1]; }
    int slot=frame_slot(f,8);
    store_new(f,t,"ebp",slot);
    return slot;
}
static void load_parked(F *f, Ty *t, int slot){
    load_mem(f,t,"ebp",slot);
    if(is_ptr(t)) incref(f);
}

static void gen_augassign(F *f, AAssign *a, int line){
    Expr *t=a->target[0], *v=a->value;
    Ty *cur=TY(t), *tv=TY(v); int num=numeric_ty(cur);
    XInfo *xi=xinfo(t);
    if(a->opfn){                                             /* v += w with __add__ & co: v = v.__add__(w) */
        AFunc *m=a->opfn; Ty *pt=m->params[1]->ty; int total=op_frame(m);
        if(t->kind==EXPR_INDEX && TY(t->a)->k==TY_OBJ) cg_fail(line,"augmented assignment to obj[key] is not supported in compiled code");
        E(f,"sub esp,%d",total);
        gen_borrow(f,t); E(f,"mov [esp],eax");
        gen_as(f,v,pt,0); put_arg(f,pt,4);
        op_defaults(f,m,2);
        call_on_top(f,m); E(f,"add esp,%d",total);
        store_target(f,t,m->ret,line);
        return;
    }
    if(cur->k==TY_LIST && a->aug==T_PLUS_ASSIGN){            /* list += x extends in place */
        gen_borrow(f,t); E(f,"push eax");
        if(TY(v)->k==TY_LIST) gen_borrow(f,v); else { gen_list_of(f,v,cur->elem); hold(f); }
        E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_list_extend");
        return;
    }
    if(cur->k==TY_SET && TY(v)->k==TY_SET && (a->aug==T_PIPE_ASSIGN||a->aug==T_AMP_ASSIGN||a->aug==T_MINUS_ASSIGN||a->aug==T_CARET_ASSIGN)){
        gen_borrow(f,t); E(f,"push eax"); gen_borrow(f,v);      /* in place, as CPython's set does */
        E(f,"mov edx,eax"); E(f,"pop eax");
        E(f,"mov ecx,%d",a->aug==T_PIPE_ASSIGN?0:a->aug==T_AMP_ASSIGN?1:a->aug==T_MINUS_ASSIGN?2:3); CALLRT(f,"rt_set_iop");
        return;
    }
    const char *mleft=binop_none_msg(a->aug,cur,tv,1), *mright=binop_none_msg(a->aug,cur,tv,0);
    if(t->kind==EXPR_NAME || (t->kind==EXPR_ATTRIBUTE && xi->kind==X_VAR)){
        AVar *var=xi->var?xi->var:find_var(f,t->name,line);
        load_var(f,var); check_none(f,cur,"TypeError",mleft); push_value(f,cur);
        gen_as(f,v,num?cur:tv,0); check_none(f,tv,"TypeError",mright);
        int o=apply_binop(f,a->aug,cur,cur,tv,line);
        if(is_ptr(cur) && !o) incref(f);
        store_var(f,var);
        return;
    }
    if(t->kind==EXPR_ATTRIBUTE){
        AField *fd=xi->field;
        if(xi->name && !strcmp(xi->name,"frozen")){ gen_borrow(f,t->a); attr_check(f,t->name); load_field(f,fd); gen_frozen_raise(f,t,"assign to"); return; }   /* (the value is never computed) */
        gen_borrow(f,t->a); attr_check(f,t->name); E(f,"push eax");
        load_field(f,fd); check_none(f,cur,"TypeError",mleft); push_value(f,cur);
        gen_as(f,v,num?cur:tv,0); check_none(f,tv,"TypeError",mright);
        int o=apply_binop(f,a->aug,cur,cur,tv,line);
        if(is_ptr(cur) && !o) incref(f);
        E(f,"pop ecx"); if(fd->setoff) E(f,"mov dword [ecx+%d],1",fd->setoff);
        store_mem(f,cur,"ecx",fd->offset);
        return;
    }
    if(t->kind==EXPR_INDEX){
        Ty *ct=TY(t->a); int es=esize(cur), dict=ct->k==TY_DICT;
        Ty *kt=dict?ty_dkey(ct):TY_INT_T; int ks=dict?esize(kt):4;
        const char *at=dict?"rt_dict_get":"rt_list_at";
        gen_borrow(f,t->a); check_none(f,ct,"TypeError","'NoneType' object is not subscriptable"); E(f,"push eax");
        if(dict){ gen_as(f,t->b,kt,0); push_value(f,kt); } else { gen(f,t->b); to_i32(f,TY(t->b)); E(f,"push eax"); }   /* [esp] key, then the container */
        if(dict) E(f,"mov edx,esp"); else E(f,"mov edx,[esp]");
        E(f,"mov eax,[esp+%d]",ks); CALLRT(f,at);
        load_mem(f,cur,"eax",0); check_none(f,cur,"TypeError",mleft); push_value(f,cur);
        gen_as(f,v,num?cur:tv,0); check_none(f,tv,"TypeError",mright);
        int o=apply_binop(f,a->aug,cur,cur,tv,line);
        if(is_ptr(cur) && !o) incref(f);
        push_value(f,cur);
        if(dict) E(f,"lea edx,[esp+%d]",es); else E(f,"mov edx,[esp+%d]",es);
        E(f,"mov eax,[esp+%d]",es+ks); CALLRT(f,at);
        E(f,"mov ecx,eax"); pop_value(f,cur); store_mem(f,cur,"ecx",0);
        E(f,"add esp,%d",ks+4);
        return;
    }
    cg_fail(line,"invalid target of augmented assignment");
}

static void gen_assign(F *f, Stmt *s, AAssign *a){
    if(!a->value) return;
    if(a->aug){ gen_augassign(f,a,s->line); return; }
    Expr *v=a->value;
    int parallel=v->kind==EXPR_TUPLE;                         /* same rule as the checker */
    for(int k=0;k<a->ntarget && parallel;k++){ Expr *t=a->target[k]; if((t->kind!=EXPR_TUPLE&&t->kind!=EXPR_LIST)||t->count!=v->count) parallel=0; }
    if(parallel){                                             /* a, b = x, y: evaluate everything first */
        int n=v->count, slots[64];
        if(n>64) cg_fail(s->line,"too many values");
        for(int i=0;i<n;i++){ Ty *t=TY(v->items[i]); int o=gen(f,v->items[i]); slots[i]=park_value(f,t,o); }
        for(int k=0;k<a->ntarget;k++) for(int i=0;i<n;i++){
            Ty *t=TY(v->items[i]); load_parked(f,t,slots[i]); store_target(f,a->target[k]->items[i],t,s->line); }
        return;
    }
    Ty *t=TY(v);
    if(a->ntarget==1){
        Expr *tg=a->target[0];
        if(v->kind==EXPR_NONE){ Ty *want=TY(tg); if(tg->kind==EXPR_NAME||tg->kind==EXPR_ATTRIBUTE||tg->kind==EXPR_INDEX){ gen_none(f,want); store_target(f,tg,want,s->line); return; } }
        gen_owned(f,v); store_target(f,tg,t,s->line); return;
    }
    int o=gen(f,v), slot=park_value(f,t,o);
    for(int k=0;k<a->ntarget;k++){ load_parked(f,t,slot); store_target(f,a->target[k],t,s->line); }
}

static AModule *module_named(const char *name){
    for(int i=0;i<gg->p->nmods;i++) if(!strcmp(gg->p->mods[i]->name,name)) return gg->p->mods[i];
    return NULL;
}
/* run an imported module's body the first time it is imported */
static void init_module(F *f, AModule *m){
    if(!m || !m->unit->ast || m->index==0 || !m->body) return;
    use_fn(m->body); m->used=1;
    int l=new_label();
    E(f,"cmp byte [%s],0",module_label(m)); E(f,"jne L%d",l);
    E(f,"mov byte [%s],1",module_label(m)); E(f,"call %s",fnl(m->body,LB_CODE));
    LBL(f,l);
}
static void init_prefixes(F *f, const char *dotted){
    char buf[300];
    for(const char *q=dotted;;q++){
        if(*q=='.'||!*q){ snprintf(buf,sizeof buf,"%.*s",(int)(q-dotted),dotted); init_module(f,module_named(buf)); }
        if(!*q) break;
    }
}
static void gen_import(F *f, Stmt *s){
    AotUnit *u=f->fn->mod->unit;
    if(s->kind==STMT_IMPORT){ init_prefixes(f,s->name2?s->name2:s->name); return; }
    char *dotted=aot_from_import_module(u,s);
    init_prefixes(f,dotted);
    AModule *t=module_named(dotted);
    for(int i=0;i<s->nnames;i++){
        const char *nm=s->names[i];
        if(!strcmp(nm,"*") || (t && symtab_find(&t->syms,nm))) continue;
        char sub[300]; snprintf(sub,sizeof sub,"%s.%s",dotted,nm);
        init_module(f,module_named(sub));
    }
    free(dotted);
}

static void gen_for(F *f, Stmt *s){
    AotUnit *u=f->fn->mod->unit;
    Expr *it=aot_expr(u,&s->expr);
    Iter I; iter_begin(f,it,&I);
    int lbreak=new_label();
    AVar *vars[16];
    for(int k=0;k<s->param_count && k<16;k++) vars[k]=find_var(f,s->params[k],s->line);
    iter_store_vars(f,&I,vars,s->param_count);
    if(f->nloops==64) cg_fail(s->line,"loops nested too deeply");
    f->loops[f->nloops].lcont=I.cont; f->loops[f->nloops].lbreak=lbreak; f->nloops++;
    gen_stmts(f,s->body,s->body_count);
    f->nloops--;
    iter_end(f,&I);
    LBL(f,I.exit);
    gen_stmts(f,s->orelse,s->orelse_count);
    LBL(f,lbreak);
    iter_release(f,&I);
}

/* ---- exceptions ---- */
static void push_handler(F *f, int rec, int label){
    rt("rt_throw");
    E(f,"lea eax,[ebp%+d]",rec); E(f,"mov ecx,[rt_exc_top]"); E(f,"mov [eax],ecx");
    E(f,"mov [eax+4],esp"); E(f,"mov [eax+8],ebp"); E(f,"mov dword [eax+12],L%d",label);
    E(f,"mov [eax+16],ebx"); E(f,"mov [eax+20],esi"); E(f,"mov [eax+24],edi");
    E(f,"mov [rt_exc_top],eax");
}
static void pop_handler(F *f, int rec){ E(f,"mov eax,[ebp%+d]",rec); E(f,"mov [rt_exc_top],eax"); }
/* Leaving try statements tries[to..] (return/break/continue): drop their
   handlers and run their finally blocks, innermost first. */
static void leave_tries(F *f, int to){
    for(int i=f->ntries-1;i>=to;i--){
        TryCtx ctx=f->tries[i];
        if(ctx.active) pop_handler(f,ctx.rec);
        if(ctx.nfin){ int save=f->ntries; f->ntries=i; gen_stmts(f,ctx.fin,ctx.nfin); f->ntries=save; }
    }
}
/* temporaries of statements interrupted by an exception */
static void release_temps_from(F *f, int mark){
    for(int k=mark;k<f->ntmp;k++){ rt("rt_decref"); E(f,"mov eax,[ebp%+d]",f->tmp[k]); E(f,"mov dword [ebp%+d],0",f->tmp[k]); E(f,"call rt_decref"); }
}
static void gen_try(F *f, Stmt *s){
    Stmt **fin=NULL; int nfin=0; Stmt *els=NULL;
    for(int i=0;i<s->orelse_count;i++){ Stmt *b=s->orelse[i]; if(b->block_tag==3){ fin=b->body; nfin=b->body_count; } else if(b->block_tag==2) els=b; }
    if(f->ntries==32) cg_fail(s->line,"try statements nested too deeply");
    int rec=frame_slot(f,28), exc=ref_slot(f), mark=f->tmp_used;
    int lhandler=new_label(), lfinexc=new_label(), ldone=new_label();
    TryCtx *ctx=&f->tries[f->ntries++];
    ctx->rec=rec; ctx->active=1; ctx->fin=fin; ctx->nfin=nfin; ctx->nloops=f->nloops;
    push_handler(f,rec,lhandler);
    gen_stmts(f,s->body,s->body_count);
    pop_handler(f,rec); f->tries[f->ntries-1].active=0;
    if(els){
        if(nfin){ push_handler(f,rec,lfinexc); f->tries[f->ntries-1].active=1; }
        gen_stmts(f,els->body,els->body_count);
        if(nfin){ pop_handler(f,rec); f->tries[f->ntries-1].active=0; }
    }
    E(f,"jmp L%d",ldone);
    /* an exception in the body: release what the interrupted statement held, keep the exception */
    LBL(f,lhandler);
    f->tries[f->ntries-1].active=0;
    release_temps_from(f,mark);
    E(f,"mov eax,[rt_exc_cur]"); E(f,"mov dword [rt_exc_cur],0"); E(f,"xchg eax,[ebp%+d]",exc); CALLRT(f,"rt_decref");
    if(nfin){ push_handler(f,rec,lfinexc); f->tries[f->ntries-1].active=1; }
    int nclause=0, clause_lbl[32]; Stmt *clauses[32];
    for(int i=0;i<s->orelse_count;i++){ Stmt *b=s->orelse[i]; if(b->block_tag!=1) continue;
        if(nclause==32) cg_fail(s->line,"too many except clauses");
        int l=new_label(); clause_lbl[nclause]=l; clauses[nclause++]=b;
        if(!b->expr){ E(f,"jmp L%d",l); continue; }
        Expr *te=b->expr; int n=te->kind==EXPR_TUPLE?te->count:1;
        for(int k=0;k<n;k++){ AClass *cls=xinfo(te->kind==EXPR_TUPLE?te->items[k]:te)->cls; use_class(cls);
            E(f,"mov eax,[ebp%+d]",exc); E(f,"mov edx,%s",class_label(cls,0)); CALLRT(f,"rt_isinstance"); E(f,"test eax,eax"); E(f,"jnz L%d",l); }
    }
    /* no clause matches: finally, then on to the enclosing handler */
    if(nfin){ pop_handler(f,rec); f->tries[f->ntries-1].active=0; }
    f->ntries--;
    if(nfin) gen_stmts(f,fin,nfin);
    E(f,"mov eax,[ebp%+d]",exc); E(f,"mov dword [ebp%+d],0",exc); CALLRT(f,"rt_throw");
    f->ntries++;
    for(int i=0;i<nclause;i++){ Stmt *b=clauses[i];
        LBL(f,clause_lbl[i]);
        f->tries[f->ntries-1].active=nfin?1:0;
        if(b->name){ AVar *v=(AVar*)b->aux; E(f,"mov eax,[ebp%+d]",exc); incref(f); store_var(f,v); }
        if(f->nexcs==32) cg_fail(b->line,"except clauses nested too deeply");
        f->excs[f->nexcs++]=exc;
        gen_stmts(f,b->body,b->body_count);
        f->nexcs--;
        if(nfin){ pop_handler(f,rec); f->tries[f->ntries-1].active=0; }
        E(f,"mov eax,[ebp%+d]",exc); E(f,"mov dword [ebp%+d],0",exc); CALLRT(f,"rt_decref");
        E(f,"jmp L%d",ldone);
    }
    f->ntries--;
    if(nfin){                                            /* an exception in an except/else block: finally, re-raise */
        LBL(f,lfinexc);
        release_temps_from(f,mark);
        gen_stmts(f,fin,nfin);
        E(f,"mov eax,[rt_exc_cur]"); E(f,"mov dword [rt_exc_cur],0"); CALLRT(f,"rt_throw");
    }
    LBL(f,ldone);
    if(nfin) gen_stmts(f,fin,nfin);
}
static int loop_tries(F *f){ int i=f->ntries; while(i>0 && f->tries[i-1].nloops>=f->nloops) i--; return i; }

static void moves_begin(F *f, Stmt *s);
/* the function of a module-level def statement (decorated: under its hidden name " name") */
static AFunc *module_def_fn(F *f, Stmt *s){
    ASym *x=symtab_find(&f->fn->mod->syms,s->name);
    if(!x || x->kind!=AS_FUNC){ char h[300]; snprintf(h,sizeof h," %s",s->name); x=symtab_find(&f->fn->mod->syms,h); }
    return x && x->kind==AS_FUNC ? (AFunc*)x->p : NULL;
}
static void gen_stmt(F *f, Stmt *s){
    AotUnit *u=f->fn->mod->unit;
    int mark=scope_open(f);
    moves_begin(f,s);
    switch(s->kind){
        case STMT_EXPR:{
            APrint *pr=aot_print(u,s);
            if(pr){ gen_print(f,pr); break; }
            if(aot_is_annotation_only(u,s)){ gen_assign(f,s,aot_assign(u,s)); break; }
            Expr *e=aot_expr(u,&s->expr);
            if(is_str_lit(e)) break;                          /* docstring */
            if(e->kind==EXPR_CALL && xinfo(e)->kind==X_SYSCALL){ gen_syscall_regs(f,e); E(f,"add esp,28"); break; }   /* result unused: no list */
            int o=gen(f,e); drop_value(f,TY(e),o);
            break; }
        case STMT_ASSIGN: gen_assign(f,s,aot_assign(u,s)); break;
        case STMT_IF:{
            int lelse=new_label();
            gen_jump(f,aot_expr(u,&s->expr),lelse,0);
            gen_stmts(f,s->body,s->body_count);
            if(s->orelse_count){ int lend=new_label(); E(f,"jmp L%d",lend); LBL(f,lelse); gen_stmts(f,s->orelse,s->orelse_count); LBL(f,lend); }
            else LBL(f,lelse);
            break; }
        case STMT_WHILE:{
            int ltop=new_label(), lelse=new_label(), lbreak=new_label();
            LBL(f,ltop);
            gen_jump(f,aot_expr(u,&s->expr),lelse,0);
            if(f->nloops==64) cg_fail(s->line,"loops nested too deeply");
            f->loops[f->nloops].lcont=ltop; f->loops[f->nloops].lbreak=lbreak; f->nloops++;
            gen_stmts(f,s->body,s->body_count);
            f->nloops--;
            E(f,"jmp L%d",ltop);
            LBL(f,lelse);
            gen_stmts(f,s->orelse,s->orelse_count);
            LBL(f,lbreak);
            break; }
        case STMT_FOR: gen_for(f,s); break;
        case STMT_BREAK: if(!f->nloops) cg_fail(s->line,"'break' outside a loop"); leave_tries(f,loop_tries(f)); E(f,"jmp L%d",f->loops[f->nloops-1].lbreak); break;
        case STMT_CONTINUE: if(!f->nloops) cg_fail(s->line,"'continue' outside a loop"); leave_tries(f,loop_tries(f)); E(f,"jmp L%d",f->loops[f->nloops-1].lcont); break;
        case STMT_TRY: gen_try(f,s); break;
        case STMT_RETURN:{
            Ty *rt_=ty_find(f->ret);
            if(s->expr && f->fn->is_gen){                         /* a generator's return value: kept in it */
                Ty *gr=gpart(f->fn->ret,1); Expr *e=aot_expr(u,&s->expr);
                gen_as(f,e,gr,1); E(f,"mov ecx,[rt_cur_gen]"); store_mem(f,gr,"ecx",100); E(f,"mov ecx,[rt_cur_gen]"); E(f,"mov dword [ecx+108],1");
            } else if(s->expr){
                Expr *e=aot_expr(u,&s->expr);
                if(rt_->k==TY_VOID){ int o=gen(f,e); drop_value(f,TY(e),o); }
                else gen_as(f,e,rt_,1);
            } else if(rt_->k!=TY_VOID){ int o=gen_zero(f,rt_); if(is_ptr(rt_) && !o) incref(f); }
            if(f->tmp_used>mark){ if(!is_flt(rt_)) push_value(f,rt_); scope_close(f,mark); if(!is_flt(rt_)) pop_value(f,rt_); }
            if(f->ntries){                                    /* finally blocks run first; keep the value meanwhile */
                int keep=frame_slot(f,8);
                if(rt_->k!=TY_VOID) store_new(f,rt_,"ebp",keep);
                leave_tries(f,0);
                if(rt_->k!=TY_VOID) load_mem(f,rt_,"ebp",keep);
            }
            E(f,"jmp L%d",f->lret);
            break; }
        case STMT_DEL:{
            ADel *d=aot_del(u,s);
            for(int i=0;i<d->n;i++){ Expr *t=d->t[i];
                if(t->kind==EXPR_NAME){ AVar *v=xinfo(t)->var; char a[320], b[320];       /* del x */
                    check_bound(f,v); var_addr(a,sizeof a,v); var_addr(b,sizeof b,v->bound);
                    E(f,"mov dword %s,0",b);
                    if(is_ptr(v->ty)){ E(f,"xor eax,eax"); E(f,"xchg eax,%s",a); CALLRT(f,"rt_decref"); }
                    continue; }
                if(t->kind==EXPR_SLICE){ Expr *parts[3]={t->b,t->c,t->d}; int flags=0;   /* del xs[a:b:c] */
                    E(f,"sub esp,20"); gen_borrow(f,t->a); check_none(f,TY(t->a),"TypeError","'NoneType' object does not support item deletion"); E(f,"mov [esp],eax");
                    for(int k=0;k<3;k++){
                        if(parts[k] && parts[k]->kind!=EXPR_NONE){ gen(f,parts[k]); to_i32(f,TY(parts[k])); E(f,"mov [esp+%d],eax",4+4*k); }
                        else { flags|=1<<k; E(f,"mov dword [esp+%d],0",4+4*k); } }
                    E(f,"mov dword [esp+16],%d",flags); CALLRT(f,"rt_list_del_slice"); E(f,"add esp,20");
                    continue; }
                Ty *ct=TY(t->a), *el=ct->elem; int dict=ct->k==TY_DICT;
                gen_borrow(f,t->a); check_none(f,ct,"TypeError","'NoneType' object does not support item deletion"); E(f,"push eax");
                if(dict){ Ty *kt=ty_dkey(ct); gen_as(f,t->b,kt,0); push_value(f,kt);
                    E(f,"mov edx,esp"); E(f,"mov eax,[esp+%d]",esize(kt)); CALLRT(f,"rt_dict_del"); E(f,"add esp,%d",esize(kt)+4); }
                else { gen(f,t->b); to_i32(f,TY(t->b)); E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_list_pop"); }
                if(is_ptr(el)){ E(f,"mov eax,[eax]"); CALLRT(f,"rt_decref"); }
            }
            break; }
        case STMT_ASSERT:{
            int lok=new_label();
            gen_jump(f,aot_expr(u,&s->expr),lok,1);
            if(s->expr2) gen_str_of(f,aot_expr(u,&s->expr2)); else E(f,"xor eax,eax");
            CALLRT(f,"rt_panic_assert");
            LBL(f,lok);
            break; }
        case STMT_RAISE:{
            if(!s->expr){                                        /* re-raise the exception being handled */
                int slot=f->excs[f->nexcs-1];
                E(f,"mov eax,[ebp%+d]",slot); incref(f); E(f,"mov eax,[ebp%+d]",slot); CALLRT(f,"rt_throw"); break; }
            Expr *e=aot_expr(u,&s->expr);
            if(e->kind==EXPR_NAME && xinfo(e)->kind==X_CTOR) gen_ctor(f,e,xinfo(e));   /* raise ValueError */
            else gen_owned(f,e);
            AClass *xc=ty_find(TY(e))->cls; AField *fcause=aot_find_field(xc,"__cause__"), *fctx=aot_find_field(xc,"__context__"), *fsup=aot_find_field(xc,"__suppress_context__");
            if(s->expr2 && fcause){                          /* raise X from Y: X.__cause__ = Y */
                Expr *y=aot_expr(u,&s->expr2);
                E(f,"push eax");
                if(y->kind==EXPR_NONE) E(f,"xor eax,eax");
                else if(y->kind==EXPR_NAME && xinfo(y)->kind==X_CTOR) gen_ctor(f,y,xinfo(y));
                else gen_owned(f,y);
                E(f,"mov ecx,[esp]"); store_mem(f,fcause->ty,"ecx",fcause->offset);
                E(f,"mov ecx,[esp]"); E(f,"mov dword [ecx+%d],1",fsup->offset);
                E(f,"pop eax"); }
            if(f->nexcs>0 && fctx){                          /* raised while handling another: its __context__ */
                int l=new_label(); E(f,"mov ecx,[ebp%+d]",f->excs[f->nexcs-1]);
                E(f,"test ecx,ecx"); E(f,"jz L%d",l); E(f,"cmp ecx,eax"); E(f,"je L%d",l); E(f,"cmp dword [eax+%d],0",fctx->offset); E(f,"jne L%d",l);
                E(f,"inc dword [ecx]"); E(f,"mov [eax+%d],ecx",fctx->offset); LBL(f,l); }
            CALLRT(f,"rt_throw");
            break; }
        case STMT_WITH:{                                      /* with open(...) as f: the file is closed at the end */
            AWith *w=aot_with(u,s); int slots[8];
            for(int i=0;i<w->n;i++){
                Ty *t=TY(w->e[i]); slots[i]=0;
                if(t->k==TY_FILE){ slots[i]=ref_slot(f); gen_owned(f,w->e[i]); E(f,"mov [ebp%+d],eax",slots[i]);
                    if(w->as[i]){ incref(f); store_target(f,&(Expr){.kind=EXPR_NAME,.name=w->as[i]},t,s->line); } }
                else if(w->as[i]){ gen_owned(f,w->e[i]); store_target(f,&(Expr){.kind=EXPR_NAME,.name=w->as[i]},t,s->line); }
                else { int o=gen(f,w->e[i]); drop_value(f,t,o); }
            }
            gen_stmts(f,s->body,s->body_count);
            for(int i=w->n-1;i>=0;i--) if(slots[i]){
                E(f,"mov eax,[ebp%+d]",slots[i]); CALLRT(f,"rt_file_close");
                E(f,"mov eax,[ebp%+d]",slots[i]); E(f,"mov dword [ebp%+d],0",slots[i]); CALLRT(f,"rt_decref"); }
            break; }
        case STMT_IMPORT: case STMT_FROM_IMPORT: gen_import(f,s); break;
        case STMT_FUNCTION_DEF:
            if(!is_fn_body(f->fn)){ AFunc *mf=module_def_fn(f,s);       /* module level: its defaults, evaluated now */
                if(mf && has_defaults(mf)){
                    for(int q=-1;q<mf->ninsts;q++){ AFunc *g=q<0?mf:mf->insts[q];  /* (a generic function: the instances calls use) */
                        if(q<0 && mf->pristine && !mf->direct) continue;
                        gg->fv_used[g->id]=1; rt("rt_static"); fill_defaults(f,g,fnl(g,LB_VALUE)); } } }   /* (its code only if something calls it) */
            if(s->expr2){                                /* decorated: name = d1(d2(function)) */
                if(is_fn_body(f->fn) && ((AFunc*)s->aux)->unused) break;
                gen_owned(f,s->expr2); store_var(f,find_var(f,s->name,s->line));
            } else if(is_fn_body(f->fn)){                /* a nested def: its closure into its variable */
                AFunc *inner=(AFunc*)s->aux;
                if(inner->unused) break;
                AVar *v=find_var(f,s->name,s->line);
                int o=gen_funcref(f,inner); if(!o) incref(f);
                store_var(f,v);
            }
            break;
        case STMT_CLASS_DEF:                             /* attributes; methods' defaults; decorated methods: Class.name = decorators(method) */
            for(int q=-1;q<((AClass*)s->aux)->ninsts;q++){ AClass *cls=q<0?(AClass*)s->aux:((AClass*)s->aux)->insts[q];   /* (and a generic class's instances) */
                for(int b=0;b<cls->def->body_count;b++){ Stmt *bs=cls->def->body[b];      /* the body, in order */
                    if(bs->kind==STMT_ASSIGN){ AAssign *a=aot_assign(u,bs);             /* name = value: the class attribute */
                        for(int k=0;k<cls->nfields;k++){ AField *fd=cls->fields[k];
                            if(fd->cvar && fd->init==a->value){ gen_as(f,fd->init,fd->ty,1); store_var(f,fd->cvar); } } }
                    for(int k=0;k<cls->nmethods;k++){ AFunc *m=cls->methods[k];
                        if(m->def!=bs) continue;
                        if(has_defaults(m)) for(int q=-1;q<m->ninsts;q++){ AFunc *g=q<0?m:m->insts[q];      /* its defaults, evaluated now (its code: if called); */
                            gg->fv_used[g->id]=1; rt("rt_static"); fill_defaults(f,g,fnl(g,LB_VALUE)); }      /* (a generic method's copies too) */
                        if(m->decovar){ gen_owned(f,m->def->expr2); store_var(f,m->decovar); } }
                    if(bs->kind==STMT_CLASS_DEF) gen_stmt(f,bs); }                 /* a nested class */
            }
            break;
        case STMT_YIELD:{
            Ty *yt=f->fn->yield_ty;
            if(!s->expr){ int o=gen_zero(f,yt); if(is_ptr(yt)&&!o) incref(f); gen_yield_value(f,yt); drop_sent(f); break; }
            Expr *e=aot_expr(u,&s->expr);
            if(s->block_tag==7){                          /* yield from: each item */
                if(ty_find(TY(e))->k==TY_GEN){ gen_yield_from(f,e,0); break; }
                Iter I; iter_begin(f,e,&I);
                iter_value(f,&I,0); if(is_ptr(I.elem)) incref(f); conv(f,I.elem,yt);
                gen_yield_value(f,yt); drop_sent(f);
                iter_end(f,&I); LBL(f,I.exit); iter_release(f,&I);
                break;
            }
            gen_as(f,e,yt,1); gen_yield_value(f,yt); drop_sent(f);
            break; }
        case STMT_NONLOCAL: case STMT_PASS: case STMT_GLOBAL: break;
        default: cg_fail(s->line,"unsupported statement");
    }
    f->live_after=NULL;
    scope_close(f,mark);
}
static void gen_stmts(F *f, Stmt **b, int n){ for(int i=0;i<n;i++) gen_stmt(f,b[i]); }

/* ---------------------------------------------------------------- functions and classes */

/* ---------------------------------------------------------------- reference counts the compiler can drop
   A loop variable normally holds a reference of its own to the current item
   (incref when it takes the item, decref when it moves on). It can simply
   borrow the item from the container when the container keeps every item
   alive for the whole loop: nobody else can reach it (a fresh list, a fresh
   generator), it cannot change (str, tuple), or the loop's body cannot remove
   anything from any container (no user code runs, no pop/remove/del/x[i]=...).
   The variable must also be used only inside the loops that bind it. */

static int fmt_pure(Ty *t){                      /* formatting t runs no user code (__str__/__repr__) */
    t=ty_find(t);
    switch(t->k){
        case TY_OBJ: for(AClass *c=t->cls;c;c=c->base) for(int i=0;i<c->nmethods;i++)
                         if(!strcmp(c->methods[i]->name,"__str__")||!strcmp(c->methods[i]->name,"__repr__")) return 0;
                     return 1;
        case TY_DICT: return fmt_pure(ty_dkey(t)) && fmt_pure(t->elem);
        case TY_LIST: case TY_SET: return fmt_pure(t->elem);
        case TY_TUPLE: for(int i=0;i<t->nelems;i++) if(!fmt_pure(t->elems[i])) return 0; return 1;
        default: return 1;
    }
}
static int iterates_code(Ty *t){ t=ty_find(t); return t->k==TY_GEN||t->k==TY_OBJ; }   /* iterating it runs user code */
static int pure_expr(Expr *e);
static int pure_exprs(Expr **v, int n){ for(int i=0;i<n;i++) if(!pure_expr(v[i])) return 0; return 1; }
static int pure_call(Expr *e){
    XInfo *xi=xinfo(e);
    if(!pure_exprs(e->items,e->count)) return 0;
    const char *n=xi->name?xi->name:"";
    switch(xi->kind){
        case X_BUILTIN:
            if(!strcmp(n,"next")) return 0;
            if(!strcmp(n,"len") && xi->fn) return 0;                     /* __len__ */
            if((!strcmp(n,"str")||!strcmp(n,"repr")||!strcmp(n,"format")) && e->count && !fmt_pure(TY(e->items[0]))) return 0;
            if(xi->key){ if(e->a && xi->key->kind==EXPR_LAMBDA) { if(!pure_expr(xi->key->a)) return 0; } else if(strcmp(n,"reduce")) return 0; }
            if(!strcmp(n,"reduce") && !pure_expr(xi->key)) return 0;
            for(int i=0;i<e->count;i++){ Ty *it=TY(e->items[i]); if(it && it->k==TY_GEN) return 0; }   /* draining a generator runs its code (a format spec: no type) */
            return 1;
        case X_TMETHOD:{
            static const char *removing[]={"pop","remove","clear","discard","update",NULL};
            for(int i=0;removing[i];i++) if(!strcmp(n,removing[i])) return 0;
            if(!strcmp(n,"sort") && xi->key && (xi->key->kind!=EXPR_LAMBDA || !pure_expr(xi->key->a))) return 0;
            if(!strcmp(n,"format")) for(int i=0;i<e->count;i++) if(!fmt_pure(TY(e->items[i]))) return 0;
            return pure_expr(e->a->a); }
        case X_SYS: case X_SYSCALL: return 1;
        case X_BMOD: return pure_expr(e->a);
        case X_CTOR: return !xi->fn && xi->cls && aot_is_exception(xi->cls) && (!e->count || fmt_pure(TY(e->items[0])));
        default: return 0;                                              /* user code */
    }
}
static int pure_expr(Expr *e){
    if(!e) return 1;
    XInfo *xi=(XInfo*)e->ty;
    XKind k=xi?xi->kind:X_NONE;
    switch(e->kind){
        case EXPR_LITERAL: case EXPR_TRUE: case EXPR_FALSE: case EXPR_NONE: case EXPR_NAME: case EXPR_LAMBDA: return 1;
        case EXPR_UNARY: return k!=X_OPMETHOD && pure_expr(e->a);
        case EXPR_BINARY:
            if(k==X_OPMETHOD) return 0;
            if(e->op==T_PERCENT && TY(e)->k==TY_STR){
                if(e->b->kind==EXPR_TUPLE){ for(int i=0;i<e->b->count;i++) if(!fmt_pure(TY(e->b->items[i]))) return 0; }
                else if(!fmt_pure(TY(e->b))) return 0; }
            return pure_expr(e->a) && pure_expr(e->b);
        case EXPR_BOOL: return pure_expr(e->a) && pure_expr(e->b);
        case EXPR_COMPARE: if(xi && xi->cmpfn) for(int i=0;i<e->count;i++) if(xi->cmpfn[i]) return 0;
            return pure_exprs(e->items,e->count);
        case EXPR_TERNARY: return pure_expr(e->a) && pure_expr(e->b) && pure_expr(e->c);
        case EXPR_ATTRIBUTE: return k!=X_PROP && pure_expr(e->a);
        case EXPR_INDEX: return k!=X_OPMETHOD && pure_expr(e->a) && pure_expr(e->b);
        case EXPR_SLICE: return pure_expr(e->a) && pure_expr(e->b) && pure_expr(e->c) && pure_expr(e->d);
        case EXPR_LIST: case EXPR_SET: case EXPR_TUPLE: return pure_exprs(e->items,e->count);
        case EXPR_DICT: return pure_exprs(e->items,e->count) && pure_exprs(e->vals,e->count);
        case EXPR_COMPREHENSION:
            if(e->comp_kind=='G') return pure_expr(e->clauses[0].iter);       /* making the generator runs nothing else */
            for(int i=0;i<e->nclause;i++){ CompClause *cl=&e->clauses[i];
                if(!pure_expr(cl->iter) || iterates_code(TY(cl->iter)) || !pure_exprs(cl->conds,cl->ncond)) return 0; }
            return pure_expr(e->a) && pure_expr(e->b);
        case EXPR_CALL: return pure_call(e);
        default: return 0;
    }
}
static int pure_stmts(AotUnit *u, Stmt **b, int n);
static int pure_target(Expr *t){
    switch(t->kind){
        case EXPR_NAME: return 1;
        case EXPR_ATTRIBUTE: return xinfo(t)->kind!=X_PROP && pure_expr(t->a);   /* a field's old value goes, items stay */
        case EXPR_TUPLE: case EXPR_LIST: for(int i=0;i<t->count;i++) if(!pure_target(t->items[i])) return 0; return 1;
        default: return 0;                                                     /* x[i] = ... replaces an item */
    }
}
static int pure_stmt(AotUnit *u, Stmt *s){
    switch(s->kind){
        case STMT_EXPR:{
            APrint *pr=aot_print(u,s);
            if(pr){ for(int i=0;i<pr->n;i++){ if(!pure_expr(pr->args[i])) return 0;
                        Ty *t=TY(pr->args[i]); if(pr->star[i]){ if(iterates_code(t)) return 0; t=ty_find(t)->k==TY_TUPLE?t:ty_find(t)->elem; }
                        if(!fmt_pure(t)) return 0; }
                    return pure_expr(pr->sep) && pure_expr(pr->end); }
            if(aot_is_annotation_only(u,s)) return 1;
            return pure_expr(s->expr); }
        case STMT_ASSIGN:{ AAssign *a=aot_assign(u,s);
            for(int k=0;k<a->ntarget;k++) if(!pure_target(a->target[k])) return 0;
            if(a->opfn) return 0;
            return pure_expr(a->value); }
        case STMT_IF: case STMT_WHILE: return pure_expr(s->expr) && pure_stmts(u,s->body,s->body_count) && pure_stmts(u,s->orelse,s->orelse_count);
        case STMT_FOR:{ Expr *it=s->expr; XInfo *xi=xinfo(it);
            if(xi->kind==X_BUILTIN && (!strcmp(xi->name,"enumerate")||!strcmp(xi->name,"zip"))){ for(int i=0;i<it->count;i++) if(iterates_code(TY(it->items[i]))) return 0; }
            else if(!(xi->kind==X_TMETHOD && !strcmp(xi->name,"items")) && iterates_code(TY(it))) return 0;
            return pure_expr(it) && pure_stmts(u,s->body,s->body_count) && pure_stmts(u,s->orelse,s->orelse_count); }
        case STMT_RETURN: case STMT_ASSERT: case STMT_RAISE: return pure_expr(s->expr) && pure_expr(s->expr2);
        case STMT_TRY: case STMT_BLOCK: return pure_stmts(u,s->body,s->body_count) && pure_stmts(u,s->orelse,s->orelse_count);
        case STMT_WITH:{ AWith *w=aot_with(u,s); for(int i=0;i<w->n;i++) if(!pure_expr(w->e[i])) return 0; return pure_stmts(u,s->body,s->body_count); }
        case STMT_PASS: case STMT_BREAK: case STMT_CONTINUE: case STMT_GLOBAL: case STMT_NONLOCAL: return 1;
        case STMT_FUNCTION_DEF: return s->expr2==NULL;
        default: return 0;                                                     /* yield (the consumer runs), del, ... */
    }
}
static int pure_stmts(AotUnit *u, Stmt **b, int n){ for(int i=0;i<n;i++) if(!pure_stmt(u,b[i])) return 0; return 1; }

/* an expression giving a new object nothing else refers to */
static int fresh_expr(Expr *e){
    switch(e->kind){
        case EXPR_COMPREHENSION: case EXPR_LIST: case EXPR_SET: case EXPR_DICT: case EXPR_TUPLE: case EXPR_SLICE: return 1;
        case EXPR_BINARY: return xinfo(e)->kind!=X_OPMETHOD && TY(e)->k==TY_LIST;
        case EXPR_CALL:{ XInfo *xi=xinfo(e); const char *n=xi->name?xi->name:"";
            if(xi->kind==X_BUILTIN){ static const char *fresh[]={"sorted","list","set","reversed","range_list","dict_copy",NULL};
                for(int i=0;fresh[i];i++) if(!strcmp(n,fresh[i])) return 1; return 0; }
            if(xi->kind==X_TMETHOD){ static const char *fresh[]={"split","splitlines","keys","values","items","copy","union","intersection","difference","readlines",NULL};
                for(int i=0;fresh[i];i++) if(!strcmp(n,fresh[i])) return 1; return 0; }
            return (xi->kind==X_FUNC||xi->kind==X_METHOD||xi->kind==X_STATIC||xi->kind==X_CALLNEST||xi->kind==X_SUPER) && xi->fn && xi->fn->is_gen; }
        default: return 0;
    }
}
/* the items of x stay alive for the whole loop, whatever the loop does */
static int src_safe(Expr *x){
    Ty *t=TY(x);
    if(t->k==TY_STR||t->k==TY_BYTES||t->k==TY_TUPLE||t->k==TY_FILE) return 1;              /* static 1-char strings; immutable; a fresh list of lines */
    if(t->k==TY_OBJ) return 0;
    return fresh_expr(x);
}
static int iter_safe(Expr *it){
    XInfo *xi=xinfo(it);
    if(xi->kind==X_BUILTIN && (!strcmp(xi->name,"range")||!strcmp(xi->name,"reversed_range"))) return 1;
    if(xi->kind==X_BUILTIN && (!strcmp(xi->name,"enumerate")||!strcmp(xi->name,"zip"))) return src_safe(it->items[0]) && (xi->name[0]=='e' || src_safe(it->items[1]));
    if(xi->kind==X_TMETHOD && !strcmp(xi->name,"items") && ty_find(xi->ty)->k==TY_VOID) return src_safe(it->a->a);
    return src_safe(it);
}

typedef struct { AFunc *fn; AotUnit *u; char *only, *ok; AVar *stack[64]; int nstack; } Plan;
static int plan_index(Plan *pl, AVar *v){ return v && !v->src && !v->global && v->owner==pl->fn && v->id>=0 && v->id<pl->fn->nvars && pl->fn->vars[v->id]==v ? v->id : -1; }
static int plan_active(Plan *pl, AVar *v){ for(int i=0;i<pl->nstack;i++) if(pl->stack[i]==v) return 1; return 0; }
static void plan_use(Plan *pl, AVar *v){ int i=plan_index(pl,v); if(i>=0 && !plan_active(pl,v)) pl->only[i]=0; }
static void plan_store(Plan *pl, AVar *v){ int i=plan_index(pl,v); if(i>=0) pl->only[i]=0; }
static void plan_expr(Plan *pl, Expr *e);
static void plan_target(Plan *pl, Expr *t);
static int xi_of_iter(Expr *it);
/* loop variables vars (n), bound per item; safe: the items stay alive meanwhile; then the body */
static void plan_bind(Plan *pl, AVar **vars, int n, int safe){
    for(int k=0;k<n;k++){ int i=plan_index(pl,vars[k]); if(i<0) continue;
        if(plan_active(pl,vars[k])) pl->only[i]=0;                              /* bound again inside its own loop */
        if(!safe) pl->ok[i]=0; }
    for(int k=0;k<n && pl->nstack<64;k++) pl->stack[pl->nstack++]=vars[k];
}
static void plan_comp(Plan *pl, Expr *e){                                       /* a comprehension's clauses, then its items */
    XInfo *xi=xinfo(e); int save=pl->nstack;
    for(int i=0;i<e->nclause;i++){ CompClause *cl=&e->clauses[i];
        plan_expr(pl,cl->iter);
        int safe=iter_safe(cl->iter);
        if(!safe){ safe=pure_expr(e->a) && pure_expr(e->b);                    /* the rest of the comprehension runs per item */
            for(int k=i;k<e->nclause && safe;k++){ CompClause *c2=&e->clauses[k];
                if(!pure_exprs(c2->conds,c2->ncond) || (k>i && (!pure_expr(c2->iter) || iterates_code(TY(c2->iter))))) safe=0; } }
        if(cl->nvars==1 && xi_of_iter(cl->iter)) plan_store(pl,xi->cvars[2*i]); else plan_bind(pl,&xi->cvars[2*i],cl->nvars,safe);
        if(cl->target) plan_target(pl,cl->target);
        if(cl->target2) plan_target(pl,cl->target2);
        for(int k=0;k<cl->ncond;k++) plan_expr(pl,cl->conds[k]);
    }
    plan_expr(pl,e->a); plan_expr(pl,e->b);
    pl->nstack=save;
}
static void plan_expr(Plan *pl, Expr *e){
    if(!e) return;
    XInfo *xi=(XInfo*)e->ty;
    switch(e->kind){
        case EXPR_NAME: if(xi && xi->kind==X_VAR) plan_use(pl,xi->var); return;
        case EXPR_LAMBDA: return;                                               /* its own function */
        case EXPR_WALRUS: plan_expr(pl,e->a); plan_target(pl,e->b); return;
        case EXPR_COMPREHENSION:
            if(e->comp_kind=='G'){ if(xi->key) plan_expr(pl,e->clauses[0].iter); return; }   /* the rest is the generator's function */
            plan_comp(pl,e); return;
        case EXPR_CALL:
            plan_expr(pl,e->a);
            for(int i=0;i<e->count;i++) plan_expr(pl,e->items[i]);
            if(xi && xi->key && xi->key->kind==EXPR_LAMBDA && xinfo(xi->key)->var){  /* key=lambda k: ...: k takes each item */
                int save=pl->nstack; AVar *kv=xinfo(xi->key)->var;
                plan_bind(pl,&kv,1,pure_expr(xi->key->a));
                plan_expr(pl,xi->key->a); pl->nstack=save; }
            else if(xi && xi->key && xi->kind==X_BUILTIN && xi->name && !strcmp(xi->name,"reduce")){
                int save=pl->nstack;
                plan_store(pl,xi->cvars[0]);                                    /* the accumulator takes results */
                plan_bind(pl,&xi->cvars[1],1,pure_expr(xi->key));               /* the item: one per element */
                plan_expr(pl,xi->key); pl->nstack=save; }
            else if(xi && xi->key) plan_expr(pl,xi->key);
            if(xi && xi->rev) plan_expr(pl,xi->rev);
            return;
        default:
            plan_expr(pl,e->a); plan_expr(pl,e->b); plan_expr(pl,e->c); plan_expr(pl,e->d);
            for(int i=0;i<e->count;i++) plan_expr(pl,e->items[i]);
            for(int i=0;i<e->vcount;i++) plan_expr(pl,e->vals[i]);
            return;
    }
}
static void plan_target(Plan *pl, Expr *t){
    if(t->kind==EXPR_NAME){ XInfo *xi=(XInfo*)t->ty; if(xi && xi->var) plan_store(pl,xi->var); else plan_store(pl,NULL); return; }
    if(t->kind==EXPR_TUPLE||t->kind==EXPR_LIST){ for(int i=0;i<t->count;i++) plan_target(pl,t->items[i]); return; }
    plan_expr(pl,t->a); plan_expr(pl,t->b);
}
static void plan_stmts(Plan *pl, F *f, Stmt **b, int n);
/* 1 when iterating it gives a tuple made per item to a single variable (zip, enumerate, items) */
static int xi_of_iter(Expr *it){ XInfo *xi=xinfo(it);
    return (xi->kind==X_BUILTIN && xi->name && (!strcmp(xi->name,"enumerate")||!strcmp(xi->name,"zip"))) || (xi->kind==X_TMETHOD && xi->name && !strcmp(xi->name,"items")); }
static void plan_stmt(Plan *pl, F *f, Stmt *s){
    AotUnit *u=pl->u;
    switch(s->kind){
        case STMT_EXPR:{ APrint *pr=aot_print(u,s);
            if(pr){ for(int i=0;i<pr->n;i++) plan_expr(pl,pr->args[i]); plan_expr(pl,pr->sep); plan_expr(pl,pr->end); return; }
            if(aot_is_annotation_only(u,s)){ AAssign *a=aot_assign(u,s); for(int k=0;k<a->ntarget;k++) plan_target(pl,a->target[k]); return; }
            plan_expr(pl,s->expr); return; }
        case STMT_ASSIGN:{ AAssign *a=aot_assign(u,s);
            for(int k=0;k<a->ntarget;k++){ plan_target(pl,a->target[k]); if(a->aug) plan_expr(pl,a->target[k]); }
            plan_expr(pl,a->value); return; }
        case STMT_FOR:{
            Expr *it=s->expr; plan_expr(pl,it);
            AVar *vars[16]; int n=s->param_count<16?s->param_count:16;
            for(int k=0;k<n;k++) vars[k]=find_var(f,s->params[k],s->line);
            int safe=iter_safe(it) || (pure_stmts(u,s->body,s->body_count) && pure_stmts(u,s->orelse,s->orelse_count));
            int save=pl->nstack;
            if(n==1 && ((xi_of_iter(it)==1))) plan_store(pl,vars[0]);              /* for t in zip(...): a new tuple, owned */
            else plan_bind(pl,vars,n,safe);
            plan_stmts(pl,f,s->body,s->body_count); plan_stmts(pl,f,s->orelse,s->orelse_count);
            pl->nstack=save; return; }
        case STMT_WITH:{ AWith *w=aot_with(u,s);
            for(int i=0;i<w->n;i++){ plan_expr(pl,w->e[i]); if(w->as[i]) plan_store(pl,find_var(f,w->as[i],s->line)); }
            plan_stmts(pl,f,s->body,s->body_count); return; }
        case STMT_FUNCTION_DEF:
            if(s->expr2) plan_expr(pl,s->expr2);
            if(is_fn_body(f->fn)) plan_store(pl,find_var(f,s->name,s->line));
            return;
        case STMT_TRY:
            plan_stmts(pl,f,s->body,s->body_count);
            for(int i=0;i<s->orelse_count;i++){ Stmt *b=s->orelse[i]; if(b->name && b->aux) plan_store(pl,(AVar*)b->aux); plan_stmts(pl,f,b->body,b->body_count); }
            return;
        case STMT_YIELD: case STMT_RETURN: case STMT_RAISE: case STMT_IF: case STMT_WHILE: case STMT_ASSERT: case STMT_DEL: case STMT_BLOCK:
            if(s->expr) plan_expr(pl,s->expr);
            if(s->kind==STMT_DEL){ ADel *d=aot_del(u,s); for(int i=0;i<d->n;i++) plan_expr(pl,d->t[i]); }
            if(s->expr2) plan_expr(pl,s->expr2);
            plan_stmts(pl,f,s->body,s->body_count); plan_stmts(pl,f,s->orelse,s->orelse_count); return;
        default: return;
    }
}
static void plan_stmts(Plan *pl, F *f, Stmt **b, int n){ for(int i=0;i<n;i++) plan_stmt(pl,f,b[i]); }
/* decide which of fn's loop variables borrow their items */
static void plan_borrowed(F *f){
    AFunc *fn=f->fn; if(!fn->nvars) return;
    Plan pl; memset(&pl,0,sizeof pl); pl.fn=fn; pl.u=fn->mod->unit;
    pl.only=(char*)xmalloc((size_t)fn->nvars); pl.ok=(char*)xmalloc((size_t)fn->nvars);
    for(int i=0;i<fn->nvars;i++){ AVar *v=fn->vars[i]; Ty *t=ty_find(v->ty);
        pl.only[i]=pl.ok[i]=(i>=fn->nparams && is_ptr(t) && t->k!=TY_FUNC && !v->cell && !v->captured && !v->bound); }
    if(fn->lam) plan_expr(&pl,fn->lam->a);
    else if(fn->genexp){                                                         /* its clauses: the body yields (anything may run) */
        Expr *e=fn->genexp; XInfo *xi=xinfo(e); int save=pl.nstack;
        for(int i=0;i<e->nclause;i++){ CompClause *cl=&e->clauses[i];
            Expr *it=(i==0&&xi->key)?xi->key:cl->iter; plan_expr(&pl,it);
            if(cl->nvars==1 && xi_of_iter(it)) plan_store(&pl,xi->cvars[2*i]); else plan_bind(&pl,&xi->cvars[2*i],cl->nvars,iter_safe(it));
            if(e->clauses[i].target) plan_target(&pl,e->clauses[i].target);
            if(e->clauses[i].target2) plan_target(&pl,e->clauses[i].target2);
            for(int k=0;k<cl->ncond;k++) plan_expr(&pl,cl->conds[k]); }
        plan_expr(&pl,e->a); pl.nstack=save;
    }
    else plan_stmts(&pl,f,fn->body,fn->nbody);
    for(int i=0;i<fn->nvars;i++) fn->vars[i]->borrowed=pl.only[i] && pl.ok[i];
    free(pl.only); free(pl.ok);
}
/* ---- moves: a local variable's reference handed over on its last use ----
   Backward liveness over the function's statements (loops to a fixpoint)
   gives, after each simple statement, the variables still read later. Where
   an owned value is wanted (y = x, return x, xs.append(x), yield x) and x is
   dead after the statement and occurs in it once, x's reference moves: the
   slot is cleared instead of an incref now and a decref later. */
typedef struct LiveTab { Stmt **keys; unsigned **vals; int cap, n, W; } LiveTab;
static unsigned **live_slot(LiveTab *t, Stmt *s){
    if(t->n*2>=t->cap){
        Stmt **ok=t->keys; unsigned **ov=t->vals; int oc=t->cap;
        t->cap=t->cap?t->cap*2:64; t->n=0;
        t->keys=(Stmt**)memset(xmalloc(sizeof(Stmt*)*(size_t)t->cap),0,sizeof(Stmt*)*(size_t)t->cap);
        t->vals=(unsigned**)memset(xmalloc(sizeof(unsigned*)*(size_t)t->cap),0,sizeof(unsigned*)*(size_t)t->cap);
        for(int i=0;i<oc;i++) if(ok[i]) *live_slot(t,ok[i])=ov[i];
        free(ok); free(ov);
    }
    unsigned h=(unsigned)(((uintptr_t)s>>4)*2654435761u)&(unsigned)(t->cap-1);
    while(t->keys[h] && t->keys[h]!=s) h=(h+1)&(unsigned)(t->cap-1);
    if(!t->keys[h]){ t->keys[h]=s; t->vals[h]=NULL; t->n++; }
    return &t->vals[h];
}
static unsigned *live_get(LiveTab *t, Stmt *s){
    if(!t || !t->cap) return NULL;
    unsigned h=(unsigned)(((uintptr_t)s>>4)*2654435761u)&(unsigned)(t->cap-1);
    while(t->keys[h]){ if(t->keys[h]==s) return t->vals[h]; h=(h+1)&(unsigned)(t->cap-1); }
    return NULL;
}
typedef struct { AFunc *fn; AotUnit *u; LiveTab *tab; int W; unsigned *brk[64], *cont[64]; int nloop; } Live;
static unsigned *bs_new(Live *L){ unsigned *b=(unsigned*)xmalloc(sizeof(unsigned)*(size_t)L->W); memset(b,0,sizeof(unsigned)*(size_t)L->W); return b; }
static unsigned *bs_dup(Live *L, unsigned *a){ unsigned *b=bs_new(L); memcpy(b,a,sizeof(unsigned)*(size_t)L->W); return b; }
static void bs_or(Live *L, unsigned *a, unsigned *b){ for(int i=0;i<L->W;i++) a[i]|=b[i]; }
static int bs_eq(Live *L, unsigned *a, unsigned *b){ return !memcmp(a,b,sizeof(unsigned)*(size_t)L->W); }
static int live_index(Live *L, AVar *v){ return v && !v->src && !v->global && v->owner==L->fn && v->id>=0 && v->id<L->fn->nvars && L->fn->vars[v->id]==v ? v->id : -1; }
static void live_uses(Live *L, Expr *e, unsigned *set){
    if(!e) return;
    XInfo *xi=(XInfo*)e->ty;
    if(e->kind==EXPR_NAME){ if(xi && xi->kind==X_VAR){ int i=live_index(L,xi->var); if(i>=0) set[i>>5]|=1u<<(i&31); } return; }
    if(e->kind==EXPR_LAMBDA) return;                         /* its own function (captured variables never move) */
    if(e->kind==EXPR_COMPREHENSION){ for(int i=0;i<e->nclause;i++){ live_uses(L,e->clauses[i].iter,set); for(int k=0;k<e->clauses[i].ncond;k++) live_uses(L,e->clauses[i].conds[k],set); } }
    live_uses(L,e->a,set); live_uses(L,e->b,set); live_uses(L,e->c,set); live_uses(L,e->d,set);
    for(int i=0;i<e->count;i++) live_uses(L,e->items[i],set);
    for(int i=0;i<e->vcount;i++) live_uses(L,e->vals[i],set);
    if(e->kind==EXPR_CALL && xi){ live_uses(L,xi->key,set); live_uses(L,xi->rev,set); }
}
static void live_kill(Live *L, AVar *v, unsigned *set){ int i=live_index(L,v); if(i>=0) set[i>>5]&=~(1u<<(i&31)); }
static void live_target(Live *L, Expr *t, unsigned *kill, unsigned *use){   /* names assigned; the rest of a target is read */
    if(t->kind==EXPR_NAME){ XInfo *xi=(XInfo*)t->ty; if(xi && xi->var){ int i=live_index(L,xi->var); if(i>=0) kill[i>>5]|=1u<<(i&31); } return; }
    if(t->kind==EXPR_TUPLE||t->kind==EXPR_LIST){ for(int i=0;i<t->count;i++) live_target(L,t->items[i],kill,use); return; }
    live_uses(L,t->a,use); live_uses(L,t->b,use);
}
static void live_stmts(Live *L, F *f, Stmt **b, int n, unsigned *live);
static void live_stmt(Live *L, F *f, Stmt *s, unsigned *live){
    AotUnit *u=L->u;
    switch(s->kind){
        case STMT_ASSIGN:{
            *live_slot(L->tab,s)=bs_dup(L,live);
            AAssign *a=aot_assign(u,s); if(!a->value) return;
            unsigned *kill=bs_new(L), *use=bs_new(L);
            for(int k=0;k<a->ntarget;k++){ live_target(L,a->target[k],kill,use); if(a->aug) live_uses(L,a->target[k],use); }
            if(!a->aug) for(int i=0;i<L->W;i++) live[i]&=~kill[i];
            live_uses(L,a->value,use); bs_or(L,live,use); free(kill); free(use); return; }
        case STMT_EXPR:{
            *live_slot(L->tab,s)=bs_dup(L,live);
            APrint *pr=aot_print(u,s);
            if(pr){ for(int i=0;i<pr->n;i++) live_uses(L,pr->args[i],live); live_uses(L,pr->sep,live); live_uses(L,pr->end,live); return; }
            if(aot_is_annotation_only(u,s)) return;
            live_uses(L,s->expr,live); return; }
        case STMT_RETURN:
            memset(live,0,sizeof(unsigned)*(size_t)L->W);
            *live_slot(L->tab,s)=bs_dup(L,live);
            live_uses(L,s->expr,live); return;
        case STMT_YIELD: *live_slot(L->tab,s)=bs_dup(L,live); live_uses(L,s->expr,live); return;
        case STMT_RAISE: memset(live,0,sizeof(unsigned)*(size_t)L->W); live_uses(L,s->expr,live); return;
        case STMT_ASSERT: case STMT_DEL:
            if(s->kind==STMT_DEL){ ADel *d=aot_del(u,s); for(int i=0;i<d->n;i++) live_uses(L,d->t[i],live); }
            else { live_uses(L,s->expr,live); live_uses(L,s->expr2,live); }
            return;
        case STMT_IF:{
            unsigned *el=bs_dup(L,live);
            live_stmts(L,f,s->orelse,s->orelse_count,el); live_stmts(L,f,s->body,s->body_count,live);
            bs_or(L,live,el); free(el); live_uses(L,s->expr,live); return; }
        case STMT_WHILE: case STMT_FOR:{
            unsigned *after=bs_dup(L,live), *head=bs_new(L);
            AVar *vars[16]; int nv=0;
            if(s->kind==STMT_FOR) for(int k=0;k<s->param_count && k<16;k++) vars[nv++]=find_var(f,s->params[k],s->line);
            for(int round=0;round<64;round++){
                if(L->nloop<64){ L->brk[L->nloop]=after; L->cont[L->nloop]=head; } L->nloop++;
                unsigned *bd=bs_dup(L,head); live_stmts(L,f,s->body,s->body_count,bd);
                for(int k=0;k<nv;k++) live_kill(L,vars[k],bd);
                unsigned *el=bs_dup(L,after); live_stmts(L,f,s->orelse,s->orelse_count,el);
                L->nloop--;
                bs_or(L,bd,el); if(s->kind==STMT_WHILE) live_uses(L,s->expr,bd);
                int same=bs_eq(L,bd,head); free(el); free(head); head=bd;
                if(same) break;
            }
            memcpy(live,head,sizeof(unsigned)*(size_t)L->W);
            if(s->kind==STMT_FOR) live_uses(L,s->expr,live);
            free(head); free(after); return; }
        case STMT_BREAK: if(L->nloop>0 && L->nloop<=64) memcpy(live,L->brk[L->nloop-1],sizeof(unsigned)*(size_t)L->W); return;
        case STMT_CONTINUE: if(L->nloop>0 && L->nloop<=64) memcpy(live,L->cont[L->nloop-1],sizeof(unsigned)*(size_t)L->W); return;
        case STMT_TRY:{                     /* no moves inside; before it: what the body, the handlers and finally read */
            unsigned *fin=bs_dup(L,live), *all=bs_new(L);
            Stmt *finb=NULL, *elb=NULL;
            for(int i=0;i<s->orelse_count;i++){ Stmt *b=s->orelse[i]; if(b->block_tag==3) finb=b; else if(b->block_tag==2) elb=b; }
            if(finb) live_stmts(L,f,finb->body,finb->body_count,fin);
            bs_or(L,all,fin);
            for(int i=0;i<s->orelse_count;i++){ Stmt *b=s->orelse[i]; if(b->block_tag!=1) continue;
                unsigned *h=bs_dup(L,fin); live_stmts(L,f,b->body,b->body_count,h); bs_or(L,all,h); free(h); }
            unsigned *bd=bs_dup(L,fin);
            if(elb) live_stmts(L,f,elb->body,elb->body_count,bd);
            bs_or(L,bd,all); live_stmts(L,f,s->body,s->body_count,bd); bs_or(L,bd,all);
            memcpy(live,bd,sizeof(unsigned)*(size_t)L->W); free(bd); free(fin); free(all); return; }
        case STMT_WITH:{ AWith *w=aot_with(u,s);
            live_stmts(L,f,s->body,s->body_count,live);
            for(int i=0;i<w->n;i++){ if(w->as[i]) live_kill(L,find_var(f,w->as[i],s->line),live); live_uses(L,w->e[i],live); }
            return; }
        case STMT_FUNCTION_DEF:
            if(is_fn_body(f->fn)) live_kill(L,find_var(f,s->name,s->line),live);
            if(s->expr2) live_uses(L,s->expr2,live);
            return;
        default: return;
    }
}
static void live_stmts(Live *L, F *f, Stmt **b, int n, unsigned *live){ for(int i=n-1;i>=0;i--) live_stmt(L,f,b[i],live); }
static void plan_moves(F *f){
    AFunc *fn=f->fn;
    if(!fn->def || !fn->nvars) return;
    Live L; memset(&L,0,sizeof L); L.fn=fn; L.u=fn->mod->unit; L.W=(fn->nvars+31)/32;
    f->live=MPY_NEW0(LiveTab); L.tab=f->live;
    unsigned *live=bs_new(&L);
    live_stmts(&L,f,fn->body,fn->nbody,live);
    free(live);
}
/* occurrences of the movable variables in a statement's expressions (2: several, or evaluated repeatedly) */
static void occ_count(F *f, Expr *e, int repeat){
    if(!e) return;
    XInfo *xi=(XInfo*)e->ty;
    if(e->kind==EXPR_NAME){ if(xi && xi->kind==X_VAR){ AVar *v=xi->var; if(v->owner==f->fn && !v->src && !v->global && v->id>=0 && v->id<f->fn->nvars && f->fn->vars[v->id]==v){
            int c=f->occ[v->id]+(repeat?2:1); f->occ[v->id]=(unsigned char)(c>2?2:c); } } return; }
    if(e->kind==EXPR_LAMBDA) return;
    if(e->kind==EXPR_COMPREHENSION){
        for(int i=0;i<e->nclause;i++){ occ_count(f,e->clauses[i].iter,repeat||(i>0)||e->comp_kind=='G'); for(int k=0;k<e->clauses[i].ncond;k++) occ_count(f,e->clauses[i].conds[k],1); }
        occ_count(f,e->a,1); occ_count(f,e->b,1); return; }
    occ_count(f,e->a,repeat); occ_count(f,e->b,repeat); occ_count(f,e->c,repeat); occ_count(f,e->d,repeat);
    for(int i=0;i<e->count;i++) occ_count(f,e->items[i],repeat);
    for(int i=0;i<e->vcount;i++) occ_count(f,e->vals[i],repeat);
    if(e->kind==EXPR_CALL && xi){ occ_count(f,xi->key,1); occ_count(f,xi->rev,repeat); }
}
static void occ_target(F *f, Expr *t){
    if(t->kind==EXPR_NAME) return;
    if(t->kind==EXPR_TUPLE||t->kind==EXPR_LIST){ for(int i=0;i<t->count;i++) occ_target(f,t->items[i]); return; }
    occ_count(f,t->a,0); occ_count(f,t->b,0);
}
/* entering simple statement s: may its variables move? */
static void moves_begin(F *f, Stmt *s){
    f->live_after=NULL;
    if(!f->live || f->ntries) return;
    unsigned *la=live_get(f->live,s); if(!la) return;
    AotUnit *u=f->fn->mod->unit;
    if(!f->occ) f->occ=(unsigned char*)xmalloc((size_t)f->fn->nvars+1);
    memset(f->occ,0,(size_t)f->fn->nvars+1);
    switch(s->kind){
        case STMT_ASSIGN:{ AAssign *a=aot_assign(u,s); if(!a->value) return;
            for(int k=0;k<a->ntarget;k++){ occ_target(f,a->target[k]); if(a->aug) occ_count(f,a->target[k],0); }
            occ_count(f,a->value,0); break; }
        case STMT_EXPR:{ APrint *pr=aot_print(u,s);
            if(pr){ for(int i=0;i<pr->n;i++) occ_count(f,pr->args[i],0); occ_count(f,pr->sep,0); occ_count(f,pr->end,0); }
            else if(!aot_is_annotation_only(u,s)) occ_count(f,s->expr,0);
            break; }
        case STMT_RETURN: case STMT_YIELD: occ_count(f,s->expr,0); break;
        default: return;
    }
    f->live_after=la;
}
/* the owned value of x: its reference itself when this is x's last use */
static int try_move(F *f, Expr *e){
    if(!f->live_after || e->kind!=EXPR_NAME) return 0;
    XInfo *xi=(XInfo*)e->ty; if(!xi || xi->kind!=X_VAR) return 0;
    AVar *v=xi->var; AFunc *fn=f->fn;
    if(v->owner!=fn || v->src || v->global || v->cell || v->captured || v->borrowed || (v->id<fn->nparams && !v->consumed) || v->id>=fn->nvars || fn->vars[v->id]!=v) return 0;
    if(!is_ptr(v->ty) || f->occ[v->id]!=1 || (f->live_after[v->id>>5]>>(v->id&31))&1) return 0;
    E(f,"mov eax,[ebp%+d]",v->offset); E(f,"and dword [ebp%+d],0",v->offset);   /* moved */
    return 1;
}
/* ---- parameters a function keeps ----
   A function whose top-level statement stores a parameter away for good
   (self.x = x, items[k] = x, xs.append(x), return x) as the parameter's last
   use takes it owned: callers hand temporaries over (no incref there, no
   decref after the call) and increfs move from the callee to the caller.
   Only for functions every call of which is direct (not values, not virtual,
   not operators, coroutines or generators). */
static int has_return(Stmt **b, int n){
    for(int i=0;i<n;i++){ Stmt *s=b[i]; if(s->kind==STMT_RETURN) return 1;
        if(s->kind!=STMT_FUNCTION_DEF && (has_return(s->body,s->body_count)||has_return(s->orelse,s->orelse_count))) return 1; }
    return 0;
}
static int consumes_param(Expr *v, AVar *p){ XInfo *xi=v?(XInfo*)v->ty:NULL; return v && v->kind==EXPR_NAME && xi && xi->kind==X_VAR && xi->var==p; }
static void plan_consumed(AFunc *fn){
    if(!fn->def || fn->value_used || fn->ndeco || fn->is_async || fn->is_gen || fn->is_property || fn->overridden || fn->outer) return;
    if(fn->cls){
        if(!strncmp(fn->name,"__",2) && strcmp(fn->name,"__init__")) return;   /* operators, __str__, __iter__ ...: called from the runtime */
        if(fn->cls->base && aot_find_method(fn->cls->base,fn->name)) return;    /* overrides: called through the vtable */
    }
    int any=0; for(int i=0;i<fn->nparams;i++){ AVar *v=fn->params[i]; if(is_ptr(v->ty) && !v->cell && !v->captured && !v->nbind && i!=fn->star && i!=fn->dstar) any=1; }
    if(!any) return;
    F tmp; memset(&tmp,0,sizeof tmp); tmp.fn=fn;
    plan_moves(&tmp);
    AotUnit *u=fn->mod->unit;
    for(int k=0;k<fn->nbody;k++){ Stmt *s=fn->body[k];
        unsigned *la=live_get(tmp.live,s); if(!la) continue;
        for(int i=0;i<fn->nparams;i++){ AVar *p=fn->params[i];
            if(!is_ptr(p->ty) || p->cell || p->captured || p->nbind || i==fn->star || i==fn->dstar || (la[i>>5]>>(i&31))&1) continue;
            if(i==0 && fn->cls && !fn->is_static) continue;                       /* self is always lent */
            int keep=0;
            if(s->kind==STMT_ASSIGN){ AAssign *a=aot_assign(u,s);
                if(a->value && !a->aug && a->ntarget==1 && consumes_param(a->value,p) && (a->target[0]->kind==EXPR_ATTRIBUTE||a->target[0]->kind==EXPR_INDEX)) keep=1; }
            else if(s->kind==STMT_EXPR && !aot_print(u,s) && !aot_is_annotation_only(u,s)){ Expr *e=s->expr; XInfo *xi=(XInfo*)e->ty;
                if(e->kind==EXPR_CALL && xi && xi->kind==X_TMETHOD && !strcmp(xi->name,"append") && e->count==1 && consumes_param(e->items[0],p)) keep=1; }
            else if(s->kind==STMT_RETURN && consumes_param(s->expr,p)) keep=1;
            if(!keep) continue;
            tmp.occ=NULL; tmp.live_after=NULL; moves_begin(&tmp,s);              /* it must be its only occurrence there */
            if(tmp.live_after && tmp.occ[i]==1 && !p->consumed)
                p->consumed=has_return(fn->body,k)?1:2;                              /* 2: always handed on before any exit */
            free(tmp.occ);
        }
    }
}
/* yield: the value (owned) into the generator, then back to its consumer */
static void gen_yield_value(F *f, Ty *yt){
    E(f,"mov ecx,[rt_cur_gen]");
    store_mem(f,yt,"ecx",24);
    CALLRT(f,"rt_gen_yield");
}
/* after a yield statement: a value sent meanwhile is dropped */
static void drop_sent(F *f){
    Ty *st=gpart(f->fn->ret,0); int l=new_label();
    E(f,"mov ecx,[rt_cur_gen]"); E(f,"cmp dword [ecx+96],0"); E(f,"je L%d",l); E(f,"mov dword [ecx+96],0");
    if(is_ptr(st)){ rt("rt_decref"); E(f,"xor eax,eax"); E(f,"xchg eax,[ecx+88]"); E(f,"call rt_decref"); }
    LBL(f,l);
}
/* eax/st0 = what the yield expression gives: the value sent (owned), None when next() resumed it */
static int take_sent(F *f, Ty *st){
    int l=new_label(), end=new_label();
    E(f,"mov ecx,[rt_cur_gen]"); E(f,"cmp dword [ecx+96],0"); E(f,"je L%d",l);
    E(f,"mov dword [ecx+96],0"); load_mem(f,st,"ecx",88);
    if(is_ptr(st)) E(f,"mov dword [ecx+88],0");
    E(f,"jmp L%d",end);
    LBL(f,l); gen_none(f,st);
    LBL(f,end);
    return is_ptr(st);
}
/* yield from a generator: its values; values sent go to it -> (want) what it returns */
static int gen_yield_from(F *f, Expr *src, int want){
    Ty *gt=ty_find(TY(src)), *iyt=ty_find(gt->elem), *yt=ty_find(f->fn->yield_ty), *gr=gpart(gt,1);
    int slot=frame_slot(f,4); add_ref_slot(f,slot);
    gen_owned(f,src); panic_if_null(f); E(f,"mov [ebp%+d],eax",slot);
    int lnext=new_label(), ldone=new_label();
    LBL(f,lnext); E(f,"mov eax,[ebp%+d]",slot); CALLRT(f,"rt_gen_next"); E(f,"test eax,eax"); E(f,"jz L%d",ldone);
    E(f,"mov ecx,[ebp%+d]",slot); load_mem(f,iyt,"ecx",24); if(is_ptr(iyt)) incref(f); conv(f,iyt,yt);
    gen_yield_value(f,yt);
    E(f,"mov ecx,[rt_cur_gen]"); E(f,"cmp dword [ecx+96],0"); E(f,"je L%d",lnext);     /* a value sent: the inner generator's */
    E(f,"mov dword [ecx+96],0"); E(f,"mov edx,[ebp%+d]",slot);
    E(f,"mov eax,[ecx+88]"); E(f,"mov [edx+88],eax"); E(f,"mov eax,[ecx+92]"); E(f,"mov [edx+92],eax");
    E(f,"mov dword [ecx+88],0"); E(f,"mov dword [ecx+92],0"); E(f,"mov dword [edx+96],1");
    E(f,"jmp L%d",lnext);
    LBL(f,ldone);
    int o=0;
    if(want && gr->k!=TY_VOID){ int ln=new_label(), le=new_label();
        E(f,"mov ecx,[ebp%+d]",slot); E(f,"cmp dword [ecx+108],0"); E(f,"je L%d",ln);
        load_mem(f,gr,"ecx",100); if(is_ptr(gr)) incref(f); E(f,"jmp L%d",le);
        LBL(f,ln); gen_none(f,gr); LBL(f,le); o=is_ptr(gr);
        push_value(f,gr); }
    rt("rt_decref"); E(f,"xor eax,eax"); E(f,"xchg eax,[ebp%+d]",slot); E(f,"call rt_decref");
    if(want && gr->k!=TY_VOID) pop_value(f,gr);
    return o;
}
static void iter_begin(F *f, Expr *it, Iter *I);
/* the function of a generator expression: its loops, yielding each element */
static void gen_genexp_body(F *f, AFunc *fn){
    Expr *e=fn->genexp; XInfo *xi=xinfo(e);
    Iter its[8]; int n=e->nclause;
    if(n>8) cg_fail(e->line,"too many for clauses");
    for(int i=0;i<n;i++){
        CompClause *cl=&e->clauses[i];
        iter_begin(f,(i==0&&xi->key)?xi->key:cl->iter,&its[i]);
        iter_store_vars(f,&its[i],&xi->cvars[2*i],cl->nvars); comp_unpack(f,e,i);
        for(int k=0;k<cl->ncond;k++) gen_jump(f,cl->conds[k],its[i].cont,0);
    }
    int mark=scope_open(f);
    gen_as(f,e->a,fn->yield_ty,1); gen_yield_value(f,fn->yield_ty);
    scope_close(f,mark);
    for(int i=n-1;i>=0;i--){ iter_end(f,&its[i]); LBL(f,its[i].exit); iter_release(f,&its[i]); }
}
static void emit_function(AFunc *fn){
    F *f=MPY_NEW0(F); f->fn=fn; f->lret=new_label();
    f->ret=fn->is_gen ? TY_VOID_T : fn->ret;
    cg_path=fn->mod->unit->path?fn->mod->unit->path:fn->mod->name;
    int off=8, argpos[16];
    for(int i=0;i<fn->nparams;i++){ fn->params[i]->offset=argpos[i]=off; off+=esize(fn->params[i]->ty); }
    int argbytes=off-8;
    if(fn->ncaps || (fn->outer && dyn_defaults(fn))){ layout_caps(fn); f->env=frame_slot(f,4); }
    if(is_fn_body(fn)){
        if(!getenv("MPY_NO_BORROW")) plan_borrowed(f);
        if(!getenv("MPY_NO_MOVE")) plan_moves(f);
        for(int i=0;i<fn->nvars;i++){ AVar *v=fn->vars[i];
            if(i<fn->nparams && !v->cell) continue;
            if(v->cell){ v->offset=frame_slot(f,4); add_ref_slot(f,v->offset); }
            else { v->offset=frame_slot(f,esize(v->ty)); if(is_ptr(v->ty) && !v->borrowed) add_ref_slot(f,v->offset); }
        }
    }
    if(fn->lam){                                          /* lambda: return its expression */
        Expr *b=fn->lam->a; Ty *r=ty_find(fn->ret); int mark=scope_open(f);
        if(r->k==TY_VOID){ if(b->kind!=EXPR_NONE){ int o=gen(f,b); drop_value(f,TY(b),o); } } else gen_as(f,b,r,1);
        if(f->tmp_used>mark){ if(!is_flt(r) && r->k!=TY_VOID) push_value(f,r); scope_close(f,mark); if(!is_flt(r) && r->k!=TY_VOID) pop_value(f,r); }
    } else if(fn->genexp) gen_genexp_body(f,fn);
    else {
        gen_stmts(f,fn->body,fn->nbody);
        Ty *ret=ty_find(f->ret);
        if(ret->k!=TY_VOID){ int o=gen_zero(f,ret); if(is_ptr(ret)&&!o) incref(f); }   /* falling off the end returns None */
    }
    Ty *ret=ty_find(f->ret);
    for(int i=0;i<fn->nparams;i++){ Ty *pt=fn->params[i]->ty;
        if(!is_ptr(pt)) continue;
        if(fn->is_gen || fn->params[i]->consumed==1) add_ref_slot(f,argpos[i]);   /* a generator owns its arguments; so does a function keeping one */
        else if(f->param_stored[i] && !fn->params[i]->cell) add_ref_slot(f,argpos[i]); }

    Buf *o=&gg->text;
    const char *kind=fn->lam?"<lambda>":fn->genexp?"<genexpr>":fn->def?fn->name:"<module>";
    buf_printf(o,"\n; %s%s%s()%s\n%s:\n",fn->cls?fn->cls->name:"",fn->cls?".":"",kind,fn->is_gen?": the generator's function":"",fnl(fn,fn->is_gen?LB_BODY:LB_CODE));
    buf_printf(o,"        push ebp\n        mov ebp,esp\n        push ebx\n        push esi\n        push edi\n");
    int words=(f->frame+3)/4;
    if(words<=8) for(int i=0;i<words;i++) buf_printf(o,"        push 0\n");
    else buf_printf(o,"        sub esp,%d\n        mov edi,esp\n        mov ecx,%d\n        xor eax,eax\n        rep stosd\n",words*4,words);
    if(f->env) buf_printf(o,"        mov [ebp%+d],edx\n",f->env);
    for(int i=0;i<fn->nparams;i++) if(fn->params[i]->bound){ char b[320]; var_addr(b,sizeof b,fn->params[i]->bound); buf_printf(o,"        mov dword %s,1\n",b); }
    if(!fn->is_gen) for(int i=0;i<fn->nparams;i++) if(f->param_stored[i] && is_ptr(fn->params[i]->ty) && !fn->params[i]->cell && !fn->params[i]->consumed){ rt("rt_incref"); buf_printf(o,"        mov eax,[ebp%+d]\n        call rt_incref\n",argpos[i]); }
    for(int i=0;i<fn->nvars;i++){ AVar *v=fn->vars[i]; if(!v->cell) continue;   /* cells of the variables closures share */
        rt("rt_cell_new"); rt("rt_incref");
        buf_printf(o,"        mov eax,%d\n        call rt_cell_new\n",is_ptr(v->ty)?1:0);
        if(i<fn->nparams){
            if(is_flt(v->ty)) buf_printf(o,"        fld qword [ebp%+d]\n        fstp qword [eax+8]\n",argpos[i]);
            else if(is_lng(v->ty)) buf_printf(o,"        mov ecx,[ebp%+d]\n        mov [eax+8],ecx\n        mov ecx,[ebp%+d]\n        mov [eax+12],ecx\n",argpos[i],argpos[i]+4);
            else { buf_printf(o,"        mov ecx,[ebp%+d]\n        mov [eax+8],ecx\n",argpos[i]);
                if(is_ptr(v->ty)) buf_printf(o,"        push eax\n        mov eax,ecx\n        call rt_incref\n        pop eax\n"); }
        }
        buf_printf(o,"        mov [ebp%+d],eax\n",v->offset);
    }
    buf_cat(o,&f->code);
    buf_printf(o,"L%d:\n",f->lret);
    if(f->nrefs){
        rt("rt_decref");
        int keep=!is_flt(ret) && ret->k!=TY_VOID;
        if(keep){ if(is_lng(ret)) buf_printf(o,"        push edx\n"); buf_printf(o,"        push eax\n"); }
        for(int i=0;i<f->nrefs;i++) buf_printf(o,"        mov eax,[ebp%+d]\n        call rt_decref\n",f->refs[i]);
        if(keep){ buf_printf(o,"        pop eax\n"); if(is_lng(ret)) buf_printf(o,"        pop edx\n"); }
    }
    buf_printf(o,"        lea esp,[ebp-12]\n        pop edi\n        pop esi\n        pop ebx\n        pop ebp\n        ret\n");
    if(fn->is_gen){                                       /* F<id> makes the generator: arguments and closure kept in it */
        Ty *yt=ty_find(fn->yield_ty);
        rt("rt_gen_new"); rt("rt_gen_next"); rt("rt_incref");
        buf_printf(&gg->data,"align 4\n%s dd %s,%d,%d",fnl(fn,LB_GEN),fnl(fn,LB_BODY),(is_ptr(yt)?1:0)|(esize(yt)==8?2:0)|(is_ptr(gpart(fn->ret,0))?4:0)|(is_ptr(gpart(fn->ret,1))?8:0),f->nrefs);
        for(int i=0;i<f->nrefs;i++) buf_printf(&gg->data,",%d",f->refs[i]);
        buf_printf(&gg->data,"   ; generator %s\n",kind);
        buf_printf(o,"%s:                            ; %s(): a new generator\n",fnl(fn,LB_CODE),kind);
        buf_printf(o,"        push ebx\n        mov ebx,edx\n        mov eax,%d\n        mov edx,%s\n        call rt_gen_new\n        push eax\n        mov ecx,[eax+36]\n",argbytes,fnl(fn,LB_GEN));
        for(int w=0;w<argbytes;w+=4) buf_printf(o,"        mov edx,[esp+%d]\n        mov [ecx+%d],edx\n",12+w,w);
        for(int i=0;i<fn->nparams;i++) if(is_ptr(fn->params[i]->ty)) buf_printf(o,"        mov eax,[esp+%d]\n        call rt_incref\n",12+argpos[i]-8);
        if(fn->ncaps) buf_printf(o,"        mov eax,ebx\n        call rt_incref\n        mov ecx,[esp]\n        mov [ecx+44],ebx\n");
        buf_printf(o,"        pop eax\n        pop ebx\n        ret\n");
    }
    free(f->code.s); free(f->tmp); free(f->refs); free(f);
}
static int adapter_index(Ty *to, Ty *from){
    for(int i=0;i<gg->nadapters;i++) if(ty_same_exact(gg->adapters[i].to,to) && ty_same_exact(gg->adapters[i].from,from)) return i;
    if(gg->nadapters==gg->cadapters){ gg->cadapters=gg->cadapters?gg->cadapters*2:4; gg->adapters=xrealloc(gg->adapters,sizeof(*gg->adapters)*(size_t)gg->cadapters); }
    gg->adapters[gg->nadapters].to=to; gg->adapters[gg->nadapters].from=from;
    rt("rt_alloc"); rt("rt_incref"); rt("rt_decref"); rt("rt_free");
    return gg->nadapters++;
}
/* AD<k>: called as `to`, calls the function at [env+16] (a `from`, which takes *args) */
static void emit_adapter(int k){
    Ty *to=gg->adapters[k].to, *from=gg->adapters[k].from;
    F ff; memset(&ff,0,sizeof ff); F *f=&ff;
    int star=1, dstar=(from->tup>>1)&1, nreg=from->nelems-star-dstar, toff[16], foff[17], tot=0, a=8;
    for(int i=0;i<to->nelems;i++){ toff[i]=a; a+=esize(to->elems[i]); }
    for(int i=0;i<from->nelems;i++){ foff[i]=tot; tot+=i<nreg?esize(from->elems[i]):4; }
    Ty *sel=from->elems[nreg];
    buf_printf(&f->code,"\nAD%d:                          ; %s as %s\n",k,ty_name(from),ty_name(to));
    E(f,"push ebp"); E(f,"mov ebp,esp"); E(f,"push ebx"); E(f,"push esi"); E(f,"push edi"); E(f,"mov ebx,edx");
    new_list(f,sel); E(f,"mov esi,eax");
    for(int i=nreg;i<to->nelems;i++){ Ty *pt=to->elems[i];
        E(f,"push esi"); load_mem(f,pt,"ebp",toff[i]); if(is_ptr(pt)) incref(f); conv_num(f,pt,sel); list_append_top(f,sel); E(f,"add esp,4"); }
    E(f,"xor edi,edi");
    if(dstar){ Ty *dt=ty_dict(TY_STR_T,from->elems[nreg+1]); new_dict(f,dt); E(f,"mov edi,eax"); }
    if(tot) E(f,"sub esp,%d",tot);
    for(int i=0;i<nreg;i++){ Ty *pt=from->elems[i]; load_mem(f,to->elems[i],"ebp",toff[i]); conv_num(f,to->elems[i],pt); put_arg(f,pt,foff[i]); }
    E(f,"mov [esp+%d],esi",foff[nreg]);
    if(dstar) E(f,"mov [esp+%d],edi",foff[nreg+1]);
    E(f,"mov edx,[ebx+16]"); E(f,"call dword [edx+8]");
    if(tot) E(f,"add esp,%d",tot);
    E(f,"push edx"); E(f,"push eax"); E(f,"mov eax,esi"); CALLRT(f,"rt_decref"); E(f,"mov eax,edi"); CALLRT(f,"rt_decref"); E(f,"pop eax"); E(f,"pop edx");
    E(f,"pop edi"); E(f,"pop esi"); E(f,"pop ebx"); E(f,"pop ebp"); E(f,"ret");
    buf_cat(&gg->text,&f->code); free(f->code.s);
}
/* BM<id>: the code of method m's bound values: the object (+16) first, then the arguments */
static void emit_bound_method(AFunc *m){
    Buf *o=&gg->text; int argbytes=0;
    for(int i=1;i<m->nparams;i++) argbytes+=esize(m->params[i]->ty);
    buf_printf(o,"\n%s:                          ; %s.%s bound to an object\n        push ebp\n        mov ebp,esp\n        sub esp,%d\n        mov eax,[edx+16]\n        mov [esp],eax\n",fnl(m,LB_BOUND),m->cls?m->cls->name:"",m->name,argbytes+4);
    for(int w=0;w<argbytes;w+=4) buf_printf(o,"        mov ecx,[ebp+%d]\n        mov [esp+%d],ecx\n",8+w,4+w);
    if(m->overridden) buf_printf(o,"        mov eax,[eax+8]\n        call dword [eax+%d]\n",8+4*m->vslot);
    else buf_printf(o,"        call %s\n",fnl(m,LB_CODE));
    buf_printf(o,"        leave\n        ret\n");
}
/* <fn>.free: destroy routine of fn's closures */
static void emit_closure_destroy(AFunc *fn){
    Buf *o=&gg->text; int any=0;
    rt("rt_free"); rt("rt_decref");
    buf_printf(o,"\n%s:                          ; destroy a closure of %s\n",fnl(fn,LB_FREE),fn->lam?"<lambda>":fn->genexp?"<genexpr>":fn->name);
    for(int i=0;i<fn->ncaps;i++){ AVar *v=fn->caps[i]; if(!root_var(v)->cell && !is_ptr(v->ty)) continue;
        if(!any){ buf_printf(o,"        push ebx\n        mov ebx,eax\n"); any=1; }
        buf_printf(o,"        mov eax,[ebx+%d]\n        call rt_decref\n",v->capoff); }
    layout_defaults(fn);
    for(int i=0;i<fn->nparams;i++){ if(!fn->defaults[i] || i==fn->star || i==fn->dstar || !is_ptr(fn->params[i]->ty)) continue;
        if(!any){ buf_printf(o,"        push ebx\n        mov ebx,eax\n"); any=1; }
        buf_printf(o,"        mov eax,[ebx+%d]\n        call rt_decref\n",16+fn->defoff[i]); }
    if(any) buf_printf(o,"        mov eax,ebx\n        pop ebx\n");
    buf_printf(o,"        jmp rt_free\n");
}

static void emit_class(AClass *c){
    const char *mn= c->builtin || !c->mod || !c->mod->name ? "builtins" : c->mod->name;     /* (before it: its module's name, and "module." as */
    char pre[300]; snprintf(pre,sizeof pre,"%s.",mn);                                       /* an uncaught exception shows it: none for __main__'s and built-in ones) */
    if(!strcmp(mn,"builtins") || !strcmp(mn,"__main__")) pre[0]=0;
    buf_printf(&gg->data,"align 4\n        dd S%d,S%d\n%s dd S%d,",str_lit(mn),str_lit(pre),class_label(c,0),str_lit(c->name));
    if(c->base) buf_printf(&gg->data,"%s",class_label(c->base,0)); else buf_printf(&gg->data,"0");
    for(int i=0;i<c->nvt;i++) buf_printf(&gg->data,",%s",c->vt[i]->unused||!c->vt[i]->used?"0":fnl(c->vt[i],LB_CODE));   /* (a mixin's own methods: only its copies run; uncalled ones: none) */
    buf_printf(&gg->data,"   ; class %s\n",c->name);
    Buf *o=&gg->text; int any=0;
    buf_printf(o,"\n%s:                          ; destroy %s\n",class_label(c,1),c->name);
    for(AClass *k=c;k;k=k->base) for(int i=0;i<k->nfields;i++) if(is_ptr(k->fields[i]->ty) && !k->fields[i]->over){
        if(!any){ buf_printf(o,"        push ebx\n        mov ebx,eax\n"); any=1; }
        buf_printf(o,"        mov eax,[ebx+%d]\n        call rt_decref\n",k->fields[i]->offset);
    }
    if(any) buf_printf(o,"        mov eax,ebx\n        pop ebx\n");
    buf_printf(o,"        jmp rt_free\n");
}

/* ---------------------------------------------------------------- the program */

static void emit_lit_pools(void){
    Buf *d=&gg->data;
    for(int i=0;i<gg->ntdescs;i++){ Ty *t=gg->tdescs[i];
        buf_printf(d,"align 4\nTD%d dd %d",i,t->nelems);
        buf_printf(d,",%d",tuple_size(t));
        for(int k=0;k<t->nelems;k++) buf_printf(d,",%s,%d",kd_of(t->elems[k]),tuple_off(t,k));
        buf_printf(d,"   ; %s\n",ty_name(t)); }
    if(gg->nflts){ buf_printf(d,"align 8\n");            /* (as bits: fasm does not always round a decimal one right) */
        for(int i=0;i<gg->nflts;i++){ double v=strtod(gg->flts[i].s,NULL); unsigned long long b; memcpy(&b,&v,8);
            buf_printf(d,"FC%d dd 0x%08X,0x%08X ; %s\n",i,(unsigned)(b&0xFFFFFFFFu),(unsigned)(b>>32),gg->flts[i].s); } }
    for(int i=0;i<gg->nlits;i++){
        buf_printf(d,"align 4\nS%d dd 0x40000000,rt_static,%d\n        ",i,gg->lits[i].len);
        buf_bytes(d,gg->lits[i].s,gg->lits[i].len,1);
        buf_printf(d,"        dd %d\n",u8_cplen(gg->lits[i].s,gg->lits[i].len));
    }
    for(int i=0;i<gg->nblits;i++){                               /* bytes: one code point per byte */
        buf_printf(d,"align 4\nB%d dd 0x40000000,rt_static,%d\n        ",i,gg->blits[i].len);
        buf_bytes(d,gg->blits[i].s,gg->blits[i].len,1);
        buf_printf(d,"        dd %d\n",gg->blits[i].len);
    }
    for(int i=0;i<gg->nplits;i++){ buf_printf(d,"P%d ",i); Lit *l=&gg->plits[i]; char *t=(char*)xmalloc((size_t)l->len+1); memcpy(t,l->s,(size_t)l->len); t[l->len]='\n'; buf_bytes(d,t,l->len+1,0); free(t); }
    for(int i=0;i<gg->nzlits;i++){ buf_printf(d,"Z%d ",i); buf_bytes(d,gg->zlits[i].s,gg->zlits[i].len,1); }
    for(int i=0;i<gg->nrlits;i++){ buf_printf(d,"RL%d ",i); buf_bytes(d,gg->rlits[i].s,gg->rlits[i].len,0); }
}

/* Dynamic linking (ctypes): the C functions the program calls are imported
   from their libraries by ld-linux.so.2 - a PT_INTERP segment, a PT_DYNAMIC
   one, and symbol / relocation / hash / string tables for the slots CI<id>
   (R_386_32: the linker writes each function's address there at start). */
static int dyn_used(AProg *p){ for(int i=0;i<p->ncfuncs;i++) if(p->cfuncs[i]->used) return 1; return 0; }
static void emit_dynamic_header(Buf *o, AProg *p){
    buf_printf(o,"segment interpreter readable\n        db '/lib/ld-linux.so.2',0\n\nsegment dynamic readable\n");
    for(int i=0;i<p->nclibs;i++) if(p->clibs[i]->used) buf_printf(o,"        dd 1,DYN_L%d-DYN_strtab       ; DT_NEEDED %s\n",i,p->clibs[i]->soname);
    buf_printf(o,"        dd 5,DYN_strtab,10,DYN_strsz   ; DT_STRTAB, DT_STRSZ\n"
                 "        dd 6,DYN_symtab,11,16          ; DT_SYMTAB, DT_SYMENT\n"
                 "        dd 17,DYN_rel,18,DYN_relsz,19,8   ; DT_REL, DT_RELSZ, DT_RELENT\n"
                 "        dd 4,DYN_hash                  ; DT_HASH\n"
                 "        dd 0,0\n\n");
}
static void emit_dynamic_tables(Buf *o, AProg *p){
    int n=0;
    buf_printf(o,"\n; ---- imports of the dynamic linker\nalign 4\nDYN_symtab:\n        dd 0,0,0,0\n");
    for(int i=0;i<p->ncfuncs;i++) if(p->cfuncs[i]->used){ n++; buf_printf(o,"        dd DYN_S%d-DYN_strtab,0,0\n        db 0x12,0\n        dw 0\n",i); }   /* STB_GLOBAL, STT_FUNC */
    buf_printf(o,"DYN_rel:\n");
    int k=0;
    for(int i=0;i<p->ncfuncs;i++) if(p->cfuncs[i]->used){ k++; buf_printf(o,"        dd CI%d,(%d shl 8) or 1   ; R_386_32 %s\n",i,k,p->cfuncs[i]->sym); }
    buf_printf(o,"DYN_relsz = $-DYN_rel\nDYN_hash:\n        dd 1,%d,0\n",n+1);           /* one bucket, empty: nothing to export */
    for(int i=0;i<=n;i++) buf_printf(o,"        dd 0\n");
    buf_printf(o,"DYN_strtab db 0\n");
    for(int i=0;i<p->ncfuncs;i++) if(p->cfuncs[i]->used) buf_printf(o,"DYN_S%d db '%s',0\n",i,p->cfuncs[i]->sym);
    for(int i=0;i<p->nclibs;i++) if(p->clibs[i]->used) buf_printf(o,"DYN_L%d db '%s',0\n",i,p->clibs[i]->soname);
    buf_printf(o,"DYN_strsz = $-DYN_strtab\nalign 4\n");
    for(int i=0;i<p->ncfuncs;i++) if(p->cfuncs[i]->used){
        buf_printf(o,"CI%d dd 0   ; %s\n",i,p->cfuncs[i]->sym);
        if(!strcmp(p->cfuncs[i]->sym,"fflush")) buf_printf(o,"CI_fflush = CI%d\n",i);
    }
}
/* macos: no dynamic linker - the C translation looks the functions up when
   the program starts; ";@import CI<id> <library> <symbol>" names each slot. */
static void emit_import_slots(Buf *o, AProg *p){
    buf_printf(o,"\n; ---- C functions (ctypes)\nalign 4\n");
    for(int i=0;i<p->ncfuncs;i++) if(p->cfuncs[i]->used){
        buf_printf(o,";@import CI%d %s %s\nCI%d dd 0   ; %s\n",i,p->cfuncs[i]->lib->soname,p->cfuncs[i]->sym,i,p->cfuncs[i]->sym);
        if(!strcmp(p->cfuncs[i]->sym,"fflush")) buf_printf(o,"CI_fflush = CI%d\n",i);
    }
}
/* With the C library linked, its stdio buffers are flushed at exit (fflush(NULL)). */
static void import_fflush(AProg *p){
    ACLib *libc=NULL;
    for(int i=0;i<p->nclibs;i++) if(p->clibs[i]->used && !strcmp(p->clibs[i]->soname,"libc.so.6")) libc=p->clibs[i];
    if(!libc) return;
    for(int i=0;i<p->ncfuncs;i++) if(p->cfuncs[i]->lib==libc && !strcmp(p->cfuncs[i]->sym,"fflush")){ p->cfuncs[i]->used=1; return; }
    ACFunc *f=MPY_NEW0(ACFunc); f->lib=libc; f->sym=xstrdup2("fflush"); f->id=p->ncfuncs; f->used=1; f->nargtypes=-1;
    p->cfuncs=(ACFunc**)xrealloc(p->cfuncs,sizeof(ACFunc*)*(size_t)(p->ncfuncs+1)); p->cfuncs[p->ncfuncs++]=f;
}

/* x87 rounding to double precision: float arithmetic gives Python's results */
#define FPU_DOUBLE "        push 0x027F\n        fldcw [esp]\n        pop eax\n"

int aot_generate(AProg *p, const AotCodegenOptions *opt, char **out, size_t *outlen){
    G g; memset(&g,0,sizeof g); gg=&g;
    g.p=p; g.target=opt->target; g.stack=opt->stack_size?opt->stack_size:65536;
    g_labels.n=0; for(int k=0;k<LB_KINDS;k++) g_fnlabels[k]=NULL;          /* a fresh set of label names */
    g_vtlabels=g_dtlabels=g_glabels=g_mlabels=NULL;
    g.rt=aot_rt_new(opt->target);
    g.class_used=(int*)xmalloc(sizeof(int)*(size_t)(p->nclasses+1)); memset(g.class_used,0,sizeof(int)*(size_t)(p->nclasses+1));
    g.fv_used=(char*)xmalloc((size_t)p->nfuncs+1); memset(g.fv_used,0,(size_t)p->nfuncs+1);
    g.cd_used=(char*)xmalloc((size_t)p->nfuncs+1); memset(g.cd_used,0,(size_t)p->nfuncs+1);
    g.bm_used=(char*)xmalloc((size_t)p->nfuncs+1); memset(g.bm_used,0,(size_t)p->nfuncs+1);
    AFunc *main_body=p->mods[0]->body;
    use_fn(main_body);
    if(p->uncaught_fn) use_fn(p->uncaught_fn);
    rt("rt_exit");
    if(!getenv("MPY_NO_CONSUME")) for(int i=0;i<p->nfuncs;i++) if(!p->funcs[i]->unused) plan_consumed(p->funcs[i]);
    int nf=0, ns=0, na=0;
    for(;;){                                   /* functions, formatters, task entries and adapters pull in more of each other */
        int progress=0;
        for(int i=0;i<g.nq;i++) if(g.queue[i]){ AFunc *fn=g.queue[i]; g.queue[i]=NULL; emit_function(fn); progress=1; }
        for(;nf<g.nfmts;nf++){ emit_formatter(nf); progress=1; }
        for(;ns<g.nstubs;ns++){ emit_task_stub(ns); progress=1; }
        for(;na<g.nadapters;na++){ emit_adapter(na); progress=1; }
        if(!progress) break;
    }
    for(int i=0;i<p->nfuncs;i++) if(g.bm_used[i]) emit_bound_method(p->funcs[i]);
    if(g.nadapters || memchr(g.bm_used,1,(size_t)p->nfuncs)){ rt("rt_decref"); rt("rt_free"); }
    if(g.nadapters || memchr(g.bm_used,1,(size_t)p->nfuncs)) buf_printf(&g.text,"\nADFREE:                        ; destroy an adapter\n        push eax\n        mov eax,[eax+16]\n        call rt_decref\n        pop eax\n        jmp rt_free\n");
    for(int i=0;i<p->nfuncs;i++){
        if(g.cd_used[i]) emit_closure_destroy(p->funcs[i]);
        if(g.fv_used[i]){ AFunc *fi=p->funcs[i]; layout_defaults(fi);
            buf_printf(&g.data,"align 4\n%s dd 0x40000000,rt_static,%s,S%d   ; %s as a value\n",fnl(fi,LB_VALUE),fi->used?fnl(fi,LB_CODE):"0",str_lit(fn_display_name(fi)),fn_display_name(fi));   /* (0: only its defaults are used) */
            if(fi->defsize){ buf_printf(&g.data,"        dd 0"); for(int k=4;k<fi->defsize;k+=4) buf_printf(&g.data,",0"); buf_printf(&g.data,"   ; its defaults\n"); } }
    }
    if(aot_rt_used(g.rt,"rt_throw")){                   /* run-time errors raise built-in exceptions */
        static const char *rtexc[]={"IndexError","KeyError","ZeroDivisionError","AttributeError","ValueError","AssertionError","FileNotFoundError","StopIteration","TypeError","OverflowError","EOFError","LookupError","UnicodeDecodeError","UnicodeEncodeError","UnboundLocalError","NameError","GeneratorExit","RuntimeError",NULL};
        rt("rt_raise_builtin");
        if(aot_rt_used(g.rt,"rt_gen_new")) rt("rt_gen_close");          /* abandoned generators are closed (their finally blocks run) */
        { int mx=0; for(int i=0;i<p->nclasses;i++) if(p->classes[i]->builtin && aot_is_exception(p->classes[i]) && p->classes[i]->size>mx) mx=p->classes[i]->size;
          buf_printf(&g.data,"RT_EXC_SIZE = %d\n",mx); }                  /* (the largest built-in one: an OSError's fields) */
        for(int k=0;rtexc[k];k++) for(int i=0;i<p->nclasses;i++) if(p->classes[i]->builtin && !strcmp(p->classes[i]->name,rtexc[k])){
            use_class(p->classes[i]);
            buf_printf(&g.data,"VTX_%s = %s\nDTX_%s = %s\n",rtexc[k],class_label(p->classes[i],0),rtexc[k],class_label(p->classes[i],1));
        }
    }
    for(int i=0;i<p->nclasses;i++) if(g.class_used[i]) emit_class(p->classes[i]);
    for(int i=0;i<g.nbtypes;i++)                                 /* built-in types as values: name, base */
        buf_printf(&g.data,"align 4\n        dd S%d,S%d\nTYD_%s dd S%d,%s\n",str_lit("builtins"),str_lit(""),g.btypes[i],str_lit(g.btypes[i]),!strcmp(g.btypes[i],"bool")?"TYD_int":"0");
    if(aot_rt_used(g.rt,"rt_sb_type")){                          /* "<class '__main__.C'>": qualified names */
        buf_printf(&g.data,"align 4\nTYQ");
        for(int i=0;i<p->nclasses;i++) if(g.class_used[i] && !p->classes[i]->builtin){ AClass *c=p->classes[i]; char q[300];
            snprintf(q,sizeof q,"%s.%s",c->mod->name,c->qualname); buf_printf(&g.data," dd %s,S%d\n   ",class_label(c,0),str_lit(q)); }
        buf_printf(&g.data," dd 0\n"); }
    if(aot_rt_used(g.rt,"rt_type_qualname")){                    /* __qualname__ unlike __name__ */
        buf_printf(&g.data,"align 4\nTYQN");
        for(int i=0;i<p->nclasses;i++) if(g.class_used[i] && strcmp(p->classes[i]->qualname,p->classes[i]->name)){ AClass *c=p->classes[i];
            buf_printf(&g.data," dd %s,S%d\n   ",class_label(c,0),str_lit(c->qualname)); }
        buf_printf(&g.data," dd 0\n"); }
    emit_lit_pools();
    for(int i=0;i<p->nglobals;i++){ AVar *v=p->globals[i]; buf_printf(&g.bss,"%s rd %d   ; %s.%s\n",global_label(v),esize(v->ty)/4,v->mod->name,v->name); }
    for(int i=1;i<p->nmods;i++) if(p->mods[i]->used) buf_printf(&g.bss,"%s rb 1   ; %s imported\n",module_label(p->mods[i]),p->mods[i]->name);

    Buf o; memset(&o,0,sizeof o);
    int heap=aot_rt_used(g.rt,"rt_os_alloc")||aot_rt_used(g.rt,"rt_con_open");
    buf_printf(&o,"; generated by minipy --compile from %s\n",p->mods[0]->unit->path?p->mods[0]->unit->path:"?");
    char hook[200]="";                                     /* an uncaught exception: _mpy_exit.__mpy_uncaught (SystemExit's status) */
    if(p->uncaught_fn && aot_rt_used(g.rt,"rt_exc_uncaught")) snprintf(hook,sizeof hook,"        mov dword [rt_uncaught_hook],%s\n",fnl(p->uncaught_fn,LB_CODE));
    const char *report="";
    if(opt->count_allocs && aot_rt_used(g.rt,"rt_alloc")){ rt("rt_live_report"); report="        call rt_live_report\n"; buf_printf(&g.data,"RT_COUNT_ALLOCS = 1\n"); }
    int dyn=g.target!=AOT_TARGET_KOLIBRI && dyn_used(p);
    if(dyn) import_fflush(p);
    if(g.target!=AOT_TARGET_KOLIBRI){
        if(g.target==AOT_TARGET_MACOS) buf_printf(&o,"; target macos: minipy translates this listing to C (aot_x2c.c)\n");
        buf_printf(&o,"format ELF executable 3\nentry start\n\n");
        if(dyn && g.target==AOT_TARGET_LINUX) emit_dynamic_header(&o,p);
        buf_printf(&o,"segment readable executable\n\nstart:\n%s        fninit\n%s%s",aot_rt_used(g.rt,"rt_sys_args")?"        mov [rt_sp0],esp\n":"",FPU_DOUBLE,hook);
        buf_printf(&o,"        call %s\n%s%s        xor ebx,ebx\n        jmp rt_exit\n",fnl(main_body,LB_CODE),aot_rt_used(g.rt,"rt_thread_drain")?"        call rt_thread_drain\n":"",report);
    } else {
        buf_printf(&o,"format binary as ''\nuse32\n        org 0\n        db 'MENUET01'\n        dd 1,start,i_end,mem_end,stack_top,%s\n\nstart:\n        fninit\n%s",
                   aot_rt_used(g.rt,"rt_sys_args")?"rt_kparams,rt_kpath":"0,0",FPU_DOUBLE);
        if(heap) buf_printf(&o,"        mov eax,68\n        mov ebx,11\n        int 0x40\n");
        buf_printf(&o,"%s",hook);
        buf_printf(&o,"        call %s\n%s%s        xor ebx,ebx\n        jmp rt_exit\n",fnl(main_body,LB_CODE),aot_rt_used(g.rt,"rt_thread_drain")?"        call rt_thread_drain\n":"",report);
    }
    buf_cat(&o,&g.text);
    buf_printf(&o,"\n; ---- runtime\n");
    aot_rt_emit(g.rt,0,put_cb,&o);
    if(g.target!=AOT_TARGET_KOLIBRI) buf_printf(&o,"\nsegment readable writeable\n\n");
    else buf_printf(&o,"\n; ---- data\n");
    if(dyn) (g.target==AOT_TARGET_MACOS ? emit_import_slots : emit_dynamic_tables)(&o,p);
    buf_cat(&o,&g.data);
    aot_rt_emit(g.rt,1,put_cb,&o);
    if(g.target==AOT_TARGET_KOLIBRI) buf_printf(&o,"\nalign 4\ni_end:\n");
    buf_printf(&o,"\n; ---- uninitialized\nalign 8\n");
    buf_cat(&o,&g.bss);
    aot_rt_emit(g.rt,2,put_cb,&o);
    if(g.target==AOT_TARGET_KOLIBRI) buf_printf(&o,"align 16\n        rb %u\nstack_top:\nmem_end:\n",(g.stack+15)&~15u);
    *out=o.s; *outlen=o.len;
    gg=NULL;
    return 0;
}

int aot_compile(const AotCodegenOptions *opt, AotUnit **units, int nunits, char **out, size_t *outlen){
    AProg *p=aot_check(units,nunits,opt->target);
    if(!p) return 1;
    return aot_generate(p,opt,out,outlen);
}
