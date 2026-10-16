/* ========================= Typed compiler, types: module declarations =========================
   What a module declares before its code is checked: type aliases and
   TypeVars, @lru_cache / @cache, @cached_property, sys.platform branches
   (decided while compiling), the module's functions and classes. */

#include "check.h"
#include "targets/targets.h"

/* typing.X or X (a name of the typing module or a built-in generic) */
static const char *typing_name(Expr *e){
    if(e->kind==EXPR_NAME) return e->name;
    if(e->kind==EXPR_LITERAL && e->tok->kind==T_STRING && e->tok->i!=2) return e->tok->text;    /* "TypeAlias" */
    if(e->kind==EXPR_ATTRIBUTE && e->a->kind==EXPR_NAME && !strcmp(e->a->name,"typing")) return e->name;
    return NULL;
}
/* type aliases (type X = ..., X: TypeAlias = ..., X = list[int]) and TypeVars: types, not variables */
static void collect_type_names(Ck *c, AModule *m){
    static const char *const generic[]={"list","dict","set","tuple","frozenset","type","Optional","Union","Callable","List","Dict","Set","Tuple",
        "FrozenSet","Sequence","MutableSequence","Mapping","MutableMapping","Iterable","Iterator","Generator","AbstractSet",NULL};
    Stmt **b=m->unit->ast->body; int n=m->unit->ast->body_count;
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_PASS && s->is_alias){ if(module_sym(m,s->name)) ck_err(c,s->line,"'%s' is defined twice",s->name); symtab_add(&m->syms,s->name,AS_TYPEALIAS,s); continue; }
        if(s->kind!=STMT_ASSIGN || s->ntargets!=1 || s->targets[0]->kind!=EXPR_NAME || !s->value || s->aug) continue;
        const char *nm=s->targets[0]->name; Expr *v=s->value; const char *an=s->ann?typing_name(s->ann):NULL;
        if(v->kind==EXPR_CALL && typing_name(v->a) && (!strcmp(typing_name(v->a),"TypeVar")||!strcmp(typing_name(v->a),"ParamSpec")||!strcmp(typing_name(v->a),"TypeVarTuple"))){
            symtab_add(&m->syms,nm,AS_TYPEVAR,(void*)nm); s->kind=STMT_PASS; continue; }
        int alias= an && !strcmp(an,"TypeAlias");
        if(!alias && !s->ann && v->kind==EXPR_INDEX && typing_name(v->a)) for(int k=0;generic[k];k++) if(!strcmp(typing_name(v->a),generic[k])) alias=1;
        if(alias){ s->kind=STMT_PASS; s->is_alias=1; s->ann=v; s->name=(char*)nm; symtab_add(&m->syms,nm,AS_TYPEALIAS,s); }
    }
}
/* @lru_cache / @cache on a module function: a wrapper keeps the results in a dict by the
   arguments (least recently used first, as functools' cache) and calls the function itself */
