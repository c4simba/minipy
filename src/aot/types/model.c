/* ========================= Typed compiler, types: building the model =========================
   The names a scope binds; modules, functions, lambdas, classes,
   fields and variables of the program model; decorators. */

#include "check.h"

static void names_add(Names *s, const char *name){
    for(int i=0;i<s->n;i++) if(!strcmp(s->v[i],name)) return;
    if(s->n==s->cap){ s->cap=s->cap?s->cap*2:8; s->v=(char**)xrealloc(s->v,sizeof(char*)*(size_t)s->cap); }
    s->v[s->n++]=(char*)name;
}
static void names_target(Expr *t, Names *out){
    if(t->kind==EXPR_NAME) names_add(out,t->name);
    else if(t->kind==EXPR_TUPLE||t->kind==EXPR_LIST) for(int i=0;i<t->count;i++) names_target(t->items[i],out);
}
static void names_expr(Expr *e, Names *out){          /* only `name := value` binds (comprehension variables are their own) */
    if(!e || e->kind==EXPR_LAMBDA) return;
    if(e->kind==EXPR_WALRUS) names_add(out,e->name);
    names_expr(e->a,out); names_expr(e->b,out); names_expr(e->c,out); names_expr(e->d,out);
    for(int i=0;i<e->count;i++) names_expr(e->items[i],out);
    for(int i=0;i<e->vcount;i++) names_expr(e->vals[i],out);
    if(e->kind==EXPR_COMPREHENSION) for(int i=0;i<e->nclause;i++){ names_expr(e->clauses[i].iter,out); for(int k=0;k<e->clauses[i].ncond;k++) names_expr(e->clauses[i].conds[k],out); }
}
/* Names assigned by statements (not descending into def/class bodies). */
static void pattern_names(Expr *p, Names *out){              /* the names a match pattern binds */
    if(!p) return;
    if(p->kind!=EXPR_PATTERN){ return; }
    if((p->akind==PAT_AS || p->akind==PAT_STAR || p->akind==PAT_MAP) && p->name) names_add(out,p->name);
    if(p->akind==PAT_AS) pattern_names(p->a,out);
    if(p->akind!=PAT_MAP) for(int i=0;i<p->count;i++) pattern_names(p->items[i],out);
    else for(int i=0;i<p->vcount;i++) pattern_names(p->vals[i],out);
}
void names_bound(Ck *c, Stmt **b, int n, Names *out, Names *globals){
    AotUnit *u=c->mod->unit;
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        switch(s->kind){
            case STMT_ASSIGN:{ AAssign *a=aot_assign(u,s); for(int k=0;k<a->ntarget;k++){ names_target(a->target[k],out); names_expr(a->target[k],out); } names_expr(a->value,out); break; }
            case STMT_EXPR:{ APrint *pr=aot_print(u,s);
                if(pr){ for(int k=0;k<pr->n;k++) names_expr(pr->args[k],out); }
                else if(aot_is_annotation_only(u,s)) names_add(out,s->targets[0]->name);
                else names_expr(aot_expr(u,&s->expr),out);
                break; }
            case STMT_FOR: for(int k=0;k<s->param_count;k++) names_add(out,s->params[k]);
                names_expr(aot_expr(u,&s->expr),out);
                names_bound(c,s->body,s->body_count,out,globals); names_bound(c,s->orelse,s->orelse_count,out,globals); break;
            case STMT_IF: case STMT_WHILE: names_expr(aot_expr(u,&s->expr),out);
                names_bound(c,s->body,s->body_count,out,globals); names_bound(c,s->orelse,s->orelse_count,out,globals); break;
            case STMT_WITH:{ AWith *w=aot_with(u,s); for(int k=0;k<w->n;k++){ names_expr(w->e[k],out); if(w->as[k]) names_add(out,w->as[k]); if(s->withtgt && s->withtgt[k]) names_target(s->withtgt[k],out); }
                names_bound(c,s->body,s->body_count,out,globals); break; }
            case STMT_TRY: case STMT_BLOCK: names_bound(c,s->body,s->body_count,out,globals); names_bound(c,s->orelse,s->orelse_count,out,globals); break;
            case STMT_RETURN: case STMT_RAISE: names_expr(aot_expr(u,&s->expr),out); break;
            case STMT_ASSERT: names_expr(aot_expr(u,&s->expr),out); names_expr(aot_expr(u,&s->expr2),out); break;
            case STMT_GLOBAL: case STMT_NONLOCAL: if(globals) for(int k=0;k<s->param_count;k++) names_add(globals,s->params[k]); break;
            case STMT_FUNCTION_DEF: names_add(out,s->name); break;
            case STMT_MATCH: names_expr(s->expr,out);
                for(int k=0;k<s->body_count;k++){ Stmt *cs=s->body[k]; pattern_names(cs->expr,out); if(cs->expr2) names_expr(cs->expr2,out); names_bound(c,cs->body,cs->body_count,out,globals); }
                break;
            default: break;
        }
    }
}
static void collect_nonlocals(Stmt **b, int n, Names *out){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_NONLOCAL) for(int k=0;k<s->param_count;k++) names_add(out,s->params[k]);
        if(s->kind==STMT_FUNCTION_DEF||s->kind==STMT_CLASS_DEF) continue;
        collect_nonlocals(s->body,s->body_count,out); collect_nonlocals(s->orelse,s->orelse_count,out);
    }
}
static int has_return_value(Stmt **b, int n){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_RETURN && s->expr) return 1;
        if(s->kind==STMT_FUNCTION_DEF||s->kind==STMT_CLASS_DEF) continue;
        if(has_return_value(s->body,s->body_count)||has_return_value(s->orelse,s->orelse_count)) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- building the model */

