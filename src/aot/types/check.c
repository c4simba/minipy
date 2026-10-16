/* ========================= Typed compiler, types: the whole program =========================
   Class attributes, operator methods, class layouts; aot_check: the
   passes over the whole program. */

#include "check.h"

/* the values of a class's attributes, in order: each sees the ones before by name */
void ck_class_attrs(Ck *c, AClass *cls){
    int save=c->ncscope;
    for(int k=0;k<cls->nfields;k++){ AField *fd=cls->fields[k];
        if(fd->init){ Ty *t=ck_expr_want(c,fd->init,fd->ty); no_void(c,t,fd->init->line); char what[160]; snprintf(what,sizeof what,"field %s.%s",cls->name,fd->name); expect(c,fd->ty,t,fd->init->line,what); }
        if(fd->cvar) cs_push(c,fd->name,fd->cvar,fd->init?fd->init->line:1); }
    c->ncscope=save;
}
/* a method only called by name: not a dunder method (the run time may call those), not one
   overriding another or overridden (calls through the class's table) */
static int plain_method(AProg *p, AFunc *f){
    const char *n=f->name; size_t l=strlen(n);
    if(l>4 && n[0]=='_' && n[1]=='_' && n[l-1]=='_' && n[l-2]=='_') return 0;
    if(f->is_static || f->is_property || f->decovar) return 0;
    for(AClass *b=f->cls->base;b;b=b->base) if(aot_find_method(b,n)) return 0;
    for(int i=0;i<p->nclasses;i++){ AClass *k=p->classes[i];
        for(AClass *b=k->base;b;b=b->base) if(b==f->cls){ for(int j=0;j<k->nmethods;j++) if(!strcmp(k->methods[j]->name,n)) return 0; } }
    return 1;
}
/* an operator's method (__iadd__, __eq__, ...): what an operator calls, giving its argument's type */
static int operator_method(AFunc *f){
    const char *n=f->name; size_t l=strlen(n);
    if(!(l>4 && n[0]=='_' && n[1]=='_' && n[l-1]=='_' && n[l-2]=='_') || f->is_static || f->nparams<2) return 0;
    return strcmp(n,"__init__") && strcmp(n,"__new__") && strcmp(n,"__init_subclass__") && strcmp(n,"__set_name__");
}
/* a method of a generic class only copies of which are made (and that no call names itself) */
static int generic_original(AFunc *f){
    return f->cls && (f->cls->ninsts || f->cls->generic) && !f->cls->origin && !f->cls->constructed && !f->ncalls && !f->direct;
}
/* a class's generic __eq__ / __lt__ (other: T): their copies for its own objects (what containers and sort use) */
static void cmp_instances(Ck *c){
    AProg *p=c->p;
    for(int i=0;i<p->nclasses;i++){ AClass *k=p->classes[i];
        if(k->builtin || !k->constructed || k->generic) continue;
        for(int w=0;w<2;w++){ AFunc *m=aot_find_method(k,w?"__lt__":"__eq__");
            if(!m || !m->pristine || m->nparams<2 || method_overridden(c,m)) continue;
            unsigned omit=0; int extra_ok=1;                    /* (__lt__(self, other, context=None): the rest left None) */
            for(int q=2;q<m->nparams;q++){ if(q<16 && m->defaults[q] && m->defaults[q]->kind==EXPR_NONE) omit|=1u<<q; else extra_ok=0; }
            if(!extra_ok) continue;
            AFunc *have= w ? k->lt_inst : k->eq_inst; if(have) continue;
            Ty *at[16]={0}; at[1]=ty_new(TY_OBJ,NULL,k);
            AModule *save=c->mod; c->mod=m->mod;
            AFunc *g=instance_for_none(c,m,at,NULL,omit,0,0,m->line);
            c->mod=save;
            g->ncalls++;
            if(w) k->lt_inst=g; else k->eq_inst=g; } }
}
int aot_check_mode;
int aot_check_errors;

/* check mode: one step of the check, an error in it reported and what it was
   about marked as failed (*failed); otherwise just the step */
