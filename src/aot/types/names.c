/* ========================= Typed compiler, types: names =========================
   Name lookup (locals, enclosing functions, globals, built-ins),
   captured variables, expressions naming modules and classes. */

#include "check.h"

/* a generator that may finish where its consumer is: its return value is StopIteration.value
   (one type for the program's StopIterations) -> what yield from gives */
Ty *stop_value(Ck *c, Ty *g, int line){
    Ty *r=aot_gen_part(g,1);
    if(r->k==TY_VAR || r->k==TY_VOID) return r;
    AClass *si=(AClass*)symtab_find(&builtin_syms,"StopIteration")->p; AField *vf=aot_find_field(si,"value");
    expect(c,vf->ty,r,line,"StopIteration.value (the return values of the generators)");
    return r;
}

AVar *var_root(AVar *v){ while(v->src) v=v->src; return v; }
/* v, a variable of `owner`, as seen from g (an inner function of owner): one
   captured copy per function in between */
static AVar *capture(AFunc *g, AFunc *owner, AVar *v){
    if(g==owner) return v;
    AVar *src=capture(g->outer,owner,v);
    for(int i=0;i<g->ncaps;i++) if(g->caps[i]->src==src) return g->caps[i];
    AVar *cv=PPY_NEW0(AVar); cv->name=v->name; cv->ty=v->ty; cv->mod=g->mod; cv->owner=g; cv->src=src; cv->bind_top=-1; cv->id=-1;
    if(g->ncaps==g->capcap){ g->capcap=g->capcap?g->capcap*2:4; g->caps=(AVar**)xrealloc(g->caps,sizeof(AVar*)*(size_t)g->capcap); g->capsym=(ASym**)xrealloc(g->capsym,sizeof(ASym*)*(size_t)g->capcap); }
    ASym *sym=PPY_NEW0(ASym); sym->name=cv->name; sym->kind=AS_VAR; sym->p=cv;
    g->caps[g->ncaps]=cv; g->capsym[g->ncaps]=sym; g->ncaps++;
    return cv;
}
static ASym *self_sym(AFunc *f){ if(!f->selfsym){ f->selfsym=PPY_NEW0(ASym); f->selfsym->name=f->name; f->selfsym->kind=AS_FUNC; f->selfsym->p=f; } return f->selfsym; }
/* A name in the current function: its comprehension/except variables, its
   locals, then those of the enclosing functions (captured), then globals. */
ASym *lookup(Ck *c, const char *name){
    AFunc *f=c->fn;
    for(AFunc *lv=f; lv; lv=lv->outer){
        ASym *s=NULL;
        for(int i=c->ncscope-1;i>=0 && !s;i--) if(c->cscope[i].fn==lv && !strcmp(c->cscope[i].name,name)) s=&c->cscope[i].sym;
        if(!s){
            if(!is_func(lv)) break;
            for(int i=0;i<lv->nglobals_decl;i++) if(!strcmp(lv->globals_decl[i],name)) return module_sym(lv->mod,name);
            s=symtab_find(&lv->locals,name);
        }
        if(!s) continue;
        if(lv==f || s->kind!=AS_VAR) return s;
        AVar *v=(AVar*)s->p;
        if(v->global) return s;
        if(v->fn_const==f) return self_sym(f);          /* a nested function naming itself */
        v->captured=1;
        if(v->first_use_top>lv->top_index) v->first_use_top=lv->top_index;
        AVar *cv=capture(f,lv,v);
        for(int i=0;i<f->ncaps;i++) if(f->caps[i]==cv) return f->capsym[i];
    }
    { AFunc *o=f; while(o && o->outer) o=o->outer;          /* a method of a class defined in a function: that function's classes */
      if(o && o->cls && o->cls->encl) for(AFunc *e=o->cls->encl;e && e->def;e=next_scope(e)){
          ASym *s=symtab_find(&e->locals,name);
          if(s && s->kind==AS_CLASS) return s;
          if(s && s->kind==AS_VAR && !((AVar*)s->p)->global)
              ck_err(c,f->line,"%s.%s uses '%s' of %s(): methods of a class defined in a function cannot use the function's variables in compiled code",o->cls->name,o->name,name,e->name); } }
    return global_sym(c->mod,name);
}
/* An expression naming a module (`m`, `pkg.sub`), or NULL. */
AModule *module_expr(Ck *c, Expr *e){
    if(e->kind==EXPR_NAME){ ASym *s=lookup(c,e->name); return s && s->kind==AS_MODULE ? (AModule*)s->p : NULL; }
    if(e->kind==EXPR_ATTRIBUTE){
        AModule *m=module_expr(c,e->a); if(!m) return NULL;
        ASym *s=module_sym(m,e->name); if(s) return s->kind==AS_MODULE ? (AModule*)s->p : NULL;
        char sub[300]; snprintf(sub,sizeof sub,"%s.%s",m->name,e->name);
        return find_module(c->p,sub);
    }
    return NULL;
}
/* An expression naming a class (`C`, `Outer.Inner`, `module.C`), or NULL. */
AClass *class_expr(Ck *c, Expr *e){
    if(e->kind==EXPR_NAME){ ASym *s=lookup(c,e->name); return s && s->kind==AS_CLASS ? (AClass*)s->p : NULL; }
    if(e->kind==EXPR_ATTRIBUTE){
        AModule *m=module_expr(c,e->a);
        if(m){ ASym *s=module_sym(m,e->name); return s && s->kind==AS_CLASS ? (AClass*)s->p : NULL; }
        if(e->a->kind==EXPR_NAME && !strcmp(e->name,"JSONDecodeError")){ ASym *s=lookup(c,e->a->name);           /* json.JSONDecodeError */
            AModule *jr=find_module(c->p,"_ppy_jsonr");
            if(s && s->kind==AS_SYS && !strcmp((const char*)s->p,"json") && jr){ ASym *k=module_sym(jr,"JSONDecodeError"); return k && k->kind==AS_CLASS ? (AClass*)k->p : NULL; } }
        if(e->a->kind==EXPR_NAME && !strcmp(e->name,"FrozenInstanceError")){ ASym *s=lookup(c,e->a->name);     /* dataclasses.FrozenInstanceError */
            if(s && s->kind==AS_SYS && !strcmp((const char*)s->p,"dataclasses")) return (AClass*)symtab_find(&builtin_syms,"FrozenInstanceError")->p; }
        AClass *o=class_expr(c,e->a); return o ? aot_nested_class(c->p,o,e->name) : NULL;
    }
    return NULL;
}
/* `name` naming a built-in module: its name, else NULL */
const char *bmod(Ck *c, Expr *e){ if(e->kind!=EXPR_NAME) return NULL; ASym *s=lookup(c,e->name); return s && s->kind==AS_SYS ? (const char*)s->p : NULL; }
int is_bmod(Ck *c, Expr *e, const char *name){ const char *m=bmod(c,e); return m && !strcmp(m,name); }
int is_sys(Ck *c, Expr *e){ return is_bmod(c,e,"sys"); }
void no_void(Ck *c, Ty *t, int line){ if(ty_find(t)->k==TY_VOID) ck_err(c,line,"this call returns nothing (None) and has no value"); }
/* Operand types still unknown: retry in a later pass; an error in the strict pass. */
Ty *pending(Ck *c, int line, const char *what){
    if(c->strict) ck_err(c,line,"cannot infer the type of %s; add a type annotation",what);
    return ty_var();
}

