/* ========================= cpython target: Python -> C =========================
   One module of the program becomes one C file. Each function, lambda,
   class body and the module body is a C function ("unit") taking the
   arguments CPython already bound (see capi_rt.h for the trampolines), with
   its locals as C variables (owned references, NULL when unbound), cells for
   the variables inner functions use, and every intermediate value in a
   temporary (t<n>) that holds a new reference until it is consumed. Errors
   jump to the innermost handler label (an except / finally / with of the
   unit, or its exit, which adds the line to the traceback); temporaries
   still holding values there are cleared.

   Comprehensions are inlined (their variables are C variables of the unit).
   What this compiler does not translate (yet) - generators and coroutines,
   async constructs, match, except*, t-strings, functions using locals() -
   CPython compiles: such a function is an "island", the code object
   CPython made from the same source (capi_helper.py), given its cells.
   Annotations (PEP 649) are always CPython's __annotate__ functions. */

#include "capi.h"
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>

/* ---------------------------------------------------------------- buffers */
typedef struct { char *s; size_t len, cap; } XB;
static void xb_put(XB *b, const char *s, size_t n){
    if(b->len+n+1>b->cap){ b->cap=(b->len+n+1)*2; b->s=(char*)xrealloc(b->s,b->cap); }
    memcpy(b->s+b->len,s,n); b->len+=n; b->s[b->len]=0;
}
static void xb_vf(XB *b, const char *fmt, va_list ap){
    char tmp[2048]; va_list ap2; va_copy(ap2,ap);
    int n=vsnprintf(tmp,sizeof tmp,fmt,ap);
    if(n<(int)sizeof tmp) xb_put(b,tmp,(size_t)n);
    else { char *big=(char*)xmalloc((size_t)n+1); vsnprintf(big,(size_t)n+1,fmt,ap2); xb_put(b,big,(size_t)n); free(big); }
    va_end(ap2);
}
static void xb_f(XB *b, const char *fmt, ...){ va_list ap; va_start(ap,fmt); xb_vf(b,fmt,ap); va_end(ap); }
static void xb_cat(XB *b, XB *x){ if(x->len) xb_put(b,x->s,x->len); }

/* ---------------------------------------------------------------- module state */
typedef struct { char *key; int idx; } KEnt;
typedef struct {
    CapiIn *in; CSymtable st;
    XB kinit, kdata;            /* constants: initialization, static data */
    KEnt *keys; int nkeys, capkeys; int nk;
    XB refs; int nrefs;         /* code objects: MpyCodeRef initializers */
    XB units, protos;           /* C functions */
    XB stub; int stub_line;     /* the trampolines (Python) */
    int future_ann;             /* from __future__ import annotations */
    int nislands, nunits;
    jmp_buf jb; char *err;
    int *lines;                 /* first line of each scope (CPython's co_firstlineno) */
    int *nths;                  /* ... and which of the code objects of that qualname and line */
    int nsites;                 /* static per-site caches (GC<n>, XS<n>) */
} Gen;

static void gfail(Gen *g, int line, const char *fmt, ...){
    char m[600]; va_list ap; va_start(ap,fmt); vsnprintf(m,sizeof m,fmt,ap); va_end(ap);
    if(!g->err){ char b[700]; snprintf(b,sizeof b,"line %d: %s",line,m); g->err=xstrdup2(b); }
    longjmp(g->jb,1);
}

/* ---- constants: K[n] */
static int k_find(Gen *g, const char *key, size_t klen){
    for(int i=0;i<g->nkeys;i++) if(!memcmp(g->keys[i].key,key,klen) && g->keys[i].key[klen]==0 && strlen(g->keys[i].key)==klen) return g->keys[i].idx;
    return -1;
}
static int k_add(Gen *g, const char *key, size_t klen){
    if(g->nkeys==g->capkeys){ g->capkeys=g->capkeys?g->capkeys*2:256; g->keys=(KEnt*)xrealloc(g->keys,sizeof(KEnt)*(size_t)g->capkeys); }
    g->keys[g->nkeys].key=xstrndup2(key,(int)klen); g->keys[g->nkeys].idx=g->nk;
    g->nkeys++;
    return g->nk++;
}
/* C string literal of bytes (octal escapes: no hex-digit ambiguity) */
static void c_lit(XB *b, const unsigned char *s, size_t n){
    xb_put(b,"\"",1);
    for(size_t i=0;i<n;i++){
        unsigned char c=s[i];
        if(c=='\\'||c=='"') xb_f(b,"\\%c",c);
        else if(c>=32 && c<127 && c!='?') xb_put(b,(const char*)&c,1);
        else xb_f(b,"\\%03o",c);
    }
    xb_put(b,"\"",1);
}
static int k_str(Gen *g, const char *s){              /* an interned str from UTF-8 */
    size_t n=strlen(s); char *key=(char*)xmalloc(n+3); key[0]='s'; key[1]=':'; memcpy(key+2,s,n+1);
    int i=k_find(g,key,n+2); if(i>=0){ free(key); return i; }
    i=k_add(g,key,n+2); free(key);
    xb_f(&g->kinit,"    if(!(K[%d]=PyUnicode_FromStringAndSize(",i); c_lit(&g->kinit,(const unsigned char*)s,n); xb_f(&g->kinit,",%zu))) return -1; PyUnicode_InternInPlace(&K[%d]);\n",n,i);
    return i;
}
static int k_int(Gen *g, long v){
    char key[64]; snprintf(key,sizeof key,"i:%ld",v);
    int i=k_find(g,key,strlen(key)); if(i>=0) return i;
    i=k_add(g,key,strlen(key));
    xb_f(&g->kinit,"    if(!(K[%d]=PyLong_FromLong(%ldL))) return -1;\n",i,v);
    return i;
}
static int k_const(Gen *g, PyConst *c){
    XB key={0};
    switch(c->kind){
        case PC_Int: xb_f(&key,"I:%s",c->text); break;
        case PC_Float: xb_f(&key,"F:%s",c->text); break;
        case PC_Complex: xb_f(&key,"J:%s",c->text); break;
        case PC_Bytes: xb_put(&key,"B:",2); for(int i=0;i<c->blen;i++) xb_f(&key,"%02x",c->b[i]); break;
        case PC_Str:
            if(c->text){ xb_f(&key,"N:%s",c->text); break; }
            xb_put(&key,"u:",2); for(int i=0;i<c->ulen;i++) xb_f(&key,"%x,",c->u[i]); break;
        default: free(key.s); return -1;
    }
    int i=k_find(g,key.s,key.len); if(i>=0){ free(key.s); return i; }
    i=k_add(g,key.s,key.len); free(key.s);
    switch(c->kind){
        case PC_Int: xb_f(&g->kinit,"    if(!(K[%d]=PyLong_FromString(\"%s\",NULL,0))) return -1;\n",i,c->text); break;
        case PC_Float: xb_f(&g->kinit,"    if(!(K[%d]=PyFloat_FromDouble(PyOS_string_to_double(\"%s\",NULL,NULL)))) return -1;\n",i,c->text); break;
        case PC_Complex: xb_f(&g->kinit,"    if(!(K[%d]=PyComplex_FromDoubles(0.0,PyOS_string_to_double(\"%s\",NULL,NULL)))) return -1;\n",i,c->text); break;
        case PC_Bytes: xb_f(&g->kinit,"    if(!(K[%d]=PyBytes_FromStringAndSize(",i); c_lit(&g->kinit,c->b,(size_t)c->blen); xb_f(&g->kinit,",%d))) return -1;\n",c->blen); break;
        case PC_Str:{
            if(c->text){ xb_f(&g->kinit,"    if(!(K[%d]=PyUnicode_DecodeUnicodeEscape(",i); c_lit(&g->kinit,(const unsigned char*)c->text,strlen(c->text)); xb_f(&g->kinit,",%zu,NULL))) return -1;\n",strlen(c->text)); break; }
            int ascii=1; for(int k=0;k<c->ulen;k++) if(c->u[k]>=128) ascii=0;
            if(ascii){
                unsigned char *tmp=(unsigned char*)xmalloc((size_t)c->ulen+1); for(int k=0;k<c->ulen;k++) tmp[k]=(unsigned char)c->u[k];
                xb_f(&g->kinit,"    if(!(K[%d]=PyUnicode_FromStringAndSize(",i); c_lit(&g->kinit,tmp,(size_t)c->ulen); xb_f(&g->kinit,",%d))) return -1;\n",c->ulen);
                free(tmp);
            } else {
                xb_f(&g->kdata,"static const Py_UCS4 KS%d[]={",i);
                for(int k=0;k<c->ulen;k++) xb_f(&g->kdata,"%s0x%x",k?",":"",c->u[k]);
                xb_f(&g->kdata,"};\n");
                xb_f(&g->kinit,"    if(!(K[%d]=PyUnicode_FromKindAndData(PyUnicode_4BYTE_KIND,KS%d,%d))) return -1;\n",i,i,c->ulen);
            }
            xb_f(&g->kinit,"    PyUnicode_InternInPlace(&K[%d]);\n",i);
            break; }
        default: break;
    }
    return i;
}
static int k_names(Gen *g, char **names, int n){     /* a tuple of str (keyword names) */
    XB key={0}; xb_put(&key,"T:",2); for(int i=0;i<n;i++) xb_f(&key,"%s,",names[i]);
    int i=k_find(g,key.s,key.len); if(i>=0){ free(key.s); return i; }
    int *ks=(int*)xmalloc(sizeof(int)*(size_t)n); for(int j=0;j<n;j++) ks[j]=k_str(g,names[j]);
    i=k_add(g,key.s,key.len); free(key.s);
    xb_f(&g->kinit,"    if(!(K[%d]=PyTuple_Pack(%d",i,n); for(int j=0;j<n;j++) xb_f(&g->kinit,",K[%d]",ks[j]); xb_f(&g->kinit,"))) return -1;\n");
    free(ks);
    return i;
}

/* ---- code objects: CO[n] */
static int ref_stub(Gen *g, CScope *u, const char *realname, int line){
    xb_f(&g->refs,"    {1,\"__mpy_u%d\",",u->id); c_lit(&g->refs,(const unsigned char*)realname,strlen(realname));
    xb_f(&g->refs,","); c_lit(&g->refs,(const unsigned char*)u->qualname,strlen(u->qualname)); xb_f(&g->refs,",%d,0},\n",line);
    return g->nrefs++;
}
static int ref_cpy(Gen *g, const char *qualname, int line, int nth){
    xb_f(&g->refs,"    {0,"); c_lit(&g->refs,(const unsigned char*)qualname,strlen(qualname)); xb_f(&g->refs,",NULL,NULL,%d,%d},\n",line,nth);
    return g->nrefs++;
}

/* ---- CPython's view */
static CpyCode *cpy_find(Gen *g, const char *qualname, int line, int nth){
    CpyMeta *m=g->in->meta; int seen=0;
    for(int i=0;i<m->ncodes;i++){
        CpyCode *c=&m->codes[i];
        if(c->line==line && !strcmp(c->qualname,qualname)){ if(seen==nth) return c; seen++; }
    }
    return NULL;
}
static int cpy_has(char **v, int n, const char *s){ for(int i=0;i<n;i++) if(!strcmp(v[i],s)) return 1; return 0; }
static const char *cpy_doc(Gen *g, int line, int col){
    CpyMeta *m=g->in->meta;
    for(int i=0;i<m->ndocs;i++) if(m->docs[i].line==line && m->docs[i].col==col) return m->docs[i].doc;
    return NULL;
}

/* ---------------------------------------------------------------- units */
enum { CX_LOOP, CX_FINALLY, CX_HANDLER, CX_WITH };
typedef struct Cx Cx;
struct Cx {
    int kind; Cx *up;
    int brk, cont;              /* loop labels */
    int id;                     /* finally / handler / with number */
    int err;                    /* the error label outside this context */
    const char *exc_name;       /* handler: its `as` name (deleted when leaving) */
    CScope *s;
};
typedef struct {
    Gen *g; CScope *sc;         /* the unit's scope */
    XB b, decl;
    int ntemp, nlab, nctx;
    int err;                    /* current error label (-1: the unit's exit) */
    Cx *cx;
    int line;
    CScope **comps; int ncomps;
    int ret_lab;
} Fn;

static int tmp(Fn *f){ return f->ntemp++; }
static int lab(Fn *f){ return f->nlab++; }
static char ebuf[8][32]; static int ebi;
static const char *ERR(Fn *f){ ebi=(ebi+1)&7; if(f->err<0) snprintf(ebuf[ebi],32,"L_err"); else snprintf(ebuf[ebi],32,"L%d",f->err); return ebuf[ebi]; }
static void E(Fn *f, const char *fmt, ...){ va_list ap; va_start(ap,fmt); xb_put(&f->b,"    ",4); xb_vf(&f->b,fmt,ap); xb_put(&f->b,"\n",1); va_end(ap); }
static void LBL(Fn *f, int l){ xb_f(&f->b,"  L%d:;\n",l); }
static void clr_range(Fn *f, int lo, int hi){ for(int i=lo;i<hi;i++) xb_f(&f->b,"    Py_CLEAR(t%d);\n",i); }
static void set_line(Fn *f, int line){ if(line && line!=f->line){ f->line=line; E(f,"ln=%d;",line); } }

/* ---------------------------------------------------------------- names */
typedef enum { NR_LOCAL, NR_CELL, NR_GLOBAL, NR_NAME, NR_CLASSDEREF } NRKind;
typedef struct { NRKind k; char var[96]; int free; } NameRef;

static int sym_index(CScope *s, const char *name){ for(int i=0;i<s->nsyms;i++) if(!strcmp(s->syms[i].name,name)) return i; return -1; }
static int free_index(CScope *s, const char *name){ for(int i=0;i<s->nfree;i++) if(!strcmp(s->freevars[i],name)) return i; return -1; }
static int is_inline(Fn *f, CScope *s){ return s!=f->sc; }

