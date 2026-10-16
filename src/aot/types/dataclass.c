/* ========================= Typed compiler, types: @dataclass =========================
   @dataclass classes: the methods it writes, fields(), asdict(),
   astuple(); @total_ordering; class decorators. */

#include "check.h"

void tx(Txt *t, const char *fmt, ...){
    va_list ap; va_start(ap,fmt); char tmp[2048]; int k=vsnprintf(tmp,sizeof tmp,fmt,ap); va_end(ap);
    if(k<0) return; if((size_t)k>=sizeof tmp) k=(int)sizeof tmp-1;
    if(t->n+(size_t)k+1>t->cap){ t->cap=(t->n+(size_t)k+1)*2; t->s=(char*)xrealloc(t->s,t->cap); }
    memcpy(t->s+t->n,tmp,(size_t)k); t->n+=(size_t)k; t->s[t->n]=0;
}
/* x names dataclasses.<what> (`what` imported from dataclasses, or dataclasses.what) */
static int dc_is(AClass *cls, Expr *x, const char *what){
    if(!x) return 0;
    if(x->kind==EXPR_INDEX) x=x->a;
    if(x->kind==EXPR_NAME){ ASym *s=global_sym(cls->mod,x->name); char full[64]; snprintf(full,sizeof full,"dataclasses.%s",what);
        return s && s->kind==AS_SYS && !strcmp((const char*)s->p,full); }
    if(x->kind==EXPR_ATTRIBUTE && x->a->kind==EXPR_NAME && !strcmp(x->name,what)){ ASym *s=global_sym(cls->mod,x->a->name);
        return s && s->kind==AS_SYS && !strcmp((const char*)s->p,"dataclasses"); }
    return 0;
}
static int is_classvar(Expr *a){
    if(a->kind==EXPR_LITERAL && a->tok->kind==T_STRING) return !strncmp(a->tok->text,"ClassVar",8) || !strncmp(a->tok->text,"typing.ClassVar",15);
    if(a->kind==EXPR_INDEX) a=a->a;
    return (a->kind==EXPR_NAME && !strcmp(a->name,"ClassVar")) || (a->kind==EXPR_ATTRIBUTE && !strcmp(a->name,"ClassVar"));
}
static int dc_flag(Ck *c, Expr *v, int line, const char *what){
    if(v->kind==EXPR_TRUE) return 1;
    if(v->kind==EXPR_FALSE) return 0;
    ck_err(c,line,"%s= must be True or False in compiled code",what);
}
static void dc_add(struct DcInfo *dc, DcField *f){
    for(int i=0;i<dc->n;i++) if(!strcmp(dc->f[i].name,f->name)){ dc->f[i]=*f; return; }
    if(dc->n==dc->cap){ dc->cap=dc->cap?dc->cap*2:8; dc->f=(DcField*)xrealloc(dc->f,sizeof(DcField)*(size_t)dc->cap); }
    dc->f[dc->n++]=*f;
}
static AProg *g_dc_prog;
Stmt *body_def(Stmt *def, const char *name){
    for(int i=0;i<def->body_count;i++) if(def->body[i]->kind==STMT_FUNCTION_DEF && !strcmp(def->body[i]->name,name)) return def->body[i];
    return NULL;
}
/* a method written as source: parsed at the class's line, added to its body */
static Stmt *dc_method(Ck *c, AClass *cls, Txt *t){
    Stmt *def=cls->def; Txt src={0};
    for(int i=1;i<def->line;i++) tx(&src,"\n");
    tx(&src,"%s",t->s);
    Stmt *blk=py_front_stmts(cls->mod->unit->path,src.s,def->line);
    if(!blk || blk->body_count!=1) ck_err(c,def->line,"internal error: a method of dataclass %s",cls->name);
    free(src.s); free(t->s); memset(t,0,sizeof *t);
    stmt_add_body(def,blk->body[0]);
    return blk->body[0];
}
static void dc_transform(Ck *c, AClass *cls, Expr *deco){
    Stmt *def=cls->def; AotUnit *u=cls->mod->unit; int line=def->line; g_dc_prog=c->p;
    struct DcInfo *dc=PPY_NEW0(struct DcInfo); dc->init=dc->repr=dc->eq=1; int kw_only=0;
    if(deco->kind==EXPR_CALL) for(int i=0;i<deco->count;i++){ Expr *a=deco->items[i];
        if(a->akind!=3) ck_err(c,line,"dataclass() takes keyword arguments only");
        if(!strcmp(a->kw,"init")) dc->init=dc_flag(c,a,line,a->kw);
        else if(!strcmp(a->kw,"repr")) dc->repr=dc_flag(c,a,line,a->kw);
        else if(!strcmp(a->kw,"eq")) dc->eq=dc_flag(c,a,line,a->kw);
        else if(!strcmp(a->kw,"order")) dc->order=dc_flag(c,a,line,a->kw);
        else if(!strcmp(a->kw,"frozen")) dc->frozen=dc_flag(c,a,line,a->kw);
        else if(!strcmp(a->kw,"unsafe_hash")) dc->unsafe_hash=dc_flag(c,a,line,a->kw);
        else if(!strcmp(a->kw,"kw_only")) kw_only=dc_flag(c,a,line,a->kw);
        else if(!strcmp(a->kw,"match_args")||!strcmp(a->kw,"slots")||!strcmp(a->kw,"weakref_slot")) dc_flag(c,a,line,a->kw);
        else ck_err(c,line,"dataclass() got an unexpected keyword argument '%s'",a->kw); }
    if(dc->order && !dc->eq) ck_err(c,line,"eq must be true if order is true");
    AClass *bdc=NULL; for(AClass *b=cls->base;b;b=b->base) if(b->dc){ bdc=b; break; }
    if(bdc){ for(int i=0;i<bdc->dc->n;i++) dc_add(dc,&bdc->dc->f[i]);
        if(bdc->dc->frozen && !dc->frozen) ck_err(c,line,"cannot inherit non-frozen dataclass from a frozen one");
        if(!bdc->dc->frozen && dc->frozen) ck_err(c,line,"cannot inherit frozen dataclass from a non-frozen one"); }
    Stmt **keep=PPY_NEW_ARR(Stmt*,def->body_count+1); int nk=0;
    for(int i=0;i<def->body_count;i++){ Stmt *s=def->body[i];
        int annonly=aot_is_annotation_only(u,s);
        AAssign *a= annonly || s->kind==STMT_ASSIGN ? aot_assign(u,s) : NULL;
        if(!a || !a->ann || a->ntarget!=1 || a->target[0]->kind!=EXPR_NAME){ keep[nk++]=s; continue; }
        const char *name=a->target[0]->name; Expr *ann=a->ann;
        if(dc_is(cls,ann,"KW_ONLY")){ kw_only=1; continue; }            /* _: KW_ONLY: the fields after it are keyword-only */
        if(is_classvar(ann)){ keep[nk++]=s; continue; }                  /* a class attribute, not a field */
        DcField f; memset(&f,0,sizeof f); f.name=name; f.ann=ann; f.init=f.repr=f.compare=1; f.hash=-1; f.kw_only=kw_only;
        if(dc_is(cls,ann,"InitVar")){ f.initvar=1; f.repr=f.compare=0; f.ann= ann->kind==EXPR_INDEX ? ann->b : ann; }
        Expr *v=a->value;
        if(v && v->kind==EXPR_CALL && dc_is(cls,v->a,"field")){        /* name: T = field(...) */
            Expr *dflt=NULL;
            for(int k=0;k<v->count;k++){ Expr *x=v->items[k];
                if(x->akind!=3) ck_err(c,s->line,"field() takes keyword arguments only");
                if(!strcmp(x->kw,"default")) dflt=x;
                else if(!strcmp(x->kw,"default_factory")) f.factory=x;
                else if(!strcmp(x->kw,"init")) f.init=dc_flag(c,x,s->line,x->kw);
                else if(!strcmp(x->kw,"repr")) f.repr=dc_flag(c,x,s->line,x->kw);
                else if(!strcmp(x->kw,"compare")) f.compare=dc_flag(c,x,s->line,x->kw);
                else if(!strcmp(x->kw,"kw_only")) f.kw_only=dc_flag(c,x,s->line,x->kw);
                else if(!strcmp(x->kw,"hash")) f.hash= x->kind==EXPR_NONE ? -1 : dc_flag(c,x,s->line,x->kw);
                else if(!strcmp(x->kw,"metadata")) {}
                else ck_err(c,s->line,"field() got an unexpected keyword argument '%s'",x->kw); }
            if(dflt && f.factory) ck_err(c,s->line,"cannot specify both default and default_factory");
            if(f.factory){ Expr *fe=PPY_NEW0(Expr); *fe=*f.factory; fe->kw=NULL; fe->akind=0; f.factory=fe; }
            if(dflt){ Expr *de=PPY_NEW0(Expr); *de=*dflt; de->kw=NULL; de->akind=0; a->value=s->value=de; v=de; }
            else { a->value=s->value=NULL; v=NULL; s->kind=STMT_EXPR; s->ann_only=1; }   /* no class attribute */
        }
        if(v){ f.has_default=1; f.defsym=cls->symname;
            const char *mk= v->kind==EXPR_LIST||(v->kind==EXPR_COMPREHENSION&&v->comp_kind=='L') ? "list" : v->kind==EXPR_DICT||(v->kind==EXPR_COMPREHENSION&&v->comp_kind=='D') ? "dict"
                          : v->kind==EXPR_SET||(v->kind==EXPR_COMPREHENSION&&v->comp_kind=='S') ? "set" : NULL;
            if(mk) ck_err(c,s->line,"mutable default <class '%s'> for field %s is not allowed: use default_factory",mk,name); }
        dc_add(dc,&f);
        if(f.initvar && !f.has_default) continue;                         /* (not an attribute) */
        keep[nk++]=s;
    }
    def->body=keep; def->body_count=nk; def->body_cap=def->body_count+1;
    cls->dc=dc; cls->dc_frozen=dc->frozen;
    { const char *seen=NULL;                                            /* defaults last (keyword-only ones aside) */
      for(int i=0;i<dc->n;i++){ DcField *f=&dc->f[i]; if(!f->init || f->kw_only) continue;
          if(f->has_default || f->factory) seen=f->name;
          else if(seen) ck_err(c,line,"non-default argument '%s' follows default argument '%s'",f->name,seen); } }
    char ref[600]; snprintf(ref,sizeof ref,"%s",cls->outer?cls->qualname:cls->name);   /* the class, as an annotation */
    Txt t={0};
    if(dc->init && !body_def(def,"__init__")){                          /* def __init__(self, x, y=..., *, z=...) */
        int post=0; for(AClass *k=cls;k;k=k->base) if(k->def && body_def(k->def,"__post_init__")) post=1;
        tx(&t,"def __init__(self");
        int star=0;
        for(int pass=0;pass<2;pass++) for(int i=0;i<dc->n;i++){ DcField *f=&dc->f[i];
            if(!f->init || f->kw_only!=pass) continue;
            if(pass && !star){ tx(&t,", *"); star=1; }
            tx(&t,", %s%s",f->name,f->has_default||f->factory?"=None":""); }
        tx(&t,"):\n");
        int body=0;
        for(int i=0;i<dc->n;i++){ DcField *f=&dc->f[i]; if(f->initvar) continue;
            if(f->init) tx(&t,f->factory?"    self.%s = __factory__() if %s is None else %s\n":"    self.%s = %s\n",f->name,f->name,f->name);
            else if(f->factory) tx(&t,"    self.%s = __factory__()\n",f->name);
            else continue;
            body++; }
        if(post){ tx(&t,"    self.__post_init__("); int n=0; for(int i=0;i<dc->n;i++) if(dc->f[i].initvar) tx(&t,"%s%s",n++?", ":"",dc->f[i].name); tx(&t,")\n"); body++; }
        if(!body) tx(&t,"    pass\n");
        Stmt *d=dc_method(c,cls,&t); dc->init_def=d;
        int p=1;                                                         /* the annotations and defaults: the fields' */
        if(!d->pdefaults){ d->pdefaults=PPY_NEW_ARR(Expr*,d->param_count+1); }
        for(int pass=0;pass<2;pass++) for(int i=0;i<dc->n;i++){ DcField *f=&dc->f[i];
            if(!f->init || f->kw_only!=pass) continue;
            Expr *an=f->ann;
            if(f->factory){ Expr *o=xnew(EXPR_BINARY,line); o->op=T_PIPE; o->a=f->ann; o->b=xnew(EXPR_NONE,line); an=o; }
            stmt_set_annotation(d,p,an);
            if(f->has_default){ char h[640]; snprintf(h,sizeof h," %s.%s",f->defsym,f->name); d->pdefaults[p]=name_expr(xstrdup2(h),line); }
            p++; }
        int k=0;                                                         /* __factory__: the field's default_factory */
        for(int i=0;i<dc->n;i++){ DcField *f=&dc->f[i]; if(f->initvar) continue;
            if(!f->init && !f->factory) continue;
            Stmt *st=d->body[k++];
            if(f->factory){ Expr *val=st->value; Expr *call= val->kind==EXPR_TERNARY ? val->b : val; call->a=f->factory; } }
    }
    if(dc->repr && !body_def(def,"__repr__")){                         /* Name(x=1, y='a') */
        tx(&t,"def __repr__(self) -> str:\n    return type(self).__qualname__ + \"(");
        int n=0; for(int i=0;i<dc->n;i++){ DcField *f=&dc->f[i]; if(!f->repr || f->initvar) continue;
            tx(&t,"%s%s=\" + repr(self.%s) + \"",n++?", ":"",f->name,f->name); }
        tx(&t,")\"\n");
        dc_method(c,cls,&t);
    }
    const char *ops[4][2]={{"__lt__","<"},{"__le__","<="},{"__gt__",">"},{"__ge__",">="}};
    #define DC_TUPLE(who,sel) do{ tx(&t,"("); for(int i=0;i<dc->n;i++){ DcField *f=&dc->f[i]; if(!(sel) || f->initvar) continue; tx(&t,"%s.%s, ",who,f->name); } tx(&t,")"); }while(0)
    if(dc->eq && !body_def(def,"__eq__")){                             /* the fields compared as tuples */
        tx(&t,"def __eq__(self, other: \"%s\") -> bool:\n    if type(other) is not type(self):\n        return False\n    return ",ref);
        DC_TUPLE("self",f->compare); tx(&t," == "); DC_TUPLE("other",f->compare); tx(&t,"\n");
        dc_method(c,cls,&t);
    }
    if(dc->order) for(int o=0;o<4;o++){
        if(body_def(def,ops[o][0])) ck_err(c,line,"Cannot overwrite attribute %s in class %s. Consider using functools.total_ordering",ops[o][0],cls->name);
        tx(&t,"def %s(self, other: \"%s\") -> bool:\n    return ",ops[o][0],ref);
        DC_TUPLE("self",f->compare); tx(&t," %s ",ops[o][1]); DC_TUPLE("other",f->compare); tx(&t,"\n");
        dc_method(c,cls,&t);
    }
    if(c->dc_conv){                                                       /* asdict(obj), astuple(obj) */
        tx(&t,"def __mp_asdict__(self):\n    return {");
        for(int i=0;i<dc->n;i++){ DcField *f=&dc->f[i]; if(f->initvar) continue; char x[300]; snprintf(x,sizeof x,"self.%s",f->name);
            tx(&t,"\"%s\": ",f->name); dc_conv(cls,f->ann,x,1,0,&t); tx(&t,", "); }
        tx(&t,"}\n"); dc_method(c,cls,&t);
        tx(&t,"def __mp_astuple__(self):\n    return (");
        for(int i=0;i<dc->n;i++){ DcField *f=&dc->f[i]; if(f->initvar) continue; char x[300]; snprintf(x,sizeof x,"self.%s",f->name);
            dc_conv(cls,f->ann,x,0,0,&t); tx(&t,", "); }
        tx(&t,")\n"); dc_method(c,cls,&t);
    }
    if((dc->unsafe_hash || (dc->eq && dc->frozen)) && !body_def(def,"__hash__")){
        tx(&t,"def __hash__(self) -> int:\n    return hash(");
        DC_TUPLE("self",f->hash<0?f->compare:f->hash); tx(&t,")\n");
        dc_method(c,cls,&t);
    }
    #undef DC_TUPLE
}
/* asdict / astuple of a value of annotation `ann` (source text `x`): dataclasses (and those in
   lists, dicts, tuples, optional values) converted, containers copied */
