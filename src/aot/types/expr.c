/* ========================= Typed compiler, types: expressions =========================
   The type of every expression (ck_expr). */

#include "check.h"

/* the number of arguments of a built-in type's method taken as a value (xs.append: 1); -1: not one of those */
static int builtin_method_arity(Ty *t, const char *m){
    static const char *const lst[]={"append:1","extend:1","remove:1","insert:2","clear:0","reverse:0","copy:0","count:1","index:1","pop:0",NULL};
    static const char *const set[]={"add:1","discard:1","remove:1","clear:0","update:1","copy:0","pop:0","union:1","intersection:1",
        "difference:1","symmetric_difference:1","issubset:1","issuperset:1","isdisjoint:1",NULL};
    static const char *const dct[]={"get:1","update:1","clear:0","keys:0","values:0","items:0","copy:0","setdefault:2","pop:1","popitem:0",NULL};
    static const char *const str[]={"upper:0","lower:0","strip:0","lstrip:0","rstrip:0","title:0","capitalize:0","casefold:0","swapcase:0",
        "isdigit:0","isalpha:0","isspace:0","isalnum:0","isupper:0","islower:0","isnumeric:0","isdecimal:0","isidentifier:0","splitlines:0",
        "split:0","encode:0","join:1","startswith:1","endswith:1","count:1","find:1","rfind:1","index:1","rindex:1","replace:2","zfill:1",
        "center:1","ljust:1","rjust:1","partition:1","rpartition:1","format_map:1","removeprefix:1","removesuffix:1","expandtabs:0",NULL};
    static const char *const byt[]={"decode:0","hex:0","join:1","startswith:1","endswith:1","find:1","count:1","replace:2","split:0",
        "strip:0","upper:0","lower:0","isdigit:0","isalpha:0","isspace:0",NULL};
    const char *const *tab= t->k==TY_LIST&&!t->tup ? lst : t->k==TY_SET ? set : t->k==TY_DICT ? dct : t->k==TY_STR ? str : t->k==TY_BYTES ? byt : NULL;
    if(!tab) return -1;
    size_t n=strlen(m);
    for(int i=0;tab[i];i++) if(!strncmp(tab[i],m,n) && tab[i][n]==':') return tab[i][n+1]-'0';
    return -1;
}
Ty *ck_expr_inner(Ck *c, Expr *e){
    XInfo *xi=xinfo(e);
    xi->kind=X_NONE;
    switch(e->kind){
        case EXPR_LITERAL:{ Tok *t=e->tok;
            if(t->kind==T_STRING) return t->i==2 ? TY_BYTES_T : TY_STR_T;
            if(t->is_float) return TY_FLOAT_T;
            return TY_INT_T; }
        case EXPR_TRUE: case EXPR_FALSE: return TY_BOOL_T;
        case EXPR_NONE:{ Ty *v=ty_var(); v->opt=1; return v; }    /* makes the type it meets optional */
        case EXPR_NAME:{
            ASym *s=lookup(c,e->name);
            if(s && s->kind==AS_SYS && !strncmp((const char*)s->p,"attr:",5)){       /* (a name of math / random imported by name: math.name) */
                char hm[200]; const char *q=(const char*)s->p+5, *colon=strchr(q,':');
                snprintf(hm,sizeof hm,"%.*s",(int)(colon-q),q);
                void *keep=e->ty; Expr *at=xnew(EXPR_ATTRIBUTE,e->line); at->a=name_expr(xstrdup2(hm),e->line); at->name=xstrdup2(colon+1);
                *e=*at; e->ty=keep;
                return ck_expr_inner(c,e); }
            if(s && s->kind==AS_SYS && !strncmp((const char*)s->p,"lazy:",5)){       /* (a lazy module attribute imported by name) */
                char hm[300]; const char *q=(const char*)s->p+5, *colon=strchr(q,':');
                snprintf(hm,sizeof hm,"%.*s",(int)(colon-q),q);
                Expr *at=xnew(EXPR_ATTRIBUTE,e->line); at->a=name_expr(xstrdup2(hm),e->line); at->name=xstrdup2(colon+1);
                void *keep=e->ty; Expr *call=xnew(EXPR_CALL,e->line); call->a=at; *e=*call; e->ty=keep;
                return ck_expr_inner(c,e); }
            if(!s && !strcmp(e->name,"__name__")){ xi->kind=X_CONST_STR; xi->name=c->mod->name; return TY_STR_T; }
            if(!s && !strcmp(e->name,"__file__") && c->mod->unit && c->mod->unit->path){ xi->kind=X_CONST_STR; xi->name=c->mod->unit->path; return TY_STR_T; }
            if(!s && is_builtin_type_name(e->name)){ xi->kind=X_TYPEVAL; xi->name=e->name; return TY_TYPE_T; }   /* int, str, ... */
            if(s && s->kind==AS_SYS && !strncmp((const char*)s->p,"string.",7) && string_const((const char*)s->p+7)){ xi->kind=X_CONST_STR; xi->name=string_const((const char*)s->p+7); return TY_STR_T; }
            if(!s && builtin_fn_value(c,e,0)) return ck_expr(c,e);
            if(!s) ck_err(c,e->line,"name '%s' is not defined",e->name);
            if(s->kind==AS_CLASS){ xi->kind=X_TYPEVAL; xi->cls=(AClass*)s->p; xi->cls->value_used=1; return ty_new(TY_TYPE,NULL,xi->cls); }   /* (the class: a call of it constructs one) */
            if(s->kind==AS_FUNC){ AFunc *fn=(AFunc*)s->p; xi->kind=X_FUNCREF; xi->fn=fn; fn->value_used=1; fn->ncalls++; return fn_type(c,fn,e->line); }
            if(s->kind==AS_CLIB||s->kind==AS_CFUNC) ck_err(c,e->line,"'%s' is a C %s: call its functions",e->name,s->kind==AS_CLIB?"library":"function");
            if(s->kind!=AS_VAR) ck_err(c,e->line,"'%s' is a %s, not a value",e->name,s->kind==AS_CLASS?"class":"module");
            xi->kind=X_VAR; xi->var=(AVar*)s->p; xi->var->live=1;
            { AVar *r=var_root(xi->var); if(r->fn_const){ r->fn_const->value_used=1; r->fn_const->ncalls++; } }
            { Ty *vt=ty_find(xi->var->ty);                              /* (known not to be None here: the type without None) */
              if(vt->opt && vt->k!=TY_VAR && c->nnn && nn_has(c,xi->var)){ Ty *nt=ty_copy(vt); nt->opt=0; nt->kwo=vt->kwo; nt->view=vt->view; return nt; } }
            return xi->var->ty; }
        case EXPR_UNARY:{
            if(e->op==T_NOT){ no_void(c,ck_cond(c,e->a),e->line); return TY_BOOL_T; }
            Ty *t=ty_find(ck_expr(c,e->a)); no_void(c,t,e->line);
            if(t->k==TY_VAR) return pending(c,e->line,"an operand");
            if(e->op==T_TILDE && t->k==TY_OBJ){ AFunc *m=op_method_of(c,t,"__invert__",NULL,e->line); if(m){ xi->kind=X_OPMETHOD; xi->fn=m; return m->ret; } }   /* ~obj */
            if(e->op==T_TILDE){ if(t->k!=TY_INT&&t->k!=TY_BOOL) ck_err(c,e->line,"~ needs an int"); return TY_INT_T; }
            if(t->k==TY_OBJ){ AFunc *m=op_method_of(c,t,e->op==T_PLUS?"__pos__":"__neg__",NULL,e->line); if(m){ xi->kind=X_OPMETHOD; xi->fn=m; return m->ret; } }   /* -obj, +obj */
            if(!numeric(t)) ck_err(c,e->line,"unary %s needs a number, not %s",e->op==T_PLUS?"+":"-",ty_name(t));
            return t->k==TY_FLOAT?TY_FLOAT_T:TY_INT_T; }
        case EXPR_BINARY:{
            Ty *a=ck_expr(c,e->a);
            if(e->op==T_PERCENT && e->a->kind==EXPR_LITERAL && e->a->tok->kind==T_STRING && e->a->tok->i==1 && !e->a->count && e->b->kind!=EXPR_TUPLE){
                Ty *ot=ty_find(ck_expr(c,e->b));                          /* f"{obj:spec}": obj.__format__("spec") */
                if(ot->k==TY_OBJ && aot_find_method(ot->cls,"__format__")){
                    const char *pys=e->a->tok->text+strlen(e->a->tok->text)+1; if(*pys=='%') pys++;
                    Expr *at=xnew(EXPR_ATTRIBUTE,e->line); at->a=e->b; at->name="__format__";
                    Expr *call=xnew(EXPR_CALL,e->line); call->a=at;
                    Expr *lit=xnew(EXPR_LITERAL,e->line); lit->tok=MPY_NEW0(Tok); *lit->tok=*e->a->tok; lit->tok->text=xstrdup2(pys); lit->tok->len=strlen(pys); lit->tok->i=0;
                    xpush(call,lit);
                    void *keep=e->ty; *e=*call; e->ty=keep;
                    return ck_expr_inner(c,e); } }
            if(e->op==T_PERCENT && e->a->kind==EXPR_LITERAL && e->a->tok->kind==T_STRING && e->a->tok->i!=2){
                for(int k=0;k<e->a->count;k++) expect(c,TY_INT_T,ck_expr(c,e->a->items[k]),e->line,"a width or precision in a format spec");
                if(e->b->kind==EXPR_TUPLE){ for(int i=0;i<e->b->count;i++) no_void(c,ck_expr(c,e->b->items[i]),e->line); xinfo(e->b)->ty=TY_VOID_T; }
                else no_void(c,ck_expr(c,e->b),e->line);
                return TY_STR_T;
            }
            if(e->op==T_PERCENT && ty_find(a)->k==TY_STR) ck_err(c,e->line,"the format string of %% must be a string literal in compiled code");
            { Ty *b=ck_expr(c,e->b); Ty *ta=ty_find(a);
              if(int_enum_ty(a) && (numeric(ty_find(b)) || (int_enum_ty(b) && !aot_find_method(ta->cls,op_method(e->op)?op_method(e->op):"")))){ a=as_enum_value(c,e->a); ta=ty_find(a); }   /* status + 1: its value */
              if(int_enum_ty(b) && numeric(ta)) b=as_enum_value(c,e->b);
              if(ta->k==TY_OBJ && op_method(e->op)){ AFunc *om=aot_find_method(ta->cls,op_method(e->op));   /* p / q (a path): p / str(q) */
                  if(om && om->nparams==2) fspath_arg(c,e->b,om->params[1]->ty,&b); }
              AFunc *m=NULL; c->reflected=0; Ty *r=binop_type_m(c,e->op,a,b,e->line,&m);
              if(m){ xi->kind=X_OPMETHOD; xi->fn=m; xi->reflected=c->reflected; }
              if(e->op==T_POWER && ty_find(r)->k==TY_INT && e->b->kind==EXPR_UNARY && e->b->op==T_MINUS
                 && e->b->a->kind==EXPR_LITERAL && !e->b->a->tok->is_float && e->b->a->tok->i>0) return TY_FLOAT_T;   /* 2 ** -1: a float, as in Python */
              return r; } }
        case EXPR_BOOL:{
            Ty *a=ty_find(ck_expr(c,e->a)), *b=ty_find(ck_expr(c,e->b));
            no_void(c,a,e->line); no_void(c,b,e->line);
            if(numeric(a)&&numeric(b)&&a->k!=b->k) return (a->k==TY_FLOAT||b->k==TY_FLOAT)?TY_FLOAT_T:TY_INT_T;
            if(!unify(c,a,b)) ck_err(c,e->line,"'%s' operands must have the same type (%s and %s)",e->op==T_AND?"and":"or",ty_name(a),ty_name(b));
            return a; }
        case EXPR_COMPARE:{
            Ty *prev=ck_expr(c,e->items[0]); no_void(c,prev,e->line);
            for(int i=1;i<e->count;i++){
                int code=e->items[i]->akind;
                if(code==CMP_IN||code==CMP_NOTIN) enum_members_expr(c,e->items[i]);           /* m in Color */
                Ty *t=(code==CMP_IN||code==CMP_NOTIN) && e->items[i]->kind==EXPR_TUPLE ? ck_tuple_as_list(c,e->items[i]) : ck_expr(c,e->items[i]);
                if((code==CMP_IN||code==CMP_NOTIN) && ty_find(t)->k==TY_TUPLE && !ty_find(t)->names){   /* x in a tuple's value: x in list(t) */
                    Expr *inner=MPY_NEW0(Expr); *inner=*e->items[i]; inner->akind=0;
                    Expr *call=xnew(EXPR_CALL,e->line); call->a=name_expr("\001list",e->line); xpush(call,inner); call->akind=code;
                    e->items[i]=call; t=ck_expr(c,call); }
                no_void(c,t,e->line);
                if(code<=CMP_NE){                                              /* status == 200 (an IntEnum member): its value */
                    if(int_enum_ty(prev) && numeric(ty_find(t))) prev=as_enum_value(c,e->items[i-1]);
                    else if(int_enum_ty(t) && numeric(ty_find(prev))) t=as_enum_value(c,e->items[i]); }
                else if((code==CMP_IN||code==CMP_NOTIN) && int_enum_ty(prev)){ Ty *h=ty_find(t);
                    Ty *ke= h->k==TY_DICT ? ty_find(ty_dkey(h)) : (h->k==TY_LIST||h->k==TY_SET) ? ty_find(h->elem) : NULL;
                    if(ke && numeric(ke)) prev=as_enum_value(c,e->items[i-1]); }
                if(!xi->cmpfn) xi->cmpfn=MPY_NEW_ARR(AFunc*,e->count);
                int neg=0, swap=0; xi->cmpfn[i]=NULL;
                ck_compare_pair_m(c,e->items[i]->akind,e->items[i-1],e->items[i],prev,t,e->line,&xi->cmpfn[i],&neg,&swap);
                if(neg) xi->cmpneg|=1<<i; else xi->cmpneg&=~(1<<i);
                if(swap) xi->cmpswap|=1<<i; else xi->cmpswap&=~(1<<i);
                prev=t; }
            return TY_BOOL_T; }
        case EXPR_TERNARY:{
            no_void(c,ck_cond(c,e->a),e->line);
            Ty *a=ck_expr(c,e->b), *b=ck_expr_want(c,e->c,a);              /* (Stack[str]() if c else Stack(): one instance) */
            if(numeric(a)&&numeric(b)&&ty_find(a)->k!=ty_find(b)->k) return (ty_find(a)->k==TY_FLOAT||ty_find(b)->k==TY_FLOAT)?TY_FLOAT_T:TY_INT_T;
            { Ty *fa=ty_find(a), *fb=ty_find(b);                         /* (Sub() if c else base: the base class) */
              if(fa->k==TY_OBJ && fb->k==TY_OBJ && fa->cls!=fb->cls && !fa->opt && !fb->opt){
                  if(aot_subclass(fa->cls,fb->cls)) return b;
                  if(aot_subclass(fb->cls,fa->cls)) return a; } }
            if(!unify(c,a,b)) ck_err(c,e->line,"both branches of `x if c else y` must have the same type (%s and %s)",ty_name(a),ty_name(b));
            return a; }
        case EXPR_CALL: return ck_call(c,e);
        case EXPR_ATTRIBUTE:{
            if(is_sys(c,e->a)){
                if(!strcmp(e->name,"_compiled")){ e->kind=EXPR_TRUE; e->a=NULL; xi->kind=X_NONE; return TY_BOOL_T; }   /* (True here) */
                if(!strcmp(e->name,"platform")){ xi->kind=X_CONST_STR; xi->name=c->p->target==AOT_TARGET_KOLIBRI?"kolibrios":c->p->target==AOT_TARGET_MACOS?"darwin":"linux"; return TY_STR_T; }
                if(!strcmp(e->name,"version")){ xi->kind=X_CONST_STR; xi->name="3.14.0 (minipy)"; return TY_STR_T; }   /* (as the interpreter's) */
                if(!strcmp(e->name,"byteorder")){ xi->kind=X_CONST_STR; xi->name="little"; return TY_STR_T; }
                if(!strcmp(e->name,"maxsize") || !strcmp(e->name,"maxunicode")){ Expr *v=lit_int(e->line,e->name[3]=='s'?0x7FFFFFFFFFFFFFFFLL:0x10FFFF); void *keep=e->ty; *e=*v; e->ty=keep; return ck_expr_inner(c,e); }
                if(!strcmp(e->name,"argv")){                       /* the list _sysargs.py makes */
                    if(!lookup(c,"__mpy_argv")) ck_err(c,e->line,"internal error: sys.argv without _sysargs.py");
                    e->kind=EXPR_NAME; e->name="__mpy_argv"; e->a=NULL; return ck_expr_inner(c,e); }
                { static const char *const std[][2]={{"stdout","__mpy_stdout"},{"stderr","__mpy_stderr"},{"stdin","__mpy_stdin"},
                      {"__stdout__","__mpy_stdout"},{"__stderr__","__mpy_stderr"},{"__stdin__","__mpy_stdin"}};
                  for(int k=0;k<6;k++) if(!strcmp(e->name,std[k][0])){                 /* the objects _sysio.py makes */
                      if(!lookup(c,std[k][1])) ck_err(c,e->line,"internal error: sys.%s without _sysio.py",e->name);
                      e->kind=EXPR_NAME; e->name=(char*)std[k][1]; e->a=NULL; return ck_expr_inner(c,e); } }
                ck_err(c,e->line,"sys.%s is not a value",e->name);
            }
            if(is_bmod(c,e->a,"math") && (!strcmp(e->name,"pi")||!strcmp(e->name,"e")||!strcmp(e->name,"tau")||!strcmp(e->name,"inf")||!strcmp(e->name,"nan"))){ xi->kind=X_BMOD; xi->name=e->name; return TY_FLOAT_T; }
            if(is_bmod(c,e->a,"string")){ const char *v=string_const(e->name); if(!v) ck_err(c,e->line,"string.%s is not available in compiled code",e->name);
                xi->kind=X_CONST_STR; xi->name=v; return TY_STR_T; }
            if(bmod(c,e->a)) ck_err(c,e->line,"%s.%s is not a value in compiled code",bmod(c,e->a),e->name);
            if(class_expr(c,e->a) && class_expr(c,e->a)->is_enum && !strcmp(e->name,"__members__")) e->name="_member_map_";   /* Color.__members__ */
            if(class_expr(c,e->a)){                          /* Class.CONST: a field's constant default */
                ASym csx={0}, *cs=&csx; csx.kind=AS_CLASS; csx.p=class_expr(c,e->a);
                if(cs && cs->kind==AS_CLASS){ AClass *in=aot_nested_class(c->p,(AClass*)cs->p,e->name);     /* Outer.Inner */
                    if(in){ xi->kind=X_TYPEVAL; xi->cls=in; in->value_used=1; return ty_new(TY_TYPE,NULL,in); } }
                if(cs && cs->kind==AS_CLASS && !aot_find_field((AClass*)cs->p,e->name) && aot_find_method((AClass*)cs->p,e->name)){   /* Class.method: a function taking the object */
                    AFunc *fn=aot_find_method((AClass*)cs->p,e->name);
                    if(fn->decovar) ck_err(c,e->line,"%s.%s is decorated; use it through an object",((AClass*)cs->p)->name,e->name);
                    xi->kind=X_FUNCREF; xi->fn=fn; fn->value_used=1; return fn_type(c,fn,e->line); }
                if(cs && cs->kind==AS_CLASS && !strcmp(e->name,"__mro__")){          /* C.__mro__: its classes, object last */
                    xi->kind=X_BUILTIN; xi->name="class_mro"; xi->cls=(AClass*)cs->p; Ty *r=ty_new(TY_LIST,ty_copy(TY_TYPE_T),NULL); r->tup=1; return r; }
                if(cs && cs->kind==AS_CLASS && strcmp(e->name,"__name__") && strcmp(e->name,"__qualname__")){ AClass *cls=(AClass*)cs->p; AField *fd=aot_find_field(cls,e->name);
                    if(fd && fd->cvar){ xi->kind=X_VAR; xi->var=fd->cvar; return fd->cvar->ty; }      /* Class.name: the class-level value */
                    if(!fd && !c->strict) return pending(c,e->line,"the class attribute");      /* (Class.name = v in a module not checked yet) */
                    if(!fd || !fd->init) ck_err(c,e->line,"class %s has no constant '%s'",cls->name,e->name);
                    Expr *v=fd->init; if(v->kind==EXPR_UNARY && v->op==T_MINUS) v=v->a;
                    if(v->kind!=EXPR_LITERAL && v->kind!=EXPR_TRUE && v->kind!=EXPR_FALSE) ck_err(c,e->line,"%s.%s: only constant class attributes can be read through the class",cls->name,e->name);
                    xi->kind=X_CLASSCONST; xi->field=fd; return fd->ty; }
            }
            AModule *m=module_expr(c,e->a);
            if(m){
                ASym *s=module_sym(m,e->name);
                if(!s){ char lz[200]; snprintf(lz,sizeof lz,"_lazy_%s",e->name); ASym *l=module_sym(m,lz);   /* time.tzname: time._lazy_tzname() */
                    if(l && l->kind==AS_FUNC){ Expr *at=xnew(EXPR_ATTRIBUTE,e->line); at->a=e->a; at->name=xstrdup2(lz);
                        void *keep=e->ty; Expr *call=xnew(EXPR_CALL,e->line); call->a=at; *e=*call; e->ty=keep;
                        return ck_expr_inner(c,e); } }
                if(!s){ char sub[300]; snprintf(sub,sizeof sub,"%s.%s",m->name,e->name); if(find_module(c->p,sub)) ck_err(c,e->line,"module %s is not a value",sub); ck_err(c,e->line,"module %s has no attribute '%s'",m->name,e->name); }
                if(s->kind==AS_FUNC){ AFunc *fn=(AFunc*)s->p; xi->kind=X_FUNCREF; xi->fn=fn; fn->value_used=1; fn->ncalls++; return fn_type(c,fn,e->line); }
                if(s->kind==AS_CLASS){ xi->kind=X_TYPEVAL; xi->cls=(AClass*)s->p; xi->cls->value_used=1; return ty_new(TY_TYPE,NULL,xi->cls); }   /* module.Class as a value */
                if(s->kind!=AS_VAR) ck_err(c,e->line,"%s.%s is not a value",m->name,e->name);
                xi->kind=X_VAR; xi->var=(AVar*)s->p; return xi->var->ty;
            }
            Ty *t=ty_find(ck_expr(c,e->a));
            if(t->k==TY_VAR) return pending(c,e->line,"the object");
            if(!strcmp(e->name,"__class__")){ xi->kind=X_BUILTIN; xi->name="type"; return TY_TYPE_T; }      /* x.__class__: type(x) */
            if(t->k==TY_OBJ && aot_is_exception(t->cls) && !strcmp(e->name,"args") && !aot_find_field(t->cls,"args")){   /* e.args: (message,) or () */
                xi->kind=X_BUILTIN; xi->name="exc_args"; Ty *r=ty_new(TY_LIST,TY_STR_T,NULL); r->tup=1; return r; }
            if(t->k==TY_TYPE && (!strcmp(e->name,"__name__")||!strcmp(e->name,"__qualname__"))){ xi->kind=X_BUILTIN; xi->name=e->name[2]=='q'?"type_qualname":"type_name"; return TY_STR_T; }
            if(t->k==TY_TYPE && !strcmp(e->name,"__module__")){ xi->kind=X_BUILTIN; xi->name="type_module"; return TY_STR_T; }
            if(t->k==TY_FUNC && !strcmp(e->name,"__name__")){       /* fn.__name__: kept in the function value */
                static AField name_field; name_field.name="__name__"; name_field.ty=TY_STR_T; name_field.offset=12;
                xi->kind=X_FIELD; xi->field=&name_field; xi->cls=NULL; return TY_STR_T; }
            if(t->k!=TY_OBJ && pure_ref(e->a)){                       /* xs.append as a value: lambda v: xs.append(v) */
                int ar=builtin_method_arity(t,e->name);
                if(ar>=0){ char txt[200]; int n=snprintf(txt,sizeof txt,"lambda");
                    for(int i=0;i<ar;i++) n+=snprintf(txt+n,sizeof txt-(size_t)n,"%s __mpy_a%d",i?",":"",i);
                    n+=snprintf(txt+n,sizeof txt-(size_t)n,": __mpy_o.%s(",e->name);
                    for(int i=0;i<ar;i++) n+=snprintf(txt+n,sizeof txt-(size_t)n,"%s__mpy_a%d",i?", ":"",i);
                    snprintf(txt+n,sizeof txt-(size_t)n,")");
                    Expr *lam=py_front_expr(c->mod->unit->path,txt,e->line);
                    if(lam){ lam=expr_subst(lam,"__mpy_o",e->a); void *keep=e->ty; *e=*lam; e->ty=keep; return ck_expr_inner(c,e); } } }
            if(t->k!=TY_OBJ) ck_err(c,e->line,"%s has no attribute '%s'",ty_name(t),e->name);
            AField *fd=aot_find_field(t->cls,e->name);
            if(fd){ xi->kind=X_FIELD; xi->field=fd; xi->cls=t->cls; return fd->ty; }
            { AFunc *m=aot_find_method(t->cls,e->name);
              if(m && m->is_property){ xi->kind=X_PROP; xi->fn=m; return m->ret; }
              if(m){                                               /* obj.method as a value: bound to obj */
                  if(m->is_static){ xi->kind=X_FUNCREF; xi->fn=m; m->value_used=1; return fn_type(c,m,e->line); }
                  if(m->decovar || m->star>=0 || m->dstar>=0 || m->kwonly<m->nparams || m->is_async) ck_err(c,e->line,"method %s.%s cannot be used as a value in compiled code (call it)",t->cls->name,e->name);
                  Ty *ps[16]; for(int i=1;i<m->nparams;i++) ps[i-1]=m->params[i]->ty;
                  xi->kind=X_BOUND; xi->fn=m; xi->cls=t->cls; m->value_used=1;
                  return ty_func(ps,m->nparams-1,m->ret); } }
            if(c->strict) ck_err(c,e->line,"%s has no attribute '%s'",t->cls->name,e->name);
            return ty_var(); }
        case EXPR_INDEX:{
            if(e->a->kind==EXPR_NAME || e->a->kind==EXPR_ATTRIBUTE){ AClass *ek=class_expr(c,e->a);   /* Color["RED"]: the member of that name */
                if(ek && ek->is_enum){ Expr *at=xnew(EXPR_ATTRIBUTE,e->line); at->a=e->a; at->name="_by_name_"; Expr *ix=e->b;
                    e->kind=EXPR_CALL; e->a=at; e->b=NULL; e->count=0; e->items=NULL; xpush(e,ix); return ck_expr_inner(c,e); } }
            if(e->a->kind==EXPR_NAME){ ASym *cs=lookup(c,e->a->name);           /* Box[int]: the class (type arguments only constrain types) */
                if((cs && cs->kind==AS_CLASS) || (!cs && is_builtin_type_name(e->a->name))){ Ty *t=ck_expr(c,e->a); *xi=*xinfo(e->a); return t; } }
            Ty *t=ty_find(ck_expr(c,e->a)), *i=ck_expr(c,e->b);
            if(t->k==TY_VAR) return pending(c,e->line,"the indexed value");
            if(t->k==TY_OBJ){ AFunc *m=op_method_of(c,t,"__getitem__",i,e->line); if(!m) ck_err(c,e->line,"%s has no __getitem__",t->cls->name); xi->kind=X_OPMETHOD; xi->fn=m; return m->ret; }
            if(t->k==TY_STR){ expect(c,TY_INT_T,i,e->line,"a string index"); return TY_STR_T; }
            if(t->k==TY_BYTES){ expect(c,TY_INT_T,i,e->line,"a bytes index"); return TY_INT_T; }
            if(t->k==TY_LIST){ expect(c,TY_INT_T,i,e->line,"a list index"); return t->elem; }
            if(t->k==TY_DICT){ expect(c,ty_dkey(t),i,e->line,"a dictionary key"); return t->elem; }
            if(t->k==TY_TUPLE && t->names){                 /* record["key"] */
                Expr *k=e->b;
                if(k->kind!=EXPR_LITERAL || k->tok->kind!=T_STRING) ck_err(c,e->line,"%s (a dict literal with values of different types) is indexed by a key literal",ty_name(t));
                for(int ix=0;ix<t->nelems;ix++) if(!strcmp(t->names[ix],k->tok->text)){ xi->argmap[0]=ix; xi->argmap[1]=1; return t->elems[ix]; }
                ck_err(c,e->line,"KeyError: %s has no key '%s'",ty_name(t),k->tok->text);
            }
            if(t->k==TY_TUPLE){
                expect(c,TY_INT_T,i,e->line,"a tuple index");
                Expr *k=e->b; int neg=0;
                if(k->kind==EXPR_UNARY && k->op==T_MINUS && k->a->kind==EXPR_LITERAL){ neg=1; k=k->a; }
                if(k->kind==EXPR_LITERAL && k->tok->kind==T_NUMBER && !k->tok->is_float){
                    int ix=(int)(neg?-k->tok->i:k->tok->i); if(ix<0) ix+=t->nelems;
                    if(ix<0||ix>=t->nelems) ck_err(c,e->line,"tuple index out of range");
                    xi->argmap[0]=ix; xi->argmap[1]=1; return t->elems[ix];
                }
                for(int k2=1;k2<t->nelems;k2++) if(!ty_same(t->elems[k2],t->elems[0])) ck_err(c,e->line,"a %s can only be indexed by a constant",ty_name(t));
                xi->argmap[1]=0; return t->nelems?t->elems[0]:TY_INT_T;
            }
            ck_err(c,e->line,"%s cannot be indexed",ty_name(t)); }
        case EXPR_SLICE:{
            Ty *t=ty_find(ck_expr(c,e->a));
            Expr *bounds[3]={e->b,e->c,e->d};
            for(int k=0;k<3;k++) if(bounds[k]) expect(c,TY_INT_T,ck_expr(c,bounds[k]),e->line,"a slice bound");
            if(t->k==TY_VAR) return pending(c,e->line,"the sliced value");
            if(t->k==TY_TUPLE && !t->names && pure_ref(e->a)){             /* t[:2] of a tuple: (t[0], t[1]) */
                int bv[3]={0,t->nelems,1}, lit=1;
                for(int k=0;k<3;k++) if(bounds[k]){ Expr *x=bounds[k]; int neg=0;
                    if(x->kind==EXPR_UNARY && x->op==T_MINUS && x->a->kind==EXPR_LITERAL){ neg=1; x=x->a; }
                    if(x->kind==EXPR_LITERAL && x->tok->kind==T_NUMBER && !x->tok->is_float) bv[k]=(int)(neg?-x->tok->i:x->tok->i); else lit=0; }
                if(lit && bv[2]>0){
                    for(int k=0;k<2;k++){ if(bv[k]<0) bv[k]+=t->nelems; if(bv[k]<0) bv[k]=0; if(bv[k]>t->nelems) bv[k]=t->nelems; }
                    Expr *tup=xnew(EXPR_TUPLE,e->line);
                    for(int i=bv[0];i<bv[1];i+=bv[2]){ Expr *ix=xnew(EXPR_INDEX,e->line); ix->a=e->a; ix->b=lit_int(e->line,i); xpush(tup,ix); }
                    void *keep=e->ty; *e=*tup; e->ty=keep; return ck_expr_inner(c,e); } }
            if(t->k==TY_OBJ && aot_find_method(t->cls,"__mpy_getslice__")){     /* obj[a:b:c]: obj.__mpy_getslice__(a, b, c) */
                Expr *call=slice_call(e,"__mpy_getslice__",e->line);
                void *keep=e->ty; *e=*call; e->ty=keep; return ck_expr_inner(c,e); }
            if(t->k!=TY_STR&&t->k!=TY_LIST&&t->k!=TY_BYTES) ck_err(c,e->line,"%s cannot be sliced",ty_name(t));
            return t; }
        case EXPR_LIST: case EXPR_SET:{
            if(has_splat(e)){                                   /* [*xs, y]: the elements of xs */
                Ty *el=ty_var();
                for(int i=0;i<e->count;i++){ Expr *x=e->items[i]; Ty *t=ck_expr(c,x); no_void(c,t,e->line);
                    expect(c,el,x->akind==1?elem_of(c,t,e->line,"the unpacked value"):t,e->line,"a list element"); }
                return ty_new(e->kind==EXPR_LIST?TY_LIST:TY_SET,el,NULL); }
            Ty *el=literal_elem(c,e->items,e->count);
            for(int i=0;i<e->count;i++){ Ty *t=ck_expr(c,e->items[i]); no_void(c,t,e->line); expect(c,el,t,e->line,"a list element"); }
            return ty_new(e->kind==EXPR_LIST?TY_LIST:TY_SET,el,NULL); }
        case EXPR_DICT:{
            if(has_splat(e)){                                   /* {**d, k: v}: d's keys and values */
                Ty *k=ty_var(), *v=ty_var();
                for(int i=0;i<e->count;i++){ Expr *x=e->items[i];
                    if(x->akind==2){ Ty *d=ty_find(ck_expr(c,x)); if(d->k==TY_VAR){ pending(c,e->line,"the unpacked dict"); continue; }
                        if(d->k!=TY_DICT) ck_err(c,e->line,"'%s' object is not a mapping",ty_name(d));
                        expect(c,k,ty_dkey(d),e->line,"a dictionary key"); expect(c,v,d->elem,e->line,"a dictionary value"); continue; }
                    Ty *kt=ck_expr(c,x); no_void(c,kt,e->line); expect(c,k,kt,e->line,"a dictionary key");
                    Ty *t=ck_expr(c,e->vals[i]); no_void(c,t,e->line); expect(c,v,t,e->line,"a dictionary value"); }
                return ty_dict(k,v); }
            if(e->count>=2 && e->count<=64){ Ty *vts[64];
                for(int i=0;i<e->count;i++){ vts[i]=ck_expr(c,e->vals[i]); no_void(c,vts[i],e->line); }
                Ty *r=record_of(e,vts);
                if(r){ for(int i=0;i<e->count;i++) ck_expr(c,e->items[i]); return r; }
                if(record_keys(e) && !c->strict)                    /* a record or a dict? not before the values are known */
                    for(int i=0;i<e->count;i++) if(ty_find(vts[i])->k==TY_VAR) return pending(c,e->line,"a dictionary value"); }
            Ty *v=literal_elem(c,e->vals,e->count), *k=ty_var();
            for(int i=0;i<e->count;i++){ Ty *kt=ck_expr(c,e->items[i]); no_void(c,kt,e->line); expect(c,k,kt,e->line,"a dictionary key"); Ty *t=ck_expr(c,e->vals[i]); no_void(c,t,e->line); expect(c,v,t,e->line,"a dictionary value"); }
            return ty_dict(k,v); }
        case EXPR_COMPREHENSION: return e->comp_kind=='G' ? ck_genexp(c,e) : ck_comprehension(c,e);
        case EXPR_TUPLE:{
            Ty *ts[64];
            if(has_splat(e)){                                   /* (*xs, y): a tuple[T, ...] */
                Ty *el=ty_var();
                for(int i=0;i<e->count;i++){ Expr *x=e->items[i]; Ty *t=ck_expr(c,x); no_void(c,t,e->line);
                    expect(c,el,x->akind==1?elem_of(c,t,e->line,"the unpacked value"):t,e->line,"a tuple element"); }
                Ty *r=ty_new(TY_LIST,el,NULL); r->tup=1; return r; }
            if(e->count>64) ck_err(c,e->line,"tuple too long");
            for(int i=0;i<e->count;i++){ ts[i]=ck_expr(c,e->items[i]); no_void(c,ts[i],e->line); }
            return ty_tuple(ts,e->count); }
        case EXPR_LAMBDA:{
            AFunc *lf=xi->fn; if(!lf) lf=xi->fn=new_lambda(c,e);
            lf->value_used=1; lf->ncalls++;
            ck_def_defaults(c,lf,e->line);                      /* evaluated here, once */
            AFunc *save=c->fn; c->fn=lf;
            Ty *r=e->a->kind==EXPR_NONE ? TY_VOID_T : ck_expr(c,e->a);     /* lambda: None returns nothing */
            c->fn=save;
            if(!unify(c,lf->ret,r)) ck_err(c,e->line,"the lambda returns %s, expected %s",ty_name(r),ty_name(lf->ret));
            xi->kind=X_FUNCREF; xi->fn=lf;
            return fn_type(c,lf,e->line); }
        case EXPR_YIELD:{                                       /* x = yield v: what send() gives; x = yield from g: what g returns */
            AFunc *g=c->fn;
            if(!g || !g->is_gen) ck_err(c,e->line,"'yield' outside a function");
            if(e->akind==7){
                Ty *ts[1]; iter_types(c,e->a,1,ts,e->line);
                expect(c,g->yield_ty,ts[0],e->line,"the yielded values");
                Ty *it=ty_find(xinfo(e->a)->ty?xinfo(e->a)->ty:TY_VOID_T);
                if(it->k!=TY_GEN) return TY_VOID_T;
                unify(c,aot_gen_part(it,0),aot_gen_part(g->ret,0));
                return aot_gen_part(it,1); }
            if(e->a){ Ty *t=ck_expr_want(c,e->a,g->yield_ty); no_void(c,t,e->line); expect(c,g->yield_ty,t,e->line,"the yielded value"); }
            return aot_gen_part(g->ret,0); }
        case EXPR_WALRUS:{                                      /* name := value: the name's type, the value stored */
            Ty *t=ck_expr(c,e->a); no_void(c,t,e->line);
            ck_store(c,e->b,t,e->line);
            xi->var=xinfo(e->b)->var; return xi->var->ty; }
        case EXPR_AWAIT:{
            if(!c->fn || !c->fn->is_async) ck_err(c,e->line,"'await' outside an async function");
            Expr *x=e->a;
            xi->kind=X_ASYNC;
            if(x->kind==EXPR_CALL && x->a->kind==EXPR_ATTRIBUTE && is_bmod(c,x->a->a,"asyncio")){
                const char *m=x->a->name; XInfo *xx=xinfo(x);
                for(int i=0;i<x->count;i++) if(x->items[i]->akind) ck_err(c,e->line,"asyncio.%s() takes positional arguments",m);
                if(!strcmp(m,"sleep")){
                    if(x->count!=1) ck_err(c,e->line,"asyncio.sleep() takes one argument (seconds)");
                    Ty *t=ck_expr(c,x->items[0]); if(!numeric(t)&&!is_var(t)) ck_err(c,e->line,"asyncio.sleep() takes a number of seconds");
                    xi->name="sleep"; xx->kind=X_ASYNC; xx->name="sleep"; xx->ty=TY_VOID_T; return TY_VOID_T;
                }
                if(!strcmp(m,"gather")){
                    Ty *r=NULL;
                    for(int i=0;i<x->count;i++){ Ty *t=ck_coro(c,x->items[i]);
                        if(!r) r=t; else if(!unify(c,r,t)) ck_err(c,e->line,"asyncio.gather() needs coroutines of one result type (%s and %s)",ty_name(r),ty_name(t)); }
                    xi->name="gather"; xx->kind=X_ASYNC; xx->name="gather";
                    Ty *res=(!r||ty_find(r)->k==TY_VOID) ? TY_VOID_T : ty_new(TY_LIST,r,NULL);
                    xx->ty=res; return res;
                }
                return ck_expr(c,x);                     /* reports what is wrong */
            }
            if(x->kind==EXPR_CALL && ((x->a->kind==EXPR_NAME && (!strcmp(x->a->name,"anext")||!strcmp(x->a->name,"\001anext")) && !lookup(c,"anext"))
               || (x->a->kind==EXPR_ATTRIBUTE && (!strcmp(x->a->name,"__anext__")||!strcmp(x->a->name,"asend")||!strcmp(x->a->name,"athrow")||!strcmp(x->a->name,"aclose"))
                   && ty_find(ck_expr(c,x->a->a))->k==TY_GEN))){              /* await anext(g), await g.asend(v): the generator's step */
                xi->name="plain"; return ck_expr(c,x); }
            if(x->kind==EXPR_CALL){
                Expr *save=c->coro_ok; c->coro_ok=x;
                Ty *t=ck_expr(c,x);
                c->coro_ok=save;
                XInfo *xx=xinfo(x);
                if((xx->kind==X_FUNC||xx->kind==X_METHOD||xx->kind==X_STATIC||xx->kind==X_SUPER||xx->kind==X_CALLNEST) && xx->fn && xx->fn->is_async){ xi->name="call"; return t; }
                Ty *tt=ty_find(t);
                if(tt->k==TY_TASK){ xi->name="task"; return tt->elem; }
                if(tt->k==TY_VAR) return pending(c,e->line,"the awaited value");
                ck_err(c,e->line,"await needs a coroutine call or a task, not %s",ty_name(t));
            }
            Ty *t=ty_find(ck_expr(c,x));
            if(t->k==TY_TASK){ xi->name="task"; return t->elem; }
            if(t->k==TY_VAR) return pending(c,e->line,"the awaited value");
            ck_err(c,e->line,"await needs a coroutine call or a task, not %s",ty_name(t)); }
        default: ck_err(c,e->line,"unsupported expression");
    }
}
Ty *ck_expr(Ck *c, Expr *e){
    Ty *t=ck_expr_inner(c,e);
    xinfo(e)->ty=t;
    if(c->strict){
        if(c->nseen==c->cseen){ c->cseen=c->cseen?c->cseen*2:256; c->seen=(Ty**)xrealloc(c->seen,sizeof(Ty*)*(size_t)c->cseen); }
        c->seen[c->nseen++]=t;
    }
    return t;
}

