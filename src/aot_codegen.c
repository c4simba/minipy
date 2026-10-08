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
    Lit *plits; int nplits, cplits;         /* print literals P<i>: text + newline */
    Lit *flts; int nflts, cflts;            /* float constants FC<i>, as decimal text fasm converts */
    Lit *zlits; int nzlits, czlits;         /* NUL-terminated strings Z<i> */
    Lit *rlits; int nrlits, crlits;         /* raw bytes RL<i> (format string pieces) */
    AFunc **queue; int nq, cq;              /* functions to emit */
    Fmt *fmts; int nfmts, cfmts;            /* generated formatters FMT<i> */
    struct { AFunc *fn; int virt; } *stubs; int nstubs, cstubs;   /* task entries TS<i> */
    Ty **tdescs; int ntdescs, ctdescs;      /* tuple descriptors TD<i> */
    int *class_used;
    char *fv_used, *cd_used;                /* per function: static closure FV<id>, closure destroy routine CD<id> */
    struct { Ty *to, *from; } *adapters; int nadapters, cadapters;   /* AD<k> */
    char *bm_used;                          /* per method: BM<id>, the code of its bound values */
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
typedef struct { int fill, align, sign, alt, zero, width, comma, prec; char type; } Spec;
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
/* container element kinds of the runtime: 0 int/bool, 1 str, 2 float, 3 other reference */
static void rt(const char *name);
static int kind_of(Ty *t){
    t=ty_find(t);
    if(t->k==TY_FLOAT) return 2;
    if(t->k==TY_STR) return 1;
    if(t->k==TY_TUPLE){ rt("rt_tuple_eq"); return 4; }      /* compared by value */
    return ty_is_ptr(t)?3:0;
}
static int esize(Ty *t){ return ty_size(t); }
static const char *list_destroy(Ty *elem){ const char *n=is_ptr(elem)?"rt_list_destroy_ptr":"rt_list_destroy"; rt(n); return n; }
static const char *dict_destroy(Ty *elem){ const char *n=is_ptr(elem)?"rt_dict_destroy_ptr":"rt_dict_destroy"; rt(n); return n; }
static void cg_fail(int line, const char *msg);
/* a new empty dict of type t -> eax (owned) */
static void new_dict(F *f, Ty *t){
    Ty *k=ty_dkey(t); TyKind kk=ty_find(k)->k;
    if(kk==TY_FLOAT||kk==TY_LIST||kk==TY_SET||kk==TY_DICT) cg_fail(0,"dictionary keys must be int, str, tuples or objects");
    E(f,"mov eax,%s",dict_destroy(ty_find(t)->elem)); E(f,"mov edx,%d",kk==TY_BOOL?0:kind_of(k)); CALLRT(f,"rt_dict_new");
}