static int dc_decorated(AClass *k){
    for(;k;k=k->base){ if(k->dc) return 1;
        if(k->def) for(int i=0;i<k->def->decorator_count;i++){ Expr *d=k->def->decorator_exprs[i]; if(dc_is(k,d->kind==EXPR_CALL?d->a:d,"dataclass")) return 1; } }
    return 0;
}
static AClass *ann_class(AClass *cls, Expr *a){
    ASym *s=NULL;
    if(a->kind==EXPR_NAME){ if(cls->encl) s=local_class_in(cls->encl,a->name); if(!s) s=global_sym(cls->mod,a->name); }
    else if(a->kind==EXPR_ATTRIBUTE && a->a->kind==EXPR_NAME){ ASym *o=global_sym(cls->mod,a->a->name);
        if(o && o->kind==AS_MODULE) s=module_sym((AModule*)o->p,a->name);
        else if(o && o->kind==AS_CLASS) return aot_nested_class(g_dc_prog,(AClass*)o->p,a->name); }
    return s && s->kind==AS_CLASS ? (AClass*)s->p : NULL;
}
void dc_conv(AClass *cls, Expr *a, const char *x, int asdict, int depth, Txt *t){
    if(a->kind==EXPR_LITERAL && a->tok->kind==T_STRING){ Expr *p=py_front_expr(cls->mod->unit->path,a->tok->text,a->line); if(p) a=p; }
    const char *bn= a->kind==EXPR_INDEX&&a->a->kind==EXPR_NAME ? a->a->name : a->kind==EXPR_INDEX&&a->a->kind==EXPR_ATTRIBUTE ? a->a->name : NULL;
    Expr *opt=NULL;
    if(a->kind==EXPR_BINARY && a->op==T_PIPE){ if(a->b->kind==EXPR_NONE) opt=a->a; else if(a->a->kind==EXPR_NONE) opt=a->b; }
    if(bn && !strcmp(bn,"Optional")) opt=a->b;
    if(opt){ tx(t,"(None if %s is None else ",x); dc_conv(cls,opt,x,asdict,depth,t); tx(t,")"); return; }
    if(bn && (!strcmp(bn,"list")||!strcmp(bn,"List")||!strcmp(bn,"Sequence")||!strcmp(bn,"MutableSequence"))){
        char v[32]; snprintf(v,sizeof v,"_e%d",depth); tx(t,"["); dc_conv(cls,a->b,v,asdict,depth+1,t); tx(t," for %s in %s]",v,x); return; }
    if(bn && (!strcmp(bn,"dict")||!strcmp(bn,"Dict")||!strcmp(bn,"Mapping")||!strcmp(bn,"MutableMapping")) && a->b->kind==EXPR_TUPLE && a->b->count==2){
        char k[32], v[32]; snprintf(k,sizeof k,"_k%d",depth); snprintf(v,sizeof v,"_v%d",depth);
        tx(t,"{"); dc_conv(cls,a->b->items[0],k,asdict,depth+1,t); tx(t,": "); dc_conv(cls,a->b->items[1],v,asdict,depth+1,t); tx(t," for %s, %s in %s.items()}",k,v,x); return; }
    if(bn && (!strcmp(bn,"tuple")||!strcmp(bn,"Tuple"))){
        Expr *tp=a->b; int n=tp->kind==EXPR_TUPLE?tp->count:1; Expr **it=tp->kind==EXPR_TUPLE?tp->items:&a->b;
        if(n==2 && it[1]->kind==EXPR_NAME && !strcmp(it[1]->name,"...")){ char v[32]; snprintf(v,sizeof v,"_e%d",depth);
            tx(t,"tuple(["); dc_conv(cls,it[0],v,asdict,depth+1,t); tx(t," for %s in %s])",v,x); return; }
        tx(t,"("); for(int i=0;i<n;i++){ char v[300]; snprintf(v,sizeof v,"%s[%d]",x,i); dc_conv(cls,it[i],v,asdict,depth+1,t); tx(t,", "); } tx(t,")"); return; }
    AClass *k=ann_class(cls,a);
    if(k && dc_decorated(k)){ tx(t,"%s.%s()",x,asdict?"__mp_asdict__":"__mp_astuple__"); return; }
    tx(t,"%s",x);
}
/* @total_ordering: the orderings the class leaves out, from the one it has (functools' rules) */
static void total_ordering(Ck *c, AClass *cls){
    static const char *const ops[4]={"__lt__","__le__","__gt__","__ge__"};
    /* per root: the derived op and its body (r: the root's result) */
    static const char *const conv[4][3][2]={
        {{"__gt__","not r and self != other"},{"__le__","r or self == other"},{"__ge__","not r"}},
        {{"__ge__","not r or self == other"},{"__lt__","r and self != other"},{"__gt__","not r"}},
        {{"__lt__","not r and self != other"},{"__ge__","r or self == other"},{"__le__","not r"}},
        {{"__le__","not r or self == other"},{"__gt__","r and self != other"},{"__lt__","not r"}}};
    int root=-1; Stmt *rd=NULL;
    for(int i=0;i<4 && root<0;i++){ Stmt *d=body_def(cls->def,ops[i]); if(d){ root=i; rd=d; } }
    if(root<0) ck_err(c,cls->def->line,"must define at least one ordering operation: < > <= >=");
    char ref[600]; snprintf(ref,sizeof ref,"%s",cls->outer?cls->qualname:cls->name);
    for(int k=0;k<3;k++){ const char *op=conv[root][k][0]; if(body_def(cls->def,op)) continue;
        Txt t={0}; tx(&t,"def %s(self, other: \"%s\") -> bool:\n    r = self.%s(other)\n    return %s\n",op,ref,ops[root],conv[root][k][1]);
        Stmt *m=dc_method(c,cls,&t);
        if(rd->param_count>1 && rd->annotations && rd->annotation_cap>1 && rd->annotations[1]) stmt_set_annotation(m,1,rd->annotations[1]); }
}
/* the class's decorators: @dataclass (with its options) or none */
void class_decorators(Ck *c, AClass *cls){
    Stmt *def=cls->def;
    for(int i=0;i<def->decorator_count;i++){ Expr *d=def->decorator_exprs[i];
        if(dc_is(cls,d->kind==EXPR_CALL?d->a:d,"dataclass")){ if(cls->dc) ck_err(c,def->line,"@dataclass given twice"); dc_transform(c,cls,d); continue; }
        if((d->kind==EXPR_NAME && !strcmp(d->name,"total_ordering")) || (d->kind==EXPR_ATTRIBUTE && !strcmp(d->name,"total_ordering") && d->a->kind==EXPR_NAME && !strcmp(d->a->name,"functools"))){
            total_ordering(c,cls); continue; }
        if(ut_deco_name(cls->mod && cls->mod->unit ? cls->mod->unit->src : NULL,d)) continue;   /* @unittest.skip(...) ...: read by ut_generate */
        ck_err(c,def->line,"class decorators other than @dataclass and @total_ordering are not supported in compiled code"); }
    if(!cls->dc) for(AClass *b=cls->base;b;b=b->base) if(b->dc_frozen) cls->dc_frozen=1;   /* (a frozen dataclass's __setattr__) */
}