static void module_step(Ck *c, AModule *m, void (*step)(Ck *c, AModule *m)){
    if(!aot_check_mode){ step(c,m); return; }
    if(m->check_failed) return;
    CkTry t; ck_try_begin(c,&t);
    if(!setjmp(c->fail)){ step(c,m); ck_try_end(c,&t); }
    else { ck_try_fail(c,&t); m->check_failed=1; }
}
static void body_step(Ck *c, AFunc *f){
    if(!aot_check_mode){ ck_body(c,f); return; }
    if(f->check_failed) return;
    CkTry t; ck_try_begin(c,&t);
    if(!setjmp(c->fail)){ ck_body(c,f); ck_try_end(c,&t); }
    else { ck_try_fail(c,&t); f->check_failed=1; f->unused=1; }
}
static void class_attrs_step(Ck *c, AClass *cls){
    if(!aot_check_mode){ ck_class_attrs(c,cls); return; }
    if(cls->check_failed || cls->mod->check_failed) return;
    CkTry t; ck_try_begin(c,&t);
    if(!setjmp(c->fail)){ ck_class_attrs(c,cls); ck_try_end(c,&t); }
    else { ck_try_fail(c,&t); cls->check_failed=1; }
}

static void check_all(Ck *c){
    AProg *p=c->p;
    if(!aot_check_mode) cmp_instances(c);
    else { CkTry t; ck_try_begin(c,&t); if(!setjmp(c->fail)){ cmp_instances(c); ck_try_end(c,&t); } else ck_try_fail(c,&t); }
    for(int i=0;i<p->nclasses;i++){ AClass *cls=p->classes[i];           /* class attributes, in the module scope */
        if(cls->encl) continue;                                     /* (a function's: where its class statement is) */
        c->mod=cls->mod; c->fn=cls->mod->body; class_attrs_step(c,cls);
    }
    AFunc **later=NULL; int nlater=0;
    for(int i=0;i<p->nfuncs;i++){ AFunc *f=p->funcs[i]; c->fn=f; c->mod=f->mod;
        if(f->outer || f->unused || f->check_failed) continue;  /* nested functions are checked where they are defined */
        if(f->cls && f->cls->mixin && !f->cls->constructed) continue;   /* (a mixin's own methods: its copies are checked) */
        if(generic_original(f)) continue;                           /* (a generic class only its copies of are made: those are) */
        if(c->strict && f->def && !f->ncalls && !f->direct && (!f->cls || plain_method(p,f) || f->pristine || operator_method(f))){   /* a library function (or method; a generic one) no call has */
            int unknown=0; for(int k=0;k<f->nparams;k++) if(!ty_known(f->params[k]->ty)) unknown=1;   /* reached yet: after the others */
            if(unknown){ later=(AFunc**)xrealloc(later,sizeof(AFunc*)*(size_t)(nlater+1)); later[nlater++]=f; continue; }
        }
        body_step(c,f); }
    for(int i=0;i<nlater;i++){ AFunc *f=later[i]; c->fn=f; c->mod=f->mod;   /* called after all (a call checked later in this pass): checked; */
        if(f->ncalls || f->direct) body_step(c,f);                            /* else this program never runs it */
        else { f->unused=1; if(aot_check_mode && !f->check_failed) ck_unclear(c,f); } }
    free(later);
}

/* Element types nothing constrains (e.g. a list that stays empty) default to int. */
Ty *default_of(Ty *v, Ty *t){ if(!v->opt) return t; Ty *n=ty_copy(t); n->opt=1; return n; }   /* (None alone: Optional[...]) */
void default_elems(Ty *t){
    t=ty_find(t);
    if(t->k==TY_DICT && t->key){ Ty *k=ty_find(t->key); if(k->k==TY_VAR) k->link=default_of(k,TY_STR_T); else default_elems(k); }
    if(t->k==TY_LIST||t->k==TY_DICT||t->k==TY_SET||t->k==TY_GEN){ Ty *e=ty_find(t->elem); if(e->k==TY_VAR) e->link=default_of(e,TY_INT_T); else default_elems(e); }
    if(t->k==TY_TUPLE) for(int i=0;i<t->nelems;i++) default_elems(t->elems[i]);
    if(t->k==TY_GEN && t->nelems==2){ Ty *s=ty_find(t->elems[0]); if(s->k==TY_VAR) s->link=default_of(s,TY_INT_T); else default_elems(s);   /* nothing sent: int */
        Ty *r=ty_find(t->elems[1]); if(r->k==TY_VAR) r->link=TY_VOID_T; }
}
static void must_know(Ck *c, Ty *t, int line, const char *what, const char *name){
    { Ty *v=ty_find(t); if(v->k==TY_VAR && v->opt) v->link=default_of(v,TY_INT_T); }   /* only ever None */
    default_elems(t);
    if(!ty_known(t)) ck_err(c,line,"cannot infer the type of %s '%s'; add a type annotation",what,name);
}

