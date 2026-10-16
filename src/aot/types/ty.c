/* ========================= Typed compiler, types: types and symbol tables =========================
   Ty: making, comparing and naming types; the symbol tables of modules,
   functions and classes. */

#include "check.h"

int ty_ids;
Ty *TY_INT_T, *TY_BOOL_T, *TY_FLOAT_T, *TY_STR_T, *TY_VOID_T, *TY_BUF_T, *TY_BYTES_T, *TY_TYPE_T;

Ty *ty_new(TyKind k, Ty *elem, AClass *cls){ Ty *t=PPY_NEW0(Ty); t->k=k; t->elem=elem; t->cls=cls; t->id=++ty_ids;
    if(k==TY_GEN){ t->nelems=2; t->elems=PPY_NEW_ARR(Ty*,2); t->elems[0]=ty_var(); t->elems[1]=TY_VOID_T; }   /* what send() takes, what it returns (None) */
    return t; }
Ty *aot_gen_part(Ty *g, int i){ g=ty_find(g); if(g->k!=TY_GEN || g->nelems!=2) return TY_VOID_T; Ty *t=ty_find(g->elems[i]); return t; }
Ty *ty_var(void){ return ty_new(TY_VAR,NULL,NULL); }
Ty *ty_dict(Ty *key, Ty *val){ Ty *t=ty_new(TY_DICT,val,NULL); t->key=key; return t; }
Ty *ty_dkey(Ty *d){ d=ty_find(d); return d->key?d->key:TY_STR_T; }
Ty *ty_tuple(Ty **elems, int n){
    Ty *t=ty_new(TY_TUPLE,NULL,NULL); t->nelems=n; t->elems=PPY_NEW_ARR(Ty*,n>0?n:1);
    for(int i=0;i<n;i++) t->elems[i]=elems[i];
    return t;
}
Ty *ty_func(Ty **params, int n, Ty *ret){ Ty *t=ty_tuple(params,n); t->k=TY_FUNC; t->elem=ret; return t; }
Ty *ty_find(Ty *t){ while(t && t->k==TY_VAR && t->link) t=t->link; return t; }
int ty_opt(Ty *t){ t=ty_find(t); return t && t->opt; }
/* the shared types (TY_INT_T ...) are never made optional: a variable gets a copy of its own */
int ty_shared(Ty *t){ return t==TY_INT_T||t==TY_BOOL_T||t==TY_FLOAT_T||t==TY_STR_T||t==TY_BUF_T||t==TY_VOID_T||t==TY_BYTES_T||t==TY_TYPE_T; }
Ty *ty_copy(Ty *t){ Ty *n=ty_new(t->k,t->elem,t->cls); n->elems=t->elems; n->nelems=t->nelems; n->key=t->key; n->tup=t->tup; n->names=t->names; n->opt=t->opt; n->ndef=t->ndef; return n; }
/* element-like parts: list/dict/set/task/generator elements, a function's result */
int has_elem(Ty *t){ return t->k==TY_LIST||t->k==TY_DICT||t->k==TY_SET||t->k==TY_TASK||t->k==TY_GEN||t->k==TY_FUNC||t->k==TY_FILE; }
int ty_known(Ty *t){
    t=ty_find(t);
    if(!t || t->k==TY_VAR) return 0;
    if(t->k==TY_DICT && !ty_known(ty_dkey(t))) return 0;
    if(t->k==TY_TUPLE||t->k==TY_FUNC){ for(int i=0;i<t->nelems;i++) if(!ty_known(t->elems[i])) return 0; }
    if(has_elem(t)) return ty_known(t->elem);
    return 1;
}
int ty_is_ptr(Ty *t){ t=ty_find(t); return t->k==TY_STR||t->k==TY_BYTES||t->k==TY_LIST||t->k==TY_DICT||t->k==TY_SET||t->k==TY_OBJ||t->k==TY_BUF||t->k==TY_TASK||t->k==TY_TUPLE||t->k==TY_FILE||t->k==TY_FUNC||t->k==TY_GEN; }
int ty_size(Ty *t){ TyKind k=ty_find(t)->k; return k==TY_FLOAT||k==TY_INT ? 8 : 4; }
int names_eq(Ty *a, Ty *b){
    if(!a->names || !b->names) return a->names==b->names;
    for(int i=0;i<a->nelems;i++) if(strcmp(a->names[i],b->names[i])) return 0;
    return 1;
}
static int same_rec(Ty *a, Ty *b, int exact);
int ty_same(Ty *a, Ty *b){ return same_rec(a,b,0); }
int ty_same_exact(Ty *a, Ty *b){ return same_rec(a,b,1); }    /* Optional[...] counts too */
static int same_rec(Ty *a, Ty *b, int exact){
    a=ty_find(a); b=ty_find(b);
    if(a==b) return 1;
    if(a->k!=b->k) return 0;
    if(exact && a->opt!=b->opt) return 0;
    if(a->k==TY_DICT && !same_rec(ty_dkey(a),ty_dkey(b),exact)) return 0;
    if((a->k==TY_LIST||a->k==TY_FUNC||a->k==TY_GEN) && a->tup!=b->tup) return 0;
    if(a->k==TY_TUPLE||a->k==TY_FUNC){ if(a->nelems!=b->nelems) return 0; for(int i=0;i<a->nelems;i++) if(!same_rec(a->elems[i],b->elems[i],exact)) return 0; if(a->k==TY_TUPLE) return names_eq(a,b); }
    if(has_elem(a)) return same_rec(a->elem,b->elem,exact);
    if(a->k==TY_OBJ) return a->cls==b->cls;
    return a->k!=TY_VAR;
}
const char *ty_name(Ty *t){
    static char ring[8][160]; static int k;
    char *b=ring[k++&7];
    t=ty_find(t);
    switch(t->k){
        case TY_VAR: return "?";
        case TY_VOID: return "None";
        case TY_INT: return "int";
        case TY_BOOL: return "bool";
        case TY_FLOAT: return "float";
        case TY_STR: return "str";
        case TY_BYTES: return "bytes";
        case TY_TYPE: return "type";
        case TY_BUF: return "buffer";
        case TY_FILE: return "file";
        case TY_LIST: snprintf(b,160,t->tup?"tuple[%s, ...]":"list[%s]",ty_name(t->elem)); return b;
        case TY_GEN: snprintf(b,160,"%sIterator[%s]",t->tup?"Async":"",ty_name(t->elem)); return b;
        case TY_FUNC:{ int n=snprintf(b,160,"Callable[[");
            int nreg=t->nelems-(t->tup&1)-((t->tup>>1)&1);
            for(int i=0;i<t->nelems && n<140;i++) n+=snprintf(b+n,160-(size_t)n,"%s%s%s",i?", ":"",i<nreg?"":i==nreg&&(t->tup&1)?"*":"**",ty_name(t->elems[i]));
            snprintf(b+n,160-(size_t)n,"], %s]",ty_name(t->elem)); return b; }
        case TY_SET: snprintf(b,160,"set[%s]",ty_name(t->elem)); return b;
        case TY_DICT: snprintf(b,160,"dict[%s, %s]",ty_name(ty_dkey(t)),ty_name(t->elem)); return b;
        case TY_OBJ: return t->cls->name;
        case TY_TASK: snprintf(b,160,"Task[%s]",ty_name(t->elem)); return b;
        case TY_TUPLE: if(t->names){ int n=snprintf(b,160,"{");
                for(int i=0;i<t->nelems && n<150;i++) n+=snprintf(b+n,160-(size_t)n,"%s'%s': %s",i?", ":"",t->names[i],ty_name(t->elems[i]));
                snprintf(b+n,160-(size_t)n,"}"); return b; }
            { int n=snprintf(b,160,"tuple[");
            for(int i=0;i<t->nelems && n<150;i++) n+=snprintf(b+n,160-(size_t)n,"%s%s",i?", ":"",ty_name(t->elems[i]));
            snprintf(b+n,160-(size_t)n,"]"); return b; }
    }
    return "?";
}