static int memo_deco(Expr *d, int *maxsize){
    Expr *f= d->kind==EXPR_CALL ? d->a : d;
    const char *nm= f->kind==EXPR_NAME ? f->name : f->kind==EXPR_ATTRIBUTE && f->a->kind==EXPR_NAME && !strcmp(f->a->name,"functools") ? f->name : NULL;
    if(!nm || (strcmp(nm,"lru_cache") && strcmp(nm,"cache"))) return 0;
    *maxsize= nm[0]=='c' ? -1 : 128;
    if(d->kind==EXPR_CALL) for(int i=0;i<d->count;i++){ Expr *a=d->items[i];
        if(a->akind==3 && strcmp(a->kw,"maxsize")) continue;           /* (typed=...) */
        if(a->kind==EXPR_NONE) *maxsize=-1;
        else if(a->kind==EXPR_LITERAL && a->tok->kind==T_NUMBER && !a->tok->is_float) *maxsize=(int)a->tok->i;
        else return -1; }
    return 1;
}
static void memoize_defs(Ck *c, AModule *m){
    AotUnit *u=m->unit; Stmt *mod=u->ast;
    for(int i=0;i<mod->body_count;i++){ Stmt *s=mod->body[i]; int maxsize=0, which=-1;
        if(s->kind!=STMT_FUNCTION_DEF) continue;
        for(int k=0;k<s->decorator_count;k++){ int r=memo_deco(aot_expr(u,&s->decorator_exprs[k]),&maxsize); if(r<0) ck_err(c,s->line,"lru_cache(maxsize=...) needs a number or None in compiled code"); if(r) which=k; }
        if(which<0) continue;
        if(s->decorator_count>1) ck_err(c,s->line,"@lru_cache together with other decorators is not supported in compiled code");
        if(s->star_index>=0 || s->dstar_index>=0) ck_err(c,s->line,"@lru_cache on a function with *args / **kwargs is not supported in compiled code");
        s->decorator_count=0;
        if(maxsize==0) continue;                                     /* (no cache) */
        char un[300], ca[300]; snprintf(un,sizeof un,"__ppy_uncached_%s",s->name); snprintf(ca,sizeof ca,"__ppy_cache_%s",s->name);
        Txt t={0}; for(int l=1;l<s->line;l++) tx(&t,"\n");
        tx(&t,"%s = {}\ndef %s(",ca,s->name);
        for(int p=0;p<s->param_count;p++) tx(&t,"%s%s%s",p?", ":"",s->params[p],s->pdefaults&&s->pdefaults[p]?"=None":"");
        tx(&t,"):\n    __ppy_k = ");
        if(!s->param_count) tx(&t,"0"); else { tx(&t,"("); for(int p=0;p<s->param_count;p++) tx(&t,"%s, ",s->params[p]); tx(&t,")"); }
        tx(&t,"\n    if __ppy_k in %s:\n        __ppy_v = %s.pop(__ppy_k)\n        %s[__ppy_k] = __ppy_v\n        return __ppy_v\n",ca,ca,ca);
        tx(&t,"    __ppy_r = %s(",un); for(int p=0;p<s->param_count;p++) tx(&t,"%s%s",p?", ":"",s->params[p]); tx(&t,")\n    %s[__ppy_k] = __ppy_r\n",ca);
        if(maxsize>0) tx(&t,"    if len(%s) > %d:\n        del %s[next(iter(%s))]\n",ca,maxsize,ca,ca);
        tx(&t,"    return __ppy_r\n");
        Stmt *blk=py_front_stmts(u->path,t.s,s->line); free(t.s);
        if(!blk || blk->body_count!=2) ck_err(c,s->line,"internal error: lru_cache");
        Stmt *w=blk->body[1];
        for(int p=0;p<s->param_count;p++){ if(s->annotations && p<s->annotation_cap && s->annotations[p]) stmt_set_annotation(w,p,s->annotations[p]);
            if(s->pdefaults && s->pdefaults[p]){ if(!w->pdefaults) w->pdefaults=PPY_NEW_ARR(Expr*,s->param_count+1); w->pdefaults[p]=s->pdefaults[p]; } }
        w->returns=s->returns;
        s->name=xstrdup2(un);
        Stmt **nb=PPY_NEW_ARR(Stmt*,mod->body_count+3); int nn=0;
        for(int j=0;j<i;j++) nb[nn++]=mod->body[j];
        nb[nn++]=blk->body[0]; nb[nn++]=s; nb[nn++]=w;
        for(int j=i+1;j<mod->body_count;j++) nb[nn++]=mod->body[j];
        mod->body=nb; mod->body_count=nn; mod->body_cap=nn;
        i+=2; }
}
/* @cached_property def x(self): ... -> the method as __ppy_cf_x, a class attribute
   __ppy_cv_x = None and a property x computing it once, keeping it there */
static int is_cached_property(Expr *d){
    return (d->kind==EXPR_NAME && !strcmp(d->name,"cached_property"))
        || (d->kind==EXPR_ATTRIBUTE && d->a->kind==EXPR_NAME && !strcmp(d->a->name,"functools") && !strcmp(d->name,"cached_property"));
}
static void cached_properties(Ck *c, AotUnit *u, Stmt *cls){
    for(int i=0;i<cls->body_count;i++){ Stmt *s=cls->body[i];
        if(s->kind==STMT_CLASS_DEF){ cached_properties(c,u,s); continue; }
        if(s->kind!=STMT_FUNCTION_DEF) continue;
        int which=-1; for(int k=0;k<s->decorator_count;k++) if(is_cached_property(aot_expr(u,&s->decorator_exprs[k]))) which=k;
        if(which<0) continue;
        if(s->decorator_count>1) ck_err(c,s->line,"@cached_property together with other decorators is not supported in compiled code");
        if(s->param_count!=1) ck_err(c,s->line,"a cached_property takes only self");
        s->decorator_count=0;
        Txt t={0}; for(int l=1;l<s->line;l++) tx(&t,"\n");
        tx(&t,"__ppy_cv_%s = None\n@property\ndef %s(self):\n    __ppy_v = self.__ppy_cv_%s\n    if __ppy_v is None:\n"
              "        __ppy_v = self.__ppy_cf_%s()\n        self.__ppy_cv_%s = __ppy_v\n    return __ppy_v\n",s->name,s->name,s->name,s->name,s->name);
        Stmt *blk=py_front_stmts(u->path,t.s,s->line); free(t.s);
        if(!blk || blk->body_count!=2) ck_err(c,s->line,"internal error: cached_property");
        blk->body[1]->returns=s->returns;
        char nm[300]; snprintf(nm,sizeof nm,"__ppy_cf_%s",s->name); s->name=xstrdup2(nm);
        Stmt **nb=PPY_NEW_ARR(Stmt*,cls->body_count+2); int nn=0;
        for(int j=0;j<=i;j++) nb[nn++]=cls->body[j];
        nb[nn++]=blk->body[0]; nb[nn++]=blk->body[1];
        for(int j=i+1;j<cls->body_count;j++) nb[nn++]=cls->body[j];
        cls->body=nb; cls->body_count=nn; cls->body_cap=nn;
        i+=2; }
}
/* `if sys.platform == "kolibrios":` (!=, in, startswith, not/and/or of such): decided when
   compiling, only the branch for the target is kept (the other may use another system's
   calls, or define the same names) -> 1 true, 0 false, -1 not a platform test */