AVar *new_global(Ck *c, AModule *m, const char *name){
    AProg *p=c->p; AVar *v=PPY_NEW0(AVar);
    v->name=xstrdup2(name); v->ty=ty_var(); v->global=1; v->mod=m; v->id=p->nglobals;
    if(p->nglobals==p->gcap){ p->gcap=p->gcap?p->gcap*2:32; p->globals=(AVar**)xrealloc(p->globals,sizeof(AVar*)*(size_t)p->gcap); }
    p->globals[p->nglobals++]=v;
    return v;
}
AVar *global_var(Ck *c, AModule *m, const char *name, int line){
    ASym *s=module_sym(m,name);
    if(!s) return (AVar*)symtab_add(&m->syms,name,AS_VAR,new_global(c,m,name))->p;
    if(s->kind!=AS_VAR) ck_err(c,line,"'%s' is a %s and cannot be assigned",name,s->kind==AS_FUNC?"function":s->kind==AS_CLASS?"class":"module");
    return (AVar*)s->p;
}
AVar *hidden_local(AFunc *f, const char *name, Ty *ty){   /* a local slot no name refers to */
    AVar *v=PPY_NEW0(AVar); v->name=xstrdup2(name); v->ty=ty?ty:ty_var(); v->mod=f->mod; v->id=f->nvars;
    v->owner=f; v->bind_top=-1; v->first_use_top=0x7FFFFFFF;
    if(f->nvars==f->vcap){ f->vcap=f->vcap?f->vcap*2:8; f->vars=(AVar**)xrealloc(f->vars,sizeof(AVar*)*(size_t)f->vcap); }
    f->vars[f->nvars++]=v;
    return v;
}
AVar *new_local(AFunc *f, const char *name, Ty *ty){
    AVar *v=hidden_local(f,name,ty);
    symtab_add(&f->locals,name,AS_VAR,v);
    return v;
}
void add_func(AProg *p, AFunc *f){
    if(p->nfuncs==p->fcap){ p->fcap=p->fcap?p->fcap*2:32; p->funcs=(AFunc**)xrealloc(p->funcs,sizeof(AFunc*)*(size_t)p->fcap); }
    f->id=p->nfuncs; p->funcs[p->nfuncs++]=f;
}
void no_nested_defs(Ck *c, Stmt **b, int n, int in_func){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_FUNCTION_DEF){
            if(!in_func) ck_err(c,s->line,"define '%s' at the top level of the module (or inside a function) in compiled code",s->name);
            continue;                                   /* its body is checked by its own new_func */
        }
        if(s->kind==STMT_CLASS_DEF){
            if(!in_func) ck_err(c,s->line,"define class '%s' at the top level of the module (or inside a function) in compiled code",s->name);
            continue; }
        no_nested_defs(c,s->body,s->body_count,in_func); no_nested_defs(c,s->orelse,s->orelse_count,in_func);
    }
}
/* what decorator i of a def is: 0 a general one (name = d(function)),
   1 @staticmethod, 2 @property, 3 ignored (@wraps(f): only copies names) */
