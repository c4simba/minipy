/* ========================= Typed compiler, code generator: labels =========================
   Readable labels: module-qualified names (main.read_item,
   fastapi.FastAPI.get), unique in the listing. */

#include "gen.h"

/* ---- readable labels: module-qualified names (main.read_item,
   fastapi.FastAPI.get, main.outer.inner) made unique with _2, _3 ... ---- */

LabelSet g_labels;
static int label_reserved(const char *s){
    static const char *fixed[]={"start","i_end","mem_end","stack_top",NULL};
    for(int i=0;fixed[i];i++) if(!strcmp(s,fixed[i])) return 1;
    if(!strncmp(s,"rt_",3)||!strncmp(s,"RT_",3)||!strncmp(s,"DYN_",4)||!strncmp(s,"VTX_",4)||!strncmp(s,"DTX_",4)||!strncmp(s,"CI_",3)||!strncmp(s,"TYD_",4)||!strcmp(s,"TYQ")||!strcmp(s,"TYQN")) return 1;
    static const char *families[]={"L","S","B","Z","P","FC","RL","TD","FMT","AD","CI","CT","CTI","GCC","GCF",NULL};   /* numbered internal labels */
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

const char **g_fnlabels[LB_KINDS];      /* per function id */
const char **g_vtlabels, **g_dtlabels, **g_glabels, **g_mlabels;
static void fn_qualname(AFunc *fn, char *out, size_t n){
    if(fn->outer){ snprintf(out,n,"%s.%s",fnl(fn->outer,LB_CODE),fn->lam?"lambda":fn->genexp?"genexpr":fn->name); return; }   /* under the outer function's label */
    if(!fn->def){ snprintf(out,n,"%s.module_body",mod_prefix(fn->mod)); return; }
    if(fn->cls){ snprintf(out,n,"%s.%s.%s",mod_prefix(fn->cls->mod),fn->cls->name,fn->name); return; }   /* (a mixin's copy: in its own module) */
    ASym *s=symtab_find(&fn->mod->syms,fn->name);
    snprintf(out,n,"%s.%s%s",mod_prefix(fn->mod),fn->name,s && s->kind==AS_VAR?".def":"");   /* decorated: the name is the decorated value */
}
/* a function's labels: its code, and what is made of it (static value, closure
   destroy, bound-method code, task entries, generator body and descriptor) */
const char *fnl(AFunc *fn, int kind){
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
const char *class_label(AClass *c, int destroy){
    const char ***tab=destroy?&g_dtlabels:&g_vtlabels;
    if(!*tab){ *tab=(const char**)xmalloc(sizeof(char*)*(size_t)(gg->p->nclasses+1)); memset(*tab,0,sizeof(char*)*(size_t)(gg->p->nclasses+1)); }
    if(!(*tab)[c->id]){ char full[300];
        if(c->builtin) snprintf(full,sizeof full,"%s.%s",c->name,destroy?"destroy":"vtable");
        else snprintf(full,sizeof full,"%s.%s.%s",mod_prefix(c->mod),c->name,destroy?"destroy":"vtable");
        (*tab)[c->id]=unique_label(full); }
    return (*tab)[c->id];
}
const char *global_label(AVar *v){
    if(!g_glabels){ g_glabels=(const char**)xmalloc(sizeof(char*)*(size_t)(gg->p->nglobals+1)); memset(g_glabels,0,sizeof(char*)*(size_t)(gg->p->nglobals+1)); }
    if(!g_glabels[v->id]){ char full[300]; snprintf(full,sizeof full,"%s.%s",mod_prefix(v->mod),v->name); g_glabels[v->id]=unique_label(full); }
    return g_glabels[v->id];
}
const char *module_label(AModule *m){
    if(!g_mlabels){ g_mlabels=(const char**)xmalloc(sizeof(char*)*(size_t)(gg->p->nmods+1)); memset(g_mlabels,0,sizeof(char*)*(size_t)(gg->p->nmods+1)); }
    if(!g_mlabels[m->index]){ char full[300]; snprintf(full,sizeof full,"%s.imported",mod_prefix(m)); g_mlabels[m->index]=unique_label(full); }
    return g_mlabels[m->index];
}

