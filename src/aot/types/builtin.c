/* ========================= Typed compiler, types: built-in functions =========================
   Calls of the built-in functions (len, print, int, sorted, ...). */

#include "check.h"

Ty *dunder_of_arg(Ck *c, Expr *e, const char *const *ms){
    if(e->count<1 || e->items[0]->akind) return NULL;
    Ty *t=ty_find(ck_expr(c,e->items[0]));
    if(t->k!=TY_OBJ) return NULL;
    for(int i=0;ms[i];i++) if(aot_find_method(t->cls,ms[i])){
        Expr *at=xnew(EXPR_ATTRIBUTE,e->line); at->a=e->items[0]; at->name=(char*)ms[i];
        for(int k=1;k<e->count;k++) e->items[k-1]=e->items[k];
        e->count--; e->a=at; xinfo(e)->kind=X_NONE;
        return ck_call_inner(c,e); }
    return NULL;
}
Ty *ck_builtin(Ck *c, Expr *e, const char *name){
    XInfo *xi=xinfo(e); xi->kind=X_BUILTIN; xi->name=name;
    int line=e->line;
    if(!strcmp(name,"anext")||!strcmp(name,"aiter")){            /* anext(g[, default]) (awaited), aiter(g) */
        ck_positional(c,e,1,name[0]=='a'&&name[1]=='n'?2:1,name); Ty *t=ty_find(arg(c,e,0));
        if(t->k==TY_VAR) return pending(c,line,"the async iterator");
        if(t->k!=TY_GEN || !t->tup) ck_err(c,line,"'%s' object is not an async %s",ty_name(t),name[1]=='n'?"iterator":"iterable");
        if(name[1]=='i') return t;
        if(e->count==2){ Ty *d=ck_expr_want(c,e->items[1],t->elem); no_void(c,d,line); expect(c,t->elem,d,line,"the default of anext()"); }
        return t->elem; }
    if(!strcmp(name,"downcast")){                              /* (match: an object known to be a C) */
        Ty *k=ty_find(ck_expr(c,e->items[0])); XInfo *kx=xinfo(e->items[0]); (void)k;
        Ty *v=ty_find(ck_expr(c,e->items[1]));
        if(kx->kind!=X_TYPEVAL || !kx->cls || v->k!=TY_OBJ) ck_err(c,line,"internal error: downcast");
        return ty_new(TY_OBJ,NULL,kx->cls); }
    if(!strcmp(name,"map")||!strcmp(name,"filter")) return ck_mapfilter(c,e,name);
    if(!strcmp(name,"iter")) return ck_iter(c,e);
    if(!strcmp(name,"sum") && e->count==3 && e->items[0]->kind==EXPR_LAMBDA && e->items[0]->neparam==2
       && !strcmp(e->items[0]->eparams[0],"__mpy_s1")) return ck_reduce(c,e);   /* (sum of objects, made a reduce() in an earlier pass) */
    if(!strcmp(name,"sum")||!strcmp(name,"sorted")||!strcmp(name,"min")||!strcmp(name,"max")||!strcmp(name,"set")||!strcmp(name,"list")||!strcmp(name,"any")||!strcmp(name,"all"))
        genexp_as_list(e,0);
    if((!strcmp(name,"sum")||!strcmp(name,"sorted")||!strcmp(name,"min")||!strcmp(name,"max")||!strcmp(name,"set")||!strcmp(name,"any")||!strcmp(name,"all")
        ||!strcmp(name,"enumerate")||!strcmp(name,"frozenset")) && e->count && !e->items[0]->akind && e->items[0]->kind!=EXPR_TUPLE){   /* sorted(t), t a tuple: */
        Ty *t0=ty_find(ck_expr(c,e->items[0]));                                                                            /* sorted(list(t)) */
        if(t0->k==TY_TUPLE && !t0->names){
            Expr *inner=MPY_NEW0(Expr); *inner=*e->items[0]; inner->akind=0; inner->kw=NULL;
            Expr *call=xnew(EXPR_CALL,e->line); call->a=name_expr("\001list",e->line); xpush(call,inner);
            e->items[0]=call; } }
    if(!strcmp(name,"sum")||!strcmp(name,"sorted")||!strcmp(name,"min")||!strcmp(name,"max")||!strcmp(name,"set")||!strcmp(name,"list")||!strcmp(name,"any")
       ||!strcmp(name,"all")||!strcmp(name,"tuple")||!strcmp(name,"enumerate")||!strcmp(name,"dict")||!strcmp(name,"frozenset"))
        { if(e->count && !e->items[0]->akind) iterator_protocol(c,e->items[0]); }
    if(!strcmp(name,"zip")) for(int i=0;i<e->count;i++) iterator_protocol(c,e->items[i]);
    if(!strcmp(name,"next")){ ck_positional(c,e,1,2,name); Ty *t=ty_find(arg(c,e,0));
        if(t->k==TY_VAR) return pending(c,line,"the argument of next()");
        if(t->k==TY_OBJ && aot_find_method(t->cls,"__next__")){      /* an iterator object: it.__next__() */
            Expr *it=e->items[0];
            if(e->count==2){ Expr *d=e->items[1]; e->count=0; e->a=name_expr("__mpy_next",line); xpush(e,it); xpush(e,d); return ck_call_inner(c,e); }
            Expr *m=xnew(EXPR_ATTRIBUTE,line); m->a=it; m->name="__next__"; e->a=m; e->count=0; return ck_call_inner(c,e); }
        if(t->k!=TY_GEN) ck_err(c,line,"next() needs a generator (or iter(...)), not %s",ty_name(t));
        if(e->count==2){ Ty *d=ck_expr_want(c,e->items[1],t->elem); no_void(c,d,line); expect(c,t->elem,d,line,"the default of next()"); }
        stop_value(c,t,line);
        return t->elem; }
    if(!strcmp(name,"sorted")||!strcmp(name,"min")||!strcmp(name,"max")) take_sort_kwargs(c,e,name[0]=='s');
    if(!strcmp(name,"sum")||!strcmp(name,"sorted")||(e->count==1&&(!strcmp(name,"min")||!strcmp(name,"max")))||!strcmp(name,"set")||!strcmp(name,"reversed")||!strcmp(name,"any")||!strcmp(name,"all"))
        gen_as_list(c,e,0);
    if(xi->rev) expect(c,TY_BOOL_T,ck_expr(c,xi->rev),line,"reverse=");
    if(!strcmp(name,"len")){ ck_positional(c,e,1,1,name); enum_members_expr(c,e->items[0]); Ty *t=ty_find(arg(c,e,0));
        if(t->k==TY_VAR) return pending(c,line,"the argument of len()");
        if(t->k==TY_STR||t->k==TY_BYTES||t->k==TY_LIST||t->k==TY_DICT||t->k==TY_SET||t->k==TY_BUF||t->k==TY_TUPLE) return TY_INT_T;
        if(t->k==TY_OBJ){ AFunc *m=aot_find_method(t->cls,"__len__"); if(m){ xi->fn=m; return TY_INT_T; } }
        ck_err(c,line,"len() of %s",ty_name(t)); }
    if(!strcmp(name,"str") && e->count>=2){                    /* str(b, encoding[, errors]): b.decode(...) */
        static const char *const n_dec[]={"object","encoding","errors"};
        kw_to_pos(c,e,name,n_dec,3); ck_positional(c,e,2,3,name);
        if(codecs_module(c)){ expect(c,TY_BYTES_T,arg(c,e,0),line,"the argument of str()"); return ck_codec_call(c,e,e->items[0],1,1); }
        expect(c,TY_BYTES_T,arg(c,e,0),line,"the argument of str()");
        for(int i=1;i<e->count;i++) if(!is_none(e->items[i])) expect(c,TY_STR_T,arg(c,e,i),line,i==1?"the encoding":"errors");
        xi->name="str_decode"; return TY_STR_T; }
    if(!strcmp(name,"str")||!strcmp(name,"repr")){ ck_positional(c,e,0,1,name); if(e->count) arg(c,e,0); return TY_STR_T; }
    if(!strcmp(name,"ascii")){                                  /* ascii(x): repr(x).encode("ascii", "backslashreplace").decode() */
        ck_positional(c,e,1,1,name);
        Expr *t=py_front_expr(c->mod->unit->path,"repr(0).encode(\"ascii\", \"backslashreplace\").decode()",line);
        Expr *rc=t->a->a->a->a; rc->a->name="\001repr"; rc->items[0]=e->items[0];
        void *keep=e->ty; *e=*t; e->ty=keep;
        return ck_expr(c,e); }
    if(!strcmp(name,"bytes")){
        static const char *const n_bytes[]={"source","encoding","errors"};
        kw_to_pos(c,e,name,n_bytes,3); ck_positional(c,e,0,3,name); genexp_as_list(e,0);
        if(!e->count){ xi->name="bytes_empty"; return TY_BYTES_T; }
        Ty *t=ty_find(gen_as_list(c,e,0)); if(t->k==TY_VAR) return pending(c,line,"the argument of bytes()");
        if(t->k==TY_STR){
            if(e->count<2) ck_err(c,line,"TypeError: string argument without an encoding");
            if(codecs_module(c)) return ck_codec_call(c,e,e->items[0],0,1);
            for(int i=1;i<e->count;i++) if(!is_none(e->items[i])) expect(c,TY_STR_T,arg(c,e,i),line,i==1?"the encoding":"errors");
            xi->name="bytes_encode"; return TY_BYTES_T; }
        if(e->count>1) ck_err(c,line,"TypeError: encoding without a string argument");
        if(t->k==TY_INT||t->k==TY_BOOL){ xi->name="bytes_zeros"; return TY_BYTES_T; }
        if(t->k==TY_BYTES){ xi->name="bytes_same"; return TY_BYTES_T; }
        if(t->k==TY_LIST||t->k==TY_SET){ expect(c,TY_INT_T,t->elem,line,"the values of bytes()"); xi->name="bytes_list"; return TY_BYTES_T; }
        if(t->k==TY_OBJ){ static const char *const ms[]={"__bytes__",NULL}; Ty *r=dunder_of_arg(c,e,ms); if(r) return r; }
        ck_err(c,line,"TypeError: cannot convert '%s' object to bytes",ty_name(t)); }
    if(!strcmp(name,"bytes.fromhex")){ ck_positional(c,e,1,1,"fromhex"); expect(c,TY_STR_T,arg(c,e,0),line,"the argument of fromhex()"); return TY_BYTES_T; }
    if(!strcmp(name,"int.from_bytes")){
        static const char *const n_fb[]={"bytes","byteorder","signed"};
        kw_to_pos(c,e,"from_bytes",n_fb,3); ck_positional(c,e,1,3,"from_bytes");
        expect(c,TY_BYTES_T,arg(c,e,0),line,"the bytes");
        if(e->count>1 && !is_none(e->items[1])) expect(c,TY_STR_T,arg(c,e,1),line,"byteorder");
        if(e->count>2) expect(c,TY_BOOL_T,arg(c,e,2),line,"signed");
        return TY_INT_T; }
    if(!strcmp(name,"int") && e->count==1){ static const char *const ms[]={"__int__","__index__","__trunc__",NULL}; Ty *r=dunder_of_arg(c,e,ms); if(r) return r; }
    if(!strcmp(name,"float") && e->count==1){ static const char *const ms[]={"__float__","__index__",NULL}; Ty *r=dunder_of_arg(c,e,ms); if(r) return r; }
    if(!strcmp(name,"round")){ static const char *const m1[]={"__round__",NULL}, *const m2[]={"__mpy_round_n","__round__",NULL};   /* round(obj, n): one result type per method */
        Ty *r=dunder_of_arg(c,e,e->count==2?m2:m1); if(r) return r; }
    if(!strcmp(name,"int")){ static const char *const n_int[]={"x","base"}; kw_to_pos(c,e,name,n_int,2); ck_positional(c,e,0,2,name);
        if(e->count){ Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) pending(c,line,"the argument of int()");
            else if(e->count==2){ if(t->k!=TY_STR && t->k!=TY_BYTES) ck_err(c,line,"int() can't convert non-string with explicit base");
                expect(c,TY_INT_T,arg(c,e,1),line,"the base"); }
            else if(!numeric(t)&&t->k!=TY_STR&&t->k!=TY_BYTES) ck_err(c,line,"int() of %s",ty_name(t)); }
        return TY_INT_T; }
    if(!strcmp(name,"float")){ ck_positional(c,e,0,1,name); if(e->count){ Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) pending(c,line,"the argument of float()"); else if(!numeric(t)&&t->k!=TY_STR) ck_err(c,line,"float() of %s",ty_name(t)); } return TY_FLOAT_T; }
    if(!strcmp(name,"bool")){ ck_positional(c,e,0,1,name); if(e->count) arg(c,e,0); return TY_BOOL_T; }
    if(!strcmp(name,"abs")){ ck_positional(c,e,1,1,name); Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) return pending(c,line,"the argument of abs()");
        if(t->k==TY_OBJ && aot_find_method(t->cls,"__abs__")){ Expr *at=xnew(EXPR_ATTRIBUTE,line); at->a=e->items[0]; at->name="__abs__"; e->a=at; e->count=0; return ck_call_inner(c,e); }   /* abs(obj): obj.__abs__() */
        if(!numeric(t)) ck_err(c,line,"bad operand type for abs(): '%s'",ty_name(t)); return t->k==TY_FLOAT?TY_FLOAT_T:TY_INT_T; }
    if(!strcmp(name,"min")||!strcmp(name,"max")){
        ck_positional(c,e,1,16,name);
        if(xi->key){ if(e->count!=1) ck_err(c,line,"%s(key=...) takes one list",name);
            Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) return pending(c,line,"the argument");
            if(t->k!=TY_LIST&&t->k!=TY_SET&&t->k!=TY_DICT) ck_err(c,line,"%s() of %s",name,ty_name(t));
            Ty *el=t->k==TY_DICT?ty_dkey(t):t->elem;
            ck_sort_key(c,xi->key,el,line); xi->ty=t; return el; }
        if(e->count==1){ Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) return pending(c,line,"the argument");
            if(t->k==TY_STR){ xi->ty=t; return TY_STR_T; }                     /* its characters */
            if(t->k!=TY_LIST&&t->k!=TY_SET&&t->k!=TY_DICT) ck_err(c,line,"%s() of %s",name,ty_name(t));
            if(t->k==TY_DICT){ Ty *k=ty_find(ty_dkey(t)); if(k->k==TY_VAR) return pending(c,line,"the keys"); if(!numeric(k)&&k->k!=TY_STR) ck_err(c,line,"%s() needs numbers or strings",name); xi->ty=t; return k; }
            Ty *el=ty_find(t->elem); if(el->k==TY_VAR) return pending(c,line,"the elements");
            if(!orderable(el)) ck_err(c,line,"%s() needs values that can be ordered, not %s",name,ty_name(el));
            xi->ty=t; return t->elem; }
        { Ty *t0=ty_find(arg(c,e,0));                                       /* max(a, b) of objects (__lt__): max([a, b]) */
          if(t0->k==TY_OBJ){ Expr *l=xnew(EXPR_LIST,line); for(int i=0;i<e->count;i++){ if(e->items[i]->akind) ck_err(c,line,"%s() takes positional arguments",name); xpush(l,e->items[i]); }
              e->count=0; xpush(e,l); return ck_builtin(c,e,name); } }
        Ty *r=NULL; int fl=0, allnum=1;
        for(int i=0;i<e->count;i++){ Ty *t=ty_find(arg(c,e,i)); if(t->k==TY_VAR) return pending(c,line,"an argument"); if(t->k==TY_FLOAT) fl=1; if(!numeric(t)) allnum=0; if(!r) r=t; else if(!allnum && !unify(c,r,t)) ck_err(c,line,"%s() arguments must have the same type",name); }
        if(allnum) return fl?TY_FLOAT_T:TY_INT_T;
        if(ty_find(r)->k!=TY_STR) ck_err(c,line,"%s() of %s",name,ty_name(r));
        return r; }
    if(!strcmp(name,"divmod")){ ck_positional(c,e,2,2,name); Ty *a=ty_find(arg(c,e,0)), *b=ty_find(arg(c,e,1));
        if(a->k==TY_VAR||b->k==TY_VAR) return pending(c,line,"an argument of divmod()");
        if(a->k==TY_OBJ && aot_find_method(a->cls,"__divmod__")){              /* divmod(obj, x): obj.__divmod__(x) */
            Expr *at=xnew(EXPR_ATTRIBUTE,line); at->a=e->items[0]; at->name="__divmod__"; e->a=at; e->items[0]=e->items[1]; e->count=1; return ck_call_inner(c,e); }
        if(b->k==TY_OBJ && a->k!=TY_OBJ && aot_find_method(b->cls,"__rdivmod__")){   /* divmod(x, obj): obj.__rdivmod__(x) */
            Expr *at=xnew(EXPR_ATTRIBUTE,line); at->a=e->items[1]; at->name="__rdivmod__"; e->a=at; e->count=1; return ck_call_inner(c,e); }
        if(!numeric(a)||!numeric(b)) ck_err(c,line,"divmod() needs numbers");
        Ty *r=(a->k==TY_FLOAT||b->k==TY_FLOAT)?TY_FLOAT_T:TY_INT_T; Ty *qr[2]={r,r}; return ty_tuple(qr,2); }
    if(!strcmp(name,"open") && lookup(c,"__mpy_open_text")) return ck_open(c,e);
    if(!strcmp(name,"open")){
        int n=0; for(int i=0;i<e->count;i++){ Expr *a=e->items[i]; if(a->akind==3 && (!strcmp(a->kw,"encoding")||!strcmp(a->kw,"newline")||!strcmp(a->kw,"errors"))) continue; e->items[n++]=a; }
        e->count=n;                                      /* encoding= and friends: the bytes are the text */
        ck_positional(c,e,1,2,name); expect(c,TY_STR_T,arg(c,e,0),line,"the path"); if(e->count==2) expect(c,TY_STR_T,arg(c,e,1),line,"the mode");
        Expr *md= e->count==2 ? e->items[1] : NULL;            /* a literal mode with "b": a file of bytes */
        int binary= md && md->kind==EXPR_LITERAL && md->tok->kind==T_STRING && strchr(md->tok->text,'b');
        return ty_new(TY_FILE,binary?TY_BYTES_T:TY_STR_T,NULL); }
    if(!strcmp(name,"any")||!strcmp(name,"all")){ ck_positional(c,e,1,1,name); elem_of(c,arg(c,e,0),line,"the argument"); return TY_BOOL_T; }
    if(!strcmp(name,"reversed")){ ck_positional(c,e,1,1,name); enum_members_expr(c,e->items[0]); Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) return pending(c,line,"the argument of reversed()");
        if(t->k==TY_OBJ && aot_find_method(t->cls,"__reversed__")){      /* reversed(obj): obj.__reversed__() */
            Expr *m=xnew(EXPR_ATTRIBUTE,line); m->a=e->items[0]; m->name="__reversed__"; e->a=m; e->count=0; xi->kind=X_NONE; return ck_call_inner(c,e); }
        if(t->k!=TY_LIST&&t->k!=TY_STR&&t->k!=TY_TUPLE) ck_err(c,line,"reversed() of %s",ty_name(t));     /* (bytes: a list by now) */
        return ty_new(TY_LIST,elem_of(c,t,line,"the argument"),NULL); }
    if(!strcmp(name,"hex")||!strcmp(name,"bin")||!strcmp(name,"oct")){ ck_positional(c,e,1,1,name); expect(c,TY_INT_T,arg(c,e,0),line,"the argument"); return TY_STR_T; }
    if(!strcmp(name,"pow")){ ck_positional(c,e,2,3,name); Ty *a=ty_find(arg(c,e,0)), *b=ty_find(arg(c,e,1));
        if(a->k==TY_VAR||b->k==TY_VAR) return pending(c,line,"an argument of pow()");
        if(a->k==TY_OBJ && aot_find_method(a->cls,"__pow__")){                  /* pow(obj, y[, z]): obj.__pow__(y[, z]) */
            Expr *at=xnew(EXPR_ATTRIBUTE,line); at->a=e->items[0]; at->name="__pow__"; e->a=at;
            for(int i=1;i<e->count;i++) e->items[i-1]=e->items[i]; e->count--; xi->kind=X_NONE; return ck_call_inner(c,e); }
        if(e->count==2 && b->k==TY_OBJ && aot_find_method(b->cls,"__rpow__")){   /* pow(x, obj): obj.__rpow__(x) */
            Expr *at=xnew(EXPR_ATTRIBUTE,line); at->a=e->items[1]; at->name="__rpow__"; e->a=at; e->count=1; xi->kind=X_NONE; return ck_call_inner(c,e); }
        if(e->count==3){ expect(c,TY_INT_T,a,line,"pow() base"); expect(c,TY_INT_T,b,line,"pow() exponent"); expect(c,TY_INT_T,arg(c,e,2),line,"pow() modulus"); return TY_INT_T; }
        return binop_type(c,T_POWER,a,b,line); }
    if(!strcmp(name,"format")){ ck_positional(c,e,1,2,name);
        { Ty *ot=ty_find(arg(c,e,0));                                     /* format(obj, spec): obj.__format__(spec) */
          if(ot->k==TY_OBJ && aot_find_method(ot->cls,"__format__")){
              Expr *at=xnew(EXPR_ATTRIBUTE,line); at->a=e->items[0]; at->name="__format__"; e->a=at;
              if(e->count==2){ e->items[0]=e->items[1]; e->count=1; }
              else { Expr *lit=xnew(EXPR_LITERAL,line); lit->tok=MPY_NEW0(Tok); lit->tok->kind=T_STRING; lit->tok->text=xstrdup2(""); lit->tok->len=0; lit->tok->line=line; e->items[0]=lit; }
              return ck_call_inner(c,e); } }
        if(e->count==2){ Expr *sp=e->items[1]; if(sp->kind!=EXPR_LITERAL||sp->tok->kind!=T_STRING) ck_err(c,line,"format() needs a literal spec in compiled code"); }
        return TY_STR_T; }
    if(!strcmp(name,"ord")){ ck_positional(c,e,1,1,name); expect(c,TY_STR_T,arg(c,e,0),line,"the argument of ord()"); return TY_INT_T; }
    if(!strcmp(name,"chr")){ ck_positional(c,e,1,1,name); expect(c,TY_INT_T,arg(c,e,0),line,"the argument of chr()"); return TY_STR_T; }
    if(!strcmp(name,"sum") && e->count>=1 && e->count<=2){ Ty *t=ty_find(arg(c,e,0));      /* sum(objects[, start]): start + x0 + x1 ... (their __add__) */
        if(t->k==TY_VAR) return pending(c,line,"the argument of sum()");
        if((t->k==TY_LIST||t->k==TY_SET) && ty_find(t->elem)->k==TY_OBJ){
            Expr *lam=py_front_expr(c->mod->unit->path,"lambda __mpy_s1, __mpy_s2: __mpy_s1 + __mpy_s2",line);
            Expr *start= e->count==2 ? e->items[1] : py_front_expr(c->mod->unit->path,"0",line);
            Expr *xs=e->items[0]; start->kw=NULL; start->akind=0;
            e->count=0; xpush(e,lam); xpush(e,xs); xpush(e,start);
            return ck_reduce(c,e); } }
    if(!strcmp(name,"sum")){ ck_positional(c,e,1,1,name); Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) return pending(c,line,"the argument of sum()");
        if((t->k!=TY_LIST&&t->k!=TY_SET)) ck_err(c,line,"sum() of %s",ty_name(t));
        Ty *el=ty_find(t->elem); if(el->k==TY_VAR) return pending(c,line,"the elements"); if(!numeric(el)) ck_err(c,line,"sum() of %s",ty_name(t));
        return el->k==TY_FLOAT?TY_FLOAT_T:TY_INT_T; }
    if(!strcmp(name,"sorted")){ ck_positional(c,e,1,1,name); Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) return pending(c,line,"the argument of sorted()");
        if(t->k!=TY_LIST&&t->k!=TY_SET&&t->k!=TY_DICT&&t->k!=TY_STR) ck_err(c,line,"sorted() of %s",ty_name(t));
        Ty *el=t->k==TY_DICT?ty_dkey(t):t->k==TY_STR?TY_STR_T:t->elem;
        if(xi->key){ ck_sort_key(c,xi->key,el,line); return ty_new(TY_LIST,el,NULL); }
        Ty *f=ty_find(el); if(f->k==TY_VAR) return pending(c,line,"the elements");
        if(!orderable(f)) ck_err(c,line,"sorted() needs values that can be ordered, not %s",ty_name(f));
        return ty_new(TY_LIST,el,NULL); }
    if(!strcmp(name,"isinstance")||!strcmp(name,"issubclass")){
        ck_positional(c,e,2,2,name);
        Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) pending(c,line,"the object");
        if(name[2]=='s' && t->k!=TY_TYPE) ck_err(c,line,"TypeError: issubclass() arg 1 must be a class");
        Expr *k=e->items[1]; int n=k->kind==EXPR_TUPLE?k->count:1;
        for(int i=0;i<n;i++){ Expr *x=k->kind==EXPR_TUPLE?k->items[i]:k;       /* classes and built-in types, by name */
            if(x->kind==EXPR_ATTRIBUTE){ AClass *ce=class_expr(c,x);                  /* module.Class */
                if(ce){ xinfo(x)->kind=X_TYPEVAL; xinfo(x)->cls=ce; xinfo(x)->ty=TY_TYPE_T; continue; } }
            ASym *s=x->kind==EXPR_NAME?lookup(c,x->name):NULL;
            if(s && s->kind==AS_CLASS){ xinfo(x)->kind=X_TYPEVAL; xinfo(x)->cls=(AClass*)s->p; xinfo(x)->ty=TY_TYPE_T; continue; }
            if(!s && x->kind==EXPR_NAME && is_builtin_type_name(x->name)){ xinfo(x)->kind=X_TYPEVAL; xinfo(x)->name=x->name; xinfo(x)->ty=TY_TYPE_T; continue; }
            if(!s && x->kind==EXPR_CALL && x->a->kind==EXPR_NAME && !strcmp(x->a->name,"type") && x->count==1 && x->items[0]->kind==EXPR_NONE){   /* type(None) */
                xinfo(x)->kind=X_TYPEVAL; xinfo(x)->name="NoneType"; xinfo(x)->ty=TY_TYPE_T; continue; }
            if(k->kind!=EXPR_TUPLE && (!s || s->kind==AS_VAR)){           /* isinstance(obj, cls) / issubclass(c, cls) with a class value: checked when running */
                Ty *xt=ty_find(ck_expr(c,x));
                if(xt->k==TY_VAR){ pending(c,line,"the class"); continue; }
                if(xt->k==TY_TYPE){ if(name[2]=='i' && t->k!=TY_OBJ && t->k!=TY_VAR) ck_err(c,line,"isinstance() of a %s with a class value is not supported in compiled code",ty_name(t)); continue; } }
            ck_err(c,line,"%s() needs a class (or a tuple of classes) in compiled code",name); }
        if(name[2]=='i' && t->k!=TY_VAR && pure_test(e->items[0])){         /* isinstance(5, numbers.Number): decided here */
            int any=0; for(int i=0;i<n;i++){ AClass *kk=class_expr(c,k->kind==EXPR_TUPLE?k->items[i]:k); if(kk && kk->mod && kk->mod->name && !strcmp(kk->mod->name,"numbers")) any=1; }
            if(any){ int st=static_isinstance(c,e); if(st>=0){ e->kind=st?EXPR_TRUE:EXPR_FALSE; e->count=0; xi->kind=X_NONE; } } }
        return TY_BOOL_T; }
    if(!strcmp(name,"getattr")||!strcmp(name,"hasattr")){          /* getattr(o, "name"[, default]) / hasattr(o, "name"): by a literal name */
        ck_positional(c,e,2,name[0]=='g'?3:2,name);
        Expr *k=e->items[1];
        if(k->kind!=EXPR_LITERAL || k->tok->kind!=T_STRING || k->tok->i==2) ck_err(c,line,"%s() with a computed name is not supported in compiled code (the attributes are fixed)",name);
        Expr *o=e->items[0]; Ty *t=ty_find(ck_expr(c,o));
        if(t->k==TY_VAR) return pending(c,line,"the object");
        const char *an=k->tok->text;
        int has= t->k==TY_OBJ ? (aot_find_field(t->cls,an) || aot_find_method(t->cls,an))
               : (t->k==TY_TYPE||t->k==TY_FUNC) && (!strcmp(an,"__name__")||!strcmp(an,"__qualname__")||(t->k==TY_TYPE && !strcmp(an,"__module__")));
        if(!has && t->k==TY_OBJ && aot_is_exception(t->cls) && (!strcmp(an,"args"))) has=1;
        if(name[0]=='h'){ e->kind=has?EXPR_TRUE:EXPR_FALSE; e->count=0; xi->kind=X_NONE; return TY_BOOL_T; }
        if(!has && e->count==3){ Expr *d=e->items[2]; *e=*d; return ck_expr(c,e); }          /* not there: the default */
        if(!has) ck_err(c,line,"AttributeError: '%s' object has no attribute '%s'",ty_name(t),an);
        Expr *a=xnew(EXPR_ATTRIBUTE,line); a->a=o; a->name=(char*)an;
        *e=*a; xi->kind=X_NONE; return ck_expr(c,e); }
    if(!strcmp(name,"type")){
        if(e->count==3) ck_err(c,line,"type(name, bases, dict) creates classes while the program runs: not in compiled code");
        ck_positional(c,e,1,1,name); if(is_none(e->items[0])) return TY_TYPE_T;      /* type(None) */
        Ty *t=ty_find(arg(c,e,0));
        if(t->k==TY_VAR && t->opt && (c->strict || c->defaulting)){ unify(c,t,optional(TY_INT_T)); t=ty_find(t); }   /* (only ever None: NoneType) */
        if(t->k==TY_VAR) return pending(c,line,"the argument of type()");
        return TY_TYPE_T; }
    if(!strcmp(name,"input")){ ck_positional(c,e,0,1,name); if(e->count) arg(c,e,0); return TY_STR_T; }
    if(!strcmp(name,"round")){ ck_positional(c,e,1,2,name); Ty *t=arg(c,e,0); if(!numeric(t)&&!is_var(t)) ck_err(c,line,"round() of %s",ty_name(t)); if(e->count==2){ expect(c,TY_INT_T,arg(c,e,1),line,"round() digits"); return TY_FLOAT_T; } return TY_INT_T; }
    if(!strcmp(name,"list")){
        ck_positional(c,e,0,1,name);
        if(!e->count) return ty_new(TY_LIST,ty_var(),NULL);
        Ty *el; Ty *ts[1];
        Expr *it=e->items[0];
        if(is_builtin_call(c,it,"range")){ iter_types(c,it,1,ts,line); el=ts[0]; }
        else el=elem_of(c,arg(c,e,0),line,"the argument of list()");
        return ty_new(TY_LIST,el,NULL); }
    if(!strcmp(name,"set")){ ck_positional(c,e,0,1,name); if(!e->count) return ty_new(TY_SET,ty_var(),NULL); return ty_new(TY_SET,elem_of(c,arg(c,e,0),line,"the argument of set()"),NULL); }
    if(!strcmp(name,"frozenset")){ ck_positional(c,e,0,1,name); xi->name="set";       /* a set that prints as frozenset({...}) */
        Ty *t=ty_new(TY_SET,e->count?elem_of(c,arg(c,e,0),line,"the argument of frozenset()"):ty_var(),NULL); t->tup=1; return t; }
    if(!strcmp(name,"dict")){ ck_positional(c,e,0,1,name);
        if(!e->count) return ty_dict(ty_var(),ty_var());
        genexp_as_list(e,0);
        Ty *t=ty_find(arg(c,e,0));
        if(t->k==TY_VAR) return pending(c,line,"the argument of dict()");
        if(t->k==TY_DICT){ xi->name="dict_copy"; return t; }
        Expr *src=e->items[0];                          /* dict(pairs) -> {k: v for k, v in pairs} */
        char **vars=MPY_NEW_ARR(char*,2); vars[0]="__d0"; vars[1]="__d1";
        make_comprehension(e,'D',name_expr("__d0",line),name_expr("__d1",line),vars,2,src);
        return ck_comprehension(c,e); }
    if(!strcmp(name,"tuple")){                          /* tuple(xs): a tuple[T, ...] */
        ck_positional(c,e,0,1,name); genexp_as_list(e,0);
        Ty *el=ty_var();
        if(e->count){ Ty *ts[1]; Expr *it=e->items[0];
            if(is_builtin_call(c,it,"range")){ iter_types(c,it,1,ts,line); el=ts[0]; } else el=elem_of(c,arg(c,e,0),line,"the argument of tuple()"); }
        xi->name="list"; Ty *r=ty_new(TY_LIST,el,NULL); r->tup=1; return r; }
    if(!strcmp(name,"print")) ck_err(c,line,"print() is a statement here");
    if(!strcmp(name,"enumerate")||!strcmp(name,"zip")) return ck_pairs(c,e,name);
    if(!strcmp(name,"range")){                          /* range(...) as a value (zip(xs, range(n)), len(range(...))): a list */
        ck_positional(c,e,1,3,name); for(int i=0;i<e->count;i++) expect(c,TY_INT_T,arg(c,e,i),line,"a range() argument");
        xi->name="range_list"; return ty_new(TY_LIST,TY_INT_T,NULL); }
    if(!strcmp(name,"hash")){ ck_positional(c,e,1,1,name); Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) return pending(c,line,"the argument of hash()");
        if(t->k==TY_LIST||t->k==TY_DICT||t->k==TY_SET) ck_err(c,line,"unhashable type: '%s'",t->k==TY_LIST?"list":t->k==TY_DICT?"dict":"set");
        return TY_INT_T; }
    if(!strcmp(name,"id")){ ck_positional(c,e,1,1,name); Ty *t=ty_find(arg(c,e,0)); if(t->k==TY_VAR) return pending(c,line,"the argument of id()");
        if(t->k==TY_FLOAT) ck_err(c,line,"id() of a float is not supported in compiled code");
        return TY_INT_T; }                                         /* (an object's address; an int: its value) */
    if(!strcmp(name,"super")) ck_err(c,line,"super() can only be used as super().method(...)");
    ck_err(c,line,"name '%s' is not defined",name);
}

