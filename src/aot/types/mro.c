/* ========================= Typed compiler, types: several base classes =========================
   Classes with several bases: the C3 method resolution order, super()
   rewritten for it. */

#include "check.h"

/* A class with several bases keeps the first as its layout parent; the members of the other
   classes of its C3 MRO are copied into it (their methods made again from the source, so that
   self is this class), and super() goes to the next class of the MRO that has the method:
   a copy named __mro_<class>_<method>. */
/* rest ("server.Base", "Inner") of a dotted name after the symbol ms (a module or a class): what it names */
ASym *dotted_rest(Ck *c, ASym *ms, const char *rest){
    static ASym found[8]; static int nf;
    while(ms && *rest){ const char *dot=strchr(rest,'.'); char part[256]; snprintf(part,sizeof part,"%.*s",dot?(int)(dot-rest):(int)strlen(rest),rest);
        if(ms->kind==AS_MODULE){ AModule *m=(AModule*)ms->p; ASym *n=module_sym(m,part);
            if(!n){ char full[512]; snprintf(full,sizeof full,"%s.%s",m->name,part); AModule *sub=find_module(c->p,full);   /* package.submodule */
                if(sub){ ASym *f=&found[nf++&7]; f->kind=AS_MODULE; f->p=sub; f->name=sub->name; n=f; } }
            ms=n; }
        else if(ms->kind==AS_CLASS){ AClass *k=aot_nested_class(c->p,(AClass*)ms->p,part); if(!k) return NULL;
            ASym *f=&found[nf++&7]; f->kind=AS_CLASS; f->p=k; f->name=k->name; ms=f; }
        else return NULL;
        rest= dot ? dot+1 : rest+strlen(rest); }
    return ms;
}
static AClass *resolve_base(Ck *c, AClass *cls, const char *name, int line){
    ASym *s=cls->encl ? local_class_in(cls->encl,name) : NULL;
    if(!s) s=global_sym(cls->mod,name);
    const char *dot=strchr(name,'.');
    if(dot){ char mn[256]; snprintf(mn,sizeof mn,"%.*s",(int)(dot-name),name);
        ASym *ms=cls->encl ? local_class_in(cls->encl,mn) : NULL; if(!ms) ms=global_sym(cls->mod,mn);
        if(ms && ms->kind==AS_SYS && !strcmp((const char*)ms->p,"abc") && !strcmp(dot+1,"ABC")) return abc_class(c);
        if(ms && ms->kind==AS_CLASS && !strchr(dot+1,'.')){ AClass *k=aot_nested_class(c->p,(AClass*)ms->p,dot+1); if(!k) ck_err(c,line,"base class '%s' is not a class",name); return k; }
        s=dotted_rest(c,ms,dot+1); }
    if(!s || s->kind!=AS_CLASS) ck_err(c,line,"base class '%s' is not a class",name);
    return (AClass*)s->p;
}
static void mro_of(AClass *k, AClass ***out, int *n){
    if(k->mro){ *out=k->mro; *n=k->nmro; return; }
    static AClass *buf[64]; int m=0; for(AClass *x=k;x && m<64;x=x->base) buf[m++]=x;
    *out=PPY_NEW_ARR(AClass*,m); memcpy(*out,buf,sizeof(AClass*)*(size_t)m); *n=m;
}
static int c3(Ck *c, AClass *cls, AClass **bases, int nb, int line){
    AClass **seq[17]; int len[17], pos[17], ns=0;
    for(int i=0;i<nb;i++){ mro_of(bases[i],&seq[ns],&len[ns]); pos[ns++]=0; }
    seq[ns]=bases; len[ns]=nb; pos[ns++]=0;
    AClass *out[64]; int n=0; out[n++]=cls;
    for(;;){
        int left=0; for(int i=0;i<ns;i++) if(pos[i]<len[i]) left=1;
        if(!left) break;
        AClass *pick=NULL;
        for(int i=0;i<ns && !pick;i++){ if(pos[i]>=len[i]) continue; AClass *h=seq[i][pos[i]]; int bad=0;
            for(int j=0;j<ns && !bad;j++) for(int q=pos[j]+1;q<len[j];q++) if(seq[j][q]==h){ bad=1; break; }
            if(!bad) pick=h; }
        if(!pick) ck_err(c,line,"Cannot create a consistent method resolution order (MRO) for bases");
        if(n<64) out[n++]=pick;
        for(int i=0;i<ns;i++) if(pos[i]<len[i] && seq[i][pos[i]]==pick) pos[i]++; }
    cls->mro=PPY_NEW_ARR(AClass*,n); memcpy(cls->mro,out,sizeof(AClass*)*(size_t)n); cls->nmro=n;
    return n;
}
typedef struct { AClass *cls; AClass *from; const char *self; Ck *c; int line; } SuperCtx;
/* the class after `from` in cls's MRO with method n of its own */
static AClass *mro_next(AClass *cls, AClass *from, const char *n){
    int i=0; while(i<cls->nmro && cls->mro[i]!=from) i++;
    for(i++;i<cls->nmro;i++){ AClass *k=cls->mro[i]; if(k->def && body_def(k->def,n)) return k;
        if(!k->def){ for(int j=0;j<k->nmethods;j++) if(!strcmp(k->methods[j]->name,n)) return k; } }
    return NULL;
}
static void mro_name(char *out, size_t sz, AClass *k, const char *n){ snprintf(out,sz,"__mro_%s_%s",k->symname,n); for(char *p=out;*p;p++) if(*p=='.'||*p=='#'||*p=='<'||*p=='>') *p='_'; }
static void super_rewrite_expr(Expr *e, SuperCtx *sc);
static void super_rewrite_stmts(Stmt **b, int n, SuperCtx *sc);
static void super_rewrite_expr(Expr *e, SuperCtx *sc){
    if(!e) return;
    if(e->kind==EXPR_CALL && e->a && e->a->kind==EXPR_ATTRIBUTE && e->a->a->kind==EXPR_CALL && e->a->a->a->kind==EXPR_NAME && !strcmp(e->a->a->a->name,"super") && !e->a->a->count){
        const char *n=e->a->name; AClass *y=mro_next(sc->cls,sc->from,n);
        if(!y){ if(!strcmp(n,"__init__")){ for(int i=0;i<e->count;i++) super_rewrite_expr(e->items[i],sc); Expr *z=xnew(EXPR_NONE,e->line); *e=*z; return; }
            ck_err(sc->c,e->line,"super(): no class after %s in %s's MRO has '%s'",sc->from->name,sc->cls->name,n); }
        char nm[400]; mro_name(nm,sizeof nm,y,n);
        Expr *at=xnew(EXPR_ATTRIBUTE,e->line); at->a=name_expr(sc->self,e->line); at->name=xstrdup2(nm); e->a=at; }
    else if(e->kind==EXPR_CALL && e->a && e->a->kind==EXPR_ATTRIBUTE && e->a->a->kind==EXPR_NAME && e->count && e->items[0]->kind==EXPR_NAME && !e->items[0]->akind
            && !strcmp(e->items[0]->name,sc->self)){                      /* Mixin.m(self, ...): the copy of Mixin's m */
        ASym *s=global_sym(sc->cls->mod,e->a->a->name); AClass *x= s && s->kind==AS_CLASS ? (AClass*)s->p : NULL;
        int vb=0; for(int i=0;x && i<sc->cls->nvbases;i++) if(sc->cls->vbases[i]==x) vb=1;
        if(vb && x->def && body_def(x->def,e->a->name)){ char nm[400]; mro_name(nm,sizeof nm,x,e->a->name);
            Expr *at=xnew(EXPR_ATTRIBUTE,e->line); at->a=name_expr(sc->self,e->line); at->name=xstrdup2(nm); e->a=at;
            for(int i=1;i<e->count;i++) e->items[i-1]=e->items[i];
            e->count--; } }
    super_rewrite_expr(e->a,sc); super_rewrite_expr(e->b,sc); super_rewrite_expr(e->c,sc); super_rewrite_expr(e->d,sc);
    for(int i=0;i<e->count;i++) super_rewrite_expr(e->items[i],sc);
    for(int i=0;i<e->vcount;i++) super_rewrite_expr(e->vals[i],sc);
    for(int i=0;i<e->nclause;i++){ CompClause *cl=&e->clauses[i]; super_rewrite_expr(cl->iter,sc); for(int k=0;k<cl->ncond;k++) super_rewrite_expr(cl->conds[k],sc); }
    if(e->edefaults) for(int i=0;i<e->neparam;i++) super_rewrite_expr(e->edefaults[i],sc);
}
static void super_rewrite_stmts(Stmt **b, int n, SuperCtx *sc){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_CLASS_DEF) continue;
        super_rewrite_expr(s->expr,sc); super_rewrite_expr(s->expr2,sc); super_rewrite_expr(s->value,sc);
        for(int k=0;k<s->ntargets;k++) super_rewrite_expr(s->targets[k],sc);
        if(s->kind==STMT_FUNCTION_DEF){ AClass *sv=sc->from; super_rewrite_stmts(s->body,s->body_count,sc); sc->from=sv; continue; }
        super_rewrite_stmts(s->body,s->body_count,sc); super_rewrite_stmts(s->orelse,s->orelse_count,sc); }
}
/* class D(A, B, ...): MRO, the members of the classes not on D's layout chain, the MRO copies of methods */
void several_bases(Ck *c, AClass *cls){
    Stmt *def=cls->def; int line=def->line;
    AClass *bases[16]; int nb=0;
    for(int i=0;i<def->param_count && nb<16;i++){ AClass *b=resolve_base(c,cls,def->params[i],line);
        if(b->def){ AModule *save=c->mod; c->mod=b->mod; fill_class(c,b); c->mod=save; }
        if(aot_is_exception(b) && i) ck_err(c,line,"an exception class with several bases is not supported in compiled code");
        bases[nb++]=b; }
    cls->base=bases[0];                                          /* the layout parent: a base with an __init__ (state), else the first */
    for(int i=0;i<nb;i++) if(aot_find_method(bases[i],"__init__") || aot_is_exception(bases[i])){ cls->base=bases[i]; break; }
    c3(c,cls,bases,nb,line);
    AClass *vb[64]; int nv=0;
    for(int i=1;i<cls->nmro;i++){ AClass *k=cls->mro[i]; if(!aot_subclass(cls->base,k) && nv<64) vb[nv++]=k; }
    cls->vbases=PPY_NEW_ARR(AClass*,nv+1); memcpy(cls->vbases,vb,sizeof(AClass*)*(size_t)nv); cls->nvbases=nv;
    Stmt **add=NULL; int nadd=0, cadd=0;
    #define ADD(st) do{ if(nadd==cadd){ cadd=cadd?cadd*2:16; add=(Stmt**)xrealloc(add,sizeof(Stmt*)*(size_t)cadd); } add[nadd++]=(st); }while(0)
    for(int i=1;i<cls->nmro;i++){ AClass *k=cls->mro[i]; if(!k->def) continue;
        Stmt *copy=py_front_copy(k->mod->unit->path,k->def);
        if(!copy) ck_err(c,line,"cannot copy class %s",k->name);
        int vbase=0; for(int j=0;j<nv;j++) if(vb[j]==k) vbase=1;
        for(int j=0;j<copy->body_count;j++){ Stmt *m=copy->body[j];
            if(m->kind==STMT_FUNCTION_DEF){
                if(m->decorator_count) continue;                            /* (decorated ones: through the class they are in) */
                char nm[400]; mro_name(nm,sizeof nm,k,m->name);
                SuperCtx sc={cls,k,m->param_count?m->params[0]:"self",c,m->line}; super_rewrite_stmts(m->body,m->body_count,&sc);
                const char *orig=m->name; m->name=xstrdup2(nm); if(k->mod!=cls->mod) m->src_mod=k->mod; ADD(m);
                if(!body_def(def,orig)){                                      /* the visible one: the first in the MRO */
                    int first=1; for(int q=1;q<i;q++){ AClass *e2=cls->mro[q]; if(e2->def && body_def(e2->def,orig)) first=0; }
                    if(first){ Stmt *again=py_front_copy(k->mod->unit->path,k->def); Stmt *vm=body_def(again,orig);
                        super_rewrite_stmts(vm->body,vm->body_count,&sc); if(k->mod!=cls->mod) vm->src_mod=k->mod; ADD(vm); } } }
            else if(vbase && (m->kind==STMT_ASSIGN || (m->kind==STMT_EXPR && m->ann_only))){ if(k->mod!=cls->mod) m->src_mod=k->mod; ADD(m); }   /* a mixin's fields / class attributes */
        } }
    { SuperCtx sc={cls,cls,"self",c,line};                                        /* the class's own methods */
      for(int j=0;j<def->body_count;j++){ Stmt *m=def->body[j]; if(m->kind!=STMT_FUNCTION_DEF) continue;
          sc.self=m->param_count?m->params[0]:"self"; super_rewrite_stmts(m->body,m->body_count,&sc); } }
    for(int j=0;j<nadd;j++) stmt_add_body(def,add[j]);
    #undef ADD
}
/* Fields and methods; done once every class of the module is known (annotations may name them). */