static int is_abstract_deco(Expr *d){
    return (d->kind==EXPR_NAME && !strcmp(d->name,"abstractmethod")) || (d->kind==EXPR_ATTRIBUTE && !strcmp(d->name,"abstractmethod") && d->a->kind==EXPR_NAME && !strcmp(d->a->name,"abc"));
}
static int deco_kind(AotUnit *u, Stmt *def, int i){
    Expr *d=aot_expr(u,&def->decorator_exprs[i]);
    if(is_abstract_deco(d)) return 3;                           /* @abstractmethod: see is_abstract */
    if(ut_deco_name(u?u->src:NULL,d)) return 3;                 /* @unittest.skip(...) ...: read by ut_generate */
    if(d->kind==EXPR_NAME && !strcmp(d->name,"staticmethod")) return 1;
    if(d->kind==EXPR_NAME && !strcmp(d->name,"classmethod")) return 5;
    if(d->kind==EXPR_NAME && !strcmp(d->name,"property")) return 2;
    if((d->kind==EXPR_NAME && !strcmp(d->name,"contextmanager")) ||
       (d->kind==EXPR_ATTRIBUTE && !strcmp(d->name,"contextmanager") && d->a->kind==EXPR_NAME && !strcmp(d->a->name,"contextlib"))) return 4;   /* (its calls wrapped) */
    if(d->kind==EXPR_CALL){ Expr *f=d->a;
        if((f->kind==EXPR_NAME && !strcmp(f->name,"wraps")) || (f->kind==EXPR_ATTRIBUTE && !strcmp(f->name,"wraps") && f->a->kind==EXPR_NAME && !strcmp(f->a->name,"functools"))) return 3;
        if((f->kind==EXPR_NAME && !strcmp(f->name,"recursive_repr")) || (f->kind==EXPR_ATTRIBUTE && !strcmp(f->name,"recursive_repr") && f->a->kind==EXPR_NAME && !strcmp(f->a->name,"reprlib"))) return 3; }   /* (typed values do not contain themselves) */
    return 0;
}
/* a name renamed in code (a @classmethod's cls: its class), but where a nested scope binds it again */
static void rename_expr(Expr *e, const char *from, const char *to);
static void rename_stmts(Stmt **b, int n, const char *from, const char *to){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        rename_expr(s->expr,from,to); rename_expr(s->expr2,from,to); rename_expr(s->value,from,to);
        for(int k=0;k<s->ntargets;k++) rename_expr(s->targets[k],from,to);
        if(s->withtgt) for(int k=0;k<s->ntargets;k++) rename_expr(s->withtgt[k],from,to);
        if(s->kind==STMT_FUNCTION_DEF){
            if(s->pdefaults) for(int k=0;k<s->param_count;k++) rename_expr(s->pdefaults[k],from,to);
            for(int k=0;k<s->decorator_count;k++) rename_expr(s->decorator_exprs[k],from,to);
            int bound=0; for(int k=0;k<s->param_count;k++) if(!strcmp(s->params[k],from)) bound=1;
            if(!bound) rename_stmts(s->body,s->body_count,from,to);
            continue; }
        if(s->kind==STMT_CLASS_DEF) continue;
        rename_stmts(s->body,s->body_count,from,to); rename_stmts(s->orelse,s->orelse_count,from,to); }
}
static void rename_expr(Expr *e, const char *from, const char *to){
    if(!e) return;
    if(e->kind==EXPR_NAME && e->name && !strcmp(e->name,from)) e->name=(char*)to;
    if(e->kind==EXPR_LAMBDA){ for(int k=0;k<e->neparam;k++) if(!strcmp(e->eparams[k],from)) return; }
    rename_expr(e->a,from,to); rename_expr(e->b,from,to); rename_expr(e->c,from,to); rename_expr(e->d,from,to);
    for(int k=0;k<e->count;k++) rename_expr(e->items[k],from,to);
    for(int k=0;k<e->vcount;k++) rename_expr(e->vals[k],from,to);
    if(e->edefaults) for(int k=0;k<e->neparam;k++) rename_expr(e->edefaults[k],from,to);
    for(int k=0;k<e->nclause;k++){ CompClause *cl=&e->clauses[k]; rename_expr(cl->iter,from,to); for(int j=0;j<cl->ncond;j++) rename_expr(cl->conds[j],from,to); }
}
/* @classmethod def f(cls, ...): a static method whose cls is its class (the one it is defined in) */
void classmethod_def(AClass *cls, Stmt *def){
    if(def->param_count<1) return;
    const char *from=def->params[0];
    for(int k=0;k+1<def->param_count;k++){ def->params[k]=def->params[k+1];
        if(def->annotations && k+1<def->annotation_cap) def->annotations[k]=def->annotations[k+1];
        if(def->pdefaults) def->pdefaults[k]=def->pdefaults[k+1]; }
    if(def->annotations && def->param_count-1<def->annotation_cap) def->annotations[def->param_count-1]=NULL;
    def->param_count--;
    if(def->star_index>=0) def->star_index--;
    if(def->dstar_index>=0) def->dstar_index--;
    if(def->kwonly_index>=0) def->kwonly_index--;
    rename_stmts(def->body,def->body_count,from,cls->symname);
}
int general_decos(AotUnit *u, Stmt *def, int in_class){
    int n=0; for(int i=0;i<def->decorator_count;i++){ int k=deco_kind(u,def,i); if(k==0 || (!in_class && k!=3 && k!=4)) n++; } return n;
}
char *hidden_name(const char *a, const char *b){        /* a name no source can spell */
    char buf[300]; snprintf(buf,sizeof buf," %s%s%s",a,b?".":"",b?b:""); return xstrdup2(buf);
}
/* name = d1(d2(fnref)): the decorators' expressions applied, innermost first */
Expr *deco_app(Ck *c, AotUnit *u, Stmt *def, const char *fnref, int in_class){
    if(def->expr2) return def->expr2;
    Expr *cur=name_expr(fnref,def->line);
    for(int i=def->decorator_count-1;i>=0;i--){ int k=deco_kind(u,def,i);
        if(k==3 || k==4 || (in_class && k)) continue;
        Expr *d=def->decorator_exprs[i];
        Expr *nm=d->kind==EXPR_NAME?d:d->kind==EXPR_CALL&&d->a->kind==EXPR_NAME?d->a:NULL;
        if(nm){ ASym *x=lookup(c,nm->name);                 /* a generic decorator function: an instance of its own */
            if(x && x->kind==AS_FUNC && ((AFunc*)x->p)->pristine) nm->name=(char*)instantiate(c,(AFunc*)x->p); }
        Expr *call=xnew(EXPR_CALL,def->line); call->a=d; xpush(call,cur); cur=call; }
    def->expr2=cur;
    return cur;
}
static int has_yield(Stmt **b, int n){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_YIELD) return 1;
        if(s->kind==STMT_FUNCTION_DEF||s->kind==STMT_CLASS_DEF) continue;
        if(has_yield(s->body,s->body_count)||has_yield(s->orelse,s->orelse_count)) return 1;
    }
    return 0;
}
/* Binding statements of each local: closures copy a variable bound once
   before them; anything else is shared through a cell. */
