/* ========================= Typed compiler, types: narrowing =========================
   Conditions: isinstance() and `is None` tests that narrow a type,
   decided statically where they can be. */

#include "check.h"

/* An expression only tested for truth (if/while/assert, not, `x if c else y`, comprehension
   filters): `a and b` there tests a, then b, so they may be of different types */
/* isinstance(x, list) (int, str, dict, ... or a tuple of them) when x's type is known and not a
   class's: 1 / 0, decided when compiling (a generic function's copy keeps only its own branch);
   -1: not such a test */
/* an expression without side effects (names, constants, attributes, comparisons and logic of them) */
int pure_test(Expr *e){
    if(!e) return 1;
    switch(e->kind){
        case EXPR_NAME: case EXPR_LITERAL: case EXPR_TRUE: case EXPR_FALSE: case EXPR_NONE: return 1;
        case EXPR_ATTRIBUTE: return pure_test(e->a);
        case EXPR_UNARY: return pure_test(e->a);
        case EXPR_BOOL: return pure_test(e->a) && pure_test(e->b);
        case EXPR_COMPARE: for(int i=0;i<e->count;i++) if(!pure_test(e->items[i])) return 0; return 1;
        default: return 0;
    }
}
int static_isinstance(Ck *c, Expr *e){
    if(!e) return -1;
    if(e->kind==EXPR_UNARY && e->op==T_NOT){ int r=static_isinstance(c,e->a); return r<0?r:!r; }
    if(e->kind==EXPR_BOOL){                                   /* x and isinstance(v, list) with v an int: false (x has no effects) */
        int a=static_isinstance(c,e->a), b=static_isinstance(c,e->b), and= e->op==T_AND;
        if(a==-2 || b==-2) return -2;
        if(a<0 && b<0) return -1;
        if(a>=0 && b>=0) return and ? (a && b) : (a || b);
        int known= a>=0 ? a : b; Expr *other= a>=0 ? e->b : e->a;
        if(and && known==0 && pure_test(other)) return 0;
        if(!and && known==1 && pure_test(other)) return 1;
        return -1; }
    if(e->kind==EXPR_TRUE || e->kind==EXPR_FALSE) return e->kind==EXPR_TRUE;   /* (a hasattr() decided) */
    if(e->kind==EXPR_CALL && e->a->kind==EXPR_NAME && !strcmp(e->a->name,"hasattr") && !lookup(c,"hasattr") && e->count==2 && !e->items[0]->akind && !e->items[1]->akind
       && e->items[1]->kind==EXPR_LITERAL && e->items[1]->tok->kind==T_STRING){                 /* hasattr(o, "name"): what o's type has */
        Ty *t=ty_find(ck_expr(c,e->items[0])); if(t->k==TY_VAR) return -2;
        ck_expr(c,e); return e->kind==EXPR_TRUE ? 1 : e->kind==EXPR_FALSE ? 0 : -1; }
    if(e->kind==EXPR_CALL && e->a->kind==EXPR_ATTRIBUTE && !strcmp(e->a->name,"_void") && is_sys(c,e->a->a) && e->count==1){   /* sys._void(f(x)): f returns nothing */
        Ty *t=ty_find(ck_expr(c,e->items[0]));
        if(t->k==TY_VAR && !c->strict) return -2;
        return t->k==TY_VOID ? 1 : 0; }
    if(e->kind!=EXPR_CALL || e->a->kind!=EXPR_NAME || strcmp(e->a->name,"isinstance") || lookup(c,"isinstance") || e->count!=2 || e->items[0]->akind || e->items[1]->akind) return -1;
    Expr *k=e->items[1]; Expr *one[1]={k}; Expr **ks= k->kind==EXPR_TUPLE ? k->items : one; int nk= k->kind==EXPR_TUPLE ? k->count : 1;
    static const char *const names[]={"int","float","str","bytes","list","dict","set","tuple","bool","frozenset",NULL};
    static const TyKind kinds[]={TY_INT,TY_FLOAT,TY_STR,TY_BYTES,TY_LIST,TY_DICT,TY_SET,TY_TUPLE,TY_BOOL,TY_SET};   /* (set / frozenset: the frozen flag) */
    AClass *kc[16];                                               /* (the classes of the program among them) */
    if(nk>16) return -1;
    for(int i=0;i<nk;i++){ int ok=0; kc[i]=NULL;
        if(ks[i]->kind==EXPR_NAME){ ASym *ks_sym=lookup(c,ks[i]->name);
            if(!ks_sym){ for(int q=0;names[q];q++) if(!strcmp(names[q],ks[i]->name)) ok=1; }
            else if(ks_sym->kind==AS_CLASS){ kc[i]=(AClass*)ks_sym->p; ok=!kc[i]->origin && (!kc[i]->builtin || aot_is_exception(kc[i])); } }
        else if(ks[i]->kind==EXPR_ATTRIBUTE){ AClass *ce=class_expr(c,ks[i]);   /* module.Class */
            if(ce){ kc[i]=ce; ok=!ce->origin && (!ce->builtin || aot_is_exception(ce)); } }
        if(!ok) return -1; }
    Ty *t=ty_find(ck_expr(c,e->items[0]));
    if(t->k==TY_VAR && t->opt && c->defaulting) return 0;          /* (only ever None, in the end: an instance of none of them) */
    if(t->k==TY_VAR) return -2;                                   /* (not known yet: decided in a later pass) */
    if(t->k==TY_TYPE) return 0;                                   /* a class: an instance of classes only of type / object (not among these) */
    if(t->k==TY_VOID) return -1;
    int undecided=0;
    for(int i=0;i<nk;i++){
        if(kc[i] && kc[i]->mod && kc[i]->mod->name && !strcmp(kc[i]->mod->name,"numbers") && t->k!=TY_OBJ){   /* numbers.Integral ...: the built-in numbers' places */
            const char *n=kc[i]->name; int num= t->k==TY_INT||t->k==TY_BOOL ? 3 : t->k==TY_FLOAT ? 2 : 0;
            int need= !strcmp(n,"Integral")||!strcmp(n,"Rational") ? 3 : !strcmp(n,"Real") ? 2 : !strcmp(n,"Complex")||!strcmp(n,"Number") ? 1 : 9;
            if(num>=need) return t->opt ? -1 : 1;
            continue; }
        if(kc[i] && kc[i]->mod && kc[i]->mod->name && !strcmp(kc[i]->mod->name,"numbers") && t->k==TY_OBJ && t->cls==complex_class){
            const char *n=kc[i]->name; if(!strcmp(n,"Complex")||!strcmp(n,"Number")) return t->opt ? -1 : 1; continue; }
        if(kc[i]){                                                /* a class: an instance of it or of a subclass */
            if(t->k!=TY_OBJ) continue;
            if(kc[i]->generic){                                   /* (a generic class: one of its instances, or a subclass of one) */
                if(!t->cls) return -1;
                for(AClass *b=t->cls;b;b=b->base) if(b==kc[i] || b->origin==kc[i]) return t->opt ? -1 : 1;
                if(t->cls->generic || (t->cls->builtin && !aot_is_exception(t->cls))) return -1;
                if(aot_subclass(kc[i],t->cls)) undecided=1;       /* (a t may be one of kc's) */
                for(int j=0;j<kc[i]->ninsts;j++) if(aot_subclass(kc[i]->insts[j],t->cls)) undecided=1;
                continue; }
            if(!t->cls || t->cls->generic || t->cls->origin || (t->cls->builtin && !aot_is_exception(t->cls))) return -1;
            if(aot_subclass(t->cls,kc[i])) return t->opt ? -1 : 1;
            if(aot_subclass(kc[i],t->cls)) undecided=1;           /* (a t may be one of kc's) */
            continue; }
        for(int q=0;names[q];q++) if(!strcmp(names[q],ks[i]->name)){
            if(t->k==TY_SET && kinds[q]==TY_SET && (q==6)==(t->tup!=0)) continue;      /* (a set is not a frozenset) */
            if(t->k==kinds[q] || (q==0 && t->k==TY_BOOL)) return t->opt ? -1 : 1; } }   /* (a bool is an int; an optional one may be None) */
    return undecided ? -1 : 0;
}
/* `p is None` / `p is not None` for a parameter defaulting to None that no call gives (its type
   still unknown at the end): it is None (as an optional int), the test decided -> 1 / 0, else -1 */
