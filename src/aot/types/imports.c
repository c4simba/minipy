/* ========================= Typed compiler, types: imports =========================
   The functions of the program, the modules the compiler provides, and
   the names each import binds. */

#include "check.h"

/* check mode: a class or function whose declaration has an error is reported
   and marked as failed (its uses are errors); the others are made */
static void fill_step(Ck *c, AClass *k){
    if(!aot_check_mode){ fill_class(c,k); return; }
    CkTry t; ck_try_begin(c,&t);
    if(!setjmp(c->fail)){ fill_class(c,k); ck_try_end(c,&t); }
    else { ck_try_fail(c,&t); k->check_failed=1;
        for(int i=0;i<k->nmethods;i++){ k->methods[i]->check_failed=1; k->methods[i]->unused=1; } }
}
static void func_step(Ck *c, AFunc *fn, AModule *m, Stmt *def){
    if(!aot_check_mode){ new_func(c,fn,m,NULL,def,NULL); return; }
    CkTry t; ck_try_begin(c,&t);
    if(!setjmp(c->fail)){ new_func(c,fn,m,NULL,def,NULL); ck_try_end(c,&t); }
    else { ck_try_fail(c,&t); fn->check_failed=1; fn->unused=1;
        if(!fn->name) fn->name=xstrdup2(def->name);
        if(!fn->mod) fn->mod=m;
        if(!fn->def) fn->def=def;
        if(!fn->ret) fn->ret=ty_var();
        fn->line=def->line; }
}

void collect_functions(Ck *c, AModule *m){
    c->mod=m;
    Stmt **b=m->unit->ast->body; int n=m->unit->ast->body_count;
    for(int i=0;i<c->p->nclasses;i++){ AClass *k=c->p->classes[i]; if(k->mod==m && !k->builtin && !k->encl) fill_step(c,k); }   /* (nested ones too) */
    for(int i=0;i<n;i++) if(b[i]->kind==STMT_FUNCTION_DEF){
        ASym *x=module_sym(m,b[i]->name);
        if(x->kind!=AS_FUNC){ char *h=hidden_name(b[i]->name,NULL); x=module_sym(m,h); free(h); }
        AFunc *fn=(AFunc*)x->p;
        if(!b[i]->decorator_count && (has_untyped_params(b[i]) || b[i]->ntparams || typevar_params(m,b[i]) || protocol_params(b[i]))) fn->pristine=b[i];   /* (instances are made from its tree) */
        func_step(c,fn,m,b[i]); }
    /* the module body itself */
    AFunc *body=MPY_NEW0(AFunc);
    body->name=xstrdup2(m->name); body->mod=m; body->body=b; body->nbody=n; body->ret=TY_VOID_T; body->line=1;
    add_func(c->p,body);
    m->body=body;
}

/* the constants of the string module */
const char *string_const(const char *n){
    static const char *const t[][2]={{"whitespace"," \t\n\r\x0b\x0c"},{"ascii_lowercase","abcdefghijklmnopqrstuvwxyz"},{"ascii_uppercase","ABCDEFGHIJKLMNOPQRSTUVWXYZ"},
        {"ascii_letters","abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"},{"digits","0123456789"},{"hexdigits","0123456789abcdefABCDEF"},{"octdigits","01234567"},
        {"punctuation","!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"},
        {"printable","0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~ \t\n\r\x0b\x0c"},{NULL,NULL}};
    for(int i=0;t[i][0];i++) if(!strcmp(t[i][0],n)) return t[i][1];
    return NULL;
}
/* Modules the compiler provides itself (no source file). */
static const char *builtin_module(const char *name){
    static const char *mods[]={"sys","asyncio","math","typing","functools","ctypes","json","minipy","dataclasses","abc","thread",NULL};
    for(int i=0;mods[i];i++) if(!strcmp(mods[i],name)) return mods[i];
    return NULL;
}

