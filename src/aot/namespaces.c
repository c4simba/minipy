/* ========================= AOT driver: namespace classes =========================
   The classes argparse (Namespace), optparse (Values) and
   types.SimpleNamespace give in compiled programs: a typed field per
   name the program uses, written from its calls. */

#include "driver.h"

/* ---- argparse in compiled programs: parse_args() gives a Namespace with a typed field per
   destination of the program's add_argument() calls (and set_defaults() literals); its class is
   written here from those calls and added to argparse.py's source. */

typedef struct { char *dest, *field, *ty, *init; } NsField;
typedef struct { NsField *f; int n; SBuf *err; const char *path; int line; } NsScan;
void sbf(SBuf *b, const char *fmt, ...){
    va_list ap; va_start(ap,fmt); int k=vsnprintf(NULL,0,fmt,ap); va_end(ap);
    if(k<0) return;
    if(b->n+(size_t)k+1>b->cap){ b->cap=(b->n+(size_t)k+1)*2; b->s=(char*)xrealloc(b->s,b->cap); }
    va_start(ap,fmt); vsnprintf(b->s+b->n,(size_t)k+1,fmt,ap); va_end(ap); b->n+=(size_t)k;
}
/* a literal as source text (numbers, strings, None, True / False, lists / tuples of them); 0: not one */
static int lit_text(Expr *e, SBuf *o){
    if(!e) return 0;
    if(e->kind==EXPR_NONE){ sbf(o,"None"); return 1; }
    if(e->kind==EXPR_TRUE){ sbf(o,"True"); return 1; }
    if(e->kind==EXPR_FALSE){ sbf(o,"False"); return 1; }
    if(e->kind==EXPR_UNARY && e->op==T_MINUS && e->a->kind==EXPR_LITERAL && e->a->tok->kind==T_NUMBER){ sbf(o,"-%s",e->a->tok->text); return 1; }
    if(e->kind==EXPR_LITERAL && e->tok->kind==T_NUMBER){ sbf(o,"%s",e->tok->text); return 1; }
    if(e->kind==EXPR_LITERAL && e->tok->kind==T_STRING && !e->tok->i){
        sbf(o,"\"");
        for(int64_t i=0;i<e->tok->len;i++){ unsigned char ch=(unsigned char)e->tok->text[i];
            if(ch=='"'||ch=='\\') sbf(o,"\\%c",ch); else if(ch<32) sbf(o,"\\x%02x",ch); else sbf(o,"%c",ch); }
        sbf(o,"\""); return 1; }
    if(e->kind==EXPR_LIST || e->kind==EXPR_TUPLE){
        sbf(o,e->kind==EXPR_LIST?"[":"(");
        for(int i=0;i<e->count;i++){ if(i) sbf(o,", "); if(!lit_text(e->items[i],o)) return 0; }
        if(e->kind==EXPR_TUPLE && e->count==1) sbf(o,",");
        sbf(o,e->kind==EXPR_LIST?"]":")"); return 1; }
    return 0;
}
/* the type of a literal: "int", "str", "list[int]" ...; NULL: not known */
static const char *lit_ty(Expr *e){
    if(!e) return NULL;
    if(e->kind==EXPR_TRUE||e->kind==EXPR_FALSE) return "bool";
    if(e->kind==EXPR_UNARY && e->op==T_MINUS) e=e->a;
    if(e->kind==EXPR_LITERAL && e->tok->kind==T_NUMBER) return e->tok->is_float?"float":"int";
    if(e->kind==EXPR_LITERAL && e->tok->kind==T_STRING && !e->tok->i) return "str";
    if(e->kind==EXPR_LIST && e->count){ const char *t=lit_ty(e->items[0]); if(!t) return NULL;
        static char buf[8][64]; static int k; k=(k+1)&7; snprintf(buf[k],sizeof buf[k],"list[%s]",t); return buf[k]; }
    return NULL;
}
static Expr *kwarg(Expr *call, const char *name){
    for(int i=0;i<call->count;i++) if(call->items[i]->akind==3 && !strcmp(call->items[i]->kw,name)) return call->items[i];
    return NULL;
}
static const char *str_lit(Expr *e){ return e && e->kind==EXPR_LITERAL && e->tok->kind==T_STRING && !e->tok->i ? e->tok->text : NULL; }
static void ns_add(NsScan *ns, const char *dest, const char *ty, const char *init, int line){
    for(int i=0;i<ns->n;i++) if(!strcmp(ns->f[i].dest,dest)){
        if(strcmp(ns->f[i].ty,ty)){ sbf(ns->err,"%s:%d: error: argparse: '%s' is %s here and %s elsewhere (one type per destination in compiled code)\n",ns->path,line,dest,ty,ns->f[i].ty); }
        return; }
    ns->f=(NsField*)xrealloc(ns->f,sizeof(NsField)*(size_t)(ns->n+1));
    NsField *f=&ns->f[ns->n++]; f->dest=xstrdup2(dest); f->ty=xstrdup2(ty); f->init=xstrdup2(init);
    char fl[256]; snprintf(fl,sizeof fl,"%s",dest); for(char *q=fl;*q;q++) if(!((*q>='a'&&*q<='z')||(*q>='A'&&*q<='Z')||(*q>='0'&&*q<='9')||*q=='_')) *q='_';
    f->field=xstrdup2(fl);
}
static void ns_call(NsScan *ns, Expr *e){
    int line=e->line; const char *flags[16]; int nf=0;
    for(int i=0;i<e->count;i++){ Expr *a=e->items[i]; if(a->akind) continue; const char *s=str_lit(a); if(!s) return; if(nf<16) flags[nf++]=s; }
    if(!nf && !kwarg(e,"dest")) return;
    Expr *k_action=kwarg(e,"action"), *k_nargs=kwarg(e,"nargs"), *k_const=kwarg(e,"const"), *k_default=kwarg(e,"default"),
         *k_type=kwarg(e,"type"), *k_required=kwarg(e,"required"), *k_dest=kwarg(e,"dest");
    const char *action= k_action ? str_lit(k_action) : "store";
    if(!action){ sbf(ns->err,"%s:%d: error: argparse: action= must be a string in compiled code (custom Action classes are not supported)\n",ns->path,line); return; }
    if(!strcmp(action,"help") || !strcmp(action,"version")) return;
    int positional= nf && flags[0][0]!='-';
    char dest[256]={0};
    if(k_dest && str_lit(k_dest)) snprintf(dest,sizeof dest,"%s",str_lit(k_dest));
    else if(positional) snprintf(dest,sizeof dest,"%s",flags[0]);
    else { for(int i=0;i<nf && !dest[0];i++) if(flags[i][0]=='-' && flags[i][1]=='-' && flags[i][2]) snprintf(dest,sizeof dest,"%s",flags[i]+2);
        if(!dest[0] && nf) snprintf(dest,sizeof dest,"%s",flags[0]+1);
        for(char *q=dest;*q;q++) if(*q=='-') *q='_'; }
    if(!dest[0]) return;
    const char *T="str", *get="str";
    if(k_type){ if(k_type->kind==EXPR_NAME && !strcmp(k_type->name,"int")){ T="int"; get="int"; }
        else if(k_type->kind==EXPR_NAME && !strcmp(k_type->name,"float")){ T="float"; get="float"; }
        else if(k_type->kind==EXPR_NAME && !strcmp(k_type->name,"str")){ T="str"; get="str"; }
        else { sbf(ns->err,"%s:%d: error: argparse: type= must be int, float or str in compiled code\n",ns->path,line); return; } }
    char nargs[32]="";
    if(k_nargs){ const char *s=str_lit(k_nargs); if(s) snprintf(nargs,sizeof nargs,"%s",s);
        else if(k_nargs->kind==EXPR_LITERAL && k_nargs->tok->kind==T_NUMBER && !k_nargs->tok->is_float) snprintf(nargs,sizeof nargs,"%lld",(long long)k_nargs->tok->i);
        else if(k_nargs->kind!=EXPR_NONE){ sbf(ns->err,"%s:%d: error: argparse: nargs= must be a literal in compiled code\n",ns->path,line); return; } }
    int required= k_required && k_required->kind==EXPR_TRUE;
    SBuf dflt={0}; int has_dflt=0, dflt_none=1, dflt_lit=0;
    if(k_default){ has_dflt=1; dflt_none= k_default->kind==EXPR_NONE; dflt_lit=lit_text(k_default,&dflt); }
    SBuf ty={0}, init={0}; char q[300]; snprintf(q,sizeof q,"\"%s\"",dest);
    int list= !strcmp(nargs,"*") || !strcmp(nargs,"+") || !strcmp(nargs,"...") || (nargs[0]>='0'&&nargs[0]<='9');
    const char *dty= k_default ? lit_ty(k_default) : NULL;
    int dfit= dflt_lit && dty && (!strcmp(dty,T) || (list && !strncmp(dty,"list[",5)));
    #define DFLT(ok_none) do{ if(has_dflt && !dflt_none){ if(dfit || (dflt_lit && strcmp(action,"store"))) sbf(&init,"%s",dflt.s); else if(!list) sbf(&init,"p.dflt_%s(%s)",get,q); \
            else { sbf(ns->err,"%s:%d: error: argparse: the default of '%s' must be a literal in compiled code\n",ns->path,line); return; } } else sbf(&init,"None"); }while(0)
    if(!strcmp(action,"store_true") || !strcmp(action,"store_false")){
        int t= action[6]=='t';
        sbf(&ty,"bool"); sbf(&init,"%s if p.seen(%s) else %s",t?"True":"False",q, has_dflt&&dflt_lit&&!dflt_none ? dflt.s : t?"False":"True"); }
    else if(!strcmp(action,"store_const") || !strcmp(action,"append_const")){
        SBuf c={0}; if(!k_const || !lit_text(k_const,&c) || !lit_ty(k_const)){ sbf(ns->err,"%s:%d: error: argparse: const= must be a literal in compiled code\n",ns->path,line); return; }
        const char *ct=lit_ty(k_const);
        if(action[0]=='s'){ int opt= !(has_dflt && !dflt_none); sbf(&ty,"%s%s",ct,opt?" | None":""); sbf(&init,"%s if p.seen(%s) else ",c.s,q); DFLT(1); }
        else { sbf(&ty,"list[%s] | None",ct); sbf(&init,"[%s] * p.count(%s) if p.seen(%s) else ",c.s,q,q); DFLT(1); }
        free(c.s); }
    else if(!strcmp(action,"count")){
        int opt= !(has_dflt && !dflt_none); sbf(&ty,"int%s",opt?" | None":""); sbf(&init,"p.count(%s) + %s if p.seen(%s) else ",q,opt?"0":dflt.s,q); DFLT(1); }
    else if(!strcmp(action,"append") || !strcmp(action,"extend")){
        if(list && !strcmp(action,"append")){ sbf(ns->err,"%s:%d: error: argparse: append with nargs= is not supported in compiled code\n",ns->path,line); return; }
        int opt= !(has_dflt && !dflt_none);
        sbf(&ty,"list[%s]%s",T,opt?" | None":"");
        if(opt) sbf(&init,"p.get_%ss(%s) if p.seen(%s) else None",get,q,q);
        else { if(!dflt_lit){ sbf(ns->err,"%s:%d: error: argparse: the default of '%s' must be a literal in compiled code\n",ns->path,line); return; }
            sbf(&init,"%s + p.get_%ss(%s) if p.seen(%s) else %s",dflt.s,get,q,q,dflt.s); } }
    else if(!strcmp(action,"store")){
        if(list){
            int always= positional && (strcmp(nargs,"*") || !has_dflt);
            if(positional && !strcmp(nargs,"*")){ sbf(&ty,"list[%s]",T); sbf(&init,"p.get_%ss(%s) if p.seen(%s) else ",get,q,q); if(has_dflt && !dflt_none && dflt_lit) sbf(&init,"%s",dflt.s); else sbf(&init,"[]"); }
            else if(always){ sbf(&ty,"list[%s]",T); sbf(&init,"p.get_%ss(%s)",get,q); }
            else { int opt= !(has_dflt && !dflt_none) && !required; sbf(&ty,"list[%s]%s",T,opt?" | None":""); sbf(&init,"p.get_%ss(%s) if p.seen(%s) else ",get,q,q); if(required) sbf(&init,"[]"); else DFLT(1); } }
        else if(!strcmp(nargs,"?")){
            int opt= !(has_dflt && !dflt_none);
            sbf(&ty,"%s%s",T,opt?" | None":"");
            SBuf c={0}; int hc= k_const && lit_text(k_const,&c);
            sbf(&init,"(p.get_%s(%s) if not p.none(%s) else %s) if p.seen(%s) else ",get,q,q,hc?c.s:"None",q); DFLT(1); free(c.s);
            if(hc && !opt){ /* (const and default given: both of T) */ } }
        else {
            if(positional || required){ sbf(&ty,"%s",T); sbf(&init,"p.get_%s(%s)",get,q); }
            else { int opt= !(has_dflt && !dflt_none); sbf(&ty,"%s%s",T,opt?" | None":""); sbf(&init,"p.get_%s(%s) if p.seen(%s) else ",get,q,q); DFLT(1); } } }
    else { sbf(ns->err,"%s:%d: error: argparse: action '%s' is not supported in compiled code\n",ns->path,line); return; }
    #undef DFLT
    ns_add(ns,dest,ty.s,init.s,line);
    free(ty.s); free(init.s); free(dflt.s);
}
static void ns_defaults(NsScan *ns, Expr *e){                      /* set_defaults(name=literal) */
    for(int i=0;i<e->count;i++){ Expr *a=e->items[i]; if(a->akind!=3) continue;
        SBuf v={0}; const char *t=lit_ty(a);
        if(!t || !lit_text(a,&v)){ sbf(ns->err,"%s:%d: error: argparse: set_defaults(%s=...) must be a literal in compiled code\n",ns->path,e->line,a->kw); free(v.s); continue; }
        int known=0; for(int k=0;k<ns->n;k++) if(!strcmp(ns->f[k].dest,a->kw)) known=1;
        if(!known) ns_add(ns,a->kw,t,v.s,e->line);
        free(v.s); }
}
static void ns_expr(NsScan *ns, Expr *e);
static void ns_stmts(NsScan *ns, Stmt **b, int n){
    for(int i=0;i<n;i++){ Stmt *s=b[i]; ns->line=s->line;
        ns_expr(ns,s->expr); ns_expr(ns,s->expr2); ns_expr(ns,s->value);
        for(int k=0;k<s->ntargets;k++) ns_expr(ns,s->targets[k]);
        ns_stmts(ns,s->body,s->body_count); ns_stmts(ns,s->orelse,s->orelse_count); }
}
static void ns_expr(NsScan *ns, Expr *e){
    if(!e) return;
    if(e->kind==EXPR_CALL && e->a && e->a->kind==EXPR_ATTRIBUTE){
        if(!strcmp(e->a->name,"add_argument")) ns_call(ns,e);
        else if(!strcmp(e->a->name,"set_defaults")) ns_defaults(ns,e);
        else if(!strcmp(e->a->name,"add_subparsers")){ Expr *d=kwarg(e,"dest");      /* the command chosen */
            if(d && str_lit(d)){ SBuf t={0}; sbf(&t,"p.get_str(\"%s\") if p.seen(\"%s\") else None",str_lit(d),str_lit(d)); ns_add(ns,str_lit(d),"str | None",t.s,e->line); free(t.s); } } }
    ns_expr(ns,e->a); ns_expr(ns,e->b); ns_expr(ns,e->c); ns_expr(ns,e->d);
    for(int i=0;i<e->count;i++) ns_expr(ns,e->items[i]);
    for(int i=0;i<e->vcount;i++) ns_expr(ns,e->vals[i]);
    for(int i=0;i<e->nclause;i++) ns_expr(ns,e->clauses[i].iter);
}
/* argparse.py's source with the program's Namespace class after it */
char *argparse_with_namespace(Units *us, const char *src, SBuf *err){
    NsScan ns; memset(&ns,0,sizeof ns); ns.err=err;
    for(int i=0;i<us->n;i++){ AotUnit *u=us->v[i]; if(!u->ast || !strcmp(u->name,"argparse")) continue;
        if(!u->src || !strstr(u->src,"add_argument")) continue;
        ns.path=u->path; ns_stmts(&ns,u->ast->body,u->ast->body_count); }
    SBuf b={0}; size_t sl=strlen(src);
    b.cap=sl+4096; b.s=(char*)xmalloc(b.cap); memcpy(b.s,src,sl); b.n=sl; b.s[sl]=0;      /* (the module's text, then the class) */
    sbf(&b,"\n\nif sys._compiled:\n    class Namespace(_NamespaceBase):\n");
    sbf(&b,"        \"\"\"The arguments parse_args() found: a field per destination.\"\"\"\n\n");
    sbf(&b,"        def __init__(self, p: _Parsed | None = None) -> None:\n            if p is None:\n                p = _Parsed()\n            self._dests: list[str] = p.dests\n");
    for(int i=0;i<ns.n;i++){ const char *t=ns.f[i].ty;                 /* (a destination of another parser: nothing) */
        const char *zero= strstr(t,"| None") ? "None" : !strncmp(t,"list",4) ? "[]" : !strcmp(t,"int") ? "0" : !strcmp(t,"float") ? "0.0" : !strcmp(t,"bool") ? "False" : "\"\"";
        sbf(&b,"            self.%s: %s = (%s) if p.has(\"%s\") else %s\n",ns.f[i].field,t,ns.f[i].init,ns.f[i].dest,zero); }
    sbf(&b,"\n        def _repr_of(self, d: str) -> str:\n");
    for(int i=0;i<ns.n;i++) sbf(&b,"            if d == \"%s\":\n                return repr(self.%s)\n",ns.f[i].dest,ns.f[i].field);
    sbf(&b,"            return \"None\"\n\n");
    sbf(&b,"        def __repr__(self) -> str:\n            parts: list[str] = []\n            for d in self._dests:\n");
    sbf(&b,"                parts.append(d + \"=\" + self._repr_of(d) if d.isidentifier() else repr(d) + \": \" + self._repr_of(d))\n");
    sbf(&b,"            return \"Namespace(\" + \", \".join(parts) + \")\"\n\n");
    sbf(&b,"        def __contains__(self, key: str) -> bool:\n            return key in self._dests\n");
    for(int i=0;i<ns.n;i++){ free(ns.f[i].dest); free(ns.f[i].field); free(ns.f[i].ty); free(ns.f[i].init); }
    free(ns.f);
    return b.s;
}
/* ---- optparse in compiled programs: parse_args() gives a Values object with a typed field per
   destination of the program's add_option() / make_option() calls; the parser records events
   (destination, action, option, value strings) that Values replays. Its class is written here
   and added to optparse.py's source. */