/* the C expression of the cell of `name` (mangled) as scope s sees it */
static int cell_expr(Fn *f, CScope *s, const char *name, char *out, size_t n){
    if(s->kind==SC_CLASS && s==f->sc){
        if(!strcmp(name,"__class__") && s->needs_class_cell){ snprintf(out,n,"c_class"); return 1; }
        if(!strcmp(name,"__classdict__")){ snprintf(out,n,"c_classdict"); return 1; }
        if(!strcmp(name,"__conditional_annotations__")){ snprintf(out,n,"c_condann"); return 1; }
        int k=free_index(s,name); if(k>=0){ snprintf(out,n,"PyTuple_GET_ITEM(env->cells,%d)",k); return 1; }
        return 0;
    }
    int i=sym_index(s,name);
    if(i>=0 && s->syms[i].res==R_CELL){ snprintf(out,n,"c%d_%d",s->id,i); return 1; }
    if(s->kind==SC_FUNCTION && !strcmp(name,"__class__") && s->needs_class_cell){ snprintf(out,n,"c_class"); return 1; }
    if(i>=0 && s->syms[i].res==R_FREE){
        if(is_inline(f,s)) return cell_expr(f,s->parent,name,out,n);
        int k=free_index(s,name); if(k<0) return 0;
        snprintf(out,n,"PyTuple_GET_ITEM(env->cells,%d)",k); return 1;
    }
    if(i<0 && is_inline(f,s)) return cell_expr(f,s->parent,name,out,n);
    if(i<0){ int k=free_index(s,name); if(k>=0){ snprintf(out,n,"PyTuple_GET_ITEM(env->cells,%d)",k); return 1; } }
    return 0;
}
static NameRef name_ref(Fn *f, CScope *s, const char *name){
    NameRef r; memset(&r,0,sizeof r);
    if(s->kind==SC_MODULE){ r.k=NR_GLOBAL; return r; }
    int i=sym_index(s,name);
    CRes res= i>=0 ? s->syms[i].res : R_GLOBAL_IMPLICIT;
    if(s->kind==SC_CLASS){
        if(res==R_GLOBAL_EXPLICIT){ r.k=NR_GLOBAL; return r; }
        if(res==R_FREE && !(s->syms[i].flags&DF_FREE_PASS)){ r.k=NR_CLASSDEREF; cell_expr(f,s,name,r.var,sizeof r.var); return r; }
        r.k=NR_NAME; return r;
    }
    switch(res){
        case R_LOCAL: r.k=NR_LOCAL; snprintf(r.var,sizeof r.var,"v%d_%d",s->id,i); return r;
        case R_CELL: r.k=NR_CELL; snprintf(r.var,sizeof r.var,"c%d_%d",s->id,i); return r;
        case R_FREE:
            if(is_inline(f,s)){
                NameRef p=name_ref(f,s->parent,name);
                if(p.k==NR_NAME || p.k==NR_CLASSDEREF){ p.k=NR_CELL; p.free=1; if(!cell_expr(f,s->parent,name,p.var,sizeof p.var)) gfail(f->g,f->line,"no cell for %s",name); }
                return p;
            }
            r.k=NR_CELL; r.free=1; snprintf(r.var,sizeof r.var,"PyTuple_GET_ITEM(env->cells,%d)",free_index(s,name)); return r;
        default: r.k=NR_GLOBAL; return r;
    }
}
static char *mangled(CScope *s, const char *name){ return capi_mangle(s->private_name,name); }

/* load: -> a temp */
static int load_name(Fn *f, CScope *s, const char *raw){
    int t=tmp(f);
    if(!strcmp(raw,"__debug__")){ E(f,"t%d=Py_NewRef(Py_True);",t); return t; }
    char *nm=mangled(s,raw); int k=k_str(f->g,nm);
    NameRef r=name_ref(f,s,nm); free(nm);
    switch(r.k){
        case NR_LOCAL: E(f,"if(!%s){ mpy_unbound(K[%d]); goto %s; } t%d=Py_NewRef(%s);",r.var,k,ERR(f),t,r.var); break;
        case NR_CELL: E(f,"if(!(t%d=mpy_load_cell(%s,K[%d],%d))) goto %s;",t,r.var,k,r.free,ERR(f)); break;
        case NR_GLOBAL:{
            int c=f->g->nsites++;                    /* a per-site cache */
            xb_f(&f->g->kdata,"static MpyGCache GC%d;\n",c);
            E(f,"if(!(t%d=mpy_load_global_cached(G,K[%d],&GC%d))) goto %s;",t,k,c,ERR(f)); break; }
        case NR_NAME: E(f,"if(!(t%d=mpy_load_name(NS,G,K[%d]))) goto %s;",t,k,ERR(f)); break;
        case NR_CLASSDEREF: E(f,"if(!(t%d=mpy_load_classderef(NS,%s,K[%d]))) goto %s;",t,r.var,k,ERR(f)); break;
    }
    return t;
}
/* store: consumes temp t */
static void store_name(Fn *f, CScope *s, const char *raw, int t){
    char *nm=mangled(s,raw); int k=k_str(f->g,nm);
    NameRef r=name_ref(f,s,nm); free(nm);
    switch(r.k){
        case NR_LOCAL: E(f,"Py_XSETREF(%s,t%d); t%d=NULL;",r.var,t,t); break;
        case NR_CELL: E(f,"PyCell_Set(%s,t%d); Py_CLEAR(t%d);",r.var,t,t); break;
        case NR_GLOBAL: E(f,"if(PyDict_SetItem(G,K[%d],t%d)){ Py_CLEAR(t%d); goto %s; } Py_CLEAR(t%d);",k,t,t,ERR(f),t); break;
        case NR_NAME: case NR_CLASSDEREF: E(f,"if(mpy_store_name(NS,K[%d],t%d)){ Py_CLEAR(t%d); goto %s; } Py_CLEAR(t%d);",k,t,t,ERR(f),t); break;
    }
}
static void delete_name(Fn *f, CScope *s, const char *raw){
    char *nm=mangled(s,raw); int k=k_str(f->g,nm);
    NameRef r=name_ref(f,s,nm); free(nm);
    switch(r.k){
        case NR_LOCAL: E(f,"if(!%s){ mpy_unbound(K[%d]); goto %s; } Py_CLEAR(%s);",r.var,k,ERR(f),r.var); break;
        case NR_CELL: E(f,"{ PyObject *o_=PyCell_Get(%s); if(!o_){ if(%d) PyErr_Format(PyExc_NameError,\"cannot access free variable '%%U' where it is not associated with a value in enclosing scope\",K[%d]); else mpy_unbound(K[%d]); goto %s; } Py_DECREF(o_); PyCell_Set(%s,NULL); }",r.var,r.free,k,k,ERR(f),r.var); break;
        case NR_GLOBAL: E(f,"if(mpy_delete_global(G,K[%d])) goto %s;",k,ERR(f)); break;
        case NR_NAME: case NR_CLASSDEREF: E(f,"if(mpy_delete_name(NS,K[%d])) goto %s;",k,ERR(f)); break;
    }
}

/* ---------------------------------------------------------------- expressions */
static int ex(Fn *f, CScope *s, PyNode *e);
static void stmts(Fn *f, CScope *s, PyList *l);
static int make_function(Fn *f, CScope *s, PyNode *def);
static void store_target(Fn *f, CScope *s, PyNode *t, int v);

static int ex_const(Fn *f, PyConst *c){
    int t=tmp(f);
    switch(c->kind){
        case PC_None: E(f,"t%d=Py_NewRef(Py_None);",t); break;
        case PC_True: E(f,"t%d=Py_NewRef(Py_True);",t); break;
        case PC_False: E(f,"t%d=Py_NewRef(Py_False);",t); break;
        case PC_Ellipsis: E(f,"t%d=Py_NewRef(Py_Ellipsis);",t); break;
        default: E(f,"t%d=Py_NewRef(K[%d]);",t,k_const(f->g,c)); break;
    }
    return t;
}
static const char *binop_fn(int op, int inplace){
    static const char *n[]={NULL,"Add","Subtract","Multiply","MatrixMultiply","TrueDivide","Remainder","Power","Lshift","Rshift","Or","Xor","And","FloorDivide"};
    static char b[64];
    if(op<OP_Add||op>OP_FloorDiv) return NULL;
    snprintf(b,sizeof b,"PyNumber_%s%s",inplace?"InPlace":"",n[op]);
    return b;
}
static int binop(Fn *f, int op, int a, int b, int inplace){
    int t=tmp(f);
    const char *fn=binop_fn(op,inplace);
    /* small-int fast paths (capi_rt.h) */
    if(op==OP_Add) fn=inplace?"mpy_iadd":"mpy_add";
    else if(op==OP_Sub) fn=inplace?"mpy_isub":"mpy_sub";
    else if(op==OP_Mult && !inplace) fn="mpy_mul";
    else if(op==OP_Mod && !inplace) fn="mpy_mod";
    if(op==OP_Pow) E(f,"t%d=%s(t%d,t%d,Py_None); Py_CLEAR(t%d); Py_CLEAR(t%d); if(!t%d) goto %s;",t,fn,a,b,a,b,t,ERR(f));
    else E(f,"t%d=%s(t%d,t%d); Py_CLEAR(t%d); Py_CLEAR(t%d); if(!t%d) goto %s;",t,fn,a,b,a,b,t,ERR(f));
    return t;
}
static int ivar(Fn *f){ int k=f->nctx++; xb_f(&f->decl,"    int i%d=0;\n",k); return k; }
static int cmp_code(int op){
    switch(op){ case OP_Lt: return 0; case OP_LtE: return 1; case OP_Eq: return 2; case OP_NotEq: return 3; case OP_Gt: return 4; case OP_GtE: return 5;
                case OP_In: return 6; case OP_NotIn: return 7; case OP_Is: return 8; default: return 9; }
}
static int ex_compare(Fn *f, CScope *s, PyNode *e){
    int left=ex(f,s,e->n[0]);
    int n=e->L[0].n, res=tmp(f), done=lab(f), iv=ivar(f);
    for(int i=0;i<n;i++){
        int right=ex(f,s,e->L[1].v[i]);
        int code=cmp_code(e->L[0].v[i]->op);
        const char *cf= code<=5 ? "mpy_richcmp" : "mpy_compare";
        if(i==n-1){
            E(f,"t%d=%s(t%d,t%d,%d); Py_CLEAR(t%d); Py_CLEAR(t%d); if(!t%d) goto %s;",res,cf,left,right,code,left,right,res,ERR(f));
        } else {
            E(f,"t%d=%s(t%d,t%d,%d); Py_CLEAR(t%d); if(!t%d){ Py_CLEAR(t%d); goto %s; }",res,cf,left,right,code,left,res,right,ERR(f));
            E(f,"i%d=mpy_truth(t%d); if(i%d<0){ Py_CLEAR(t%d); goto %s; }",iv,res,iv,right,ERR(f));
            E(f,"if(!i%d){ Py_CLEAR(t%d); goto L%d; }",iv,right,done);
            E(f,"Py_CLEAR(t%d); t%d=t%d; t%d=NULL;",res,left,right,right);
        }
    }
    LBL(f,done);
    return res;
}
static int ex_boolop(Fn *f, CScope *s, PyNode *e){
    int res=tmp(f), done=lab(f), iv=ivar(f);
    for(int i=0;i<e->L[0].n;i++){
        int v=ex(f,s,e->L[0].v[i]);
        E(f,"t%d=t%d; t%d=NULL;",res,v,v);
        if(i==e->L[0].n-1) break;
        E(f,"i%d=mpy_truth(t%d); if(i%d<0) goto %s;",iv,res,iv,ERR(f));
        E(f,"if(%si%d) goto L%d;",e->op==OP_Or?"":"!",iv,done);
        E(f,"Py_CLEAR(t%d);",res);
    }
    LBL(f,done);
    return res;
}
/* an array of argument temps: declares PyObject *a<k>[n+1] (slot 0 free for PY_VECTORCALL_ARGUMENTS_OFFSET) */
static int argarray(Fn *f, int n){ int k=f->nctx++; xb_f(&f->decl,"    PyObject *a%d[%d];\n",k,n+2); return k; }

static int ex_call_star(Fn *f, CScope *s, PyNode *e, int fn){
    int lst=tmp(f), dict=tmp(f), t=tmp(f);
    E(f,"if(!(t%d=PyList_New(0))) goto %s;",lst,ERR(f));
    for(int i=0;i<e->L[0].n;i++){
        PyNode *a=e->L[0].v[i];
        if(a->kind==PK_Starred){ int v=ex(f,s,a->n[0]); E(f,"if(mpy_args_extend(t%d,t%d)){ Py_CLEAR(t%d); goto %s; } Py_CLEAR(t%d);",lst,v,v,ERR(f),v); }
        else { int v=ex(f,s,a); E(f,"if(PyList_Append(t%d,t%d)){ Py_CLEAR(t%d); goto %s; } Py_CLEAR(t%d);",lst,v,v,ERR(f),v); }
    }
    if(e->L[1].n){
        E(f,"if(!(t%d=PyDict_New())) goto %s;",dict,ERR(f));
        for(int i=0;i<e->L[1].n;i++){
            PyNode *kw=e->L[1].v[i]; int v=ex(f,s,kw->n[0]);
            if(kw->id[0]){
                int kn=k_str(f->g,kw->id[0]); int one=tmp(f);
                E(f,"if(!(t%d=PyDict_New())){ Py_CLEAR(t%d); goto %s; } if(PyDict_SetItem(t%d,K[%d],t%d)){ Py_CLEAR(t%d); goto %s; } Py_CLEAR(t%d);",one,v,ERR(f),one,kn,v,v,ERR(f),v);
                E(f,"if(mpy_kwargs_merge(t%d,t%d,t%d)){ Py_CLEAR(t%d); goto %s; } Py_CLEAR(t%d);",dict,one,fn,one,ERR(f),one);
            } else E(f,"if(mpy_kwargs_merge(t%d,t%d,t%d)){ Py_CLEAR(t%d); goto %s; } Py_CLEAR(t%d);",dict,v,fn,v,ERR(f),v);
        }
    }
    E(f,"t%d=mpy_call_ex(t%d,t%d,t%d); Py_CLEAR(t%d); Py_CLEAR(t%d); Py_CLEAR(t%d); if(!t%d) goto %s;",t,fn,lst,dict,fn,lst,dict,t,ERR(f));
    return t;
}
static int has_star_args(PyNode *e){
    for(int i=0;i<e->L[0].n;i++) if(e->L[0].v[i]->kind==PK_Starred) return 1;
    for(int i=0;i<e->L[1].n;i++) if(!e->L[1].v[i]->id[0]) return 1;
    return 0;
}
/* the first parameter of the function a zero-argument super() is in */
static CScope *enclosing_function(Fn *f, CScope *s){ (void)f; while(s && s->is_comprehension) s=s->parent; return s; }
static int ex_call(Fn *f, CScope *s, PyNode *e){
    PyNode *fnode=e->n[0];
    int n=e->L[0].n, nk=e->L[1].n;
    if(fnode->kind==PK_Name && !strcmp(fnode->id[0],"super") && !n && !nk){
        /* super(): the method's class (its __class__ cell) and first argument; CPython finds them in the frame */
        CScope *fs=enclosing_function(f,s);
        char cell[96];
        if(fs && fs->kind==SC_FUNCTION && fs->node && fs->node->n[0] && cell_expr(f,s,"__class__",cell,sizeof cell)){
            PyNode *a=fs->node->n[0];
            PyNode *first= a->L[0].n ? a->L[0].v[0] : a->L[1].n ? a->L[1].v[0] : NULL;
            if(first){
                int sup=load_name(f,s,"super"), self=load_name(f,fs,first->id[0]), t=tmp(f), cls=tmp(f);
                E(f,"if(t%d==(PyObject*)&PySuper_Type){ if(!(t%d=PyCell_Get(%s))){ PyErr_SetString(PyExc_RuntimeError,\"super(): empty __class__ cell\"); Py_CLEAR(t%d); Py_CLEAR(t%d); goto %s; }",sup,cls,cell,sup,self,ERR(f));
                E(f,"  t%d=PyObject_CallFunctionObjArgs(t%d,t%d,t%d,NULL); Py_CLEAR(t%d); }",t,sup,cls,self,cls);
                E(f,"else t%d=PyObject_CallNoArgs(t%d);",t,sup);
                E(f,"Py_CLEAR(t%d); Py_CLEAR(t%d); if(!t%d) goto %s;",sup,self,t,ERR(f));
                return t;
            }
        }
    }
    if(has_star_args(e)){ int fn=ex(f,s,fnode); return ex_call_star(f,s,e,fn); }
    char **kwn=(char**)xmalloc(sizeof(char*)*(size_t)(nk+1));
    for(int i=0;i<nk;i++) kwn[i]=e->L[1].v[i]->id[0];
    int kt= nk ? k_names(f->g,kwn,nk) : -1;
    free(kwn);
    char kwexpr[32]; if(kt>=0) snprintf(kwexpr,sizeof kwexpr,"K[%d]",kt); else snprintf(kwexpr,sizeof kwexpr,"NULL");
    int t=tmp(f);
    if(fnode->kind==PK_Attribute){
        int obj=ex(f,s,fnode->n[0]);
        char *an=mangled(s,fnode->id[0]); int ka=k_str(f->g,an); free(an);
        int meth=tmp(f), sv=ivar(f);
        E(f,"i%d=_PyObject_GetMethod(t%d,K[%d],&t%d); if(!t%d){ Py_CLEAR(t%d); goto %s; }",sv,obj,ka,meth,meth,obj,ERR(f));
        E(f,"if(!i%d) Py_CLEAR(t%d);",sv,obj);
        int *av=(int*)xmalloc(sizeof(int)*(size_t)(n+nk+1));
        for(int i=0;i<n;i++) av[i]=ex(f,s,e->L[0].v[i]);
        for(int i=0;i<nk;i++) av[n+i]=ex(f,s,e->L[1].v[i]->n[0]);
        int arr=argarray(f,n+nk);
        for(int i=0;i<n+nk;i++) E(f,"a%d[%d]=t%d;",arr,i+2,av[i]);
        E(f,"a%d[1]=t%d;",arr,obj);
        E(f,"if(i%d) t%d=mpy_vcall(t%d,a%d+1,%d|PY_VECTORCALL_ARGUMENTS_OFFSET,%s);",sv,t,meth,arr,n+1,kwexpr);
        E(f,"else t%d=mpy_vcall(t%d,a%d+2,%d|PY_VECTORCALL_ARGUMENTS_OFFSET,%s);",t,meth,arr,n,kwexpr);
        xb_put(&f->b,"    ",4);
        xb_f(&f->b,"Py_CLEAR(t%d); Py_CLEAR(t%d);",meth,obj);
        for(int i=0;i<n+nk;i++) xb_f(&f->b," Py_CLEAR(t%d);",av[i]);
        xb_f(&f->b," if(!t%d) goto %s;\n",t,ERR(f));
        free(av);
        return t;
    }
    int fn=ex(f,s,fnode);
    int *av=(int*)xmalloc(sizeof(int)*(size_t)(n+nk+1));
    for(int i=0;i<n;i++) av[i]=ex(f,s,e->L[0].v[i]);
    for(int i=0;i<nk;i++) av[n+i]=ex(f,s,e->L[1].v[i]->n[0]);
    int arr=argarray(f,n+nk);
    for(int i=0;i<n+nk;i++) E(f,"a%d[%d]=t%d;",arr,i+1,av[i]);
    E(f,"t%d=mpy_vcall(t%d,a%d+1,%d|PY_VECTORCALL_ARGUMENTS_OFFSET,%s);",t,fn,arr,n,kwexpr);
    xb_put(&f->b,"    ",4);
    xb_f(&f->b,"Py_CLEAR(t%d);",fn);
    for(int i=0;i<n+nk;i++) xb_f(&f->b," Py_CLEAR(t%d);",av[i]);
    xb_f(&f->b," if(!t%d) goto %s;\n",t,ERR(f));
    free(av);
    return t;
}

