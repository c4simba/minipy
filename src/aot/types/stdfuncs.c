/* ========================= Typed compiler, types: library functions =========================
   Calls of standard library functions the compiler rewrites (text /
   unicode variants, tomllib, json.dumps, dataclasses, functools.partial,
   string.capwords, threads, coroutines). */

#include "check.h"

static int text_variant(Ck *c, Expr *e, AModule *mod, const char *m);

int unicode_variant(Ck *c, Expr *e, AModule *mod, const char *m){
    if(text_variant(c,e,mod,m)) return 1;
    char un[96]; snprintf(un,sizeof un,"_%s_unicode",m);
    ASym *us=module_sym(mod,un);
    if(!us || us->kind!=AS_FUNC) return 0;
    Expr *enc=NULL; int pos=0;
    for(int i=0;i<e->count;i++){ Expr *a=e->items[i]; if(!a->akind){ if(pos==1) enc=a; pos++; } else if(a->akind==3 && !strcmp(a->kw,"encoding")) enc=a; }
    if(!enc || enc->kind!=EXPR_LITERAL || enc->tok->kind!=T_STRING || enc->tok->i==2) return 0;
    char low[32]; snprintf(low,sizeof low,"%s",enc->tok->text); for(char *q=low;*q;q++) *q=(char)tolower((unsigned char)*q);
    if(strcmp(low,"unicode")) return 0;
    char hn[160]; snprintf(hn,sizeof hn," %s.%s",mod->name,un);
    if(!module_sym(c->mod,hn)) symtab_add(&c->mod->syms,xstrdup2(hn),AS_FUNC,us->p);
    e->a=name_expr(module_sym(c->mod,hn)->name,e->line);
    return 1;
}
/* f(..., text=True) (or universal_newlines=True, encoding= / errors= a str) of a module with _f_text
   (subprocess's run, check_output): that one (str results, not bytes); 1: done */
static int text_variant(Ck *c, Expr *e, AModule *mod, const char *m){
    char tn[96]; snprintf(tn,sizeof tn,"_%s_text",m);
    ASym *ts=module_sym(mod,tn);
    if(!ts || ts->kind!=AS_FUNC) return 0;
    int text=0;
    for(int i=0;i<e->count;i++){ Expr *a=e->items[i]; if(a->akind!=3) continue;
        if((!strcmp(a->kw,"text") || !strcmp(a->kw,"universal_newlines")) && a->kind==EXPR_TRUE) text=1;
        if((!strcmp(a->kw,"encoding") || !strcmp(a->kw,"errors")) && a->kind==EXPR_LITERAL && a->tok->kind==T_STRING && a->tok->i!=2) text=1; }
    if(!text) return 0;
    char hn[160]; snprintf(hn,sizeof hn," %s.%s",mod->name,tn);
    if(!module_sym(c->mod,hn)) symtab_add(&c->mod->syms,xstrdup2(hn),AS_FUNC,ts->p);
    e->a=name_expr(module_sym(c->mod,hn)->name,e->line);
    return 1;
}
/* f("x", ...) with a literal first argument, of a module with _f_x (array's array("d", ...)): that one
   (the literal decides the types, e.g. of the items); 1: done */
int lit_variant(Ck *c, Expr *e, AModule *mod, const char *m){
    Expr *a0=e->count?e->items[0]:NULL;
    if(!a0 || a0->akind || a0->kind!=EXPR_LITERAL || a0->tok->kind!=T_STRING || a0->tok->i==2) return 0;
    const char *lit=a0->tok->text; size_t n=strlen(lit);
    if(n<1 || n>8) return 0;
    for(size_t i=0;i<n;i++) if(!isalnum((unsigned char)lit[i]) && lit[i]!='_') return 0;
    ASym *ms=module_sym(mod,m);
    if(ms && ms->kind==AS_CLASS && ((AClass*)ms->p)->mod==mod) m=((AClass*)ms->p)->name;   /* (an alias of the class: ArrayType) */
    char vn[96]; snprintf(vn,sizeof vn,"_%s_%s",m,lit);
    ASym *vs=module_sym(mod,vn);
    if(!vs || vs->kind!=AS_FUNC) return 0;
    char hn[160]; snprintf(hn,sizeof hn," %s.%s",mod->name,vn);
    if(!module_sym(c->mod,hn)) symtab_add(&c->mod->syms,xstrdup2(hn),AS_FUNC,vs->p);
    e->a=name_expr(module_sym(c->mod,hn)->name,e->line);
    return 1;
}
/* tomllib.loads(s) / tomllib.load(fp) (a module with _to_json(s) -> JSON text): json.loads(tomllib._to_json(s)),
   json.loads(tomllib._to_json(tomllib._load_text(fp))): the value of the type wanted */
