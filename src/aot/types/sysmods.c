/* ========================= Typed compiler, types: built-in modules =========================
   sys, asyncio, math, time, random; ctypes calls. */

#include "check.h"
#include "targets/targets.h"

/* sys.exception() in compiled code: the exception of the except clause it is in (lexically; None
   outside one). A clause without `as` gets a hidden variable for it. */
Ty *lexical_exception(Ck *c, Expr *e){
    XInfo *xi=xinfo(e);
    int k=c->nhst-1;
    if(k<0 || k>=64 || c->hst[k].fn!=c->fn){ e->kind=EXPR_NONE; e->a=NULL; e->count=0; xi->kind=0; return ck_expr_inner(c,e); }
    Stmt *b=c->hst[k].b;
    if(!b->name){ char h[40]; snprintf(h,sizeof h," exc%d",c->nhidden++); b->name=xstrdup2(h); }
    AVar *v=(AVar*)b->aux;
    if(!v){ v=scope_hidden(c,b->name); b->aux=v; expect(c,v->ty,c->hst[k].ty,b->line,"the exception variable"); }
    if(!lookup(c,b->name)) cs_push(c,b->name,v,b->line);          /* (made in this pass: in scope from here) */
    e->kind=EXPR_NAME; e->name=b->name; e->a=NULL; e->count=0; e->items=NULL;
    xi->kind=X_VAR; xi->var=v;
    return v->ty;
}
Expr *lit_int(int line, long long v){ Expr *x=xnew(EXPR_LITERAL,line); x->tok=PPY_NEW0(Tok); x->tok->kind=T_NUMBER; x->tok->i=v;
    char b[32]; snprintf(b,sizeof b,"%lld",v); x->tok->text=xstrdup2(b); x->tok->len=(int64_t)strlen(b); x->tok->line=line; return x; }
Expr *lit_str(int line, const char *v){ Expr *x=xnew(EXPR_LITERAL,line); x->tok=PPY_NEW0(Tok); x->tok->kind=T_STRING;
    x->tok->text=xstrdup2(v); x->tok->len=(int64_t)strlen(v); x->tok->line=line; return x; }
/* sys._site("line" / "file" / "func" / "module") (the call `d` of a parameter's default, for a
   call at line): where that call is, as a literal (logging's records: lineno, pathname, funcName) */