static const char *target_platform(Ck *c){ return aot_target(c->p->target)->platform; }
static int is_sys_platform(Expr *e){ return e->kind==EXPR_ATTRIBUTE && !strcmp(e->name,"platform") && e->a->kind==EXPR_NAME && !strcmp(e->a->name,"sys"); }
static int is_str_lit(Expr *e){ return e->kind==EXPR_LITERAL && e->tok->kind==T_STRING && e->tok->i!=2; }
static int platform_test(Ck *c, Expr *e){
    if(!e) return -1;
    const char *pf=target_platform(c);
    if(e->kind==EXPR_UNARY && e->op==T_NOT){ int r=platform_test(c,e->a); return r<0?-1:!r; }
    if(e->kind==EXPR_ATTRIBUTE && !strcmp(e->name,"_compiled") && e->a->kind==EXPR_NAME && !strcmp(e->a->name,"sys")) return 1;   /* sys._compiled: the standard library's interpreter-only parts */
    if(e->kind==EXPR_BOOL){ int a=platform_test(c,e->a), b=platform_test(c,e->b); if(a<0||b<0) return -1; return e->op==T_AND ? a&&b : a||b; }
    if(e->kind==EXPR_COMPARE && e->count==2){
        Expr *a=e->items[0], *b=e->items[1]; int code=b->akind;
        if((code==CMP_EQ||code==CMP_NE) && ((is_sys_platform(a)&&is_str_lit(b)) || (is_sys_platform(b)&&is_str_lit(a)))){
            const char *lit=is_str_lit(a)?a->tok->text:b->tok->text; int eq=!strcmp(lit,pf); return code==CMP_EQ?eq:!eq; }
        if((code==CMP_IN||code==CMP_NOTIN) && is_sys_platform(a) && (b->kind==EXPR_TUPLE||b->kind==EXPR_LIST)){
            int in=0; for(int i=0;i<b->count;i++){ if(!is_str_lit(b->items[i])) return -1; if(!strcmp(b->items[i]->tok->text,pf)) in=1; }
            return code==CMP_IN?in:!in; }
    }
    if(e->kind==EXPR_CALL && e->a->kind==EXPR_ATTRIBUTE && !strcmp(e->a->name,"startswith") && is_sys_platform(e->a->a) && e->count==1 && is_str_lit(e->items[0]) && !e->items[0]->akind)
        return !strncmp(pf,e->items[0]->tok->text,strlen(e->items[0]->tok->text));
    return -1;
}
void fold_platform(Ck *c, Stmt ***arr, int *n, int *cap){
    int changed=0;
    for(int i=0;i<*n;i++){ Stmt *s=(*arr)[i];
        if(s->kind==STMT_IF && platform_test(c,s->expr)>=0){ changed=1; break; }
        fold_platform(c,&s->body,&s->body_count,&s->body_cap); fold_platform(c,&s->orelse,&s->orelse_count,&s->orelse_cap); }
    if(!changed) return;
    Stmt **out=NULL; int k=0, oc=0;
    for(int i=0;i<*n;i++){ Stmt *s=(*arr)[i]; int r;
        if(s->kind==STMT_IF && (r=platform_test(c,s->expr))>=0){
            Stmt **b=r?s->body:s->orelse; int bn=r?s->body_count:s->orelse_count;
            for(int j=0;j<bn;j++){ if(k==oc){ oc=oc?oc*2:16; out=(Stmt**)xrealloc(out,sizeof(Stmt*)*(size_t)oc); } out[k++]=b[j]; }
            if(bn && !falls_through(b,bn)) break;                              /* (the rest is for the other platforms) */
            continue; }
        if(k==oc){ oc=oc?oc*2:16; out=(Stmt**)xrealloc(out,sizeof(Stmt*)*(size_t)oc); } out[k++]=s; }
    if(!k){ out=(Stmt**)xrealloc(out,sizeof(Stmt*)); out[k++]=stmt_new(STMT_PASS,NULL,(*arr)[0]->line); }   /* (an emptied block) */
    *arr=out; *n=k; if(cap) *cap=oc>k?oc:k;
    fold_platform(c,arr,n,cap);                                             /* (an elif; what a taken branch holds) */
}
void collect_module(Ck *c, AModule *m){
    c->mod=m;
    fold_platform(c,&m->unit->ast->body,&m->unit->ast->body_count,&m->unit->ast->body_cap);
    memoize_defs(c,m);
    for(int i=0;i<m->unit->ast->body_count;i++) if(m->unit->ast->body[i]->kind==STMT_CLASS_DEF) cached_properties(c,m->unit,m->unit->ast->body[i]);
    collect_ctypes(c,m);                        /* lib = ctypes.CDLL(...) and its declarations: not variables */
    collect_type_names(c,m);
    Stmt **b=m->unit->ast->body; int n=m->unit->ast->body_count;
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_CLASS_DEF){ if(module_sym(m,s->name)) ck_err(c,s->line,"'%s' is defined twice",s->name); AClass *cls=new_class(c,m,s); symtab_add(&m->syms,s->name,AS_CLASS,cls);
            for(int k=0;k<s->body_count;k++) if(s->body[k]->kind==STMT_CLASS_DEF) new_inner_class(c,m,s->body[k],cls,NULL); }
    }
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_FUNCTION_DEF){ if(module_sym(m,s->name)) ck_err(c,s->line,"'%s' is defined twice",s->name);
            if(general_decos(m->unit,s,0)){ symtab_add(&m->syms,hidden_name(s->name,NULL),AS_FUNC,PPY_NEW0(AFunc)); global_var(c,m,s->name,s->line); }   /* name = decorators(function) */
            else symtab_add(&m->syms,s->name,AS_FUNC,PPY_NEW0(AFunc)); }
    }
    for(int i=0;i<n;i++){ Stmt *s=b[i];                  /* error = OSError, insort = insort_right: another name of a class or function (assigned once) */
        if(s->kind!=STMT_ASSIGN || s->ntargets!=1 || s->targets[0]->kind!=EXPR_NAME || !s->value || s->aug || s->ann || s->value->kind!=EXPR_NAME) continue;
        const char *nm=s->targets[0]->name; ASym *k=module_sym(m,s->value->name); if(!k) k=symtab_find(&builtin_syms,s->value->name);
        if(!k || (k->kind!=AS_CLASS && k->kind!=AS_FUNC) || module_sym(m,nm)) continue;
        int again=0; for(int j=0;j<n;j++) if(j!=i && b[j]->kind==STMT_ASSIGN) for(int t=0;t<b[j]->ntargets;t++) if(b[j]->targets[t]->kind==EXPR_NAME && !strcmp(b[j]->targets[t]->name,nm)) again=1;
        if(again) continue;
        symtab_add(&m->syms,nm,k->kind,k->p); s->kind=STMT_PASS; }
    Names globals={0};
    Stmt **rest=PPY_NEW_ARR(Stmt*,n>0?n:1); int nrest=0;
    for(int i=0;i<n;i++) if(b[i]->kind!=STMT_FUNCTION_DEF && b[i]->kind!=STMT_CLASS_DEF){
        Stmt *s=b[i];
        if(s->kind==STMT_IMPORT||s->kind==STMT_FROM_IMPORT) continue;
        if(s->kind==STMT_GLOBAL) continue;
        rest[nrest++]=s;
    }
    names_bound(c,rest,nrest,&globals,NULL);
    for(int i=0;i<nrest;i++){ no_nested_defs(c,rest[i]->body,rest[i]->body_count,0); no_nested_defs(c,rest[i]->orelse,rest[i]->orelse_count,0); }
    for(int i=0;i<globals.n;i++) global_var(c,m,globals.v[i],1);
    free(globals.v); free(rest);
}
/* A fresh copy of a def (made again from the parser's tree): every instance of a
   generic function gets its own expression trees and types. */