static void count_bind(AFunc *f, const char *name, int top, int nested){
    ASym *s=symtab_find(&f->locals,name); if(!s || s->kind!=AS_VAR) return;
    AVar *v=(AVar*)s->p; v->nbind++; v->bind_top=(v->nbind==1 && !nested) ? top : -1;
}
static void count_bind_target(AFunc *f, Expr *t, int top, int nested){
    if(t->kind==EXPR_NAME) count_bind(f,t->name,top,nested);
    else if(t->kind==EXPR_TUPLE||t->kind==EXPR_LIST) for(int i=0;i<t->count;i++) count_bind_target(f,t->items[i],top,nested);
}
static void count_bindings(AFunc *f, Stmt **b, int n, int top, int nested){
    AotUnit *u=f->mod->unit;
    for(int i=0;i<n;i++){ Stmt *s=b[i]; int t=nested?top:i;
        switch(s->kind){
            case STMT_ASSIGN:{ AAssign *a=aot_assign(u,s); if(!a->value) break; for(int k=0;k<a->ntarget;k++) count_bind_target(f,a->target[k],t,nested); break; }
            case STMT_FOR: for(int k=0;k<s->param_count;k++) count_bind(f,s->params[k],t,1);
                count_bindings(f,s->body,s->body_count,t,1); count_bindings(f,s->orelse,s->orelse_count,t,1); break;
            case STMT_WITH:{ AWith *w=aot_with(u,s); for(int k=0;k<w->n;k++) if(w->as[k]) count_bind(f,w->as[k],t,nested);
                count_bindings(f,s->body,s->body_count,t,1); break; }
            case STMT_FUNCTION_DEF: count_bind(f,s->name,t,nested); break;
            case STMT_MATCH: for(int k=0;k<s->body_count;k++){ Names pn={0}; pattern_names(s->body[k]->expr,&pn); for(int q=0;q<pn.n;q++) count_bind(f,pn.v[q],t,1); free(pn.v); }
                count_bindings(f,s->body,s->body_count,t,1); break;
            case STMT_IF: case STMT_WHILE: case STMT_TRY: case STMT_BLOCK: case STMT_CASE:
                count_bindings(f,s->body,s->body_count,t,1); count_bindings(f,s->orelse,s->orelse_count,t,1); break;
            default: break;
        }
    }
}
/* nested defs of f's body (not of their own bodies) */
static void collect_nested(Ck *c, AFunc *f, Stmt **b, int n){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_CLASS_DEF){                        /* a class of the function: hoisted, seen by name in its body */
            ASym *x=symtab_find(&f->locals,s->name);
            if(x) ck_err(c,s->line,"'%s' is defined twice in %s()",s->name,f->name);
            AClass *cls=new_inner_class(c,f->mod,s,NULL,f);
            symtab_add(&f->locals,s->name,AS_CLASS,cls);
            continue; }
        if(s->kind==STMT_FUNCTION_DEF){
            AFunc *inner=new_func(c,NULL,f->mod,NULL,s,f);
            s->aux=inner;
            if(inner->ndeco) symtab_add(&f->locals,hidden_name(s->name,NULL),AS_FUNC,inner);   /* the undecorated function */
            continue;
        }
        collect_nested(c,f,s->body,s->body_count); collect_nested(c,f,s->orelse,s->orelse_count);
    }
}
static void set_fn_const(AFunc *f, Stmt **b, int n){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_FUNCTION_DEF){ ASym *x=symtab_find(&f->locals,s->name);
            if(x && x->kind==AS_VAR && ((AVar*)x->p)->nbind==1 && !((AFunc*)s->aux)->ndeco){ ((AVar*)x->p)->fn_const=(AFunc*)s->aux; ((AFunc*)s->aux)->selfvar=(AVar*)x->p; }
            continue; }
        set_fn_const(f,s->body,s->body_count); set_fn_const(f,s->orelse,s->orelse_count);
    }
}

