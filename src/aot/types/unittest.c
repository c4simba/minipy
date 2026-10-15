/* ========================= Typed compiler, types: unittest =========================
   The TestCase subclasses of the program and their test methods: the
   test runner is generated. */

#include "check.h"

/* ---- unittest in compiled programs: the TestCase subclasses of the program and their test methods
   are known here; functions written for them (in unittest's namespace) make the tests and run them by
   name: __mpy_ut_tests(main_only), __mpy_ut_call(t, name), __mpy_ut_skip / _xfail / _doc / _fixture /
   _class / _module / _modfix. A skip decorator's expression is evaluated in the module it is written in
   (a function __ut_s<class id>(name, own) there). */
static int g_ut_done;
/* @skip(...) / @skipIf(...) / @skipUnless(...) / @expectedFailure (unittest's, or their bare names) */
const char *ut_deco_name(const char *src, Expr *d){
    Expr *f= d->kind==EXPR_CALL ? d->a : d;
    const char *n= f->kind==EXPR_NAME ? f->name : f->kind==EXPR_ATTRIBUTE ? f->name : NULL;
    if(!n) return NULL;
    if(f->kind==EXPR_ATTRIBUTE && !(f->a->kind==EXPR_NAME && !strcmp(f->a->name,"unittest"))) return NULL;
    if(f->kind==EXPR_NAME && !(src && strstr(src,"from unittest import"))) return NULL;
    static const char *const ks[]={"skip","skipIf","skipUnless","expectedFailure",NULL};
    for(int i=0;ks[i];i++) if(!strcmp(n,ks[i])) return (d->kind==EXPR_CALL)==(i<3) ? ks[i] : NULL;
    return NULL;
}
static void subst_stmts(Stmt **b, int n, const char *var, Expr *with){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->expr) s->expr=expr_subst(s->expr,var,with);
        if(s->expr2) s->expr2=expr_subst(s->expr2,var,with);
        if(s->value) s->value=expr_subst(s->value,var,with);
        subst_stmts(s->body,s->body_count,var,with); subst_stmts(s->orelse,s->orelse_count,var,with); }
}
static int ut_depth(AClass *k){ int d=0; for(;k;k=k->base) d++; return d; }
static int ut_mro(AClass *k, AClass **out, int max){
    int n=0;
    if(k->nmro>0){ for(int i=0;i<k->nmro && n<max;i++) out[n++]=k->mro[i]; }
    else for(;k && n<max;k=k->base) out[n++]=k;
    return n;
}
static AFunc *ut_own_method(AClass *k, const char *name){ for(int i=0;i<k->nmethods;i++) if(!strcmp(k->methods[i]->name,name)) return k->methods[i]; return NULL; }
static const char *ut_docstring(Stmt *def){
    if(!def || def->body_count<1) return NULL; Stmt *b=def->body[0];
    if(b->kind!=STMT_EXPR || !b->expr || b->expr->kind!=EXPR_LITERAL || b->expr->tok->kind!=T_STRING || b->expr->tok->i==2) return NULL;
    return b->expr->tok->text;
}
static void ut_str_lit(SrcBuf *o, const char *t){
    sb_add(o,"\"");
    for(const unsigned char *p=(const unsigned char*)t;*p;p++){
        if(*p=='"'||*p=='\\') sb_add(o,"\\%c",*p); else if(*p<32) sb_add(o,"\\x%02x",*p); else sb_add(o,"%c",*p); }
    sb_add(o,"\"");
}
static int ut_cmp_name(const void *a, const void *b){ return strcmp(*(const char*const*)a,*(const char*const*)b); }
/* the names of k's test methods (its own and inherited), sorted */
static int ut_test_names(AClass *tc, AClass *k, const char **names, int max){
    AClass *mro[32]; int nm=ut_mro(k,mro,32), n=0;
    for(int j=0;j<nm;j++){ if(mro[j]==tc || !aot_subclass(mro[j],tc)) continue;
        for(int i=0;i<mro[j]->nmethods;i++){ AFunc *f=mro[j]->methods[i];
            if(strncmp(f->name,"test",4) || f->is_static || f->is_property) continue;
            int dup=0; for(int q=0;q<n;q++) if(!strcmp(names[q],f->name)) dup=1;
            if(!dup && n<max) names[n++]=f->name; } }
    qsort(names,(size_t)n,sizeof *names,ut_cmp_name);
    return n;
}
/* compile functions of source src in a copy of module base's namespace; each def registered as a built-in name */
static void ut_compile(Ck *c, AModule *base, const char *src, Expr **subst, int nsubst, int line, AClass **ks, int nk){
    AotUnit *u=MPY_NEW0(AotUnit); u->name=base->unit->name; u->path=base->unit->path; u->src=xstrdup2(src);
    if(getenv("MPY_UTDEBUG")) fprintf(stderr,"---- (in %s)\n%s",base->name,src);
    u->ast=py_front(u->path,u->src);
    if(!u->ast){ if(getenv("MPY_UTDEBUG")) fprintf(stderr,"%s",src); ck_err(c,line,"internal error: the unittest functions do not compile"); }
    for(int k=0;k<nsubst;k++){ char nm[32]; snprintf(nm,sizeof nm,"__ut_x%d",k); subst_stmts(u->ast->body,u->ast->body_count,nm,subst[k]); }
    AModule *m=MPY_NEW0(AModule); *m=*base; m->unit=u;
    m->syms.n=base->syms.n; m->syms.cap=base->syms.n+8; m->syms.v=(ASym*)xmalloc(sizeof(ASym)*(size_t)m->syms.cap);
    memcpy(m->syms.v,base->syms.v,sizeof(ASym)*(size_t)base->syms.n);
    for(int i=0;i<nk;i++){ ASym *have=symtab_find(&m->syms,ks[i]->symname);       /* (isinstance(t, C) narrows t by C's name) */
        if(have && (have->kind!=AS_CLASS || have->p!=ks[i])) ck_err(c,ks[i]->def?ks[i]->def->line:line,"a test class cannot be named '%s' in compiled code (a name of unittest's)",ks[i]->symname);
        if(!have) symtab_add(&m->syms,ks[i]->symname,AS_CLASS,ks[i]); }
    AModule *save=c->mod; AFunc *savef=c->fn; c->mod=m;
    for(int i=0;i<u->ast->body_count;i++){ Stmt *d=u->ast->body[i];
        if(d->kind!=STMT_FUNCTION_DEF) continue;
        for(int k=0;k<d->body_count;k++) d->body[k]->line=line;
        AFunc *f=new_func(c,NULL,m,NULL,d,NULL); f->ncalls++;
        symtab_add(&m->syms,d->name,AS_FUNC,f);
        symtab_add(&builtin_syms,d->name,AS_FUNC,f); }
    c->mod=save; c->fn=savef;
    c->changed=1;
}
void ut_generate(Ck *c, int line){
    if(g_ut_done) return;
    g_ut_done=1;
    AProg *p=c->p;
    AModule *ut=find_module(p,"unittest");
    ASym *ts= ut ? module_sym(ut,"TestCase") : NULL;
    if(!ts || ts->kind!=AS_CLASS) ck_err(c,line,"internal error: unittest.TestCase not found");
    AClass *tc=(AClass*)ts->p;
    AClass **ks=NULL; int nk=0;                                 /* the test classes, deepest first */
    for(int i=0;i<p->nclasses;i++){ AClass *k=p->classes[i];
        if(k==tc || k->origin || k->builtin || k->encl || k->mod==ut || !aot_subclass(k,tc)) continue;
        ks=(AClass**)xrealloc(ks,sizeof(AClass*)*(size_t)(nk+1)); ks[nk++]=k; }
    for(int i=1;i<nk;i++) for(int j=i;j>0 && ut_depth(ks[j])>ut_depth(ks[j-1]);j--){ AClass *t=ks[j]; ks[j]=ks[j-1]; ks[j-1]=t; }
    SrcBuf o={0};
    for(int i=0;i<nk;i++){ char nm[32]; snprintf(nm,sizeof nm,"__ut_k%d",ks[i]->id); symtab_add(&builtin_syms,xstrdup2(nm),AS_CLASS,ks[i]); }
    /* the skip decorators: a function per class that has some, in its module */
    int *has_s=(int*)calloc((size_t)(nk>0?nk:1),sizeof(int));
    for(int i=0;i<nk;i++){ AClass *k=ks[i]; AotUnit *ku=k->mod->unit; const char *ksrc=ku?ku->src:NULL;
        Expr **sub=NULL; int ns=0; SrcBuf f={0}; int any=0;
        sb_add(&f,"def __ut_s%d(name: str, own: bool) -> str | None:\n",k->id);
        if(k->def) for(int d=0;d<k->def->decorator_count;d++){ Expr *de=aot_expr(ku,&k->def->decorator_exprs[d]); const char *dn=ut_deco_name(ksrc,de);
            if(!dn || !strcmp(dn,"expectedFailure")) continue;
            sub=(Expr**)xrealloc(sub,sizeof(Expr*)*(size_t)(ns+1)); sub[ns]=de;
            sb_add(&f,"    s = __ut_x%d\n    if s.reason is not None:\n        return s.reason\n",ns); ns++; any=1; }
        sb_add(&f,"    if own:\n        pass\n");
        for(int mi=0;mi<k->nmethods;mi++){ AFunc *fm=k->methods[mi]; if(strncmp(fm->name,"test",4) || !fm->def) continue;
            int first=1;
            for(int d=0;d<fm->def->decorator_count;d++){ Expr *de=aot_expr(ku,&fm->def->decorator_exprs[d]); const char *dn=ut_deco_name(ksrc,de);
                if(!dn || !strcmp(dn,"expectedFailure")) continue;
                if(first){ sb_add(&f,"        if name == \"%s\":\n            pass\n",fm->name); first=0; }
                sub=(Expr**)xrealloc(sub,sizeof(Expr*)*(size_t)(ns+1)); sub[ns]=de;
                sb_add(&f,"            s = __ut_x%d\n            if s.reason is not None:\n                return s.reason\n",ns); ns++; any=1; } }
        sb_add(&f,"    return None\n");
        if(any){ ut_compile(c,k->mod,f.s,sub,ns,line,NULL,0); has_s[i]=1; }
        free(f.s); free(sub); }
    /* the tests: those of the main module's classes (in the order of their names there), then the others' */
    AModule *mainm=p->mods[0];
    sb_add(&o,"def __mpy_ut_tests(main_only: bool) -> list[TestCase]:\n    out: list[TestCase] = []\n");
    { const char **sn=NULL; AClass **sk=NULL; int nsn=0;
      for(int q=0;q<mainm->syms.n;q++){ ASym *sy=&mainm->syms.v[q]; if(sy->kind!=AS_CLASS || sy->name[0]==' ') continue;
          for(int i=0;i<nk;i++) if(ks[i]==(AClass*)sy->p){ sn=(const char**)xrealloc(sn,sizeof(char*)*(size_t)(nsn+1)); sk=(AClass**)xrealloc(sk,sizeof(AClass*)*(size_t)(nsn+1)); sn[nsn]=sy->name; sk[nsn]=ks[i]; nsn++; } }
      for(int a=1;a<nsn;a++) for(int b=a;b>0 && strcmp(sn[b],sn[b-1])<0;b--){ const char *t=sn[b]; sn[b]=sn[b-1]; sn[b-1]=t; AClass *tk=sk[b]; sk[b]=sk[b-1]; sk[b-1]=tk; }
      for(int a=0;a<nsn;a++){ const char *names[256]; int n=ut_test_names(tc,sk[a],names,256);
          for(int t=0;t<n;t++) sb_add(&o,"    out.append(__ut_k%d(\"%s\"))\n",sk[a]->id,names[t]); }
      sb_add(&o,"    if main_only:\n        return out\n");
      for(int i=nk-1;i>=0;i--){ int inmain=0; for(int a=0;a<nsn;a++) if(sk[a]==ks[i]) inmain=1;
          if(inmain) continue;
          const char *names[256]; int n=ut_test_names(tc,ks[i],names,256);
          for(int t=0;t<n;t++) sb_add(&o,"    out.append(__ut_k%d(\"%s\"))\n",ks[i]->id,names[t]); }
      free(sn); free(sk); }
    sb_add(&o,"    return out\n");
    /* call a test method by its name */
    sb_add(&o,"def __mpy_ut_call(t: TestCase, name: str) -> bool:\n");
    for(int i=0;i<nk;i++){ const char *names[256]; int n=ut_test_names(tc,ks[i],names,256);
        if(!n && !aot_find_method(ks[i],"runTest")) continue;
        sb_add(&o,"    if isinstance(t, __ut_k%d):\n",ks[i]->id);
        for(int t=0;t<n;t++) sb_add(&o,"        if name == \"%s\":\n            t.%s()\n            return True\n",names[t],names[t]);
        if(aot_find_method(ks[i],"runTest") && aot_find_method(ks[i],"runTest")->cls!=tc) sb_add(&o,"        if name == \"runTest\":\n            t.runTest()\n            return True\n");
        sb_add(&o,"        return False\n"); }
    sb_add(&o,"    return False\n");
    /* skip reasons (decorators), expected failures, docstrings */
    sb_add(&o,"def __mpy_ut_skip(t: TestCase, name: str) -> str | None:\n");
    for(int i=0;i<nk;i++){ AClass *mro[32]; int nm=ut_mro(ks[i],mro,32);
        sb_add(&o,"    if isinstance(t, __ut_k%d):\n",ks[i]->id);
        for(int j=0;j<nm;j++){ int q=-1; for(int z=0;z<nk;z++) if(ks[z]==mro[j]) q=z;
            if(q<0 || !has_s[q]) continue;
            sb_add(&o,"        r = __ut_s%d(name, ",mro[j]->id);              /* own: the methods it is the first in the MRO to define */
            int none=1;
            for(int mi=0;mi<mro[j]->nmethods;mi++){ const char *mn=mro[j]->methods[mi]->name; if(strncmp(mn,"test",4)) continue;
                int earlier=0; for(int e=0;e<j;e++) if(ut_own_method(mro[e],mn)) earlier=1;
                if(earlier) continue;
                sb_add(&o,"%sname == \"%s\"",none?"":" or ",mn); none=0; }
            sb_add(&o,"%s)\n        if r is not None:\n            return r\n",none?"False":""); }
        sb_add(&o,"        return None\n"); }
    sb_add(&o,"    return None\n");
    sb_add(&o,"def __mpy_ut_xfail(t: TestCase, name: str) -> bool:\n");
    for(int i=0;i<nk;i++){ AClass *mro[32]; int nm=ut_mro(ks[i],mro,32); int cls_x=0; SrcBuf names={0};
        for(int j=0;j<nm;j++){ AotUnit *ku=mro[j]->mod?mro[j]->mod->unit:NULL; const char *ksrc=ku?ku->src:NULL;
            if(mro[j]->def) for(int d=0;d<mro[j]->def->decorator_count;d++){ const char *dn=ut_deco_name(ksrc,aot_expr(ku,&mro[j]->def->decorator_exprs[d])); if(dn && !strcmp(dn,"expectedFailure")) cls_x=1; }
            for(int mi=0;mi<mro[j]->nmethods;mi++){ AFunc *fm=mro[j]->methods[mi]; if(strncmp(fm->name,"test",4) || !fm->def) continue;
                int earlier=0; for(int e=0;e<j;e++) if(ut_own_method(mro[e],fm->name)) earlier=1;
                if(earlier) continue;
                for(int d=0;d<fm->def->decorator_count;d++){ const char *dn=ut_deco_name(ksrc,aot_expr(ku,&fm->def->decorator_exprs[d]));
                    if(dn && !strcmp(dn,"expectedFailure")) sb_add(&names,"%sname == \"%s\"",names.s&&names.n?" or ":"",fm->name); } } }
        if(cls_x) sb_add(&o,"    if isinstance(t, __ut_k%d):\n        return True\n",ks[i]->id);
        else sb_add(&o,"    if isinstance(t, __ut_k%d):\n        return %s\n",ks[i]->id,names.s&&names.n?names.s:"False");
        free(names.s); }
    sb_add(&o,"    return False\n");
    sb_add(&o,"def __mpy_ut_doc(t: TestCase, name: str) -> str:\n");
    for(int i=0;i<nk;i++){ AClass *mro[32]; int nm=ut_mro(ks[i],mro,32); SrcBuf b={0};
        const char *names[256]; int n=ut_test_names(tc,ks[i],names,256);
        for(int t=0;t<n;t++){ AFunc *fm=NULL; for(int j=0;j<nm && !fm;j++) fm=ut_own_method(mro[j],names[t]);
            const char *doc=fm?ut_docstring(fm->def):NULL; if(!doc) continue;
            sb_add(&b,"        if name == \"%s\":\n            return ",names[t]); ut_str_lit(&b,doc); sb_add(&b,"\n"); }
        sb_add(&o,"    if isinstance(t, __ut_k%d):\n%s        return \"\"\n",ks[i]->id,b.s?b.s:"");
        free(b.s); }
    sb_add(&o,"    return \"\"\n");
    /* setUpClass / tearDownClass (0 / 1) of the test's class */
    sb_add(&o,"def __mpy_ut_fixture(t: TestCase, what: int) -> None:\n");
    for(int i=0;i<nk;i++) sb_add(&o,"    if isinstance(t, __ut_k%d):\n        if what == 0:\n            __ut_k%d.setUpClass()\n        else:\n            __ut_k%d.tearDownClass()\n        return\n",ks[i]->id,ks[i]->id,ks[i]->id);
    sb_add(&o,"    pass\n");
    /* its class's name (module.Qualname) and module */
    sb_add(&o,"def __mpy_ut_class(t: TestCase) -> str:\n");
    for(int i=0;i<nk;i++) sb_add(&o,"    if isinstance(t, __ut_k%d):\n        return \"%s.%s\"\n",ks[i]->id,ks[i]->mod==mainm?"__main__":ks[i]->mod->name,ks[i]->qualname?ks[i]->qualname:ks[i]->name);
    sb_add(&o,"    return \"\"\n");
    sb_add(&o,"def __mpy_ut_file(t: TestCase) -> str:\n");
    for(int i=0;i<nk;i++){ sb_add(&o,"    if isinstance(t, __ut_k%d):\n        return ",ks[i]->id); ut_str_lit(&o,ks[i]->mod->unit&&ks[i]->mod->unit->path?ks[i]->mod->unit->path:"?"); sb_add(&o,"\n"); }
    sb_add(&o,"    return \"?\"\n");
    sb_add(&o,"def __mpy_ut_module(t: TestCase) -> str:\n");
    for(int i=0;i<nk;i++) sb_add(&o,"    if isinstance(t, __ut_k%d):\n        return \"%s\"\n",ks[i]->id,ks[i]->mod==mainm?"__main__":ks[i]->mod->name);
    sb_add(&o,"    return \"\"\n");
    /* setUpModule / tearDownModule of a module of tests */
    sb_add(&o,"def __mpy_ut_modfix(module: str, setup: bool) -> bool:\n");
    for(int mi=0;mi<p->nmods;mi++){ AModule *m=p->mods[mi]; int has=0; for(int i=0;i<nk;i++) if(ks[i]->mod==m) has=1;
        if(!has) continue;
        for(int w=0;w<2;w++){ ASym *fs=module_sym(m,w?"tearDownModule":"setUpModule"); if(!fs || fs->kind!=AS_FUNC) continue;
            char hn[64]; snprintf(hn,sizeof hn,"__ut_m%d_%s",mi,w?"tearDownModule":"setUpModule"); symtab_add(&builtin_syms,xstrdup2(hn),AS_FUNC,fs->p); ((AFunc*)fs->p)->ncalls++;
            sb_add(&o,"    if module == \"%s\" and %ssetup:\n        %s()\n        return True\n",m==mainm?"__main__":m->name,w?"not ":"",hn); } }
    sb_add(&o,"    return False\n");
    ut_compile(c,ut,o.s,NULL,0,line,ks,nk);
    free(o.s); free(ks); free(has_s);
}
/* f(..., encoding="unicode") of a module with _f_unicode (xml.etree's tostring): that one (a str, not bytes).
   The call's callee is made a name of it (a hidden one in this module); 1: done */