Expr *site_value(Ck *c, Expr *d, int line){
    if(d->kind!=EXPR_CALL || d->count!=1 || d->a->kind!=EXPR_ATTRIBUTE || strcmp(d->a->name,"_site") || d->a->a->kind!=EXPR_NAME || strcmp(d->a->a->name,"sys")) return NULL;
    Expr *w=d->items[0]; if(w->kind!=EXPR_LITERAL || w->tok->kind!=T_STRING) return NULL;
    const char *k=w->tok->text, *path=c->mod && c->mod->unit && c->mod->unit->path ? c->mod->unit->path : "<string>";
    if(!strcmp(k,"line")) return lit_int(line,line);
    if(!strcmp(k,"file")) return lit_str(line,path);
    if(!strcmp(k,"func")){ AFunc *f=c->fn; return lit_str(line,!is_func(f)?"<module>":f->lam?"<lambda>":f->name?f->name:"<module>"); }
    if(!strcmp(k,"module")){ const char *b=strrchr(path,'/'); b=b?b+1:path; char m[256]; snprintf(m,sizeof m,"%s",b);
        char *dot=strrchr(m,'.'); if(dot && dot!=m) *dot=0; return lit_str(line,m); }
    if(!strcmp(k,"name")) return lit_str(line,c->mod && c->mod->name ? c->mod->name : "__main__");      /* its __name__ */
    if(!strcmp(k,"frame")){                                                      /* a traceback's lines for the call: */
        Expr *fe=lit_str(line,"func"), *se=lit_str(line,"source");               /* File "...", line N, in f / its source */
        Expr *fq=xnew(EXPR_CALL,line); fq->a=xnew(EXPR_ATTRIBUTE,line); fq->a->a=name_expr("sys",line); fq->a->name="_site"; xpush(fq,fe);
        Expr *sq=xnew(EXPR_CALL,line); sq->a=xnew(EXPR_ATTRIBUTE,line); sq->a->a=name_expr("sys",line); sq->a->name="_site"; xpush(sq,se);
        Expr *fv=site_value(c,fq,line), *sv=site_value(c,sq,line);
        const char *src=sv->tok->text;
        size_t n=strlen(path)+strlen(fv->tok->text)+strlen(src)+64; char *t=(char*)xmalloc(n);
        if(*src) snprintf(t,n,"  File \"%s\", line %d, in %s\n    %s\n",path,line,fv->tok->text,src);
        else snprintf(t,n,"  File \"%s\", line %d, in %s\n",path,line,fv->tok->text);
        const char *cn=c->site_callee; size_t cl=cn?strlen(cn):0;          /* as CPython: ~ under the callee, ^ under its (arguments) */
        const char *at=NULL; for(const char *q=src;cn && (q=strstr(q,cn));q++){ if(q[cl]=='(' && (q==src || !(isalnum((unsigned char)q[-1])||q[-1]=='_'))) at=q; }
        if(at){ const char *b=at; while(b>src && (isalnum((unsigned char)b[-1])||b[-1]=='_'||b[-1]=='.')) b--;
            const char *q=at+cl; int depth=0; char quote=0; const char *end=NULL;
            for(;*q;q++){ if(quote){ if(*q=='\\' && q[1]) q++; else if(*q==quote) quote=0; continue; }
                if(*q=='\'' || *q=='"') quote=*q; else if(*q=='(') depth++; else if(*q==')' && --depth==0){ end=q+1; break; } }
            if(end){ size_t k=strlen(t), need=k+8+(size_t)(end-src)+2; t=(char*)xrealloc(t,need);
                char *w=t+k; memcpy(w,"    ",4); w+=4;
                for(const char *z=src;z<b;z++) *w++=' ';
                for(const char *z=b;z<at+cl;z++) *w++='~';
                for(const char *z=at+cl;z<end;z++) *w++='^';
                *w++='\n'; *w=0; } }
        Expr *x=lit_str(line,t); free(t); return x; }
    if(!strcmp(k,"source")){                                                     /* that line of the source, stripped */
        const char *src=c->mod && c->mod->unit ? c->mod->unit->src : NULL; int n=1;
        if(!src) return lit_str(line,"");
        while(*src && n<line){ if(*src=='\n') n++; src++; }
        while(*src==' ' || *src=='\t') src++;
        const char *end=src; while(*end && *end!='\n' && *end!='\r') end++;
        while(end>src && (end[-1]==' ' || end[-1]=='\t')) end--;
        char *t=(char*)xmalloc((size_t)(end-src)+1); memcpy(t,src,(size_t)(end-src)); t[end-src]=0;
        Expr *x=lit_str(line,t); free(t); return x; }
    return NULL;
}
Ty *ck_sys(Ck *c, Expr *e, const char *m){
    XInfo *xi=xinfo(e); xi->kind=X_SYS; xi->name=m; int line=e->line;
    if(!strcmp(m,"syscall")){
        xi->kind=X_SYSCALL;
        ck_positional(c,e,1,7,"syscall");
        for(int i=0;i<e->count;i++){ Ty *t=ty_find(arg(c,e,i));
            if(t->k==TY_VAR){ pending(c,line,"a syscall argument"); continue; }
            if(t->k!=TY_INT&&t->k!=TY_BOOL&&t->k!=TY_STR&&t->k!=TY_BYTES&&t->k!=TY_BUF) ck_err(c,line,"syscall() arguments are int, str, bytes or buffer, not %s",ty_name(t)); }
        return ty_new(TY_LIST,TY_INT_T,NULL);
    }
    if(!strcmp(m,"buffer")){ ck_positional(c,e,1,1,m); Ty *t=ty_find(arg(c,e,0)); if(t->k!=TY_INT&&t->k!=TY_STR&&t->k!=TY_BYTES&&t->k!=TY_VAR) ck_err(c,line,"buffer() takes a size, a str or bytes"); return TY_BUF_T; }
    if(!strcmp(m,"poke")){ ck_positional(c,e,4,4,m); expect(c,TY_BUF_T,arg(c,e,0),line,"the buffer"); for(int i=1;i<4;i++) expect(c,TY_INT_T,arg(c,e,i),line,"a poke() argument"); return TY_VOID_T; }
    if(!strcmp(m,"peek")){ ck_positional(c,e,3,3,m); expect(c,TY_BUF_T,arg(c,e,0),line,"the buffer"); for(int i=1;i<3;i++) expect(c,TY_INT_T,arg(c,e,i),line,"a peek() argument"); return TY_INT_T; }
    if(!strcmp(m,"poke_str")){ ck_positional(c,e,3,3,m); expect(c,TY_BUF_T,arg(c,e,0),line,"the buffer"); expect(c,TY_INT_T,arg(c,e,1),line,"the offset"); expect(c,TY_STR_T,arg(c,e,2),line,"the string"); return TY_VOID_T; }
    if(!strcmp(m,"peek_str")){ ck_positional(c,e,3,3,m); expect(c,TY_BUF_T,arg(c,e,0),line,"the buffer"); expect(c,TY_INT_T,arg(c,e,1),line,"the offset"); expect(c,TY_INT_T,arg(c,e,2),line,"the length"); return TY_STR_T; }
    if(!strcmp(m,"poke_bytes")){ ck_positional(c,e,3,3,m); expect(c,TY_BUF_T,arg(c,e,0),line,"the buffer"); expect(c,TY_INT_T,arg(c,e,1),line,"the offset"); expect(c,TY_BYTES_T,arg(c,e,2),line,"the bytes"); return TY_VOID_T; }
    if(!strcmp(m,"peek_bytes")){ ck_positional(c,e,3,3,m); expect(c,TY_BUF_T,arg(c,e,0),line,"the buffer"); expect(c,TY_INT_T,arg(c,e,1),line,"the offset"); expect(c,TY_INT_T,arg(c,e,2),line,"the length"); return TY_BYTES_T; }
    if(!strcmp(m,"addr")){ ck_positional(c,e,1,1,m); expect(c,TY_BUF_T,arg(c,e,0),line,"the buffer"); return TY_INT_T; }
    if(!strcmp(m,"_sleep")){ ck_positional(c,e,1,1,m); expect(c,TY_FLOAT_T,arg(c,e,0),line,"the seconds"); return TY_VOID_T; }   /* (time.py) */
    if(!strcmp(m,"_rawargs")){ ck_positional(c,e,1,1,m); expect(c,TY_INT_T,arg(c,e,0),line,"the kind"); return ty_new(TY_LIST,TY_STR_T,NULL); }   /* (_sysargs.py) */
    if(!strcmp(m,"_gc_collect")){ ck_positional(c,e,0,0,m); return TY_INT_T; }   /* (gc.py: the cycle collector) */
    if(!strcmp(m,"exit")){ ck_positional(c,e,0,1,m); if(e->count) expect(c,TY_INT_T,arg(c,e,0),line,"the exit code"); return TY_VOID_T; }
    if(!strcmp(m,"exception")){ ck_positional(c,e,0,0,m); return lexical_exception(c,e); }
    if(!strcmp(m,"_site")){ Expr *v=site_value(c,e,line); if(!v) ck_err(c,line,"sys._site() takes \"line\", \"file\", \"func\", \"module\", \"name\" or \"source\"");
        int ak=e->akind; char *kw=e->kw; *e=*v; e->akind=ak; e->kw=kw; e->ty=NULL; return ck_expr(c,e); }
    /* raw memory at an address a system call gave (nothing is checked) */
    if(!strcmp(m,"peek_at")){ ck_positional(c,e,2,2,m); expect(c,TY_INT_T,arg(c,e,0),line,"the address"); expect(c,TY_INT_T,arg(c,e,1),line,"the size"); return TY_INT_T; }
    if(!strcmp(m,"poke_at")){ ck_positional(c,e,3,3,m); for(int i=0;i<3;i++) expect(c,TY_INT_T,arg(c,e,i),line,"a poke_at() argument"); return TY_VOID_T; }
    if(!strcmp(m,"peek_str_at")||!strcmp(m,"cstr_at")){ ck_positional(c,e,2,2,m); expect(c,TY_INT_T,arg(c,e,0),line,"the address"); expect(c,TY_INT_T,arg(c,e,1),line,"the length"); return TY_STR_T; }
    if(!strcmp(m,"poke_str_at")){ ck_positional(c,e,2,2,m); expect(c,TY_INT_T,arg(c,e,0),line,"the address"); expect(c,TY_STR_T,arg(c,e,1),line,"the string"); return TY_VOID_T; }
    ck_err(c,line,"sys.%s is not available in compiled code",m);
}