/* f: a function object made earlier (module-level functions exist before imports are linked) or NULL */
AFunc *new_func(Ck *c, AFunc *f, AModule *m, AClass *cls, Stmt *def, AFunc *outer){
    if(!f) f=PPY_NEW0(AFunc);
    f->name=xstrdup2(def->name); f->mod=m; f->cls=cls; f->def=def; f->body=def->body; f->nbody=def->body_count; f->line=def->line;
    f->outer=outer;
    if(cls && c->mk_static) f->is_static=1;
    size_t dnl=strlen(def->name);                                         /* (a copy from a mixin: __mro_Base___exit__) */
    if(cls && def->param_count==4 && ((dnl>=8 && !strcmp(def->name+dnl-8,"__exit__"))||(dnl>=9 && !strcmp(def->name+dnl-9,"__aexit__")))){    /* __exit__(self, et, ev, tb): what `with` passes */
        static const char *const ann[]={NULL,"type[BaseException] | None","BaseException | None","BaseException | None"};
        for(int i=1;i<4;i++) if(!def->annotations || i>=def->annotation_cap || !def->annotations[i]) stmt_set_annotation(def,i,py_front_expr(m->unit->path,ann[i],def->line)); }
    for(int i=0;i<def->decorator_count;i++){
        if(cls && is_abstract_deco(aot_expr(m->unit,&def->decorator_exprs[i]))) f->is_abstract=1;
        int k=deco_kind(m->unit,def,i);
        if(cls && k==5){ f->is_static=1; f->is_clsm=1; classmethod_def(cls,def);         /* (once: the decorator goes) */
            for(int j=i;j+1<def->decorator_count;j++){ def->decorator_exprs[j]=def->decorator_exprs[j+1]; if(def->decorators) def->decorators[j]=def->decorators[j+1]; }
            def->decorator_count--; i--; continue; }
        if(cls && k==1) f->is_static=1;
        else if(cls && k==2) f->is_property=1;
        else if(k==4) f->cm=1;
        else if(k!=3) f->ndeco++;
    }
    if(f->ndeco && (f->is_property || (cls && !strcmp(def->name,"__init__")))) ck_err(c,def->line,"%s.%s: decorators cannot be combined with @property or __init__ in compiled code",cls?cls->name:"",def->name);
    if(def->param_count>16) ck_err(c,def->line,"too many parameters");
    no_nested_defs(c,def->body,def->body_count,1);
    if(def->star_index>=0 && def->annotations && def->star_index<def->annotation_cap){ Expr *an=def->annotations[def->star_index];   /* *args: sys._PackArgs */
        if(an && an->kind==EXPR_ATTRIBUTE && !strcmp(an->name,"_PackArgs") && an->a->kind==EXPR_NAME && !strcmp(an->a->name,"sys")){
            int p=def->star_index; def->packargs=p+1;                       /* (a plain parameter: the tuple the call's extra arguments make) */
            if(def->kwonly_index<0) def->kwonly_index=p+1;
            def->star_index=-1; def->annotations[p]=NULL; } }        /* (the calls give it: () without extra arguments) */
    f->packargs=def->packargs;
    f->nparams=def->param_count;
    f->star=def->star_index; f->dstar=def->dstar_index;
    f->kwonly=def->kwonly_index>=0 ? def->kwonly_index : f->star>=0 ? f->star+1 : f->dstar>=0 ? f->dstar : f->nparams;
    f->params=PPY_NEW_ARR(AVar*,f->nparams>0?f->nparams:1);
    f->defaults=PPY_NEW_ARR(Expr*,f->nparams>0?f->nparams:1);
    AFunc *save_sig=c->sig_fn; c->sig_fn=f; f->cls=cls;
    AModule *save_mod=c->mod; c->mod=m;                                    /* (its annotations name its module's) */
    for(int i=0;i<f->nparams;i++){
        Ty *t=annotation(c,def->annotations && i<def->annotation_cap ? def->annotations[i] : NULL);
        if(i==f->star){ Ty *l=ty_new(TY_LIST,t?t:ty_var(),NULL); l->tup=1; t=l; }          /* *args: a tuple of them */
        else if(i==f->dstar) t=ty_dict(TY_STR_T,t?t:ty_var());                          /* **kwargs: a dict by name */
        if(cls && !f->is_static && i==0 && i!=f->star){
            Ty *self=ty_new(TY_OBJ,NULL,cls);
            if(t && !ty_same(t,self)) ck_err(c,def->line,"'%s' of %s.%s must be %s",def->params[0],cls->name,f->name,cls->name);
            t=self;
        }
        if(t && ty_find(t)->k==TY_VOID) ck_err(c,def->line,"parameter '%s' cannot be None",def->params[i]);
        f->params[i]=new_local(f,def->params[i],t);
        f->defaults[i]=NULL;
    }
    if(cls && !f->is_static && f->nparams==0) ck_err(c,def->line,"method %s.%s needs a self parameter",cls->name,f->name);
    if(def->pdefaults) for(int i=0;i<f->nparams;i++){ Expr *d=def->pdefaults[i]; f->defaults[i]=d;
        if(is_sys_call(d,"_site")||is_sys_call(d,"exception")){ Expr *cp=PPY_NEW0(Expr); *cp=*d; cp->ty=NULL; f->defaults[i]=cp; } }   /* (def->pdefaults keeps it: put in at each call) */
    f->returns_value=has_return_value(def->body,def->body_count);
    f->is_gen=has_yield(def->body,def->body_count) || def->yield_expr;
    if(f->is_gen){
        if(def->returns){ f->ret=annotation(c,def->returns); if(ty_find(f->ret)->k!=TY_GEN) ck_err(c,def->line,"generator %s() must be annotated -> %s",f->name,def->is_async?"AsyncIterator[T] (or AsyncGenerator[T, S])":"Iterator[T] (or Generator[T, S, R])"); }
        else f->ret=ty_new(TY_GEN,ty_var(),NULL);
        if(def->is_async){ Ty *g=ty_find(f->ret); if(!g->tup && def->returns) ck_err(c,def->line,"async generator %s() must be annotated -> AsyncIterator[T]",f->name); g->tup=1; }   /* an async generator (awaits run in it) */
        if(f->returns_value){ Ty *g=ty_find(f->ret);                          /* return v: StopIteration.value, what yield from gives */
            if(!def->returns) g->elems[1]=ty_var();
            else if(ty_find(g->elems[1])->k==TY_VOID) ck_err(c,def->line,"generator %s() returns a value: annotate it -> Generator[T, S, R]",f->name); }
        f->yield_ty=ty_find(f->ret)->elem;
    }
    else if(def->returns){ f->ret=annotation(c,def->returns); if(ty_find(f->ret)->k==TY_VOID && f->returns_value) ck_err(c,def->line,"%s() is annotated -> None but returns a value",f->name); }
    else f->ret=f->returns_value ? ty_var() : TY_VOID_T;
    c->sig_fn=save_sig; c->mod=save_mod;
    if(cls && !strcmp(f->name,"__init__") && ty_find(f->ret)->k!=TY_VOID) ck_err(c,def->line,"__init__ must not return a value");
    f->is_async=def->is_async;
    if(f->is_async && cls && (f->is_static || (!strncmp(f->name,"__",2) && strcmp(f->name,"__aenter__") && strcmp(f->name,"__aexit__") && strcmp(f->name,"__anext__")))) ck_err(c,def->line,"%s.%s cannot be async in compiled code",cls->name,f->name);
    Names locals={0}, globals={0}, nonlocals={0}, decl={0};
    names_bound(c,def->body,def->body_count,&locals,&decl);    /* decl: global and nonlocal names */
    collect_nonlocals(def->body,def->body_count,&nonlocals);
    for(int i=0;i<decl.n;i++){ int nl=0; for(int k=0;k<nonlocals.n;k++) if(!strcmp(nonlocals.v[k],decl.v[i])) nl=1; if(!nl) names_add(&globals,decl.v[i]); }
    if(nonlocals.n && !outer) ck_err(c,def->line,"nonlocal outside a nested function");
    f->globals_decl=globals.v; f->nglobals_decl=globals.n;
    for(int i=0;i<globals.n;i++) global_var(c,m,globals.v[i],def->line);
    for(int i=0;i<locals.n;i++){
        int isdecl=0; for(int k=0;k<decl.n;k++) if(!strcmp(decl.v[k],locals.v[i])) isdecl=1;
        if(!isdecl && !symtab_find(&f->locals,locals.v[i])) new_local(f,locals.v[i],NULL);
    }
    free(decl.v); free(nonlocals.v);
    free(locals.v);
    add_func(c->p,f);
    collect_nested(c,f,def->body,def->body_count);
    for(int i=0;i<c->p->nclasses;i++) if(c->p->classes[i]->encl==f) fill_class(c,c->p->classes[i]);   /* its classes */
    count_bindings(f,def->body,def->body_count,0,0);
    set_fn_const(f,def->body,def->body_count);
    return f;
}
/* a lambda expression's function (made once; checked inline where it appears) */
AFunc *new_lambda(Ck *c, Expr *e){
    AFunc *f=PPY_NEW0(AFunc);
    f->name="<lambda>"; f->mod=c->mod; f->outer=c->fn; f->lam=e; f->line=e->line; f->star=f->dstar=-1;
    if(e->name) f->shown=e->name;                                          /* (a type made a function: <class 'list'>) */
    if(e->neparam>16) ck_err(c,e->line,"too many parameters");
    f->nparams=e->neparam; f->kwonly=f->nparams;
    if(e->edefaults){ f->star=e->estar; f->dstar=e->edstar; f->kwonly=e->ekwonly; }          /* (lambdas the compiler makes have none) */
    f->params=PPY_NEW_ARR(AVar*,f->nparams>0?f->nparams:1);
    f->defaults=PPY_NEW_ARR(Expr*,f->nparams>0?f->nparams:1);
    for(int i=0;i<f->nparams;i++){ if(symtab_find(&f->locals,e->eparams[i])) ck_err(c,e->line,"duplicate lambda parameter '%s'",e->eparams[i]);
        Ty *t=NULL;
        if(i==f->star){ t=ty_new(TY_LIST,ty_var(),NULL); t->tup=1; } else if(i==f->dstar) t=ty_dict(TY_STR_T,ty_var());
        f->params[i]=new_local(f,e->eparams[i],t); f->defaults[i]=e->edefaults?e->edefaults[i]:NULL; }
    f->ret=ty_var(); f->returns_value=1;
    add_func(c->p,f);
    return f;
}
/* the function's type as a value */
/* Callable[[regular params..., *args item, **kwargs value], ret]: a value of
   it can be called with positionals (extras into *args) and, given
   **kwargs, keywords. Keyword-only parameters cannot be expressed. */
