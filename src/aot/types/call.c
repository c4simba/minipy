/* ========================= Typed compiler, types: calls =========================
   Every call (ck_call_inner): re patterns' groups, @contextmanager
   functions, then built-ins, methods, constructors, functions. */

#include "check.h"

/* ---- re: findall gives tuples for a pattern with several groups (in compiled code: when the pattern
   is a literal - re.findall(LIT, s), or a Pattern only ever made by re.compile(LIT): _findall_t) */
static int re_literal_groups(const char *p){
    int n=0;
    for(const char *q=p;*q;q++){
        if(*q=='\\'){ if(q[1]) q++; continue; }
        if(*q=='['){ q++; if(*q=='^') q++; if(*q==']') q++;
            while(*q && *q!=']'){ if(*q=='\\' && q[1]) q++; q++; }
            if(!*q) break;
            continue; }
        if(*q=='(' && (q[1]!='?' || (q[2]=='P' && q[3]=='<'))) n++; }
    return n;
}
/* f names re's function name */
static int re_callee(Ck *c, Expr *f, const char *name){
    if(!strcmp(c->mod->name,"re")) return 0;
    if(f->kind==EXPR_ATTRIBUTE){ AModule *m=module_expr(c,f->a); return m && !strcmp(m->name,"re") && !strcmp(f->name,name); }
    if(f->kind==EXPR_NAME){ ASym *s=lookup(c,f->name); if(!s || s->kind!=AS_FUNC) return 0;
        AFunc *fn=(AFunc*)s->p; if(fn->origin) fn=fn->origin;
        return !strcmp(fn->mod->name,"re") && !strcmp(fn->name,name); }
    return 0;
}
/* v is re.compile(LIT ...) (or re.findall(LIT ...) for name "findall"): LIT's number of groups; else -1 */
static int re_literal_call(Ck *c, Expr *v, const char *name){
    if(!v || v->kind!=EXPR_CALL || !v->count || v->items[0]->akind) return -1;
    Expr *p=v->items[0];
    if(!(p->kind==EXPR_LITERAL && p->tok->kind==T_STRING && p->tok->i!=2) || !re_callee(c,v->a,name)) return -1;
    return re_literal_groups(p->tok->text);
}
/* target = value: what re_groups records */
void note_re_groups(Ck *c, Expr *t, Expr *v){
    int g=re_literal_call(c,v,"compile"), code= g<0 ? -1 : g+1;
    int *slot=NULL;
    if(t->kind==EXPR_NAME){ ASym *s=lookup(c,t->name); if(s && s->kind==AS_VAR) slot=&var_root((AVar*)s->p)->re_groups; }
    else if(t->kind==EXPR_ATTRIBUTE){ Ty *ot=ty_find(ck_expr(c,t->a));
        if(ot->k==TY_OBJ){ AField *fd=aot_find_field(ot->cls,t->name); if(fd) slot=&aot_field_root(fd)->re_groups; } }
    if(!slot) return;
    if(!*slot) *slot=code; else if(*slot!=code) *slot=-1;
}
/* the receiver of p.findall(...): its pattern's number of groups (-1: not known; -2: not bound yet) */
static int re_receiver_groups(Ck *c, Expr *x){
    if(x->kind==EXPR_CALL) return re_literal_call(c,x,"compile");
    int code=0;
    if(x->kind==EXPR_NAME){ ASym *s=lookup(c,x->name); if(!s || s->kind!=AS_VAR) return -1; code=var_root((AVar*)s->p)->re_groups; }
    else if(x->kind==EXPR_ATTRIBUTE){ Ty *ot=ty_find(ck_expr(c,x->a)); if(ot->k!=TY_OBJ) return -1;
        AField *fd=aot_find_field(ot->cls,x->name); if(!fd) return -1; code=aot_field_root(fd)->re_groups; }
    else return -1;
    return code>0 ? code-1 : code==0 ? -2 : -1;
}
/* f(...) calling a @contextmanager function (or method): it */
static AFunc *cm_callee(Ck *c, Expr *f){
    AFunc *fn=NULL;
    if(f->kind==EXPR_NAME){ ASym *s=lookup(c,f->name); if(s && s->kind==AS_FUNC) fn=(AFunc*)s->p; }
    else if(f->kind==EXPR_ATTRIBUTE){
        AModule *m=module_expr(c,f->a);
        if(m){ ASym *s=module_sym(m,f->name); if(s && s->kind==AS_FUNC) fn=(AFunc*)s->p; }
        else if(class_expr(c,f->a)) fn=aot_find_method(class_expr(c,f->a),f->name);
        else {                                                   /* obj.m(...): obj rooted in a variable or a construction */
            Expr *r=f->a; while(r->kind==EXPR_ATTRIBUTE || r->kind==EXPR_INDEX || r->kind==EXPR_CALL) r=r->a;
            ASym *rs= r->kind==EXPR_NAME ? lookup(c,r->name) : NULL;
            if(rs && (rs->kind==AS_VAR || rs->kind==AS_CLASS)){ Ty *t=ty_find(ck_expr(c,f->a)); if(t->k==TY_OBJ) fn=aot_find_method(t->cls,f->name); } } }
    if(fn && fn->origin) fn=fn->origin;
    return fn && fn->cm ? fn : NULL;
}
Ty *ck_call_inner(Ck *c, Expr *e){
    Expr *f=e->a; XInfo *xi=xinfo(e);
    if(f->kind==EXPR_NAME){ ASym *as=lookup(c,f->name);                  /* sqrt(x) after from math import sqrt: math.sqrt(x) */
        if(as && as->kind==AS_SYS && !strncmp((const char*)as->p,"attr:",5)){
            char hm[200]; const char *q=(const char*)as->p+5, *colon=strchr(q,':');
            snprintf(hm,sizeof hm,"%.*s",(int)(colon-q),q);
            Expr *at=xnew(EXPR_ATTRIBUTE,e->line); at->a=name_expr(xstrdup2(hm),e->line); at->name=xstrdup2(colon+1);
            e->a=f=at; } }
    if((f->kind==EXPR_NAME || f->kind==EXPR_ATTRIBUTE) && e->count==1 && !e->items[0]->akind){ AClass *ek=class_expr(c,f);   /* Color(1): the member of that value */
        if(ek && ek->is_enum){ Expr *at=xnew(EXPR_ATTRIBUTE,e->line); at->a=f; at->name="_lookup_"; e->a=f=at; } }
    if(!e->cm_done && (f->kind==EXPR_NAME || f->kind==EXPR_ATTRIBUTE) && cm_callee(c,f)){   /* cm(...): contextlib._GeneratorCM(the generator) */
        Expr *inner=PPY_NEW0(Expr); *inner=*e; inner->ty=NULL; inner->cm_done=1;
        if(!lookup(c,"__ppy_contextlib__GeneratorCM")) ck_err(c,e->line,"internal error: @contextmanager without contextlib");
        Expr *call=xnew(EXPR_CALL,e->line); call->a=name_expr(xstrdup2("__ppy_contextlib__GeneratorCM"),e->line); xpush(call,inner);
        void *keep=e->ty; *e=*call; e->ty=keep; e->cm_done=1;
        return ck_call_inner(c,e); }
    eg_class_arg(c,e);
    if(re_literal_call(c,e,"findall")>=2) e->a=f=name_expr(xstrdup2("__ppy_re__findall_t"),e->line);   /* re.findall(LIT, ...): tuples */
    if(e->a->kind==EXPR_NAME || e->a->kind==EXPR_ATTRIBUTE){ const char *sf=struct_func(c,e->a);
        if(sf){ Ty *r=ck_struct(c,e,sf); if(r) return r; }
        const char *tf=tempfile_func(c,e->a);
        if(tf){ Expr *mode=NULL;                                           /* NamedTemporaryFile(mode): the binary or the text one */
            for(int i=0;i<e->count;i++){ Expr *a=e->items[i]; if((!i && !a->akind) || (a->akind==3 && !strcmp(a->kw,"mode"))) mode=a; }
            int text=0;
            if(mode){ if(!(mode->kind==EXPR_LITERAL && mode->tok->kind==T_STRING && mode->tok->i!=2)) ck_err(c,e->line,"tempfile.%s(): the mode must be a literal in compiled code",tf);
                text=!strchr(mode->tok->text,'b'); }
            char nm[96]; snprintf(nm,sizeof nm,"__ppy_tempfile__%s%c",tf,text?'T':'B');
            e->a=name_expr(xstrdup2(nm),e->line); } }
    if(f->kind==EXPR_NAME){
        if(f->name[0]=='\001') return ck_builtin(c,e,f->name+1);         /* made by the compiler: always the builtin */
        if(!strncmp(f->name,"__ppy_ut_",9) && !lookup(c,f->name)) ut_generate(c,e->line);   /* (unittest: the program's tests) */
        ASym *s=lookup(c,f->name);
        if(!s) return ck_builtin(c,e,f->name);
        if(s->kind==AS_FUNC){ AFunc *fn=(AFunc*)s->p;
            if(fn->outer){ xi->kind=X_CALLNEST; xi->fn=fn; xi->var=NULL; ck_args(c,e,fn,0,xi); return fn->ret; }   /* a nested function calling itself */
            if(fn->origin) fn=fn->origin;                          /* (an instance naming itself) */
            if(fn->pristine && !fn->cls){ fn=pick_instance(c,e,fn,xi); if(!fn) return pending(c,e->line,"the arguments"); }
            if(!fn->cls && fn->mod && (!strcmp(fn->name,"loads") || !strcmp(fn->name,"load")) && module_sym(fn->mod,"_to_json"))
                return ck_text_loads(c,e,fn->mod,fn->name[4]==0);         /* (from tomllib import loads) */
            if(!fn->cls && fn->mod && (!strcmp(fn->name,"loads") || !strcmp(fn->name,"load")) && module_sym(fn->mod,"PReader"))
                return ck_pickle_loads(c,e,fn->mod,fn->name[4]==0);       /* (from pickle import loads) */
            if(!fn->cls && fn->mod && unicode_variant(c,e,fn->mod,fn->name)) return ck_call_inner(c,e);
            xi->kind=X_FUNC; xi->fn=fn; ck_args(c,e,fn,0,xi); return fn->ret; }
        if(s->kind==AS_CLASS){ AClass *k=(AClass*)s->p;
            if(k->mod && lit_variant(c,e,k->mod,k->name)) return ck_call_inner(c,e);   /* (from array import array; array("d")) */
            return ck_ctor(c,e,k); }
        if(s->kind==AS_SYS && !strcmp((const char*)s->p,"functools.reduce")) return ck_reduce(c,e);
        if(s->kind==AS_SYS && !strcmp((const char*)s->p,"functools.partial")) return ck_partial(c,e);
        if(s->kind==AS_SYS && !strcmp((const char*)s->p,"string.capwords")) return ck_capwords(c,e);
        if(s->kind==AS_SYS && !strncmp((const char*)s->p,"ctypes.",7)) return ck_ctypes_fn(c,e,(const char*)s->p+7);
        if(s->kind==AS_SYS && !strncmp((const char*)s->p,"json.",5)) return ck_json(c,e,(const char*)s->p+5);
        if(s->kind==AS_SYS && !strncmp((const char*)s->p,"plainpy.",8)) return ck_plainpy(c,e,(const char*)s->p+8);
        if(s->kind==AS_SYS && !strncmp((const char*)s->p,"dataclasses.",12)) return ck_dataclasses(c,e,(const char*)s->p+12);
        if(s->kind==AS_CFUNC) return ck_ccall(c,e,(ACFunc*)s->p);
        if(s->kind==AS_VAR){
            AVar *v=(AVar*)s->p, *r=var_root(v);
            if(r->fn_const){                                       /* a nested def called by its name */
                XInfo *fx=xinfo(f); fx->kind=X_VAR; fx->var=v; fx->ty=v->ty;
                xi->kind=X_CALLNEST; xi->fn=r->fn_const; xi->var=v; ck_args(c,e,r->fn_const,0,xi); return r->fn_const->ret; }
            return ck_callval(c,e,ck_expr(c,f));
        }
        ck_err(c,e->line,"'%s' is not callable",f->name);
    }
    if(f->kind==EXPR_INDEX){ AClass *k=class_expr(c,f->a);               /* Box[int](...), mod.Box[int](...): the instance for those types */
        if(k){ XInfo *cx=xinfo(e);
            if(k->generic && !cx->icls && !class_has_subclass(c,k)){ Ty *t=ty_find(type_of(c,f,e->line)); if(t->k==TY_OBJ && t->cls!=k) cx->icls=t->cls; }
            return ck_ctor(c,e,k); } }
    if(f->kind!=EXPR_ATTRIBUTE) return ck_callval(c,e,ck_expr(c,f));      /* (lambda x: ...)(1), fs[i](x), make()(x) */
    const char *m=f->name;
    if(is_sys(c,f->a)) return ck_sys(c,e,m);
    if(is_bmod(c,f->a,"asyncio")) return ck_asyncio(c,e,m);
    if(is_bmod(c,f->a,"ctypes")) return ck_ctypes_fn(c,e,m);
    if(is_bmod(c,f->a,"json")) return ck_json(c,e,m);
    if(is_bmod(c,f->a,"plainpy")) return ck_plainpy(c,e,m);
    if(is_bmod(c,f->a,"dataclasses")) return ck_dataclasses(c,e,m);
    if(is_bmod(c,f->a,"functools") && !strcmp(m,"partial")) return ck_partial(c,e);
    if(is_bmod(c,f->a,"string") && !strcmp(m,"capwords")) return ck_capwords(c,e);
    if(is_bmod(c,f->a,"thread")) return ck_thread(c,e,m);
    if(bmod(c,f->a)) return ck_bmod(c,e,bmod(c,f->a),m);
    if(f->a->kind==EXPR_NAME && !lookup(c,f->a->name)){             /* bytes.fromhex(...), int.from_bytes(...) */
        static const char *const cm[]={"bytes.fromhex","int.from_bytes",NULL};
        char q[64]; snprintf(q,sizeof q,"%s.%s",f->a->name,m);
        for(int i=0;cm[i];i++) if(!strcmp(q,cm[i])) return ck_builtin(c,e,cm[i]);
    }
    if(f->a->kind==EXPR_NAME){ ASym *s=lookup(c,f->a->name);       /* lib.f(...) */
        if(s && s->kind==AS_CLIB) return ck_ccall(c,e,cfunc_get(c->p,(ACLib*)s->p,m)); }
    if(f->a->kind==EXPR_ATTRIBUTE){ AModule *lm=module_expr(c,f->a->a);   /* module.lib.f(...) */
        ASym *s=lm?module_sym(lm,f->a->name):NULL;
        if(s && s->kind==AS_CLIB) return ck_ccall(c,e,cfunc_get(c->p,(ACLib*)s->p,m)); }
    AModule *mod=module_expr(c,f->a);
    if(mod){
        if(!strcmp(mod->name,"io") && !strcmp(m,"open")) return ck_open(c,e);   /* io.open is open */
        if((!strcmp(m,"loads") || !strcmp(m,"load")) && module_sym(mod,"_to_json")) return ck_text_loads(c,e,mod,m[4]==0);
        if((!strcmp(m,"loads") || !strcmp(m,"load")) && module_sym(mod,"PReader")) return ck_pickle_loads(c,e,mod,m[4]==0);
        if(unicode_variant(c,e,mod,m)) return ck_call_inner(c,e);           /* ET.tostring(e, encoding="unicode") */
        if(lit_variant(c,e,mod,m)) return ck_call_inner(c,e);               /* array.array("d", ...) */
        if(!strcmp(m,"open") && module_sym(mod,"_open_t") && module_sym(mod,"_open_b")){   /* gzip.open(name, mode): the text or the binary one */
            Expr *mode=NULL; int text=0, pos=0;
            for(int i=0;i<e->count;i++){ Expr *a=e->items[i]; if(!a->akind){ if(pos==1) mode=a; pos++; } else if(a->akind==3 && !strcmp(a->kw,"mode")) mode=a; }
            if(mode){ if(!(mode->kind==EXPR_LITERAL && mode->tok->kind==T_STRING && mode->tok->i!=2)) ck_err(c,e->line,"%s.open(): the mode must be a literal in compiled code",mod->name);
                text=strchr(mode->tok->text,'t')!=NULL; }
            f->name=text?"_open_t":"_open_b"; m=f->name; }
        ASym *s=module_sym(mod,m);
        if(!s) ck_err(c,e->line,"module %s has no attribute '%s'",mod->name,m);
        if(s->kind==AS_CFUNC) return ck_ccall(c,e,(ACFunc*)s->p);
        if(s->kind==AS_FUNC){ AFunc *fn=(AFunc*)s->p;
            if(fn->origin) fn=fn->origin;
            if(fn->pristine && !fn->cls){ fn=pick_instance(c,e,fn,xi); if(!fn) return pending(c,e->line,"the arguments"); }   /* (a generic function) */
            xi->kind=X_FUNC; xi->fn=fn; ck_args(c,e,fn,0,xi); return fn->ret; }
        if(s->kind==AS_CLASS) return ck_ctor(c,e,(AClass*)s->p);
        if(s->kind==AS_VAR) return ck_callval(c,e,ck_expr(c,f));
        ck_err(c,e->line,"%s.%s is not callable",mod->name,m);
    }
    if(class_expr(c,f->a)){
        ASym sx={0}, *s=&sx; sx.kind=AS_CLASS; sx.p=class_expr(c,f->a);
        if(s && s->kind==AS_CLASS){                       /* Class.method(obj, ...) / static method */
            AClass *cls=(AClass*)s->p; AFunc *fn=aot_find_method(cls,m);
            { AClass *in=aot_nested_class(c->p,cls,m); if(in && !fn) return ck_ctor(c,e,in); }        /* Outer.Inner(...) */
            if(!fn) ck_err(c,e->line,"class %s has no method '%s'",cls->name,m);
            if(fn->pristine && fn->is_static && !method_overridden(c,fn)){       /* a generic static method / classmethod: its copy for these arguments */
                AFunc *g=pick_instance(c,e,fn,xi); if(!g){ for(int i=0;i<e->count;i++) ck_expr(c,e->items[i]); return pending(c,e->line,"the arguments"); }
                fn=g; }
            if(!fn->is_static && !fn->is_clsm) fn->unbound_called=1;
            xi->kind=X_STATIC; xi->fn=fn; xi->cls=cls; ck_args(c,e,fn,0,xi); return fn->ret;
        }
    }
    if(f->a->kind==EXPR_CALL && f->a->a->kind==EXPR_NAME && !strcmp(f->a->a->name,"super") && !lookup(c,"super")){
        AFunc *cur=c->fn;
        if(!cur || !cur->cls || cur->is_static) ck_err(c,e->line,"super() is only available in methods");
        if(f->a->count) ck_err(c,e->line,"use super() without arguments");
        if(!cur->cls->base) ck_err(c,e->line,"class %s has no base class",cur->cls->name);
        AFunc *fn=aot_find_method(cur->cls->base,m);
        if(!fn && !strcmp(m,"__init__") && aot_is_exception(cur->cls->base)){
            if(e->count>1) ck_err(c,e->line,"%s.__init__() takes at most one argument (the message) in compiled code",cur->cls->base->name);
            for(int i=0;i<e->count;i++) no_void(c,ck_expr(c,e->items[i]),e->line);
            xi->kind=X_SUPER; xi->fn=NULL; xi->name="exc_init"; xi->cls=cur->cls->base; return TY_VOID_T;
        }
        if(!fn) ck_err(c,e->line,"base classes of %s have no method '%s'",cur->cls->name,m);
        if(fn->pristine && !fn->is_static){                                  /* a generic method of the base (TypeVar parameters): its copy for these arguments */
            AFunc *g=pick_instance(c,e,fn,xi);
            if(!g){ for(int i=0;i<e->count;i++) ck_expr(c,e->items[i]); return pending(c,e->line,"the arguments"); }
            fn=g; }
        xi->kind=X_SUPER; xi->fn=fn; xi->cls=cur->cls->base; ck_args(c,e,fn,fn->is_static?0:1,xi); return fn->ret;
    }
    Ty *t=ty_find(ck_expr(c,f->a));
    if(t->k==TY_VAR){ for(int i=0;i<e->count;i++) ck_expr(c,e->items[i]); return pending(c,e->line,"the object whose method is called"); }
    if(t->k==TY_OBJ){
        if(!strcmp(m,"group") && e->count>=2 && t->cls->mod && !strcmp(t->cls->mod->name,"re") && !strcmp(t->cls->name,"Match") && f->a->kind==EXPR_NAME){
            Expr *tup=xnew(EXPR_TUPLE,e->line);                              /* m.group(1, 2): (m.group(1), m.group(2)) */
            for(int i=0;i<e->count;i++){ Expr *one=xnew(EXPR_CALL,e->line); Expr *at=xnew(EXPR_ATTRIBUTE,e->line); at->a=f->a; at->name="group"; one->a=at; xpush(one,e->items[i]); xpush(tup,one); }
            void *keep=e->ty; int ak=e->akind; char *kw=e->kw; *e=*tup; e->ty=keep; e->akind=ak; e->kw=kw; xi->kind=X_NONE;
            return ck_expr_inner(c,e); }
        if(!strcmp(m,"findall") && t->cls->mod && !strcmp(t->cls->mod->name,"re") && !strcmp(t->cls->name,"Pattern") && strcmp(c->mod->name,"re")){
            int g=re_receiver_groups(c,f->a);                                 /* p.findall(s) for p = re.compile(LIT) */
            if(g==-2 && !c->strict){ for(int i=0;i<e->count;i++) ck_expr(c,e->items[i]); return pending(c,e->line,"the pattern"); }
            if(g>=2){ f->name="_findall_t"; m=f->name; } }
        if(!strcmp(m,"open") && aot_find_method(t->cls,"_open_w")){          /* zf.open(name, "w"): the writing one */
            Expr *mode=NULL; int pos=0;
            for(int i=0;i<e->count;i++){ Expr *a=e->items[i]; if(!a->akind){ if(pos==1) mode=a; pos++; } else if(a->akind==3 && !strcmp(a->kw,"mode")) mode=a; }
            if(mode && mode->kind==EXPR_LITERAL && mode->tok->kind==T_STRING && strchr(mode->tok->text,'w')){ f->name="_open_w"; m=f->name; } }
        AFunc *fn=aot_find_method(t->cls,m);
        if(!fn && (!strcmp(m,"assertIsInstance")||!strcmp(m,"assertNotIsInstance")) && aot_find_method(t->cls,"_isInstance")){
            /* self.assertIsInstance(obj, C, msg): self._isInstance(isinstance(obj, C), obj, "<class 'C'>", msg) */
            Expr *ao[3]={NULL,NULL,NULL}; int pos=0;
            for(int i=0;i<e->count;i++){ Expr *a=e->items[i];
                if(!a->akind && pos<3) ao[pos++]=a;
                else if(a->akind==3 && !strcmp(a->kw,"obj")) ao[0]=a; else if(a->akind==3 && !strcmp(a->kw,"cls")) ao[1]=a; else if(a->akind==3 && !strcmp(a->kw,"msg")) ao[2]=a;
                else ck_err(c,e->line,"%s(): unexpected argument",m); }
            if(!ao[0] || !ao[1]) ck_err(c,e->line,"%s() takes the object and the class",m);
            SrcBuf rep={0}; Expr *one[1]={ao[1]}; Expr **cs= ao[1]->kind==EXPR_TUPLE ? ao[1]->items : one; int ncs= ao[1]->kind==EXPR_TUPLE ? ao[1]->count : 1;
            if(ao[1]->kind==EXPR_TUPLE) sb_add(&rep,"(");
            for(int i=0;i<ncs;i++){ AClass *k=class_expr(c,cs[i]);
                if(i) sb_add(&rep,", ");
                if(k && !k->builtin) sb_add(&rep,"<class '%s.%s'>",k->mod==c->p->mods[0]?"__main__":k->mod->name,k->qualname?k->qualname:k->name);
                else if(k) sb_add(&rep,"<class '%s'>",k->name);
                else if(cs[i]->kind==EXPR_NAME) sb_add(&rep,"<class '%s'>",cs[i]->name);
                else ck_err(c,e->line,"%s(): the class must be named in compiled code",m); }
            if(ao[1]->kind==EXPR_TUPLE) sb_add(&rep,ncs==1?",)":")");
            Expr *isi=xnew(EXPR_CALL,e->line); isi->a=name_expr("isinstance",e->line); Expr *o0=ao[0], *c0=ao[1]; o0->akind=0; c0->akind=0;
            xpush(isi,o0); xpush(isi,c0);
            Expr *o2=expr_subst(ao[0],NULL,NULL); o2->akind=0;
            Expr *rl=lit_str(e->line,rep.s); free(rep.s);
            Expr *ml= ao[2] ? ao[2] : xnew(EXPR_NONE,e->line); ml->akind=0;
            e->count=0; xpush(e,isi); xpush(e,o2); xpush(e,rl); xpush(e,ml);
            f->kw=f->name;                                                   /* (its name in the source: for sys._site("frame")) */
            f->name= m[6]=='N' ? "_notIsInstance" : "_isInstance"; m=f->name; fn=aot_find_method(t->cls,m); }
        if(!fn && (!strcmp(m,"open")||!strcmp(m,"makefile"))){         /* pathlib's p.open(mode), sock.makefile(mode): */
            char nb[32], nt[32]; snprintf(nb,sizeof nb,"_%s_b",m); snprintf(nt,sizeof nt,"_%s_t",m);       /* the binary or the text one */
            if(aot_find_method(t->cls,nb) && aot_find_method(t->cls,nt)){
                Expr *mode=NULL; int text=1;
                for(int i=0;i<e->count;i++){ Expr *a=e->items[i]; if((!i && !a->akind) || (a->akind==3 && !strcmp(a->kw,"mode"))) mode=a; }
                if(mode){ if(!(mode->kind==EXPR_LITERAL && mode->tok->kind==T_STRING && mode->tok->i!=2)) ck_err(c,e->line,"%s(): the mode must be a literal in compiled code",m);
                    text=!strchr(mode->tok->text,'b'); }
                f->name=xstrdup2(text?nt:nb); m=f->name; fn=aot_find_method(t->cls,m); } }
        if(!fn){ AField *fd=aot_find_field(t->cls,m); TyKind fk=fd?ty_find(fd->ty)->k:TY_VOID;
            if(fd && (fk==TY_FUNC||fk==TY_VAR||fk==TY_OBJ||fk==TY_TYPE)) return ck_callval(c,e,ck_expr(c,f));   /* self.callback(x), self.cls(x) */
            if(fd) ck_err(c,e->line,"field %s.%s is not callable",t->cls->name,m);
            if(!c->strict){ for(int i=0;i<e->count;i++) ck_expr(c,e->items[i]); return pending(c,e->line,"the field"); }   /* (a class instance whose __init__ is not checked yet) */
            ck_err(c,e->line,"%s has no method '%s'",t->cls->name,m); }
        if(fn->decovar){                                        /* through Class.name (decorated): name(obj, args) */
            Ty *ft=ty_find(fn->decovar->ty);
            if(ft->k==TY_VAR){ for(int i=0;i<e->count;i++) ck_expr(c,e->items[i]); return pending(c,e->line,"the decorated method"); }
            if(ft->k!=TY_FUNC || ft->nelems-(ft->tup&1)-((ft->tup>>1)&1)<1) ck_err(c,e->line,"%s.%s is not a method after its decorators (%s)",t->cls->name,m,ty_name(ft));
            expect(c,ft->elems[0],t,e->line,"self");
            Ty view=*ft; view.elems=ft->elems+1; view.nelems--; view.link=NULL;
            Ty *r=ck_callval(c,e,&view);
            xi->kind=X_CALLDECO; xi->var=fn->decovar; xi->fn=fn;
            return r;
        }
        if(fn->pristine && !method_overridden(c,fn)){                        /* a generic method (TypeVar parameters): its copy for these arguments */
            AFunc *g=pick_instance(c,e,fn,xi); if(!g){ for(int i=0;i<e->count;i++) ck_expr(c,e->items[i]); return pending(c,e->line,"the arguments"); }
            fn=g; }
        xi->kind=fn->is_static?X_STATIC:X_METHOD; xi->fn=fn; xi->cls=t->cls;
        ck_args(c,e,fn,fn->is_static?0:1,xi);
        return fn->ret;
    }
    return ck_tmethod(c,e,t,m);
}