/* name=value arguments of a built-in method -> their places among the
   positional ones (skipped ones before them become None: the default) */
void kw_to_pos(Ck *c, Expr *e, const char *m, const char *const *names, int n){
    int has=0;
    for(int i=0;i<e->count;i++) if(e->items[i]->akind==3) has=1;
    if(!has) return;
    Expr **p=MPY_NEW_ARR(Expr*,n); int np=0;
    memset(p,0,sizeof(Expr*)*(size_t)n);
    for(int i=0;i<e->count;i++){ Expr *a=e->items[i];
        if(a->akind==0){ if(np>=n) ck_err(c,e->line,"%s() takes at most %d arguments",m,n); p[np++]=a; continue; }
        if(a->akind!=3) ck_err(c,e->line,"%s() takes no *args or **kwargs in compiled code",m);
        int k=-1; for(int j=0;j<n;j++) if(!strcmp(names[j],a->kw)) k=j;
        if(k<0) ck_err(c,e->line,"%s() got an unexpected keyword argument '%s'",m,a->kw);
        if(p[k]) ck_err(c,e->line,"argument for %s() given by name ('%s') and position",m,a->kw);
        a->akind=0; p[k]=a;
    }
    int last=-1;
    for(int j=0;j<n;j++) if(p[j]) last=j;
    for(int j=0;j<=last;j++) if(!p[j]){ Expr *x=MPY_NEW0(Expr); x->kind=EXPR_NONE; x->line=e->line; p[j]=x; }
    e->items=p; e->count=last+1;
}
/* start / end of find, count, startswith ...: ints (None: the default) */
void ck_range_args(Ck *c, Expr *e, int first){
    for(int i=first;i<e->count;i++) if(!is_none(e->items[i])) expect(c,TY_INT_T,arg(c,e,i),e->line,i==first?"the start":"the end");
}
/* the built-in types a program can name as values (type objects) */
int is_builtin_type_name(const char *name){
    static const char *const names[]={"int","str","float","bool","bytes","list","dict","set","tuple","object","type","frozenset","bytearray","complex",NULL};
    for(int i=0;names[i];i++) if(!strcmp(name,names[i])) return 1;
    return 0;
}
