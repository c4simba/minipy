/* ========================= Typed compiler, types: match =========================
   match statements rewritten into if / elif chains. */

#include "check.h"

/* A match statement becomes ifs once its subject's type is known: each pattern tests (and
   binds) parts of the subject, kept in hidden variables; a pattern no value of that type can
   match is left out (CPython would just not match it). */
typedef struct { Stmt **v; int n, cap; } SL;
static void sl_add(SL *l, Stmt *s){ if(l->n==l->cap){ l->cap=l->cap?l->cap*2:8; l->v=(Stmt**)xrealloc(l->v,sizeof(Stmt*)*(size_t)l->cap); } l->v[l->n++]=s; }
static void sl_cat(SL *l, SL *m){ for(int i=0;i<m->n;i++) sl_add(l,m->v[i]); }
Expr *nx(const char *name, int line){ return name_expr(xstrdup2(name),line); }
Stmt *st_assign(Expr *t, Expr *v, int line){ Stmt *s=stmt_new(STMT_ASSIGN,NULL,line); s->targets=MPY_NEW_ARR(Expr*,1); s->targets[0]=t; s->ntargets=1; s->value=v; return s; }
static Stmt *st_if(Expr *cond, SL *body, int line){ Stmt *s=stmt_new(STMT_IF,NULL,line); s->expr=cond; for(int i=0;i<body->n;i++) stmt_add_body(s,body->v[i]); if(!body->n) stmt_add_body(s,stmt_new(STMT_PASS,NULL,line)); return s; }
static Expr *ex_cmp(Expr *a, int code, Expr *b, int line){ Expr *e=xnew(EXPR_COMPARE,line); xpush(e,a); b->akind=code; xpush(e,b); return e; }
Expr *ex_call(const char *fn, Expr *a, Expr *b, int line){ Expr *e=xnew(EXPR_CALL,line); e->a=nx(fn,line); if(a) xpush(e,a); if(b) xpush(e,b); return e; }
Expr *ex_index(Expr *o, Expr *i, int line){ Expr *e=xnew(EXPR_INDEX,line); e->a=o; e->b=i; return e; }
static Expr *ex_int(Ck *c, long long v, int line){ char t[32]; snprintf(t,sizeof t,"%lld",v); return py_front_expr(c->mod->unit->path,t,line); }
static Expr *ex_not(Expr *a, int line){ Expr *e=xnew(EXPR_UNARY,line); e->op=T_NOT; e->a=a; return e; }
const char *hidden_for(Ck *c, const char *base){ char h[64]; named_hidden(c,base,h,sizeof h); return xstrdup2(h); }
/* class C's positional sub-patterns name these attributes (__match_args__; a dataclass's fields) */
static const char *match_arg(Ck *c, AClass *k, int i, int line){
    for(AClass *b=k;b;b=b->base){
        AField *fd=NULL; for(int j=0;j<b->nfields;j++) if(!strcmp(b->fields[j]->name,"__match_args__")) fd=b->fields[j];
        if(fd && fd->init){ Expr *t=fd->init;
            if(t->kind!=EXPR_TUPLE) ck_err(c,line,"%s.__match_args__ must be a tuple of strings",k->name);
            if(i>=t->count) ck_err(c,line,"%s() accepts %d positional sub-pattern%s (%d given)",k->name,t->count,t->count==1?"":"s",i+1);
            if(t->items[i]->kind!=EXPR_LITERAL || t->items[i]->tok->kind!=T_STRING) ck_err(c,line,"%s.__match_args__ must be a tuple of strings",k->name);
            return t->items[i]->tok->text; }
        if(b->dc){ int n=0; for(int j=0;j<b->dc->n;j++){ DcField *f=&b->dc->f[j]; if(!f->init || f->kw_only || f->initvar) continue; if(n++==i) return f->name; }
            ck_err(c,line,"%s() accepts %d positional sub-pattern%s (%d given)",k->name,n,n==1?"":"s",i+1); }
    }
    ck_err(c,line,"%s() accepts 0 positional sub-patterns (%d given)",k->name,i+1);
}
static int match_pat(Ck *c, const char *xv, Ty *T, Expr *p, SL *inner, SL *out);
/* the patterns of a sequence pattern from item i on (xv's items), then inner */
static int match_items(Ck *c, const char *xv, Ty *T, Expr *p, int i, int n, int star, SL *inner, SL *out, int line){
    if(i==n){ sl_cat(out,inner); return 1; }
    Expr *it=p->items[i]; Ty *base=ty_find(T);
    if(it->akind==PAT_STAR){                                    /* *rest: a list of the middle items */
        if(it->name){ Expr *sl=xnew(EXPR_SLICE,line); sl->a=nx(xv,line);
            if(base->k==TY_TUPLE){
                Expr *tl=xnew(EXPR_LIST,line); for(int k=i;k<base->nelems-(n-1-i);k++) xpush(tl,ex_index(nx(xv,line),ex_int(c,k,line),line));
                sl_add(out,st_assign(nx(it->name,line),tl,line)); }
            else { sl->b=ex_int(c,i,line); sl->c=xnew(EXPR_BINARY,line); sl->c->op=T_MINUS; sl->c->a=ex_call("\001len",nx(xv,line),NULL,line); sl->c->b=ex_int(c,n-1-i,line);
                Expr *v= base->tup ? ex_call("\001list",sl,NULL,line) : sl;
                sl_add(out,st_assign(nx(it->name,line),v,line)); } }
        return match_items(c,xv,T,p,i+1,n,star,inner,out,line); }
    Ty *et; Expr *idx;
    if(base->k==TY_TUPLE){ int k= star<0||i<star ? i : base->nelems-(n-i); et=base->elems[k]; idx=ex_int(c,k,line); }
    else { et=base->elem; idx= star<0||i<star ? ex_int(c,i,line) : ex_int(c,i-n,line); }
    const char *h=hidden_for(c,"item");
    SL rest={0}; if(!match_items(c,xv,T,p,i+1,n,star,inner,&rest,line)) return 0;
    SL here={0}; if(!match_pat(c,h,et,it,&rest,&here)) return 0;
    sl_add(out,st_assign(nx(h,line),ex_index(nx(xv,line),idx,line),line));
    sl_cat(out,&here);
    return 1;
}
static int match_pat(Ck *c, const char *xv, Ty *T, Expr *p, SL *inner, SL *out){
    int line=p->line; T=ty_find(T); int opt=ty_opt(T);
    switch(p->akind){
        case PAT_AS:{
            SL in2={0}; if(p->name) sl_add(&in2,st_assign(nx(p->name,line),nx(xv,line),line)); sl_cat(&in2,inner);
            if(!p->a){ sl_cat(out,&in2); return 1; }
            return match_pat(c,xv,T,p->a,&in2,out); }
        case PAT_VALUE:{
            Ty *tv=ty_find(ck_expr(c,p->a));
            if(tv->k==TY_VAR){ pending(c,line,"a pattern's value"); return 0; }
            int ok=(numeric(T)&&numeric(tv)) || (T->k==tv->k && T->k!=TY_OBJ) || (T->k==TY_OBJ && tv->k==TY_OBJ && (aot_subclass(T->cls,tv->cls)||aot_subclass(tv->cls,T->cls)));
            if(!ok) return 0;
            sl_add(out,st_if(ex_cmp(nx(xv,line),CMP_EQ,p->a,line),inner,line)); return 1; }
        case PAT_SINGLETON:
            if(p->a->kind==EXPR_NONE){ if(!opt && T->k!=TY_VOID) return 0;
                sl_add(out,st_if(ex_cmp(nx(xv,line),CMP_IS,xnew(EXPR_NONE,line),line),inner,line)); return 1; }
            if(T->k!=TY_BOOL) return 0;
            sl_add(out,st_if(ex_cmp(nx(xv,line),CMP_EQ,xnew(p->a->kind,line),line),inner,line)); return 1;
        case PAT_OR:{
            const char *hf=hidden_for(c,"alt"); int any=0;
            SL alts={0}; sl_add(&alts,st_assign(nx(hf,line),xnew(EXPR_FALSE,line),line));
            for(int i=0;i<p->count;i++){ SL set={0}, one={0}; sl_add(&set,st_assign(nx(hf,line),xnew(EXPR_TRUE,line),line));
                if(!match_pat(c,xv,T,p->items[i],&set,&one)) continue;
                any=1; if(i==0) sl_cat(&alts,&one); else sl_add(&alts,st_if(ex_not(nx(hf,line),line),&one,line)); }
            if(!any) return 0;
            sl_cat(out,&alts); sl_add(out,st_if(nx(hf,line),inner,line)); return 1; }
        case PAT_SEQ:{
            if(T->k!=TY_LIST && !(T->k==TY_TUPLE && !T->names)) return 0;
            int n=p->count, star=-1; for(int i=0;i<n;i++) if(p->items[i]->akind==PAT_STAR) star=i;
            SL body={0};
            if(T->k==TY_TUPLE){ if(star<0 ? T->nelems!=n : T->nelems<n-1) return 0;
                if(!match_items(c,xv,T,p,0,n,star,inner,&body,line)) return 0;
                if(opt) sl_add(out,st_if(ex_cmp(nx(xv,line),CMP_ISNOT,xnew(EXPR_NONE,line),line),&body,line)); else sl_cat(out,&body);
                return 1; }
            if(!match_items(c,xv,T,p,0,n,star,inner,&body,line)) return 0;
            Expr *cond=ex_cmp(ex_call("\001len",nx(xv,line),NULL,line),star<0?CMP_EQ:CMP_GE,ex_int(c,star<0?n:n-1,line),line);
            if(opt){ Expr *b=xnew(EXPR_BOOL,line); b->op=T_AND; b->a=ex_cmp(nx(xv,line),CMP_ISNOT,xnew(EXPR_NONE,line),line); b->b=cond; cond=b; }
            sl_add(out,st_if(cond,&body,line)); return 1; }
        case PAT_MAP:{
            if(T->k!=TY_DICT) return 0;
            SL cur={0}; sl_cat(&cur,inner);
            const char **keys=MPY_NEW_ARR(const char*,p->count+1);
            for(int i=0;i<p->count;i++) keys[i]=hidden_for(c,"key");
            if(p->name){ SL r={0}; sl_add(&r,st_assign(nx(p->name,line),ex_call("\001dict",nx(xv,line),NULL,line),line));
                for(int i=0;i<p->count;i++){ Stmt *d=stmt_new(STMT_DEL,NULL,line); d->targets=MPY_NEW_ARR(Expr*,1); d->targets[0]=ex_index(nx(p->name,line),nx(keys[i],line),line); d->ntargets=1; sl_add(&r,d); }
                sl_cat(&r,&cur); cur=r; }
            for(int i=p->count-1;i>=0;i--){ const char *hv=hidden_for(c,"value");
                SL here={0}; if(!match_pat(c,hv,T->elem,p->vals[i],&cur,&here)) return 0;
                SL got={0}; sl_add(&got,st_assign(nx(hv,line),ex_index(nx(xv,line),nx(keys[i],line),line),line)); sl_cat(&got,&here);
                SL step={0}; sl_add(&step,st_assign(nx(keys[i],line),p->items[i],line));
                sl_add(&step,st_if(ex_cmp(nx(keys[i],line),CMP_IN,nx(xv,line),line),&got,line));
                cur=step; }
            if(opt) sl_add(out,st_if(ex_cmp(nx(xv,line),CMP_ISNOT,xnew(EXPR_NONE,line),line),&cur,line)); else sl_cat(out,&cur);
            return 1; }
        case PAT_CLASS:{
            AClass *k=class_expr(c,p->a); const char *subj=xv; SL pre={0}; Expr *cond=NULL;
            if(!k){
                if(p->a->kind!=EXPR_NAME || !is_builtin_type_name(p->a->name)) ck_err(c,line,"%s is not a class",p->a->kind==EXPR_NAME?p->a->name:"the pattern");
                const char *bn=p->a->name;
                int ok= (!strcmp(bn,"int")&&(T->k==TY_INT||T->k==TY_BOOL)) || (!strcmp(bn,"bool")&&T->k==TY_BOOL) || (!strcmp(bn,"float")&&T->k==TY_FLOAT)
                     || (!strcmp(bn,"str")&&T->k==TY_STR) || (!strcmp(bn,"bytes")&&T->k==TY_BYTES) || (!strcmp(bn,"list")&&T->k==TY_LIST&&!T->tup)
                     || (!strcmp(bn,"tuple")&&(T->k==TY_TUPLE||(T->k==TY_LIST&&T->tup))) || (!strcmp(bn,"dict")&&T->k==TY_DICT) || (!strcmp(bn,"set")&&T->k==TY_SET) || !strcmp(bn,"object");
                if(!ok) return 0;
                for(int i=0;i<p->count;i++) if(p->items[i]->kw) ck_err(c,line,"%s() patterns take no keyword sub-patterns in compiled code",bn);
                if(p->count>1) ck_err(c,line,"%s() accepts 1 positional sub-pattern (%d given)",bn,p->count);
                SL body={0}; if(p->count){ if(!match_pat(c,xv,T,p->items[0],inner,&body)) return 0; } else sl_cat(&body,inner);
                if(opt) sl_add(out,st_if(ex_cmp(nx(xv,line),CMP_ISNOT,xnew(EXPR_NONE,line),line),&body,line)); else sl_cat(out,&body);
                return 1; }
            if(T->k!=TY_OBJ) return 0;
            if(aot_subclass(T->cls,k)){ if(opt) cond=ex_cmp(nx(xv,line),CMP_ISNOT,xnew(EXPR_NONE,line),line); }
            else if(aot_subclass(k,T->cls)){                       /* isinstance(x, C): x as a C from there on */
                Expr *kx=nx(k->symname,line);
                cond=ex_call("\001isinstance",nx(xv,line),kx,line);
                subj=hidden_for(c,"as");
                sl_add(&pre,st_assign(nx(subj,line),ex_call("\001downcast",nx(k->symname,line),nx(xv,line),line),line)); }
            else return 0;
            SL cur={0}; sl_cat(&cur,inner);
            for(int i=p->count-1;i>=0;i--){ Expr *sp=p->items[i]; int pos=0; for(int j=0;j<i;j++) if(!p->items[j]->kw) pos++;
                const char *an= sp->kw ? sp->kw : match_arg(c,k,pos,line);
                AField *fd=aot_find_field(k,an); AFunc *pm=fd?NULL:aot_find_method(k,an);
                if(!fd && !(pm && pm->is_property)) ck_err(c,line,"%s has no attribute '%s'",k->name,an);
                const char *ha=hidden_for(c,"attr");
                SL here={0}; if(!match_pat(c,ha,fd?fd->ty:pm->ret,sp,&cur,&here)) return 0;
                Expr *at=xnew(EXPR_ATTRIBUTE,line); at->a=nx(subj,line); at->name=(char*)an;
                SL step={0}; sl_add(&step,st_assign(nx(ha,line),at,line)); sl_cat(&step,&here); cur=step; }
            SL body={0}; sl_cat(&body,&pre); sl_cat(&body,&cur);
            if(cond) sl_add(out,st_if(cond,&body,line)); else sl_cat(out,&body);
            return 1; }
        default: ck_err(c,line,"unsupported pattern");
    }
}
/* match subject: case p [if guard]: body ... -> ifs (in place; the statement is checked as that) */
int desugar_match(Ck *c, Stmt *s){
    Expr *subject=s->expr; int line=s->line;
    Ty *T=ty_find(ck_expr(c,subject));
    if(!ty_known(T)){ pending(c,line,"the match subject"); return 0; }
    const char *m=hidden_for(c,"subject"), *done=hidden_for(c,"matched");
    SL all={0};
    sl_add(&all,st_assign(nx(m,line),subject,line));
    sl_add(&all,st_assign(nx(done,line),xnew(EXPR_FALSE,line),line));
    int first=1;
    for(int i=0;i<s->body_count;i++){ Stmt *cs=s->body[i];
        SL inner={0}, body={0};
        sl_add(&body,st_assign(nx(done,cs->line),xnew(EXPR_TRUE,cs->line),cs->line));
        for(int k=0;k<cs->body_count;k++) sl_add(&body,cs->body[k]);
        if(cs->expr2) sl_add(&inner,st_if(cs->expr2,&body,cs->line)); else sl_cat(&inner,&body);
        SL got={0};
        if(!match_pat(c,m,T,cs->expr,&inner,&got)) continue;            /* (no value of this type matches it) */
        if(first) sl_cat(&all,&got); else sl_add(&all,st_if(ex_not(nx(done,cs->line),cs->line),&got,cs->line));
        first=0; }
    s->kind=STMT_IF; s->expr=xnew(EXPR_TRUE,line); s->body=all.v; s->body_count=all.n; s->body_cap=all.cap; s->aux=NULL;
    return 1;
}
