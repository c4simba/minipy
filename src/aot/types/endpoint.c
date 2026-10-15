/* ========================= Typed compiler, types: minipy.Endpoint =========================
   A function made callable with its parameters as strings (web
   framework handlers). */

#include "check.h"

/* ---- minipy.Endpoint: a function made callable with its parameters as strings
   (path and query parameters of an HTTP request). The compiler writes the
   adapter as Python and compiles it with the program:
       def __endpoint__(values: dict[str, str]) -> str:
           if "item_id" not in values: raise ValueError("#endpoint\nmissing\nitem_id\nint\n")
           a0 = int(values["item_id"])          ValueError("#endpoint\nparsing\nitem_id\nint\n<text>") if it is not one
           a1 = <default>                      (optional) / if "q" in values: a1 = values["q"]
           return json.dumps(f(a0, a1), separators=(",", ":"), ensure_ascii=False)
   An async function is called directly: the adapter runs on a task (await = a call). */

void sb_add(SrcBuf *b, const char *fmt, ...){
    va_list ap; va_start(ap,fmt); int k=vsnprintf(NULL,0,fmt,ap); va_end(ap);
    if(k<0) return;
    if(b->n+(size_t)k+1>b->cap){ b->cap=(b->n+(size_t)k+1)*2; b->s=(char*)xrealloc(b->s,b->cap); }
    va_start(ap,fmt); vsnprintf(b->s+b->n,(size_t)k+1,fmt,ap); va_end(ap); b->n+=(size_t)k;
}
void lit_source(Ck *c, Expr *d, SrcBuf *b, int line){
    if(d->kind==EXPR_NONE){ sb_add(b,"None"); return; }
    if(d->kind==EXPR_TRUE){ sb_add(b,"True"); return; }
    if(d->kind==EXPR_FALSE){ sb_add(b,"False"); return; }
    if(d->kind==EXPR_UNARY && d->op==T_MINUS && d->a->kind==EXPR_LITERAL && d->a->tok->kind==T_NUMBER){ sb_add(b,"-%s",d->a->tok->text); return; }
    if(d->kind==EXPR_LITERAL && d->tok->kind==T_NUMBER){ sb_add(b,"%s",d->tok->text); return; }
    if(d->kind==EXPR_LITERAL && d->tok->kind==T_STRING){
        sb_add(b,"\"");
        for(const unsigned char *p=(const unsigned char*)d->tok->text;*p;p++){
            if(*p=='"'||*p=='\\') sb_add(b,"\\%c",*p); else if(*p<32) sb_add(b,"\\x%02x",*p); else sb_add(b,"%c",*p); }
        sb_add(b,"\""); return; }
    ck_err(c,line,"an endpoint parameter's default must be a literal");
}
static AFunc *ep_adapter(Ck *c, AFunc *fn, int line){
    if(fn->ep_adapter) return fn->ep_adapter;
    if(fn->outer || fn->cls) ck_err(c,line,"an endpoint must be a module-level function");
    if(fn->kwonly<fn->nparams || (fn->def && (fn->def->star_index>=0 || fn->def->dstar_index>=0))) ck_err(c,line,"endpoint %s(): only plain parameters (no *args, **kwargs, keyword-only)",fn->name);
    SrcBuf b={0};
    sb_add(&b,"def __endpoint__(values: dict[str, str]) -> str:\n");
    for(int k=0;k<fn->nparams;k++){ const char *nm=fn->params[k]->name; Ty *t=ty_find(fn->params[k]->ty);
        const char *conv=t->k==TY_INT?"int":t->k==TY_FLOAT?"float":t->k==TY_BOOL?"bool":(t->k==TY_STR||t->k==TY_VAR)?"str":NULL;
        if(!conv) ck_err(c,line,"endpoint %s(): parameter '%s' is %s; path and query parameters are int, float, bool or str",fn->name,nm,ty_name(t));
        if(fn->defaults[k]){ sb_add(&b,"    a%d = ",k); lit_source(c,fn->defaults[k],&b,line); sb_add(&b,"\n    if \"%s\" in values:\n",nm); }
        else sb_add(&b,"    if \"%s\" not in values:\n        raise ValueError(\"#endpoint\\nmissing\\n%s\\n%s\\n\")\n    if True:\n",nm,nm,conv);
        sb_add(&b,"        v%d = values[\"%s\"]\n",k,nm);
        if(!strcmp(conv,"str")) sb_add(&b,"        a%d = v%d\n",k,k);
        else if(!strcmp(conv,"bool")){
            sb_add(&b,"        l%d = v%d.lower()\n        if l%d in (\"1\", \"true\", \"t\", \"yes\", \"y\", \"on\"):\n            a%d = True\n",k,k,k,k);
            sb_add(&b,"        elif l%d in (\"0\", \"false\", \"f\", \"no\", \"n\", \"off\"):\n            a%d = False\n",k,k);
            sb_add(&b,"        else:\n            raise ValueError(\"#endpoint\\nparsing\\n%s\\nbool\\n\" + v%d)\n",nm,k);
        } else {
            sb_add(&b,"        try:\n            a%d = %s(v%d)\n        except ValueError:\n            raise ValueError(\"#endpoint\\nparsing\\n%s\\n%s\\n\" + v%d)\n",k,conv,k,nm,conv,k);
        }
    }
    sb_add(&b,"    return __json__.dumps(__target__(");
    for(int k=0;k<fn->nparams;k++) sb_add(&b,"%sa%d",k?", ":"",k);
    sb_add(&b,"), separators=(\",\", \":\"), ensure_ascii=False)\n");
    AotUnit *u=MPY_NEW0(AotUnit);
    u->name=fn->mod->unit->name; u->path=fn->mod->unit->path; u->src=b.s;
    u->ast=py_front(u->path,b.s);
    if(!u->ast) ck_err(c,line,"internal error: the adapter of endpoint %s() does not compile",fn->name);
    AModule *m=MPY_NEW0(AModule); m->unit=u; m->name=fn->mod->name; m->index=fn->mod->index; m->body=fn->mod->body;
    symtab_add(&m->syms,"__target__",AS_FUNC,fn);
    symtab_add(&m->syms,"__json__",AS_SYS,(void*)"json");
    Stmt *def=u->ast->body[0];
    for(int k=0;k<def->body_count;k++) def->body[k]->line=line;      /* errors in it point at the conversion */
    AModule *save=c->mod; AFunc *savef=c->fn;
    c->mod=m;
    AFunc *ad=new_func(c,NULL,m,NULL,def,NULL);
    c->mod=save; c->fn=savef;
    free(ad->name); ad->name=(char*)xmalloc(strlen(fn->name)+16); sprintf(ad->name,"%s<endpoint>",fn->name);
    ad->calls_coroutines=1; ad->ncalls++; ad->value_used=1;
    fn->ncalls++; fn->ep_adapter=ad; c->changed=1;
    return ad;
}
/* an argument passed where minipy.Endpoint is expected: the function itself */
void ep_arg(Ck *c, Ty *pt, Expr *a, int line){
    if(!is_endpoint(pt) || is_endpoint(xinfo(a)->ty)) return;
    XInfo *x=xinfo(a);
    if(x->kind!=X_FUNCREF || !x->fn) ck_err(c,line,"a minipy.Endpoint is made from a function given by its name");
    ep_adapter(c,x->fn,line);
}