/* displays: list / tuple / set (with *unpacking) */
static int ex_seq(Fn *f, CScope *s, PyList *elts, int kind){
    int star=0; for(int i=0;i<elts->n;i++) if(elts->v[i]->kind==PK_Starred) star=1;
    int t=tmp(f);
    if(kind==PK_Set){
        E(f,"if(!(t%d=PySet_New(NULL))) goto %s;",t,ERR(f));
        for(int i=0;i<elts->n;i++){
            PyNode *x=elts->v[i];
            if(x->kind==PK_Starred){ int v=ex(f,s,x->n[0]); E(f,"if(mpy_set_update(t%d,t%d)){ Py_CLEAR(t%d); goto %s; } Py_CLEAR(t%d);",t,v,v,ERR(f),v); }
            else { int v=ex(f,s,x); E(f,"if(PySet_Add(t%d,t%d)){ Py_CLEAR(t%d); goto %s; } Py_CLEAR(t%d);",t,v,v,ERR(f),v); }
        }
        return t;
    }
    if(!star){
        int *v=(int*)xmalloc(sizeof(int)*(size_t)(elts->n+1));
        for(int i=0;i<elts->n;i++) v[i]=ex(f,s,elts->v[i]);
        E(f,"if(!(t%d=Py%s_New(%d))) goto %s;",t,kind==PK_List?"List":"Tuple",elts->n,ERR(f));
        for(int i=0;i<elts->n;i++) E(f,"Py%s_SET_ITEM(t%d,%d,t%d); t%d=NULL;",kind==PK_List?"List":"Tuple",t,i,v[i],v[i]);
        free(v);
        return t;
    }
    E(f,"if(!(t%d=PyList_New(0))) goto %s;",t,ERR(f));
    for(int i=0;i<elts->n;i++){
        PyNode *x=elts->v[i];
        if(x->kind==PK_Starred){ int v=ex(f,s,x->n[0]); E(f,"if(mpy_list_extend(t%d,t%d)){ Py_CLEAR(t%d); goto %s; } Py_CLEAR(t%d);",t,v,v,ERR(f),v); }
        else { int v=ex(f,s,x); E(f,"if(PyList_Append(t%d,t%d)){ Py_CLEAR(t%d); goto %s; } Py_CLEAR(t%d);",t,v,v,ERR(f),v); }
    }
    if(kind==PK_Tuple){ int r=tmp(f); E(f,"t%d=PyList_AsTuple(t%d); Py_CLEAR(t%d); if(!t%d) goto %s;",r,t,t,r,ERR(f)); return r; }
    return t;
}
static int ex_dict(Fn *f, CScope *s, PyNode *e){
    int t=tmp(f);
    E(f,"if(!(t%d=PyDict_New())) goto %s;",t,ERR(f));
    for(int i=0;i<e->L[0].n;i++){
        PyNode *k=e->L[0].v[i], *v=e->L[1].v[i];
        if(!k){ int m=ex(f,s,v); E(f,"if(mpy_dict_update(t%d,t%d)){ Py_CLEAR(t%d); goto %s; } Py_CLEAR(t%d);",t,m,m,ERR(f),m); continue; }
        int kt=ex(f,s,k), vt=ex(f,s,v);
        E(f,"if(PyDict_SetItem(t%d,t%d,t%d)){ Py_CLEAR(t%d); Py_CLEAR(t%d); goto %s; } Py_CLEAR(t%d); Py_CLEAR(t%d);",t,kt,vt,kt,vt,ERR(f),kt,vt);
    }
    return t;
}
static int ex_joined(Fn *f, CScope *s, PyNode *e){
    int n=e->L[0].n;
    if(n==0){ int t=tmp(f); int k=k_str(f->g,""); E(f,"t%d=Py_NewRef(K[%d]);",t,k); return t; }
    int *parts=(int*)xmalloc(sizeof(int)*(size_t)n);
    for(int i=0;i<n;i++){
        PyNode *p=e->L[0].v[i];
        if(p->kind==PK_Constant){ parts[i]=ex_const(f,p->k); continue; }
        int v=ex(f,s,p->n[0]);
        int spec= p->n[1] ? ex_joined(f,s,p->n[1]) : -1;
        int r=tmp(f);
        if(spec>=0) E(f,"t%d=mpy_format(t%d,%d,t%d); Py_CLEAR(t%d); Py_CLEAR(t%d); if(!t%d) goto %s;",r,v,p->op,spec,v,spec,r,ERR(f));
        else E(f,"t%d=mpy_format(t%d,%d,NULL); Py_CLEAR(t%d); if(!t%d) goto %s;",r,v,p->op,v,r,ERR(f));
        parts[i]=r;
    }
    if(n==1) { int r=parts[0]; free(parts); return r; }
    int arr=argarray(f,n), t=tmp(f);
    for(int i=0;i<n;i++) E(f,"a%d[%d]=t%d;",arr,i,parts[i]);
    xb_put(&f->b,"    ",4);
    xb_f(&f->b,"t%d=mpy_join(a%d,%d);",t,arr,n);
    for(int i=0;i<n;i++) xb_f(&f->b," Py_CLEAR(t%d);",parts[i]);
    xb_f(&f->b," if(!t%d) goto %s;\n",t,ERR(f));
    free(parts);
    return t;
}
static int ex_slice(Fn *f, CScope *s, PyNode *e){
    int lo= e->n[0]?ex(f,s,e->n[0]):-1, hi= e->n[1]?ex(f,s,e->n[1]):-1, st= e->n[2]?ex(f,s,e->n[2]):-1;
    int t=tmp(f);
    char a[16],b[16],c[16];
    if(lo>=0) snprintf(a,16,"t%d",lo); else snprintf(a,16,"Py_None");
    if(hi>=0) snprintf(b,16,"t%d",hi); else snprintf(b,16,"Py_None");
    if(st>=0) snprintf(c,16,"t%d",st); else snprintf(c,16,"Py_None");
    E(f,"t%d=PySlice_New(%s,%s,%s);",t,a,b,c);
    if(lo>=0) E(f,"Py_CLEAR(t%d);",lo);
    if(hi>=0) E(f,"Py_CLEAR(t%d);",hi);
    if(st>=0) E(f,"Py_CLEAR(t%d);",st);
    E(f,"if(!t%d) goto %s;",t,ERR(f));
    return t;
}
static int ex_sub_index(Fn *f, CScope *s, PyNode *e){ return e->kind==PK_Slice ? ex_slice(f,s,e) : ex(f,s,e); }

/* comprehensions, inlined: c = the comprehension's scope */
static void comp_loops(Fn *f, CScope *c, PyNode *e, int gi, int acc, int first_iter);
static void comp_declare(Fn *f, CScope *c){
    for(int i=0;i<f->ncomps;i++) if(f->comps[i]==c) return;
    f->comps=(CScope**)xrealloc(f->comps,sizeof(CScope*)*(size_t)(f->ncomps+1)); f->comps[f->ncomps++]=c;
}
static int ex_comprehension(Fn *f, CScope *s, PyNode *e){
    CScope *c=capi_scope_of(&f->g->st,e);
    if(!c) gfail(f->g,e->line,"comprehension without a scope");
    comp_declare(f,c);
    int it0=ex(f,s,e->L[0].v[0]->n[1]);          /* the first iterable: in the enclosing scope */
    int acc=tmp(f);
    if(e->kind==PK_ListComp) E(f,"if(!(t%d=PyList_New(0))) goto %s;",acc,ERR(f));
    else if(e->kind==PK_SetComp) E(f,"if(!(t%d=PySet_New(NULL))) goto %s;",acc,ERR(f));
    else E(f,"if(!(t%d=PyDict_New())) goto %s;",acc,ERR(f));
    /* cells of the comprehension (a lambda in it captures its variables) */
    for(int i=0;i<c->nsyms;i++) if(c->syms[i].res==R_CELL) E(f,"Py_XSETREF(c%d_%d,PyCell_New(NULL)); if(!c%d_%d) goto %s;",c->id,i,c->id,i,ERR(f));
    comp_loops(f,c,e,0,acc,it0);
    /* its variables do not outlive it */
    for(int i=0;i<c->nsyms;i++){
        if(c->syms[i].res==R_LOCAL) E(f,"Py_CLEAR(v%d_%d);",c->id,i);
        if(c->syms[i].res==R_CELL) E(f,"Py_CLEAR(c%d_%d);",c->id,i);
    }
    return acc;
}
static void comp_loops(Fn *f, CScope *c, PyNode *e, int gi, int acc, int first_iter){
    PyList *gens=&e->L[0];
    if(gi==gens->n){
        if(e->kind==PK_DictComp){
            int k=ex(f,c,e->n[0]), v=ex(f,c,e->n[1]);
            E(f,"if(PyDict_SetItem(t%d,t%d,t%d)){ Py_CLEAR(t%d); Py_CLEAR(t%d); goto %s; } Py_CLEAR(t%d); Py_CLEAR(t%d);",acc,k,v,k,v,ERR(f),k,v);
        } else {
            int v=ex(f,c,e->n[0]);
            E(f,"if(%s(t%d,t%d)){ Py_CLEAR(t%d); goto %s; } Py_CLEAR(t%d);",e->kind==PK_ListComp?"PyList_Append":"PySet_Add",acc,v,v,ERR(f),v);
        }
        return;
    }
    PyNode *g=gens->v[gi];
    if(g->op) gfail(f->g,g->line,"async comprehension");
    int src= gi==0 ? first_iter : ex(f,c,g->n[1]);
    int it=tmp(f), item=tmp(f), top=lab(f), done=lab(f), iv=ivar(f);
    E(f,"t%d=PyObject_GetIter(t%d); Py_CLEAR(t%d); if(!t%d) goto %s;",it,src,src,it,ERR(f));
    LBL(f,top);
    E(f,"i%d=PyIter_NextItem(t%d,&t%d); if(i%d<0) goto %s; if(!i%d) goto L%d;",iv,it,item,iv,ERR(f),iv,done);
    store_target(f,c,g->n[0],item);
    for(int k=0;k<g->L[0].n;k++){                  /* if ...: on to the next item */
        int cv=ex(f,c,g->L[0].v[k]); int jv=ivar(f);
        E(f,"i%d=mpy_truth(t%d); Py_CLEAR(t%d); if(i%d<0) goto %s; if(!i%d) goto L%d;",jv,cv,cv,jv,ERR(f),jv,top);
    }
    comp_loops(f,c,e,gi+1,acc,-1);
    E(f,"goto L%d;",top);
    LBL(f,done);
    E(f,"Py_CLEAR(t%d);",it);
}

static int island_function(Fn *f, CScope *s, CScope *u, PyNode *node, int line);
static int ex_genexp(Fn *f, CScope *s, PyNode *e){
    CScope *c=capi_scope_of(&f->g->st,e);
    int it0=ex(f,s,e->L[0].v[0]->n[1]);
    int itr=tmp(f);
    E(f,"t%d=PyObject_GetIter(t%d); Py_CLEAR(t%d); if(!t%d) goto %s;",itr,it0,it0,itr,ERR(f));
    int fn=island_function(f,s,c,e,e->line);
    int t=tmp(f);
    E(f,"t%d=PyObject_CallOneArg(t%d,t%d); Py_CLEAR(t%d); Py_CLEAR(t%d); if(!t%d) goto %s;",t,fn,itr,fn,itr,t,ERR(f));
    return t;
}

