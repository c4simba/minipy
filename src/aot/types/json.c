/* ========================= Typed compiler, types: json, pickle =========================
   What json.dumps can write; json.loads / pickle.loads: a decoder for
   the type the result goes to. */

#include "check.h"

/* What json.dumps can write: as CPython's json, plus objects (their fields,
   as a JSON object) and records. */
void json_ok(Ck *c, Ty *t, int line, int depth){
    t=ty_find(t);
    if(depth>32) return;
    switch(t->k){
        case TY_VAR: if(!depth) pending(c,line,"the value written as JSON"); return;   /* items of an empty container: defaulted */
        case TY_INT: case TY_BOOL: case TY_FLOAT: case TY_STR: case TY_VOID: return;
        case TY_LIST: json_ok(c,t->elem,line,depth+1); return;
        case TY_TUPLE: for(int i=0;i<t->nelems;i++) json_ok(c,t->elems[i],line,depth+1); return;
        case TY_DICT:{ Ty *k=ty_find(ty_dkey(t));
            if(k->k!=TY_STR && k->k!=TY_INT && k->k!=TY_BOOL && k->k!=TY_FLOAT && k->k!=TY_VAR) ck_err(c,line,"TypeError: keys must be str, int, float or bool, not %s (json)",ty_name(k));
            json_ok(c,t->elem,line,depth+1); return; }
        case TY_OBJ: for(AClass *k=t->cls;k;k=k->base) for(int i=0;i<k->nfields;i++) json_ok(c,k->fields[i]->ty,line,depth+1); return;
        default: ck_err(c,line,"TypeError: Object of type %s is not JSON serializable",ty_name(t));
    }
}
/* ---- json.loads(s) in compiled code: the decoder of the type its result goes to (bound by an
   annotation, a parameter, a return type), written as Python over _ppy_jsonr.Reader:
   int / float / str / bool, T | None, list, set, tuple, dict (str / int / float keys) and objects
   made by their __init__ from the members of their parameters' names (a dataclass: its fields). */
