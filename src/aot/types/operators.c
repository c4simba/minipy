/* ========================= Typed compiler, types: operators =========================
   Binary operators and comparisons: built-in ones and __add__ / __lt__
   ... methods. */

#include "check.h"

static int is_aug_op(TokKind op){ return (op>=T_PLUS_ASSIGN && op<=T_SLASH_ASSIGN) || op>=T_PERCENT_ASSIGN; }
const char *op_method(TokKind op){
    switch(op){
        case T_PLUS: case T_PLUS_ASSIGN: return "__add__"; case T_MINUS: case T_MINUS_ASSIGN: return "__sub__";
        case T_STAR: case T_STAR_ASSIGN: return "__mul__"; case T_SLASH: case T_SLASH_ASSIGN: return "__truediv__";
        case T_FLOOR_DIV: case T_FLOOR_DIV_ASSIGN: return "__floordiv__"; case T_PERCENT: case T_PERCENT_ASSIGN: return "__mod__";
        case T_POWER: case T_POWER_ASSIGN: return "__pow__"; case T_AMP: case T_AMP_ASSIGN: return "__and__";
        case T_PIPE: case T_PIPE_ASSIGN: return "__or__"; case T_CARET: case T_CARET_ASSIGN: return "__xor__";
        case T_SHL: case T_SHL_ASSIGN: return "__lshift__"; case T_SHR: case T_SHR_ASSIGN: return "__rshift__";
        case T_AT: case T_AT_ASSIGN: return "__matmul__";
        default: return NULL;
    }
}
/* obj.<name>(a, b, c) for the slice obj[a:b:c] (an omitted bound: None) */
Expr *slice_call(Expr *sl, const char *name, int line){
    Expr *at=xnew(EXPR_ATTRIBUTE,line); at->a=sl->a; at->name=(char*)name;
    Expr *call=xnew(EXPR_CALL,line); call->a=at;
    Expr *bounds[3]={sl->b,sl->c,sl->d};
    for(int k=0;k<3;k++) xpush(call,bounds[k]?bounds[k]:xnew(EXPR_NONE,line));
    return call;
}
/* a method of obj's class implementing an operator: m(self, arg) -> its result type */
/* the parameters of an operator method after its operand: all with constant defaults (put in at the call) */
static int op_defaults_ok(Ck *c, AFunc *m, int need){
    for(int i=need;i<m->nparams;i++) if(!m->defaults[i] || i==m->star || i==m->dstar || !const_default(m->defaults[i])) return 0;
    for(int i=need;i<m->nparams;i++){ AModule *save=c->mod; c->mod=m->mod; Ty *t=ck_expr_want(c,m->defaults[i],m->params[i]->ty); c->mod=save;
        expect(c,m->params[i]->ty,t,m->line,"a default"); }
    return 1;
}
AFunc *op_method_of(Ck *c, Ty *obj, const char *name, Ty *arg, int line){
    obj=ty_find(obj);
    if(obj->k!=TY_OBJ || !name) return NULL;
    AFunc *m=aot_find_method(obj->cls,name);
    if(!m || m->is_static) return NULL;
    if(m->nparams<(arg?2:1) || (m->nparams>(arg?2:1) && !op_defaults_ok(c,m,arg?2:1)))     /* (x + y with __add__(self, other, context=None)) */
        ck_err(c,line,"%s.%s takes %d argument%s",obj->cls->name,name,arg?1:0,arg?"":"s");
    if(arg && m->pristine && !method_overridden(c,m)){                     /* a generic one (x[i] and x["name"]): its copy for arg */
        Ty *at[16]={0}; Ty *a=ty_find(arg); unsigned omit=0;
        if(!ty_known(a)) return m;                                          /* (decided in a later pass) */
        for(int i=2;i<m->nparams && i<16;i++) if(m->defaults[i] && m->defaults[i]->kind==EXPR_NONE) omit|=1u<<i;   /* (its defaults: None) */
        at[1]=a; m=instance_for_none(c,m,at,NULL,omit,0,0,line); }
    if(arg){ char what[96]; snprintf(what,sizeof what,"the operand of %s",name); expect(c,m->params[1]->ty,arg,line,what); }
    return m;
}
Ty *binop_type(Ck *c, TokKind op, Ty *a, Ty *b, int line){ return binop_type_m(c,op,a,b,line,NULL); }
Ty *binop_type_m(Ck *c, TokKind op, Ty *a, Ty *b, int line, AFunc **opfn){
    a=ty_find(a); b=ty_find(b);
    if(a->k==TY_VOID||b->k==TY_VOID) no_void(c,TY_VOID_T,line);
    if(a->k==TY_VAR||b->k==TY_VAR) return pending(c,line,"an operand");
    if(opfn && a->k==TY_OBJ && op_method(op)){
        const char *nm=op_method(op);
        if(is_aug_op(op)){ char inm[40]; snprintf(inm,sizeof inm,"__i%s",nm+2);           /* x += y: __iadd__ first */
            AFunc *m=op_method_of(c,a,xstrdup2(inm),b,line); if(m){ *opfn=m; return m->ret; } }
        AFunc *m=op_method_of(c,a,nm,b,line); if(m){ *opfn=m; return m->ret; } }
    if(opfn && b->k==TY_OBJ && op_method(op) && !is_aug_op(op)){                         /* 2 * v: v.__rmul__(2) */
        char rnm[40]; snprintf(rnm,sizeof rnm,"__r%s",op_method(op)+2);
        AFunc *m=op_method_of(c,b,xstrdup2(rnm),a,line); if(m){ *opfn=m; c->reflected=1; return m->ret; } }
    int fl=(a->k==TY_FLOAT||b->k==TY_FLOAT);
    switch(op){
        case T_PLUS: case T_PLUS_ASSIGN:
            if(numeric(a)&&numeric(b)) return fl?TY_FLOAT_T:TY_INT_T;
            if(a->k==TY_STR&&b->k==TY_STR) return TY_STR_T;
            if(a->k==TY_BYTES&&b->k==TY_BYTES) return TY_BYTES_T;
            if(a->k==TY_LIST&&b->k==TY_LIST){ if(!unify(c,a->elem,b->elem)) break; return a; }
            break;
        case T_MINUS: case T_MINUS_ASSIGN: case T_FLOOR_DIV: case T_FLOOR_DIV_ASSIGN:
            if(numeric(a)&&numeric(b)) return fl?TY_FLOAT_T:TY_INT_T;
            if(a->k==TY_SET&&b->k==TY_SET&&(op==T_MINUS||op==T_MINUS_ASSIGN)){ if(!unify(c,a->elem,b->elem)) break; return a; }
            break;
        case T_STAR: case T_STAR_ASSIGN:
            if(numeric(a)&&numeric(b)) return fl?TY_FLOAT_T:TY_INT_T;
            if(a->k==TY_STR&&(b->k==TY_INT||b->k==TY_BOOL)) return TY_STR_T;
            if((a->k==TY_INT||a->k==TY_BOOL)&&b->k==TY_STR) return TY_STR_T;
            if(a->k==TY_BYTES&&(b->k==TY_INT||b->k==TY_BOOL)) return TY_BYTES_T;
            if((a->k==TY_INT||a->k==TY_BOOL)&&b->k==TY_BYTES) return TY_BYTES_T;
            if(a->k==TY_LIST&&(b->k==TY_INT||b->k==TY_BOOL)) return a;
            break;
        case T_SLASH: case T_SLASH_ASSIGN:
            if(numeric(a)&&numeric(b)) return TY_FLOAT_T;
            break;
        case T_PERCENT: case T_PERCENT_ASSIGN:
            if(numeric(a)&&numeric(b)) return fl?TY_FLOAT_T:TY_INT_T;
            if(a->k==TY_STR) return TY_STR_T;           /* formatting; the arguments are checked by the caller */
            break;
        case T_POWER: case T_POWER_ASSIGN:
            if(numeric(a)&&numeric(b)) return fl?TY_FLOAT_T:TY_INT_T;
            break;
        case T_AMP: case T_PIPE: case T_CARET: case T_AMP_ASSIGN: case T_PIPE_ASSIGN: case T_CARET_ASSIGN:
            if(a->k==TY_BOOL&&b->k==TY_BOOL) return TY_BOOL_T;
            if((a->k==TY_INT||a->k==TY_BOOL)&&(b->k==TY_INT||b->k==TY_BOOL)) return TY_INT_T;
            if(a->k==TY_SET&&b->k==TY_SET){ if(!unify(c,a->elem,b->elem)) break; return a; }
            break;
        case T_SHL: case T_SHR: case T_SHL_ASSIGN: case T_SHR_ASSIGN:
            if((a->k==TY_INT||a->k==TY_BOOL)&&(b->k==TY_INT||b->k==TY_BOOL)) return TY_INT_T;
            break;
        default: break;
    }
    ck_err(c,line,"unsupported operand types %s and %s",ty_name(a),ty_name(b));
}