static void layout_class(AClass *cls){
    if(cls->size) return;
    int off=12;                                               /* refcount, destroy, vtable */
    if(cls->base){ layout_class(cls->base); off=cls->base->size; }
    for(int i=0;i<cls->nfields;i++){ AField *fd=cls->fields[i];
        if(fd->over){ fd->offset=aot_field_root(fd)->offset; continue; }
        fd->offset=off; off+=ty_size(fd->ty); }
    for(int i=0;i<cls->nfields;i++){ AField *fd=cls->fields[i];      /* obj.name: its own value once set, the class's until then */
        if(!fd->over && fd->cvar && fd->inst_set && !fd->overridden){ fd->setoff=off; off+=4; } }
    cls->size=off;
    int n=cls->base?cls->base->nvt:0;
    cls->vt=PPY_NEW_ARR(AFunc*,n+cls->nmethods+1);
    for(int i=0;i<n;i++) cls->vt[i]=cls->base->vt[i];
    for(int i=0;i<cls->nmethods;i++){ AFunc *m=cls->methods[i];
        if(m->is_static) continue;
        int slot=-1; for(int k=0;k<n;k++) if(!strcmp(cls->vt[k]->name,m->name)) slot=k;
        if(slot>=0){ cls->vt[slot]->overridden=1; m->vslot=slot; cls->vt[slot]=m; }
        else { m->vslot=n; cls->vt[n++]=m; }
    }
    cls->nvt=n;
}
/* A method overridden anywhere below needs a virtual call. */
static void mark_overrides(AProg *p){
    for(int i=0;i<p->nclasses;i++){ AClass *cls=p->classes[i];
        for(int k=0;k<cls->nmethods;k++){ AFunc *m=cls->methods[k]; if(m->is_static) continue;
            for(AClass *b=cls->base;b;b=b->base){ AFunc *bm=NULL; for(int j=0;j<b->nmethods;j++) if(!strcmp(b->methods[j]->name,m->name)) bm=b->methods[j];
                if(bm) bm->overridden=1; } } }
}

/* The last checks: every type known (check mode: each one on its own) */
static void know_global(Ck *c, AVar *v){ must_know(c,v->ty,1,"variable",v->name); }
static void know_func(Ck *c, AFunc *f){
    for(int k=0;k<f->nvars;k++){ AVar *v=f->vars[k];
        if(k>=f->nparams && !v->live){ Ty *t=ty_find(v->ty); if(t->k==TY_VAR) t->link=TY_INT_T; }   /* (only in code left out: if isinstance(x, dict) ...) */
        must_know(c,v->ty,f->line,k<f->nparams?"parameter":"variable",v->name); }
    if(is_func(f)){ { Ty *v=ty_find(f->ret); if(v->k==TY_VAR && v->opt) v->link=default_of(v,TY_INT_T); }
        default_elems(f->ret); if(!ty_known(f->ret)) ck_err(c,f->line,"cannot infer the return type of %s(); annotate it (-> type)",f->name); }
}
static void know_class(Ck *c, AClass *cls){
    for(int k=0;k<cls->nfields;k++) must_know(c,cls->fields[k]->ty,cls->def->line,"field",cls->fields[k]->name);
}
static void final_global(Ck *c, AVar *v){
    if(!aot_check_mode){ know_global(c,v); return; }
    if(v->mod && v->mod->check_failed) return;
    CkTry t; ck_try_begin(c,&t);
    if(!setjmp(c->fail)){ know_global(c,v); ck_try_end(c,&t); } else ck_try_fail(c,&t);
}
static void final_func(Ck *c, AFunc *f){
    if(!aot_check_mode){ know_func(c,f); return; }
    if(f->check_failed) return;
    CkTry t; ck_try_begin(c,&t);
    if(!setjmp(c->fail)){ know_func(c,f); ck_try_end(c,&t); } else { ck_try_fail(c,&t); f->check_failed=1; }
}
static void final_class(Ck *c, AClass *cls){
    if(!aot_check_mode){ know_class(c,cls); return; }
    if(cls->check_failed || cls->mod->check_failed) return;
    CkTry t; ck_try_begin(c,&t);
    if(!setjmp(c->fail)){ know_class(c,cls); ck_try_end(c,&t); } else { ck_try_fail(c,&t); cls->check_failed=1; }
}

