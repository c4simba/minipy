/* ========================= Typed compiler, types: container literals =========================
   List / dict / set / tuple displays, records (dicts with fixed str
   keys), built-in functions as values; an expression checked against
   the type it goes to. */

#include "check.h"

/* [1, 2.5] is a float list; [Rect(), Circle()] a list of their nearest common base class */
/* Could values of types a and b go into one container? (no type variables bound) */
static int could_unify(Ty *a, Ty *b){
    a=ty_find(a); b=ty_find(b);
    if(a==b || a->k==TY_VAR || b->k==TY_VAR) return 1;
    if(numeric(a) && numeric(b)) return 1;
    if(a->k!=b->k) return 0;
    if(a->k==TY_OBJ) return aot_subclass(a->cls,b->cls) || aot_subclass(b->cls,a->cls);
    if(a->k==TY_DICT && !could_unify(ty_dkey(a),ty_dkey(b))) return 0;
    if(a->k==TY_TUPLE){ if(a->nelems!=b->nelems || !names_eq(a,b)) return 0; for(int i=0;i<a->nelems;i++) if(!could_unify(a->elems[i],b->elems[i])) return 0; return 1; }
    if(has_elem(a) && a->k!=TY_FUNC) return could_unify(a->elem,b->elem);
    return 1;
}
/* {"id": 5, "name": "x"}: distinct str-literal keys */
int record_keys(Expr *e){
    if(e->count<2 || e->count>64) return 0;
    for(int i=0;i<e->count;i++){ Expr *k=e->items[i];
        if(k->akind || k->kind!=EXPR_LITERAL || k->tok->kind!=T_STRING) return 0;
        for(int j=0;j<i;j++) if(!strcmp(e->items[j]->tok->text,k->tok->text)) return 0; }
    return 1;
}
/* such keys and values of types no one type covers -> a record (a tuple
   whose items have the keys as names) */