typedef struct { char *dest, *field, *ty, *dflt; int opt; SBuf apply; } OpField;
typedef struct { OpField *f; int n; SBuf *err; const char *path; } OpScan;
static OpField *op_field(OpScan *os, const char *dest){
    for(int i=0;i<os->n;i++) if(!strcmp(os->f[i].dest,dest)) return &os->f[i];
    os->f=(OpField*)xrealloc(os->f,sizeof(OpField)*(size_t)(os->n+1));
    OpField *f=&os->f[os->n++]; memset(f,0,sizeof *f); f->dest=xstrdup2(dest);
    char fl[256]; snprintf(fl,sizeof fl,"%s",dest); for(char *q=fl;*q;q++) if(!((*q>='a'&&*q<='z')||(*q>='A'&&*q<='Z')||(*q>='0'&&*q<='9')||*q=='_')) *q='_';
    f->field=xstrdup2(fl); f->opt=1;
    return f;
}
static int op_type_of(OpScan *os, OpField *f, const char *ty, int line){
    if(!f->ty){ f->ty=xstrdup2(ty); return 1; }
    if(strcmp(f->ty,ty)){ sbf(os->err,"%s:%d: error: optparse: '%s' is %s here and %s elsewhere (one type per destination in compiled code)\n",os->path,line,f->dest,ty,f->ty); return 0; }
    return 1;
}
static void op_call(OpScan *os, Expr *e){
    int line=e->line; const char *flags[16]; int nf=0;
    for(int i=0;i<e->count;i++){ Expr *a=e->items[i]; if(a->akind) continue; const char *s=str_lit(a); if(!s) return; if(nf<16) flags[nf++]=s; }
    if(!nf) return;
    Expr *k_action=kwarg(e,"action"), *k_type=kwarg(e,"type"), *k_dest=kwarg(e,"dest"), *k_default=kwarg(e,"default"),
         *k_nargs=kwarg(e,"nargs"), *k_const=kwarg(e,"const"), *k_choices=kwarg(e,"choices");
    if(kwarg(e,"callback")){ sbf(os->err,"%s:%d: error: optparse: callbacks are not supported in compiled code\n",os->path,line); return; }
    const char *action= k_action ? str_lit(k_action) : "store";
    if(!action){ sbf(os->err,"%s:%d: error: optparse: action= must be a string in compiled code\n",os->path,line); return; }
    if(!strcmp(action,"help") || !strcmp(action,"version")) return;
    { static const char *const known[]={"store","store_const","store_true","store_false","append","append_const","count",NULL};
      int ok=0; for(int i=0;known[i];i++) if(!strcmp(action,known[i])) ok=1;
      if(!ok) return; }                                             /* (an unknown one: the OptionError when it runs) */
    for(int i=0;i<nf;i++){ const char *o=flags[i]; size_t l=strlen(o);    /* (bad option strings: the OptionError too) */
        if(l<2 || o[0]!='-' || (l==2 && o[1]=='-') || (l>2 && (o[1]!='-' || o[2]=='-'))) return; }
    const char *type=NULL;
    if(k_type){ if(str_lit(k_type)) type=str_lit(k_type);
        else if(k_type->kind==EXPR_NAME) type=k_type->name;
        else { sbf(os->err,"%s:%d: error: optparse: type= must be a literal in compiled code\n",os->path,line); return; } }
    int store= !strcmp(action,"store") || !strcmp(action,"append");
    if(!type && store) type= k_choices ? "choice" : "string";
    const char *V="str";
    if(type){ if(!strcmp(type,"int") || !strcmp(type,"long")) V="int"; else if(!strcmp(type,"float")) V="float";
        else if(!strcmp(type,"string") || !strcmp(type,"str") || !strcmp(type,"choice")) V="str";
        else { sbf(os->err,"%s:%d: error: optparse: type '%s' is not supported in compiled code\n",os->path,line,type); return; } }
    char shorts[512]="", longs[512]="";
    for(int i=0;i<nf;i++){ char *b= strlen(flags[i])==2 ? shorts : longs; if(b[0]) strncat(b,"/",sizeof shorts-strlen(b)-1); strncat(b,flags[i],sizeof shorts-strlen(b)-1); }
    char id[1024]; snprintf(id,sizeof id,"%s%s%s",shorts,shorts[0]&&longs[0]?"/":"",longs);
    char dest[256]="";
    if(k_dest && str_lit(k_dest)) snprintf(dest,sizeof dest,"%s",str_lit(k_dest));
    else if(k_dest){ sbf(os->err,"%s:%d: error: optparse: dest= must be a string in compiled code\n",os->path,line); return; }
    else { for(int i=0;i<nf && !dest[0];i++) if(strlen(flags[i])>2) snprintf(dest,sizeof dest,"%s",flags[i]+2);
        if(!dest[0]) snprintf(dest,sizeof dest,"%c",flags[0][1]);
        for(char *q=dest;*q;q++) if(*q=='-') *q='_'; }
    int nargs=1;
    if(k_nargs){ if(k_nargs->kind==EXPR_LITERAL && k_nargs->tok->kind==T_NUMBER && !k_nargs->tok->is_float) nargs=(int)k_nargs->tok->i;
        else { sbf(os->err,"%s:%d: error: optparse: nargs= must be a literal in compiled code\n",os->path,line); return; } }
    OpField *f=op_field(os,dest);
    char conv[160], val[1200]; SBuf ty={0};
    #define CONV(k) (snprintf(conv,sizeof conv,!strcmp(V,"int")?"_parse_int(ev.vals[%d])":!strcmp(V,"float")?"float(ev.vals[%d])":"ev.vals[%d]",(k)),conv)
    if(store){
        if(nargs>1){ SBuf t={0}, v={0}; sbf(&t,"tuple["); sbf(&v,"(");
            for(int k=0;k<nargs;k++){ sbf(&t,"%s%s",k?", ":"",V); sbf(&v,"%s%s",k?", ":"",CONV(k)); }
            sbf(&t,"]"); sbf(&v,")"); snprintf(val,sizeof val,"%s",v.s);
            if(!strcmp(action,"append")) sbf(&ty,"list[%s]",t.s); else sbf(&ty,"%s",t.s); free(t.s); free(v.s); }
        else { snprintf(val,sizeof val,"%s",CONV(0)); if(!strcmp(action,"append")) sbf(&ty,"list[%s]",V); else sbf(&ty,"%s",V); } }
    else if(!strcmp(action,"store_true") || !strcmp(action,"store_false")){ sbf(&ty,"bool"); snprintf(val,sizeof val,"%s",action[6]=='t'?"True":"False"); }
    else if(!strcmp(action,"store_const") || !strcmp(action,"append_const")){
        SBuf c={0}; const char *ct=lit_ty(k_const);
        if(!k_const || !ct || !lit_text(k_const,&c)){ sbf(os->err,"%s:%d: error: optparse: const= must be a literal in compiled code\n",os->path,line); free(c.s); return; }
        snprintf(val,sizeof val,"%s",c.s); free(c.s);
        if(action[0]=='a') sbf(&ty,"list[%s]",ct); else sbf(&ty,"%s",ct); }
    else if(!strcmp(action,"count")){ sbf(&ty,"int"); snprintf(val,sizeof val,"1"); }
    else return;                                                    /* (an unknown one: the OptionError when it runs) */
    #undef CONV
    if(!op_type_of(os,f,ty.s,line)){ free(ty.s); return; }
    free(ty.s);
    if(k_default && k_default->kind!=EXPR_NONE){
        SBuf d={0}; const char *dt=lit_ty(k_default);
        if(!dt || !lit_text(k_default,&d)){ sbf(os->err,"%s:%d: error: optparse: the default of '%s' must be a literal in compiled code\n",os->path,line,dest); free(d.s); return; }
        int ok= !strcmp(dt,f->ty) || (!strcmp(dt,"str") && (!strcmp(f->ty,"int") || !strcmp(f->ty,"float"))) || (!strcmp(dt,"int") && !strcmp(f->ty,"float"));
        if(!ok){ sbf(os->err,"%s:%d: error: optparse: the default of '%s' is %s, its values %s (one type per destination in compiled code)\n",os->path,line,dest,dt,f->ty); free(d.s); return; }
        free(f->dflt); f->dflt=d.s; f->opt=0; }
    /* what the option does to the field */
    int list= !strncmp(f->ty,"list[",5);
    sbf(&f->apply,"            %s ev.opt == \"%s\":\n",f->apply.n?"elif":"if",id);
    if(!strcmp(action,"count")) sbf(&f->apply,"                self.%s = _inc(self.%s)\n",f->field,f->field);
    else if(list) sbf(&f->apply,"                self.%s = _app(self.%s, %s)\n",f->field,f->field,val);
    else sbf(&f->apply,"                self.%s = %s\n",f->field,val);
}
static void op_defaults(OpScan *os, Expr *e, int single){        /* set_defaults(name=literal), set_default("name", literal) */
    for(int i=0;i<e->count;i++){ Expr *a=e->items[i]; const char *name;
        if(single){ if(i || a->akind || !str_lit(a) || e->count<2) return; name=str_lit(a); a=e->items[1]; }
        else { if(a->akind!=3) continue; name=a->kw; }
        if(a->kind==EXPR_NONE){ if(single) return; continue; }
        const char *t=lit_ty(a); SBuf v={0};
        if(!t || !lit_text(a,&v)){ free(v.s); if(single) return; continue; }      /* (the runtime default text still applies to scalars) */
        OpField *f=op_field(os,name);
        if(!f->ty) f->ty=xstrdup2(t);
        free(f->dflt); f->dflt=v.s; f->opt=0;
        if(single) return; }
}
static void op_expr(OpScan *os, Expr *e);
static void op_stmts(OpScan *os, Stmt **b, int n){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        op_expr(os,s->expr); op_expr(os,s->expr2); op_expr(os,s->value);
        for(int k=0;k<s->ntargets;k++) op_expr(os,s->targets[k]);
        op_stmts(os,s->body,s->body_count); op_stmts(os,s->orelse,s->orelse_count); }
}
static void op_expr(OpScan *os, Expr *e){
    if(!e) return;
    if(e->kind==EXPR_CALL && e->a){
        const char *nm= e->a->kind==EXPR_ATTRIBUTE ? e->a->name : e->a->kind==EXPR_NAME ? e->a->name : NULL;
        if(nm && (!strcmp(nm,"add_option") || !strcmp(nm,"make_option"))) op_call(os,e);
        else if(nm && e->a->kind==EXPR_ATTRIBUTE && !strcmp(nm,"set_defaults")) op_defaults(os,e,0);
        else if(nm && e->a->kind==EXPR_ATTRIBUTE && !strcmp(nm,"set_default")) op_defaults(os,e,1); }
    op_expr(os,e->a); op_expr(os,e->b); op_expr(os,e->c); op_expr(os,e->d);
    for(int i=0;i<e->count;i++) op_expr(os,e->items[i]);
    for(int i=0;i<e->vcount;i++) op_expr(os,e->vals[i]);
    for(int i=0;i<e->nclause;i++) op_expr(os,e->clauses[i].iter);
}
/* optparse.py's source with the program's Values class after it */
char *optparse_with_values(Units *us, const char *src, SBuf *err){
    OpScan os; memset(&os,0,sizeof os); os.err=err;
    for(int i=0;i<us->n;i++){ AotUnit *u=us->v[i]; if(!u->ast || !strcmp(u->name,"optparse")) continue;
        if(!u->src || (!strstr(u->src,"add_option") && !strstr(u->src,"make_option"))) continue;
        os.path=u->path; op_stmts(&os,u->ast->body,u->ast->body_count); }
    SBuf b={0}; size_t sl=strlen(src);
    b.cap=sl+4096; b.s=(char*)xmalloc(b.cap); memcpy(b.s,src,sl); b.n=sl; b.s[sl]=0;
    sbf(&b,"\n\nif sys._compiled:\n    def _app(xs: list[_T] | None, v: _T) -> list[_T]:\n        if xs is None:\n            return [v]\n        xs.append(v)\n        return xs\n\n");
    sbf(&b,"    def _inc(n: int | None) -> int:\n        return 1 if n is None else n + 1\n\n");
    sbf(&b,"    class Values(_ValuesBase):\n        \"\"\"The option values parse_args() found: a field per destination.\"\"\"\n\n");
    sbf(&b,"        def __init__(self, p: _Parsed | None = None) -> None:\n            if p is None:\n                p = _Parsed()\n            self._dests: list[str] = p.dests\n");
    for(int i=0;i<os.n;i++){ OpField *f=&os.f[i]; const char *t=f->ty?f->ty:"str"; const char *q=f->dest;
        int list= !strncmp(t,"list[",5) || !strncmp(t,"tuple[",6);
        if(f->opt){
            if(list) sbf(&b,"            self.%s: %s | None = None\n",f->field,t);
            else sbf(&b,"            self.%s: %s | None = p.d_%s(\"%s\")\n",f->field,t,t,q); }
        else {
            const char *zero= list ? "[]" : !strcmp(t,"int") ? "0" : !strcmp(t,"float") ? "0.0" : !strcmp(t,"bool") ? "False" : "\"\"";
            if(list) sbf(&b,"            self.%s: %s = list(%s) if p.has(\"%s\") else %s\n",f->field,t,f->dflt,q,zero);
            else sbf(&b,"            self.%s: %s = %s\n            _%s = p.d_%s(\"%s\")\n            if _%s is not None:\n                self.%s = _%s\n",
                     f->field,t,zero,f->field,t,q,f->field,f->field,f->field); } }
    sbf(&b,"            for ev in p.events:\n                self._apply(ev)\n\n");
    sbf(&b,"        def _apply(self, ev: _Event) -> None:\n            d = ev.dest\n");
    int any=0;
    for(int i=0;i<os.n;i++){ OpField *f=&os.f[i]; if(!f->apply.n) continue;
        sbf(&b,"            %s d == \"%s\":\n",any?"elif":"if",f->dest); any=1;
        for(char *l=f->apply.s;*l;){ char *nl=strchr(l,'\n'); sbf(&b,"    %.*s\n",(int)(nl-l),l); l=nl+1; } }
    if(!any) sbf(&b,"            pass\n");
    sbf(&b,"\n        def _repr_of(self, d: str) -> str:\n");
    for(int i=0;i<os.n;i++) sbf(&b,"            if d == \"%s\":\n                return repr(self.%s)\n",os.f[i].dest,os.f[i].field);
    sbf(&b,"            return \"None\"\n\n");
    sbf(&b,"        def __str__(self) -> str:\n            parts: list[str] = []\n            for d in self._dests:\n                parts.append(repr(d) + \": \" + self._repr_of(d))\n            return \"{\" + \", \".join(parts) + \"}\"\n\n");
    sbf(&b,"        def __repr__(self) -> str:\n            return \"<Values at 0x%%x: %%s>\" %% (id(self), str(self))\n");
    for(int i=0;i<os.n;i++){ free(os.f[i].dest); free(os.f[i].field); free(os.f[i].ty); free(os.f[i].dflt); free(os.f[i].apply.s); }
    free(os.f);
    return b.s;
}
/* ---- types.SimpleNamespace in compiled programs: a class with a field for each keyword name the
   program's SimpleNamespace(...) calls give (in that order), appended to types.py's source. */