typedef struct { char *key; AFunc *fn; char *name; } JLoad;
static JLoad *g_jl; static int g_njl;
typedef struct { SrcBuf b; Ty **done; char **dname; int ndone; AClass **cls; int ncls; Ck *c; int line; ASym **dsym; int nd; int pick; } JGen;
static const char *jl_fn(JGen *g, Ty *t);
static void jl_ann(JGen *g, Ty *t, SrcBuf *o){
    t=ty_find(t);
    if(t->opt){ Ty *u=ty_copy(t); u->opt=0; jl_ann(g,u,o); sb_add(o," | None"); return; }
    switch(t->k){
        case TY_INT: sb_add(o,"int"); return;
        case TY_FLOAT: sb_add(o,"float"); return;
        case TY_STR: sb_add(o,"str"); return;
        case TY_BOOL: sb_add(o,"bool"); return;
        case TY_BYTES: if(!g->pick) break; sb_add(o,"bytes"); return;      /* (pickle.loads: bytes too) */
        case TY_LIST: if(t->tup){ sb_add(o,"tuple["); jl_ann(g,t->elem,o); sb_add(o,", ...]"); return; } sb_add(o,"list["); jl_ann(g,t->elem,o); sb_add(o,"]"); return;
        case TY_SET: sb_add(o,"set["); jl_ann(g,t->elem,o); sb_add(o,"]"); return;
        case TY_DICT: sb_add(o,"dict["); jl_ann(g,ty_dkey(t),o); sb_add(o,", "); jl_ann(g,t->elem,o); sb_add(o,"]"); return;
        case TY_TUPLE: if(t->names) break; sb_add(o,"tuple["); for(int i=0;i<t->nelems;i++){ if(i) sb_add(o,", "); jl_ann(g,t->elems[i],o); } sb_add(o,"]"); return;
        case TY_OBJ:{ int k=0; for(;k<g->ncls;k++) if(g->cls[k]==t->cls) break;
            if(k==g->ncls){ g->cls=(AClass**)xrealloc(g->cls,sizeof(AClass*)*(size_t)(g->ncls+1)); g->cls[g->ncls++]=t->cls; }
            sb_add(o,"__jc%d",k); return; }
        default: break; }
    ck_err(g->c,g->line,g->pick?"pickle.loads(): %s cannot be read from a pickle in compiled code":"json.loads(): %s cannot be read from JSON in compiled code",ty_name(t));
}
/* the expression reading a T from r */
static void jl_read(JGen *g, Ty *t, SrcBuf *o){
    t=ty_find(t);
    if(!t->opt){
        if(t->k==TY_INT){ sb_add(o,"r.read_int()"); return; }
        if(t->k==TY_FLOAT){ sb_add(o,"r.read_float()"); return; }
        if(t->k==TY_STR){ sb_add(o,"r.read_str()"); return; }
        if(t->k==TY_BOOL){ sb_add(o,"r.read_bool()"); return; }
        if(t->k==TY_BYTES && g->pick){ sb_add(o,"r.read_bytes()"); return; } }
    sb_add(o,"%s(r)",jl_fn(g,t));
}
/* the function reading a T (made once per type) */
static const char *jl_fn(JGen *g, Ty *t){
    t=ty_find(t);
    for(int i=0;i<g->ndone;i++) if(ty_same_exact(g->done[i],t)) return g->dname[i];
    char nm[64]; snprintf(nm,sizeof nm,"__jl%d_%d",g_njl,g->ndone);
    g->done=(Ty**)xrealloc(g->done,sizeof(Ty*)*(size_t)(g->ndone+1)); g->dname=(char**)xrealloc(g->dname,sizeof(char*)*(size_t)(g->ndone+1));
    g->done[g->ndone]=t; g->dname[g->ndone]=xstrdup2(nm); const char *name=g->dname[g->ndone]; g->ndone++;
    SrcBuf f={0};
    sb_add(&f,"def %s(r: __jr__.Reader) -> ",name); jl_ann(g,t,&f); sb_add(&f,":\n");
    if(t->opt){ Ty *u=ty_copy(t); u->opt=0;
        sb_add(&f,"    if r.null():\n        return None\n    return "); jl_read(g,u,&f); sb_add(&f,"\n"); }
    else switch(t->k){
        case TY_INT: case TY_FLOAT: case TY_STR: case TY_BOOL: case TY_BYTES: sb_add(&f,"    return "); jl_read(g,t,&f); sb_add(&f,"\n"); break;
        case TY_LIST: case TY_SET:
            sb_add(&f,"    out: "); jl_ann(g,t->k==TY_LIST&&t->tup?ty_new(TY_LIST,t->elem,NULL):t,&f); sb_add(&f," = %s\n    r.begin_list()\n    while r.next_item():\n        out.%s(",t->k==TY_SET?"set()":"[]",t->k==TY_SET?"add":"append");
            jl_read(g,t->elem,&f); sb_add(&f,")\n    return %s\n",t->k==TY_LIST&&t->tup?"tuple(out)":"out"); break;
        case TY_TUPLE:
            sb_add(&f,"    r.begin_list()\n");
            for(int i=0;i<t->nelems;i++){ sb_add(&f,"    if not r.next_item():\n        raise r.type_error(\"a JSON array of %d items is wanted\")\n    v%d = ",t->nelems,i); jl_read(g,t->elems[i],&f); sb_add(&f,"\n"); }
            sb_add(&f,"    if r.next_item():\n        raise r.type_error(\"a JSON array of %d items is wanted\")\n    return (",t->nelems);
            for(int i=0;i<t->nelems;i++) sb_add(&f,"%sv%d",i?", ":"",i); sb_add(&f,"%s)\n",t->nelems==1?",":""); break;
        case TY_DICT:{ Ty *kt=ty_find(ty_dkey(t));
            const char *kc= kt->k==TY_STR&&!kt->opt ? "r.key" : kt->k==TY_INT&&!kt->opt ? "int(r.key)" : kt->k==TY_FLOAT&&!kt->opt ? "float(r.key)" : NULL;
            if(!kc) ck_err(g->c,g->line,"json.loads(): the keys of %s must be str, int or float",ty_name(t));
            sb_add(&f,"    out: "); jl_ann(g,t,&f); sb_add(&f," = {}\n    r.begin_obj()\n    while r.next_key():\n        k = %s\n        out[k] = ",kc);
            jl_read(g,t->elem,&f); sb_add(&f,"\n    return out\n"); break; }
        case TY_OBJ:{ AClass *k=t->cls; AFunc *ini=aot_find_method(k,"__init__");
            if(!ini || ini->cls->builtin) ck_err(g->c,g->line,"json.loads(): class %s has no __init__ to make it with",k->name);
            if(ini->star>=0 || ini->dstar>=0) ck_err(g->c,g->line,"json.loads(): %s.__init__ takes *args / **kwargs",k->name);
            for(int p=1;p<ini->nparams;p++){ Ty *pt=ty_find(ini->params[p]->ty); if(!ty_known(pt)) ck_err(g->c,g->line,"json.loads(): the type of %s.__init__'s parameter '%s' is not known",k->name,ini->params[p]->name);
                Ty *ot=ty_copy(pt); ot->opt=1; sb_add(&f,"    v%d: ",p); jl_ann(g,ot,&f); sb_add(&f," = None\n    h%d = False\n",p); }
            sb_add(&f,"    r.begin_obj()\n    while r.next_key():\n        k = r.key\n");
            for(int p=1;p<ini->nparams;p++){ sb_add(&f,"        %sif k == \"%s\":\n            v%d = ",p>1?"el":"",ini->params[p]->name,p); jl_read(g,ini->params[p]->ty,&f); sb_add(&f,"\n            h%d = True\n",p); }
            sb_add(&f,"        %s\n            r.skip()\n",ini->nparams>1?"else:":"if True:");
            for(int p=1;p<ini->nparams;p++){ Expr *d=ini->defaults[p];
                if(!d) sb_add(&f,"    if not h%d:\n        raise r.missing(\"%s\", \"%s\")\n",p,ini->params[p]->name,k->name);
                else { sb_add(&f,"    if not h%d:\n        v%d = ",p,p);
                    ASym *ds= d->kind==EXPR_NAME ? module_sym(ini->mod,d->name) : NULL;     /* a dataclass field's default: its class-level value */
                    if(ds){ g->dsym=(ASym**)xrealloc(g->dsym,sizeof(ASym*)*(size_t)(g->nd+1)); g->dsym[g->nd]=ds; sb_add(&f,"__jd%d",g->nd); g->nd++; }
                    else lit_source(g->c,d,&f,g->line);
                    sb_add(&f,"\n"); }
                if(!ty_find(ini->params[p]->ty)->opt) sb_add(&f,"    if v%d is None:\n        raise r.type_error(\"null for %s.%s\")\n",p,k->name,ini->params[p]->name); }
            sb_add(&f,"    return "); jl_ann(g,t,&f); sb_add(&f,"(");
            for(int p=1;p<ini->nparams;p++) sb_add(&f,"%sv%d",p>1?", ":"",p); sb_add(&f,")\n"); break; }
        default: jl_ann(g,t,&f); break; }
    sb_add(&g->b,"%s",f.s); free(f.s);
    return name;
}
static AFunc *json_loader_of(Ck *c, Ty *t, int line, AModule *pick);
static AFunc *json_loader(Ck *c, Ty *t, int line){ return json_loader_of(c,t,line,NULL); }
/* ... pick: the pickle module (pickle.loads(b): the decoder reads through its PReader) */
static AFunc *json_loader_of(Ck *c, Ty *t, int line, AModule *pick){
    t=ty_find(t);
    char keyb[300]; snprintf(keyb,sizeof keyb,"%s%s",pick?"pickle:":"",t->k==TY_VOID ? "None" : ty_name(t));
    const char *key=keyb;
    for(int i=0;i<g_njl;i++) if(!strcmp(g_jl[i].key,key)) return g_jl[i].fn;
    AModule *jr=find_module(c->p,"_ppy_jsonr");
    if(!jr) ck_err(c,line,"internal error: json.loads() without _ppy_jsonr.py");
    JGen g; memset(&g,0,sizeof g); g.c=c; g.line=line; g.pick= pick!=NULL;
    char top[64]; snprintf(top,sizeof top,"__jl%d",g_njl);
    const char *first= ty_find(t)->k==TY_VOID ? NULL : jl_fn(&g,t);
    const char *arg= pick ? "s: bytes" : "s: str", *mk= pick ? "__pm__.PReader(s)" : "__jr__.Reader(s)";
    if(t->k==TY_VOID) sb_add(&g.b,"def %s(%s) -> None:\n    r = %s\n    r.skip()\n    r.end()\n",top,arg,mk);
    else { sb_add(&g.b,"def %s(%s) -> ",top,arg); jl_ann(&g,t,&g.b);
        sb_add(&g.b,":\n    r = %s\n    v = %s(r)\n    r.end()\n    return v\n",mk,first); }
    AotUnit *u=PPY_NEW0(AotUnit); u->name=jr->unit->name; u->path=jr->unit->path; u->src=g.b.s;
    u->ast=py_front(u->path,g.b.s);
    if(!u->ast){ if(getenv("PPY_JDEBUG")) fprintf(stderr,"%s",g.b.s); ck_err(c,line,"internal error: the JSON decoder of %s does not compile",key); }
    AModule *m=PPY_NEW0(AModule); *m=*jr; m->unit=u; memset(&m->syms,0,sizeof m->syms);
    symtab_add(&m->syms,"__jr__",AS_MODULE,jr);
    if(pick) symtab_add(&m->syms,"__pm__",AS_MODULE,pick);
    for(int k=0;k<g.ncls;k++){ char cn[32]; snprintf(cn,sizeof cn,"__jc%d",k); symtab_add(&m->syms,cn,AS_CLASS,g.cls[k]); }
    for(int k=0;k<g.nd;k++){ char dn[32]; snprintf(dn,sizeof dn,"__jd%d",k); symtab_add(&m->syms,dn,g.dsym[k]->kind,g.dsym[k]->p); }
    AModule *save=c->mod; AFunc *savef=c->fn; AFunc *topf=NULL;
    c->mod=m;
    for(int i=0;i<u->ast->body_count;i++){ Stmt *d=u->ast->body[i];
        for(int k=0;k<d->body_count;k++) d->body[k]->line=line;
        AFunc *f=new_func(c,NULL,m,NULL,d,NULL); f->ncalls++;
        symtab_add(&m->syms,d->name,AS_FUNC,f);
        if(!strcmp(d->name,top)) topf=f; }
    c->mod=save; c->fn=savef;
    g_jl=(JLoad*)xrealloc(g_jl,sizeof(JLoad)*(size_t)(g_njl+1)); g_jl[g_njl].key=xstrdup2(key); g_jl[g_njl].fn=topf; g_jl[g_njl].name=xstrdup2(top); g_njl++;
    c->changed=1;
    return topf;
}
/* json.loads(s): the call of the decoder for the type wanted (s: str or bytes) */
Ty *ck_json_loads(Ck *c, Expr *e){
    XInfo *xi=xinfo(e); int line=e->line;
    for(int i=0;i<e->count;i++) if(e->items[i]->akind) ck_err(c,line,"json.loads(): only the text is supported in compiled code (no keyword arguments)");
    if(e->count!=1) ck_err(c,line,"json.loads() takes one argument");
    Ty *st=ty_find(ck_expr(c,e->items[0]));
    if(st->k==TY_VAR) return pending(c,line,"the JSON text");
    if(st->k==TY_BYTES){ Expr *at=xnew(EXPR_ATTRIBUTE,line); at->a=e->items[0]; at->name="decode"; Expr *dc=xnew(EXPR_CALL,line); dc->a=at; e->items[0]=dc; }
    else if(st->k!=TY_STR) ck_err(c,line,"the JSON object must be str, bytes or bytearray, not %s",ty_name(st));
    Ty *rv= xi->ty ? xi->ty : ty_var();
    Ty *t=ty_find(rv);
    if(c->discard==e && t->k==TY_VAR) t=TY_VOID_T;                          /* json.loads(s) as a statement: only the text checked */
    if(!ty_known(t) && t->k!=TY_VOID){
        if(c->strict) ck_err(c,line,"json.loads(): the type of its result is not known; annotate it (x: dict[str, int] = json.loads(s))");
        xi->kind=X_NONE; return rv; }
    AFunc *fn=json_loader(c,t,line);
    char hn[80]; snprintf(hn,sizeof hn," json.loads %s",fn->name);
    if(!module_sym(c->mod,hn)) symtab_add(&c->mod->syms,hn,AS_FUNC,fn);
    e->a=name_expr(module_sym(c->mod,hn)->name,line);
    return ck_call_inner(c,e);
}
/* pickle.loads(b) / pickle.load(f) (a module with PReader): the decoder of the type wanted, reading the
   pickle through PReader (load(f): f.read() first) */