Ty *ck_text_loads(Ck *c, Expr *e, AModule *mod, int load){
    int line=e->line;
    if(e->count!=1 || e->items[0]->akind) ck_err(c,line,"%s.%s() takes the %s (only) in compiled code",mod->name,load?"load":"loads",load?"file":"text");
    char hn[96];
    snprintf(hn,sizeof hn," %s._to_json",mod->name);
    if(!module_sym(c->mod,hn)){ ASym *s=module_sym(mod,"_to_json"); symtab_add(&c->mod->syms,xstrdup2(hn),s->kind,s->p); }
    Expr *tj=xnew(EXPR_CALL,line); tj->a=name_expr(module_sym(c->mod,hn)->name,line); xpush(tj,e->items[0]);
    if(load){ ASym *ls=module_sym(mod,"_load_text"); if(!ls) ck_err(c,line,"%s.load() is not available in compiled code",mod->name);
        snprintf(hn,sizeof hn," %s._load_text",mod->name);
        if(!module_sym(c->mod,hn)) symtab_add(&c->mod->syms,xstrdup2(hn),ls->kind,ls->p);
        Expr *lt=xnew(EXPR_CALL,line); lt->a=name_expr(module_sym(c->mod,hn)->name,line); xpush(lt,e->items[0]); tj->items[0]=lt; }
    e->items[0]=tj;
    if(!module_sym(c->mod," json.loads")) symtab_add(&c->mod->syms," json.loads",AS_SYS,(void*)"json.loads");
    e->a=name_expr(module_sym(c->mod," json.loads")->name,line);
    return ck_json_loads(c,e);
}
/* json.dumps(obj, separators=(item, key), ensure_ascii=bool) */
Ty *ck_json(Ck *c, Expr *e, const char *m){
    XInfo *xi=xinfo(e); xi->kind=X_BMOD; int line=e->line;
    if(!strcmp(m,"loads")) return ck_json_loads(c,e);
    if(!strcmp(m,"load")){                                          /* json.load(f): json.loads(f.read()) */
        if(e->count!=1 || e->items[0]->akind) ck_err(c,line,"json.load() takes the file (only) in compiled code");
        Expr *at=xnew(EXPR_ATTRIBUTE,line); at->a=e->items[0]; at->name="read"; Expr *rd=xnew(EXPR_CALL,line); rd->a=at; e->items[0]=rd;
        if(e->a->kind==EXPR_ATTRIBUTE) e->a->name="loads";              /* (done once: json.loads from now on) */
        else { if(!module_sym(c->mod," json.loads")) symtab_add(&c->mod->syms," json.loads",AS_SYS,(void*)"json.loads");
            e->a=name_expr(module_sym(c->mod," json.loads")->name,line); }
        return ck_json_loads(c,e); }
    if(!strcmp(m,"dump")){                                          /* json.dump(obj, fp, ...): fp.write(json.dumps(obj, ...)) */
        if(e->count<2 || e->items[0]->akind || e->items[1]->akind) ck_err(c,line,"json.dump() takes the value and the file");
        Expr *fp=e->items[1];
        Expr *du=xnew(EXPR_CALL,line);
        if(e->a->kind==EXPR_ATTRIBUTE){ du->a=xnew(EXPR_ATTRIBUTE,line); du->a->a=e->a->a; du->a->name="dumps"; }
        else { if(!module_sym(c->mod," json.dumps")) symtab_add(&c->mod->syms," json.dumps",AS_SYS,(void*)"json.dumps");   /* (from json import dump) */
            du->a=name_expr(module_sym(c->mod," json.dumps")->name,line); }
        for(int i=0;i<e->count;i++) if(i!=1) xpush(du,e->items[i]);
        Expr *at=xnew(EXPR_ATTRIBUTE,line); at->a=fp; at->name="write";
        void *keep=e->ty; memset(e,0,sizeof *e); e->kind=EXPR_CALL; e->line=line; e->a=at; e->ty=keep; xpush(e,du);
        xi->kind=X_NONE; ck_call_inner(c,e); return TY_VOID_T; }
    if(strcmp(m,"dumps")) ck_err(c,line,"json.%s is not available in compiled code (json.dumps and json.loads are)",m);
    xi->name="json.dumps"; xi->key=NULL; xi->argmap[0]=1;
    int npos=0;
    for(int i=0;i<e->count;i++){ Expr *a=e->items[i];
        if(a->akind==0){ if(npos++) ck_err(c,line,"json.dumps() takes one positional argument"); continue; }
        if(a->akind!=3) ck_err(c,line,"json.dumps(): *args / **kwargs are not supported");
        if(!strcmp(a->kw,"separators")){
            if(a->kind!=EXPR_TUPLE || a->count!=2 || a->items[0]->kind!=EXPR_LITERAL || a->items[0]->tok->kind!=T_STRING || a->items[1]->kind!=EXPR_LITERAL || a->items[1]->tok->kind!=T_STRING)
                ck_err(c,line,"json.dumps(): separators must be a tuple of two string literals");
            xi->key=a; ck_expr(c,a);
        } else if(!strcmp(a->kw,"ensure_ascii")){
            if(a->kind!=EXPR_TRUE && a->kind!=EXPR_FALSE) ck_err(c,line,"json.dumps(): ensure_ascii must be True or False");
            xi->argmap[0]=a->kind==EXPR_TRUE; ck_expr(c,a);
        } else if(!strcmp(a->kw,"indent") || !strcmp(a->kw,"sort_keys")){
            if(a->kind!=EXPR_NONE && a->kind!=EXPR_FALSE) ck_err(c,line,"json.dumps(): %s is not supported in compiled code",a->kw);
            if(a->kind==EXPR_NONE) unify(c,ck_expr(c,a),TY_VOID_T); else ck_expr(c,a);
        } else ck_err(c,line,"json.dumps(): '%s' is not supported in compiled code",a->kw);
    }
    if(npos!=1 || e->items[0]->akind) ck_err(c,line,"json.dumps() takes the value to write first");
    Expr *v=e->items[0];
    Ty *t=ck_expr(c,v);
    if(v->kind==EXPR_NONE) unify(c,t,TY_VOID_T);
    json_ok(c,t,line,0);
    return TY_STR_T;
}
/* minipy.endpoint(f): f as an Endpoint (a function: its adapter) */
Ty *ck_minipy(Ck *c, Expr *e, const char *m){
    XInfo *xi=xinfo(e); xi->kind=X_BMOD; int line=e->line;
    if(strcmp(m,"endpoint")) ck_err(c,line,"minipy.%s is not available",m);
    xi->name="minipy.endpoint";
    ck_positional(c,e,1,1,"endpoint");
    Ty *want=endpoint_type();
    c->ep_value++; Ty *t=ck_expr(c,e->items[0]); c->ep_value--;
    expect(c,want,t,line,"the argument of minipy.endpoint()");
    ep_arg(c,want,e->items[0],line);
    return want;
}
Ty *ck_call(Ck *c, Expr *e){
    Ty *t=ck_call_inner(c,e);
    XInfo *xi=xinfo(e);
    if((xi->kind==X_FUNC||xi->kind==X_METHOD||xi->kind==X_STATIC||xi->kind==X_SUPER||xi->kind==X_CALLNEST) && xi->fn && xi->fn->is_async && !xi->fn->is_gen && c->coro_ok!=e && !(c->fn && c->fn->calls_coroutines))
        ck_err(c,e->line,"%s() is a coroutine: await it, or pass it to asyncio.create_task/gather/run",xi->fn->name);
    return t;
}
/* x must be a call of an async function: its result type */
Ty *ck_coro(Ck *c, Expr *x){
    if(x->kind!=EXPR_CALL) ck_err(c,x->line,"expected a coroutine call (a call of an async function)");
    Expr *save=c->coro_ok; c->coro_ok=x;
    Ty *t=ck_expr(c,x);
    c->coro_ok=save;
    XInfo *xi=xinfo(x);
    if(xi->kind==X_CALLNEST && xi->fn && xi->fn->is_async) ck_err(c,x->line,"a nested coroutine can only be awaited in compiled code (define %s() at module level to make a task of it)",xi->fn->name);
    if(!((xi->kind==X_FUNC||xi->kind==X_METHOD||xi->kind==X_STATIC||xi->kind==X_SUPER) && xi->fn && xi->fn->is_async))
        ck_err(c,x->line,"expected a coroutine call (a call of an async function)");
    return t;
}
/* dataclasses.replace / asdict / astuple / is_dataclass: rewritten into the class's constructor
   and methods (the rewritten call is checked from then on) */
