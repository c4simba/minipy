/* ========================= Typed compiler, types: assignments =========================
   Assignment targets and their types. */

#include "check.h"

/* Store a value of type vt into target t (name, attribute, item, or unpacking). */
void ck_store(Ck *c, Expr *t, Ty *vt, int line){
    XInfo *xi=xinfo(t);
    no_void(c,vt,line);
    switch(t->kind){
        case EXPR_NAME:{
            if(xi->own_var){ xi->kind=X_VAR; xi->ty=xi->var->ty; xi->var->live=1; char what[160]; snprintf(what,sizeof what,"variable '%s'",t->name); expect(c,xi->var->ty,vt,line,what); return; }
            ASym *s=lookup(c,t->name);
            if(!s || s->kind!=AS_VAR) ck_err(c,line,"cannot assign to '%s'",t->name);
            if(!strcmp(t->name,"_") && xi->kind!=X_VAR){             /* _ takes throwaway values: one of another type is dropped */
                Ty *have=ty_find(((AVar*)s->p)->ty), *got=ty_find(vt);
                if(xi->kind==X_NONE && have->k!=TY_VAR && got->k!=TY_VAR && !ty_same(have,got) && !(numeric(have)&&numeric(got))){ xi->kind=X_NONE; xi->var=NULL; xi->name="_discard"; return; }
            }
            if(xi->name && !strcmp(xi->name,"_discard")) return;
            xi->kind=X_VAR; xi->var=(AVar*)s->p; xi->ty=xi->var->ty; xi->var->live=1;
            if(c->nnn) nn_drop(c,xi->var);                          /* (assigned: maybe None again) */
            if(xi->var->src) var_root(xi->var)->nonlocal_set=1;     /* nonlocal x; x = ...: shared through a cell */
            char what[160]; snprintf(what,sizeof what,"variable '%s'",t->name);
            expect(c,xi->var->ty,vt,line,what);
            return; }
        case EXPR_ATTRIBUTE:{
            AModule *m=module_expr(c,t->a);
            if(m){ ASym *s=module_sym(m,t->name); if(!s||s->kind!=AS_VAR) ck_err(c,line,"cannot assign %s.%s",m->name,t->name);
                xi->kind=X_VAR; xi->var=(AVar*)s->p; xi->ty=xi->var->ty; expect(c,xi->var->ty,vt,line,"the module variable"); return; }
            if(class_expr(c,t->a)){ ASym csx={0}, *cs=&csx; csx.kind=AS_CLASS; csx.p=class_expr(c,t->a);   /* Class.name = value: its class-level value */
                if(cs && cs->kind==AS_CLASS){ AClass *cls=(AClass*)cs->p; AField *fd=aot_find_field(cls,t->name);
                    if(!fd){ if(aot_find_method(cls,t->name)) ck_err(c,line,"cannot replace method %s.%s in compiled code",cls->name,t->name);
                        fd=add_field(cls,t->name,NULL,NULL);                     /* a new class attribute */
                        char h[600]; snprintf(h,sizeof h," %s.%s",cls->symname,fd->name); fd->cvar=new_global(c,cls->mod,h); fd->cvar->ty=fd->ty; symtab_add(&cls->mod->syms,h,AS_VAR,fd->cvar); }
                    if(!fd->cvar) ck_err(c,line,"%s.%s is an instance attribute; assign it through an object",cls->name,t->name);
                    int own=0; for(int i=0;i<cls->nfields;i++) if(cls->fields[i]==fd) own=1;
                    if(!own) ck_err(c,line,"%s.%s is defined by a base class: assigning it through %s is not supported in compiled code",cls->name,t->name,cls->name);
                    xi->kind=X_VAR; xi->var=fd->cvar; xi->ty=fd->ty;
                    char what[160]; snprintf(what,sizeof what,"class attribute %s.%s",cls->name,t->name);
                    expect(c,fd->ty,vt,line,what); return; } }
            Ty *o=ty_find(ck_expr(c,t->a));
            if(o->k==TY_VAR){ pending(c,line,"the object"); return; }
            if(o->k!=TY_OBJ) ck_err(c,line,"cannot set attribute '%s' of %s",t->name,ty_name(o));
            AField *fd=aot_find_field(o->cls,t->name);
            if(!fd){
                if(aot_find_method(o->cls,t->name)) ck_err(c,line,"'%s' is a method of %s",t->name,o->cls->name);
                AClass *owner=o->cls;                                    /* (a base class's methods setting self.name: */
                for(AClass *k=o->cls->base;k;k=k->base) if(class_assigns_self(k,t->name)) owner=k;   /* its field, checked or not yet) */
                fd=add_field(owner,t->name,NULL,NULL);
            }
            xi->kind=X_FIELD; xi->field=fd; xi->cls=o->cls; xi->ty=fd->ty; aot_field_root(fd)->inst_set=1;
            if(o->cls->dc_frozen && t->akind!=7){                   /* a frozen dataclass: FrozenInstanceError (but in its __init__) */
                AFunc *cf=c->fn; int init= cf && cf->cls && cf->cls->dc && cf->def==cf->cls->dc->init_def && aot_subclass(o->cls,cf->cls);
                if(!init){ xi->name="frozen"; xi->fn=NULL; } }
            char what[160]; snprintf(what,sizeof what,"field %s.%s",o->cls->name,t->name);
            expect(c,fd->ty,vt,line,what);
            return; }
        case EXPR_SLICE:{                                       /* xs[a:b] = items */
            Ty *o=ty_find(ck_expr(c,t->a)); Expr *bounds[3]={t->b,t->c,t->d};
            for(int k=0;k<3;k++) if(bounds[k] && bounds[k]->kind!=EXPR_NONE) expect(c,TY_INT_T,ck_expr(c,bounds[k]),line,"a slice bound");
            if(o->k==TY_VAR){ pending(c,line,"the list"); return; }
            if(o->k!=TY_LIST || o->tup) ck_err(c,line,"'%s' object does not support item assignment",ty_name(o));
            Ty *v=ty_find(vt); if(v->k==TY_VAR){ pending(c,line,"the assigned value"); return; }
            Ty *el= v->k==TY_STR ? TY_STR_T : (v->k==TY_LIST||v->k==TY_SET||v->k==TY_GEN) ? v->elem : NULL;
            if(!el) ck_err(c,line,"can only assign an iterable to a slice in compiled code (a list, a set, a str or a generator), not %s",ty_name(v));
            expect(c,o->elem,el,line,"an item of the slice");
            xi->ty=o; return; }
        case EXPR_INDEX:{
            Ty *o=ty_find(ck_expr(c,t->a)), *i=ck_expr(c,t->b);
            if(o->k==TY_VAR){ pending(c,line,"the indexed value"); return; }
            if(o->k==TY_OBJ){
                AFunc *m=aot_find_method(o->cls,"__setitem__");
                if(!m || m->nparams!=3) ck_err(c,line,"%s has no __setitem__(self, key, value)",o->cls->name);
                expect(c,m->params[1]->ty,i,line,"the key of __setitem__"); expect(c,m->params[2]->ty,vt,line,"the value of __setitem__");
                xi->kind=X_OPMETHOD; xi->fn=m; xi->ty=m->params[2]->ty;
                AFunc *g=aot_find_method(o->cls,"__getitem__"); xi->field=NULL; xi->var=NULL; (void)g;
                return; }
            if(o->k==TY_LIST){ expect(c,TY_INT_T,i,line,"a list index"); expect(c,o->elem,vt,line,"a list element"); xi->ty=o->elem; return; }
            if(o->k==TY_DICT){ expect(c,ty_dkey(o),i,line,"a dictionary key"); expect(c,o->elem,vt,line,"a dictionary value"); xi->ty=o->elem; return; }
            if(o->k==TY_STR) ck_err(c,line,"strings are immutable");
            ck_err(c,line,"%s does not support item assignment",ty_name(o)); }
        case EXPR_TUPLE: case EXPR_LIST:{
            Ty *v=ty_find(vt);
            if(v->k==TY_VAR){ pending(c,line,"the unpacked value"); return; }
            int star=-1; for(int i=0;i<t->count;i++) if(t->items[i]->akind==1) star=i;
            if(star>=0){                                        /* a, *b, c = ...: b gets a list of the rest */
                xi->ty=vt;
                if(v->k==TY_TUPLE){
                    if(v->names) ck_err(c,line,"%s (a dict literal with values of different types) cannot be unpacked in compiled code",ty_name(v));
                    if(v->nelems<t->count-1) ck_err(c,line,"ValueError: not enough values to unpack (expected at least %d, got %d)",t->count-1,v->nelems);
                    int rest=v->nelems-(t->count-1);
                    for(int i=0;i<star;i++) ck_store(c,t->items[i],v->elems[i],line);
                    for(int i=star+1;i<t->count;i++) ck_store(c,t->items[i],v->elems[v->nelems-(t->count-i)],line);
                    Ty *el=rest?v->elems[star]:ty_var();
                    for(int i=star+1;i<star+rest;i++) if(!unify(c,el,v->elems[i])) ck_err(c,line,"the starred target would hold values of different types (%s)",ty_name(v));
                    ck_store(c,t->items[star],ty_new(TY_LIST,el,NULL),line);
                    return; }
                Ty *el= v->k==TY_STR ? TY_STR_T : v->k==TY_LIST ? v->elem : NULL;
                if(!el) ck_err(c,line,"only a list, a tuple or a str can be unpacked, not %s",ty_name(v));
                for(int i=0;i<t->count;i++) ck_store(c,t->items[i],i==star?ty_new(TY_LIST,el,NULL):el,line);
                return; }
            if(v->k==TY_TUPLE){
                if(v->names) ck_err(c,line,"%s (a dict literal with values of different types) cannot be unpacked in compiled code",ty_name(v));
                if(v->nelems!=t->count) ck_err(c,line,"cannot unpack %s into %d targets",ty_name(v),t->count);
                xi->ty=vt;
                for(int i=0;i<t->count;i++) ck_store(c,t->items[i],v->elems[i],line);
                return;
            }
            if(v->k!=TY_LIST && v->k!=TY_STR) ck_err(c,line,"only a list, a tuple or a str can be unpacked, not %s",ty_name(v));
            xi->ty=vt;
            for(int i=0;i<t->count;i++) ck_store(c,t->items[i],v->k==TY_STR?TY_STR_T:v->elem,line);
            return; }
        default: ck_err(c,line,"cannot assign to this expression");
    }
}
/* Current type of an assignment target (for augmented assignment). */
static Ty *ck_target_value(Ck *c, Expr *t, int line){
    if(t->kind==EXPR_NAME||t->kind==EXPR_ATTRIBUTE||t->kind==EXPR_INDEX) return ck_expr(c,t);
    ck_err(c,line,"invalid target of augmented assignment");
}