static int ex(Fn *f, CScope *s, PyNode *e){
    switch(e->kind){
        case PK_Constant: return ex_const(f,e->k);
        case PK_Name: return load_name(f,s,e->id[0]);
        case PK_Attribute:{
            int o=ex(f,s,e->n[0]); char *an=mangled(s,e->id[0]); int k=k_str(f->g,an); free(an); int t=tmp(f);
            E(f,"t%d=PyObject_GetAttr(t%d,K[%d]); Py_CLEAR(t%d); if(!t%d) goto %s;",t,o,k,o,t,ERR(f));
            return t; }
        case PK_Subscript:{
            int o=ex(f,s,e->n[0]), i=ex_sub_index(f,s,e->n[1]), t=tmp(f);
            E(f,"t%d=PyObject_GetItem(t%d,t%d); Py_CLEAR(t%d); Py_CLEAR(t%d); if(!t%d) goto %s;",t,o,i,o,i,t,ERR(f));
            return t; }
        case PK_Call: return ex_call(f,s,e);
        case PK_BinOp:{ int a=ex(f,s,e->n[0]), b=ex(f,s,e->n[1]); return binop(f,e->op,a,b,0); }
        case PK_UnaryOp:{
            int v=ex(f,s,e->n[0]), t=tmp(f);
            if(e->op==OP_Not){ int iv=ivar(f); E(f,"i%d=mpy_truth(t%d); Py_CLEAR(t%d); if(i%d<0) goto %s; t%d=Py_NewRef(i%d?Py_False:Py_True);",iv,v,v,iv,ERR(f),t,iv); return t; }
            const char *fn= e->op==OP_USub?"PyNumber_Negative" : e->op==OP_UAdd?"PyNumber_Positive" : "PyNumber_Invert";
            E(f,"t%d=%s(t%d); Py_CLEAR(t%d); if(!t%d) goto %s;",t,fn,v,v,t,ERR(f));
            return t; }
        case PK_BoolOp: return ex_boolop(f,s,e);
        case PK_Compare: return ex_compare(f,s,e);
        case PK_IfExp:{
            int c=ex(f,s,e->n[0]), iv=ivar(f), t=tmp(f), other=lab(f), done=lab(f);
            E(f,"i%d=mpy_truth(t%d); Py_CLEAR(t%d); if(i%d<0) goto %s; if(!i%d) goto L%d;",iv,c,c,iv,ERR(f),iv,other);
            int a=ex(f,s,e->n[1]); E(f,"t%d=t%d; t%d=NULL; goto L%d;",t,a,a,done);
            LBL(f,other);
            int b=ex(f,s,e->n[2]); E(f,"t%d=t%d; t%d=NULL;",t,b,b);
            LBL(f,done);
            return t; }
        case PK_List: case PK_Tuple: return ex_seq(f,s,&e->L[0],e->kind);
        case PK_Set: return ex_seq(f,s,&e->L[0],PK_Set);
        case PK_Dict: return ex_dict(f,s,e);
        case PK_ListComp: case PK_SetComp: case PK_DictComp: return ex_comprehension(f,s,e);
        case PK_GeneratorExp: return ex_genexp(f,s,e);
        case PK_Lambda: return make_function(f,s,e);
        case PK_JoinedStr: return ex_joined(f,s,e);
        case PK_NamedExpr:{
            int v=ex(f,s,e->n[1]), c=tmp(f);
            E(f,"t%d=Py_NewRef(t%d);",c,v);
            store_name(f,s,e->n[0]->id[0],c);
            return v; }
        case PK_Slice: return ex_slice(f,s,e);
        case PK_Starred: gfail(f->g,e->line,"starred expression here");
        default: gfail(f->g,e->line,"%s is not compiled",py_kind_name(e->kind));
    }
    return -1;
}

/* ---------------------------------------------------------------- targets */
static void store_target(Fn *f, CScope *s, PyNode *t, int v){
    switch(t->kind){
        case PK_Name: store_name(f,s,t->id[0],v); return;
        case PK_Attribute:{
            int o=ex(f,s,t->n[0]); char *an=mangled(s,t->id[0]); int k=k_str(f->g,an); free(an);
            E(f,"if(PyObject_SetAttr(t%d,K[%d],t%d)){ Py_CLEAR(t%d); Py_CLEAR(t%d); goto %s; } Py_CLEAR(t%d); Py_CLEAR(t%d);",o,k,v,o,v,ERR(f),o,v);
            return; }
        case PK_Subscript:{
            int o=ex(f,s,t->n[0]), i=ex_sub_index(f,s,t->n[1]);
            E(f,"if(PyObject_SetItem(t%d,t%d,t%d)){ Py_CLEAR(t%d); Py_CLEAR(t%d); Py_CLEAR(t%d); goto %s; } Py_CLEAR(t%d); Py_CLEAR(t%d); Py_CLEAR(t%d);",o,i,v,o,i,v,ERR(f),o,i,v);
            return; }
        case PK_Tuple: case PK_List:{
            int n=t->L[0].n, star=-1;
            for(int i=0;i<n;i++) if(t->L[0].v[i]->kind==PK_Starred) star=i;
            int arr=argarray(f,n);
            if(star<0) E(f,"if(mpy_unpack(t%d,%d,a%d)){ Py_CLEAR(t%d); goto %s; } Py_CLEAR(t%d);",v,n,arr,v,ERR(f),v);
            else E(f,"if(mpy_unpack_ex(t%d,%d,%d,a%d)){ Py_CLEAR(t%d); goto %s; } Py_CLEAR(t%d);",v,star,n-star-1,arr,v,ERR(f),v);
            int *ts=(int*)xmalloc(sizeof(int)*(size_t)n);
            for(int i=0;i<n;i++){ ts[i]=tmp(f); E(f,"t%d=a%d[%d];",ts[i],arr,i); }
            for(int i=0;i<n;i++){
                PyNode *x=t->L[0].v[i];
                store_target(f,s,x->kind==PK_Starred?x->n[0]:x,ts[i]);
            }
            free(ts);
            return; }
        case PK_Starred: store_target(f,s,t->n[0],v); return;
        default: gfail(f->g,t->line,"cannot assign to %s",py_kind_name(t->kind));
    }
}
static void delete_target(Fn *f, CScope *s, PyNode *t){
    switch(t->kind){
        case PK_Name: delete_name(f,s,t->id[0]); return;
        case PK_Attribute:{
            int o=ex(f,s,t->n[0]); char *an=mangled(s,t->id[0]); int k=k_str(f->g,an); free(an);
            E(f,"if(PyObject_DelAttr(t%d,K[%d])){ Py_CLEAR(t%d); goto %s; } Py_CLEAR(t%d);",o,k,o,ERR(f),o);
            return; }
        case PK_Subscript:{
            int o=ex(f,s,t->n[0]), i=ex_sub_index(f,s,t->n[1]);
            E(f,"if(PyObject_DelItem(t%d,t%d)){ Py_CLEAR(t%d); Py_CLEAR(t%d); goto %s; } Py_CLEAR(t%d); Py_CLEAR(t%d);",o,i,o,i,ERR(f),o,i);
            return; }
        case PK_Tuple: case PK_List: for(int i=0;i<t->L[0].n;i++) delete_target(f,s,t->L[0].v[i]); return;
        default: gfail(f->g,t->line,"cannot delete %s",py_kind_name(t->kind));
    }
}

/* ---------------------------------------------------------------- statements */
/* leave contexts up to (not including) `until` for return / break / continue:
   handlers and withs are left right here; a finally takes over (why) */
enum { WHY_NONE, WHY_EXC, WHY_RETURN, WHY_BREAK, WHY_CONTINUE };
static void leave_handler(Fn *f, Cx *c){
    E(f,"PyErr_SetHandledException(hp%d); Py_CLEAR(hp%d); Py_CLEAR(hx%d);",c->id,c->id,c->id);
    if(c->exc_name){ int t=tmp(f); E(f,"t%d=Py_NewRef(Py_None);",t); store_name(f,c->s,c->exc_name,t); delete_name(f,c->s,c->exc_name); }
}
/* returns 1 when a finally took over */
static int unwind(Fn *f, Cx *until, int why, Cx **stopped){
    for(Cx *c=f->cx;c && c!=until;c=c->up){
        if(c->kind==CX_HANDLER) leave_handler(f,c);
        else if(c->kind==CX_WITH){
            int save=f->err; f->err=c->err;
            if(why==WHY_RETURN) E(f,"if(mpy_with_exit(wx%d,NULL)<0){ Py_CLEAR(wx%d); Py_CLEAR(rv); goto %s; } Py_CLEAR(wx%d);",c->id,c->id,ERR(f),c->id);
            else E(f,"if(mpy_with_exit(wx%d,NULL)<0){ Py_CLEAR(wx%d); goto %s; } Py_CLEAR(wx%d);",c->id,c->id,ERR(f),c->id);
            f->err=save;
        }
        else if(c->kind==CX_FINALLY){ E(f,"why%d=%d; goto L%d;",c->id,why,c->brk); *stopped=c; return 1; }
    }
    return 0;
}
static Cx *innermost_loop(Fn *f){ for(Cx *c=f->cx;c;c=c->up) if(c->kind==CX_LOOP) return c; return NULL; }
static void do_return(Fn *f, Cx *from){
    Cx *save=f->cx; f->cx=from; Cx *st=NULL;
    if(!unwind(f,NULL,WHY_RETURN,&st)) E(f,"goto L_ret;");
    f->cx=save;
}
static void do_jump(Fn *f, Cx *from, int why){
    Cx *save=f->cx; f->cx=from;
    Cx *loop=innermost_loop(f); Cx *st=NULL;
    if(!loop) gfail(f->g,f->line,"'%s' outside loop",why==WHY_BREAK?"break":"continue");
    if(!unwind(f,loop,why,&st)) E(f,"goto L%d;",why==WHY_BREAK?loop->brk:loop->cont);
    f->cx=save;
}

static void stmt(Fn *f, CScope *s, PyNode *n);
static void stmts(Fn *f, CScope *s, PyList *l){ for(int i=0;i<l->n;i++) stmt(f,s,l->v[i]); }

static int is_docstring(PyNode *st){ return st->kind==PK_Expr && st->n[0]->kind==PK_Constant && st->n[0]->k->kind==PC_Str; }

static void st_try(Fn *f, CScope *s, PyNode *n){
    int has_final=n->L[2].n>0;
    int fid=-1, fin=-1, fe=-1, done_all=lab(f);
    int outer_err=f->err;
    Cx fcx;
    if(has_final){
        fid=f->nctx++; fin=lab(f); fe=lab(f);
        xb_f(&f->decl,"    int why%d=0; PyObject *fx%d=NULL, *fp%d=NULL;\n",fid,fid,fid);
        memset(&fcx,0,sizeof fcx); fcx.kind=CX_FINALLY; fcx.up=f->cx; fcx.id=fid; fcx.brk=fin; fcx.err=outer_err; fcx.s=s;
        f->cx=&fcx; f->err=fe;
        E(f,"why%d=0;",fid);
    }
    int lo=f->ntemp;
    if(n->L[3].n){
        int hid=f->nctx++, handler=lab(f), after_else=lab(f), body_err=f->err;
        xb_f(&f->decl,"    PyObject *hx%d=NULL, *hp%d=NULL;\n",hid,hid);
        f->err=handler;
        stmts(f,s,&n->L[0]);
        f->err=body_err;
        E(f,"goto L%d;",after_else);
        LBL(f,handler);
        clr_range(f,lo,f->ntemp);
        E(f,"hx%d=PyErr_GetRaisedException(); hp%d=PyErr_GetHandledException(); PyErr_SetHandledException(hx%d);",hid,hid,hid);
        int restore=lab(f), done=lab(f);
        Cx hcx; memset(&hcx,0,sizeof hcx); hcx.kind=CX_HANDLER; hcx.up=f->cx; hcx.id=hid; hcx.err=body_err; hcx.s=s;
        for(int i=0;i<n->L[3].n;i++){
            PyNode *h=n->L[3].v[i]; int next=lab(f);
            set_line(f,h->line);
            if(h->n[0]){
                f->err=restore;
                int ty=ex(f,s,h->n[0]); int mv=ivar(f);
                E(f,"i%d=mpy_exc_matches(hx%d,t%d); Py_CLEAR(t%d); if(i%d<0) goto L%d; if(!i%d) goto L%d;",mv,hid,ty,ty,mv,restore,mv,next);
            }
            hcx.exc_name=h->id[0];
            if(h->id[0]){ int t=tmp(f); E(f,"t%d=Py_NewRef(hx%d);",t,hid); f->err=restore; store_name(f,s,h->id[0],t); }
            /* the body: errors leave the handler (restoring the exception state) */
            int herr=lab(f), skip=lab(f);
            f->cx=&hcx; f->err=herr;
            int hlo=f->ntemp;
            stmts(f,s,&h->L[0]);
            f->cx=hcx.up; f->err=body_err;
            leave_handler(f,&hcx);
            E(f,"goto L%d;",done);
            LBL(f,herr);
            clr_range(f,hlo,f->ntemp);
            { int save=f->err; f->err=body_err; E(f,"PyErr_SetHandledException(hp%d); Py_CLEAR(hp%d); Py_CLEAR(hx%d);",hid,hid,hid);
              if(h->id[0]){ char *nm=mangled(s,h->id[0]); NameRef r=name_ref(f,s,nm); int k=k_str(f->g,nm); free(nm);
                  /* `except E as e` deletes e however the handler ends (an error already pending: no new one) */
                  if(r.k==NR_LOCAL) E(f,"Py_CLEAR(%s);",r.var);
                  else if(r.k==NR_CELL) E(f,"PyCell_Set(%s,NULL);",r.var);
                  else if(r.k==NR_GLOBAL) E(f,"{ PyObject *e_=PyErr_GetRaisedException(); PyDict_DelItem(G,K[%d]); PyErr_Clear(); PyErr_SetRaisedException(e_); }",k);
                  else E(f,"{ PyObject *e_=PyErr_GetRaisedException(); PyObject_DelItem(NS,K[%d]); PyErr_Clear(); PyErr_SetRaisedException(e_); }",k); }
              E(f,"goto %s;",ERR(f)); f->err=save; }
            (void)skip;
            LBL(f,next);
            hcx.exc_name=NULL;
        }
        /* nothing matched: re-raise */
        f->err=body_err;
        E(f,"PyErr_SetHandledException(hp%d); Py_CLEAR(hp%d); PyErr_SetRaisedException(hx%d); hx%d=NULL; goto %s;",hid,hid,hid,hid,ERR(f));
        LBL(f,restore);           /* an error while matching */
        E(f,"PyErr_SetHandledException(hp%d); Py_CLEAR(hp%d); Py_CLEAR(hx%d); goto %s;",hid,hid,hid,ERR(f));
        LBL(f,after_else);
        stmts(f,s,&n->L[1]);
        LBL(f,done);
    } else stmts(f,s,&n->L[0]);
    if(has_final){
        f->cx=fcx.up; f->err=outer_err;
        E(f,"goto L%d;",fin);
        LBL(f,fe);                /* an exception: run the finally with it as the handled one */
        clr_range(f,lo,f->ntemp);
        E(f,"fx%d=PyErr_GetRaisedException(); fp%d=PyErr_GetHandledException(); PyErr_SetHandledException(fx%d); why%d=%d;",fid,fid,fid,fid,WHY_EXC);
        LBL(f,fin);
        int ferr=lab(f), flo=f->ntemp;
        f->err=ferr;
        stmts(f,s,&n->L[2]);
        f->err=outer_err;
        E(f,"switch(why%d){",fid);
        E(f,"case %d: PyErr_SetHandledException(fp%d); Py_CLEAR(fp%d); PyErr_SetRaisedException(fx%d); fx%d=NULL; goto %s;",WHY_EXC,fid,fid,fid,fid,ERR(f));
        E(f,"case %d:",WHY_RETURN); do_return(f,f->cx);
        if(innermost_loop(f)){ E(f,"case %d:",WHY_BREAK); do_jump(f,f->cx,WHY_BREAK); E(f,"case %d:",WHY_CONTINUE); do_jump(f,f->cx,WHY_CONTINUE); }
        E(f,"default: break; }");
        E(f,"goto L%d;",done_all);
        LBL(f,ferr);              /* an error in the finally: the pending exception / return is dropped */
        clr_range(f,flo,f->ntemp);
        E(f,"if(why%d==%d){ PyErr_SetHandledException(fp%d); Py_CLEAR(fp%d); Py_CLEAR(fx%d); } if(why%d==%d) Py_CLEAR(rv); goto %s;",fid,WHY_EXC,fid,fid,fid,fid,WHY_RETURN,ERR(f));
    }
    LBL(f,done_all);
}