/* Calls of async functions are only valid where they are awaited or given to asyncio. */
Ty *ck_asyncio(Ck *c, Expr *e, const char *m){
    XInfo *xi=xinfo(e); xi->kind=X_ASYNC; xi->name=m; int line=e->line;
    for(int i=0;i<e->count;i++) if(e->items[i]->akind) ck_err(c,line,"asyncio.%s() takes positional arguments",m);
    if(!strcmp(m,"run")){
        if(e->count!=1) ck_err(c,line,"asyncio.run() takes one coroutine");
        if(c->fn && c->fn->is_async) ck_err(c,line,"asyncio.run() cannot be called from a coroutine (await instead)");
        return ck_coro(c,e->items[0]);
    }
    if(!strcmp(m,"create_task")||!strcmp(m,"ensure_future")){
        if(e->count!=1) ck_err(c,line,"asyncio.%s() takes one coroutine",m);
        if(!c->fn || !c->fn->is_async) ck_err(c,line,"asyncio.%s() needs a running event loop: call it from a coroutine",m);
        xi->name="create_task";
        return ty_new(TY_TASK,ck_coro(c,e->items[0]),NULL);
    }
    if(!strcmp(m,"sleep")||!strcmp(m,"gather")) ck_err(c,line,"asyncio.%s() must be awaited",m);
    ck_err(c,line,"asyncio.%s is not available in compiled code",m);
}
/* math / time / random */
Ty *ck_bmod(Ck *c, Expr *e, const char *mod, const char *m){
    XInfo *xi=xinfo(e); xi->kind=X_BMOD; xi->name=m; int line=e->line;
    if(!strcmp(mod,"functools") && !strcmp(m,"reduce")) return ck_reduce(c,e);
    char what[64]; snprintf(what,sizeof what,"%s.%s",mod,m);
    #define FARG(i) expect(c,TY_FLOAT_T,arg(c,e,i),line,"an argument of " what_)
    #define what_ ""
    if(!strcmp(mod,"math")){
        static const char *f1[]={"sqrt","sin","cos","tan","asin","acos","atan","exp","log10","log2","fabs","degrees","radians",NULL};
        for(int i=0;f1[i];i++) if(!strcmp(m,f1[i])){ ck_positional(c,e,1,1,what); FARG(0); return TY_FLOAT_T; }
        if(!strcmp(m,"log")){ ck_positional(c,e,1,2,what); FARG(0); if(e->count==2) FARG(1); return TY_FLOAT_T; }
        if(!strcmp(m,"atan2")||!strcmp(m,"pow")||!strcmp(m,"hypot")||!strcmp(m,"fmod")||!strcmp(m,"copysign")){ ck_positional(c,e,2,2,what); FARG(0); FARG(1); return TY_FLOAT_T; }
        if(!strcmp(m,"isnan")||!strcmp(m,"isinf")||!strcmp(m,"isfinite")){ ck_positional(c,e,1,1,what); FARG(0); return TY_BOOL_T; }
        if(!strcmp(m,"floor")||!strcmp(m,"ceil")||!strcmp(m,"trunc")){ ck_positional(c,e,1,1,what);
            { const char *ms[2]={m[0]=='f'?"__floor__":m[0]=='c'?"__ceil__":"__trunc__",NULL}; Ty *r=dunder_of_arg(c,e,ms); if(r) return r; }   /* math.floor(obj): obj.__floor__() */
            FARG(0); return TY_INT_T; }
        if(!strcmp(m,"gcd")){ ck_positional(c,e,2,2,what); expect(c,TY_INT_T,arg(c,e,0),line,"an argument of gcd"); expect(c,TY_INT_T,arg(c,e,1),line,"an argument of gcd"); return TY_INT_T; }
        if(!strcmp(m,"isqrt")){ ck_positional(c,e,1,1,what); expect(c,TY_INT_T,arg(c,e,0),line,"the argument of isqrt"); return TY_INT_T; }
        if(!strcmp(m,"frexp")){ ck_positional(c,e,1,1,what); FARG(0); Ty *two[2]={TY_FLOAT_T,TY_INT_T}; return ty_tuple(two,2); }
        if(!strcmp(m,"ldexp")){ ck_positional(c,e,2,2,what); FARG(0); expect(c,TY_INT_T,arg(c,e,1),line,"the exponent of ldexp"); return TY_FLOAT_T; }
    }
    if(!strcmp(mod,"time")){
        if(!strcmp(m,"time")||!strcmp(m,"monotonic")||!strcmp(m,"perf_counter")){ ck_positional(c,e,0,0,what); return TY_FLOAT_T; }
        if(!strcmp(m,"sleep")){ ck_positional(c,e,1,1,what); FARG(0); return TY_VOID_T; }
    }
    if(!strcmp(mod,"random")){
        if(!strcmp(m,"random")){ ck_positional(c,e,0,0,what); return TY_FLOAT_T; }
        if(!strcmp(m,"seed")){ ck_positional(c,e,1,1,what); expect(c,TY_INT_T,arg(c,e,0),line,"the seed"); return TY_VOID_T; }
        if(!strcmp(m,"randint")){ ck_positional(c,e,2,2,what); expect(c,TY_INT_T,arg(c,e,0),line,"a bound"); expect(c,TY_INT_T,arg(c,e,1),line,"a bound"); return TY_INT_T; }
        if(!strcmp(m,"randrange")){ ck_positional(c,e,1,2,what); for(int i=0;i<e->count;i++) expect(c,TY_INT_T,arg(c,e,i),line,"a bound"); return TY_INT_T; }
        if(!strcmp(m,"uniform")){ ck_positional(c,e,2,2,what); FARG(0); FARG(1); return TY_FLOAT_T; }
        if(!strcmp(m,"choice")){ ck_positional(c,e,1,1,what); Ty *t=ty_find(arg(c,e,0));
            if(t->k==TY_VAR) return pending(c,line,"the argument of choice()");
            if(t->k==TY_STR) return TY_STR_T;
            if(t->k!=TY_LIST) ck_err(c,line,"random.choice() needs a list or a str");
            return t->elem; }
        if(!strcmp(m,"shuffle")){ ck_positional(c,e,1,1,what); Ty *t=ty_find(arg(c,e,0));
            if(t->k!=TY_LIST && t->k!=TY_VAR) ck_err(c,line,"random.shuffle() needs a list"); return TY_VOID_T; }
    }
    #undef FARG
    #undef what_
    if(!strcmp(mod,"math")){                                              /* math.f written in Python: _mathx.py */
        char nm[96]; snprintf(nm,sizeof nm,"__ppy__mathx__%s",m);
        if(lookup(c,nm)){ e->a=name_expr(xstrdup2(nm),line); xi->kind=X_NONE; xi->name=NULL; return ck_call_inner(c,e); } }
    ck_err(c,line,"%s.%s is not available in compiled code",mod,m);
}
/* lib.f(args) / f(args): a cdecl call. Without argtypes each argument goes by
   its type: int/bool c_int, float c_double, str and buffers char *, None NULL. */