/* the import statements of a module, wherever they are: the module's top level first */
static void imports_of(Stmt **b, int n, Stmt ***out, int *nout, int *cap, int nested){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_IMPORT||s->kind==STMT_FROM_IMPORT){ if(nested){ if(*nout==*cap){ *cap=*cap?*cap*2:8; *out=(Stmt**)xrealloc(*out,sizeof(Stmt*)*(size_t)*cap); } (*out)[(*nout)++]=s; } continue; }
        imports_of(s->body,s->body_count,out,nout,cap,1); imports_of(s->orelse,s->orelse_count,out,nout,cap,1);
    }
}
/* import a.b.c / import a.b as x / from a.b import n [as m] / from a import *
   (a module's names may be ones it imports itself - `from .item import Item`
   in a module others take Item from - so its imports are linked first) */
void link_imports(Ck *c, AModule *m){
    if(m->linked) return;                              /* (done, or under way: a circular import) */
    m->linked=1;
    c->mod=m;
    AotUnit *u=m->unit;
    Stmt **top=u->ast->body; int ntop=u->ast->body_count;
    Stmt **b=NULL; int n=0, cap=0;
    for(int i=0;i<ntop;i++) if(top[i]->kind==STMT_IMPORT||top[i]->kind==STMT_FROM_IMPORT){ if(n==cap){ cap=cap?cap*2:8; b=(Stmt**)xrealloc(b,sizeof(Stmt*)*(size_t)cap); } b[n++]=top[i]; }
    int ntopimp=n;
    imports_of(top,ntop,&b,&n,&cap,0);                 /* then those inside functions and blocks (their names: the module's) */
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(i>=ntopimp && s->kind==STMT_IMPORT && module_sym(m,s->name)) continue;     /* already known by that name */
        if(s->kind==STMT_IMPORT){
            const char *dotted=s->name2?s->name2:s->name;
            const char *bm=builtin_module(dotted);
            if(bm){ symtab_add(&m->syms,s->name,AS_SYS,(void*)bm); if(!strcmp(bm,"dataclasses")) c->dc_conv=1; continue; }
            if(s->block_tag){ AModule *t=find_module(c->p,dotted); if(!t) ck_err(c,s->line,"no module named '%s'",dotted); symtab_add(&m->syms,s->name,AS_MODULE,t); }
            else { AModule *t=find_module(c->p,s->name); if(!t) ck_err(c,s->line,"no module named '%s'",s->name); symtab_add(&m->syms,s->name,AS_MODULE,t); }
        } else if(s->kind==STMT_FROM_IMPORT){
            char *dotted=aot_from_import_module(u,s);
            if(!dotted) ck_err(c,s->line,"bad relative import");
            if(!strcmp(dotted,"typing")||!strcmp(dotted,"collections.abc")||!strcmp(dotted,"__future__")){ free(dotted); continue; }   /* type names are understood directly */
            if(!strcmp(dotted,"abc")){                          /* from abc import ABC, abstractmethod */
                for(int k=0;k<s->nnames;k++){ const char *nm=s->names[k], *alias=s->asnames[k]?s->asnames[k]:nm;
                    if(!strcmp(nm,"ABC")) symtab_add(&m->syms,alias,AS_CLASS,abc_class(c));
                    else if(!strcmp(nm,"abstractmethod")||!strcmp(nm,"ABCMeta")) symtab_add(&m->syms,alias,AS_SYS,(void*)(nm[1]=='b'?"abc.abstractmethod":"abc.ABCMeta"));
                    else ck_err(c,s->line,"abc.%s is not available in compiled code",nm); }
                free(dotted); continue;
            }
            if(!strcmp(dotted,"functools")){                    /* from functools import reduce */
                for(int k=0;k<s->nnames;k++){ const char *nm=s->names[k], *alias=s->asnames[k]?s->asnames[k]:nm;
                    static const char *const ok[]={"reduce","wraps","partial","lru_cache","cache","total_ordering","cmp_to_key","cached_property",NULL}; const char *full=NULL;
                    for(int q=0;ok[q];q++) if(!strcmp(ok[q],nm)){ static char names[8][32]; snprintf(names[q],sizeof names[q],"functools.%s",ok[q]); full=names[q]; }
                    if(!full) ck_err(c,s->line,"functools.%s is not available in compiled code",nm);
                    symtab_add(&m->syms,alias,AS_SYS,(void*)full); }
                free(dotted); continue;
            }
            if(!strcmp(dotted,"ctypes")||!strcmp(dotted,"json")||!strcmp(dotted,"minipy")||!strcmp(dotted,"dataclasses")){    /* from ctypes import CDLL, c_int / from json import dumps */
                for(int k=0;k<s->nnames;k++){ const char *nm=s->names[k], *alias=s->asnames[k]?s->asnames[k]:nm;
                    if(!strcmp(nm,"*")) ck_err(c,s->line,"use `import %s` and %s.name in compiled code",dotted,dotted);
                    if(!strcmp(dotted,"dataclasses") && !strcmp(nm,"FrozenInstanceError")){ symtab_add(&m->syms,alias,AS_CLASS,symtab_find(&builtin_syms,nm)->p); continue; }
                    if(!strcmp(dotted,"dataclasses")){ static const char *ok[]={"dataclass","field","KW_ONLY","InitVar","asdict","astuple","replace","is_dataclass","fields","MISSING",NULL};
                        int known=0; for(int q=0;ok[q];q++) if(!strcmp(ok[q],nm)) known=1;
                        if(!known) ck_err(c,s->line,"dataclasses.%s is not available in compiled code",nm);
                        if(!strcmp(nm,"asdict")||!strcmp(nm,"astuple")) c->dc_conv=1; }
                    char full[96]; snprintf(full,sizeof full,"%s.%s",dotted,nm);
                    symtab_add(&m->syms,alias,AS_SYS,xstrdup2(full)); }
                free(dotted); continue;
            }
            if(!strcmp(dotted,"math")){                                   /* from math import sqrt, pi: math.sqrt, math.pi */
                char hm[64]; snprintf(hm,sizeof hm,"__mpy_bmod_%s",dotted);
                if(!module_sym(m,hm)) symtab_add(&m->syms,xstrdup2(hm),AS_SYS,(void*)builtin_module(dotted));
                for(int k=0;k<s->nnames;k++){ const char *nm=s->names[k], *alias=s->asnames[k]?s->asnames[k]:nm;
                    if(!strcmp(nm,"*")) ck_err(c,s->line,"use `import %s` and %s.name in compiled code",dotted,dotted);
                    char key[200]; snprintf(key,sizeof key,"attr:%s:%s",hm,nm); symtab_add(&m->syms,alias,AS_SYS,xstrdup2(key)); }
                free(dotted); continue;
            }
            if(builtin_module(dotted)) ck_err(c,s->line,"use `import %s` and %s.name in compiled code",dotted,dotted);
            AModule *t=find_module(c->p,dotted);
            if(!t) ck_err(c,s->line,"no module named '%s'",dotted);
            if(!t->linked && t->unit && t->unit->ast){ link_imports(c,t); c->mod=m; }
            if(s->nnames==1 && !strcmp(s->names[0],"*")){
                for(int k=0;k<t->syms.n;k++){ ASym *x=&t->syms.v[k]; if(x->name[0]!='_') symtab_add(&m->syms,x->name,x->kind,x->p); }
            } else for(int k=0;k<s->nnames;k++){
                const char *nm=s->names[k], *alias=s->asnames[k]?s->asnames[k]:nm;
                if(i>=ntopimp && module_sym(m,alias)) continue;         /* (an import in a function: the name may be known already) */
                ASym *x=symtab_find(&t->syms,nm);
                if(x) symtab_add(&m->syms,alias,x->kind,x->p);
                else {
                    char sub[300]; snprintf(sub,sizeof sub,"%s.%s",dotted,nm);
                    AModule *sm=find_module(c->p,sub);
                    char lz[200]; snprintf(lz,sizeof lz,"_lazy_%s",nm); ASym *l=symtab_find(&t->syms,lz);
                    if(!sm && l && l->kind==AS_FUNC){                           /* from time import tzname: a call of time._lazy_tzname() */
                        char hm[300]; snprintf(hm,sizeof hm,"__mpy_mod_%s",dotted); if(!module_sym(m,hm)) symtab_add(&m->syms,hm,AS_MODULE,t);
                        char key[600]; snprintf(key,sizeof key,"lazy:%s:%s",hm,lz); symtab_add(&m->syms,alias,AS_SYS,xstrdup2(key));
                        continue; }
                    if(!sm) ck_err(c,s->line,"cannot import name '%s' from '%s'",nm,dotted);
                    symtab_add(&m->syms,alias,AS_MODULE,sm);
                }
            }
            free(dotted);
        }
    }
    m->linked=2;
}