Ty *record_of(Expr *e, Ty **vts){
    if(!record_keys(e)) return NULL;
    int conflict=0;
    for(int i=0;i<e->count && !conflict;i++) for(int j=0;j<i;j++) if(!could_unify(vts[i],vts[j])){ conflict=1; break; }
    if(!conflict) return NULL;
    Ty *t=ty_tuple(vts,e->count); t->names=MPY_NEW_ARR(char*,e->count);
    for(int i=0;i<e->count;i++) t->names[i]=e->items[i]->tok->text;
    return t;
}
/* a display with *xs or **d items */
int has_splat(Expr *e){ for(int i=0;i<e->count;i++) if(e->items[i]->akind==1||e->items[i]->akind==2) return 1; return 0; }
Ty *literal_elem(Ck *c, Expr **items, int n){
    int fl=0, num=1, obj=n>0; AClass *base=NULL;
    for(int i=0;i<n;i++){ Ty *t=ty_find(ck_expr(c,items[i]));
        if(t->k==TY_FLOAT) fl=1; else if(t->k!=TY_INT&&t->k!=TY_BOOL) num=0;
        if(t->k!=TY_OBJ) obj=0;
        else if(obj){ if(!base) base=t->cls; else { while(base && !aot_subclass(t->cls,base)) base=base->base; if(!base) obj=0; } } }
    if(fl&&num) return TY_FLOAT_T;
    if(obj&&base) return ty_new(TY_OBJ,NULL,base);
    return ty_var();
}
/* (a, b, c) iterated or tested with `in`: a list of one element type */
Ty *ck_tuple_as_list(Ck *c, Expr *e){
    Ty *el=literal_elem(c,e->items,e->count);
    for(int i=0;i<e->count;i++){ Ty *t=ck_expr(c,e->items[i]); no_void(c,t,e->line); expect(c,el,t,e->line,"an element"); }
    Ty *t=ty_new(TY_LIST,el,NULL); t->tup=1;            /* (only read: a constant one is static) */
    xinfo(e)->kind=X_NONE; xinfo(e)->ty=t;
    return t;
}
/* A container literal checked against the type it is going to (list[Base] = [Derived(), ...]). */
/* a built-in function used as a value (apply(len, s), reduce(max, xs)): lambda x: len(x) */
int builtin_fn_value(Ck *c, Expr *e, int arity){
    static const char *const one[]={"len","abs","repr","ascii","ord","chr","hex","bin","oct","hash","sorted","sum","any","all","reversed",
        "iter","next","callable","round","min","max","print","id","format",NULL}, *const two[]={"divmod","pow","isinstance","getattr","hasattr",NULL};
    static const char *const types[]={"list","dict","set","frozenset","tuple","int","float","str","bool","bytes",NULL};
    if(e->kind!=EXPR_NAME || lookup(c,e->name) || e->name[0]=='\001') return 0;
    int n=0, type=0; for(int i=0;one[i];i++) if(!strcmp(one[i],e->name)) n=1;
    for(int i=0;two[i];i++) if(!strcmp(two[i],e->name)) n=2;
    for(int i=0;types[i];i++) if(!strcmp(types[i],e->name)){ type=1; n=arity>0?arity:0; }   /* defaultdict(list): lambda: list() */
    if(!n && !type) return 0;
    if(arity>0) n=arity;
    char shown[64]; snprintf(shown,sizeof shown,"<class '%s'>",e->name);
    char fn[64]; snprintf(fn,sizeof fn,"\001%s",e->name);
    Expr *call=xnew(EXPR_CALL,e->line); call->a=name_expr(xstrdup2(fn),e->line);
    char **ps=MPY_NEW_ARR(char*,n); Expr **ds=MPY_NEW_ARR(Expr*,n);
    for(int i=0;i<n;i++){ char v[16]; snprintf(v,sizeof v,"_a%d",i); ps[i]=xstrdup2(v); xpush(call,name_expr(ps[i],e->line)); }
    void *keep=e->ty; memset(e,0,sizeof *e);
    e->kind=EXPR_LAMBDA; e->line=call->line; e->ty=keep; e->eparams=ps; e->neparam=n; e->edefaults=ds; e->estar=e->edstar=-1; e->ekwonly=n; e->a=call;
    if(type) e->name=xstrdup2(shown);                                    /* (printed as the type: <class 'list'>) */
    return 1;
}
/* the number of parameters of a Callable[[...], R] annotation (also inside `| None`), else -1 */
int ann_callable_arity(Expr *a){
    if(!a) return -1;
    if(a->kind==EXPR_BINARY && a->op==T_PIPE){ int r=ann_callable_arity(a->a); return r>=0?r:ann_callable_arity(a->b); }
    if(a->kind==EXPR_INDEX && a->a->kind==EXPR_NAME && (!strcmp(a->a->name,"Callable")||!strcmp(a->a->name,"Optional"))){
        Expr *x=a->b; if(!strcmp(a->a->name,"Optional")) return ann_callable_arity(x);
        if(x && x->kind==EXPR_TUPLE && x->count==2 && x->items[0]->kind==EXPR_LIST) return x->items[0]->count;
        return 0; }
    return -1;
}
Ty *ck_expr_want(Ck *c, Expr *e, Ty *want){
    Ty *w=ty_find(want);
    if(e->kind==EXPR_NAME && w->k==TY_FUNC && builtin_fn_value(c,e,w->nelems)) return ck_expr_want(c,e,want);
    if((e->kind==EXPR_LIST||e->kind==EXPR_SET||e->kind==EXPR_TUPLE||e->kind==EXPR_DICT) && has_splat(e)) return ck_expr(c,e);
    if(e->count && (((e->kind==EXPR_LIST&&w->k==TY_LIST)||(e->kind==EXPR_SET&&w->k==TY_SET)))){
        for(int i=0;i<e->count;i++){ Ty *t=ck_expr(c,e->items[i]); no_void(c,t,e->line); expect(c,w->elem,t,e->line,"an element"); }
        xinfo(e)->kind=X_NONE; xinfo(e)->ty=w; return w;
    }
    if(e->kind==EXPR_LIST && !e->count && w->k==TY_LIST && w->tup && xinfo(e)->ty && ty_find(xinfo(e)->ty)->k==TY_LIST && ty_find(xinfo(e)->ty)->tup){
        xinfo(e)->kind=X_NONE; return xinfo(e)->ty; }                            /* (an empty one made so before) */
    if(e->kind==EXPR_TUPLE && w->k==TY_LIST && w->tup && !has_splat(e)){        /* x: tuple[str, ...] = ("a", "b"): that kind of tuple */
        for(int i=0;i<e->count;i++){ Ty *t=ck_expr_want(c,e->items[i],w->elem); no_void(c,t,e->line); expect(c,w->elem,t,e->line,"a tuple item"); }
        e->kind=EXPR_LIST; xinfo(e)->kind=X_NONE; xinfo(e)->ty=w; return w;
    }
    if(e->kind==EXPR_TUPLE && w->k==TY_TUPLE && w->nelems==e->count){
        for(int i=0;i<e->count;i++){ Ty *t=ck_expr_want(c,e->items[i],w->elems[i]); no_void(c,t,e->line); expect(c,w->elems[i],t,e->line,"a tuple item"); }
        xinfo(e)->kind=X_NONE; xinfo(e)->ty=w; return w;
    }
    if(e->kind==EXPR_CALL && w->k==TY_OBJ && w->cls && (w->cls->origin || w->cls->generic) && !xinfo(e)->icls){   /* q: deque[int] = deque(): that instance */
        AClass *k=class_expr(c,e->a);
        if(k && (k==w->cls->origin || k==w->cls)) xinfo(e)->icls=w->cls; }
    if(e->count && e->kind==EXPR_DICT && w->k==TY_DICT){
        for(int i=0;i<e->count;i++){ expect(c,ty_dkey(w),ck_expr(c,e->items[i]),e->line,"a dictionary key");
            Ty *t=ck_expr(c,e->vals[i]); no_void(c,t,e->line); expect(c,w->elem,t,e->line,"a dictionary value"); }
        xinfo(e)->kind=X_NONE; xinfo(e)->ty=w; return w;
    }
    return ck_expr(c,e);
}