int static_none_param(Ck *c, Expr *e){
    if(!c->fn || !c->fn->def || !e || e->kind!=EXPR_COMPARE || e->count!=2) return -1;
    int code=e->items[1]->akind; Expr *a=e->items[0], *b=e->items[1];
    if((code!=CMP_IS && code!=CMP_ISNOT) || a->kind!=EXPR_NAME || b->kind!=EXPR_NONE) return -1;
    Stmt *def=c->fn->def; int p=-1;
    for(int i=0;i<def->param_count;i++) if(!strcmp(def->params[i],a->name)) p=i;
    if(p<0 || !def->pdefaults || !def->pdefaults[p] || def->pdefaults[p]->kind!=EXPR_NONE || p>=c->fn->nparams) return -1;
    if(p<16 && (!c->fn->cls || c->fn->origin || !method_overridden(c,c->fn)) && (c->fn->origin || c->fn->pristine) && !stores_name(c,c->fn->body,c->fn->nbody,a->name)){   /* (a generic method: called through pick_instance) */
        if((c->fn->none_omit>>p&1) && !(c->fn->none_given>>p&1)){      /* every call of this copy leaves it out */
            Ty *t=ty_find(c->fn->params[p]->ty);
            if(t->k==TY_VAR) unify(c,t,optional(TY_INT_T));
            if(ty_find(c->fn->params[p]->ty)->opt){ c->fn->none_folded|=1u<<p; return code==CMP_IS; } }
        if(!c->fn->none_calls && !c->strict) return -2; }                 /* (no call seen yet: what it is waits for them) */
    if(!c->strict) return -1;
    Ty *t=ty_find(c->fn->params[p]->ty);
    if(t->k!=TY_VAR) return -1;
    unify(c,t,optional(TY_INT_T));
    return code==CMP_IS;
}
Ty *ck_cond(Ck *c, Expr *e){
    if(e->kind==EXPR_BOOL){ no_void(c,ck_cond(c,e->a),e->line); no_void(c,ck_cond(c,e->b),e->line); return xinfo(e)->ty=TY_BOOL_T; }
    if(e->kind==EXPR_UNARY && e->op==T_NOT){ no_void(c,ck_cond(c,e->a),e->line); return xinfo(e)->ty=TY_BOOL_T; }
    return ck_expr(c,e);
}
/* an IntEnum / IntFlag member where an int is wanted (compared with an int, in arithmetic, an int
   parameter): its .value */
int int_enum_ty(Ty *t){ t=ty_find(t); return t && t->k==TY_OBJ && t->cls && t->cls->enum_int && !t->opt; }
Ty *as_enum_value(Ck *c, Expr *x){
    Expr *inner=PPY_NEW0(Expr); *inner=*x; int ak=x->akind; char *kw=x->kw;
    memset(x,0,sizeof *x); x->kind=EXPR_ATTRIBUTE; x->line=inner->line; x->a=inner; x->name="value"; x->akind=ak; x->kw=kw;
    inner->akind=0; inner->kw=NULL;
    return ck_expr(c,x);
}
