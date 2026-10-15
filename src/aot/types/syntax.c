/* ========================= Typed compiler, types: syntax made by the checker =========================
   Helpers building frontend trees (assignments, with, del, print ...)
   for the code the checker writes in place of the program's. */

#include "check.h"

Expr *xnew(ExprKind k, int line){ Expr *e=MPY_NEW0(Expr); e->kind=k; e->line=line; return e; }
void xpush(Expr *e, Expr *item){
    if(e->count==e->cap){ e->cap=e->cap?e->cap*2:4; e->items=(Expr**)xrealloc(e->items,sizeof(Expr*)*(size_t)e->cap); }
    e->items[e->count++]=item;
}
MPY_NORETURN static void parse_fail(AotUnit *u, int line, const char *msg){
    fprintf(stderr,"%s:%d: error: %s\n",u->path?u->path:"<source>",line,msg); exit(1);
}

/* The statements' parts, as the frontend (py_front.c) spelled them out. */
Expr *aot_expr(AotUnit *u, Expr **slot){ (void)u; return *slot; }

int aot_is_annotation_only(AotUnit *u, Stmt *s){ (void)u; return s->kind==STMT_EXPR && s->ann_only; }

/* targets "=" ... value | target augop value | target ":" annotation ["=" value] */
AAssign *aot_assign(AotUnit *u, Stmt *s){
    if(s->aux) return (AAssign*)s->aux;
    AAssign *a=MPY_NEW0(AAssign);
    if(s->ntargets>16) parse_fail(u,s->line,"too many assignment targets");
    for(int i=0;i<s->ntargets;i++) a->target[a->ntarget++]=s->targets[i];
    a->value=s->value; a->aug=(TokKind)s->aug; a->ann=s->ann;
    s->aux=a; return a;
}
AWith *aot_with(AotUnit *u, Stmt *s){
    if(s->aux) return (AWith*)s->aux;
    AWith *w=MPY_NEW0(AWith);
    if(s->ntargets>8) parse_fail(u,s->line,"too many with items");
    for(int i=0;i<s->ntargets;i++){ w->e[w->n]=s->targets[i]; w->as[w->n]=s->withas[i]; w->n++; }
    s->aux=w; return w;
}
ADel *aot_del(AotUnit *u, Stmt *s){
    if(s->aux) return (ADel*)s->aux;
    ADel *d=MPY_NEW0(ADel);
    if(s->ntargets>16) parse_fail(u,s->line,"too many del targets");
    for(int i=0;i<s->ntargets;i++) d->t[d->n++]=s->targets[i];
    s->aux=d; return d;
}
/* print(a, b, ..., sep=..., end=...) */
APrint *aot_print(AotUnit *u, Stmt *s){
    if(s->kind!=STMT_EXPR || s->ann_only || !s->expr || s->expr->kind!=EXPR_CALL || s->expr->a->kind!=EXPR_NAME || strcmp(s->expr->a->name,"print")) return NULL;
    if(s->aux) return (APrint*)s->aux;
    APrint *pr=MPY_NEW0(APrint);
    Expr *call=s->expr;
    for(int i=0;i<call->count;i++){ Expr *v=call->items[i];
        if(v->akind==3){
            if(!strcmp(v->kw,"sep")) pr->sep=v; else if(!strcmp(v->kw,"end")) pr->end=v;
            else if(!strcmp(v->kw,"file")) pr->file=v; else if(!strcmp(v->kw,"flush")) pr->flush=v;
            else parse_fail(u,s->line,"print() takes only sep=, end=, file= and flush= keywords");
        } else if(v->akind==2) parse_fail(u,s->line,"print(**kwargs) is not supported in compiled code");
        else {
            if(pr->n==32) parse_fail(u,s->line,"too many print arguments");
            pr->star[pr->n]=(char)(v->akind==1);
            pr->args[pr->n++]=v;
        }
        v->akind=0;                                         /* (values, as any other expression) */
    }
    s->aux=pr; return pr;
}

/* the module a from-import names: relative ones (from .m import x) resolved against the importing
   module's package; NULL (an error printed) if there is none */
char *aot_from_import_module(AotUnit *u, Stmt *s){
    if(!s->level) return xstrdup2(s->module);
    char base[600]; snprintf(base,sizeof base,"%s",u->name);
    const char *bn=u->path?strrchr(u->path,'/'):NULL; bn=bn?bn+1:u->path;
    int pkg= bn && !strncmp(bn,"__init__.",9);
    for(int i=pkg?1:0;i<s->level;i++){ char *dot=strrchr(base,'.');
        if(!dot || !strcmp(u->name,"__main__")){
            fprintf(stderr,"%s:%d: error: ImportError: attempted relative import %s\n",u->path?u->path:"<source>",s->line,
                    !strchr(u->name,'.') && !pkg ? "with no known parent package" : "beyond top-level package");
            return NULL; }
        *dot=0; }
    if(s->module){ size_t n=strlen(base); snprintf(base+n,sizeof base-n,".%s",s->module); }
    return xstrdup2(base);
}