Ty *ck_ccall(Ck *c, Expr *e, ACFunc *cf){
    XInfo *xi=xinfo(e); xi->kind=X_CCALL; xi->cfn=cf; int line=e->line;
    if(aot_target(c->p->target)->cfn==AOT_CFN_NONE) ck_err(c,line,"C functions (ctypes) need a target with C functions (linux, macos, c, esp32)");
    if(e->count>16) ck_err(c,line,"%s(): at most 16 arguments",cf->sym);
    if(cf->nargtypes>=0 && e->count<cf->nargtypes) ck_err(c,line,"%s() takes %d arguments (its argtypes), not %d",cf->sym,cf->nargtypes,e->count);
    for(int i=0;i<e->count;i++){ Expr *a=e->items[i];
        if(a->akind) ck_err(c,line,"%s() takes positional arguments",cf->sym);
        CType k=i<cf->nargtypes?cf->argtypes[i]:CT_DEFAULT;
        Ty *t=ty_find(arg(c,e,i));
        if(a->kind==EXPR_NONE){
            if(k!=CT_DEFAULT && k!=CT_CHARP && k!=CT_VOIDP) ck_err(c,line,"argument %d of %s(): None is a NULL pointer, the argtype is not a pointer",i+1,cf->sym);
            unify(c,t,TY_BUF_T); xi->argmap[i]=CT_VOIDP; continue;
        }
        if(t->k==TY_VAR){ pending(c,line,"an argument of a C function"); continue; }
        int isint=t->k==TY_INT||t->k==TY_BOOL, isptr=t->k==TY_STR||t->k==TY_BYTES||t->k==TY_BUF;
        if(k==CT_DEFAULT){
            if(isint) k=CT_INT; else if(t->k==TY_FLOAT) k=CT_DOUBLE; else if(t->k==TY_STR||t->k==TY_BYTES) k=CT_CHARP; else if(t->k==TY_BUF) k=CT_VOIDP;
            else ck_err(c,line,"argument %d of %s() is %s: C functions take int, float, str, buffers or None",i+1,cf->sym,ty_name(t));
        } else if(k==CT_DOUBLE || k==CT_FLOAT){
            if(!numeric(t)) ck_err(c,line,"argument %d of %s() must be a number (its argtype is a float), not %s",i+1,cf->sym,ty_name(t));
        } else if(k==CT_CHARP){
            if(!isptr && !isint) ck_err(c,line,"argument %d of %s() must be a str or a buffer (c_char_p), not %s",i+1,cf->sym,ty_name(t));
        } else if(k==CT_VOIDP){
            if(!isptr && !isint) ck_err(c,line,"argument %d of %s() must be an address, a str or a buffer (c_void_p), not %s",i+1,cf->sym,ty_name(t));
        } else if(!isint) ck_err(c,line,"argument %d of %s() must be an int, not %s",i+1,cf->sym,ty_name(t));
        xi->argmap[i]=k;
    }
    switch(cf->restype){                                 /* (the function and its library: used once code calls it - codegen/ctypes.c) */
        case CT_DOUBLE: case CT_FLOAT: return TY_FLOAT_T;
        case CT_CHARP: return optional(TY_STR_T);          /* NULL is None */
        case CT_VOID: return TY_VOID_T;
        case CT_BOOL: return TY_BOOL_T;
        default: return TY_INT_T;
    }
}
/* ctypes.create_string_buffer / string_at / addressof / get_errno */
Ty *ck_ctypes_fn(Ck *c, Expr *e, const char *m){
    static const char *names[]={"ctypes.create_string_buffer","ctypes.string_at","ctypes.addressof","ctypes.get_errno",NULL};
    XInfo *xi=xinfo(e); xi->kind=X_BMOD; xi->name=m; int line=e->line;
    for(int i=0;names[i];i++) if(!strcmp(names[i]+7,m)) xi->name=names[i];
    if(aot_target(c->p->target)->cfn==AOT_CFN_NONE) ck_err(c,line,"ctypes needs a target with C functions (linux, macos, c, esp32)");
    if(!strcmp(m,"create_string_buffer")){ ck_positional(c,e,1,1,"create_string_buffer"); Ty *t=ty_find(arg(c,e,0));
        if(t->k!=TY_INT&&t->k!=TY_STR&&t->k!=TY_VAR) ck_err(c,line,"create_string_buffer() takes a size or a str");
        return TY_BUF_T; }
    if(!strcmp(m,"string_at")){ ck_positional(c,e,1,2,"string_at"); expect(c,TY_INT_T,arg(c,e,0),line,"the address");
        if(e->count==2) expect(c,TY_INT_T,arg(c,e,1),line,"the size"); return TY_STR_T; }
    if(!strcmp(m,"addressof")){ ck_positional(c,e,1,1,"addressof"); expect(c,TY_BUF_T,arg(c,e,0),line,"the buffer"); return TY_INT_T; }
    if(!strcmp(m,"get_errno")){ ck_positional(c,e,0,0,"get_errno");
        ACFunc *f=cfunc_get(c->p,clib_get(c->p,"libc.so.6"),"__errno_location"); f->restype=CT_VOIDP; xi->cfn=f;
        return TY_INT_T; }
    ck_err(c,line,"ctypes.%s is not available in compiled code",m);
}

