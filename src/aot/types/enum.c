/* ========================= Typed compiler, types: enum =========================
   Enum / IntEnum / Flag subclasses: their members become objects of
   the class. */

#include "check.h"

/* class Color(Enum): RED = 1 ... (a subclass of enum.py's Enum, IntEnum, StrEnum, Flag, IntFlag):
   each member becomes a class attribute the class makes of itself, RED = Color("RED", 1) (an
   alias: RED's object); written here are __init__, value, repr, the lists _members_ (what
   iterating gives), _all_, _member_map_ (__members__) and the lookups _lookup_(value) and
   _by_name_(name), which Color(v), Color["RED"], `for m in Color`, len() and `in` use. auto()
   and int expressions of earlier members are worked out here. */
static AClass *enum_base(AClass *cls, const char *name){
    for(AClass *b=cls->base;b;b=b->base) if(b->mod && b->mod->name && !strcmp(b->mod->name,"enum") && !strcmp(b->name,name)) return b;
    return NULL;
}
static int enum_int(Expr *e, char **names, long long *vals, int *isint, int n, long long *out){
    if(!e) return 0;
    if(e->kind==EXPR_LITERAL && e->tok->kind==T_NUMBER && !e->tok->is_float){ *out=e->tok->i; return 1; }
    if(e->kind==EXPR_NAME){ for(int i=n-1;i>=0;i--) if(isint[i] && !strcmp(names[i],e->name)){ *out=vals[i]; return 1; } return 0; }
    long long a, b;
    if(e->kind==EXPR_UNARY){ if(!enum_int(e->a,names,vals,isint,n,&a)) return 0;
        if(e->op==T_MINUS){ *out=-a; return 1; } if(e->op==T_PLUS){ *out=a; return 1; } if(e->op==T_TILDE){ *out=~a; return 1; } return 0; }
    if(e->kind==EXPR_BINARY){ if(!enum_int(e->a,names,vals,isint,n,&a) || !enum_int(e->b,names,vals,isint,n,&b)) return 0;
        switch(e->op){ case T_PIPE: *out=a|b; return 1; case T_AMP: *out=a&b; return 1; case T_CARET: *out=a^b; return 1;
            case T_PLUS: *out=a+b; return 1; case T_MINUS: *out=a-b; return 1; case T_STAR: *out=a*b; return 1;
            case T_SHL: if(b<0||b>62) return 0; *out=a<<b; return 1; case T_SHR: if(b<0||b>63) return 0; *out=a>>b; return 1;
            default: return 0; } }
    return 0;
}
/* the type of a literal value, as an annotation ("int", "tuple[float, float]"), or NULL */
static char *enum_lit_type(Expr *e){
    if(e->kind==EXPR_UNARY && (e->op==T_MINUS||e->op==T_PLUS)) e=e->a;
    if(e->kind==EXPR_LITERAL && e->tok->kind==T_NUMBER) return xstrdup2(e->tok->is_float?"float":"int");
    if(e->kind==EXPR_LITERAL && e->tok->kind==T_STRING) return e->tok->i==2 ? xstrdup2("bytes") : e->tok->i==1 ? NULL : xstrdup2("str");
    if(e->kind==EXPR_TRUE || e->kind==EXPR_FALSE) return xstrdup2("bool");
    if(e->kind==EXPR_TUPLE && e->count>0){ char buf[600]; snprintf(buf,sizeof buf,"tuple[");
        for(int i=0;i<e->count;i++){ char *t=enum_lit_type(e->items[i]); if(!t) return NULL;
            size_t l=strlen(buf); snprintf(buf+l,sizeof buf-l,"%s%s",i?", ":"",t); free(t); }
        size_t l=strlen(buf); snprintf(buf+l,sizeof buf-l,"]"); return xstrdup2(buf); }
    return NULL;
}
/* a type annotation as text (names, attributes, subscripts, | None) */
static int ann_text(Expr *e, char *out, size_t n){
    size_t l=strlen(out);
    if(e->kind==EXPR_NAME){ snprintf(out+l,n-l,"%s",e->name); return 1; }
    if(e->kind==EXPR_NONE){ snprintf(out+l,n-l,"None"); return 1; }
    if(e->kind==EXPR_LITERAL && e->tok->kind==T_STRING && !e->tok->i){ snprintf(out+l,n-l,"%s",e->tok->text); return 1; }
    if(e->kind==EXPR_ATTRIBUTE){ if(!ann_text(e->a,out,n)) return 0; l=strlen(out); snprintf(out+l,n-l,".%s",e->name); return 1; }
    if(e->kind==EXPR_INDEX){ if(!ann_text(e->a,out,n)) return 0; l=strlen(out); snprintf(out+l,n-l,"["); if(!ann_text(e->b,out,n)) return 0; l=strlen(out); snprintf(out+l,n-l,"]"); return 1; }
    if(e->kind==EXPR_TUPLE){ for(int i=0;i<e->count;i++){ if(i){ l=strlen(out); snprintf(out+l,n-l,", "); } if(!ann_text(e->items[i],out,n)) return 0; } return 1; }
    if(e->kind==EXPR_BINARY && e->op==T_PIPE){ if(!ann_text(e->a,out,n)) return 0; l=strlen(out); snprintf(out+l,n-l," | "); return ann_text(e->b,out,n); }
    return 0;
}
static int enum_same_lit(Expr *a, Expr *b){
    if(a->kind!=EXPR_LITERAL || b->kind!=EXPR_LITERAL || a->tok->kind!=b->tok->kind) return 0;
    if(a->tok->kind==T_NUMBER) return a->tok->is_float==b->tok->is_float && (a->tok->is_float ? a->tok->f==b->tok->f : a->tok->i==b->tok->i);
    return a->tok->i==b->tok->i && a->tok->len==b->tok->len && !memcmp(a->tok->text,b->tok->text,(size_t)a->tok->len);
}
static void sbx(char **b, size_t *n, size_t *cap, const char *fmt, ...){
    va_list ap; char tmp[4096]; va_start(ap,fmt); int k=vsnprintf(tmp,sizeof tmp,fmt,ap); va_end(ap);
    if(k<0) return; if((size_t)k>=sizeof tmp) k=(int)sizeof tmp-1;
    if(*n+(size_t)k+1>*cap){ *cap=(*n+(size_t)k+1)*2; *b=(char*)xrealloc(*b,*cap); }
    memcpy(*b+*n,tmp,(size_t)k); *n+=(size_t)k; (*b)[*n]=0;
}
static int is_call_of(Expr *e, const char *name){
    return e && e->kind==EXPR_CALL && ((e->a->kind==EXPR_NAME && !strcmp(e->a->name,name)) || (e->a->kind==EXPR_ATTRIBUTE && !strcmp(e->a->name,name) && e->a->a->kind==EXPR_NAME && !strcmp(e->a->a->name,"enum")));
}
static void enum_transform(Ck *c, AClass *cls){
    if(!enum_base(cls,"Enum")) return;
    Stmt *def=cls->def; AotUnit *u=cls->mod->unit; const char *cn=def->name; int line=def->line;
    int flag=enum_base(cls,"Flag")!=NULL, intk=enum_base(cls,"IntEnum")||enum_base(cls,"IntFlag"), strk=enum_base(cls,"StrEnum")!=NULL;
    enum { MAXM=512 };
    char **names=PPY_NEW_ARR(char*,MAXM); Expr **vals=PPY_NEW_ARR(Expr*,MAXM); Stmt **stmts=PPY_NEW_ARR(Stmt*,MAXM);
    int *alias=PPY_NEW_ARR(int,MAXM), *isint=PPY_NEW_ARR(int,MAXM); long long *iv=PPY_NEW_ARR(long long,MAXM); int n=0;
    char *vt=NULL; int vt_bad=0; char vt_user[300]={0};
    Stmt *user_init=NULL; int has_missing=0, has_repr=0, has_str=0, done=0;
    for(int i=0;i<def->body_count;i++){ Stmt *s=def->body[i];
        if(s->kind==STMT_FUNCTION_DEF){
            if(!strcmp(s->name,"__init__")) user_init=s;
            if(!strcmp(s->name,"_missing_")) has_missing=1;
            if(!strcmp(s->name,"__repr__")) has_repr=1;
            if(!strcmp(s->name,"__str__")) has_str=1;
            if(!strcmp(s->name,"_lookup_")) done=1;
            continue; }
        if(aot_is_annotation_only(u,s)){
            if(s->targets[0]->kind==EXPR_NAME && !strcmp(s->targets[0]->name,"_value_") && s->ann && !ann_text(s->ann,vt_user,sizeof vt_user))
                ck_err(c,s->line,"this annotation of _value_ is not supported in compiled code");
            continue; }
        if(s->kind!=STMT_ASSIGN) continue;
        AAssign *a=aot_assign(u,s);
        if(a->ntarget!=1 || a->target[0]->kind!=EXPR_NAME || a->aug || !a->value) continue;
        const char *nm=a->target[0]->name; size_t ln=strlen(nm);
        if(nm[0]=='_' && ((ln>2 && nm[ln-1]=='_') || nm[1]=='_')) continue;                   /* _sunder_, __dunder__, __private: not members */
        Expr *v=a->value;
        if(is_call_of(v,"nonmember") && v->count==1){ a->value=s->value=v->items[0]; continue; }
        if(is_call_of(v,"member") && v->count==1) v=v->items[0];
        if(v->kind==EXPR_LAMBDA) continue;
        if(n==MAXM) ck_err(c,s->line,"too many members in enum %s",cn);
        if(is_call_of(v,"auto") && v->count==0){                                               /* auto(): the next value */
            if(strk){ char low[256]; snprintf(low,sizeof low,"%s",nm); for(char *q=low;*q;q++) if(*q>='A'&&*q<='Z') *q=(char)(*q-'A'+'a'); v=lit_str(s->line,low); }
            else { long long last=0; int have=0;
                for(int k=n-1;k>=0;k--) if(!alias[k] || 1){ if(!isint[k]) ck_err(c,s->line,"auto() after a member whose value is not an int (enum %s)",cn); last=iv[k]; have=1; break; }
                long long nv;
                if(!have) nv=1;
                else if(flag){ int hb=0; for(long long x=last;x>0;x>>=1) hb++; nv=1LL<<hb; }
                else nv=last+1;
                v=lit_int(s->line,nv); } }
        long long iv0; int isi=enum_int(v,names,iv,isint,n,&iv0);
        if(isi && !(v->kind==EXPR_LITERAL)) v=lit_int(s->line,iv0);
        names[n]=(char*)nm; vals[n]=v; stmts[n]=s; isint[n]=isi; iv[n]=isi?iv0:0; alias[n]=0;
        for(int k=0;k<n;k++) if(!alias[k] && ((isi && isint[k] && iv[k]==iv0) || (!isi && enum_same_lit(vals[k],v)))){ alias[n]=k+1; break; }
        if(!alias[n] && !vt_user[0]){ char *t=enum_lit_type(v);
            if(!t) vt_bad=1;
            else if(!vt) vt=t;
            else if(strcmp(vt,t)){ if((!strcmp(vt,"int")&&!strcmp(t,"float"))||(!strcmp(vt,"float")&&!strcmp(t,"int"))){ free(vt); vt=xstrdup2("float"); } else vt_bad=1; } }
        n++; }
    if(!n || done) return;
    if(flag){ for(int k=0;k<n;k++) if(!isint[k]) ck_err(c,stmts[k]->line,"the members of Flag %s must have int values in compiled code",cn); }
    const char *VT= vt_user[0] ? vt_user : vt;
    if(!VT || (vt_bad && !vt_user[0])) ck_err(c,line,"the members of enum %s must have literal values of one type in compiled code (or declare it: _value_: T)",cn);
    cls->is_enum= flag ? 2 : 1;
    cls->enum_int= intk;
    for(int i=0;i<def->decorator_count;i++){ Expr *d=aot_expr(u,&def->decorator_exprs[i]); Expr *dn= d->kind==EXPR_CALL ? d->a : d;    /* @unique, @verify(...) */
        const char *dname= dn->kind==EXPR_NAME ? dn->name : dn->kind==EXPR_ATTRIBUTE ? dn->name : "";
        if(strcmp(dname,"unique") && strcmp(dname,"verify")) continue;
        if(!strcmp(dname,"unique")){ char dup[600]={0};
            for(int k=0;k<n;k++) if(alias[k]){ size_t l=strlen(dup); snprintf(dup+l,sizeof dup-l,"%s%s -> %s",dup[0]?", ":"",names[k],names[alias[k]-1]); }
            if(dup[0]) ck_err(c,line,"ValueError: duplicate values found in <enum '%s'>: %s",cn,dup); }
        for(int j=i;j+1<def->decorator_count;j++) def->decorator_exprs[j]=def->decorator_exprs[j+1];
        def->decorator_count--; i--; }
    int user_tuple=0;
    for(int k=0;k<n;k++){ Stmt *s=stmts[k]; AAssign *a=aot_assign(u,s); Expr *nv;
        if(alias[k]) nv=name_expr(names[alias[k]-1],s->line);
        else { nv=xnew(EXPR_CALL,s->line); nv->a=name_expr((char*)cn,s->line); xpush(nv,lit_str(s->line,names[k])); xpush(nv,vals[k]);
            if(vals[k]->kind==EXPR_TUPLE) user_tuple=1; }
        a->value=s->value=nv; }
    if(user_init) user_init->name="_enum_init_";
    long long allbits=0; for(int k=0;k<n;k++) if(!alias[k] && isint[k]) allbits|=iv[k];
    char *b=NULL; size_t bn=0, bc=0;
    sbx(&b,&bn,&bc,"class __E:\n");
    sbx(&b,&bn,&bc,"    def __init__(self, _name_: str, _value_: %s) -> None:\n        self._name_ = _name_\n        self._value_ = _value_\n",VT);
    if(user_init) sbx(&b,&bn,&bc,"        self._enum_init_(%s_value_)\n",user_tuple?"*":"");
    sbx(&b,&bn,&bc,"    @property\n    def value(self) -> %s:\n        return self._value_\n",VT);
    if(!has_repr){
        if(flag) sbx(&b,&bn,&bc,"    def __repr__(self) -> str:\n        if self._name_ == \"\":\n            return \"<%s: \" + repr(self._value_) + \">\"\n        return \"<%s.\" + self._name_ + \": \" + repr(self._value_) + \">\"\n",cn,cn);
        else sbx(&b,&bn,&bc,"    def __repr__(self) -> str:\n        return \"<%s.\" + self._name_ + \": \" + repr(self._value_) + \">\"\n",cn); }
    if(!has_str){
        if(intk||strk) sbx(&b,&bn,&bc,"    def __str__(self) -> str:\n        return str(self._value_)\n");
        else if(flag) sbx(&b,&bn,&bc,"    def __str__(self) -> str:\n        if self._name_ == \"\":\n            return \"%s(\" + repr(self._value_) + \")\"\n        return \"%s.\" + self._name_\n",cn,cn); }
    if(intk){ sbx(&b,&bn,&bc,"    def __int__(self) -> int:\n        return self._value_\n    def __index__(self) -> int:\n        return self._value_\n");
        static const char *const ops[][2]={{"lt","<"},{"le","<="},{"gt",">"},{"ge",">="}};
        for(int k=0;k<4;k++) sbx(&b,&bn,&bc,"    def __%s__(self, other: \"%s\") -> bool:\n        return self._value_ %s other._value_\n",ops[k][0],cn,ops[k][1]); }
    sbx(&b,&bn,&bc,"    @staticmethod\n    def _lookup_(value: %s) -> \"%s\":\n        for m in %s._all_:\n            if m._value_ == value:\n                return m\n",VT,cn,cn);
    if(flag){
        sbx(&b,&bn,&bc,"        p = %s._pseudo_.get(value)\n        if p is not None:\n            return p\n",cn);
        if(!intk) sbx(&b,&bn,&bc,"        if value < 0 or value & ~%lld != 0:\n            raise ValueError(repr(value) + \" is not a valid %s\")\n",allbits,cn);
        sbx(&b,&bn,&bc,"        name = \"\"\n        rest = value\n        for m in %s._members_:\n            if m._value_ & value == m._value_:\n"
                       "                name = m._name_ if name == \"\" else name + \"|\" + m._name_\n                rest &= ~m._value_\n",cn);
        if(intk) sbx(&b,&bn,&bc,"        if rest != 0:\n            name = str(rest) if name == \"\" else name + \"|\" + str(rest)\n");
        sbx(&b,&bn,&bc,"        p = %s(name, value)\n        %s._pseudo_[value] = p\n        return p\n",cn,cn); }
    else {
        if(has_missing) sbx(&b,&bn,&bc,"        r = %s._missing_(value)\n        if r is not None:\n            return r\n",cn);
        sbx(&b,&bn,&bc,"        raise ValueError(repr(value) + \" is not a valid %s\")\n",cn); }
    sbx(&b,&bn,&bc,"    @staticmethod\n    def _by_name_(name: str) -> \"%s\":\n        m = %s._member_map_.get(name)\n        if m is None:\n            raise KeyError(name)\n        return m\n",cn,cn);
    if(flag){
        sbx(&b,&bn,&bc,"    def __or__(self, other: \"%s\") -> \"%s\":\n        return %s._lookup_(self._value_ | other._value_)\n",cn,cn,cn);
        sbx(&b,&bn,&bc,"    def __and__(self, other: \"%s\") -> \"%s\":\n        return %s._lookup_(self._value_ & other._value_)\n",cn,cn,cn);
        sbx(&b,&bn,&bc,"    def __xor__(self, other: \"%s\") -> \"%s\":\n        return %s._lookup_(self._value_ ^ other._value_)\n",cn,cn,cn);
        sbx(&b,&bn,&bc,"    def __invert__(self) -> \"%s\":\n        return %s._lookup_(%lld & ~self._value_)\n",cn,cn,allbits);
        sbx(&b,&bn,&bc,"    def __contains__(self, other: \"%s\") -> bool:\n        return other._value_ & self._value_ == other._value_\n",cn);
        sbx(&b,&bn,&bc,"    def __iter__(self) -> list[\"%s\"]:\n        return [m for m in %s._members_ if m._value_ & self._value_ == m._value_]\n",cn,cn);
        sbx(&b,&bn,&bc,"    def __len__(self) -> int:\n        return len(self.__iter__())\n");
        sbx(&b,&bn,&bc,"    def __bool__(self) -> bool:\n        return self._value_ != 0\n");
        sbx(&b,&bn,&bc,"    _pseudo_: dict[int, \"%s\"] = {}\n",cn); }
    sbx(&b,&bn,&bc,"    _members_: list[\"%s\"] = [",cn);
    { int first=1; for(int k=0;k<n;k++) if(!alias[k] && (!flag || (iv[k]>0 && !(iv[k]&(iv[k]-1))))){ sbx(&b,&bn,&bc,"%s%s",first?"":", ",names[k]); first=0; } }
    sbx(&b,&bn,&bc,"]\n    _all_: list[\"%s\"] = [",cn);
    { int first=1; for(int k=0;k<n;k++) if(!alias[k]){ sbx(&b,&bn,&bc,"%s%s",first?"":", ",names[k]); first=0; } }
    sbx(&b,&bn,&bc,"]\n    _member_map_: dict[str, \"%s\"] = {",cn);
    for(int k=0;k<n;k++) sbx(&b,&bn,&bc,"%s\"%s\": %s",k?", ":"",names[k],names[k]);
    sbx(&b,&bn,&bc,"}\n");
    Stmt *blk=py_front_stmts(u->path,b,line);
    if(!blk || blk->body_count!=1 || blk->body[0]->kind!=STMT_CLASS_DEF) ck_err(c,line,"internal error: enum %s",cn);
    Stmt *g=blk->body[0];
    Stmt **nb=PPY_NEW_ARR(Stmt*,def->body_count+g->body_count);
    for(int i=0;i<def->body_count;i++) nb[i]=def->body[i];
    for(int i=0;i<g->body_count;i++){ nb[def->body_count+i]=g->body[i]; }
    def->body=nb; def->body_count+=g->body_count; def->body_cap=def->body_count;
    free(b); free(vt);
}
/* Color, an enum class, where its members are meant (for m in Color, len(Color), m in Color): Color._members_ */
int enum_members_expr(Ck *c, Expr *e){
    if(!e || (e->kind!=EXPR_NAME && e->kind!=EXPR_ATTRIBUTE)) return 0;
    AClass *k=class_expr(c,e); if(!k || !k->is_enum) return 0;
    Expr *orig=PPY_NEW0(Expr); *orig=*e; orig->ty=NULL; orig->akind=0; orig->kw=NULL;
    int line=e->line, ak=e->akind; char *kw=e->kw; memset(e,0,sizeof *e); e->kind=EXPR_ATTRIBUTE; e->line=line; e->a=orig; e->name="_members_"; e->akind=ak; e->kw=kw;
    return 1;
}
void fill_class(Ck *c, AClass *cls){
    if(cls->filled) return;
    cls->filled=1;
    Stmt *def=cls->def; AotUnit *u=cls->mod->unit;
    AClass *save_cls=c->sig_cls; c->sig_cls=cls;
    if(def->param_count>1) several_bases(c,cls);
    else if(def->name2){
        ASym *s=cls->encl ? local_class_in(cls->encl,def->name2) : NULL;
        if(!s) s=global_sym(cls->mod,def->name2);
        const char *dot=strchr(def->name2,'.');
        if(dot){ char mn[256]; snprintf(mn,sizeof mn,"%.*s",(int)(dot-def->name2),def->name2);   /* class A(module.Base), class A(Outer.Inner) */
            ASym *ms=cls->encl ? local_class_in(cls->encl,mn) : NULL; if(!ms) ms=global_sym(cls->mod,mn);
            static ASym abcs; if(ms && ms->kind==AS_SYS && !strcmp((const char*)ms->p,"abc") && !strcmp(dot+1,"ABC")){ abcs.kind=AS_CLASS; abcs.p=abc_class(c); ms=NULL; s=&abcs; }   /* class C(abc.ABC) */
            else
            if(ms && ms->kind==AS_CLASS && !strchr(dot+1,'.')){ static ASym ns; AClass *k=aot_nested_class(c->p,(AClass*)ms->p,dot+1); ns.kind=AS_CLASS; ns.p=k; s=k?&ns:NULL; }
            else s=dotted_rest(c,ms,dot+1); }                              /* (module.Base, package.module.Base) */
        if(!s || s->kind!=AS_CLASS) ck_err(c,def->line,"base class '%s' is not a class",def->name2);
        cls->base=(AClass*)s->p;
        if(aot_subclass(cls->base,cls)) ck_err(c,def->line,"class %s inherits from itself",cls->name);
        if(cls->base->def){ AModule *save=c->mod; c->mod=cls->base->mod; fill_class(c,cls->base); c->mod=save; }   /* (its attributes may be redefined here) */
    }
    enum_transform(c,cls);
    class_decorators(c,cls);
    if(def->ntparams) cls->generic=1;
    { Stmt *ini=body_def(def,"__init__"); if(ini){ for(int i=1;i<ini->param_count;i++) if(!ini->annotations || i>=ini->annotation_cap || !ini->annotations[i]) cls->generic=1; } }
    for(int i=0;i<def->body_count;i++){ Stmt *s=def->body[i];
        if(s->kind==STMT_FUNCTION_DEF && s->decorator_count==1){                 /* @x.setter def x(self, v): the method __ppy_set_x */
            Expr *d=aot_expr(u,&s->decorator_exprs[0]);
            if(d->kind==EXPR_ATTRIBUTE && d->a->kind==EXPR_NAME && !strcmp(d->a->name,s->name) && (!strcmp(d->name,"setter")||!strcmp(d->name,"deleter"))){
                char nm[300]; snprintf(nm,sizeof nm,"__ppy_%s_%s",d->name[0]=='s'?"set":"del",s->name);
                s->name=xstrdup2(nm); s->decorator_count=0; } }
        if(s->kind==STMT_FUNCTION_DEF){
            AFunc *f=new_func(c,NULL,s->src_mod?(AModule*)s->src_mod:cls->mod,cls,s,NULL);   /* (a mixin's: in its module) */
            if((!s->decorator_count || (f->is_static && !f->ndeco && !f->is_property && !f->cm)) && !s->ntparams && s->param_count>(f->is_static?0:1)
               && (def->ntparams ? foreign_typevar_params(cls->mod,s,def) : (typevar_params(cls->mod,s) || s->packargs))) f->pristine=s;   /* def m(self, x: T): a copy per kind of x (static ones too) */
            if(s->param_count>0 && !s->decorator_count) annotated_self_fields(c,cls,u,s->params[0],s->body,s->body_count);
            if(f->ndeco){ char *vn=hidden_name(cls->symname,s->name);
                f->decovar=new_global(c,cls->mod,vn+1);          /* Class.name after its decorators */
                symtab_add(&cls->mod->syms,vn,AS_FUNC,f); }
            if(cls->nmethods==cls->mcap){ cls->mcap=cls->mcap?cls->mcap*2:8; cls->methods=(AFunc**)xrealloc(cls->methods,sizeof(AFunc*)*(size_t)cls->mcap); }
            cls->methods[cls->nmethods++]=f;
        } else if(aot_is_annotation_only(u,s)){
            AAssign *a=aot_assign(u,s);
            AModule *sm=c->mod; if(s->src_mod) c->mod=(AModule*)s->src_mod;          /* (a mixin's field: its annotation in its module) */
            Ty *t=type_of(c,a->ann,s->line); c->mod=sm;
            add_field(cls,a->target[0]->name,t,NULL);
        } else if(s->kind==STMT_ASSIGN){
            AAssign *a=aot_assign(u,s);
            if(a->ntarget!=1 || a->target[0]->kind!=EXPR_NAME || a->aug) ck_err(c,s->line,"a class body may only declare fields (name: type [= value]) and methods");
            AModule *sm=c->mod; if(s->src_mod) c->mod=(AModule*)s->src_mod;
            Ty *t=a->ann ? type_of(c,a->ann,s->line) : NULL; c->mod=sm;
            AField *bf=cls->base ? aot_find_field(cls->base,a->target[0]->name) : NULL, *fd;
            if(bf){ bf=aot_field_root(bf); bf->overridden=1;                  /* redefined in a subclass: the same slot, a value of its own */
                fd=add_field(cls,bf->name,bf->ty,a->value); fd->over=bf;
                if(t) expect(c,bf->ty,t,s->line,"the redefined class attribute"); }
            else fd=add_field(cls,a->target[0]->name,t,a->value);
            char h[600]; snprintf(h,sizeof h," %s.%s",cls->symname,fd->name);     /* its class-level value: evaluated when the class statement runs */
            fd->cvar=new_global(c,cls->mod,h); fd->cvar->ty=fd->ty; symtab_add(&cls->mod->syms,h,AS_VAR,fd->cvar);
        } else if(s->kind==STMT_CLASS_DEF){                    /* a nested class: made with this one */
        } else if(s->kind==STMT_PASS || (s->kind==STMT_EXPR && aot_expr(u,&s->expr)->kind==EXPR_LITERAL)){
        } else ck_err(c,s->line,"a class body may only declare fields and methods in compiled code");
    }
    c->sig_cls=save_cls;
}

/* Module namespace: functions, classes, globals; imports are linked afterwards. */