static void st_with(Fn *f, CScope *s, PyNode *n, int item){
    if(item==n->L[3].n){ stmts(f,s,&n->L[0]); return; }
    PyNode *w=n->L[3].v[item];
    int mgr=ex(f,s,w->n[0]);
    int wid=f->nctx++; xb_f(&f->decl,"    PyObject *wx%d=NULL;\n",wid);
    int r=tmp(f);
    E(f,"t%d=mpy_with_enter(t%d,&wx%d,0); Py_CLEAR(t%d); if(!t%d) goto %s;",r,mgr,wid,mgr,r,ERR(f));
    int outer_err=f->err, werr=lab(f), done=lab(f);
    Cx wcx; memset(&wcx,0,sizeof wcx); wcx.kind=CX_WITH; wcx.up=f->cx; wcx.id=wid; wcx.err=outer_err; wcx.s=s;
    f->cx=&wcx; f->err=werr;
    int lo=f->ntemp;
    if(w->n[1]) store_target(f,s,w->n[1],r); else E(f,"Py_CLEAR(t%d);",r);
    st_with(f,s,n,item+1);
    f->cx=wcx.up; f->err=outer_err;
    E(f,"if(mpy_with_exit(wx%d,NULL)<0){ Py_CLEAR(wx%d); goto %s; } Py_CLEAR(wx%d); goto L%d;",wid,wid,ERR(f),wid,done);
    LBL(f,werr);
    clr_range(f,lo,f->ntemp);
    int ex_=f->nctx++; xb_f(&f->decl,"    PyObject *we%d=NULL, *wp%d=NULL; int wr%d=0;\n",ex_,ex_,ex_);
    E(f,"we%d=PyErr_GetRaisedException(); wp%d=PyErr_GetHandledException(); PyErr_SetHandledException(we%d);",ex_,ex_,ex_);
    E(f,"wr%d=mpy_with_exit(wx%d,we%d); PyErr_SetHandledException(wp%d); Py_CLEAR(wp%d); Py_CLEAR(wx%d);",ex_,wid,ex_,ex_,ex_,wid);
    E(f,"if(wr%d<0){ Py_CLEAR(we%d); goto %s; } if(!wr%d){ PyErr_SetRaisedException(we%d); we%d=NULL; goto %s; } Py_CLEAR(we%d);",ex_,ex_,ERR(f),ex_,ex_,ex_,ERR(f),ex_);
    LBL(f,done);
}

static void st_import(Fn *f, CScope *s, PyNode *n){
    for(int i=0;i<n->L[3].n;i++){
        PyNode *al=n->L[3].v[i];
        int kn=k_str(f->g,al->id[0]);
        int m=tmp(f);
        E(f,"if(!(t%d=mpy_import(G,K[%d],NULL,0))) goto %s;",m,kn,ERR(f));
        if(al->id[1]){
            /* import a.b.c as x: the submodule */
            const char *p=strchr(al->id[0],'.');
            while(p){
                const char *q=strchr(p+1,'.');
                char part[512]; size_t l=q?(size_t)(q-p-1):strlen(p+1); if(l>=sizeof part) l=sizeof part-1; memcpy(part,p+1,l); part[l]=0;
                int kp=k_str(f->g,part), t=tmp(f);
                E(f,"t%d=mpy_import_from(t%d,K[%d]); Py_CLEAR(t%d); if(!t%d) goto %s;",t,m,kp,m,t,ERR(f));
                m=t; p=q;
            }
            store_name(f,s,al->id[1],m);
        } else {
            char first[512]; const char *d=strchr(al->id[0],'.'); size_t l=d?(size_t)(d-al->id[0]):strlen(al->id[0]);
            if(l>=sizeof first) l=sizeof first-1; memcpy(first,al->id[0],l); first[l]=0;
            store_name(f,s,first,m);
        }
    }
}
static void st_importfrom(Fn *f, CScope *s, PyNode *n){
    int nn=n->L[3].n; char **names=(char**)xmalloc(sizeof(char*)*(size_t)nn);
    for(int i=0;i<nn;i++) names[i]=n->L[3].v[i]->id[0];
    int fl=k_names(f->g,names,nn); free(names);
    int kn=k_str(f->g,n->id[0]?n->id[0]:"");
    int m=tmp(f);
    E(f,"if(!(t%d=mpy_import(G,K[%d],K[%d],%d))) goto %s;",m,kn,fl,n->op,ERR(f));
    if(nn==1 && !strcmp(n->L[3].v[0]->id[0],"*")){
        E(f,"if(mpy_import_star(t%d,%s)){ Py_CLEAR(t%d); goto %s; } Py_CLEAR(t%d);",m,s->kind==SC_CLASS?"NS":"G",m,ERR(f),m);
        return;
    }
    for(int i=0;i<nn;i++){
        PyNode *al=n->L[3].v[i]; int k=k_str(f->g,al->id[0]); int t=tmp(f);
        E(f,"if(!(t%d=mpy_import_from(t%d,K[%d]))){ Py_CLEAR(t%d); goto %s; }",t,m,k,m,ERR(f));
        int save=f->err; int cl=lab(f), ok=lab(f);
        f->err=cl;
        store_name(f,s,al->id[1]?al->id[1]:al->id[0],t);
        f->err=save;
        E(f,"goto L%d;",ok); LBL(f,cl); E(f,"Py_CLEAR(t%d); goto %s;",m,ERR(f)); LBL(f,ok);
    }
    E(f,"Py_CLEAR(t%d);",m);
}

/* a module-level statement CPython compiles: its source text at its line, run with the module's globals */
static void island_stmt(Fn *f, PyNode *n){
    Gen *g=f->g;
    int k=f->g->nsites++;                         /* its compiled code, once */
    xb_f(&g->kdata,"static PyObject *XS%d;\n",k);
    XB src={0};
    for(int i=1;i<n->line;i++) xb_put(&src,"\n",1);
    size_t a=(size_t)n->soff, b=(size_t)n->eoff;
    /* from the start of its first line (decorators included) */
    while(a>0 && g->in->src[a-1]!='\n') a--;
    xb_put(&src,g->in->src+a,b-a); xb_put(&src,"\n",1);
    xb_f(&f->b,"    if(mpy_exec_source(&XS%d,",k);
    c_lit(&f->b,(const unsigned char*)src.s,src.len);
    xb_f(&f->b,",MODFILE,%d,G,G)) goto %s;\n",g->future_ann?0x1000000:0,ERR(f));
    free(src.s);
    g->nislands++;
}
static void stmt(Fn *f, CScope *s, PyNode *n){
    set_line(f,n->line);
    if(n->island){ island_stmt(f,n); return; }
    switch(n->kind){
        case PK_Expr:{
            if(n->n[0]->kind==PK_Constant) return;     /* a docstring / bare constant: nothing */
            int t=ex(f,s,n->n[0]); E(f,"Py_CLEAR(t%d);",t); return; }
        case PK_Assign:{
            int v=ex(f,s,n->n[0]);
            for(int i=0;i<n->L[0].n;i++){
                int c=v;
                if(i<n->L[0].n-1){ c=tmp(f); E(f,"t%d=Py_NewRef(t%d);",c,v); }
                store_target(f,s,n->L[0].v[i],c);
            }
            return; }
        case PK_AugAssign:{
            PyNode *t=n->n[0];
            if(t->kind==PK_Name){
                int old=load_name(f,s,t->id[0]); int v=ex(f,s,n->n[1]);
                int r=binop(f,n->op,old,v,1);
                store_name(f,s,t->id[0],r);
            } else if(t->kind==PK_Attribute){
                int o=ex(f,s,t->n[0]); char *an=mangled(s,t->id[0]); int k=k_str(f->g,an); free(an);
                int old=tmp(f);
                E(f,"if(!(t%d=PyObject_GetAttr(t%d,K[%d]))){ Py_CLEAR(t%d); goto %s; }",old,o,k,o,ERR(f));
                int v=ex(f,s,n->n[1]); int r=binop(f,n->op,old,v,1);
                E(f,"if(PyObject_SetAttr(t%d,K[%d],t%d)){ Py_CLEAR(t%d); Py_CLEAR(t%d); goto %s; } Py_CLEAR(t%d); Py_CLEAR(t%d);",o,k,r,o,r,ERR(f),o,r);
            } else if(t->kind==PK_Subscript){
                int o=ex(f,s,t->n[0]), i=ex_sub_index(f,s,t->n[1]), old=tmp(f);
                E(f,"if(!(t%d=PyObject_GetItem(t%d,t%d))) goto %s;",old,o,i,ERR(f));
                int v=ex(f,s,n->n[1]); int r=binop(f,n->op,old,v,1);
                E(f,"if(PyObject_SetItem(t%d,t%d,t%d)){ Py_CLEAR(t%d); Py_CLEAR(t%d); Py_CLEAR(t%d); goto %s; } Py_CLEAR(t%d); Py_CLEAR(t%d); Py_CLEAR(t%d);",o,i,r,o,i,r,ERR(f),o,i,r);
            } else gfail(f->g,n->line,"augmented assignment target");
            return; }
        case PK_AnnAssign:{
            PyNode *t=n->n[0];
            if(n->n[2]){ int v=ex(f,s,n->n[2]); store_target(f,s,t,v); }
            else if(t->kind!=PK_Name){                /* x.a: T / x[i]: T: the target's parts are evaluated */
                if(t->kind==PK_Attribute){ int o=ex(f,s,t->n[0]); E(f,"Py_CLEAR(t%d);",o); }
                else if(t->kind==PK_Subscript){ int o=ex(f,s,t->n[0]); int i=ex_sub_index(f,s,t->n[1]); E(f,"Py_CLEAR(t%d); Py_CLEAR(t%d);",o,i); }
            }
            if(n->op && (s->kind==SC_MODULE || s->kind==SC_CLASS) && f->g->future_ann){
                /* __annotations__[name] = 'the annotation, as text' (CPython's text) */
                char *nm=mangled(s,t->id[0]); const char *text=NULL;
                CpyMeta *m=f->g->in->meta;
                for(int i=0;i<m->nanns;i++) if(!strcmp(m->anns[i].name,nm) && (m->anns[i].line==n->n[1]->line || m->anns[i].line==n->line)){ text=m->anns[i].text; break; }
                if(!text){                              /* CPython removed it: dead code (`if False:`) */
                    E(f,"PyErr_SetString(PyExc_SystemError,\"minipy: no annotation text (line %d)\"); goto %s;",n->line,ERR(f));
                    free(nm); return;
                }
                int ann=load_name(f,s,"__annotations__"), v=tmp(f);
                E(f,"t%d=Py_NewRef(K[%d]);",v,k_str(f->g,text));
                E(f,"if(PyObject_SetItem(t%d,K[%d],t%d)){ Py_CLEAR(t%d); Py_CLEAR(t%d); goto %s; } Py_CLEAR(t%d); Py_CLEAR(t%d);",ann,k_str(f->g,nm),v,ann,v,ERR(f),ann,v);
                free(nm);
            }
            else if(n->op && (s->kind==SC_MODULE || s->kind==SC_CLASS) && n->aux2){
                /* a module annotation / a conditional class one: __annotate__ looks in __conditional_annotations__ */
                int k=k_int(f->g,n->aux1);
                if(s->kind==SC_MODULE) E(f,"{ PyObject *ca_=mpy_load_global(G,K[%d]); if(!ca_) goto %s; int e_=PySet_Add(ca_,K[%d]); Py_DECREF(ca_); if(e_) goto %s; }",k_str(f->g,"__conditional_annotations__"),ERR(f),k,ERR(f));
                else E(f,"if(c_condann && PySet_Add(PyCell_GET(c_condann),K[%d])) goto %s;",k,ERR(f));
            }
            return; }
        case PK_Delete: for(int i=0;i<n->L[0].n;i++) delete_target(f,s,n->L[0].v[i]); return;
        case PK_Pass: return;
        case PK_Return:{
            if(n->n[0]){ int v=ex(f,s,n->n[0]); E(f,"Py_XSETREF(rv,t%d); t%d=NULL;",v,v); }
            else E(f,"Py_XSETREF(rv,Py_NewRef(Py_None));");
            do_return(f,f->cx);
            return; }
        case PK_If:{
            int c=ex(f,s,n->n[0]), iv=ivar(f), other=lab(f), done=lab(f);
            E(f,"i%d=mpy_truth(t%d); Py_CLEAR(t%d); if(i%d<0) goto %s; if(!i%d) goto L%d;",iv,c,c,iv,ERR(f),iv,other);
            stmts(f,s,&n->L[0]);
            if(n->L[1].n){ E(f,"goto L%d;",done); LBL(f,other); stmts(f,s,&n->L[1]); LBL(f,done); }
            else LBL(f,other);
            return; }
        case PK_While:{
            int top=lab(f), brk=lab(f), orelse=lab(f), iv=ivar(f);
            LBL(f,top);
            int c=ex(f,s,n->n[0]);
            E(f,"i%d=mpy_truth(t%d); Py_CLEAR(t%d); if(i%d<0) goto %s; if(!i%d) goto L%d;",iv,c,c,iv,ERR(f),iv,orelse);
            Cx lc; memset(&lc,0,sizeof lc); lc.kind=CX_LOOP; lc.up=f->cx; lc.brk=brk; lc.cont=top; lc.s=s;
            f->cx=&lc; stmts(f,s,&n->L[0]); f->cx=lc.up;
            E(f,"goto L%d;",top);
            LBL(f,orelse);
            stmts(f,s,&n->L[1]);
            LBL(f,brk);
            return; }
        case PK_For:{
            int src=ex(f,s,n->n[1]);
            int it=tmp(f), item=tmp(f), top=lab(f), done=lab(f), brk=lab(f), end=lab(f), iv=ivar(f);
            E(f,"t%d=PyObject_GetIter(t%d); Py_CLEAR(t%d); if(!t%d) goto %s;",it,src,src,it,ERR(f));
            LBL(f,top);
            E(f,"i%d=PyIter_NextItem(t%d,&t%d); if(i%d<0) goto %s; if(!i%d) goto L%d;",iv,it,item,iv,ERR(f),iv,done);
            store_target(f,s,n->n[0],item);
            Cx lc; memset(&lc,0,sizeof lc); lc.kind=CX_LOOP; lc.up=f->cx; lc.brk=brk; lc.cont=top; lc.s=s;
            f->cx=&lc; stmts(f,s,&n->L[0]); f->cx=lc.up;
            E(f,"goto L%d;",top);
            LBL(f,done);
            E(f,"Py_CLEAR(t%d);",it);
            stmts(f,s,&n->L[1]);
            E(f,"goto L%d;",end);
            LBL(f,brk);
            E(f,"Py_CLEAR(t%d);",it);
            LBL(f,end);
            return; }
        case PK_Break: do_jump(f,f->cx,WHY_BREAK); return;
        case PK_Continue: do_jump(f,f->cx,WHY_CONTINUE); return;
        case PK_Raise:{
            int ex_=n->n[0]?ex(f,s,n->n[0]):-1, ca=n->n[1]?ex(f,s,n->n[1]):-1;
            char a[16], b[16];
            if(ex_>=0) snprintf(a,16,"t%d",ex_); else snprintf(a,16,"NULL");
            if(ca>=0) snprintf(b,16,"t%d",ca); else snprintf(b,16,"NULL");
            E(f,"mpy_raise(%s,%s);",a,b);
            if(ex_>=0) E(f,"Py_CLEAR(t%d);",ex_);
            if(ca>=0) E(f,"Py_CLEAR(t%d);",ca);
            E(f,"goto %s;",ERR(f));
            return; }
        case PK_Try: st_try(f,s,n); return;
        case PK_With: st_with(f,s,n,0); return;
        case PK_Assert:{
            int c=ex(f,s,n->n[0]), iv=ivar(f), ok=lab(f);
            E(f,"i%d=mpy_truth(t%d); Py_CLEAR(t%d); if(i%d<0) goto %s; if(i%d) goto L%d;",iv,c,c,iv,ERR(f),iv,ok);
            if(n->n[1]){ int m=ex(f,s,n->n[1]); E(f,"mpy_assert_fail(t%d); Py_CLEAR(t%d); goto %s;",m,m,ERR(f)); }
            else E(f,"mpy_assert_fail(NULL); goto %s;",ERR(f));
            LBL(f,ok);
            return; }
        case PK_Import: st_import(f,s,n); return;
        case PK_ImportFrom:
            if(n->id[0] && !strcmp(n->id[0],"__future__")) return;    /* compile-time only */
            st_importfrom(f,s,n); return;
        case PK_Global: case PK_Nonlocal: return;
        case PK_FunctionDef: case PK_AsyncFunctionDef: case PK_ClassDef:{
            int fn=make_function(f,s,n);
            store_name(f,s,n->id[0],fn);
            return; }
        default:
            gfail(f->g,n->line,"%s is not compiled",py_kind_name(n->kind));
    }
}

