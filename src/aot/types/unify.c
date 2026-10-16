/* ========================= Typed compiler, types: checker state, unification =========================
   The checker's state, its errors, unification of types and what may
   be assigned to what. */

#include "check.h"

PPY_NORETURN void ck_err(Ck *c, int line, const char *fmt, ...){
    va_list ap; va_start(ap,fmt);
    AotUnit *u=c->mod?c->mod->unit:NULL;
    fprintf(stderr,"%s:%d: error: ",u&&u->path?u->path:"<source>",line);
    vfprintf(stderr,fmt,ap); fputc('\n',stderr);
    va_end(ap);
    if(aot_check_mode) aot_check_errors++;
    if(getenv("PPY_CKDEBUG") && c->fn) fprintf(stderr,"  (in %s%s%s, class %p%s, ncalls %d, direct %d, origin %p, ninsts %d, strict %d)\n",c->fn->cls?c->fn->cls->name:"",c->fn->cls?".":"",c->fn->name,(void*)c->fn->cls,c->fn->cls&&c->fn->cls->bare?" bare":"",c->fn->ncalls,c->fn->direct,(void*)c->fn->origin,c->fn->ninsts,c->strict);
    longjmp(c->fail,1);
}

/* ---- check mode: going on after an error (see CkTry) */
void ck_try_begin(Ck *c, CkTry *t){
    memcpy(t->outer,c->fail,sizeof(jmp_buf));
    t->mod=c->mod; t->fn=c->fn; t->none_lit=c->none_lit; t->mk_static=c->mk_static; t->site_callee=c->site_callee; t->ncscope=c->ncscope;
    t->coro_ok=c->coro_ok; t->ep_value=c->ep_value; t->in_except=c->in_except; t->reflected=c->reflected; t->sig_fn=c->sig_fn; t->sig_cls=c->sig_cls;
    t->nnn=c->nnn; t->alias_n=c->alias_n; t->alias_t=c->alias_t; t->nalias=c->nalias; t->discard=c->discard; t->nhst=c->nhst;
}
void ck_try_end(Ck *c, CkTry *t){ memcpy(c->fail,t->outer,sizeof(jmp_buf)); }
void ck_try_fail(Ck *c, CkTry *t){
    memcpy(c->fail,t->outer,sizeof(jmp_buf));
    c->mod=t->mod; c->fn=t->fn; c->none_lit=t->none_lit; c->mk_static=t->mk_static; c->site_callee=t->site_callee; c->ncscope=t->ncscope;
    c->coro_ok=t->coro_ok; c->ep_value=t->ep_value; c->in_except=t->in_except; c->reflected=t->reflected; c->sig_fn=t->sig_fn; c->sig_cls=t->sig_cls;
    c->nnn=t->nnn; c->alias_n=t->alias_n; c->alias_t=t->alias_t; c->nalias=t->nalias; c->discard=t->discard; c->nhst=t->nhst;
}
/* check mode: a function the check could not go through - nothing calls it and
   the types of its parameters are not known (they come from each call) */
void ck_unclear(Ck *c, AFunc *f){
    (void)c;
    if(!f->def || !f->mod || !f->mod->unit || !f->mod->unit->path) return;
    char names[200]; int n=0; names[0]=0;
    Stmt *d=f->def;
    for(int i=(f->cls && !f->is_static)?1:0;i<d->param_count;i++)
        if(!d->annotations || i>=d->annotation_cap || !d->annotations[i]){
            int k=snprintf(names+n,sizeof names-(size_t)n,"%s%s",n?", ":"",d->params[i]); if(k<0 || n+k>=(int)sizeof names) break; n+=k; }
    const char *q=f->cls?f->cls->name:NULL;
    if(n) fprintf(stderr,"%s:%d: unclear: %s%s%s() is not checked: its parameters %s have no types (annotate them)\n",
                  f->mod->unit->path,f->line,q?q:"",q?".":"",f->name,names);
    else fprintf(stderr,"%s:%d: unclear: %s%s%s() is generic (type variables): checked only for the types a program calls it with\n",
                 f->mod->unit->path,f->line,q?q:"",q?".":"",f->name);
}