const char *fn_type_problem(AFunc *f){
    if(f->is_async) return "a coroutine";
    int nreg=f->nparams-(f->star>=0)-(f->dstar>=0);
    if(f->star>=0 && f->star!=nreg) return "keyword-only parameters";
    if(f->dstar>=0 && f->dstar!=f->nparams-1) return "keyword-only parameters";
    return NULL;
}
Ty *fn_type(Ck *c, AFunc *f, int line){
    if(f->check_failed) ck_err(c,line,"%s() has errors (reported above): it cannot be used here",f->name);   /* (check mode) */
    const char *why=fn_type_problem(f);
    if(why && !(c->ep_value && f->is_async && !f->outer && !f->cls && !strcmp(why,"a coroutine"))) ck_err(c,line,"%s() cannot be used as a value in compiled code (%s)",f->name,why);
    if(!f->fty){ Ty *ps[16]; int n=0;
        for(int i=0;i<f->nparams;i++) ps[n++]=i==f->star||i==f->dstar ? ty_find(f->params[i]->ty)->elem : f->params[i]->ty;
        f->fty=ty_func(ps,n,f->ret); f->fty->tup=(f->star>=0?1:0)|(f->dstar>=0?2:0);
        int nreg= f->star>=0 ? f->star : f->dstar>=0 ? f->dstar : f->nparams, nd=0;   /* (keyword-only ones are parameters of the value too) */
        while(nd<nreg && f->defaults[nreg-1-nd]) nd++;
        f->fty->ndef=nd;
        if(f->lam){ f->fty->names=PPY_NEW_ARR(char*,n+1); for(int i=0;i<n && i<f->lam->neparam;i++) f->fty->names[i]=f->lam->eparams[i]; }
        if(f->def && !f->lam){ f->fty->names=PPY_NEW_ARR(char*,n+1); for(int i=0;i<n && i<f->def->param_count;i++) f->fty->names[i]=f->def->params[i];
            if(f->star<0 && f->kwonly<nreg) f->fty->kwo=nreg-f->kwonly; } }
    return f->fty;
}