AVar *named_hidden(Ck *c, const char *base, char *out, size_t n){       /* a hidden variable names can refer to */
    snprintf(out,n,".%s%d",base,++c->nhidden);
    if(is_func(c->fn)){ AVar *v=hidden_local(c->fn,out,NULL); symtab_add(&c->fn->locals,out,AS_VAR,v); return v; }
    AVar *v=new_global(c,c->mod,out); symtab_add(&c->mod->syms,out,AS_VAR,v); return v;
}
static AClass *dc_class_of(AClass *k){ for(;k;k=k->base) if(k->dc) return k; return NULL; }
Ty *ck_dataclasses(Ck *c, Expr *e, const char *m){
    int line=e->line;
    if(!strcmp(m,"field")) ck_err(c,line,"field() belongs in a dataclass's body");
    if(!strcmp(m,"fields")) ck_err(c,line,"dataclasses.fields() is not supported in compiled code");
    if(strcmp(m,"replace") && strcmp(m,"asdict") && strcmp(m,"astuple") && strcmp(m,"is_dataclass")) ck_err(c,line,"dataclasses.%s() is not supported in compiled code",m);
    if(!e->count || e->items[0]->akind) ck_err(c,line,"%s() needs the object first",m);
    Expr *obj=e->items[0];
    Ty *t=ty_find(ck_expr(c,obj));
    if(t->k==TY_VAR) return pending(c,line,"the argument");
    if(!strcmp(m,"is_dataclass")){                                 /* known from the type */
        int yes= t->k==TY_OBJ ? dc_class_of(t->cls)!=NULL : t->k==TY_TYPE && xinfo(obj)->kind==X_TYPEVAL && xinfo(obj)->cls ? dc_class_of(xinfo(obj)->cls)!=NULL : 0;
        Expr *k=xnew(yes?EXPR_TRUE:EXPR_FALSE,line); void *keep=e->ty; *e=*k; e->ty=keep; return TY_BOOL_T; }
    AClass *dcc= t->k==TY_OBJ ? dc_class_of(t->cls) : NULL;
    if(!dcc) ck_err(c,line,"%s() should be called on dataclass instances",m);
    if(!strcmp(m,"asdict")||!strcmp(m,"astuple")){                  /* obj.__mp_asdict__() */
        if(e->count!=1) ck_err(c,line,"%s(): only the object is supported in compiled code",m);
        Expr *at=xnew(EXPR_ATTRIBUTE,line); at->a=obj; at->name=m[2]=='d'?"__mp_asdict__":"__mp_astuple__";
        e->a=at; e->count=0;
        return ck_call_inner(c,e); }
    struct DcInfo *dc=dcc->dc;                                       /* replace(obj, f=v): Class(f=v, g=obj.g, ...) */
    for(int i=1;i<e->count;i++){ Expr *a=e->items[i];
        if(a->akind!=3) ck_err(c,line,"replace() takes the changes as keyword arguments");
        for(int k=0;k<dc->n;k++) if(!strcmp(dc->f[k].name,a->kw) && !dc->f[k].init)
            ck_err(c,line,"field %s is declared with init=False, it cannot be specified with replace()",a->kw); }
    Expr *src=obj; int pure= obj->kind==EXPR_NAME || (obj->kind==EXPR_ATTRIBUTE && obj->a->kind==EXPR_NAME);
    char h[64]; if(!pure) named_hidden(c,"replace",h,sizeof h);
    Expr *call=xnew(EXPR_CALL,line); call->a=name_expr(t->cls->symname,line);
    int first=1;
    for(int k=0;k<dc->n;k++){ DcField *f=&dc->f[k]; if(!f->init) continue;
        Expr *v=NULL; for(int i=1;i<e->count;i++) if(!strcmp(e->items[i]->kw,f->name)) v=e->items[i];
        if(!v){
            if(f->initvar) ck_err(c,line,"InitVar '%s' must be specified with replace()",f->name);
            Expr *o;
            if(pure) o=src;
            else if(first){ o=xnew(EXPR_WALRUS,line); o->name=xstrdup2(h); o->a=src; o->b=name_expr(xstrdup2(h),line); first=0; }
            else o=name_expr(xstrdup2(h),line);
            Expr *at=xnew(EXPR_ATTRIBUTE,line); at->a=o; at->name=(char*)f->name;
            v=at; }
        Expr *arg=MPY_NEW0(Expr); *arg=*v; arg->akind=3; arg->kw=(char*)f->name;
        xpush(call,arg); }
    for(int i=1;i<e->count;i++){ int known=0; for(int k=0;k<dc->n;k++) if(!strcmp(dc->f[k].name,e->items[i]->kw)) known=1;
        if(!known) ck_err(c,line,"%s.__init__() got an unexpected keyword argument '%s'",t->cls->name,e->items[i]->kw); }
    { void *keep=e->ty; *e=*call; e->ty=keep; }
    return ck_call_inner(c,e);
}
/* minipy's thread module: start(fn[, args]) runs fn(*args) as a task; join, lock, acquire, release, sleep */
Ty *ck_thread(Ck *c, Expr *e, const char *m){
    XInfo *xi=xinfo(e); int line=e->line;
    xi->kind=X_BUILTIN;
    for(int i=0;i<e->count;i++) if(e->items[i]->akind) ck_err(c,line,"thread.%s() takes positional arguments",m);
    if(!strcmp(m,"start")){
        if(e->count<1 || e->count>2) ck_err(c,line,"thread.start() takes a function and a tuple of its arguments");
        if(!xi->key){ Expr *call=xnew(EXPR_CALL,line); call->a=e->items[0];
            if(e->count==2){ Expr *t=e->items[1]; if(t->kind!=EXPR_TUPLE) ck_err(c,line,"thread.start() needs its arguments as a tuple literal in compiled code");
                for(int i=0;i<t->count;i++) xpush(call,t->items[i]); }
            xi->key=call; }
        Ty *r=ck_expr(c,xi->key); XInfo *cx=xinfo(xi->key);
        if(!((cx->kind==X_FUNC||cx->kind==X_METHOD||cx->kind==X_STATIC) && cx->fn) || cx->fn->is_async || cx->fn->is_gen)
            ck_err(c,line,"thread.start() needs a function defined with def (at module level) in compiled code");
        xi->name="thread.start"; return ty_new(TY_TASK,r,NULL); }
    if(!strcmp(m,"join")){ ck_positional(c,e,1,1,"join"); Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) return pending(c,line,"the thread");
        if(t->k!=TY_TASK) ck_err(c,line,"thread.join() takes what thread.start() gave, not %s",ty_name(t)); xi->name="thread.join"; return TY_VOID_T; }
    if(!strcmp(m,"lock")){ ck_positional(c,e,0,0,"lock"); xi->name="thread.lock"; return TY_INT_T; }
    if(!strcmp(m,"acquire")||!strcmp(m,"release")){ ck_positional(c,e,1,1,m); expect(c,TY_INT_T,arg(c,e,0),line,"a lock"); xi->name=m[0]=='a'?"thread.acquire":"thread.release"; return TY_VOID_T; }
    if(!strcmp(m,"sleep")){ ck_positional(c,e,1,1,"sleep"); Ty *t=arg(c,e,0); if(!numeric(t)) ck_err(c,line,"thread.sleep() takes a number of seconds"); xi->name="thread.sleep"; return TY_VOID_T; }
    if(!strcmp(m,"current")){ ck_positional(c,e,0,0,"current"); xi->name="thread.current"; return TY_INT_T; }   /* (the running task: 0 the main program) */
    ck_err(c,line,"thread.%s is not available in compiled code",m);
}
/* string.capwords(s, sep=None): (sep or ' ').join(w.capitalize() for w in s.split(sep)) */
Ty *ck_capwords(Ck *c, Expr *e){
    if(e->count<1 || e->count>2) ck_err(c,e->line,"capwords() takes 1 or 2 arguments");
    for(int i=0;i<e->count;i++) if(e->items[i]->akind && !(e->items[i]->akind==3 && !strcmp(e->items[i]->kw,"sep"))) ck_err(c,e->line,"capwords() takes s and sep");
    Expr *sep= e->count==2 && e->items[1]->kind!=EXPR_NONE ? e->items[1] : NULL;
    Expr *r=py_front_expr(c->mod->unit->path,sep?"(0).join([__mpy_w.capitalize() for __mpy_w in (0).split(0)])":"\" \".join([__mpy_w.capitalize() for __mpy_w in (0).split()])",e->line);
    if(!r) ck_err(c,e->line,"internal error: capwords");
    Expr *it=r->items[0]->clauses[0].iter;
    it->a->a=e->items[0]; if(sep){ Expr *s2=MPY_NEW0(Expr); *s2=*sep; s2->kw=NULL; s2->akind=0; it->items[0]=s2; r->a->a=sep; sep->kw=NULL; sep->akind=0; }
    void *keep=e->ty; *e=*r; e->ty=keep;
    return ck_expr(c,e);
}
/* functools.partial(f, a, k=b): lambda x: f(a, x, k=b), its fixed arguments (and f) evaluated now as defaults */
Ty *ck_partial(Ck *c, Expr *e){
    int line=e->line;
    if(!e->count || e->items[0]->akind) ck_err(c,line,"partial() needs the function first");
    Expr *fx=e->items[0]; Ty *ft=ty_find(ck_expr(c,fx));
    if(ft->k==TY_VAR) return pending(c,line,"the function of partial()");
    if(ft->k!=TY_FUNC || ft->tup) ck_err(c,line,"partial() needs a function with fixed parameters, not %s",ty_name(ft));
    AFunc *fn= xinfo(fx)->kind==X_FUNCREF ? xinfo(fx)->fn : NULL;
    int np=ft->nelems; Expr *bound[16]={0}; int pos=0;
    if(np>16) ck_err(c,line,"too many parameters");
    for(int i=1;i<e->count;i++){ Expr *a=e->items[i];
        if(a->akind==3){ int p=-1; if(fn && fn->def) for(int k=0;k<fn->nparams;k++) if(!strcmp(fn->def->params[k],a->kw)) p=k;
            if(p<0) ck_err(c,line,"partial(): keyword argument '%s' needs a function defined with that parameter",a->kw);
            Expr *v=MPY_NEW0(Expr); *v=*a; v->kw=NULL; v->akind=0; bound[p]=v; }
        else if(a->akind) ck_err(c,line,"partial(*args) is not supported in compiled code");
        else { while(pos<np && bound[pos]) pos++; if(pos>=np) ck_err(c,line,"partial(): too many arguments"); bound[pos++]=a; } }
    Expr *call=xnew(EXPR_CALL,line); call->a=name_expr("__mpy_pf",line);
    char **ps=MPY_NEW_ARR(char*,np+2); Expr **ds=MPY_NEW_ARR(Expr*,np+2); int n=0;
    for(int i=0;i<np;i++) if(!bound[i]){ char v[24]; snprintf(v,sizeof v,"__mpy_p%d",i); ps[n++]=xstrdup2(v); }
    int nfree=n;
    ps[n]=xstrdup2("__mpy_pf"); ds[n++]=fx;
    for(int i=0;i<np;i++){ char v[24];
        if(bound[i]){ snprintf(v,sizeof v,"__mpy_b%d",i); ps[n]=xstrdup2(v); ds[n++]=bound[i]; }
        else snprintf(v,sizeof v,"__mpy_p%d",i);
        xpush(call,name_expr(xstrdup2(v),line)); }
    (void)nfree;
    void *keep=e->ty; memset(e,0,sizeof *e);
    e->kind=EXPR_LAMBDA; e->line=line; e->ty=keep; e->eparams=ps; e->neparam=n; e->edefaults=ds; e->estar=e->edstar=-1; e->ekwonly=n; e->a=call;
    return ck_expr(c,e);
}