AProg *aot_check(AotUnit **units, int nunits, AotTarget target){
    if(!TY_INT_T){ TY_INT_T=ty_new(TY_INT,NULL,NULL); TY_BOOL_T=ty_new(TY_BOOL,NULL,NULL); TY_FLOAT_T=ty_new(TY_FLOAT,NULL,NULL);
        TY_STR_T=ty_new(TY_STR,NULL,NULL); TY_VOID_T=ty_new(TY_VOID,NULL,NULL); TY_BUF_T=ty_new(TY_BUF,NULL,NULL); TY_BYTES_T=ty_new(TY_BYTES,NULL,NULL); TY_TYPE_T=ty_new(TY_TYPE,NULL,NULL); }
    AProg *p=PPY_NEW0(AProg); p->target=target;
    Ck ck; memset(&ck,0,sizeof ck); ck.p=p; Ck *c=&ck;
    if(setjmp(ck.fail)) return NULL;
    p->nmods=nunits; p->mods=PPY_NEW_ARR(AModule*,nunits);
    for(int i=0;i<nunits;i++){ AModule *m=PPY_NEW0(AModule); m->unit=units[i]; m->name=units[i]->name; m->index=i; p->mods[i]=m; }
    make_builtin_exceptions(p,p->mods[0]);
    for(int i=0;i<nunits;i++) if(units[i]->ast) module_step(c,p->mods[i],collect_module);
    for(int i=0;i<nunits;i++) if(units[i]->ast && !strcmp(units[i]->name,"__ppy_eg")){    /* the ExceptionGroup prelude's classes: built-in names */
        static const char *const eg[]={"BaseExceptionGroup","ExceptionGroup","__ppy_EGState",NULL};
        for(int k=0;eg[k];k++){ ASym *s=module_sym(p->mods[i],eg[k]); if(s) symtab_add(&builtin_syms,eg[k],s->kind,s->p); } }
    for(int i=0;i<nunits;i++) if(units[i]->ast && !strcmp(units[i]->name,"codecs")){         /* encode() / decode() */
        ASym *s=module_sym(p->mods[i],"decode"); if(s) symtab_add(&builtin_syms,"__ppy_decode",s->kind,s->p);
        s=module_sym(p->mods[i],"encode"); if(s) symtab_add(&builtin_syms,"__ppy_encode",s->kind,s->p); }
    for(int i=0;i<nunits;i++) if(units[i]->ast && !strcmp(units[i]->name,"io")){              /* open(): io's files */
        ASym *s=module_sym(p->mods[i],"_open_text"); if(s) symtab_add(&builtin_syms,"__ppy_open_text",s->kind,s->p);
        s=module_sym(p->mods[i],"_open_binary"); if(s) symtab_add(&builtin_syms,"__ppy_open_binary",s->kind,s->p); }
    for(int i=0;i<nunits;i++) if(units[i]->ast && !strcmp(units[i]->name,"_ppy_iter")){
        static const char *const fs[]={"__ppy_iter","__ppy_next",NULL};
        for(int k=0;fs[k];k++){ ASym *s=module_sym(p->mods[i],fs[k]); if(s) symtab_add(&builtin_syms,fs[k],s->kind,s->p); } }
    for(int i=0;i<nunits;i++) if(units[i]->ast && !strcmp(units[i]->name,"_sysargs")){ ASym *s=module_sym(p->mods[i],"__ppy_argv"); if(s) symtab_add(&builtin_syms,"__ppy_argv",s->kind,s->p); }
    for(int i=0;i<nunits;i++) if(units[i]->ast && !strcmp(units[i]->name,"_ppy_exit")){ static const char *const n[]={"__ppy_sysexit_int","__ppy_sysexit_str","__ppy_sysexit_none","__ppy_uncaught"};
        for(int k=0;k<4;k++){ ASym *s=module_sym(p->mods[i],n[k]); if(s) symtab_add(&builtin_syms,n[k],s->kind,s->p); }
        ASym *u=module_sym(p->mods[i],"__ppy_uncaught"); if(u && u->kind==AS_FUNC){ ((AFunc*)u->p)->ncalls++; ((AFunc*)u->p)->direct=1; p->uncaught_fn=(AFunc*)u->p; } }
    for(int i=0;i<nunits;i++) if(units[i]->ast && !strcmp(units[i]->name,"_ppy_exit")){ static const char *const n[]={"__ppy_sysexit_int","__ppy_sysexit_str","__ppy_sysexit_none","__ppy_uncaught"};
        for(int k=0;k<4;k++){ ASym *s=module_sym(p->mods[i],n[k]); if(s) symtab_add(&builtin_syms,n[k],s->kind,s->p); }
        ASym *u=module_sym(p->mods[i],"__ppy_uncaught"); if(u && u->kind==AS_FUNC){ ((AFunc*)u->p)->ncalls++; ((AFunc*)u->p)->direct=1; p->uncaught_fn=(AFunc*)u->p; } }
    for(int i=0;i<nunits;i++) if(units[i]->ast && !strcmp(units[i]->name,"_sysio")){ static const char *const n[]={"__ppy_stdout","__ppy_stderr","__ppy_stdin"};
        for(int k=0;k<3;k++){ ASym *s=module_sym(p->mods[i],n[k]); if(s) symtab_add(&builtin_syms,n[k],s->kind,s->p); } }
    for(int i=0;i<nunits;i++) if(units[i]->ast && !strcmp(units[i]->name,"_oserror")){        /* OSError(errno, strerror): built-in names */
        static const char *const fs[]={"__ppy_oserror","__ppy_oserror_new","__ppy_decode_error","__ppy_encode_error",NULL};
        for(int k=0;fs[k];k++){ ASym *s=module_sym(p->mods[i],fs[k]); if(s) symtab_add(&builtin_syms,fs[k],s->kind,s->p); } }
    for(int i=0;i<nunits;i++) if(units[i]->ast && (!strcmp(units[i]->name,"struct") || !strcmp(units[i]->name,"tempfile") || !strcmp(units[i]->name,"re") || !strcmp(units[i]->name,"contextlib") || !strcmp(units[i]->name,"_mathx"))){   /* their helpers: */
        SymTab *st=&p->mods[i]->syms; int k0=st->n;                                                    /* what pack(...) etc. become */
        for(int k=0;k<k0;k++) if(st->v[k].name[0]=='_' && st->v[k].name[1]!='_' && (st->v[k].kind==AS_FUNC||st->v[k].kind==AS_CLASS)){
            char nm[96]; snprintf(nm,sizeof nm,"__ppy_%s_%s",units[i]->name,st->v[k].name); symtab_add(&builtin_syms,nm,st->v[k].kind,st->v[k].p); } }
    for(int i=0;i<nunits;i++) if(units[i]->ast && !strcmp(units[i]->name,"__ppy_cmpkey")){ ASym *s=module_sym(p->mods[i],"__ppy_CmpKey"); if(s) symtab_add(&builtin_syms,"__ppy_CmpKey",s->kind,s->p); }
    for(int i=0;i<nunits;i++) if(units[i]->ast && !strcmp(units[i]->name,"cmath") && units[i]->path && !strcmp(units[i]->path,"<cmath>")){   /* complex: a built-in name */
        ASym *s=module_sym(p->mods[i],"complex"); if(s){ symtab_add(&builtin_syms,"complex",s->kind,s->p); complex_class=(AClass*)s->p; }
        s=module_sym(p->mods[i],"__ppy_complex_parse"); if(s) symtab_add(&builtin_syms,"__ppy_complex_parse",s->kind,s->p); }
    for(int i=nunits-1;i>=0;i--) if(units[i]->ast) module_step(c,p->mods[i],link_imports);
    for(int i=0;i<nunits;i++) if(units[i]->ast) module_step(c,p->mods[i],collect_functions);
    if(aot_check_mode) for(int i=0;i<nunits;i++) if(!p->mods[i]->body){     /* (a module that failed: an empty body) */
        AFunc *body=PPY_NEW0(AFunc); body->name=xstrdup2(p->mods[i]->name); body->mod=p->mods[i]; body->ret=TY_VOID_T; body->line=1;
        body->unused=1; add_func(p,body); p->mods[i]->body=body; }
    for(int i=0;i<p->nclasses;i++){ AClass *m=p->classes[i]; int vb=0, lay=0;      /* a class only used as a mixin: its methods live on as copies */
        for(int j=0;j<p->nclasses;j++){ AClass *k=p->classes[j]; if(k->base==m) lay=1; for(int v=0;v<k->nvbases;v++) if(k->vbases[v]==m) vb=1; }
        if(vb && !lay) m->mixin=1; }
    for(int pass=0;pass<100;pass++){
        c->changed=0; c->pass_first=ty_ids+1;
        check_all(c);
        if(!c->changed && !c->defaulting){ c->defaulting=1; continue; }   /* (then once more: what waited for such arguments) */
        if(!c->changed) break;
    }
    for(int i=0;i<p->nfuncs;i++) for(int k=0;k<p->funcs[i]->nvars;k++) p->funcs[i]->vars[k]->live=0;   /* (what the last pass reaches: not branches decided away since) */
    c->strict=1; c->pass_first=ty_ids+1;
    check_all(c);
    for(int i=0;i<p->nfuncs;i++){ AFunc *f=p->funcs[i]; if((f->cls && f->cls->mixin && !f->cls->constructed) || generic_original(f)) f->unused=1;
        if(f->cls && !f->cls->constructed && !strncmp(f->name,"__mro_",6) && !f->ncalls) f->unused=1; }   /* (a mixin's method copied into a class nothing makes) */
    for(int i=0;i<c->nseen;i++) default_elems(c->seen[i]);
    free(c->seen);
    for(int i=0;i<p->nglobals;i++){ AVar *v=p->globals[i]; c->mod=v->mod; final_global(c,v); }
    for(int i=0;i<p->nfuncs;i++){ AFunc *f=p->funcs[i];               /* inside a function nothing uses */
        for(AFunc *o=f->outer;o;o=o->outer) if(o->unused) f->unused=1; }
    for(int i=0;i<p->nfuncs;i++){ AFunc *f=p->funcs[i]; c->mod=f->mod; if(f->unused) continue; c->fn=f; final_func(c,f); }
    for(int i=0;i<p->nfuncs;i++){ AFunc *f=p->funcs[i];               /* closures: copy the value, or share a cell */
        for(int k=0;k<f->nvars;k++){ AVar *v=f->vars[k]; if(!v->captured) continue;
            int param=k<f->nparams;
            int byval=!v->nonlocal_set && ((param && v->nbind==0) || (!param && v->nbind==1 && v->bind_top>=0 && v->bind_top<v->first_use_top));
            v->cell=!byval; } }
    for(int i=0;i<p->nclasses;i++){ AClass *cls=p->classes[i]; c->mod=cls->mod; if(cls->builtin) continue;
        if((cls->ninsts || cls->generic) && !cls->origin && !cls->constructed){   /* a generic class only copies of are made (or none): not used itself */
            for(int k=0;k<cls->nfields;k++){ Ty *t=ty_find(cls->fields[k]->ty); if(!ty_known(t)) cls->fields[k]->ty=TY_INT_T; }
            continue; }
        final_class(c,cls); }
    for(int i=0;i<p->nclasses;i++) layout_class(p->classes[i]);
    mark_overrides(p);
    return p;
}