/* ---------------------------------------------------------------- functions, classes */
static int scope_line(PyNode *n){
    int line=n->line;
    if(n->kind==PK_FunctionDef||n->kind==PK_AsyncFunctionDef||n->kind==PK_ClassDef)
        for(int i=0;i<n->L[1].n;i++) if(n->L[1].v[i]->line<line) line=n->L[1].v[i]->line;
    return line;
}
static void gen_unit(Gen *g, CScope *u);
/* cells for the given free variable names, as seen from scope s: arrays cv<k>[] / cn<k>[] */
static int cell_arrays(Fn *f, CScope *s, char **names, int n){
    int k=f->nctx++;
    xb_f(&f->decl,"    PyObject *cv%d[%d], *cn%d[%d];\n",k,n+1,k,n+1);
    for(int i=0;i<n;i++){
        char ce[96];
        if(!cell_expr(f,s,names[i],ce,sizeof ce)) gfail(f->g,f->line,"no cell for the free variable %s",names[i]);
        E(f,"cv%d[%d]=%s; cn%d[%d]=K[%d];",k,i,ce,k,i,k_str(f->g,names[i]));
    }
    return k;
}
static int closure_tuple(Fn *f, CScope *s, CScope *u){
    if(!u->nfree) return -1;
    int t=tmp(f);
    E(f,"if(!(t%d=PyTuple_New(%d))) goto %s;",t,u->nfree,ERR(f));
    for(int i=0;i<u->nfree;i++){
        char ce[96];
        if(!cell_expr(f,s,u->freevars[i],ce,sizeof ce)) gfail(f->g,f->line,"no cell for the free variable %s",u->freevars[i]);
        E(f,"PyTuple_SET_ITEM(t%d,%d,Py_NewRef(%s));",t,i,ce);
    }
    return t;
}
/* the CPython code object of scope u (an island / genexp), made a function with its cells */
static int island_function(Fn *f, CScope *s, CScope *u, PyNode *node, int line){
    (void)node;
    Gen *g=f->g;
    int nth=g->nths[u->id];
    CpyCode *cc=cpy_find(g,u->qualname,line,nth);
    if(!cc) gfail(g,line,"CPython has no code object %s at line %d",u->qualname,line);
    int r=ref_cpy(g,u->qualname,line,nth);
    int ca=cell_arrays(f,s,cc->freevars,cc->nfree);
    int t=tmp(f);
    E(f,"if(!(t%d=mpy_func_cpython(CO[%d],G,cv%d,cn%d,%d,NULL,NULL,NULL,NULL))) goto %s;",t,r,ca,ca,cc->nfree,ERR(f));
    g->nislands++;
    return t;
}
/* defaults, kwdefaults of a def / lambda: temps or -1 */
static void def_defaults(Fn *f, CScope *s, PyNode *a, int *defs, int *kwdefs){
    *defs=-1; *kwdefs=-1;
    if(a->L[4].n) *defs=ex_seq(f,s,&a->L[4],PK_Tuple);
    int any=0; for(int i=0;i<a->L[3].n;i++) if(a->L[3].v[i]) any=1;
    if(any){
        int d=tmp(f); *kwdefs=d;
        E(f,"if(!(t%d=PyDict_New())) goto %s;",d,ERR(f));
        for(int i=0;i<a->L[2].n;i++){
            PyNode *v=a->L[3].v[i]; if(!v) continue;
            char *nm=mangled(s,a->L[2].v[i]->id[0]); int k=k_str(f->g,nm); free(nm);
            int t=ex(f,s,v);
            E(f,"if(PyDict_SetItem(t%d,K[%d],t%d)){ Py_CLEAR(t%d); goto %s; } Py_CLEAR(t%d);",d,k,t,t,ERR(f),t);
        }
    }
}
/* the qualname CPython gives the __annotate__ code of something defined in scope s */
static void annotate_qualname(CScope *s, char *q, size_t n){
    while(s && s->is_comprehension) s=s->parent;
    if(!s || s->kind==SC_MODULE) snprintf(q,n,"__annotate__");
    else if(s->kind==SC_CLASS) snprintf(q,n,"%s.__annotate__",s->qualname);
    else snprintf(q,n,"%s.<locals>.__annotate__",s->qualname);
}
/* the __annotate__ of def n (CPython's, at the line of `def`), or -1 */
static int def_annotate(Fn *f, CScope *s, PyNode *n, int line){
    Gen *g=f->g;
    (void)line;
    char q[1024]; annotate_qualname(s,q,sizeof q);
    CpyCode *cc=cpy_find(g,q,n->line,0);
    if(!cc) return -1;
    int r=ref_cpy(g,q,n->line,0);
    int ca=cell_arrays(f,s,cc->freevars,cc->nfree);
    int t=tmp(f);
    char fq[1100]; snprintf(fq,sizeof fq,"%s.__annotate__",capi_scope_of(&g->st,n)->qualname);
    E(f,"if(!(t%d=mpy_func_cpython(CO[%d],G,cv%d,cn%d,%d,NULL,NULL,NULL,NULL))) goto %s;",t,r,ca,ca,cc->nfree,ERR(f));
    E(f,"if(PyObject_SetAttrString(t%d,\"__qualname__\",K[%d])) goto %s;",t,k_str(g,fq),ERR(f));
    return t;
}
static int make_function(Fn *f, CScope *s, PyNode *n){
    Gen *g=f->g;
    CScope *u=capi_scope_of(&g->st,n);
    if(!u) gfail(g,n->line,"no scope for a definition");
    int line= n->kind==PK_Lambda||n->kind==PK_GeneratorExp ? n->line : scope_line(n);
    if(n->kind==PK_ClassDef){
        int ndeco=n->L[1].n; int *decos=(int*)xmalloc(sizeof(int)*(size_t)(ndeco+1));
        for(int i=0;i<ndeco;i++) decos[i]=ex(f,s,n->L[1].v[i]);
        int star=0; for(int i=0;i<n->L[3].n;i++) if(n->L[3].v[i]->kind==PK_Starred) star=1;
        for(int i=0;i<n->L[4].n;i++) if(!n->L[4].v[i]->id[0]) star=1;
        if(star) gfail(g,n->line,"class bases with * or **");
        int nb=n->L[3].n, nk=n->L[4].n;
        int *bs=(int*)xmalloc(sizeof(int)*(size_t)(nb+nk+1));
        for(int i=0;i<nb;i++) bs[i]=ex(f,s,n->L[3].v[i]);
        for(int i=0;i<nk;i++) bs[nb+i]=ex(f,s,n->L[4].v[i]->n[0]);
        char **kwn=(char**)xmalloc(sizeof(char*)*(size_t)(nk+1)); for(int i=0;i<nk;i++) kwn[i]=n->L[4].v[i]->id[0];
        int kt= nk ? k_names(g,kwn,nk) : -1; free(kwn);
        int arr=argarray(f,nb+nk);
        for(int i=0;i<nb+nk;i++) E(f,"a%d[%d]=t%d;",arr,i,bs[i]);
        int kname=k_str(g,n->id[0]);              /* (the class's own name; the binding is mangled) */
        int t=tmp(f);
        char ks[32]; if(kt>=0) snprintf(ks,sizeof ks,"K[%d]",kt); else snprintf(ks,sizeof ks,"NULL");
        if(u->island){
            int nth=g->nths[u->id];
            CpyCode *cc=cpy_find(g,u->qualname,line,nth);
            if(!cc) gfail(g,line,"CPython has no code object %s at line %d",u->qualname,line);
            int r=ref_cpy(g,u->qualname,line,nth);
            int ca=cell_arrays(f,s,cc->freevars,cc->nfree);
            E(f,"t%d=mpy_class_cpython(CO[%d],G,cv%d,cn%d,%d,K[%d],a%d,%d,%s,a%d+%d,%d);",t,r,ca,ca,cc->nfree,kname,arr,nb,ks,arr,nb,nk);
            g->nislands++;
        } else {
            gen_unit(g,u);
            int r=ref_stub(g,u,n->id[0],line);
            int clo=closure_tuple(f,s,u);
            char CL[16]; if(clo>=0) snprintf(CL,16,"t%d",clo); else snprintf(CL,16,"NULL");
            E(f,"t%d=mpy_class(CO[%d],G,u%d,%s,K[%d],a%d,%d,%s,a%d+%d,%d);",t,r,u->id,CL,kname,arr,nb,ks,arr,nb,nk);
            if(clo>=0) E(f,"Py_CLEAR(t%d);",clo);
        }
        xb_put(&f->b,"    ",4);
        for(int i=0;i<nb+nk;i++) xb_f(&f->b,"Py_CLEAR(t%d); ",bs[i]);
        xb_f(&f->b,"if(!t%d){",t);
        for(int i=0;i<ndeco;i++) xb_f(&f->b," Py_CLEAR(t%d);",decos[i]);
        xb_f(&f->b," goto %s; }\n",ERR(f));
        for(int i=ndeco-1;i>=0;i--){
            int r=tmp(f);
            E(f,"t%d=PyObject_CallOneArg(t%d,t%d); Py_CLEAR(t%d); Py_CLEAR(t%d); if(!t%d){",r,decos[i],t,decos[i],t,r);
            xb_put(&f->b,"   ",3); for(int k=0;k<i;k++) xb_f(&f->b," Py_CLEAR(t%d);",decos[k]); xb_f(&f->b," goto %s; }\n",ERR(f));
            t=r;
        }
        free(decos); free(bs);
        return t;
    }
    /* def / async def / lambda */
    PyNode *a=n->n[0];
    int ndeco= n->kind==PK_Lambda ? 0 : n->L[1].n;
    int *decos=(int*)xmalloc(sizeof(int)*(size_t)(ndeco+1));
    for(int i=0;i<ndeco;i++) decos[i]=ex(f,s,n->L[1].v[i]);
    int defs, kwdefs; def_defaults(f,s,a,&defs,&kwdefs);
    int ann= n->kind==PK_Lambda ? -1 : def_annotate(f,s,n,line);
    int t=tmp(f);
    char D[16], KD[16], AN[16];
    if(defs>=0) snprintf(D,16,"t%d",defs); else snprintf(D,16,"NULL");
    if(kwdefs>=0) snprintf(KD,16,"t%d",kwdefs); else snprintf(KD,16,"NULL");
    if(ann>=0) snprintf(AN,16,"t%d",ann); else snprintf(AN,16,"NULL");
    if(u->island || u->is_generator || u->is_coroutine){
        int nth=g->nths[u->id];
        CpyCode *cc=cpy_find(g,u->qualname,line,nth);
        if(!cc) gfail(g,line,"CPython has no code object %s at line %d",u->qualname,line);
        int r=ref_cpy(g,u->qualname,line,nth);
        int ca=cell_arrays(f,s,cc->freevars,cc->nfree);
        E(f,"t%d=mpy_func_cpython(CO[%d],G,cv%d,cn%d,%d,%s,%s,%s,NULL);",t,r,ca,ca,cc->nfree,D,KD,AN);
        g->nislands++;
    } else {
        gen_unit(g,u);
        const char *realname= n->kind==PK_Lambda ? "<lambda>" : u->name;
        int r=ref_stub(g,u,realname,line);
        int clo=closure_tuple(f,s,u);
        const char *doc=NULL;
        if(n->kind!=PK_Lambda && n->L[0].n && is_docstring(n->L[0].v[0])) doc=cpy_doc(g,n->line,n->col);
        char DOC[16];
        if(doc){ int k=k_str(g,doc); snprintf(DOC,16,"K[%d]",k); } else snprintf(DOC,16,"NULL");
        char CL[16]; if(clo>=0) snprintf(CL,16,"t%d",clo); else snprintf(CL,16,"NULL");
        /* callable directly (capi_rt.h mpy_vcall) when it has just positional parameters */
        PyNode *pa=n->n[0]; int direct= (g->in->fast_calls && !pa->n[0] && !pa->n[1] && !pa->L[2].n) ? pa->L[0].n+pa->L[1].n : -1;
        E(f,"t%d=mpy_func(CO[%d],G,u%d,%s,%s,%s,%s,NULL,%s,%d);",t,r,u->id,CL,D,KD,AN,DOC,direct);
        if(clo>=0) E(f,"Py_CLEAR(t%d);",clo);
    }
    if(defs>=0) E(f,"Py_CLEAR(t%d);",defs);
    if(kwdefs>=0) E(f,"Py_CLEAR(t%d);",kwdefs);
    if(ann>=0) E(f,"Py_CLEAR(t%d);",ann);
    xb_put(&f->b,"    ",4); xb_f(&f->b,"if(!t%d){",t);
    for(int i=0;i<ndeco;i++) xb_f(&f->b," Py_CLEAR(t%d);",decos[i]);
    xb_f(&f->b," goto %s; }\n",ERR(f));
    for(int i=ndeco-1;i>=0;i--){
        int r=tmp(f);
        E(f,"t%d=PyObject_CallOneArg(t%d,t%d); Py_CLEAR(t%d); Py_CLEAR(t%d); if(!t%d){",r,decos[i],t,decos[i],t,r);
        xb_put(&f->b,"   ",3); for(int k=0;k<i;k++) xb_f(&f->b," Py_CLEAR(t%d);",decos[k]); xb_f(&f->b," goto %s; }\n",ERR(f));
        t=r;
    }
    free(decos);
    return t;
}