/* ---------------------------------------------------------------- symbol tables */

ASym *symtab_find(SymTab *t, const char *name){ for(int i=0;i<t->n;i++) if(!strcmp(t->v[i].name,name)) return &t->v[i]; return NULL; }
ASym *symtab_add(SymTab *t, const char *name, SymKind kind, void *p){
    ASym *s=symtab_find(t,name);
    if(!s){ if(t->n==t->cap){ t->cap=t->cap?t->cap*2:16; t->v=(ASym*)xrealloc(t->v,sizeof(ASym)*(size_t)t->cap); } s=&t->v[t->n++]; s->name=xstrdup2(name); }
    s->kind=kind; s->p=p; return s;
}
XInfo *xinfo(Expr *e){ if(!e->ty) e->ty=PPY_NEW0(XInfo); return (XInfo*)e->ty; }

AField *aot_find_field(AClass *c, const char *name){ for(;c;c=c->base) for(int i=0;i<c->nfields;i++) if(!strcmp(c->fields[i]->name,name)) return c->fields[i]; return NULL; }
AField *aot_field_root(AField *fd){ while(fd->over) fd=fd->over; return fd; }
int aot_field_shared(AField *fd){ AField *r=aot_field_root(fd); return fd->cvar && !r->inst_set && !r->overridden; }
AFunc *aot_find_method(AClass *c, const char *name){ for(;c;c=c->base) for(int i=0;i<c->nmethods;i++) if(!strcmp(c->methods[i]->name,name)) return c->methods[i]; return NULL; }
int aot_subclass(AClass *c, AClass *base){ for(;c;c=c->base) if(c==base) return 1; return 0; }