/* self.name = ... (also in a tuple target) somewhere in statements b (not in nested defs) */
static int target_sets(Expr *t, const char *self, const char *name){
    if(!t) return 0;
    if(t->kind==EXPR_ATTRIBUTE && t->a && t->a->kind==EXPR_NAME && !strcmp(t->a->name,self) && !strcmp(t->name,name)) return 1;
    if(t->kind==EXPR_TUPLE || t->kind==EXPR_LIST){ for(int i=0;i<t->count;i++) if(target_sets(t->items[i],self,name)) return 1; }
    return 0;
}
static int stmts_set_self(Stmt **b, int n, const char *self, const char *name){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_FUNCTION_DEF || s->kind==STMT_CLASS_DEF) continue;
        if(s->kind==STMT_ASSIGN) for(int k=0;k<s->ntargets;k++) if(target_sets(s->targets[k],self,name)) return 1;
        if(stmts_set_self(s->body,s->body_count,self,name) || stmts_set_self(s->orelse,s->orelse_count,self,name)) return 1; }
    return 0;
}
/* a method of class k sets self.name */
int class_assigns_self(AClass *k, const char *name){
    Stmt *def=k->def; if(!def) return 0;
    for(int i=0;i<def->body_count;i++){ Stmt *m=def->body[i];
        if(m->kind==STMT_FUNCTION_DEF && m->param_count>0 && stmts_set_self(m->body,m->body_count,m->params[0],name)) return 1; }
    return 0;
}
/* self.name: T = v in a method: the field, with its type, before any code using it is checked */
void annotated_self_fields(Ck *c, AClass *cls, AotUnit *u, const char *self, Stmt **b, int n){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_ASSIGN && s->ann){ AAssign *a=aot_assign(u,s);
            if(a->ann && a->ntarget==1 && a->target[0]->kind==EXPR_ATTRIBUTE && a->target[0]->a->kind==EXPR_NAME &&
               !strcmp(a->target[0]->a->name,self) && !aot_find_field(cls,a->target[0]->name)){
                if(!a->annot) a->annot=type_of(c,a->ann,s->line);
                add_field(cls,a->target[0]->name,a->annot,NULL); } }
        if(s->kind==STMT_FUNCTION_DEF || s->kind==STMT_CLASS_DEF) continue;
        annotated_self_fields(c,cls,u,self,s->body,s->body_count);
        annotated_self_fields(c,cls,u,self,s->orelse,s->orelse_count); }
}
AField *add_field(AClass *cls, const char *name, Ty *ty, Expr *init){
    AField *fd=PPY_NEW0(AField); fd->name=xstrdup2(name); fd->ty=ty?ty:ty_var(); fd->init=init; fd->mod=cls->mod;
    if(cls->nfields==cls->fcap){ cls->fcap=cls->fcap?cls->fcap*2:8; cls->fields=(AField**)xrealloc(cls->fields,sizeof(AField*)*(size_t)cls->fcap); }
    cls->fields[cls->nfields++]=fd;
    return fd;
}
AClass *new_class(Ck *c, AModule *m, Stmt *def){
    AProg *p=c->p; AClass *cls=PPY_NEW0(AClass);
    cls->name=xstrdup2(def->name); cls->mod=m; cls->def=def; cls->id=p->nclasses;
    cls->qualname=cls->name; cls->symname=cls->name; def->aux=cls;
    if(p->nclasses==p->ccap){ p->ccap=p->ccap?p->ccap*2:16; p->classes=(AClass**)xrealloc(p->classes,sizeof(AClass*)*(size_t)p->ccap); }
    p->classes[p->nclasses++]=cls;
    return cls;
}
/* a function's qualified name: C.m, f.<locals>.g */
static void fn_qual(AFunc *f, char *out, size_t n){
    if(f->outer){ char o[400]; fn_qual(f->outer,o,sizeof o); snprintf(out,n,"%s.<locals>.%s",o,f->name); }
    else if(f->cls) snprintf(out,n,"%s.%s",f->cls->qualname,f->name);
    else snprintf(out,n,"%s",f->name);
}
/* A class defined in a class body (Outer.Inner) or in a function: made like the module's
   classes, under its qualified name; the class statement evaluates its attributes. */