/* ---- the trampoline of a unit (Python source) */
static void stub_line(Gen *g, const char *fmt, ...){
    va_list ap; va_start(ap,fmt); xb_vf(&g->stub,fmt,ap); va_end(ap); xb_put(&g->stub,"\n",1); g->stub_line++;
}
/* the parameters, in order, as names of u's symbols */
static int param_names(CScope *u, PyNode *a, char ***out){
    int n=0; char **v=(char**)xmalloc(sizeof(char*)*64); int cap=64;
    #define ADD(raw) do{ if(n==cap){ cap*=2; v=(char**)xrealloc(v,sizeof(char*)*(size_t)cap); } v[n++]=capi_mangle(u->private_name,raw); }while(0)
    for(int i=0;i<a->L[0].n;i++) ADD(a->L[0].v[i]->id[0]);
    for(int i=0;i<a->L[1].n;i++) ADD(a->L[1].v[i]->id[0]);
    if(a->n[0]) ADD(a->n[0]->id[0]);
    for(int i=0;i<a->L[2].n;i++) ADD(a->L[2].v[i]->id[0]);
    if(a->n[1]) ADD(a->n[1]->id[0]);
    #undef ADD
    *out=v; return n;
}

/* ---- class / module annotations: the index CPython gives each (simple-name) annotation */
static void number_annotations(PyList *body, int *counter, int depth, int module){
    for(int i=0;i<body->n;i++){
        PyNode *st=body->v[i];
        if(st->kind==PK_AnnAssign && st->op && st->n[0]->kind==PK_Name){
            st->aux1=*counter;                           /* its index */
            st->aux2= module || depth>0;                 /* tracked in __conditional_annotations__ */
            (*counter)++;
        }
        switch(st->kind){
            case PK_If: case PK_While: case PK_For: case PK_AsyncFor: case PK_With: case PK_AsyncWith:
                number_annotations(&st->L[0],counter,depth+1,module); number_annotations(&st->L[1],counter,depth+1,module); break;
            case PK_Try: case PK_TryStar:
                number_annotations(&st->L[0],counter,depth+1,module); number_annotations(&st->L[1],counter,depth+1,module); number_annotations(&st->L[2],counter,depth+1,module);
                for(int h=0;h<st->L[3].n;h++) number_annotations(&st->L[3].v[h]->L[0],counter,depth+1,module);
                break;
            case PK_Match: for(int c=0;c<st->L[3].n;c++) number_annotations(&st->L[3].v[c]->L[0],counter,depth+1,module); break;
            default: break;
        }
    }
}

static void gen_unit(Gen *g, CScope *u){
    PyNode *n=u->node;
    Fn F; memset(&F,0,sizeof F); Fn *f=&F;
    f->g=g; f->sc=u; f->err=-1;
    g->nunits++;
    PyList *body=NULL;
    int is_module=u->kind==SC_MODULE, is_class=u->kind==SC_CLASS;
    if(is_module || is_class) body=&n->L[0];
    else if(n->kind==PK_Lambda) body=NULL;
    else body=&n->L[0];
    /* the trampoline */
    if(is_module){
        stub_line(g,"def __mpy_mk_%d(__mpy_env__):",u->id);
        stub_line(g,"    def __mpy_u%d():",u->id);
        stub_line(g,"        return __mpy_env__()");
    } else if(is_class){
        stub_line(g,"def __mpy_mk_%d(__mpy_env__):",u->id);
        stub_line(g,"    class __mpy_u%d:",u->id);
        stub_line(g,"        __mpy_env__()");
    } else {
        char **pn; int np=param_names(u,n->n[0],&pn);
        XB sig={0}, call={0}; xb_put(&sig,"",0); xb_put(&call,"",0);
        PyNode *a=n->n[0];
        int k=0;
        for(int i=0;i<a->L[0].n;i++,k++){ if(sig.len) xb_put(&sig,", ",2); xb_f(&sig,"%s",pn[k]); }
        if(a->L[0].n) xb_put(&sig,", /",3);
        for(int i=0;i<a->L[1].n;i++,k++){ if(sig.len) xb_put(&sig,", ",2); xb_f(&sig,"%s",pn[k]); }
        if(a->n[0]){ if(sig.len) xb_put(&sig,", ",2); xb_f(&sig,"*%s",pn[k]); k++; }
        else if(a->L[2].n){ if(sig.len) xb_put(&sig,", ",2); xb_put(&sig,"*",1); }
        for(int i=0;i<a->L[2].n;i++,k++){ if(sig.len) xb_put(&sig,", ",2); xb_f(&sig,"%s",pn[k]); }
        if(a->n[1]){ if(sig.len) xb_put(&sig,", ",2); xb_f(&sig,"**%s",pn[k]); k++; }
        for(int i=0;i<np;i++){ if(i) xb_put(&call,", ",2); xb_f(&call,"%s",pn[i]); }
        stub_line(g,"def __mpy_mk_%d(__mpy_env__):",u->id);
        stub_line(g,"    def __mpy_u%d(%s):",u->id,sig.s);
        stub_line(g,"        return __mpy_env__(%s)",call.s);
        free(sig.s); free(call.s);
        for(int i=0;i<np;i++) free(pn[i]);
        free(pn);
    }
    /* prologue */
    XB pro={0}; xb_put(&pro,"",0);
    if(is_class){
        xb_f(&pro,"    if(!(NS=mpy_class_ns())) goto L_err;\n");
        int kq=k_str(g,"__qualname__"), kqv=k_str(g,u->qualname), kfl=k_str(g,"__firstlineno__"), kl=k_int(g,scope_line(n));
        xb_f(&pro,"    if(mpy_store_name(NS,K[%d],K[%d]) || mpy_store_name(NS,K[%d],K[%d])) goto L_err;\n",kq,kqv,kfl,kl);
        const char *doc= body->n && is_docstring(body->v[0]) ? cpy_doc(g,n->line,n->col) : NULL;
        if(doc) xb_f(&pro,"    if(mpy_store_name(NS,K[%d],K[%d])) goto L_err;\n",k_str(g,"__doc__"),k_str(g,doc));
        if(u->needs_class_cell) xb_f(&pro,"    if(!(c_class=PyCell_New(NULL))) goto L_err;\n");
    }
    if(is_module){
        const char *doc= body->n && is_docstring(body->v[0]) ? cpy_doc(g,0,0) : NULL;
        if(doc) xb_f(&pro,"    if(PyDict_SetItem(G,K[%d],K[%d])) goto L_err;\n",k_str(g,"__doc__"),k_str(g,doc));
    }
    /* annotations of a module / class body */
    int has_ann=0, ncond=0, counter=0;
    if(is_module || is_class){
        number_annotations(body,&counter,0,is_module);
        has_ann=counter>0;
    }
    CpyCode *bodycode=NULL;
    if(is_class){
        bodycode=cpy_find(g,u->qualname,scope_line(n),g->nths[u->id]);
        if(bodycode){
            if(cpy_has(bodycode->cellvars,bodycode->ncell,"__classdict__")) xb_f(&pro,"    if(!(c_classdict=PyCell_New(NS))) goto L_err;\n");
            if(cpy_has(bodycode->cellvars,bodycode->ncell,"__conditional_annotations__")){ xb_f(&pro,"    { PyObject *s_=PySet_New(NULL); if(!s_) goto L_err; c_condann=PyCell_New(s_); Py_DECREF(s_); if(!c_condann) goto L_err; }\n"); ncond=1; }
        }
    }
    if((is_module || is_class) && has_ann && g->future_ann){
        /* SETUP_ANNOTATIONS: the body fills __annotations__ with text */
        int ka=k_str(g,"__annotations__");
        if(is_module) xb_f(&pro,"    { int h_=PyDict_Contains(G,K[%d]); if(h_<0) goto L_err; if(!h_){ PyObject *d_=PyDict_New(); if(!d_) goto L_err; int e_=PyDict_SetItem(G,K[%d],d_); Py_DECREF(d_); if(e_) goto L_err; } }\n",ka,ka);
        else xb_f(&pro,"    { PyObject *o_=NULL; int h_=PyMapping_GetOptionalItem(NS,K[%d],&o_); Py_XDECREF(o_); if(h_<0) goto L_err; if(!h_){ PyObject *d_=PyDict_New(); if(!d_) goto L_err; int e_=mpy_store_name(NS,K[%d],d_); Py_DECREF(d_); if(e_) goto L_err; } }\n",ka,ka);
    }
    if(is_module && has_ann){
        /* the module's own __annotate__: the last __annotate__ constant of the module's code */
        CpyCode *mc=NULL; int line=0, nth=0;
        { CpyMeta *m=g->in->meta; for(int i=0;i<m->ncodes;i++) if(!strcmp(m->codes[i].qualname,"__annotate__") && m->codes[i].parent==0) mc=&m->codes[i];
          if(mc) for(int i=0;i<m->ncodes && &m->codes[i]!=mc;i++) if(!strcmp(m->codes[i].qualname,"__annotate__") && m->codes[i].line==mc->line) nth++; }
        if(mc){
            line=mc->line;
            int r=ref_cpy(g,"__annotate__",line,nth);
            xb_f(&pro,"    { PyObject *a_=mpy_func_cpython(CO[%d],G,NULL,NULL,0,NULL,NULL,NULL,NULL); if(!a_) goto L_err; int e_=PyDict_SetItem(G,K[%d],a_); Py_DECREF(a_); if(e_) goto L_err; }\n",r,k_str(g,"__annotate__"));
        }
        xb_f(&pro,"    { PyObject *s_=PySet_New(NULL); if(!s_) goto L_err; int e_=PyDict_SetItem(G,K[%d],s_); Py_DECREF(s_); if(e_) goto L_err; }\n",k_str(g,"__conditional_annotations__"));
    }
    (void)ncond;
    /* parameters and cells */
    if(!is_module && !is_class){
        char **pn; int np=param_names(u,n->n[0],&pn);
        for(int i=0;i<np;i++){
            int si=sym_index(u,pn[i]);
            if(si<0) gfail(g,n->line,"parameter %s",pn[i]);
            if(u->syms[si].res==R_CELL) xb_f(&pro,"    if(!(c%d_%d=PyCell_New(args[%d]))) goto L_err;\n",u->id,si,i);
            else xb_f(&pro,"    v%d_%d=Py_NewRef(args[%d]);\n",u->id,si,i);
            free(pn[i]);
        }
        free(pn);
        for(int i=0;i<u->nsyms;i++){
            if(u->syms[i].res!=R_CELL) continue;
            int isparam=(u->syms[i].flags&DF_PARAM)!=0;
            if(!isparam) xb_f(&pro,"    if(!(c%d_%d=PyCell_New(NULL))) goto L_err;\n",u->id,i);
        }
        if(u->needs_class_cell) {}
    }
    /* body */
    f->line=0;
    if(body) stmts(f,u,body);
    else {                                         /* lambda: return its expression */
        set_line(f,n->line);
        int v=ex(f,u,n->n[1]); E(f,"Py_XSETREF(rv,t%d); t%d=NULL; goto L_ret;",v,v);
    }
    /* epilogue of class bodies */
    XB epi={0}; xb_put(&epi,"",0);
    if(is_class){
        if(has_ann){
            char q[1024]; snprintf(q,sizeof q,"%s.__annotate__",u->qualname);
            CpyCode *ac=NULL; CpyMeta *m=g->in->meta;
            for(int i=0;i<m->ncodes;i++) if(!strcmp(m->codes[i].qualname,q) && m->codes[i].line==scope_line(n)){ ac=&m->codes[i]; break; }
            if(ac){
                int r=ref_cpy(g,q,ac->line,0);
                Fn *ff=f; XB save=ff->b; ff->b=epi;
                int ca=cell_arrays(ff,u,ac->freevars,ac->nfree);
                E(ff,"{ PyObject *a_=mpy_func_cpython(CO[%d],G,cv%d,cn%d,%d,NULL,NULL,NULL,NULL); if(!a_) goto L_err; int e_=mpy_store_name(NS,K[%d],a_); Py_DECREF(a_); if(e_) goto L_err; }",r,ca,ca,ac->nfree,k_str(g,"__annotate_func__"));
                epi=ff->b; ff->b=save;
            }
        }
        if(bodycode && cpy_has(bodycode->cellvars,bodycode->ncell,"__classdict__")) xb_f(&epi,"    if(mpy_store_name(NS,K[%d],c_classdict)) goto L_err;\n",k_str(g,"__classdictcell__"));
        if(u->needs_class_cell) xb_f(&epi,"    if(mpy_store_name(NS,K[%d],c_class)) goto L_err;\n",k_str(g,"__classcell__"));
    }
    /* assemble */
    XB out={0};
    xb_f(&out,"/* %s */\nstatic PyObject *u%d(MpyEnv *env, PyObject *const *args){\n",u->qualname,u->id);
    xb_f(&out,"    PyObject *const G=env->globals; (void)args;\n    PyObject *rv=NULL; int ln=%d;\n",scope_line(n));
    if(is_class) xb_f(&out,"    PyObject *NS=NULL, *c_class=NULL, *c_classdict=NULL, *c_condann=NULL;\n");
    else if(u->needs_class_cell) xb_f(&out,"    PyObject *c_class=NULL;\n");
    for(int i=0;i<f->ntemp;i++) xb_f(&out,"%sPyObject *t%d=NULL;%s",i%8==0?"    ":" ",i,i%8==7||i==f->ntemp-1?"\n":"");
    CScope **sc=(CScope**)xmalloc(sizeof(CScope*)*(size_t)(f->ncomps+1)); int nsc=0;
    if(!is_module && !is_class) sc[nsc++]=u;
    for(int i=0;i<f->ncomps;i++) sc[nsc++]=f->comps[i];
    for(int k=0;k<nsc;k++){ CScope *x=sc[k];
        for(int i=0;i<x->nsyms;i++){
            if(x->syms[i].res==R_LOCAL) xb_f(&out,"    PyObject *v%d_%d=NULL; /* %s */\n",x->id,i,x->syms[i].name);
            if(x->syms[i].res==R_CELL) xb_f(&out,"    PyObject *c%d_%d=NULL; /* %s (cell) */\n",x->id,i,x->syms[i].name);
        }
    }
    xb_cat(&out,&f->decl);
    xb_cat(&out,&pro);
    xb_cat(&out,&f->b);
    xb_cat(&out,&epi);
    if(is_class) xb_f(&out,"    rv=Py_NewRef(Py_None);\n");
    else xb_f(&out,"    if(!rv) rv=Py_NewRef(Py_None);\n");
    xb_f(&out,"  L_ret:;\n");
    for(int i=0;i<f->ntemp;i++) xb_f(&out,"%sPy_CLEAR(t%d);%s",i%8==0?"    ":" ",i,i%8==7||i==f->ntemp-1?"\n":"");
    for(int k=0;k<nsc;k++){ CScope *x=sc[k];
        for(int i=0;i<x->nsyms;i++){
            if(x->syms[i].res==R_LOCAL) xb_f(&out,"    Py_CLEAR(v%d_%d);\n",x->id,i);
            if(x->syms[i].res==R_CELL) xb_f(&out,"    Py_CLEAR(c%d_%d);\n",x->id,i);
        }
    }
    if(is_class) xb_f(&out,"    Py_CLEAR(NS); Py_CLEAR(c_class); Py_CLEAR(c_classdict); Py_CLEAR(c_condann);\n");
    else if(u->needs_class_cell) xb_f(&out,"    Py_CLEAR(c_class);\n");
    xb_f(&out,"    return rv;\n  L_err:\n    mpy_traceback(\"%s\",MODFILE,ln);\n    Py_CLEAR(rv);\n    goto L_ret;\n}\n\n",n->kind==PK_Lambda?"<lambda>":is_module?"<module>":u->name);
    xb_f(&g->protos,"static PyObject *u%d(MpyEnv *env, PyObject *const *args);\n",u->id);
    xb_cat(&g->units,&out);
    free(out.s); free(pro.s); free(epi.s); free(f->b.s); free(f->decl.s); free(sc); free(f->comps);
}