/* a function body (not a module body): def, lambda, generator expression */
int is_func(AFunc *f){ return f && (f->def||f->lam||f->genexp); }
int is_sys_call(Expr *d, const char *name){ return d && d->kind==EXPR_CALL && d->a && d->a->kind==EXPR_ATTRIBUTE && !strcmp(d->a->name,name) && d->a->a->kind==EXPR_NAME && !strcmp(d->a->a->name,"sys"); }
void cs_push(Ck *c, const char *name, AVar *v, int line){
    if(c->ncscope==256) ck_err(c,line,"nested too deeply");
    c->cscope[c->ncscope].name=name; c->cscope[c->ncscope].sym.name=(char*)name;
    c->cscope[c->ncscope].sym.kind=AS_VAR; c->cscope[c->ncscope].sym.p=v; c->cscope[c->ncscope].fn=c->fn; c->ncscope++;
}
/* a hidden variable of the current scope (comprehension / except / key variable) */
AVar *scope_hidden(Ck *c, const char *name){ return is_func(c->fn) ? hidden_local(c->fn,name,NULL) : new_global(c,c->mod,name); }

static int occurs(Ty *v, Ty *t){
    t=ty_find(t); if(t==v) return 1;
    if(t->k==TY_TUPLE||t->k==TY_FUNC||t->k==TY_GEN){ for(int i=0;i<t->nelems;i++) if(t->elems[i] && occurs(v,t->elems[i])) return 1; if(t->k==TY_TUPLE) return 0; }
    if(t->k==TY_DICT && t->key && occurs(v,t->key)) return 1;
    return t->elem ? occurs(v,t->elem) : 0;
}
/* t (found, not shared) may be None from now on */
void set_opt(Ck *c, Ty *t){ if(t->opt || ty_shared(t) || t->k==TY_VOID) return; t->opt=1; if(t->id<c->pass_first) c->changed=1; }
static void bind(Ck *c, Ty *var, Ty *t){
    if(var->opt && t->k==TY_VAR) set_opt(c,t);
    var->link=t; if(var->id<c->pass_first) c->changed=1;
}
int unify(Ck *c, Ty *a, Ty *b){
    a=ty_find(a); b=ty_find(b);
    if(a==b) return 1;
    if(a->k==TY_VAR && b->k==TY_VAR){ if(a->id<b->id) bind(c,b,a); else bind(c,a,b); return 1; }   /* younger -> older */
    if(a->k==TY_VAR || b->k==TY_VAR){
        Ty *v=a->k==TY_VAR?a:b, *t=a->k==TY_VAR?b:a;
        if(occurs(v,t)) return 0;
        if(ty_shared(t)) t=ty_copy(t);                      /* a type of its own: it may become optional */
        if(v->opt) set_opt(c,t);
        bind(c,v,t); return 1;
    }
    if(a->k!=b->k) return 0;
    if(a->opt!=b->opt){ set_opt(c,a); set_opt(c,b); }       /* None flows both ways */
    if(a->k==TY_DICT && !unify(c,ty_dkey(a),ty_dkey(b))) return 0;
    if((a->k==TY_LIST||a->k==TY_FUNC||a->k==TY_GEN) && a->tup!=b->tup) return 0;
    if(a->k==TY_FUNC && a->ndef!=b->ndef){ int m=a->ndef<b->ndef?a->ndef:b->ndef; a->ndef=b->ndef=m; }   /* defaults: the ones both have */
    if(a->k==TY_FUNC && a->kwo!=b->kwo){ int m=a->kwo<b->kwo?a->kwo:b->kwo; a->kwo=b->kwo=m; }   /* keyword-only: the ones both have */
    if(a->k==TY_TYPE && a!=b && !ty_shared(a) && !ty_shared(b)){          /* class values: their common base (or a subclass of it) */
        if(a->cls && b->cls && a->cls!=b->cls){ AClass *cb=a->cls; while(cb && !aot_subclass(b->cls,cb)) cb=cb->base; a->cls=b->cls=cb; a->tup=b->tup=cb?1:0; }
        else if(a->cls && b->cls){ if(a->tup||b->tup) a->tup=b->tup=1; }
        else { a->cls=b->cls=NULL; a->tup=b->tup=0; } }
    if(a->k==TY_FUNC && !a->names && b->names) a->names=b->names;            /* (names: the ones a function gave) */
    if(a->k==TY_FUNC && !b->names && a->names) b->names=a->names;
    if(a->k==TY_GEN && a->nelems==2 && b->nelems==2){                  /* send() values; return values unless one is None */
        if(!unify(c,a->elems[0],b->elems[0])) return 0;
        if(ty_find(a->elems[1])->k!=TY_VOID && ty_find(b->elems[1])->k!=TY_VOID && !unify(c,a->elems[1],b->elems[1])) return 0; }
    if(a->k==TY_TUPLE||a->k==TY_FUNC){ if(a->nelems!=b->nelems) return 0; if(a->k==TY_TUPLE && !names_eq(a,b)) return 0; for(int i=0;i<a->nelems;i++) if(!unify(c,a->elems[i],b->elems[i])) return 0; if(a->k==TY_TUPLE) return 1; }
    if(has_elem(a)) return unify(c,a->elem,b->elem);
    if(a->k==TY_OBJ) return a->cls==b->cls;
    return 1;
}
/* May a value of type src be stored where dst is expected? (instances: subclass -> base) */
/* a function taking *args where a fixed signature is wanted: an adapter packs the extra arguments */
static int func_adaptable(Ck *c, Ty *d, Ty *s){
    int nreg=s->nelems-1-((s->tup>>1)&1);
    if(d->nelems<nreg) return 0;
    for(int i=0;i<nreg;i++) if(!unify(c,d->elems[i],s->elems[i])) return 0;
    for(int i=nreg;i<d->nelems;i++) if(!unify(c,d->elems[i],s->elems[nreg])) return 0;
    return unify(c,d->elem,s->elem);
}
AClass *complex_class;                                    /* (the cmath prelude's) */
int aot_is_complex(Ty *t){ t=ty_find(t); return complex_class && t->k==TY_OBJ && t->cls==complex_class; }
int assignable(Ck *c, Ty *dst, Ty *src){
    Ty *d=ty_find(dst), *s=ty_find(src);
    if(aot_is_complex(d) && (s->k==TY_INT||s->k==TY_BOOL||s->k==TY_FLOAT)) return 1;   /* 1 + 2j: converted on the way */
    if(d->k==TY_FUNC && (d->tup&4) && s->k==TY_FUNC && !(s->tup&4)) return 1;   /* a function -> its endpoint adapter */
    if(s->k==TY_FUNC && (s->tup&4) && d->k==TY_FUNC && !(d->tup&4)){ Ty *p=ty_find(d->elems[0]); /* an Endpoint is also a plain Callable */
        return d->nelems==1 && !d->tup && unify(c,p,s->elems[0]) && unify(c,d->elem,s->elem); }
    if(d->k==TY_FUNC && s->k==TY_FUNC && !d->tup && (s->tup&1)) return func_adaptable(c,d,s);
    if(d->k==TY_OBJ && s->k==TY_OBJ) return aot_subclass(s->cls,d->cls);
    if(d->k==TY_FLOAT && (s->k==TY_INT||s->k==TY_BOOL)) return 1;      /* converted on the way */
    if(d->k==TY_INT && s->k==TY_BOOL) return 1;
    if(d->opt && !s->opt && d->k==s->k && d->k!=TY_VAR && d!=s){       /* a value where an optional one may go: */
        s->opt=1; int r=unify(c,d,s); s->opt=0; return r; }               /* (None flows into dst, not back into the value) */
    return unify(c,d,s);
}
void expect(Ck *c, Ty *dst, Ty *src, int line, const char *what){
    if(!assignable(c,dst,src)) ck_err(c,line,"type mismatch: %s is %s, cannot take %s",what,ty_name(dst),ty_name(src));
}
int numeric(Ty *t){ t=ty_find(t); return t->k==TY_INT||t->k==TY_BOOL||t->k==TY_FLOAT; }
/* values sorted(), min() and max() can order (as CPython can): numbers, str,
   tuples, lists of such, objects with __lt__ */
int orderable(Ty *t){
    t=ty_find(t);
    if(numeric(t)||t->k==TY_STR||t->k==TY_BYTES||t->k==TY_TUPLE||t->k==TY_VAR) return 1;
    if(t->k==TY_LIST) return orderable(t->elem);
    if(t->k==TY_OBJ) return aot_find_method(t->cls,"__lt__")!=NULL;
    return 0;
}
int is_var(Ty *t){ return ty_find(t)->k==TY_VAR; }