int is_none(Expr *e){ return e->kind==EXPR_NONE; }
int aot_tuple_prefix(Ty *x, Ty *y);
#define tuple_prefix aot_tuple_prefix
void ck_compare_pair_m(Ck *c, int code, Expr *ea, Expr *eb, Ty *a, Ty *b, int line, AFunc **opfn, int *neg, int *swap){
    if(opfn && !is_none(ea) && !is_none(eb)){            /* __eq__ __ne__ __lt__ __le__ __gt__ __ge__ __contains__ */
        static const char *names[]={"__lt__","__le__","__gt__","__ge__","__eq__","__ne__"};
        AFunc *m=NULL;
        if(code<=CMP_NE) m=op_method_of(c,a,names[code],b,line);
        if(!m && code==CMP_NE){ m=op_method_of(c,a,"__eq__",b,line); if(m) *neg=1; }
        if(!m && (code==CMP_IN||code==CMP_NOTIN)) m=op_method_of(c,b,"__contains__",a,line);
        if(m){ *opfn=m; return; }
        if(code<=CMP_NE && ty_find(b)->k==TY_OBJ && ty_find(a)->k!=TY_OBJ && ty_find(a)->k!=TY_VAR){   /* 1 < obj: obj.__gt__(1) */
            static const char *const refl[]={"__gt__","__ge__","__lt__","__le__","__eq__","__ne__"};
            m=op_method_of(c,b,refl[code],a,line);
            if(!m && code==CMP_NE){ m=op_method_of(c,b,"__eq__",a,line); if(m) *neg=1; }
            if(m){ *opfn=m; *swap=1; return; } }
    }
    if(code==CMP_IS||code==CMP_ISNOT){
        if(is_none(ea)||is_none(eb)) return;                        /* x is None: whether x holds None */
        a=ty_find(a); b=ty_find(b);
        if((a->k==TY_VAR && a->opt) || (b->k==TY_VAR && b->opt)) return;   /* (a value only ever None) */
        if(a->k==TY_VAR||b->k==TY_VAR){ pending(c,line,"an operand of 'is'"); return; }
        if(a->k==TY_OBJ&&b->k==TY_OBJ&&(aot_subclass(a->cls,b->cls)||aot_subclass(b->cls,a->cls))) return;
        if(ty_is_ptr(a) && unify(c,a,b)) return;                    /* the same object */
        if(a->k==TY_TYPE && b->k==TY_TYPE) return;                  /* the same class */
        ck_err(c,line,"'is' compares objects (or with None); got %s and %s",ty_name(a),ty_name(b));
    }
    if(code==CMP_IN||code==CMP_NOTIN){
        Ty *h=ty_find(b);
        if(h->k==TY_VAR){ pending(c,line,"the right operand of 'in'"); return; }
        if(h->k==TY_STR){ expect(c,TY_STR_T,a,line,"the left operand of 'in'"); return; }
        if(h->k==TY_BYTES){ Ty *n=ty_find(a); if(n->k==TY_VAR){ pending(c,line,"the left operand of 'in'"); return; }
            if(n->k!=TY_BYTES&&n->k!=TY_INT&&n->k!=TY_BOOL) ck_err(c,line,"a bytes-like object is required, not '%s'",ty_name(n)); return; }
        if(h->k==TY_DICT){ expect(c,ty_dkey(h),a,line,"the left operand of 'in'"); return; }
        if(h->k==TY_LIST||h->k==TY_SET){ expect(c,h->elem,a,line,"the left operand of 'in'"); return; }
        ck_err(c,line,"'in' needs a str, bytes, list, set or dict on the right, not %s",ty_name(h));
    }
    if(is_none(ea)||is_none(eb)) return;
    Ty *x=ty_find(a), *y=ty_find(b);
    if(x->k==TY_VAR||y->k==TY_VAR){ if(!unify(c,x,y)) pending(c,line,"a comparison operand"); return; }
    if(numeric(x)&&numeric(y)) return;
    if(!c->strict && x->k==y->k && (!ty_known(x) || !ty_known(y))) return;   /* (tuples of types not known yet: a later pass) */
    if(code==CMP_EQ||code==CMP_NE){
        if(x->k==TY_OBJ&&y->k==TY_OBJ&&(aot_subclass(x->cls,y->cls)||aot_subclass(y->cls,x->cls))) return;
        if(x->k==TY_STR&&y->k==TY_STR) return;
        if(x->k==TY_BYTES&&y->k==TY_BYTES) return;
        if(x->k==TY_TYPE&&y->k==TY_TYPE) return;
        if(ty_same(x,y) && (x->k==TY_LIST||x->k==TY_SET||x->k==TY_TUPLE||x->k==TY_DICT)) return;
        if(tuple_prefix(x,y)) return;                                  /* tuples of different lengths: unequal */
        if((x->k==TY_FUNC||x->k==TY_GEN) && unify(c,x,y)) return;      /* identity */
        ck_err(c,line,"cannot compare %s with %s",ty_name(x),ty_name(y));
    }
    if(x->k==TY_TUPLE && !x->names && (ty_same(x,y) || tuple_prefix(x,y))) return;
    if(x->k==TY_STR&&y->k==TY_STR) return;
    if(x->k==TY_BYTES&&y->k==TY_BYTES) return;
    if((x->k==TY_LIST||x->k==TY_SET) && x->k==y->k && unify(c,x->elem,y->elem)) return;     /* lexicographic / subsets */
    ck_err(c,line,"cannot order %s and %s",ty_name(x),ty_name(y));
}

/* (1, 'a') < (1, 'a', 2.0): tuples of different lengths, the shorter one's item types first in the longer */
int aot_tuple_prefix(Ty *x, Ty *y){
    x=ty_find(x); y=ty_find(y);
    if(x->k!=TY_TUPLE || y->k!=TY_TUPLE || x->names || y->names || x->nelems==y->nelems) return 0;
    int n=x->nelems<y->nelems?x->nelems:y->nelems;
    for(int i=0;i<n;i++) if(!ty_same(x->elems[i],y->elems[i])) return 0;
    return 1;
}