/* ---------------------------------------------------------------- islands */
static int unsupported_expr(PyNode *e);
static int unsupported_list(PyList *l){ for(int i=0;i<l->n;i++) if(l->v[i] && unsupported_expr(l->v[i])) return 1; return 0; }
/* constructs this compiler leaves to CPython (inside one unit's own code) */
static int unsupported_expr(PyNode *e){
    if(!e) return 0;
    switch(e->kind){
        case PK_Await: case PK_Yield: case PK_YieldFrom: case PK_TemplateStr: case PK_Interpolation: return 1;
        case PK_Lambda: case PK_FunctionDef: case PK_AsyncFunctionDef: case PK_ClassDef: return 0;   /* their own units */
        case PK_ListComp: case PK_SetComp: case PK_DictComp:
            for(int i=0;i<e->L[0].n;i++) if(e->L[0].v[i]->op) return 1;   /* async comprehension */
            break;
        default: break;
    }
    for(int i=0;i<4;i++) if(e->n[i] && unsupported_expr(e->n[i])) return 1;
    for(int k=0;k<5;k++) for(int i=0;i<e->L[k].n;i++){
        PyNode *c=e->L[k].v[i];
        if(!c || c->kind==PK_ident) continue;
        if(c->kind==PK_FunctionDef||c->kind==PK_AsyncFunctionDef||c->kind==PK_ClassDef||c->kind==PK_Lambda) continue;
        if(unsupported_expr(c)) return 1;
    }
    return 0;
}
static int unsupported_stmt(PyNode *s, int in_class_or_module){
    switch(s->kind){
        case PK_Match: case PK_TryStar: case PK_AsyncFor: case PK_AsyncWith: case PK_TypeAlias: return 1;
        case PK_FunctionDef: case PK_AsyncFunctionDef:
            if(s->L[2].n) return 1;                       /* type parameters */
            /* decorators, defaults: this unit's code */
            if(unsupported_list(&s->L[1])) return 1;
            if(unsupported_list(&s->n[0]->L[4]) || unsupported_list(&s->n[0]->L[3])) return 1;
            return 0;
        case PK_ClassDef:
            if(s->L[2].n) return 1;
            if(unsupported_list(&s->L[1]) || unsupported_list(&s->L[3])) return 1;
            for(int i=0;i<s->L[4].n;i++){ if(unsupported_expr(s->L[4].v[i]->n[0])) return 1; if(!s->L[4].v[i]->id[0]) return 1; }
            for(int i=0;i<s->L[3].n;i++) if(s->L[3].v[i]->kind==PK_Starred) return 1;
            return 0;
        case PK_AnnAssign:
            if(s->n[2] && unsupported_expr(s->n[2])) return 1;
            return unsupported_expr(s->n[0]);
        default: break;
    }
    (void)in_class_or_module;
    for(int i=0;i<4;i++) if(s->n[i] && unsupported_expr(s->n[i])) return 1;
    for(int k=0;k<5;k++) for(int i=0;i<s->L[k].n;i++){
        PyNode *c=s->L[k].v[i];
        if(!c || c->kind==PK_ident) continue;
        if(c->kind==PK_ExceptHandler){ if(c->n[0] && unsupported_expr(c->n[0])) return 1; for(int j=0;j<c->L[0].n;j++) if(unsupported_stmt(c->L[0].v[j],0)) return 1; continue; }
        if(c->kind==PK_withitem){ if(unsupported_expr(c->n[0]) || unsupported_expr(c->n[1])) return 1; continue; }
        if(c->kind==PK_alias||c->kind==PK_keyword) continue;
        if(c->kind>=PK_FunctionDef && c->kind<=PK_Continue){ if(unsupported_stmt(c,0)) return 1; }
        else if(unsupported_expr(c)) return 1;
    }
    return 0;
}
static void decide(Gen *g, CScope *s){
    PyNode *n=s->node;
    if(!s->island && s->kind==SC_FUNCTION && !s->is_comprehension){
        if(s->is_generator || s->is_coroutine){ s->island=1; s->island_reason="generator / coroutine"; }
        else if(n->kind==PK_Lambda){ if(unsupported_expr(n->n[1])){ s->island=1; s->island_reason="unsupported expression"; } }
        else for(int i=0;i<n->L[0].n;i++) if(unsupported_stmt(n->L[0].v[i],0)){ s->island=1; s->island_reason="unsupported statement"; break; }
    }
    if(!s->island && s->kind==SC_CLASS){
        for(int i=0;i<n->L[0].n;i++) if(unsupported_stmt(n->L[0].v[i],1)){ s->island=1; s->island_reason="unsupported statement"; break; }
    }
    if(s->kind==SC_MODULE) for(int i=0;i<n->L[0].n;i++) if(unsupported_stmt(n->L[0].v[i],1)) n->L[0].v[i]->island=1;   /* CPython runs it */
    for(int k=0;k<s->nkids;k++) decide(g,s->kids[k]);
}
/* first lines (co_firstlineno) and which code object of a qualname and line each scope is */
static void scope_lines(Gen *g){
    int n=g->st.nall;
    g->lines=(int*)xmalloc(sizeof(int)*(size_t)n); g->nths=(int*)xmalloc(sizeof(int)*(size_t)n);
    for(int i=0;i<n;i++){
        CScope *s=g->st.all[i]; PyNode *node=s->node;
        g->lines[i]= s->kind==SC_MODULE ? 1 : node ? (node->kind==PK_Lambda||node->kind==PK_GeneratorExp||node->kind==PK_ListComp||node->kind==PK_SetComp||node->kind==PK_DictComp ? node->line : scope_line(node)) : 0;
        int nth=0;
        for(int j=0;j<i;j++){ CScope *t=g->st.all[j]; if(t->kind!=SC_ANNOTATION && g->lines[j]==g->lines[i] && !strcmp(t->qualname,s->qualname)) nth++; }
        g->nths[i]=nth;
    }
}

static int has_future_annotations(PyNode *mod){
    for(int i=0;i<mod->L[0].n;i++){
        PyNode *s=mod->L[0].v[i];
        if(s->kind==PK_Expr && s->n[0]->kind==PK_Constant) continue;
        if(s->kind!=PK_ImportFrom || !s->id[0] || strcmp(s->id[0],"__future__")) break;
        for(int k=0;k<s->L[3].n;k++) if(!strcmp(s->L[3].v[k]->id[0],"annotations")) return 1;
    }
    return 0;
}

int capi_codegen(CapiIn *in, CapiOut *out){
    memset(out,0,sizeof *out);
    PyParse pp;
    if(py_parse(&pp,in->path,in->src,in->len)){
        char b[1024]; snprintf(b,sizeof b,"%s:%d: SyntaxError: %s",in->path,pp.error_line,pp.error); out->error=xstrdup2(b); return 1;
    }
    Gen *g=(Gen*)xmalloc(sizeof(Gen)); memset(g,0,sizeof *g); g->in=in;
    if(capi_symtable(&g->st,pp.mod,in->modname)){
        char b[1024]; snprintf(b,sizeof b,"%s:%d: %s",in->path,g->st.error_line,g->st.error); out->error=xstrdup2(b); return 1;
    }
    if(setjmp(g->jb)){ char b[1200]; snprintf(b,sizeof b,"%s: %s",in->path,g->err); out->error=xstrdup2(b); return 1; }
    g->future_ann=has_future_annotations(pp.mod);
    scope_lines(g);
    decide(g,g->st.top);
    gen_unit(g,g->st.top);
    int mref=ref_stub(g,g->st.top,"<module>",1);
    /* the C file */
    XB c={0};
    xb_f(&c,"/* minipy --compile --target cpython: module %s (%s) */\n#include \"mpy_rt.h\"\n#define MODFILE ",in->modname,in->path);
    c_lit(&c,(const unsigned char*)in->path,strlen(in->path));
    xb_f(&c,"\nstatic PyObject *K[%d];\nstatic PyObject *CO[%d];\n",g->nk+1,g->nrefs+1);
    xb_f(&c,"static const unsigned char CODE[]={\n#include \"m%d_code.inc\"\n};\nstatic const unsigned char STUB[]={\n#include \"m%d_stub.inc\"\n};\n",in->index,in->index);
    xb_cat(&c,&g->kdata);
    xb_f(&c,"static const MpyCodeRef REFS[]={\n"); xb_cat(&c,&g->refs); xb_f(&c,"    {0,NULL,NULL,NULL,0,0}\n};\n");
    xb_f(&c,"static int init_consts(void){\n"); xb_cat(&c,&g->kinit); xb_f(&c,"    return 0;\n}\n");
    xb_cat(&c,&g->protos);
    xb_put(&c,"\n",1);
    xb_cat(&c,&g->units);
    xb_f(&c,"int mpy_exec_%d(PyObject *module){\n",in->index);
    xb_f(&c,"    static int ready;\n");
    xb_f(&c,"    if(!ready){ if(init_consts() || mpy_load_codes(CODE,sizeof CODE,STUB,sizeof STUB,REFS,%d,CO,",g->nrefs);
    c_lit(&c,(const unsigned char*)in->modname,strlen(in->modname));
    xb_f(&c,")) return -1; ready=1; }\n");
    xb_f(&c,"    PyObject *g=PyModule_GetDict(module);\n    if(mpy_prepare_globals(g)) return -1;\n");
    xb_f(&c,"    PyObject *f=mpy_func(CO[%d],g,u%d,NULL,NULL,NULL,NULL,NULL,NULL,-1);\n    if(!f) return -1;\n",mref,g->st.top->id);
    xb_f(&c,"    PyObject *r=PyObject_CallNoArgs(f); Py_DECREF(f);\n    if(!r) return -1;\n    Py_DECREF(r);\n    return 0;\n}\n");
    out->c=c.s; out->clen=c.len;
    out->stub=g->stub.s?g->stub.s:xstrdup2(""); out->stublen=g->stub.len;
    out->nunits=g->nunits; out->nislands=g->nislands;
    return 0;
}

/* ---------------------------------------------------------------- .meta */
static char *unhex(const char *s, size_t n){
    char *r=(char*)xmalloc(n/2+1); size_t k=0;
    for(size_t i=0;i+1<n;i+=2){ int a=s[i], b=s[i+1]; a= isdigit(a)?a-'0':(tolower(a)-'a'+10); b= isdigit(b)?b-'0':(tolower(b)-'a'+10); r[k++]=(char)(a*16+b); }
    r[k]=0; return r;
}
static char **unhex_list(const char *s, size_t n, int *cnt){
    char **v=NULL; int c=0;
    size_t i=0;
    while(i<n){
        size_t j=i; while(j<n && s[j]!=',') j++;
        if(j>i){ v=(char**)xrealloc(v,sizeof(char*)*(size_t)(c+1)); v[c++]=unhex(s+i,j-i); }
        i=j+1;
    }
    *cnt=c; return v;
}
int capi_read_meta(const char *path, CpyMeta *m){
    memset(m,0,sizeof *m);
    FILE *fp=fopen(path,"rb"); if(!fp) return 1;
    fseek(fp,0,SEEK_END); long len=ftell(fp); fseek(fp,0,SEEK_SET);
    char *buf=(char*)xmalloc((size_t)len+1); size_t got=fread(buf,1,(size_t)len,fp); fclose(fp); buf[got]=0;
    char *p=buf;
    while(*p){
        char *eol=strchr(p,'\n'); if(!eol) eol=p+strlen(p);
        char *f[8]; int nf=0; char *q=p;
        while(nf<8){ f[nf++]=q; char *t=memchr(q,'\t',(size_t)(eol-q)); if(!t) break; q=t+1; }
        char *ends[8]; for(int i=0;i<nf;i++){ char *t= i+1<nf ? f[i+1]-1 : eol; ends[i]=t; }
        if(p[0]=='C' && nf>=8){
            m->codes=(CpyCode*)xrealloc(m->codes,sizeof(CpyCode)*(size_t)(m->ncodes+1));
            CpyCode *c=&m->codes[m->ncodes++]; memset(c,0,sizeof *c);
            c->qualname=unhex(f[1],(size_t)(ends[1]-f[1])); c->line=atoi(f[2]); c->name=unhex(f[3],(size_t)(ends[3]-f[3])); c->flags=atoi(f[4]);
            c->cellvars=unhex_list(f[5],(size_t)(ends[5]-f[5]),&c->ncell); c->freevars=unhex_list(f[6],(size_t)(ends[6]-f[6]),&c->nfree);
            c->parent=atoi(f[7]);
        } else if(p[0]=='A' && nf>=4){
            m->anns=(CpyAnn*)xrealloc(m->anns,sizeof(CpyAnn)*(size_t)(m->nanns+1));
            CpyAnn *a=&m->anns[m->nanns++]; a->line=atoi(f[1]); a->name=unhex(f[2],(size_t)(ends[2]-f[2])); a->text=unhex(f[3],(size_t)(ends[3]-f[3]));
        } else if(p[0]=='D' && nf>=4){
            m->docs=(CpyDoc*)xrealloc(m->docs,sizeof(CpyDoc)*(size_t)(m->ndocs+1));
            CpyDoc *d=&m->docs[m->ndocs++]; d->line=atoi(f[1]); d->col=atoi(f[2]); d->doc=unhex(f[3],(size_t)(ends[3]-f[3]));
        }
        p= *eol ? eol+1 : eol;
    }
    free(buf);
    return 0;
}