void ck_assign(Ck *c, Stmt *s){
    AotUnit *u=c->mod->unit;
    AAssign *a=aot_assign(u,s);
    if(a->ntarget==1 && a->target[0]->kind==EXPR_ATTRIBUTE && a->value && !a->ann){   /* obj.x = v, x a property with a setter */
        Expr *t=a->target[0]; Ty *ot=ty_find(ck_expr(c,t->a));
        char nm[300]; snprintf(nm,sizeof nm,"__mpy_set_%s",t->name);
        if(ot->k==TY_OBJ && !aot_find_field(ot->cls,t->name) && aot_find_method(ot->cls,nm)){
            int line=s->line; Expr *v=a->value;
            if(a->aug){
                static const struct { int aug, op; } ops[]={{T_PLUS_ASSIGN,T_PLUS},{T_MINUS_ASSIGN,T_MINUS},{T_STAR_ASSIGN,T_STAR},{T_SLASH_ASSIGN,T_SLASH},
                    {T_FLOOR_DIV_ASSIGN,T_FLOOR_DIV},{T_PERCENT_ASSIGN,T_PERCENT},{T_POWER_ASSIGN,T_POWER},{T_AMP_ASSIGN,T_AMP},{T_PIPE_ASSIGN,T_PIPE},
                    {T_CARET_ASSIGN,T_CARET},{T_SHL_ASSIGN,T_SHL},{T_SHR_ASSIGN,T_SHR},{0,0}};
                int op=a->aug; for(int i=0;ops[i].aug;i++) if(ops[i].aug==a->aug) op=ops[i].op;
                if(!pure_test(t->a)) ck_err(c,line,"obj.%s %s= v with a property: obj must be a name in compiled code",t->name,"");
                Expr *get=xnew(EXPR_ATTRIBUTE,line); get->a=t->a; get->name=t->name;
                Expr *bin=xnew(EXPR_BINARY,line); bin->op=op; bin->a=get; bin->b=v; v=bin; }
            Expr *m=xnew(EXPR_ATTRIBUTE,line); m->a=t->a; m->name=xstrdup2(nm);
            Expr *call=xnew(EXPR_CALL,line); call->a=m; xpush(call,v);
            Stmt *es=stmt_new(STMT_EXPR,NULL,line); es->expr=call;
            Stmt **b=MPY_NEW_ARR(Stmt*,1); b[0]=es;
            s->kind=STMT_IF; s->expr=xnew(EXPR_TRUE,line); s->body=b; s->body_count=s->body_cap=1; s->orelse_count=0; s->aux=NULL; s->aug=0; s->targets=NULL; s->ntargets=0; s->value=NULL;
            ck_stmt(c,s); return; } }
    if(!a->aug && !a->ann && a->ntarget==1 && a->target[0]->kind==EXPR_SLICE && a->value){   /* obj[a:b] = v: obj.__mpy_setslice__(a, b, None, v) */
        Ty *ot=ty_find(ck_expr(c,a->target[0]->a));
        if(ot->k==TY_OBJ && aot_find_method(ot->cls,"__mpy_setslice__")){
            Expr *call=slice_call(a->target[0],"__mpy_setslice__",s->line); xpush(call,a->value);
            Stmt *es=stmt_new(STMT_EXPR,NULL,s->line); es->expr=call;
            Stmt **b=MPY_NEW_ARR(Stmt*,1); b[0]=es;
            s->kind=STMT_IF; s->expr=xnew(EXPR_TRUE,s->line); s->body=b; s->body_count=s->body_cap=1; s->orelse_count=0; s->aux=NULL; s->aug=0; s->targets=NULL; s->ntargets=0; s->value=NULL;
            ck_stmt(c,s); return; } }
    if(a->aug && a->ntarget==1 && a->target[0]->kind==EXPR_INDEX && a->value){   /* obj[k] += v on an object: its __getitem__ / __setitem__ */
        Ty *ct=ty_find(ck_expr(c,a->target[0]->a));
        if(ct->k==TY_OBJ){ desugar_aug_item(c,s,a); ck_stmt(c,s); return; } }
    if(a->ann && !a->annot) a->annot=type_of(c,a->ann,s->line);
    if(a->annot){
        if(a->ntarget!=1||(a->target[0]->kind!=EXPR_NAME&&a->target[0]->kind!=EXPR_ATTRIBUTE)) ck_err(c,s->line,"only a name or an attribute can be annotated");
        if(ty_find(a->annot)->k==TY_VOID) ck_err(c,s->line,"a variable cannot be annotated as None");
        ck_store(c,a->target[0],a->annot,s->line);
    }
    if(!a->value) return;
    if(!a->aug && a->ntarget==1) note_re_groups(c,a->target[0],a->value);
    if(a->aug){
        Expr *t=a->target[0];
        Ty *cur=ck_target_value(c,t,s->line), *v=ck_expr(c,a->value);
        no_void(c,v,s->line);
        if(a->aug==T_PERCENT_ASSIGN && ty_find(cur)->k==TY_STR) ck_err(c,s->line,"%%= formatting is not supported in compiled code");
        Ty *r=binop_type_m(c,a->aug,cur,v,s->line,&a->opfn);
        if(numeric(cur)&&numeric(r)&&ty_find(cur)->k!=ty_find(r)->k&&!(ty_find(cur)->k==TY_INT&&ty_find(r)->k==TY_INT))
            ck_err(c,s->line,"type mismatch: the target is %s, the result is %s",ty_name(cur),ty_name(r));
        else if(!ty_same(cur,r) && !unify(c,cur,r)) ck_err(c,s->line,"type mismatch: the target is %s, the result is %s",ty_name(cur),ty_name(r));
        if(t->kind==EXPR_INDEX||t->kind==EXPR_ATTRIBUTE||t->kind==EXPR_NAME) ck_store(c,t,cur,s->line);
        return;
    }
    int parallel=a->value->kind==EXPR_TUPLE;
    for(int k=0;k<a->ntarget && parallel;k++){ Expr *t=a->target[k]; if((t->kind!=EXPR_TUPLE&&t->kind!=EXPR_LIST)||t->count!=a->value->count) parallel=0; }
    if(parallel){                                         /* a, b = x, y: no tuple is built */
        for(int i=0;i<a->value->count;i++){
            Ty *v=ck_expr(c,a->value->items[i]);
            for(int k=0;k<a->ntarget;k++) ck_store(c,a->target[k]->items[i],v,s->line);
        }
        xinfo(a->value)->ty=TY_VOID_T;
        return;
    }
    if(a->target[0]->kind==EXPR_TUPLE || a->target[0]->kind==EXPR_LIST){        /* a, b = obj (an object with __iter__): list(obj) */
        Ty *ot=ty_find(ck_expr(c,a->value));
        if(ot->k==TY_OBJ && aot_find_method(ot->cls,"__iter__")){
            Expr *call=xnew(EXPR_CALL,s->line); call->a=name_expr(xstrdup2("\001list"),s->line); xpush(call,a->value); a->value=call; } }
    Ty *want=a->annot;
    if(!want && a->ntarget==1 && a->target[0]->kind==EXPR_NAME){ ASym *t=lookup(c,a->target[0]->name); if(t && t->kind==AS_VAR) want=((AVar*)t->p)->ty; }
    Ty *v=want ? ck_expr_want(c,a->value,want) : ck_expr(c,a->value);
    for(int k=0;k<a->ntarget;k++) ck_store(c,a->target[k],v,s->line);
}

