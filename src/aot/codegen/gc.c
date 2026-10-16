/* ========================= Typed compiler, code generator: cycles =========================
   Which classes and closures can be part of a reference cycle (a type
   graph and its strongly connected components) and the maps the
   runtime's cycle collector reads. */

#include "gen.h"

/* ---------------------------------------------------------------- cycles
   Reference counting frees an object when its last reference goes, but
   objects that refer to each other - a window and its children's parent
   fields, an object holding its own bound method, a list a closure captures
   and holds - keep their counts up once nothing else refers to them. The
   types say which objects can be in such a cycle at all: a node per class,
   per function whose values are closures, and one for function values; an
   edge where a field (through lists, dicts, sets, tuples), a capture or a
   function value (a closure, a method bound to an object) can refer to such
   an object. The objects of a node in a cycle of this graph (a strongly
   connected component) get the runtime's CYC mark when made, as do the
   containers and cells that can hold them; for a program without such
   types nothing is linked and nothing changes. When a CYC object loses a
   reference but lives on, the runtime notes it, and now and then checks
   whether only cycles among such objects still refer to it - then they go
   (aot/runtime, rt_gc_root, rt_gc_collect). Generators and tasks are not
   looked into: a cycle through one stays. */
static int gc_is_sub(AClass *d, AClass *c){                 /* an object of class d is a c */
    for(AClass *k=d;k;k=k->base) if(k==c || k->origin==c) return 1;
    for(int i=0;i<d->nmro;i++) if(d->mro[i]==c) return 1;
    return 0;
}
static int gc_type_d(Ty *t, int depth){
    t=ty_find(t);
    if(!gg || !gg->gc_on || !t || depth>24) return 0;
    switch(t->k){
        case TY_OBJ:{
            AClass *c=t->cls; if(!c) return 0;
            if(gg->gc_sub[c->id]<0){ gg->gc_sub[c->id]=0;
                for(int i=0;i<gg->p->nclasses;i++) if(gg->gc_cls[gg->p->classes[i]->id] && gc_is_sub(gg->p->classes[i],c)) gg->gc_sub[c->id]=1; }
            return gg->gc_sub[c->id]; }
        case TY_LIST: case TY_SET: return gc_type_d(t->elem,depth+1);
        case TY_DICT: return gc_type_d(ty_dkey(t),depth+1) || gc_type_d(t->elem,depth+1);
        case TY_TUPLE: for(int i=0;i<t->nelems;i++) if(gc_type_d(t->elems[i],depth+1)) return 1; return 0;
        case TY_FUNC: return gg->gc_func;
        default: return 0;
    }
}
/* a value of type t can be (or be a container holding) an object that can be in a cycle */
int gc_type(Ty *t){ return gc_type_d(t,0); }
typedef struct { int n, fnode; unsigned char *adj; int *idx, *low, *stk, nstk, counter; unsigned char *on, *cyc; } GcGraph;
static void gc_edge(GcGraph *g, int a, int b){ g->adj[((size_t)a*(size_t)g->n+(size_t)b)>>3]|=(unsigned char)(1u<<((a*g->n+b)&7)); }
static int gc_has(GcGraph *g, int a, int b){ return g->adj[((size_t)a*(size_t)g->n+(size_t)b)>>3]>>(((size_t)a*(size_t)g->n+(size_t)b)&7)&1; }
static void gc_expand(GcGraph *g, Ty *t, int from, int depth){   /* edges from `from` to what a value of type t can be */
    t=ty_find(t); if(!t || depth>24) return;
    switch(t->k){
        case TY_OBJ: if(t->cls) for(int i=0;i<gg->p->nclasses;i++) if(gc_is_sub(gg->p->classes[i],t->cls)) gc_edge(g,from,gg->p->classes[i]->id); return;
        case TY_LIST: case TY_SET: gc_expand(g,t->elem,from,depth+1); return;
        case TY_DICT: gc_expand(g,ty_dkey(t),from,depth+1); gc_expand(g,t->elem,from,depth+1); return;
        case TY_TUPLE: for(int i=0;i<t->nelems;i++) gc_expand(g,t->elems[i],from,depth+1); return;
        case TY_FUNC: gc_edge(g,from,g->fnode); return;
        default: return;
    }
}
static void gc_scc(GcGraph *g, int v){                      /* Tarjan's strongly connected components */
    g->idx[v]=g->low[v]=++g->counter; g->stk[g->nstk++]=v; g->on[v]=1;
    for(int w=0;w<g->n;w++) if(gc_has(g,v,w)){
        if(!g->idx[w]){ gc_scc(g,w); if(g->low[w]<g->low[v]) g->low[v]=g->low[w]; }
        else if(g->on[w] && g->idx[w]<g->low[v]) g->low[v]=g->idx[w];
    }
    if(g->low[v]==g->idx[v]){
        int first=g->nstk-1; while(g->stk[first]!=v) first--;
        int many=g->nstk-first>1;
        for(int k=first;k<g->nstk;k++){ int w=g->stk[k]; g->on[w]=0; g->cyc[w]=(unsigned char)(many || gc_has(g,w,w)); }
        g->nstk=first;
    }
}
void gc_analyze(void){
    AProg *p=gg->p; int ncls=1, nfn=p->nfuncs+1;
    for(int i=0;i<p->nclasses;i++) if(p->classes[i]->id+1>ncls) ncls=p->classes[i]->id+1;
    gg->gc_cls=(unsigned char*)xmalloc((size_t)ncls); memset(gg->gc_cls,0,(size_t)ncls);
    gg->gc_sub=(signed char*)xmalloc((size_t)ncls); memset(gg->gc_sub,-1,(size_t)ncls);
    gg->gc_clo=(unsigned char*)xmalloc((size_t)nfn); memset(gg->gc_clo,0,(size_t)nfn);
    if(getenv("PPY_NO_GC")) return;
    int *clo=(int*)xmalloc(sizeof(int)*(size_t)nfn), n=ncls;
    for(int i=0;i<nfn;i++) clo[i]=-1;
    for(int i=0;i<p->nfuncs;i++){ AFunc *fn=p->funcs[i]; if(!fn->unused && (fn->ncaps || dyn_defaults(fn))) clo[fn->id]=n++; }
    GcGraph g; memset(&g,0,sizeof g);
    g.fnode=n++; g.n=n;
    g.adj=(unsigned char*)xmalloc(((size_t)n*(size_t)n+7)/8); memset(g.adj,0,((size_t)n*(size_t)n+7)/8);
    for(int i=0;i<p->nclasses;i++){ AClass *c=p->classes[i];             /* an object: its fields */
        for(AClass *k=c;k;k=k->base) for(int j=0;j<k->nfields;j++){ AField *fd=k->fields[j];
            if(k->builtin && (!strcmp(fd->name,"__cause__")||!strcmp(fd->name,"__context__"))) continue;   /* (a chain: raise keeps it one, as CPython) */
            if(is_ptr(fd->ty) && !fd->over) gc_expand(&g,fd->ty,c->id,0); } }
    for(int i=0;i<p->nfuncs;i++){ AFunc *fn=p->funcs[i]; if(clo[fn->id]<0) continue;   /* a closure: its captures and defaults */
        int v=clo[fn->id];
        for(int k=0;k<fn->ncaps;k++) if(is_ptr(fn->caps[k]->ty)) gc_expand(&g,fn->caps[k]->ty,v,0);
        for(int k=0;k<fn->nparams;k++) if(fn->defaults[k] && k!=fn->star && k!=fn->dstar && is_ptr(fn->params[k]->ty)) gc_expand(&g,fn->params[k]->ty,v,0);
        gc_edge(&g,g.fnode,v); }
    for(int i=0;i<p->nfuncs;i++){ AFunc *m=p->funcs[i];                  /* a function value: a method bound to its object */
        if(m->cls && m->value_used && !m->is_static && !m->unused){ Ty ct; memset(&ct,0,sizeof ct); ct.k=TY_OBJ; ct.cls=m->cls; gc_expand(&g,&ct,g.fnode,0); } }
    g.idx=(int*)xmalloc(sizeof(int)*(size_t)n); g.low=(int*)xmalloc(sizeof(int)*(size_t)n); g.stk=(int*)xmalloc(sizeof(int)*(size_t)n);
    g.on=(unsigned char*)xmalloc((size_t)n); g.cyc=(unsigned char*)xmalloc((size_t)n);
    memset(g.idx,0,sizeof(int)*(size_t)n); memset(g.on,0,(size_t)n); memset(g.cyc,0,(size_t)n);
    for(int v=0;v<n;v++) if(!g.idx[v]) gc_scc(&g,v);
    for(int i=0;i<p->nclasses;i++) if(g.cyc[p->classes[i]->id]){ gg->gc_cls[p->classes[i]->id]=1; gg->gc_on=1; }
    for(int i=0;i<nfn;i++) if(clo[i]>=0 && g.cyc[clo[i]]){ gg->gc_clo[i]=1; gg->gc_on=1; }
    gg->gc_func=g.cyc[g.fnode];
    if(getenv("PPY_GC_TRACE")){
        for(int i=0;i<p->nclasses;i++) if(gg->gc_cls[p->classes[i]->id]) fprintf(stderr,"gc: class %s\n",p->classes[i]->name);
        for(int i=0;i<p->nfuncs;i++) if(gg->gc_clo[p->funcs[i]->id]) fprintf(stderr,"gc: closures of %s\n",p->funcs[i]->name);
        if(gg->gc_func) fprintf(stderr,"gc: function values\n"); }
    free(g.adj); free(g.idx); free(g.low); free(g.stk); free(g.on); free(g.cyc); free(clo);
}
/* GCC<id>: the offsets of a class's fields that can hold CYC objects (rt_gc_kids) */
const char *gc_class_map(AClass *c){
    static char lab[32]; snprintf(lab,sizeof lab,"GCC%d",c->id);
    Buf m={0}; int n=0;
    for(AClass *k=c;k;k=k->base) for(int i=0;i<k->nfields;i++){ AField *fd=k->fields[i];
        if(is_ptr(fd->ty) && !fd->over && gc_type(fd->ty)){ buf_printf(&m,",%d",fd->offset); n++; } }
    buf_printf(&gg->data,"align 4\n%s dd %d%s   ; %s: its references to what can be in a cycle\n",lab,n,m.s?m.s:"",c->name);
    free(m.s);
    buf_printf(&gg->gc_tab,"        dd %s,%s\n",class_label(c,1),lab); gg->gc_ntab++;
    return lab;
}
/* GCF<id>: the offsets of a closure's captures and defaults that can hold CYC objects (cells of such variables too) */
void gc_closure_map(AFunc *fn){
    layout_caps(fn); layout_defaults(fn);
    Buf m={0}; int n=0;
    for(int k=0;k<fn->ncaps;k++){ AVar *cv=fn->caps[k]; if(is_ptr(cv->ty) && gc_type(cv->ty)){ buf_printf(&m,",%d",cv->capoff); n++; } }
    for(int k=0;k<fn->nparams;k++) if(fn->defaults[k] && k!=fn->star && k!=fn->dstar && is_ptr(fn->params[k]->ty) && gc_type(fn->params[k]->ty)){ buf_printf(&m,",%d",16+fn->defoff[k]); n++; }
    buf_printf(&gg->data,"align 4\nGCF%d dd %d%s   ; closures of %s: their references to what can be in a cycle\n",fn->id,n,m.s?m.s:"",fn_display_name(fn));
    free(m.s);
    buf_printf(&gg->gc_tab,"        dd %s,GCF%d\n",fnl(fn,LB_FREE),fn->id); gg->gc_ntab++;
}