typedef struct { char **v; int n; } SnNames;
static void sn_expr(SnNames *sn, Expr *e);
static void sn_stmts(SnNames *sn, Stmt **b, int n){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        sn_expr(sn,s->expr); sn_expr(sn,s->expr2); sn_expr(sn,s->value);
        for(int k=0;k<s->ntargets;k++) sn_expr(sn,s->targets[k]);
        sn_stmts(sn,s->body,s->body_count); sn_stmts(sn,s->orelse,s->orelse_count); }
}
static void sn_expr(SnNames *sn, Expr *e){
    if(!e) return;
    if(e->kind==EXPR_CALL && e->a && ((e->a->kind==EXPR_NAME && !strcmp(e->a->name,"SimpleNamespace")) ||
                                      (e->a->kind==EXPR_ATTRIBUTE && !strcmp(e->a->name,"SimpleNamespace"))))
        for(int i=0;i<e->count;i++){ Expr *a=e->items[i]; if(a->akind!=3 || !a->kw) continue;
            int have=0; for(int k=0;k<sn->n;k++) if(!strcmp(sn->v[k],a->kw)) have=1;
            if(!have){ sn->v=(char**)xrealloc(sn->v,sizeof(char*)*(size_t)(sn->n+1)); sn->v[sn->n++]=xstrdup2(a->kw); } }
    sn_expr(sn,e->a); sn_expr(sn,e->b); sn_expr(sn,e->c); sn_expr(sn,e->d);
    for(int i=0;i<e->count;i++) sn_expr(sn,e->items[i]);
    for(int i=0;i<e->vcount;i++) sn_expr(sn,e->vals[i]);
    for(int i=0;i<e->nclause;i++) sn_expr(sn,e->clauses[i].iter);
}
char *types_with_namespace(Units *us, const char *src){
    SnNames sn={0};
    for(int i=0;i<us->n;i++){ AotUnit *u=us->v[i]; if(!u->ast || !strcmp(u->name,"types")) continue;
        if(!u->src || !strstr(u->src,"SimpleNamespace")) continue;
        sn_stmts(&sn,u->ast->body,u->ast->body_count); }
    SBuf b={0}; size_t sl=strlen(src);
    b.cap=sl+4096; b.s=(char*)xmalloc(b.cap); memcpy(b.s,src,sl); b.n=sl; b.s[sl]=0;
    sbf(&b,"\n\nif sys._compiled:\n    class SimpleNamespace:\n");
    sbf(&b,"        \"\"\"An object whose attributes are the keyword arguments (those the program gives anywhere).\"\"\"\n\n");
    sbf(&b,"        def __init__(self");
    if(sn.n) sbf(&b,", *");
    for(int i=0;i<sn.n;i++) sbf(&b,", %s=None",sn.v[i]);
    sbf(&b,") -> None:\n            self._given: list[str] = []\n");
    for(int i=0;i<sn.n;i++) sbf(&b,"            self.%s = %s\n            if %s is not None:\n                self._given.append(\"%s\")\n",sn.v[i],sn.v[i],sn.v[i],sn.v[i]);
    sbf(&b,"\n        def _repr_of(self, k: str) -> str:\n");
    for(int i=0;i<sn.n;i++) sbf(&b,"            if k == \"%s\":\n                return repr(self.%s)\n",sn.v[i],sn.v[i]);
    sbf(&b,"            return \"None\"\n\n");
    sbf(&b,"        def __repr__(self) -> str:\n            return \"namespace(\" + \", \".join([k + \"=\" + self._repr_of(k) for k in self._given]) + \")\"\n");
    for(int i=0;i<sn.n;i++) free(sn.v[i]);
    free(sn.v);
    return b.s;
}