/* ---- literal pools ---- */
static int lit_index(Lit **v, int *n, int *cap, const char *s, int len){
    for(int i=0;i<*n;i++) if((*v)[i].len==len && memcmp((*v)[i].s,s,(size_t)len)==0) return i;
    if(*n==*cap){ *cap=*cap?*cap*2:32; *v=(Lit*)xrealloc(*v,sizeof(Lit)*(size_t)*cap); }
    (*v)[*n].s=xstrndup2(s,len); (*v)[*n].len=len; return (*n)++;
}
static int str_lit(const char *s){ rt("rt_static"); return lit_index(&gg->lits,&gg->nlits,&gg->clits,s,(int)strlen(s)); }
static int print_lit(const char *s){ return lit_index(&gg->plits,&gg->nplits,&gg->cplits,s,(int)strlen(s)); }
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
    if(fn->used) return;
    fn->used=1;
    if(gg->nq==gg->cq){ gg->cq=gg->cq?gg->cq*2:32; gg->queue=(AFunc**)xrealloc(gg->queue,sizeof(AFunc*)*(size_t)gg->cq); }
    gg->queue[gg->nq++]=fn;
}
static void use_class(AClass *c){
    for(;c;c=c->base){
        if(gg->class_used[c->id]) return;
        gg->class_used[c->id]=1;
        for(int i=0;i<c->nvt;i++) use_fn(c->vt[i]);
        for(int i=0;i<c->nmethods;i++) if(c->methods[i]->is_static) use_fn(c->methods[i]);
        rt("rt_free"); rt("rt_decref"); rt("rt_static");
        str_lit(c->name);
    }
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
static void layout_caps(AFunc *fn){
    if(fn->capsize || !fn->ncaps) return;
    int off=16;
    for(int i=0;i<fn->ncaps;i++){ AVar *v=fn->caps[i]; v->capoff=off; off+=root_var(v)->cell?4:ty_size(v->ty); }
    fn->capsize=off-16;
}
static const char *fn_display_name(AFunc *g){ return g->lam?"<lambda>":g->genexp?"<genexpr>":g->name; }
static void var_addr(char *buf, size_t n, AVar *v){
    if(v->global) snprintf(buf,n,"[G%d]",v->id); else snprintf(buf,n,"[ebp%+d]",v->offset);
}
static void load_mem(F *f, Ty *t, const char *reg, int off);
static void store_mem(F *f, Ty *t, const char *reg, int off);
static void load_var(F *f, AVar *v){
    if(v->src){                                         /* captured: in the closure (or its cell) */
        layout_caps(v->owner);
        E(f,"mov ecx,[ebp%+d]",f->env);
        if(root_var(v)->cell){ E(f,"mov ecx,[ecx+%d]",v->capoff); load_mem(f,v->ty,"ecx",8); }
        else load_mem(f,v->ty,"ecx",v->capoff);
        return;
    }
    if(v->cell){ E(f,"mov ecx,[ebp%+d]",v->offset); load_mem(f,v->ty,"ecx",8); return; }
    char a[32]; var_addr(a,sizeof a,v);
    if(is_flt(v->ty)) E(f,"fld qword %s",a); else E(f,"mov eax,%s",a);
}
/* Store eax/st0 (an owned reference for reference types) into the variable. */
static void store_var(F *f, AVar *v){
    if(v->src){ layout_caps(v->owner); E(f,"mov ecx,[ebp%+d]",f->env); E(f,"mov ecx,[ecx+%d]",v->capoff); store_mem(f,v->ty,"ecx",8); return; }   /* nonlocal: a cell */
    if(v->cell){ E(f,"mov ecx,[ebp%+d]",v->offset); store_mem(f,v->ty,"ecx",8); return; }
    char a[32]; var_addr(a,sizeof a,v);
    if(!v->global && v->id>=0 && v->id<f->fn->nparams && f->fn->params[v->id]==v) f->param_stored[v->id]=1;
    if(is_flt(v->ty)) E(f,"fstp qword %s",a);
    else if(is_ptr(v->ty)){ rt("rt_decref"); E(f,"xchg eax,%s",a); E(f,"call rt_decref"); }
    else E(f,"mov %s,eax",a);
}
/* Store eax/st0 (owned) at [reg+off], releasing the previous reference. */
static void store_mem(F *f, Ty *t, const char *reg, int off){
    if(is_flt(t)) E(f,"fstp qword [%s%+d]",reg,off);
    else if(is_ptr(t)){ rt("rt_decref"); E(f,"xchg eax,[%s%+d]",reg,off); E(f,"call rt_decref"); }
    else E(f,"mov [%s%+d],eax",reg,off);
}
/* Store into a fresh (zeroed or garbage) slot: nothing to release. */
static void store_new(F *f, Ty *t, const char *reg, int off){
    if(is_flt(t)) E(f,"fstp qword [%s%+d]",reg,off); else E(f,"mov [%s%+d],eax",reg,off);
}
static void load_mem(F *f, Ty *t, const char *reg, int off){
    if(is_flt(t)) E(f,"fld qword [%s%+d]",reg,off); else E(f,"mov eax,[%s%+d]",reg,off);
}
static void zero_value(F *f, Ty *t){ if(is_flt(t)) E(f,"fldz"); else E(f,"xor eax,eax"); }
static void push_value(F *f, Ty *t){ if(is_flt(t)){ E(f,"sub esp,8"); E(f,"fstp qword [esp]"); } else E(f,"push eax"); }
static void pop_value(F *f, Ty *t){ if(is_flt(t)){ E(f,"fld qword [esp]"); E(f,"add esp,8"); } else E(f,"pop eax"); }
static void drop_value(F *f, Ty *t, int owned){
    t=ty_find(t);
    if(t->k==TY_VOID||t->k==TY_VAR) return;
    if(is_flt(t)) E(f,"fstp st0");
    else if(is_ptr(t) && owned){ rt("rt_decref"); E(f,"call rt_decref"); }
}
static void panic_if_null(F *f){ int l=new_label(); rt("rt_panic_none"); E(f,"test eax,eax"); E(f,"jnz L%d",l); E(f,"call rt_panic_none"); LBL(f,l); }
static void incref(F *f){ CALLRT(f,"rt_incref"); }

/* ---------------------------------------------------------------- expressions */

static int  gen(F *f, Expr *e);
static void gen_bool(F *f, Expr *e);
static void gen_yield_value(F *f, Ty *yt);
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
    if(to->k==TY_FLOAT && from->k!=TY_FLOAT && from->k!=TY_VAR){ E(f,"push eax"); E(f,"fild dword [esp]"); E(f,"add esp,4"); }
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
static void conv(F *f, Ty *from, Ty *to){ conv_num(f,from,to); conv_func(f,from,to,1); }
static int gen_zero(F *f, Ty *t);
static void gen_as(F *f, Expr *e, Ty *want, int owned){
    if(e->kind==EXPR_NONE){ int o=gen_zero(f,want); if(o && !owned) hold(f); return; }
    if(owned) gen_owned(f,e); else gen_borrow(f,e);
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
    if(t->k==TY_INT||t->k==TY_BOOL){
        E(f,"mov ecx,eax"); E(f,"pop eax");
        switch(op){
            case T_PLUS: E(f,"add eax,ecx"); break;
            case T_MINUS: E(f,"sub eax,ecx"); break;
            case T_STAR: E(f,"imul eax,ecx"); break;
            case T_AMP: E(f,"and eax,ecx"); break;
            case T_PIPE: E(f,"or eax,ecx"); break;
            case T_CARET: E(f,"xor eax,ecx"); break;
            case T_SHL: E(f,"shl eax,cl"); break;
            case T_SHR: E(f,"sar eax,cl"); break;
            case T_FLOOR_DIV: E(f,"mov edx,ecx"); CALLRT(f,"rt_floordiv"); break;
            case T_PERCENT: E(f,"mov edx,ecx"); CALLRT(f,"rt_mod"); break;
            case T_POWER: E(f,"mov edx,ecx"); CALLRT(f,"rt_ipow"); break;
            default: cg_fail(line,"unsupported int operator");
        }
        return 0;
    }
    if(t->k==TY_STR){
        if(op==T_PLUS){ E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_str_concat"); return 1; }
        if(op==T_STAR){
            if(ta->k==TY_STR){ E(f,"mov edx,eax"); E(f,"pop eax"); }      /* str * n */
            else E(f,"pop edx");                                          /* n * str */
            CALLRT(f,"rt_str_mul"); return 1;
        }
    }
    if(t->k==TY_LIST){
        int k=kind_of(t->elem);
        if(op==T_PLUS){ E(f,"mov edx,eax"); E(f,"pop eax"); E(f,"mov ecx,%d",k); CALLRT(f,"rt_list_concat"); return 1; }
        if(op==T_STAR){ E(f,"mov edx,eax"); E(f,"pop eax"); E(f,"mov ecx,%d",k); CALLRT(f,"rt_list_mul"); return 1; }
    }
    if(t->k==TY_SET){
        int code=op==T_PIPE?0:op==T_AMP?1:2;
        E(f,"mov edx,eax"); E(f,"pop eax"); E(f,"mov ecx,%d",kind_of(t->elem)|(code<<8)); CALLRT(f,"rt_set_op"); return 1;
    }
    (void)tb;
    cg_fail(line,"unsupported operator");
}

/* self.m(arg): an operator implemented by a method -> its result (owned if a reference) */
static void call_on_top(F *f, AFunc *m){
    use_fn(m);
    E(f,"mov eax,[esp]"); panic_if_null(f);
    if(m->overridden){ E(f,"mov eax,[eax+8]"); E(f,"call dword [eax+%d]",8+4*m->vslot); }
    else E(f,"call F%d",m->id);
}
static int gen_op_call(F *f, AFunc *m, Expr *self, Expr *arg){
    int total=4+(arg?esize(m->params[1]->ty):0);
    E(f,"sub esp,%d",total);
    gen_borrow(f,self); E(f,"mov [esp],eax");
    if(arg){ Ty *pt=m->params[1]->ty; gen_as(f,arg,pt,0); if(is_flt(pt)) E(f,"fstp qword [esp+4]"); else E(f,"mov [esp+4],eax"); }
    call_on_top(f,m);
    E(f,"add esp,%d",total);
    return is_ptr(m->ret);
}
static int gen_format(F *f, Expr *e);
static int concat_worth(Expr *e);
static int gen_concat(F *f, Expr *e);
static int gen_binary(F *f, Expr *e){
    Ty *t=TY(e), *ta=TY(e->a), *tb=TY(e->b);
    if(xinfo(e)->kind==X_OPMETHOD) return gen_op_call(f,xinfo(e)->fn,e->a,e->b);
    if(e->op==T_POWER && t->k==TY_FLOAT && e->b->kind==EXPR_LITERAL && e->b->tok->is_float && e->b->tok->f==0.5){
        gen_as(f,e->a,TY_FLOAT_T,0); E(f,"fsqrt"); return 0; }                 /* x ** 0.5: exact */
    if(e->op==T_PERCENT && ta->k==TY_STR) return gen_format(f,e);
    if(e->op==T_PLUS && t->k==TY_STR && concat_worth(e)) return gen_concat(f,e);
    int num=numeric_ty(t);
    gen_as(f,e->a,num?t:ta,0); push_value(f,num?t:ta);
    gen_as(f,e->b,num?t:tb,0);
    return apply_binop(f,e->op,t,ta,tb,e->line);
}

/* ---- truth values ---- */
static int has_length(Ty *t){ t=ty_find(t); return t->k==TY_STR||t->k==TY_LIST||t->k==TY_DICT||t->k==TY_SET||t->k==TY_BUF; }
/* eax = truth of the value in eax/st0 (references stay put) */
static void truth(F *f, Ty *t){
    t=ty_find(t);
    if(t->k==TY_BOOL) return;
    if(t->k==TY_FLOAT){ E(f,"ftst"); E(f,"fnstsw ax"); E(f,"fstp st0"); E(f,"sahf"); E(f,"setne al"); E(f,"movzx eax,al"); return; }
    if(has_length(t)){ int l=new_label(); E(f,"xor ecx,ecx"); E(f,"test eax,eax"); E(f,"jz L%d",l); E(f,"cmp dword [eax+8],0"); E(f,"setne cl"); LBL(f,l); E(f,"mov eax,ecx"); return; }
    E(f,"test eax,eax"); E(f,"setne al"); E(f,"movzx eax,al");
}
/* jump to `label` when the truth of eax/st0 equals want, keeping the value */
static void truth_jump(F *f, Ty *t, int label, int want){
    t=ty_find(t);
    if(t->k==TY_FLOAT){ E(f,"ftst"); E(f,"fnstsw ax"); E(f,"sahf"); E(f,"j%s L%d",want?"ne":"e",label); return; }
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
/* Left value pushed on the machine stack, right in eax/st0 -> eax = bool. */
static void cmp_finish(F *f, int code, Ty *ta, Ty *tb, int line){
    ta=ty_find(ta); tb=ty_find(tb);
    if(code==CMP_IN||code==CMP_NOTIN){
        if(tb->k==TY_STR){ E(f,"pop edx"); CALLRT(f,"rt_str_find"); E(f,"cmp eax,-1"); E(f,"set%s al",code==CMP_IN?"ne":"e"); E(f,"movzx eax,al"); return; }
        if(tb->k==TY_DICT){ E(f,"pop edx"); CALLRT(f,"rt_dict_find"); E(f,"cmp eax,-1"); E(f,"set%s al",code==CMP_IN?"ne":"e"); E(f,"movzx eax,al"); return; }
        int k=kind_of(tb->elem);
        if(k==2){ E(f,"mov edx,esp"); E(f,"mov ecx,2"); CALLRT(f,"rt_list_find"); E(f,"add esp,8"); }
        else { E(f,"pop edx"); E(f,"mov ecx,%d",k); CALLRT(f,"rt_list_find"); }
        E(f,"cmp eax,-1"); E(f,"set%s al",code==CMP_IN?"ne":"e"); E(f,"movzx eax,al"); return;
    }
    if(numeric_ty(ta)&&numeric_ty(tb)){
        if(ta->k==TY_FLOAT||tb->k==TY_FLOAT){
            conv_num(f,tb,TY_FLOAT_T);
            if(ta->k==TY_FLOAT){ E(f,"fld qword [esp]"); E(f,"add esp,8"); } else { E(f,"fild dword [esp]"); E(f,"add esp,4"); }
            E(f,"fcompp"); E(f,"fnstsw ax"); E(f,"sahf");
            E(f,"set%s al",cc_of(code,1)); E(f,"movzx eax,al"); return;
        }
        E(f,"mov ecx,eax"); E(f,"pop eax"); E(f,"cmp eax,ecx"); E(f,"set%s al",cc_of(code,0)); E(f,"movzx eax,al"); return;
    }
    if(ta->k==TY_STR&&tb->k==TY_STR){
        E(f,"mov edx,eax"); E(f,"pop eax");
        if(code==CMP_EQ||code==CMP_NE){ CALLRT(f,"rt_str_eq"); if(code==CMP_NE) E(f,"xor eax,1"); return; }
        CALLRT(f,"rt_str_cmp"); E(f,"cmp eax,0"); E(f,"set%s al",cc_of(code,0)); E(f,"movzx eax,al"); return;
    }
    if(ta->k==TY_TUPLE&&tb->k==TY_TUPLE){
        E(f,"mov edx,eax"); E(f,"pop eax");
        if(code==CMP_EQ||code==CMP_NE){ CALLRT(f,"rt_tuple_eq"); if(code==CMP_NE) E(f,"xor eax,1"); return; }
        CALLRT(f,"rt_tuple_cmp"); E(f,"cmp eax,0"); E(f,"set%s al",cc_of(code,0)); E(f,"movzx eax,al"); return;
    }
    if(ta->k==TY_DICT&&(code==CMP_EQ||code==CMP_NE)){
        E(f,"mov edx,eax"); E(f,"pop eax"); E(f,"mov ecx,%d",kind_of(ta->elem)); CALLRT(f,"rt_dict_eq");
        if(code==CMP_NE) E(f,"xor eax,1"); return;
    }
    if((ta->k==TY_LIST||ta->k==TY_SET)&&(code==CMP_EQ||code==CMP_NE)){
        E(f,"mov edx,eax"); E(f,"pop eax"); E(f,"mov ecx,%d",kind_of(ta->elem)); CALLRT(f,ta->k==TY_LIST?"rt_list_eq":"rt_set_eq");
        if(code==CMP_NE) E(f,"xor eax,1"); return;
    }
    if(code==CMP_EQ||code==CMP_NE||code==CMP_IS||code==CMP_ISNOT){     /* identity */
        E(f,"mov ecx,eax"); E(f,"pop eax"); E(f,"cmp eax,ecx"); E(f,"set%s al",cc_of(code,0)); E(f,"movzx eax,al"); return;
    }
    cg_fail(line,"unsupported comparison");
}
static void gen_cmp2_m(F *f, int code, Expr *a, Expr *b, int line, AFunc *m, int neg){
    if(code==CMP_IN||code==CMP_NOTIN){ int o=gen_op_call(f,m,b,a); (void)o; }     /* b.__contains__(a) */
    else gen_op_call(f,m,a,b);
    Ty *r=ty_find(m->ret);
    if(r->k!=TY_BOOL) truth(f,r);
    if(neg || code==CMP_NOTIN) E(f,"xor eax,1");
}
static void gen_cmp2(F *f, int code, Expr *a, Expr *b, int line){
    if(is_none_lit(a)||is_none_lit(b)){                 /* x is None / x == None: x is its zero value */
        Expr *x=is_none_lit(a)?b:a;
        if(is_none_lit(x)){ E(f,"mov eax,%d",(code==CMP_EQ||code==CMP_IS)?1:0); return; }
        gen_borrow(f,x); truth(f,TY(x));
        if(code==CMP_EQ||code==CMP_IS) E(f,"xor eax,1");
        return;
    }
    Ty *ta=TY(a), *tb=TY(b);
    if((code==CMP_IN||code==CMP_NOTIN) && (tb->k==TY_LIST||tb->k==TY_SET) && is_flt(tb->elem)) ta=TY_FLOAT_T;
    gen_as(f,a,ta,0); push_value(f,ta);
    gen_borrow(f,b);
    cmp_finish(f,code,ta,tb,line);
}
static void gen_compare(F *f, Expr *e){
    XInfo *xi=xinfo(e);
    if(e->count==2 && xi->cmpfn && xi->cmpfn[1]){ gen_cmp2_m(f,e->items[1]->akind,e->items[0],e->items[1],e->line,xi->cmpfn[1],(xi->cmpneg>>1)&1); return; }
    if(e->count==2){ gen_cmp2(f,e->items[1]->akind,e->items[0],e->items[1],e->line); return; }
    int lend=new_label(), slot=frame_slot(f,8);           /* a < b < c: each middle operand once */
    Ty *prev=TY(e->items[0]);
    gen_borrow(f,e->items[0]);
    if(is_flt(prev)) E(f,"fstp qword [ebp%+d]",slot); else E(f,"mov [ebp%+d],eax",slot);
    for(int i=1;i<e->count;i++){
        Ty *t=TY(e->items[i]);
        if(is_flt(prev)){ E(f,"sub esp,8"); E(f,"fld qword [ebp%+d]",slot); E(f,"fstp qword [esp]"); } else E(f,"push dword [ebp%+d]",slot);
        gen_borrow(f,e->items[i]);
        if(i<e->count-1){ if(is_flt(t)){ E(f,"fst qword [ebp%+d]",slot); } else E(f,"mov [ebp%+d],eax",slot); }
        cmp_finish(f,e->items[i]->akind,prev,t,e->line);
        if(i<e->count-1){ E(f,"test eax,eax"); E(f,"jz L%d",lend); }
        prev=t;
    }
    LBL(f,lend);
}

/* eax = truth of e */
static void gen_bool(F *f, Expr *e){
    if(e->kind==EXPR_COMPARE){ gen_compare(f,e); return; }
    if(e->kind==EXPR_UNARY && e->op==T_NOT){ gen_bool(f,e->a); E(f,"xor eax,1"); return; }
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
       && !is_none_lit(e->items[0]) && !is_none_lit(e->items[1]) && e->items[1]->akind<=CMP_NE){
        gen(f,e->items[0]); E(f,"push eax"); gen(f,e->items[1]); E(f,"mov ecx,eax"); E(f,"pop eax");
        const char *cc=cc_of(e->items[1]->akind,0);
        if(f->tmp_used==mark){ E(f,"cmp eax,ecx"); E(f,"j%s L%d",want?cc:neg_cc(cc),label); return; }
        E(f,"cmp eax,ecx"); E(f,"set%s al",cc); E(f,"movzx eax,al");
    } else gen_bool(f,e);
    if(f->tmp_used>mark){ E(f,"push eax"); scope_close(f,mark); E(f,"pop eax"); }
    E(f,"test eax,eax"); E(f,"j%s L%d",want?"nz":"z",label);
}

/* ---- iteration (for loops and comprehensions) ---- */
typedef enum { IT_RANGE, IT_SEQ, IT_STR, IT_KEYS, IT_ITEMS, IT_ENUM, IT_ZIP, IT_TUP, IT_GEN } ItKind;
typedef struct {
    ItKind kind;
    int top, cont, exit;
    int idx, end, step, start;      /* frame slots */
    int step_const, has_step_const;
    int src, src2;                  /* reference slots of the iterated objects */
    Ty *elem, *elem2;               /* element types (enumerate: elem2 = the items) */
    ItKind sub, sub2;               /* enumerate/zip: what the underlying objects are */
} Iter;
static ItKind seq_kind(Ty *t){ t=ty_find(t); return t->k==TY_STR?IT_STR:t->k==TY_DICT?IT_KEYS:t->k==TY_TUPLE?IT_TUP:t->k==TY_GEN?IT_GEN:IT_SEQ; }
static Ty *seq_elem(Ty *t){ t=ty_find(t); return t->k==TY_STR?TY_STR_T:t->k==TY_DICT?ty_dkey(t):t->k==TY_TUPLE?t->elems[0]:t->elem; }
/* the object a loop goes over, kept in a slot of its own: a sequence, a
   generator, obj.__iter__()'s result, a file's lines -> its kind and items */
static ItKind iter_source(F *f, Expr *x, int *slot, Ty **elem){
    Ty *t=TY(x);
    *slot=ref_slot(f);
    if(t->k==TY_OBJ){
        AFunc *m=aot_find_method(t->cls,"__iter__"); Ty *r=ty_find(m->ret);
        E(f,"sub esp,4"); gen_borrow(f,x); E(f,"mov [esp],eax"); call_method_on_top(f,m); E(f,"add esp,4");
        E(f,"mov [ebp%+d],eax",*slot); *elem=r->elem; return r->k==TY_GEN?IT_GEN:IT_SEQ;
    }
    if(t->k==TY_FILE){ gen_borrow(f,x); CALLRT(f,"rt_file_readlines"); E(f,"mov [ebp%+d],eax",*slot); *elem=TY_STR_T; return IT_SEQ; }
    gen_owned(f,x); E(f,"mov [ebp%+d],eax",*slot);
    *elem=seq_elem(t); return seq_kind(t);
}
static void iter_begin(F *f, Expr *it, Iter *I){
    memset(I,0,sizeof *I);
    I->top=new_label(); I->cont=new_label(); I->exit=new_label();
    XInfo *xi=xinfo(it);
    I->idx=frame_slot(f,4);
    if(xi->kind==X_BUILTIN && !strcmp(xi->name,"reversed_range")){     /* stop-1 down to start */
        Expr *r=it->items[0];
        I->kind=IT_RANGE; I->elem=TY_INT_T; I->end=frame_slot(f,4); I->has_step_const=1; I->step_const=-1;
        if(r->count==1){ gen(f,r->items[0]); E(f,"dec eax"); E(f,"mov [ebp%+d],eax",I->idx); E(f,"mov dword [ebp%+d],-1",I->end); }
        else { gen(f,r->items[1]); E(f,"dec eax"); E(f,"mov [ebp%+d],eax",I->idx); gen(f,r->items[0]); E(f,"dec eax"); E(f,"mov [ebp%+d],eax",I->end); }
        LBL(f,I->top);
        E(f,"mov eax,[ebp%+d]",I->idx); E(f,"cmp eax,[ebp%+d]",I->end); E(f,"jle L%d",I->exit);
        return;
    }
    if(xi->kind==X_BUILTIN && !strcmp(xi->name,"range")){
        I->kind=IT_RANGE; I->elem=TY_INT_T; I->end=frame_slot(f,4);
        if(it->count==1){ E(f,"mov dword [ebp%+d],0",I->idx); gen(f,it->items[0]); E(f,"mov [ebp%+d],eax",I->end); }
        else { gen(f,it->items[0]); E(f,"mov [ebp%+d],eax",I->idx); gen(f,it->items[1]); E(f,"mov [ebp%+d],eax",I->end); }
        I->has_step_const=1; I->step_const=1;
        if(it->count==3){
            Expr *s=it->items[2]; int neg=0;
            if(s->kind==EXPR_UNARY&&s->op==T_MINUS&&s->a->kind==EXPR_LITERAL){ neg=1; s=s->a; }
            if(s->kind==EXPR_LITERAL && s->tok->kind==T_NUMBER){ I->step_const=(int)(neg?-s->tok->i:s->tok->i); if(!I->step_const) cg_fail(it->line,"range() step must not be zero"); }
            else { I->has_step_const=0; I->step=frame_slot(f,4); gen(f,it->items[2]); E(f,"mov [ebp%+d],eax",I->step);
                int ok=new_label(); E(f,"test eax,eax"); E(f,"jnz L%d",ok); E(f,"mov esi,Z%d",zlit("range() arg 3 must not be zero")); CALLRT(f,"rt_panic_value"); LBL(f,ok); }
        }
        LBL(f,I->top);
        E(f,"mov eax,[ebp%+d]",I->idx);
        E(f,"cmp eax,[ebp%+d]",I->end);
        if(I->has_step_const) E(f,"j%s L%d",I->step_const>0?"ge":"le",I->exit);
        else { int neg=new_label(), go=new_label();
            E(f,"cmp dword [ebp%+d],0",I->step); E(f,"jl L%d",neg);
            E(f,"cmp eax,[ebp%+d]",I->end); E(f,"jge L%d",I->exit); E(f,"jmp L%d",go);
            LBL(f,neg); E(f,"cmp eax,[ebp%+d]",I->end); E(f,"jle L%d",I->exit); LBL(f,go); }
        return;
    }
    E(f,"mov dword [ebp%+d],0",I->idx);
    if(xi->kind==X_BUILTIN && (!strcmp(xi->name,"enumerate")||!strcmp(xi->name,"zip"))){
        int isenum=xi->name[0]=='e'; Ty *e0, *e1;
        I->kind=isenum?IT_ENUM:IT_ZIP;
        I->sub=iter_source(f,it->items[0],&I->src,&e0);
        if(isenum){ I->elem=TY_INT_T; I->elem2=e0; I->start=frame_slot(f,4);
            if(it->count==2){ gen(f,it->items[1]); E(f,"mov [ebp%+d],eax",I->start); } else E(f,"mov dword [ebp%+d],0",I->start); }
        else { I->sub2=iter_source(f,it->items[1],&I->src2,&e1); I->elem=e0; I->elem2=e1; }
    } else if(xi->kind==X_TMETHOD && !strcmp(xi->name,"items")){
        Ty *e0; iter_source(f,it->a->a,&I->src,&e0); I->kind=IT_ITEMS; I->elem=ty_dkey(TY(it->a->a)); I->elem2=TY(it->a->a)->elem;
    } else {
        Ty *el; I->kind=iter_source(f,it,&I->src,&el); I->elem=el;
    }
    LBL(f,I->top);
    int srcs[2]={I->src,I->src2};
    for(int k=0;k<(I->kind==IT_ZIP?2:1);k++){
        ItKind sk=I->kind==IT_ZIP||I->kind==IT_ENUM ? (k?I->sub2:I->sub) : I->kind;
        E(f,"mov eax,[ebp%+d]",srcs[k]); E(f,"test eax,eax"); E(f,"jz L%d",I->exit);
        if(sk==IT_GEN){ CALLRT(f,"rt_gen_next"); E(f,"test eax,eax"); E(f,"jz L%d",I->exit); continue; }
        E(f,"mov ecx,[ebp%+d]",I->idx);
        if(sk==IT_TUP){ E(f,"mov edx,[eax+8]"); E(f,"cmp ecx,[edx]"); } else E(f,"cmp ecx,[eax+8]");
        E(f,"jae L%d",I->exit);
    }
}
/* value number k (0 or 1) of the current iteration -> eax/st0 (borrowed) */
static void iter_value(F *f, Iter *I, int k){
    ItKind kind=I->kind; int src=I->src; Ty *el=k?I->elem2:I->elem;
    if(kind==IT_RANGE){ E(f,"mov eax,[ebp%+d]",I->idx); return; }
    if(kind==IT_ENUM){ if(k==0){ E(f,"mov eax,[ebp%+d]",I->idx); E(f,"add eax,[ebp%+d]",I->start); return; } kind=I->sub; }
    else if(kind==IT_ZIP){ kind=k?I->sub2:I->sub; src=k?I->src2:I->src; }
    E(f,"mov eax,[ebp%+d]",src);
    if(kind==IT_GEN){ load_mem(f,el,"eax",24); return; }
    E(f,"mov ecx,[ebp%+d]",I->idx);
    if(kind==IT_STR){ rt("rt_chars"); E(f,"movzx eax,byte [eax+12+ecx]"); E(f,"shl eax,4"); E(f,"add eax,rt_chars"); return; }
    if(kind==IT_TUP){ if(is_flt(el)) E(f,"fld qword [eax+12+ecx*8]"); else E(f,"mov eax,[eax+12+ecx*4]"); return; }
    if(kind==IT_KEYS||(kind==IT_ITEMS&&k==0)){ E(f,"mov eax,[eax+16]"); E(f,"mov eax,[eax+ecx*4]"); return; }
    if(kind==IT_ITEMS){ E(f,"mov eax,[eax+20]"); if(is_flt(el)) E(f,"fld qword [eax+ecx*8]"); else E(f,"mov eax,[eax+ecx*4]"); return; }
    E(f,"mov eax,[eax+16]");
    if(is_flt(el)) E(f,"fld qword [eax+ecx*8]"); else E(f,"mov eax,[eax+ecx*4]");
}
static void iter_end(F *f, Iter *I){
    LBL(f,I->cont);
    if(I->kind==IT_RANGE){ if(I->has_step_const) E(f,"add dword [ebp%+d],%d",I->idx,I->step_const); else { E(f,"mov eax,[ebp%+d]",I->step); E(f,"add [ebp%+d],eax",I->idx); } }
    else E(f,"inc dword [ebp%+d]",I->idx);
    E(f,"jmp L%d",I->top);
}
static void iter_release(F *f, Iter *I){
    int srcs[2]={I->src,I->src2};
    for(int k=0;k<2;k++) if(srcs[k]){ rt("rt_decref"); E(f,"mov eax,[ebp%+d]",srcs[k]); E(f,"mov dword [ebp%+d],0",srcs[k]); E(f,"call rt_decref"); }
}
/* store the iteration value (borrowed in eax/st0) into a variable */
static void store_borrowed(F *f, AVar *v, Ty *from){
    if(v->borrowed){ conv_num(f,from,v->ty); E(f,"mov [ebp%+d],eax",v->offset); return; }   /* the item stays the container's */
    if(is_ptr(v->ty)) incref(f);
    conv(f,from,v->ty);
    store_var(f,v);
}
static int tuple_off(Ty *t, int i);
/* The loop variables of one iteration: `x`, `i, x` (enumerate/zip/items), or
   `a, b` unpacking each element (a list or a tuple). */
static void iter_store_vars(F *f, Iter *I, AVar **vars, int n){
    if(n==1 || I->kind==IT_ENUM || I->kind==IT_ZIP || I->kind==IT_ITEMS){
        for(int k=0;k<n;k++){ iter_value(f,I,k); store_borrowed(f,vars[k],k?I->elem2:I->elem); }
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
            if(is_flt(el)) E(f,"fld qword [eax+%d]",8*k); else E(f,"mov eax,[eax+%d]",4*k);
            store_borrowed(f,vars[k],el);
        }
    }
    E(f,"add esp,4");
}

static int gen_comprehension(F *f, Expr *e){
    Ty *t=TY(e);
    int res=ref_slot(f);
    if(e->comp_kind=='D') new_dict(f,t);
    else { E(f,"mov eax,%s",list_destroy(t->elem)); CALLRT(f,"rt_list_new"); }
    E(f,"mov [ebp%+d],eax",res);
    Iter its[8]; int n=e->nclause;
    if(n>8) cg_fail(e->line,"too many for clauses");
    for(int i=0;i<n;i++){
        CompClause *cl=&e->clauses[i];
        iter_begin(f,cl->iter,&its[i]);
        iter_store_vars(f,&its[i],&xinfo(e)->cvars[2*i],cl->nvars);
        for(int k=0;k<cl->ncond;k++) gen_jump(f,cl->conds[k],its[i].cont,0);
    }
    int mark=scope_open(f);
    Ty *el=t->elem;
    if(e->comp_kind=='L'||e->comp_kind=='g'){
        gen_as(f,e->a,el,1);
        if(is_flt(el)){ E(f,"mov eax,[ebp%+d]",res); E(f,"mov edx,8"); CALLRT(f,"rt_list_push"); E(f,"fstp qword [eax]"); }
        else { E(f,"push eax"); E(f,"mov eax,[ebp%+d]",res); E(f,"mov edx,4"); CALLRT(f,"rt_list_push"); E(f,"pop ecx"); E(f,"mov [eax],ecx"); }
    } else if(e->comp_kind=='S'){
        gen_as(f,e->a,el,0);
        if(is_flt(el)){ E(f,"sub esp,8"); E(f,"fstp qword [esp]"); E(f,"mov edx,esp"); E(f,"mov eax,[ebp%+d]",res); E(f,"mov ecx,2"); CALLRT(f,"rt_set_add"); E(f,"add esp,8"); }
        else { E(f,"mov edx,eax"); E(f,"mov eax,[ebp%+d]",res); E(f,"mov ecx,%d",kind_of(el)); CALLRT(f,"rt_set_add"); }
    } else {
        gen_as(f,e->b,el,1); push_value(f,el);
        gen_borrow(f,e->a); E(f,"mov edx,eax"); E(f,"mov eax,[ebp%+d]",res); E(f,"mov ecx,%d",esize(el)); CALLRT(f,"rt_dict_slot");
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
static int tdesc(Ty *t){
    t=ty_find(t);
    for(int i=0;i<gg->ntdescs;i++) if(ty_same(gg->tdescs[i],t)) return i;
    if(gg->ntdescs==gg->ctdescs){ gg->ctdescs=gg->ctdescs?gg->ctdescs*2:8; gg->tdescs=(Ty**)xrealloc(gg->tdescs,sizeof(Ty*)*(size_t)gg->ctdescs); }
    gg->tdescs[gg->ntdescs]=t;
    return gg->ntdescs++;
}
static int gen_tuple(F *f, Expr *e){
    Ty *t=TY(e);
    E(f,"mov eax,TD%d",tdesc(t)); E(f,"mov edx,%d",tuple_size(t)); CALLRT(f,"rt_tuple_new");
    E(f,"push eax");
    for(int i=0;i<e->count;i++){ Ty *et=t->elems[i]; gen_as(f,e->items[i],et,1); E(f,"mov ecx,[esp]"); store_new(f,et,"ecx",tuple_off(t,i)); }
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
        if(t->k==TY_STR){ int l=new_label(), l2=new_label();            /* its bytes, NUL-terminated ("" too) */
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
        gen_syscall_regs(f,e->a); E(f,"mov eax,[esp+%d]",4*(int)e->b->tok->i); E(f,"add esp,28"); return 0;
    }
    Ty *ct=TY(e->a);
    if(ct->k==TY_OBJ) return gen_op_call(f,aot_find_method(ct->cls,"__getitem__"),e->a,e->b);   /* obj[key] */
    if(ct->k==TY_TUPLE){
        XInfo *xi=xinfo(e);
        if(xi->argmap[1]){ gen_borrow(f,e->a); panic_if_null(f); load_mem(f,ct->elems[xi->argmap[0]],"eax",tuple_off(ct,xi->argmap[0])); return 0; }
        Ty *et=ct->elems[0];                                     /* all items of one type */
        gen_borrow(f,e->a); E(f,"push eax"); gen(f,e->b); E(f,"mov edx,eax"); E(f,"pop eax");
        E(f,"mov ecx,%d",esize(et)); CALLRT(f,"rt_tuple_at"); load_mem(f,et,"eax",0); return 0;
    }
    gen_borrow(f,e->a); E(f,"push eax");
    if(ct->k==TY_DICT) gen_borrow(f,e->b); else gen(f,e->b);
    E(f,"mov edx,eax"); E(f,"pop eax");
    if(ct->k==TY_STR){ CALLRT(f,"rt_str_char"); return 0; }
    if(ct->k==TY_LIST){ E(f,"mov ecx,%d",esize(ct->elem)); CALLRT(f,"rt_list_at"); load_mem(f,ct->elem,"eax",0); return 0; }
    E(f,"mov ecx,%d",esize(ct->elem)); CALLRT(f,"rt_dict_get"); load_mem(f,ct->elem,"eax",0); return 0;
}
static int gen_slice(F *f, Expr *e){
    Ty *t=TY(e->a); int list=t->k==TY_LIST, n=list?24:20, flags=0;
    E(f,"sub esp,%d",n);
    gen_borrow(f,e->a); E(f,"mov [esp],eax");
    Expr *parts[3]={e->b,e->c,e->d};
    for(int k=0;k<3;k++){
        if(parts[k] && parts[k]->kind!=EXPR_NONE){ gen(f,parts[k]); E(f,"mov [esp+%d],eax",4+4*k); }
        else { flags|=1<<k; E(f,"mov dword [esp+%d],0",4+4*k); }
    }
    E(f,"mov dword [esp+16],%d",flags);
    if(list) E(f,"mov dword [esp+20],%d",kind_of(t->elem));
    CALLRT(f,list?"rt_list_slice":"rt_str_slice");
    E(f,"add esp,%d",n);
    return 1;
}

/* ---- container literals ---- */
static void list_append_top(F *f, Ty *el){         /* list at [esp], value in eax/st0 (owned) */
    if(is_flt(el)){ E(f,"mov eax,[esp]"); E(f,"mov edx,8"); CALLRT(f,"rt_list_push"); E(f,"fstp qword [eax]"); }
    else { E(f,"push eax"); E(f,"mov eax,[esp+4]"); E(f,"mov edx,4"); CALLRT(f,"rt_list_push"); E(f,"pop ecx"); E(f,"mov [eax],ecx"); }
}
static void set_add_top(F *f, Ty *el){             /* set at [esp], value in eax/st0 (borrowed) */
    if(is_flt(el)){ E(f,"sub esp,8"); E(f,"fstp qword [esp]"); E(f,"mov edx,esp"); E(f,"mov eax,[esp+8]"); E(f,"mov ecx,2"); CALLRT(f,"rt_set_add"); E(f,"add esp,8"); }
    else { E(f,"mov edx,eax"); E(f,"mov eax,[esp]"); E(f,"mov ecx,%d",kind_of(el)); CALLRT(f,"rt_set_add"); }
}
static int gen_literal_container(F *f, Expr *e){
    Ty *t=TY(e), *el=t->elem;
    if(e->kind==EXPR_DICT){
        new_dict(f,t); E(f,"push eax");
        for(int i=0;i<e->count;i++){
            gen_as(f,e->vals[i],el,1); push_value(f,el);
            gen_borrow(f,e->items[i]); E(f,"mov edx,eax"); E(f,"mov eax,[esp+%d]",esize(el)); E(f,"mov ecx,%d",esize(el)); CALLRT(f,"rt_dict_slot");
            E(f,"mov ecx,eax");
            if(is_flt(el)){ E(f,"fld qword [esp]"); E(f,"add esp,8"); E(f,"fstp qword [ecx]"); }
            else if(is_ptr(el)){ E(f,"pop eax"); store_mem(f,el,"ecx",0); }
            else { E(f,"pop eax"); E(f,"mov [ecx],eax"); }
        }
        E(f,"pop eax"); return 1;
    }
    E(f,"mov eax,%s",list_destroy(el)); CALLRT(f,"rt_list_new"); E(f,"push eax");
    for(int i=0;i<e->count;i++){
        if(e->kind==EXPR_LIST||e->kind==EXPR_TUPLE){ gen_as(f,e->items[i],el,1); list_append_top(f,el); }
        else { gen_as(f,e->items[i],el,0); set_add_top(f,el); }
    }
    E(f,"pop eax"); return 1;
}

/* ---- calls ---- */
static void put_arg(F *f, Ty *pt, int off){ if(is_flt(pt)) E(f,"fstp qword [esp+%d]",off); else E(f,"mov [esp+%d],eax",off); }
static int tuple_off(Ty *t, int i);
static void list_append_top(F *f, Ty *el);
static int in_list(int *v, int n, int x){ for(int i=0;i<n;i++) if(v[i]==x) return 1; return 0; }
/* *args of a call: a new tuple (list) of the extra positionals and f(*xs) -> eax (held) */
static void gen_star_args(F *f, AFunc *fn, Expr *e, XInfo *xi){
    Ty *el=ty_find(fn->params[fn->star]->ty)->elem;
    E(f,"mov eax,%s",list_destroy(el)); CALLRT(f,"rt_list_new"); E(f,"push eax");
    for(int j=0;j<xi->nxargs;j++){ gen_as(f,e->items[xi->xargs[j]],el,1); list_append_top(f,el); }
    if(xi->splat){
        Expr *sa=e->items[xi->splat-1]; Ty *st=TY(sa);
        if(st->k==TY_LIST && is_flt(el)==is_flt(st->elem)){ gen_borrow(f,sa); E(f,"mov edx,eax"); E(f,"mov eax,[esp]"); E(f,"mov ecx,%d",kind_of(el)); CALLRT(f,"rt_list_extend"); }
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
        E(f,"mov edx,S%d",str_lit(a->kw)); E(f,"mov eax,[esp+%d]",esize(el)); E(f,"mov ecx,%d",esize(el)); CALLRT(f,"rt_dict_slot");
        E(f,"mov ecx,eax"); pop_value(f,el); store_mem(f,el,"ecx",0); }
    if(xi->dsplat){ gen_borrow(f,e->items[xi->dsplat-1]); E(f,"mov edx,eax"); E(f,"mov eax,[esp]"); E(f,"mov ecx,%d",kind_of(el)); CALLRT(f,"rt_dict_update"); }
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
        gen_as(f,fn->defaults[i],pt,fn->params[i]->consumed); put_arg(f,pt,off[i]);
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
    if(xi->kind==X_CALLNEST && fn->ncaps){               /* its closure in edx */
        if(xi->var){ load_var(f,xi->var); panic_if_null(f); E(f,"mov edx,eax"); }
        else E(f,"mov edx,[ebp%+d]",f->env);
    }
    if(self_kind==1 && fn->cls && !fn->is_static){
        E(f,"mov eax,[esp]"); panic_if_null(f);
        if(fn->overridden){ E(f,"mov eax,[eax+8]"); E(f,"call dword [eax+%d]",8+4*fn->vslot); }
        else E(f,"call F%d",fn->id);
    } else E(f,"call F%d",fn->id);
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
    E(f,"mov eax,[ebp%+d]",cs); panic_if_null(f); E(f,"mov edx,eax"); E(f,"call dword [edx+8]");
    if(total) E(f,"add esp,%d",total);
    return is_ptr(ft->elem);
}
static int call_value(F *f, Expr *e){ return call_value_of(f,e,NULL); }
static int gen_str_of(F *f, Expr *x);
/* the message of an exception: str(x), owned, in eax */
static void gen_exc_message(F *f, Expr *x){ int o=gen_str_of(f,x); if(!o) incref(f); }
static int gen_ctor(F *f, Expr *e, XInfo *xi){
    AClass *cls=xi->cls; AFunc *init=xi->fn;
    use_class(cls);
    if(!init && aot_is_exception(cls)){                  /* ValueError("message") */
        if(e->kind==EXPR_CALL && e->count){ gen_exc_message(f,e->items[0]); E(f,"push eax"); } else E(f,"push 0");
        E(f,"mov eax,%d",cls->size); E(f,"mov edx,VT%d",cls->id); E(f,"mov ecx,DT%d",cls->id); CALLRT(f,"rt_obj_new");
        E(f,"pop ecx"); E(f,"mov [eax+12],ecx");
        AClass *chain[32]; int n=0; for(AClass *c=cls;c&&n<32;c=c->base) chain[n++]=c;
        for(int k=n-1;k>=0;k--) for(int i=0;i<chain[k]->nfields;i++){ AField *fd=chain[k]->fields[i];
            if(!fd->init) continue;
            E(f,"push eax"); gen_as(f,fd->init,fd->ty,1); E(f,"mov ecx,[esp]"); store_new(f,fd->ty,"ecx",fd->offset); E(f,"pop eax");
        }
        return 1;
    }
    int off[16], total=0;
    if(init){ use_fn(init); for(int i=0;i<init->nparams;i++){ off[i]=total; total+=esize(init->params[i]->ty); } }
    if(total) E(f,"sub esp,%d",total);
    if(init) gen_call_args(f,init,e,xi,off,1);
    E(f,"mov eax,%d",cls->size); E(f,"mov edx,VT%d",cls->id); E(f,"mov ecx,DT%d",cls->id); CALLRT(f,"rt_obj_new");
    AClass *chain[32]; int n=0; for(AClass *c=cls;c&&n<32;c=c->base) chain[n++]=c;
    for(int k=n-1;k>=0;k--) for(int i=0;i<chain[k]->nfields;i++){ AField *fd=chain[k]->fields[i];
        if(!fd->init) continue;
        E(f,"push eax");
        gen_as(f,fd->init,fd->ty,1);
        E(f,"mov ecx,[esp]");
        store_new(f,fd->ty,"ecx",fd->offset);
        E(f,"pop eax");
    }
    if(init){ E(f,"mov [esp],eax"); E(f,"call F%d",init->id); E(f,"mov eax,[esp]"); }
    if(total) E(f,"add esp,%d",total);
    return 1;
}
static void call_method_on_top(F *f, AFunc *m){      /* object at [esp], one 4-byte argument area */
    use_fn(m);
    E(f,"mov eax,[esp]"); panic_if_null(f);
    if(m->overridden){ E(f,"mov eax,[eax+8]"); E(f,"call dword [eax+%d]",8+4*m->vslot); }
    else E(f,"call F%d",m->id);
}

static int sort_kind(Ty *t);
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
    E(f,"call F%d",fn->id); E(f,"add esp,%d",esize(pt));
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
    E(f,"mov eax,%s",list_destroy(pt)); CALLRT(f,"rt_list_new"); E(f,"mov [ebp%+d],eax",pairs);
    E(f,"mov dword [ebp%+d],0",idx);
    LBL(f,ltop);
    E(f,"mov eax,[ebp%+d]",src); E(f,"test eax,eax"); E(f,"jz L%d",lend);
    E(f,"mov ecx,[ebp%+d]",idx); E(f,"cmp ecx,[eax+8]"); E(f,"jae L%d",lend);
    int mark=scope_open(f);
    E(f,"mov eax,[eax+16]"); if(is_flt(el)) E(f,"fld qword [eax+ecx*8]"); else E(f,"mov eax,[eax+ecx*4]");
    gen_key_call(f,key,el); push_value(f,kt);
    E(f,"mov eax,TD%d",td); E(f,"mov edx,%d",tuple_size(pt)); CALLRT(f,"rt_tuple_new");
    E(f,"mov ecx,eax"); pop_value(f,kt);
    if(is_ptr(kt)){ E(f,"push ecx"); incref(f); E(f,"pop ecx"); }
    store_new(f,kt,"ecx",koff);
    { int l=new_label(); E(f,"mov eax,[ebp%+d]",idx); E(f,"cmp dword [ebp%+d],0",revslot); E(f,"je L%d",l); E(f,"neg eax"); LBL(f,l); }  /* reverse keeps equal keys in order */
    E(f,"mov [ecx+%d],eax",ioff);
    E(f,"push ecx"); E(f,"mov eax,[ebp%+d]",pairs); E(f,"mov edx,4"); CALLRT(f,"rt_list_push"); E(f,"pop ecx"); E(f,"mov [eax],ecx");
    scope_close(f,mark);
    E(f,"inc dword [ebp%+d]",idx); E(f,"jmp L%d",ltop);
    LBL(f,lend);
    rt("rt_tuple_cmp");
    E(f,"mov eax,[ebp%+d]",pairs); E(f,"mov edx,4"); CALLRT(f,"rt_list_sort");
    { int l=new_label(); E(f,"cmp dword [ebp%+d],0",revslot); E(f,"je L%d",l); E(f,"mov eax,[ebp%+d]",pairs); E(f,"mov edx,4"); CALLRT(f,"rt_list_reverse"); LBL(f,l); }
    E(f,"mov eax,%s",list_destroy(el)); CALLRT(f,"rt_list_new"); E(f,"mov [ebp%+d],eax",res);
    E(f,"mov dword [ebp%+d],0",idx);
    LBL(f,l2);
    E(f,"mov eax,[ebp%+d]",pairs); E(f,"mov ecx,[ebp%+d]",idx); E(f,"cmp ecx,[eax+8]"); E(f,"jae L%d",l2end);
    E(f,"mov eax,[eax+16]"); E(f,"mov eax,[eax+ecx*4]"); E(f,"mov ecx,[eax+%d]",ioff);
    { int l=new_label(); E(f,"test ecx,ecx"); E(f,"jns L%d",l); E(f,"neg ecx"); LBL(f,l); }
    E(f,"mov eax,[ebp%+d]",src); E(f,"mov eax,[eax+16]");
    if(is_flt(el)) E(f,"fld qword [eax+ecx*8]"); else { E(f,"mov eax,[eax+ecx*4]"); if(is_ptr(el)) incref(f); }
    E(f,"mov edx,[ebp%+d]",res); E(f,"push edx");
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
    if(lt->k==TY_DICT){ gen_borrow(f,e->items[0]); CALLRT(f,"rt_dict_keys"); } else gen_owned(f,e->items[0]);
    E(f,"mov [ebp%+d],eax",src);
    { int l=new_label(), lok=new_label(); E(f,"test eax,eax"); E(f,"jz L%d",l); E(f,"cmp dword [eax+8],0"); E(f,"jne L%d",lok); LBL(f,l);
      E(f,"mov esi,Z%d",zlit(is_max?"max() arg is an empty sequence":"min() arg is an empty sequence")); CALLRT(f,"rt_panic_value"); LBL(f,lok); }
    E(f,"mov dword [ebp%+d],0",idx); E(f,"mov dword [ebp%+d],-1",best);
    LBL(f,ltop);
    E(f,"mov eax,[ebp%+d]",src); E(f,"mov ecx,[ebp%+d]",idx); E(f,"cmp ecx,[eax+8]"); E(f,"jae L%d",lend);
    int mark=scope_open(f);
    E(f,"mov eax,[eax+16]"); if(is_flt(el)) E(f,"fld qword [eax+ecx*8]"); else E(f,"mov eax,[eax+ecx*4]");
    gen_key_call(f,key,el);
    E(f,"cmp dword [ebp%+d],-1",best); E(f,"je L%d",ltake);
    if(is_flt(kt)){ E(f,"fld qword [ebp%+d]",bkey); E(f,"fcomp st1"); E(f,"fnstsw ax"); E(f,"sahf"); E(f,"j%s L%d",is_max?"b":"a",ltake); E(f,"fstp st0"); E(f,"jmp L%d",lnext); }
    else {
        const char *cmpfn=kt->k==TY_STR?"rt_str_cmp":kt->k==TY_TUPLE?"rt_tuple_cmp":NULL;
        if(cmpfn){ E(f,"push eax"); E(f,"mov edx,[ebp%+d]",bkey); CALLRT(f,cmpfn); E(f,"mov ecx,eax"); E(f,"pop eax"); E(f,"cmp ecx,0"); }
        else E(f,"cmp eax,[ebp%+d]",bkey);
        E(f,"j%s L%d",is_max?"g":"l",ltake); E(f,"jmp L%d",lnext);
    }
    LBL(f,ltake);
    if(is_ptr(kt)){ incref(f); E(f,"xchg eax,[ebp%+d]",bkey); CALLRT(f,"rt_decref"); }
    else if(is_flt(kt)) E(f,"fstp qword [ebp%+d]",bkey); else E(f,"mov [ebp%+d],eax",bkey);
    E(f,"mov eax,[ebp%+d]",idx); E(f,"mov [ebp%+d],eax",best);
    LBL(f,lnext);
    scope_close(f,mark);
    E(f,"inc dword [ebp%+d]",idx); E(f,"jmp L%d",ltop);
    LBL(f,lend);
    E(f,"mov eax,[ebp%+d]",src); E(f,"mov ecx,[ebp%+d]",best); E(f,"mov eax,[eax+16]");
    if(is_flt(el)) E(f,"fld qword [eax+ecx*8]"); else { E(f,"mov eax,[eax+ecx*4]"); if(is_ptr(el)) incref(f); }
    if(!is_flt(el)) E(f,"push eax");                                          /* the sequence is released */
    E(f,"mov eax,[ebp%+d]",src); E(f,"mov dword [ebp%+d],0",src); CALLRT(f,"rt_decref");
    if(!is_flt(el)) E(f,"pop eax");
    return is_ptr(el);
}
static int sort_kind(Ty *t){ t=ty_find(t); if(t->k==TY_TUPLE){ rt("rt_tuple_cmp"); return 4; } return t->k==TY_STR?1:t->k==TY_FLOAT?2:0; }

/* any/all of a generator expression: its loops, stopping at the first deciding item */
static int gen_comp_anyall(F *f, Expr *e, int isall){
    int res=frame_slot(f,4), lfound=new_label(), ldone=new_label();
    E(f,"mov dword [ebp%+d],%d",res,isall);
    Iter its[8]; int n=e->nclause;
    if(n>8) cg_fail(e->line,"too many for clauses");
    for(int i=0;i<n;i++){
        CompClause *cl=&e->clauses[i];
        iter_begin(f,cl->iter,&its[i]);
        iter_store_vars(f,&its[i],&xinfo(e)->cvars[2*i],cl->nvars);
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
    E(f,"mov eax,[eax+16]"); if(is_flt(el)) E(f,"fld qword [eax+ecx*8]"); else E(f,"mov eax,[eax+ecx*4]");
    store_borrowed(f,x,el);
    { int mark=scope_open(f); gen_as(f,xi->key,at,1); store_var(f,acc); scope_close(f,mark); }
    E(f,"inc dword [ebp%+d]",idx); E(f,"jmp L%d",ltop);
    LBL(f,lend);
    E(f,"mov eax,[ebp%+d]",src); E(f,"mov dword [ebp%+d],0",src); CALLRT(f,"rt_decref");
    load_var(f,acc); if(is_ptr(at)) incref(f);
    return is_ptr(at);
}
static int gen_builtin(F *f, Expr *e, XInfo *xi){
    const char *n=xi->name; Ty *t=TY(e);
    Expr *a0=e->count?e->items[0]:NULL; Ty *t0=a0?TY(a0):NULL;
    if(!strcmp(n,"reduce")) return gen_reduce(f,e,xi);
    if(!strcmp(n,"next")){
        Ty *el=t; int slot=frame_slot(f,4), lmiss=new_label(), lend=new_label();
        gen_borrow(f,a0); panic_if_null(f); E(f,"mov [ebp%+d],eax",slot);
        CALLRT(f,"rt_gen_next"); E(f,"test eax,eax"); E(f,"jz L%d",lmiss);
        E(f,"mov eax,[ebp%+d]",slot); load_mem(f,el,"eax",24); if(is_ptr(el)) incref(f);
        E(f,"jmp L%d",lend);
        LBL(f,lmiss);
        if(e->count==2){ gen_as(f,e->items[1],el,1); }
        else CALLRT(f,"rt_panic_stop");
        LBL(f,lend);
        return is_ptr(el);
    }
    if((!strcmp(n,"any")||!strcmp(n,"all")) && a0->kind==EXPR_COMPREHENSION && a0->comp_kind=='g') return gen_comp_anyall(f,a0,n[1]=='l');
    if(!strcmp(n,"len") && t0->k==TY_TUPLE){ int o=gen(f,a0); drop_value(f,t0,o); E(f,"mov eax,%d",t0->nelems); return 0; }
    if(!strcmp(n,"divmod")){
        Ty *qt=t->elems[0];
        E(f,"mov eax,TD%d",tdesc(t)); E(f,"mov edx,%d",tuple_size(t)); CALLRT(f,"rt_tuple_new"); E(f,"push eax");
        gen_as(f,a0,qt,0); push_value(f,qt); gen_as(f,e->items[1],qt,0); push_value(f,qt);   /* [esp] b, then a, then the tuple */
        int sz=esize(qt);
        if(is_flt(qt)){
            E(f,"fld qword [esp]"); E(f,"fld qword [esp+8]"); CALLRT(f,"rt_ffloordiv"); E(f,"mov ecx,[esp+16]"); E(f,"fstp qword [ecx+12]");
            E(f,"fld qword [esp+8]"); E(f,"fld qword [esp]"); E(f,"fxch"); CALLRT(f,"rt_fmod"); E(f,"mov ecx,[esp+16]"); E(f,"fstp qword [ecx+20]");
        } else {
            E(f,"mov eax,[esp+4]"); E(f,"mov edx,[esp]"); CALLRT(f,"rt_floordiv"); E(f,"mov ecx,[esp+8]"); E(f,"mov [ecx+12],eax");
            E(f,"mov eax,[esp+4]"); E(f,"mov edx,[esp]"); CALLRT(f,"rt_mod"); E(f,"mov ecx,[esp+8]"); E(f,"mov [ecx+16],eax");
        }
        E(f,"add esp,%d",2*sz); E(f,"pop eax");
        return 1;
    }
    if(!strcmp(n,"len")){
        if(xi->fn){ E(f,"sub esp,4"); gen_borrow(f,a0); E(f,"mov [esp],eax"); call_method_on_top(f,xi->fn); E(f,"add esp,4"); return 0; }
        int l=new_label(); gen_borrow(f,a0); E(f,"test eax,eax"); E(f,"jz L%d",l); E(f,"mov eax,[eax+8]"); LBL(f,l); return 0;
    }
    if(!strcmp(n,"str")||!strcmp(n,"repr")){
        if(!a0){ E(f,"xor eax,eax"); return 0; }
        if(t0->k==TY_STR && n[0]=='s') return gen(f,a0);
        rt("rt_sb_need"); E(f,"push dword [rt_sb_len]");
        gen_borrow(f,a0); gen_fmt(f,t0,n[0]=='r');
        E(f,"pop eax"); CALLRT(f,"rt_sb_take"); return 1;
    }
    if(!strcmp(n,"int")){
        if(!a0){ E(f,"xor eax,eax"); return 0; }
        if(t0->k==TY_FLOAT){ gen(f,a0); CALLRT(f,"rt_ftoi"); return 0; }
        if(t0->k==TY_STR){ gen_borrow(f,a0); CALLRT(f,"rt_int_parse"); return 0; }
        gen(f,a0); return 0;
    }
    if(!strcmp(n,"float")){
        if(!a0){ E(f,"fldz"); return 0; }
        if(t0->k==TY_STR){ gen_borrow(f,a0); CALLRT(f,"rt_float_parse"); return 0; }
        gen_as(f,a0,TY_FLOAT_T,0); return 0;
    }
    if(!strcmp(n,"bool")){ if(!a0) E(f,"xor eax,eax"); else gen_bool(f,a0); return 0; }
    if(!strcmp(n,"abs")){ gen(f,a0); if(t0->k==TY_FLOAT) E(f,"fabs"); else { E(f,"cdq"); E(f,"xor eax,edx"); E(f,"sub eax,edx"); } return 0; }
    if((!strcmp(n,"min")||!strcmp(n,"max")) && xi->key) return gen_minmax_key(f,e,n[1]=='a');
    if(!strcmp(n,"min")||!strcmp(n,"max")){
        int mx=n[1]=='a';
        if(e->count==1 && t0->k==TY_DICT){ gen_borrow(f,a0); CALLRT(f,"rt_dict_keys"); hold(f); E(f,"mov edx,%d",sort_kind(ty_dkey(t0))|(mx?0x100:0)); CALLRT(f,"rt_list_minmax"); return 0; }
        if(e->count==1){ gen_borrow(f,a0); E(f,"mov edx,%d",sort_kind(t0->elem)|(mx?0x100:0)); CALLRT(f,"rt_list_minmax"); return 0; }
        int slot=frame_slot(f,8);
        gen_as(f,a0,t,0); if(is_flt(t)) E(f,"fstp qword [ebp%+d]",slot); else E(f,"mov [ebp%+d],eax",slot);
        for(int i=1;i<e->count;i++){
            int skip=new_label();
            gen_as(f,e->items[i],t,0);
            if(is_flt(t)){
                int take=new_label();
                E(f,"fld qword [ebp%+d]",slot); E(f,"fcomp st1"); E(f,"fnstsw ax"); E(f,"sahf");   /* best vs x */
                E(f,"j%s L%d",mx?"b":"a",take); E(f,"fstp st0"); E(f,"jmp L%d",skip);
                LBL(f,take); E(f,"fstp qword [ebp%+d]",slot);
            } else if(ty_find(t)->k==TY_STR){
                E(f,"push eax"); E(f,"mov edx,[ebp%+d]",slot); CALLRT(f,"rt_str_cmp"); E(f,"pop ecx");
                E(f,"cmp eax,0"); E(f,"j%s L%d",mx?"le":"ge",skip); E(f,"mov [ebp%+d],ecx",slot);
            } else { E(f,"cmp eax,[ebp%+d]",slot); E(f,"j%s L%d",mx?"le":"ge",skip); E(f,"mov [ebp%+d],eax",slot); }
            LBL(f,skip);
        }
        if(is_flt(t)) E(f,"fld qword [ebp%+d]",slot); else E(f,"mov eax,[ebp%+d]",slot);
        return 0;
    }
    if(!strcmp(n,"ord")){ gen_borrow(f,a0); CALLRT(f,"rt_ord"); return 0; }
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
        gen_borrow(f,a0);
        if(t0->k==TY_STR) CALLRT(f,"rt_str_chars");
        else if(t0->k==TY_TUPLE) cg_fail(e->line,"reversed() of a tuple: use a list");
        else { E(f,"mov edx,%d",kind_of(t0->elem)); CALLRT(f,"rt_list_copy"); }
        E(f,"push eax"); E(f,"mov edx,%d",esize(t->elem)); CALLRT(f,"rt_list_reverse"); E(f,"pop eax");
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
        if(e->count==3){ gen(f,a0); E(f,"push eax"); gen(f,e->items[1]); E(f,"push eax"); gen(f,e->items[2]); E(f,"mov ecx,eax"); E(f,"pop edx"); E(f,"pop eax"); CALLRT(f,"rt_ipowmod"); return 0; }
        int num=1; (void)num;
        gen_as(f,a0,t,0); push_value(f,t); gen_as(f,e->items[1],t,0);
        return apply_binop(f,T_POWER,t,t,TY(e->items[1]),e->line);
    }
    if(!strcmp(n,"chr")){ gen(f,a0); CALLRT(f,"rt_chr"); return 0; }
    if(!strcmp(n,"sum")){ gen_borrow(f,a0); E(f,"mov edx,%d",is_flt(t0->elem)?2:0); CALLRT(f,"rt_list_sum"); return 0; }
    if(!strcmp(n,"sorted") && (xi->key||xi->rev)){
        int rev=gen_rev_flag(f,xi->rev), src=ref_slot(f);
        gen_borrow(f,a0);
        if(t0->k==TY_DICT) CALLRT(f,"rt_dict_keys"); else if(t0->k==TY_STR) CALLRT(f,"rt_str_chars"); else if(xi->key) incref(f);
        else { E(f,"mov edx,%d",kind_of(t0->elem)); CALLRT(f,"rt_list_copy"); }
        E(f,"mov [ebp%+d],eax",src);
        if(xi->key) return gen_sort_by_key(f,src,t->elem,xi->key,rev,0);
        E(f,"mov edx,%d",sort_kind(t->elem)); CALLRT(f,"rt_list_sort");
        { int l=new_label(); E(f,"cmp dword [ebp%+d],0",rev); E(f,"je L%d",l); E(f,"mov eax,[ebp%+d]",src); E(f,"mov edx,%d",esize(t->elem)); CALLRT(f,"rt_list_reverse"); LBL(f,l); }
        E(f,"mov eax,[ebp%+d]",src); E(f,"mov dword [ebp%+d],0",src); return 1;
    }
    if(!strcmp(n,"sorted")){
        gen_borrow(f,a0);
        if(t0->k==TY_DICT) CALLRT(f,"rt_dict_keys");
        else if(t0->k==TY_STR) CALLRT(f,"rt_str_chars");
        else { E(f,"mov edx,%d",kind_of(t0->elem)); CALLRT(f,"rt_list_copy"); }
        E(f,"push eax"); E(f,"mov edx,%d",sort_kind(t->elem)); CALLRT(f,"rt_list_sort"); E(f,"pop eax");
        return 1;
    }
    if(!strcmp(n,"isinstance")){
        Expr *k=e->items[1]; int nk=k->kind==EXPR_TUPLE?k->count:1;
        gen_borrow(f,a0); E(f,"push eax"); E(f,"push 0");
        for(int i=0;i<nk;i++){ AClass *c=xinfo(k->kind==EXPR_TUPLE?k->items[i]:k)->cls; use_class(c);
            E(f,"mov eax,[esp+4]"); E(f,"mov edx,VT%d",c->id); CALLRT(f,"rt_isinstance"); E(f,"or [esp],eax"); }
        E(f,"pop eax"); E(f,"add esp,4"); return 0;
    }
    if(!strcmp(n,"input")){ if(a0) gen_borrow(f,a0); else E(f,"xor eax,eax"); CALLRT(f,"rt_input"); return 1; }
    if(!strcmp(n,"round")){
        if(e->count==1){ gen_as(f,a0,TY_FLOAT_T,0); CALLRT(f,"rt_fround"); return 0; }
        gen_as(f,a0,TY_FLOAT_T,0); E(f,"sub esp,8"); E(f,"fstp qword [esp]");
        gen(f,e->items[1]); E(f,"fld qword [esp]"); E(f,"add esp,8"); CALLRT(f,"rt_fround_n"); return 0;
    }
    if(!strcmp(n,"list")){
        if(!a0){ E(f,"mov eax,%s",list_destroy(t->elem)); CALLRT(f,"rt_list_new"); return 1; }
        XInfo *ai=xinfo(a0);
        if(ai->kind==X_BUILTIN && ai->name && !strcmp(ai->name,"range")){
            E(f,"sub esp,12"); E(f,"mov dword [esp],0"); E(f,"mov dword [esp+8],1");
            if(a0->count==1){ gen(f,a0->items[0]); E(f,"mov [esp+4],eax"); }
            else { gen(f,a0->items[0]); E(f,"mov [esp],eax"); gen(f,a0->items[1]); E(f,"mov [esp+4],eax");
                if(a0->count==3){ gen(f,a0->items[2]); E(f,"mov [esp+8],eax"); } }
            E(f,"pop eax"); E(f,"pop edx"); E(f,"pop ecx"); CALLRT(f,"rt_range_list"); return 1;
        }
        gen_borrow(f,a0);
        if(t0->k==TY_STR) CALLRT(f,"rt_str_chars");
        else if(t0->k==TY_DICT) CALLRT(f,"rt_dict_keys");
        else if(t0->k==TY_GEN){ E(f,"mov edx,%s",list_destroy(t->elem)); CALLRT(f,"rt_gen_drain"); }
        else { E(f,"mov edx,%d",kind_of(t0->elem)); CALLRT(f,"rt_list_copy"); E(f,"mov dword [eax+4],%s",list_destroy(t->elem)); }
        return 1;
    }
    if(!strcmp(n,"set")){
        if(!a0){ E(f,"mov eax,%s",list_destroy(t->elem)); CALLRT(f,"rt_list_new"); return 1; }
        gen_borrow(f,a0);
        if(t0->k==TY_STR||t0->k==TY_DICT){ CALLRT(f,t0->k==TY_STR?"rt_str_chars":"rt_dict_keys"); hold(f); }
        E(f,"mov edx,%d",kind_of(t->elem)); CALLRT(f,"rt_set_from"); return 1;
    }
    if(!strcmp(n,"range_list")){                         /* range(...) as a value */
        E(f,"sub esp,12"); E(f,"mov dword [esp],0"); E(f,"mov dword [esp+8],1");
        if(e->count==1){ gen(f,a0); E(f,"mov [esp+4],eax"); }
        else { gen(f,a0); E(f,"mov [esp],eax"); gen(f,e->items[1]); E(f,"mov [esp+4],eax");
            if(e->count==3){ gen(f,e->items[2]); E(f,"mov [esp+8],eax"); } }
        E(f,"pop eax"); E(f,"pop edx"); E(f,"pop ecx"); CALLRT(f,"rt_range_list"); return 1; }
    if(!strcmp(n,"dict_copy")){ gen_borrow(f,a0); E(f,"mov edx,%d",kind_of(t->elem)); CALLRT(f,"rt_dict_copy"); return 1; }
    if(!strcmp(n,"dict")){ new_dict(f,t); return 1; }
    cg_fail(e->line,"unsupported builtin");
}

static int gen_tmethod(F *f, Expr *e, XInfo *xi){
    const char *m=xi->name; Expr *obj=e->a->a; Ty *t0=TY(obj);
    Expr *a0=e->count?e->items[0]:NULL, *a1=e->count>1?e->items[1]:NULL;
    if(t0->k==TY_STR){
        if(!strcmp(m,"format")) return gen_str_format(f,e);
        if(!strcmp(m,"ljust")||!strcmp(m,"rjust")||!strcmp(m,"center")||!strcmp(m,"zfill")){
            rt("rt_sb_need"); E(f,"push dword [rt_sb_len]");
            gen_borrow(f,obj); CALLRT(f,"rt_sb_str");
            if(a1){ gen_borrow(f,a1); E(f,"movzx eax,byte [eax+12]"); E(f,"shl eax,24"); E(f,"push eax"); } else E(f,"push 0");
            gen(f,a0); E(f,"and eax,0xFFFF"); E(f,"or [esp],eax");
            E(f,"pop edx"); E(f,"or edx,0x%x",m[0]=='l'?0x10000:m[0]=='c'?0x40000:m[0]=='z'?0x20000:0);
            E(f,"mov eax,[esp]"); CALLRT(f,"rt_sb_pad");
            E(f,"pop eax"); CALLRT(f,"rt_sb_take"); return 1;
        }
        if(!strcmp(m,"strip")||!strcmp(m,"lstrip")||!strcmp(m,"rstrip")){
            gen_borrow(f,obj);
            if(a0){ E(f,"push eax"); gen_borrow(f,a0); E(f,"mov ecx,eax"); E(f,"pop eax"); } else E(f,"xor ecx,ecx");
            E(f,"mov edx,%d",m[0]=='s'?3:m[0]=='l'?1:2); CALLRT(f,"rt_str_strip"); return 1;
        }
        gen_borrow(f,obj);
        if(!strcmp(m,"upper")||!strcmp(m,"lower")||!strcmp(m,"capitalize")||!strcmp(m,"title")||!strcmp(m,"swapcase")){
            E(f,"mov edx,%d",m[0]=='u'?0:m[0]=='l'?1:m[0]=='c'?2:m[0]=='t'?3:4); CALLRT(f,"rt_str_case"); return 1; }
        if(!strcmp(m,"splitlines")){ CALLRT(f,"rt_str_splitlines"); return 1; }
        if(!strncmp(m,"is",2)){ static const char *cls[]={"isdigit","isalpha","isspace","isupper","islower","isalnum"};
            int k=0; for(int i=0;i<6;i++) if(!strcmp(m,cls[i])) k=i; E(f,"mov edx,%d",k); CALLRT(f,"rt_str_is"); return 0; }
        E(f,"push eax");
        if(!strcmp(m,"replace")){ gen_borrow(f,a0); E(f,"push eax"); gen_borrow(f,a1); E(f,"mov ecx,eax"); E(f,"pop edx"); E(f,"pop eax"); CALLRT(f,"rt_str_replace"); return 1; }
        if(a0) gen_borrow(f,a0); else E(f,"xor eax,eax");
        if(a0 && !strcmp(m,"join") && TY(a0)->k==TY_STR){ CALLRT(f,"rt_str_chars"); hold(f); }    /* sep.join("abc") */
        E(f,"mov edx,eax"); E(f,"pop eax");
        if(!strcmp(m,"startswith")||!strcmp(m,"endswith")){ E(f,"mov ecx,%d",m[0]=='e'); CALLRT(f,"rt_str_affix"); return 0; }
        if(!strcmp(m,"find")){ CALLRT(f,"rt_str_find"); return 0; }
        if(!strcmp(m,"rfind")){ CALLRT(f,"rt_str_rfind"); return 0; }
        if(!strcmp(m,"rindex")){ CALLRT(f,"rt_str_rfind"); int l=new_label(); E(f,"cmp eax,-1"); E(f,"jne L%d",l); E(f,"mov esi,Z%d",zlit("substring not found")); CALLRT(f,"rt_panic_value"); LBL(f,l); return 0; }
        if(!strcmp(m,"index")){ CALLRT(f,"rt_str_index"); return 0; }
        if(!strcmp(m,"count")){ CALLRT(f,"rt_str_count"); return 0; }
        if(!strcmp(m,"split")){ CALLRT(f,"rt_str_split"); return 1; }
        if(!strcmp(m,"join")){ CALLRT(f,"rt_str_join"); return 1; }
    }
    if(t0->k==TY_LIST){
        Ty *el=t0->elem; int k=kind_of(el), es=esize(el);
        gen_borrow(f,obj);
        if(!strcmp(m,"reverse")){ E(f,"mov edx,%d",es); CALLRT(f,"rt_list_reverse"); return 0; }
        if(!strcmp(m,"sort") && (xi->key||xi->rev)){
            int src=ref_slot(f); incref(f); E(f,"mov [ebp%+d],eax",src);
            int rev=gen_rev_flag(f,xi->rev);
            if(xi->key){ gen_sort_by_key(f,src,el,xi->key,rev,1); E(f,"mov eax,[ebp%+d]",src); E(f,"mov dword [ebp%+d],0",src); CALLRT(f,"rt_decref"); return 0; }
            E(f,"mov eax,[ebp%+d]",src); E(f,"mov edx,%d",sort_kind(el)); CALLRT(f,"rt_list_sort");
            { int l=new_label(); E(f,"cmp dword [ebp%+d],0",rev); E(f,"je L%d",l); E(f,"mov eax,[ebp%+d]",src); E(f,"mov edx,%d",es); CALLRT(f,"rt_list_reverse"); LBL(f,l); }
            E(f,"mov eax,[ebp%+d]",src); E(f,"mov dword [ebp%+d],0",src); CALLRT(f,"rt_decref"); return 0;
        }
        if(!strcmp(m,"sort")){ E(f,"mov edx,%d",sort_kind(el)); CALLRT(f,"rt_list_sort"); return 0; }
        if(!strcmp(m,"clear")){ E(f,"mov edx,%d",k); CALLRT(f,"rt_list_clear"); return 0; }
        if(!strcmp(m,"copy")){ E(f,"mov edx,%d",k); CALLRT(f,"rt_list_copy"); return 1; }
        E(f,"push eax");
        if(!strcmp(m,"append")){ gen_as(f,a0,el,1); list_append_top(f,el); E(f,"add esp,4"); return 0; }
        if(!strcmp(m,"pop")){ if(a0) gen(f,a0); else E(f,"or eax,-1");
            E(f,"mov edx,eax"); E(f,"pop eax"); E(f,"mov ecx,%d",es); CALLRT(f,"rt_list_pop"); load_mem(f,el,"eax",0); return is_ptr(el); }
        if(!strcmp(m,"insert")){ gen(f,a0); E(f,"push eax"); gen_as(f,a1,el,1); push_value(f,el);
            E(f,"mov eax,[esp+%d]",es+4); E(f,"mov edx,[esp+%d]",es); E(f,"mov ecx,%d",es); CALLRT(f,"rt_list_insert");
            if(is_flt(el)){ E(f,"fld qword [esp]"); E(f,"add esp,8"); E(f,"fstp qword [eax]"); } else { E(f,"pop edx"); E(f,"mov [eax],edx"); }
            E(f,"add esp,8"); return 0; }
        if(!strcmp(m,"extend")){ gen_borrow(f,a0); E(f,"mov edx,eax"); E(f,"pop eax"); E(f,"mov ecx,%d",k); CALLRT(f,"rt_list_extend"); return 0; }
        gen_as(f,a0,el,0);                                       /* remove / index / count */
        if(is_flt(el)){ E(f,"sub esp,8"); E(f,"fstp qword [esp]"); E(f,"mov edx,esp"); E(f,"mov eax,[esp+8]"); }
        else { E(f,"mov edx,eax"); E(f,"mov eax,[esp]"); }
        E(f,"mov ecx,%d",k);
        CALLRT(f,!strcmp(m,"remove")?"rt_list_remove":!strcmp(m,"index")?"rt_list_index":"rt_list_count");
        E(f,"add esp,%d",is_flt(el)?12:4);
        return 0;
    }
    if(t0->k==TY_DICT){
        Ty *v=t0->elem; int k=kind_of(v), vs=esize(v);
        gen_borrow(f,obj);
        if(!strcmp(m,"keys")){ CALLRT(f,"rt_dict_keys"); return 1; }
        if(!strcmp(m,"items")){ E(f,"mov edx,TD%d",tdesc(ty_find(TY(e))->elem)); CALLRT(f,"rt_dict_items"); return 1; }
        if(!strcmp(m,"values")){ E(f,"mov edx,%d",k); CALLRT(f,"rt_dict_values"); return 1; }
        if(!strcmp(m,"clear")){ E(f,"mov edx,%d",k); CALLRT(f,"rt_dict_clear"); return 0; }
        if(!strcmp(m,"copy")){ E(f,"mov edx,%d",k); CALLRT(f,"rt_dict_copy"); return 1; }
        E(f,"push eax");
        if(!strcmp(m,"update")){ gen_borrow(f,a0); E(f,"mov edx,eax"); E(f,"pop eax"); E(f,"mov ecx,%d",k); CALLRT(f,"rt_dict_update"); return 0; }
        gen_borrow(f,a0); E(f,"push eax");                     /* [esp] key, [esp+4] dict */
        E(f,"mov edx,eax"); E(f,"mov eax,[esp+4]"); CALLRT(f,"rt_dict_find");
        int lmiss=new_label(), lend=new_label(), owned=0;
        E(f,"cmp eax,-1"); E(f,"je L%d",lmiss);
        if(!strcmp(m,"get")||!strcmp(m,"setdefault")){
            E(f,"mov ecx,[esp+4]"); E(f,"mov ecx,[ecx+20]");
            if(is_flt(v)) E(f,"fld qword [ecx+eax*8]"); else E(f,"mov eax,[ecx+eax*4]");
            E(f,"jmp L%d",lend); LBL(f,lmiss);
            if(m[0]=='g'){ if(a1) gen_as(f,a1,v,0); else zero_value(f,v); }
            else { gen_as(f,a1,v,1); push_value(f,v);
                E(f,"mov eax,[esp+%d]",vs+4); E(f,"mov edx,[esp+%d]",vs); E(f,"mov ecx,%d",vs); CALLRT(f,"rt_dict_slot");
                E(f,"mov ecx,eax"); pop_value(f,v); if(is_ptr(v)){ E(f,"mov [ecx],eax"); } else store_new(f,v,"ecx",0);
                E(f,"mov eax,[esp+4]"); E(f,"mov edx,[esp]"); E(f,"mov ecx,%d",vs); CALLRT(f,"rt_dict_get"); load_mem(f,v,"eax",0); }
        } else {                                                    /* pop */
            E(f,"mov eax,[esp+4]"); E(f,"mov edx,[esp]"); E(f,"mov ecx,%d",vs); CALLRT(f,"rt_dict_del"); load_mem(f,v,"eax",0);
            E(f,"jmp L%d",lend); LBL(f,lmiss);
            if(a1) gen_as(f,a1,v,1); else CALLRT(f,"rt_panic_key");
            owned=1;
        }
        LBL(f,lend); E(f,"add esp,8");
        return owned && is_ptr(v);
    }
    if(t0->k==TY_FILE){
        gen_borrow(f,obj);
        if(!strcmp(m,"read")){ CALLRT(f,"rt_file_read"); return 1; }
        if(!strcmp(m,"readline")){ CALLRT(f,"rt_file_readline"); return 1; }
        if(!strcmp(m,"readlines")){ CALLRT(f,"rt_file_readlines"); return 1; }
        if(!strcmp(m,"close")){ CALLRT(f,"rt_file_close"); return 0; }
        E(f,"push eax"); gen_borrow(f,a0); E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_file_write"); return 0;
    }
    if(t0->k==TY_TASK){
        gen_borrow(f,obj);
        if(!strcmp(m,"done")){ E(f,"cmp dword [eax+8],4"); E(f,"sete al"); E(f,"movzx eax,al"); return 0; }
        CALLRT(f,"rt_task_result"); if(ty_find(t0->elem)->k!=TY_VOID) load_mem(f,t0->elem,"eax",24); return 0;
    }
    if(t0->k==TY_SET){
        Ty *el=t0->elem; int k=kind_of(el);
        gen_borrow(f,obj);
        if(!strcmp(m,"clear")){ E(f,"mov edx,%d",k); CALLRT(f,"rt_list_clear"); return 0; }
        if(!strcmp(m,"copy")){ E(f,"mov edx,%d",k); CALLRT(f,"rt_list_copy"); return 1; }
        E(f,"push eax");
        if(!strcmp(m,"union")||!strcmp(m,"intersection")||!strcmp(m,"difference")){
            gen_borrow(f,a0); E(f,"mov edx,eax"); E(f,"pop eax"); E(f,"mov ecx,%d",k|((m[0]=='u'?0:m[0]=='i'?1:2)<<8)); CALLRT(f,"rt_set_op"); return 1; }
        gen_as(f,a0,el,0);
        if(!strcmp(m,"add")){ set_add_top(f,el); E(f,"add esp,4"); return 0; }
        if(is_flt(el)){ E(f,"sub esp,8"); E(f,"fstp qword [esp]"); E(f,"mov edx,esp"); E(f,"mov eax,[esp+8]"); }
        else { E(f,"mov edx,eax"); E(f,"mov eax,[esp]"); }
        E(f,"mov ecx,%d",k|(m[0]=='d'?0x100:0)); CALLRT(f,"rt_set_remove");
        E(f,"add esp,%d",is_flt(el)?12:4); return 0;
    }
    cg_fail(e->line,"unsupported method");
}

static int gen_sys(F *f, Expr *e, XInfo *xi){
    const char *m=xi->name;
    if(!strcmp(m,"buffer")){ Ty *t0=TY(e->items[0]); gen_borrow(f,e->items[0]); CALLRT(f,t0->k==TY_STR?"rt_buf_from_str":"rt_buf_new"); return 1; }
    if(!strcmp(m,"addr")){ gen_borrow(f,e->items[0]); E(f,"add eax,12"); return 0; }
    if(!strcmp(m,"exit")){ if(e->count) gen(f,e->items[0]); else E(f,"xor eax,eax"); E(f,"mov ebx,eax"); CALLRT(f,"rt_exit"); return 0; }
    if(!strcmp(m,"peek_at")||!strcmp(m,"peek_str_at")||!strcmp(m,"cstr_at")||!strcmp(m,"poke_str_at")){   /* raw memory: (address, n | str) */
        gen(f,e->items[0]); E(f,"push eax");
        if(m[1]=='o') gen_borrow(f,e->items[1]); else gen(f,e->items[1]);
        E(f,"mov edx,eax"); E(f,"pop eax");
        if(!strcmp(m,"peek_at")){ CALLRT(f,"rt_mem_peek"); return 0; }
        if(!strcmp(m,"poke_str_at")){ CALLRT(f,"rt_mem_poke_str"); return 0; }
        CALLRT(f,m[0]=='c'?"rt_mem_cstr":"rt_mem_peek_str"); return 1;
    }
    if(!strcmp(m,"poke_at")){ gen(f,e->items[0]); E(f,"push eax"); gen(f,e->items[1]); E(f,"push eax"); gen(f,e->items[2]);
        E(f,"mov ecx,eax"); E(f,"pop edx"); E(f,"pop eax"); CALLRT(f,"rt_mem_poke"); return 0; }
    int n=e->count;
    E(f,"sub esp,%d",4*n);
    for(int i=0;i<n;i++){ gen_borrow(f,e->items[i]); E(f,"mov [esp+%d],eax",4*i); }
    if(!strcmp(m,"poke")){ CALLRT(f,"rt_buf_poke"); E(f,"add esp,16"); return 0; }
    if(!strcmp(m,"peek")){ CALLRT(f,"rt_buf_peek"); E(f,"add esp,12"); return 0; }
    E(f,"pop eax"); E(f,"pop edx"); E(f,"pop ecx");
    if(!strcmp(m,"poke_str")){ CALLRT(f,"rt_buf_poke_str"); return 0; }
    CALLRT(f,"rt_buf_peek_str"); return 1;
}

static int gen_asyncio(F *f, Expr *e, XInfo *xi);
static int gen_bmod(F *f, Expr *e, XInfo *xi);
static int gen_call(F *f, Expr *e){
    XInfo *xi=xinfo(e); Ty *t=TY(e);
    switch(xi->kind){
        case X_FUNC: case X_STATIC: case X_CALLNEST: call_user(f,xi->fn,e,xi,0); return is_ptr(t);
        case X_CALLVAL: return call_value(f,e);
        case X_CALLDECO: return call_value_of(f,e,xi->var);
        case X_METHOD: call_user(f,xi->fn,e,xi,1); return is_ptr(t);
        case X_SUPER:
            if(!xi->fn){                                         /* super().__init__(message) of an exception */
                if(e->count) gen_exc_message(f,e->items[0]); else E(f,"xor eax,eax");
                E(f,"mov ecx,[ebp+8]"); store_mem(f,TY_STR_T,"ecx",12);
                return 0;
            }
            call_user(f,xi->fn,e,xi,xi->fn->is_static?0:2); return is_ptr(t);
        case X_CTOR: return gen_ctor(f,e,xi);
        case X_BUILTIN: return gen_builtin(f,e,xi);
        case X_TMETHOD: return gen_tmethod(f,e,xi);
        case X_SYSCALL: gen_syscall_regs(f,e); E(f,"mov eax,esp"); CALLRT(f,"rt_syscall_list"); E(f,"add esp,28"); return 1;
        case X_SYS: return gen_sys(f,e,xi);
        case X_ASYNC: return gen_asyncio(f,e,xi);
        case X_BMOD: return gen_bmod(f,e,xi);
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
    if(n>=2 && s[1] && strchr("<>^=",s[1])){ sp->fill=(unsigned char)s[0]; sp->align=s[1]; i=2; }
    else if(n>=1 && s[0] && strchr("<>^=",s[0])){ sp->align=s[0]; i=1; }
    for(;i<n;i++){ char c=s[i];
        if(c=='-') sp->align='<'; else if(c=='+'||c==' ') sp->sign=c; else if(c=='#') sp->alt=1; else if(c=='0') sp->zero=1; else break; }
    for(;i<n && s[i]>='0' && s[i]<='9';i++) sp->width=sp->width*10+(s[i]-'0');
    if(i<n && (s[i]==','||s[i]=='_')){ sp->comma=1; i++; }
    if(i<n && s[i]=='.'){ i++; sp->prec=0; for(;i<n && s[i]>='0' && s[i]<='9';i++) sp->prec=sp->prec*10+(s[i]-'0'); }
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
    int padded=sp->width>0;
    if(padded) E(f,"push dword [rt_sb_len]");
    if(sp->sign && num && ty!='s') E(f,"push dword [rt_sb_len]");
    int prec=sp->prec;
    switch(ty){
        case 's': case 'r': src_load(f,src,0); gen_fmt(f,t,repr||ty=='r'); break;
        case 'd': case 'i': case 'u':
            src_load(f,src,0); if(is_flt(t)) CALLRT(f,"rt_ftoi"); CALLRT(f,sp->comma?"rt_sb_intc":"rt_sb_int"); break;
        case 'x': case 'X': case 'o': case 'b':{
            src_load(f,src,0); if(is_flt(t)) CALLRT(f,"rt_ftoi");
            if(sp->alt){ int l=new_label(); E(f,"test eax,eax"); E(f,"jns L%d",l); E(f,"push eax"); E(f,"mov al,'-'"); CALLRT(f,"rt_sb_char"); E(f,"pop eax"); E(f,"neg eax"); LBL(f,l);
                E(f,"push eax"); E(f,"mov al,'0'"); CALLRT(f,"rt_sb_char"); E(f,"mov al,'%c'",ty=='o'?'o':ty=='b'?'b':ty); CALLRT(f,"rt_sb_char"); E(f,"pop eax"); }
            E(f,"mov edx,%d",ty=='o'?8:ty=='b'?2:16); E(f,"mov ecx,%d",ty=='X'); CALLRT(f,"rt_sb_radix"); break; }
        case 'f': case 'F': case '%':
            src_load(f,src,1);
            if(ty=='%'){ E(f,"push 100"); E(f,"fimul dword [esp]"); E(f,"add esp,4"); }
            E(f,"mov edx,%d",prec<0?6:prec>40?40:prec); CALLRT(f,"rt_sb_fixed");
            if(ty=='%'){ E(f,"mov al,'%%'"); CALLRT(f,"rt_sb_char"); }
            break;
        case 'e': case 'E':
            src_load(f,src,1); E(f,"mov edx,%d",(prec<0?6:prec>16?16:prec)+1); E(f,"mov ecx,4"); CALLRT(f,"rt_sb_gen"); break;
        case 'g': case 'G': case 1:
            src_load(f,src,1); E(f,"mov edx,%d",prec<0?6:prec==0?1:prec>17?17:prec); E(f,"mov ecx,%d",ty==1?1:0); CALLRT(f,"rt_sb_gen"); break;
        case 2: src_load(f,src,1); CALLRT(f,"rt_sb_float"); break;
        case 'c': src_load(f,src,0); CALLRT(f,"rt_sb_char"); break;
        default: cg_fail(line,"unsupported format type");
    }
    if(sp->sign && num && ty!='s'){ E(f,"pop eax"); E(f,"mov dl,'%c'",sp->sign); CALLRT(f,"rt_sb_sign"); }
    if(padded){
        unsigned flags=(unsigned)sp->width;
        if(align=='<') flags|=0x10000; else if(align=='^') flags|=0x40000;
        if(sp->zero && !sp->align && num) flags|=0x20000;
        if(sp->fill) flags|=(unsigned)sp->fill<<24;
        E(f,"pop eax"); E(f,"mov edx,0x%x",flags); CALLRT(f,"rt_sb_pad");
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
static int gen_closure(F *f, AFunc *g){
    layout_caps(g); use_fn(g); gg->cd_used[g->id]=1;
    E(f,"mov eax,%d",16+g->capsize); CALLRT(f,"rt_alloc");
    E(f,"mov dword [eax],1"); E(f,"mov dword [eax+4],CD%d",g->id); E(f,"mov dword [eax+8],F%d",g->id); E(f,"mov dword [eax+12],S%d",str_lit(fn_display_name(g)));
    E(f,"push eax");
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
/* function g as a value -> eax */
static int gen_funcref(F *f, AFunc *g){
    use_fn(g);
    if(!g->ncaps){ gg->fv_used[g->id]=1; rt("rt_static"); E(f,"mov eax,FV%d",g->id); return 0; }   /* nothing captured: a static object */
    if(g==f->fn){ E(f,"mov eax,[ebp%+d]",f->env); return 0; }                                    /* a nested def naming itself */
    return gen_closure(f,g);
}
static int gen(F *f, Expr *e){
    XInfo *xi=xinfo(e); Ty *t=TY(e);
    switch(e->kind){
        case EXPR_LITERAL:{
            Tok *tk=e->tok;
            if(tk->kind==T_STRING){ if(!tk->text[0]) E(f,"xor eax,eax"); else E(f,"mov eax,S%d",str_lit(tk->text)); return 0; }
            if(tk->is_float){ double d=tk->f; if(d==0.0) E(f,"fldz"); else if(d==1.0) E(f,"fld1"); else E(f,"fld qword [FC%d]",float_const(tk->text)); return 0; }
            if(t->k==TY_FLOAT){ char num[32]; snprintf(num,sizeof num,"%lld",(long long)tk->i);
                if(tk->i==0) E(f,"fldz"); else if(tk->i==1) E(f,"fld1"); else E(f,"fld qword [FC%d]",float_const(num)); return 0; }
            if(tk->i==0) E(f,"xor eax,eax"); else E(f,"mov eax,%u",(unsigned)(uint32_t)tk->i);
            return 0; }
        case EXPR_TRUE: E(f,"mov eax,1"); return 0;
        case EXPR_FALSE: E(f,"xor eax,eax"); return 0;
        case EXPR_NONE: return gen_zero(f,t);
        case EXPR_NAME:
            if(xi->kind==X_CONST_STR){ E(f,"mov eax,S%d",str_lit(xi->name)); return 0; }
            if(xi->kind==X_FUNCREF) return gen_funcref(f,xi->fn);
            load_var(f,xi->var); return 0;
        case EXPR_LAMBDA: return gen_funcref(f,xi->fn);
        case EXPR_UNARY:
            if(e->op==T_NOT){ gen_bool(f,e->a); E(f,"xor eax,1"); return 0; }
            if(xi->kind==X_OPMETHOD) return gen_op_call(f,xi->fn,e->a,NULL);
            gen_as(f,e->a,t,0);
            if(e->op==T_MINUS) E(f,is_flt(t)?"fchs":"neg eax"); else E(f,"not eax");
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
            if(xi->kind==X_BMOD){                         /* math.pi / math.e / math.tau */
                E(f,"fld qword [FC%d]",float_const(!strcmp(xi->name,"pi")?"3.141592653589793":!strcmp(xi->name,"tau")?"6.283185307179586":"2.718281828459045"));
                return 0; }
            if(xi->kind==X_VAR){ load_var(f,xi->var); return 0; }
            if(xi->kind==X_FUNCREF) return gen_funcref(f,xi->fn);
            if(xi->kind==X_BOUND){                        /* obj.method: a closure of obj (+16) */
                AFunc *m=xi->fn; use_fn(m); gg->bm_used[m->id]=1;
                gen_owned(f,e->a); panic_if_null(f); E(f,"push eax");
                E(f,"mov eax,20"); CALLRT(f,"rt_alloc");
                E(f,"mov dword [eax],1"); E(f,"mov dword [eax+4],ADFREE"); E(f,"mov dword [eax+8],BM%d",m->id); E(f,"mov dword [eax+12],S%d",str_lit(m->name));
                E(f,"pop ecx"); E(f,"mov [eax+16],ecx");
                return 1; }
            if(xi->kind==X_PROP){ E(f,"sub esp,4"); gen_borrow(f,e->a); E(f,"mov [esp],eax"); call_on_top(f,xi->fn); E(f,"add esp,4"); return is_ptr(xi->fn->ret); }
            if(xi->kind==X_CLASSCONST){ gen_as(f,xi->field->init,xi->field->ty,0); return 0; }
            gen_borrow(f,e->a); panic_if_null(f);
            load_mem(f,xi->field->ty,"eax",xi->field->offset);
            return 0;
        case EXPR_INDEX: return gen_index(f,e);
        case EXPR_SLICE: return gen_slice(f,e);
        case EXPR_LIST: case EXPR_SET: case EXPR_DICT: return gen_literal_container(f,e);
        case EXPR_TUPLE: return t->k==TY_TUPLE ? gen_tuple(f,e) : gen_literal_container(f,e);
        case EXPR_COMPREHENSION:
            if(e->comp_kind=='G'){                       /* a generator expression: call its generator function with the first iterable */
                AFunc *g=xi->fn; use_fn(g);
                if(g->nparams){ Ty *pt=g->params[0]->ty; E(f,"sub esp,%d",esize(pt)); gen_as(f,e->clauses[0].iter,pt,0); put_arg(f,pt,0); }
                if(g->ncaps){ gen_closure(f,g); hold(f); E(f,"mov edx,eax"); }
                E(f,"call F%d",g->id);
                if(g->nparams) E(f,"add esp,%d",esize(g->params[0]->ty));
                return 1;
            }
            return gen_comprehension(f,e);
        case EXPR_AWAIT: return gen_await(f,e);
        default: cg_fail(e->line,"unsupported expression");
    }
}

/* None as a value of type t: 0, 0.0, "" (null), a null object - and a fresh
   empty container for list/dict/set (so it can be filled). Returns owned. */
static int gen_zero(F *f, Ty *t){
    t=ty_find(t);
    if(t->k==TY_LIST||t->k==TY_SET){ E(f,"mov eax,%s",list_destroy(t->elem)); CALLRT(f,"rt_list_new"); return 1; }
    if(t->k==TY_DICT){ new_dict(f,t); return 1; }
    zero_value(f,t); return 0;
}

/* ---------------------------------------------------------------- string building */

static int is_str_lit(Expr *e){ return e->kind==EXPR_LITERAL && e->tok->kind==T_STRING; }
static int is_str_call(Expr *e){ if(e->kind!=EXPR_CALL||e->count!=1) return 0; XInfo *xi=xinfo(e); return xi->kind==X_BUILTIN && !strcmp(xi->name,"str"); }
static int is_fmt_expr(Expr *e){ return e->kind==EXPR_BINARY && e->op==T_PERCENT && is_str_lit(e->a) && TY(e)->k==TY_STR; }
static int is_concat(Expr *e){ return e->kind==EXPR_BINARY && e->op==T_PLUS && TY(e)->k==TY_STR; }
static int concat_parts(Expr *e, int *special){
    if(is_concat(e)) return concat_parts(e->a,special)+concat_parts(e->b,special);
    if(is_str_call(e)||is_fmt_expr(e)) *special=1;
    return 1;
}
static int concat_worth(Expr *e){ int sp=0; int n=concat_parts(e,&sp); return n>=3||sp; }

/* append str(x) to the string builder */
static void gen_into_sb(F *f, Expr *x){
    if(is_str_lit(x)){ sb_bytes_lit(f,x->tok->text,(int)strlen(x->tok->text)); return; }
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
    if(TY(x)->k==TY_STR) return gen(f,x);
    return gen_concat(f,x);
}

/* formatters of containers and objects: FMT<i>, eax = value */
static int fmt_index(Ty *t, int repr){
    t=ty_find(t);
    if(t->k!=TY_OBJ) repr=1;
    for(int i=0;i<gg->nfmts;i++) if(gg->fmts[i].repr==repr && ty_same(gg->fmts[i].ty,t)) return i;
    if(gg->nfmts==gg->cfmts){ gg->cfmts=gg->cfmts?gg->cfmts*2:16; gg->fmts=(Fmt*)xrealloc(gg->fmts,sizeof(Fmt)*(size_t)gg->cfmts); }
    gg->fmts[gg->nfmts].ty=t; gg->fmts[gg->nfmts].repr=repr;
    return gg->nfmts++;
}
/* append str()/repr() of the value in eax/st0 (borrowed) to the string builder */
static void gen_fmt(F *f, Ty *t, int repr){
    t=ty_find(t);
    switch(t->k){
        case TY_INT: CALLRT(f,"rt_sb_int"); return;
        case TY_BOOL: CALLRT(f,"rt_sb_bool"); return;
        case TY_FLOAT: CALLRT(f,"rt_sb_float"); return;
        case TY_STR: CALLRT(f,repr?"rt_sb_repr_str":"rt_sb_str"); return;
        case TY_BUF: E(f,"mov eax,Z%d",zlit("<buffer>")); CALLRT(f,"rt_sb_cstr"); return;
        case TY_TASK: E(f,"mov eax,Z%d",zlit("<Task>")); CALLRT(f,"rt_sb_cstr"); return;
        case TY_FILE: E(f,"mov eax,Z%d",zlit("<file>")); CALLRT(f,"rt_sb_cstr"); return;
        case TY_FUNC: E(f,"mov eax,Z%d",zlit("<function>")); CALLRT(f,"rt_sb_cstr"); return;
        case TY_GEN: E(f,"mov eax,Z%d",zlit("<generator object>")); CALLRT(f,"rt_sb_cstr"); return;
        case TY_LIST: case TY_SET: case TY_DICT: case TY_OBJ: case TY_TUPLE:
            rt("rt_sb_need"); E(f,"call FMT%d",fmt_index(t,repr)); return;
        default: E(f,"mov eax,Z%d",zlit("None")); CALLRT(f,"rt_sb_cstr"); return;     /* a bare None */
    }
}
static void emit_formatter(int i){
    F ff; memset(&ff,0,sizeof ff); F *f=&ff;
    Ty *t=gg->fmts[i].ty; int repr=gg->fmts[i].repr;
    buf_printf(&f->code,"\nFMT%d:                         ; %s of %s\n",i,repr?"repr":"str",ty_name(t));
    if(t->k==TY_OBJ){
        AClass *cls=t->cls;
        AFunc *m=repr?NULL:aot_find_method(cls,"__str__");
        if(!m) m=aot_find_method(cls,"__repr__");
        int lnone=new_label();
        E(f,"test eax,eax"); E(f,"jz L%d",lnone);
        if(m && !m->is_static && ty_find(m->ret)->k==TY_STR){
            use_fn(m);
            E(f,"push eax");
            if(m->overridden){ E(f,"mov eax,[eax+8]"); E(f,"call dword [eax+%d]",8+4*m->vslot); }
            else E(f,"call F%d",m->id);
            E(f,"mov [esp],eax"); CALLRT(f,"rt_sb_str"); E(f,"pop eax"); CALLRT(f,"rt_decref"); E(f,"ret");
        } else if(aot_is_exception(cls)){                   /* str(e): the message; repr(e): Name('message') */
            int keyerr=0; for(AClass *k=cls;k;k=k->base) if(k->builtin && !strcmp(k->name,"KeyError")) keyerr=1;
            if(!repr){ E(f,"mov eax,[eax+12]"); if(keyerr){ int l=new_label(); E(f,"test eax,eax"); E(f,"jz L%d",l); E(f,"jmp rt_sb_repr_str"); LBL(f,l); E(f,"ret"); rt("rt_sb_repr_str"); }
                       else { E(f,"jmp rt_sb_str"); rt("rt_sb_str"); } }
            else {
                int lno=new_label();
                E(f,"push eax"); E(f,"mov eax,[eax+8]"); E(f,"mov eax,[eax]"); CALLRT(f,"rt_sb_str");
                E(f,"mov al,'('"); CALLRT(f,"rt_sb_char");
                E(f,"pop eax"); E(f,"mov eax,[eax+12]"); E(f,"test eax,eax"); E(f,"jz L%d",lno); CALLRT(f,"rt_sb_repr_str");
                LBL(f,lno); E(f,"mov al,')'"); E(f,"jmp rt_sb_char");
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
    E(f,"push ebx"); E(f,"push esi"); E(f,"mov ebx,eax");
    if(isset){ int lne=new_label(), lgo=new_label();               /* the empty set prints as set() */
        E(f,"test ebx,ebx"); E(f,"jz L%d",lne); E(f,"cmp dword [ebx+8],0"); E(f,"jne L%d",lgo);
        LBL(f,lne); E(f,"mov eax,Z%d",zlit("set()")); CALLRT(f,"rt_sb_cstr"); E(f,"jmp L%d",lout);
        LBL(f,lgo); }
    E(f,"mov al,'%c'",t->k==TY_LIST?(t->tup?'(':'['):'{'); CALLRT(f,"rt_sb_char");
    E(f,"xor esi,esi");
    LBL(f,ltop);
    E(f,"test ebx,ebx"); E(f,"jz L%d",lend); E(f,"cmp esi,[ebx+8]"); E(f,"jae L%d",lend);
    E(f,"test esi,esi"); E(f,"jz L%d",lskip); E(f,"mov eax,Z%d",zlit(", ")); CALLRT(f,"rt_sb_cstr"); LBL(f,lskip);
    if(isdict){
        E(f,"mov eax,[ebx+16]"); E(f,"mov eax,[eax+esi*4]"); gen_fmt(f,ty_dkey(t),1);
        E(f,"mov eax,Z%d",zlit(": ")); CALLRT(f,"rt_sb_cstr");
        E(f,"mov eax,[ebx+20]");
    } else E(f,"mov eax,[ebx+16]");
    if(is_flt(el)) E(f,"fld qword [eax+esi*8]"); else E(f,"mov eax,[eax+esi*4]");
    gen_fmt(f,el,1);
    E(f,"inc esi"); E(f,"jmp L%d",ltop);
    LBL(f,lend);
    if(t->k==TY_LIST && t->tup){ int l=new_label(); E(f,"cmp esi,1"); E(f,"jne L%d",l); E(f,"mov al,','"); CALLRT(f,"rt_sb_char"); LBL(f,l); }   /* (x,) */
    E(f,"mov al,'%c'",t->k==TY_LIST?(t->tup?')':']'):'}'); CALLRT(f,"rt_sb_char");
    LBL(f,lout); E(f,"pop esi"); E(f,"pop ebx"); E(f,"ret");
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
    buf_printf(o,"\nTS%d:                          ; task: %s%s%s()\n",i,fn->cls?fn->cls->name:"",fn->cls?".":"",fn->name);
    if(gg->stubs[i].virt) buf_printf(o,"        mov eax,[esp]\n        mov eax,[eax+8]\n        call dword [eax+%d]\n",8+4*fn->vslot);
    else buf_printf(o,"        call F%d\n",fn->id);
    buf_printf(o,"        mov ecx,[rt_cur_task]\n");
    if(is_flt(ret)) buf_printf(o,"        fstp qword [ecx+24]\n");
    else if(ret->k!=TY_VOID){ buf_printf(o,"        mov [ecx+24],eax\n"); if(is_ptr(ret)) buf_printf(o,"        mov dword [ecx+44],1\n"); }
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
    E(f,"mov eax,%d",total); E(f,"mov edx,TS%d",ts); CALLRT(f,"rt_task_new");
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
    if(!strcmp(xi->name,"call")) return gen(f,x);
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
    E(f,"mov eax,%s",list_destroy(el)); CALLRT(f,"rt_list_new"); E(f,"push eax");
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
        if(!strcmp(m,"gcd")){ gen(f,a[0]); E(f,"push eax"); gen(f,a[1]); E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_gcd"); return 0; }
        if(!strcmp(m,"isqrt")){ gen(f,a[0]); CALLRT(f,"rt_isqrt"); return 0; }
        if(e->count==2){                                                   /* two floats */
            farg(f,a[0]); E(f,"sub esp,8"); E(f,"fstp qword [esp]");
            farg(f,a[1]); E(f,"fld qword [esp]"); E(f,"add esp,8");          /* st0 = x, st1 = y */
            if(!strcmp(m,"atan2")){ E(f,"fxch"); E(f,"fpatan"); }             /* atan(st1/st0): st1 = x(=y arg), st0 = y(=x arg) */
            else if(!strcmp(m,"pow")) CALLRT(f,"rt_fpow");
            else if(!strcmp(m,"hypot")){ E(f,"fmul st0,st0"); E(f,"fxch"); E(f,"fmul st0,st0"); E(f,"faddp st1,st0"); E(f,"fsqrt"); }
            else if(!strcmp(m,"fmod")) CALLRT(f,"rt_fmod_c");
            else { E(f,"fldln2"); E(f,"fxch"); E(f,"fyl2x"); E(f,"fxch"); E(f,"fldln2"); E(f,"fxch"); E(f,"fyl2x"); E(f,"fdivp st1,st0"); }  /* log(x, base) */
            return 0;
        }
        farg(f,a[0]);
        if(!strcmp(m,"sqrt")) E(f,"fsqrt");
        else if(!strcmp(m,"sin")) E(f,"fsin");
        else if(!strcmp(m,"cos")) E(f,"fcos");
        else if(!strcmp(m,"tan")){ E(f,"fptan"); E(f,"fstp st0"); }
        else if(!strcmp(m,"atan")){ E(f,"fld1"); E(f,"fpatan"); }
        else if(!strcmp(m,"asin")){ E(f,"fld st0"); E(f,"fmul st0,st0"); E(f,"fld1"); E(f,"fsubrp st1,st0"); E(f,"fsqrt"); E(f,"fpatan"); }
        else if(!strcmp(m,"acos")){ E(f,"fld st0"); E(f,"fmul st0,st0"); E(f,"fld1"); E(f,"fsubrp st1,st0"); E(f,"fsqrt"); E(f,"fxch"); E(f,"fpatan"); }
        else if(!strcmp(m,"exp")) CALLRT(f,"rt_fexp");
        else if(!strcmp(m,"log")){ E(f,"fldln2"); E(f,"fxch"); E(f,"fyl2x"); }
        else if(!strcmp(m,"log10")){ E(f,"fldlg2"); E(f,"fxch"); E(f,"fyl2x"); }
        else if(!strcmp(m,"log2")){ E(f,"fld1"); E(f,"fxch"); E(f,"fyl2x"); }
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
        if(!strcmp(m,"randint")||!strcmp(m,"randrange")){
            if(e->count==1){ E(f,"push 0"); gen(f,a[0]); }
            else { gen(f,a[0]); E(f,"push eax"); gen(f,a[1]); if(m[4]=='i') E(f,"inc eax"); }
            E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_rand_range"); return 0; }
        if(!strcmp(m,"choice")){
            Ty *t=TY(a[0]);
            gen_borrow(f,a[0]); E(f,"push eax");
            E(f,"xor eax,eax"); E(f,"mov edx,[esp]"); E(f,"test edx,edx"); int l=new_label(); E(f,"jz L%d",l); E(f,"mov edx,[edx+8]"); LBL(f,l);
            CALLRT(f,"rt_rand_range"); E(f,"mov edx,eax"); E(f,"pop eax");
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
    E(f,"pop eax"); CALLRT(f,"rt_sb_flush");
}
static void gen_print(F *f, APrint *pr){
    int n=pr->n;
    for(int i=0;i<n;i++) if(pr->star[i]){ gen_print_star(f,pr); return; }
    if(!pr->sep && !pr->end){
        if(n==0){ E(f,"mov ecx,P%d",print_lit("")); E(f,"mov edx,1"); CALLRT(f,"rt_write"); return; }
        Expr *a=pr->args[0]; Ty *t=TY(a);
        if(n==1 && is_str_lit(a) && !memchr(a->tok->text,0,1)){
            E(f,"mov ecx,P%d",print_lit(a->tok->text)); E(f,"mov edx,%d",(int)strlen(a->tok->text)+1); CALLRT(f,"rt_write"); return; }
        if(n==1 && t->k==TY_STR && !is_concat(a) && !is_fmt_expr(a) && !is_str_call(a)){ gen_borrow(f,a); CALLRT(f,"rt_print_str"); return; }
        if(n==1 && t->k==TY_INT){ gen(f,a); CALLRT(f,"rt_print_int"); return; }
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
            if(slots[i]){ if(is_flt(t)) E(f,"fld qword [ebp%+d]",slots[i]); else E(f,"mov eax,[ebp%+d]",slots[i]); }
            gen_fmt(f,t,0); }
        else gen_into_sb(f,pr->args[i]);
    }
    if(pr->end) gen_into_sb(f,pr->end); else { E(f,"mov al,10"); CALLRT(f,"rt_sb_char"); }
    E(f,"pop eax"); CALLRT(f,"rt_sb_flush");
}

/* Store the owned value in eax/st0 (of type vt) into an assignment target. */
static void store_target(F *f, Expr *t, Ty *vt, int line){
    XInfo *xi=xinfo(t);
    switch(t->kind){
        case EXPR_NAME:{ AVar *v=xi->kind==X_VAR&&xi->var?xi->var:find_var(f,t->name,line); conv(f,vt,v->ty); store_var(f,v); return; }
        case EXPR_ATTRIBUTE:
            if(xi->kind==X_VAR){ conv(f,vt,xi->var->ty); store_var(f,xi->var); return; }
            { AField *fd=xi->field;
              conv(f,vt,fd->ty); push_value(f,fd->ty);
              gen_borrow(f,t->a); panic_if_null(f); E(f,"mov ecx,eax");
              pop_value(f,fd->ty); store_mem(f,fd->ty,"ecx",fd->offset); return; }
        case EXPR_INDEX:{
            Ty *ct=TY(t->a), *el=ct->elem; int dict=ct->k==TY_DICT;
            if(ct->k==TY_OBJ){                                    /* obj[key] = value: obj.__setitem__(key, value) */
                AFunc *m=aot_find_method(ct->cls,"__setitem__"); Ty *kt=m->params[1]->ty, *pv=m->params[2]->ty;
                conv(f,vt,pv); int slot=park_value(f,pv,1);          /* the owned value, released with the statement's temporaries */
                int total=4+esize(kt)+esize(pv);
                E(f,"sub esp,%d",total);
                gen_borrow(f,t->a); E(f,"mov [esp],eax");
                gen_as(f,t->b,kt,0); if(is_flt(kt)) E(f,"fstp qword [esp+4]"); else E(f,"mov [esp+4],eax");
                if(is_flt(pv)){ E(f,"fld qword [ebp%+d]",slot); E(f,"fstp qword [esp+%d]",4+esize(kt)); } else { E(f,"mov eax,[ebp%+d]",slot); E(f,"mov [esp+%d],eax",4+esize(kt)); }
                call_on_top(f,m); E(f,"add esp,%d",total);
                drop_value(f,m->ret,1);
                return;
            }
            conv(f,vt,el); push_value(f,el);
            gen_borrow(f,t->a); E(f,"push eax");
            if(dict) gen_borrow(f,t->b); else gen(f,t->b);
            E(f,"mov edx,eax"); E(f,"pop eax"); E(f,"mov ecx,%d",esize(el));
            CALLRT(f,dict?"rt_dict_slot":"rt_list_at");
            E(f,"mov ecx,eax"); pop_value(f,el); store_mem(f,el,"ecx",0);
            return; }
        case EXPR_TUPLE: case EXPR_LIST:{                    /* a, b = some_list / some_tuple */
            Ty *lt=ty_find(vt), *el=lt->elem;
            hold(f); int slot=f->tmp[f->tmp_used-1];
            if(lt->k==TY_TUPLE){ unpack_tuple_slot(f,slot,lt,t->items,t->count,line); return; }
            E(f,"mov edx,%d",t->count); CALLRT(f,"rt_unpack_check");
            for(int i=0;i<t->count;i++){
                E(f,"mov eax,[ebp%+d]",slot); E(f,"mov eax,[eax+16]");
                if(is_flt(el)) E(f,"fld qword [eax+%d]",8*i); else E(f,"mov eax,[eax+%d]",4*i);
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
    if(is_flt(t)) E(f,"fstp qword [ebp%+d]",slot); else E(f,"mov [ebp%+d],eax",slot);
    return slot;
}
static void load_parked(F *f, Ty *t, int slot){
    if(is_flt(t)) E(f,"fld qword [ebp%+d]",slot); else E(f,"mov eax,[ebp%+d]",slot);
    if(is_ptr(t)) incref(f);
}

static void gen_augassign(F *f, AAssign *a, int line){
    Expr *t=a->target[0], *v=a->value;
    Ty *cur=TY(t), *tv=TY(v); int num=numeric_ty(cur);
    XInfo *xi=xinfo(t);
    if(a->opfn){                                             /* v += w with __add__ & co: v = v.__add__(w) */
        AFunc *m=a->opfn; Ty *pt=m->params[1]->ty; int total=4+esize(pt);
        if(t->kind==EXPR_INDEX && TY(t->a)->k==TY_OBJ) cg_fail(line,"augmented assignment to obj[key] is not supported in compiled code");
        E(f,"sub esp,%d",total);
        gen_borrow(f,t); E(f,"mov [esp],eax");
        gen_as(f,v,pt,0); if(is_flt(pt)) E(f,"fstp qword [esp+4]"); else E(f,"mov [esp+4],eax");
        call_on_top(f,m); E(f,"add esp,%d",total);
        store_target(f,t,m->ret,line);
        return;
    }
    if(cur->k==TY_LIST && a->aug==T_PLUS_ASSIGN){            /* list += x extends in place */
        gen_borrow(f,t); E(f,"push eax"); gen_borrow(f,v);
        E(f,"mov edx,eax"); E(f,"pop eax"); E(f,"mov ecx,%d",kind_of(cur->elem)); CALLRT(f,"rt_list_extend");
        return;
    }
    if(t->kind==EXPR_NAME || (t->kind==EXPR_ATTRIBUTE && xi->kind==X_VAR)){
        AVar *var=xi->var?xi->var:find_var(f,t->name,line);
        load_var(f,var); push_value(f,cur);
        gen_as(f,v,num?cur:tv,0);
        int o=apply_binop(f,a->aug,cur,cur,tv,line);
        if(is_ptr(cur) && !o) incref(f);
        store_var(f,var);
        return;
    }
    if(t->kind==EXPR_ATTRIBUTE){
        AField *fd=xi->field;
        gen_borrow(f,t->a); panic_if_null(f); E(f,"push eax");
        load_mem(f,cur,"eax",fd->offset); push_value(f,cur);
        gen_as(f,v,num?cur:tv,0);
        int o=apply_binop(f,a->aug,cur,cur,tv,line);
        if(is_ptr(cur) && !o) incref(f);
        E(f,"pop ecx"); store_mem(f,cur,"ecx",fd->offset);
        return;
    }
    if(t->kind==EXPR_INDEX){
        Ty *ct=TY(t->a); int es=esize(cur), dict=ct->k==TY_DICT;
        const char *at=dict?"rt_dict_get":"rt_list_at";
        gen_borrow(f,t->a); E(f,"push eax");
        if(dict) gen_borrow(f,t->b); else gen(f,t->b);
        E(f,"push eax");                                         /* [esp] index, [esp+4] container */
        E(f,"mov edx,eax"); E(f,"mov eax,[esp+4]"); E(f,"mov ecx,%d",es); CALLRT(f,at);
        load_mem(f,cur,"eax",0); push_value(f,cur);
        gen_as(f,v,num?cur:tv,0);
        int o=apply_binop(f,a->aug,cur,cur,tv,line);
        if(is_ptr(cur) && !o) incref(f);
        push_value(f,cur);
        E(f,"mov eax,[esp+%d]",es+4); E(f,"mov edx,[esp+%d]",es); E(f,"mov ecx,%d",es); CALLRT(f,at);
        E(f,"mov ecx,eax"); pop_value(f,cur); store_mem(f,cur,"ecx",0);
        E(f,"add esp,8");
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
        if(v->kind==EXPR_NONE){ Ty *want=TY(tg); if(tg->kind==EXPR_NAME||tg->kind==EXPR_ATTRIBUTE||tg->kind==EXPR_INDEX){ gen_zero(f,want); store_target(f,tg,want,s->line); return; } }
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
    E(f,"cmp byte [MI%d],0",m->index); E(f,"jne L%d",l);
    E(f,"mov byte [MI%d],1",m->index); E(f,"call F%d",m->body->id);
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
    int i=s->start; while(i<s->end && u->tv.v[i].kind!=T_IMPORT) i++;
    for(i++;i<s->end;i++){
        Tok *k=&u->tv.v[i];
        if(k->kind==T_AS){ i++; continue; }
        if(k->kind!=T_NAME) continue;
        if(t && symtab_find(&t->syms,k->text)) continue;
        char sub[300]; snprintf(sub,sizeof sub,"%s.%s",dotted,k->text);
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
            E(f,"mov eax,[ebp%+d]",exc); E(f,"mov edx,VT%d",cls->id); CALLRT(f,"rt_isinstance"); E(f,"test eax,eax"); E(f,"jnz L%d",l); }
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
            if(s->expr){
                Expr *e=aot_expr(u,&s->expr);
                if(rt_->k==TY_VOID){ int o=gen(f,e); drop_value(f,TY(e),o); }
                else gen_as(f,e,rt_,1);
            } else if(rt_->k!=TY_VOID){ int o=gen_zero(f,rt_); if(is_ptr(rt_) && !o) incref(f); }
            if(f->tmp_used>mark){ if(!is_flt(rt_)) E(f,"push eax"); scope_close(f,mark); if(!is_flt(rt_)) E(f,"pop eax"); }
            if(f->ntries){                                    /* finally blocks run first; keep the value meanwhile */
                int keep=frame_slot(f,8);
                if(is_flt(rt_)) E(f,"fstp qword [ebp%+d]",keep); else if(rt_->k!=TY_VOID) E(f,"mov [ebp%+d],eax",keep);
                leave_tries(f,0);
                if(is_flt(rt_)) E(f,"fld qword [ebp%+d]",keep); else if(rt_->k!=TY_VOID) E(f,"mov eax,[ebp%+d]",keep);
            }
            E(f,"jmp L%d",f->lret);
            break; }
        case STMT_DEL:{
            ADel *d=aot_del(u,s);
            for(int i=0;i<d->n;i++){ Expr *t=d->t[i]; Ty *ct=TY(t->a), *el=ct->elem; int dict=ct->k==TY_DICT;
                gen_borrow(f,t->a); E(f,"push eax");
                if(dict) gen_borrow(f,t->b); else gen(f,t->b);
                E(f,"mov edx,eax"); E(f,"pop eax"); E(f,"mov ecx,%d",esize(el));
                CALLRT(f,dict?"rt_dict_del":"rt_list_pop");
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
        case STMT_CLASS_DEF:                             /* decorated methods: Class.name = decorators(method) */
            for(int i=0;i<f->fn->mod->syms.n;i++){ ASym *x=&f->fn->mod->syms.v[i];
                if(x->kind!=AS_CLASS || strcmp(x->name,s->name)) continue;
                AClass *cls=(AClass*)x->p;
                for(int k=0;k<cls->nmethods;k++){ AFunc *m=cls->methods[k];
                    if(m->decovar){ gen_owned(f,m->def->expr2); store_var(f,m->decovar); } }
            }
            break;
        case STMT_YIELD:{
            Ty *yt=f->fn->yield_ty;
            if(!s->expr){ int o=gen_zero(f,yt); if(is_ptr(yt)&&!o) incref(f); gen_yield_value(f,yt); break; }
            Expr *e=aot_expr(u,&s->expr);
            if(s->block_tag==7){                          /* yield from: each item */
                Iter I; iter_begin(f,e,&I);
                iter_value(f,&I,0); if(is_ptr(I.elem)) incref(f); conv(f,I.elem,yt);
                gen_yield_value(f,yt);
                iter_end(f,&I); LBL(f,I.exit); iter_release(f,&I);
                break;
            }
            gen_as(f,e,yt,1); gen_yield_value(f,yt);
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
            for(int i=0;i<e->count;i++) if(TY(e->items[i])->k==TY_GEN) return 0;   /* draining a generator runs its code */
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
    if(t->k==TY_STR||t->k==TY_TUPLE||t->k==TY_FILE) return 1;              /* static 1-char strings; immutable; a fresh list of lines */
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
        plan_bind(pl,&xi->cvars[2*i],cl->nvars,safe);
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
            plan_bind(pl,vars,n,safe);
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
            if(s->expr && s->expr->kind!=EXPR_TOKEN_RANGE) plan_expr(pl,s->expr);
            if(s->kind==STMT_DEL){ ADel *d=aot_del(u,s); for(int i=0;i<d->n;i++) plan_expr(pl,d->t[i]); }
            if(s->expr2 && s->expr2->kind!=EXPR_TOKEN_RANGE) plan_expr(pl,s->expr2);
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
        pl.only[i]=pl.ok[i]=(i>=fn->nparams && is_ptr(t) && t->k!=TY_FUNC && !v->cell && !v->captured); }
    if(fn->lam) plan_expr(&pl,fn->lam->a);
    else if(fn->genexp){                                                         /* its clauses: the body yields (anything may run) */
        Expr *e=fn->genexp; XInfo *xi=xinfo(e); int save=pl.nstack;
        for(int i=0;i<e->nclause;i++){ CompClause *cl=&e->clauses[i];
            Expr *it=(i==0&&xi->key)?xi->key:cl->iter; plan_expr(&pl,it);
            plan_bind(&pl,&xi->cvars[2*i],cl->nvars,iter_safe(it));
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
    if(!e || e->kind==EXPR_TOKEN_RANGE) return;
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
    if(!e || e->kind==EXPR_TOKEN_RANGE) return;
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
    if(is_flt(yt)) E(f,"fstp qword [ecx+24]");
    else if(is_ptr(yt)){ rt("rt_decref"); E(f,"xchg eax,[ecx+24]"); E(f,"call rt_decref"); }
    else E(f,"mov [ecx+24],eax");
    CALLRT(f,"rt_gen_yield");
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
        iter_store_vars(f,&its[i],&xi->cvars[2*i],cl->nvars);
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
    if(fn->ncaps){ layout_caps(fn); f->env=frame_slot(f,4); }
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
        if(f->tmp_used>mark){ if(!is_flt(r) && r->k!=TY_VOID) E(f,"push eax"); scope_close(f,mark); if(!is_flt(r) && r->k!=TY_VOID) E(f,"pop eax"); }
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
    buf_printf(o,"\n; %s%s%s()%s\n%s%d:\n",fn->cls?fn->cls->name:"",fn->cls?".":"",kind,fn->is_gen?": the generator's function":"",fn->is_gen?"GB":"F",fn->id);
    buf_printf(o,"        push ebp\n        mov ebp,esp\n        push ebx\n        push esi\n        push edi\n");
    int words=(f->frame+3)/4;
    if(words<=8) for(int i=0;i<words;i++) buf_printf(o,"        push 0\n");
    else buf_printf(o,"        sub esp,%d\n        mov edi,esp\n        mov ecx,%d\n        xor eax,eax\n        rep stosd\n",words*4,words);
    if(f->env) buf_printf(o,"        mov [ebp%+d],edx\n",f->env);
    if(!fn->is_gen) for(int i=0;i<fn->nparams;i++) if(f->param_stored[i] && is_ptr(fn->params[i]->ty) && !fn->params[i]->cell && !fn->params[i]->consumed){ rt("rt_incref"); buf_printf(o,"        mov eax,[ebp%+d]\n        call rt_incref\n",argpos[i]); }
    for(int i=0;i<fn->nvars;i++){ AVar *v=fn->vars[i]; if(!v->cell) continue;   /* cells of the variables closures share */
        rt("rt_cell_new"); rt("rt_incref");
        buf_printf(o,"        mov eax,%d\n        call rt_cell_new\n",is_ptr(v->ty)?1:0);
        if(i<fn->nparams){
            if(is_flt(v->ty)) buf_printf(o,"        fld qword [ebp%+d]\n        fstp qword [eax+8]\n",argpos[i]);
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
        if(keep) buf_printf(o,"        push eax\n");
        for(int i=0;i<f->nrefs;i++) buf_printf(o,"        mov eax,[ebp%+d]\n        call rt_decref\n",f->refs[i]);
        if(keep) buf_printf(o,"        pop eax\n");
    }
    buf_printf(o,"        lea esp,[ebp-12]\n        pop edi\n        pop esi\n        pop ebx\n        pop ebp\n        ret\n");
    if(fn->is_gen){                                       /* F<id> makes the generator: arguments and closure kept in it */
        Ty *yt=ty_find(fn->yield_ty);
        rt("rt_gen_new"); rt("rt_gen_next"); rt("rt_incref");
        buf_printf(&gg->data,"align 4\nGD%d dd GB%d,%d,%d",fn->id,fn->id,(is_ptr(yt)?1:0)|(is_flt(yt)?2:0),f->nrefs);
        for(int i=0;i<f->nrefs;i++) buf_printf(&gg->data,",%d",f->refs[i]);
        buf_printf(&gg->data,"   ; generator %s\n",kind);
        buf_printf(o,"F%d:                            ; %s(): a new generator\n",fn->id,kind);
        buf_printf(o,"        push ebx\n        mov ebx,edx\n        mov eax,%d\n        mov edx,GD%d\n        call rt_gen_new\n        push eax\n        mov ecx,[eax+36]\n",argbytes,fn->id);
        for(int w=0;w<argbytes;w+=4) buf_printf(o,"        mov edx,[esp+%d]\n        mov [ecx+%d],edx\n",12+w,w);
        for(int i=0;i<fn->nparams;i++) if(is_ptr(fn->params[i]->ty)) buf_printf(o,"        mov eax,[esp+%d]\n        call rt_incref\n",12+argpos[i]-8);
        if(fn->ncaps) buf_printf(o,"        mov eax,ebx\n        call rt_incref\n        mov ecx,[esp]\n        mov [ecx+44],ebx\n");
        buf_printf(o,"        pop eax\n        pop ebx\n        ret\n");
    }
    free(f->code.s); free(f->tmp); free(f->refs); free(f);
}
static int adapter_index(Ty *to, Ty *from){
    for(int i=0;i<gg->nadapters;i++) if(ty_same(gg->adapters[i].to,to) && ty_same(gg->adapters[i].from,from)) return i;
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
    E(f,"mov eax,%s",list_destroy(sel)); CALLRT(f,"rt_list_new"); E(f,"mov esi,eax");
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
    E(f,"push eax"); E(f,"mov eax,esi"); CALLRT(f,"rt_decref"); E(f,"mov eax,edi"); CALLRT(f,"rt_decref"); E(f,"pop eax");
    E(f,"pop edi"); E(f,"pop esi"); E(f,"pop ebx"); E(f,"pop ebp"); E(f,"ret");
    buf_cat(&gg->text,&f->code); free(f->code.s);
}
/* BM<id>: the code of method m's bound values: the object (+16) first, then the arguments */
static void emit_bound_method(AFunc *m){
    Buf *o=&gg->text; int argbytes=0;
    for(int i=1;i<m->nparams;i++) argbytes+=esize(m->params[i]->ty);
    buf_printf(o,"\nBM%d:                          ; %s.%s bound to an object\n        push ebp\n        mov ebp,esp\n        sub esp,%d\n        mov eax,[edx+16]\n        mov [esp],eax\n",m->id,m->cls?m->cls->name:"",m->name,argbytes+4);
    for(int w=0;w<argbytes;w+=4) buf_printf(o,"        mov ecx,[ebp+%d]\n        mov [esp+%d],ecx\n",8+w,4+w);
    if(m->overridden) buf_printf(o,"        mov eax,[eax+8]\n        call dword [eax+%d]\n",8+4*m->vslot);
    else buf_printf(o,"        call F%d\n",m->id);
    buf_printf(o,"        leave\n        ret\n");
}
/* CD<id>: destroy routine of fn's closures */
static void emit_closure_destroy(AFunc *fn){
    Buf *o=&gg->text; int any=0;
    rt("rt_free"); rt("rt_decref");
    buf_printf(o,"\nCD%d:                          ; destroy a closure of %s\n",fn->id,fn->lam?"<lambda>":fn->genexp?"<genexpr>":fn->name);
    for(int i=0;i<fn->ncaps;i++){ AVar *v=fn->caps[i]; if(!root_var(v)->cell && !is_ptr(v->ty)) continue;
        if(!any){ buf_printf(o,"        push ebx\n        mov ebx,eax\n"); any=1; }
        buf_printf(o,"        mov eax,[ebx+%d]\n        call rt_decref\n",v->capoff); }
    if(any) buf_printf(o,"        mov eax,ebx\n        pop ebx\n");
    buf_printf(o,"        jmp rt_free\n");
}

static void emit_class(AClass *c){
    buf_printf(&gg->data,"align 4\nVT%d dd S%d,",c->id,str_lit(c->name));
    if(c->base) buf_printf(&gg->data,"VT%d",c->base->id); else buf_printf(&gg->data,"0");
    for(int i=0;i<c->nvt;i++) buf_printf(&gg->data,",F%d",c->vt[i]->id);
    buf_printf(&gg->data,"   ; class %s\n",c->name);
    Buf *o=&gg->text; int any=0;
    buf_printf(o,"\nDT%d:                          ; destroy %s\n",c->id,c->name);
    for(AClass *k=c;k;k=k->base) for(int i=0;i<k->nfields;i++) if(is_ptr(k->fields[i]->ty)){
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
        for(int k=0;k<t->nelems;k++){ Ty *et=ty_find(t->elems[k]); buf_printf(d,",%d",et->k==TY_TUPLE?4:kind_of(et)); }
        buf_printf(d,"   ; %s\n",ty_name(t)); }
    if(gg->nflts){ buf_printf(d,"align 8\n"); for(int i=0;i<gg->nflts;i++) buf_printf(d,"FC%d dq %s\n",i,gg->flts[i].s); }
    for(int i=0;i<gg->nlits;i++){
        buf_printf(d,"align 4\nS%d dd 0x40000000,rt_static,%d\n        ",i,gg->lits[i].len);
        buf_bytes(d,gg->lits[i].s,gg->lits[i].len,1);
    }
    for(int i=0;i<gg->nplits;i++){ buf_printf(d,"P%d ",i); Lit *l=&gg->plits[i]; char *t=(char*)xmalloc((size_t)l->len+1); memcpy(t,l->s,(size_t)l->len); t[l->len]='\n'; buf_bytes(d,t,l->len+1,0); free(t); }
    for(int i=0;i<gg->nzlits;i++){ buf_printf(d,"Z%d ",i); buf_bytes(d,gg->zlits[i].s,gg->zlits[i].len,1); }
    for(int i=0;i<gg->nrlits;i++){ buf_printf(d,"RL%d ",i); buf_bytes(d,gg->rlits[i].s,gg->rlits[i].len,0); }
}

/* x87 rounding to double precision: float arithmetic gives Python's results */
#define FPU_DOUBLE "        push 0x027F\n        fldcw [esp]\n        pop eax\n"

int aot_generate(AProg *p, const AotCodegenOptions *opt, char **out, size_t *outlen){
    G g; memset(&g,0,sizeof g); gg=&g;
    g.p=p; g.target=opt->target; g.stack=opt->stack_size?opt->stack_size:65536;
    g.rt=aot_rt_new(opt->target);
    g.class_used=(int*)xmalloc(sizeof(int)*(size_t)(p->nclasses+1)); memset(g.class_used,0,sizeof(int)*(size_t)(p->nclasses+1));
    g.fv_used=(char*)xmalloc((size_t)p->nfuncs+1); memset(g.fv_used,0,(size_t)p->nfuncs+1);
    g.cd_used=(char*)xmalloc((size_t)p->nfuncs+1); memset(g.cd_used,0,(size_t)p->nfuncs+1);
    g.bm_used=(char*)xmalloc((size_t)p->nfuncs+1); memset(g.bm_used,0,(size_t)p->nfuncs+1);
    AFunc *main_body=p->mods[0]->body;
    use_fn(main_body);
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
        if(g.fv_used[i]) buf_printf(&g.data,"align 4\nFV%d dd 0x40000000,rt_static,F%d,S%d   ; %s as a value\n",i,i,str_lit(fn_display_name(p->funcs[i])),fn_display_name(p->funcs[i]));
    }
    if(aot_rt_used(g.rt,"rt_throw")){                   /* run-time errors raise built-in exceptions */
        static const char *rtexc[]={"IndexError","KeyError","ZeroDivisionError","AttributeError","ValueError","AssertionError","FileNotFoundError","StopIteration","TypeError",NULL};
        rt("rt_raise_builtin");
        for(int k=0;rtexc[k];k++) for(int i=0;i<p->nclasses;i++) if(p->classes[i]->builtin && !strcmp(p->classes[i]->name,rtexc[k])){
            use_class(p->classes[i]);
            buf_printf(&g.data,"VTX_%s = VT%d\nDTX_%s = DT%d\n",rtexc[k],p->classes[i]->id,rtexc[k],p->classes[i]->id);
        }
    }
    for(int i=0;i<p->nclasses;i++) if(g.class_used[i]) emit_class(p->classes[i]);
    emit_lit_pools();
    for(int i=0;i<p->nglobals;i++){ AVar *v=p->globals[i]; buf_printf(&g.bss,"G%d %s 1   ; %s.%s\n",v->id,is_flt(v->ty)?"rq":"rd",v->mod->name,v->name); }
    for(int i=1;i<p->nmods;i++) if(p->mods[i]->used) buf_printf(&g.bss,"MI%d rb 1   ; %s imported\n",i,p->mods[i]->name);

    Buf o; memset(&o,0,sizeof o);
    int heap=aot_rt_used(g.rt,"rt_os_alloc")||aot_rt_used(g.rt,"rt_con_open");
    buf_printf(&o,"; generated by minipy --compile from %s\n",p->mods[0]->unit->path?p->mods[0]->unit->path:"?");
    const char *report="";
    if(opt->count_allocs && aot_rt_used(g.rt,"rt_alloc")){ rt("rt_live_report"); report="        call rt_live_report\n"; buf_printf(&g.data,"RT_COUNT_ALLOCS = 1\n"); }
    if(g.target==AOT_TARGET_LINUX){
        buf_printf(&o,"format ELF executable 3\nentry start\n\nsegment readable executable\n\nstart:\n        fninit\n%s",FPU_DOUBLE);
        buf_printf(&o,"        call F%d\n%s        xor ebx,ebx\n        jmp rt_exit\n",main_body->id,report);
    } else {
        buf_printf(&o,"format binary as ''\nuse32\n        org 0\n        db 'MENUET01'\n        dd 1,start,i_end,mem_end,stack_top,0,0\n\nstart:\n        fninit\n%s",FPU_DOUBLE);
        if(heap) buf_printf(&o,"        mov eax,68\n        mov ebx,11\n        int 0x40\n");
        buf_printf(&o,"        call F%d\n%s        xor ebx,ebx\n        jmp rt_exit\n",main_body->id,report);
    }
    buf_cat(&o,&g.text);
    buf_printf(&o,"\n; ---- runtime\n");
    aot_rt_emit(g.rt,0,put_cb,&o);
    if(g.target==AOT_TARGET_LINUX) buf_printf(&o,"\nsegment readable writeable\n\n");
    else buf_printf(&o,"\n; ---- data\n");
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