AClass *new_inner_class(Ck *c, AModule *m, Stmt *s, AClass *outer, AFunc *encl){
    char q[600];
    if(outer) snprintf(q,sizeof q,"%s.%s",outer->qualname,s->name);
    else { char fq[500]; fn_qual(encl,fq,sizeof fq); snprintf(q,sizeof q,"%s.<locals>.%s",fq,s->name); }
    AClass *cls=new_class(c,m,s);
    cls->qualname=xstrdup2(q); cls->outer=outer; cls->encl=outer?outer->encl:encl;
    char h[640]; snprintf(h,sizeof h,"%s",q);
    for(int k=2;module_sym(m,h);k++) snprintf(h,sizeof h,"%s#%d",q,k);
    symtab_add(&m->syms,h,AS_CLASS,cls); cls->symname=symtab_find(&m->syms,h)->name;
    for(int i=0;i<s->body_count;i++) if(s->body[i]->kind==STMT_CLASS_DEF) new_inner_class(c,m,s->body[i],cls,NULL);
    return cls;
}
AClass *aot_nested_class(AProg *p, AClass *cls, const char *name){     /* Outer.Inner (inherited too) */
    for(;cls;cls=cls->base) for(int i=0;i<p->nclasses;i++){ AClass *k=p->classes[i]; if(k->outer==cls && !strcmp(k->name,name)) return k; }
    return NULL;
}
/* The scopes a function sees classes of by name: its own, the enclosing functions', and for a
   method of a class defined in a function, that function's. */
AFunc *next_scope(AFunc *e){ return e->outer ? e->outer : e->cls ? e->cls->encl : NULL; }
ASym *local_class_in(AFunc *e, const char *name){
    for(;e;e=next_scope(e)){ if(!e->def) return NULL; ASym *s=symtab_find(&e->locals,name); if(s && s->kind==AS_CLASS) return s; }
    return NULL;
}
ASym *local_class(Ck *c, const char *name){
    ASym *s=NULL;
    if(c->sig_fn) s=local_class_in(c->sig_fn,name);
    if(!s && c->sig_cls && c->sig_cls->encl) s=local_class_in(c->sig_cls->encl,name);
    if(!s && c->fn) s=local_class_in(c->fn,name);
    return s;
}