Ty *ck_pickle_loads(Ck *c, Expr *e, AModule *mod, int load){
    XInfo *xi=xinfo(e); int line=e->line;
    int n=0; for(int i=0;i<e->count;i++) if(!e->items[i]->akind) e->items[n++]=e->items[i];   /* (fix_imports=, encoding=...: no matter here) */
    e->count=n;
    if(e->count!=1) ck_err(c,line,"pickle.%s() takes the %s",load?"load":"loads",load?"file":"data");
    if(load){ Expr *at=xnew(EXPR_ATTRIBUTE,line); at->a=e->items[0]; at->name="read"; Expr *rd=xnew(EXPR_CALL,line); rd->a=at; e->items[0]=rd; }
    Ty *st=ty_find(ck_expr(c,e->items[0]));
    if(st->k==TY_VAR) return pending(c,line,"the pickle");
    if(st->k!=TY_BYTES) ck_err(c,line,"a bytes-like object is required, not %s",ty_name(st));
    Ty *rv= xi->ty ? xi->ty : ty_var();
    Ty *t=ty_find(rv);
    if(c->discard==e && t->k==TY_VAR) t=TY_VOID_T;
    if(!ty_known(t) && t->k!=TY_VOID){
        if(c->strict) ck_err(c,line,"pickle.loads(): the type of its result is not known; annotate it (x: dict[str, int] = pickle.loads(b))");
        xi->kind=X_NONE; return rv; }
    AFunc *fn=json_loader_of(c,t,line,mod);
    char hn[80]; snprintf(hn,sizeof hn," pickle.loads %s",fn->name);
    if(!module_sym(c->mod,hn)) symtab_add(&c->mod->syms,hn,AS_FUNC,fn);
    e->a=name_expr(module_sym(c->mod,hn)->name,line);
    return ck_call_inner(c,e);
}
