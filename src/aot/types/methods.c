/* ========================= Typed compiler, types: methods of built-in types =========================
   Methods of str, bytes, list, dict, set and the other built-in types. */

#include "check.h"

/* bytes.upper ... -> rt_bytes_case's number; bytes.isalpha ... -> rt_bytes_is's; or -1 */
int bytes_case_mode(const char *m){
    static const char *const names[]={"upper","lower","swapcase","title","capitalize"};
    for(int i=0;i<5;i++) if(!strcmp(m,names[i])) return i;
    return -1;
}
int bytes_is_mode(const char *m){
    static const char *const names[]={"isalpha","isdigit","isalnum","isspace","islower","isupper","istitle","isascii"};
    for(int i=0;i<8;i++) if(!strcmp(m,names[i])) return i;
    return -1;
}
/* str.isalpha ... -> the runtime's number for it (rt_str_is), or -1 */
int str_is_mode(const char *m){
    static const char *const names[]={"isalpha","isdigit","isalnum","isspace","islower","isupper","isdecimal","isascii","isnumeric","istitle","isprintable","isidentifier"};
    for(int i=0;i<12;i++) if(!strcmp(m,names[i])) return i;
    return -1;
}

/* Methods of str / list / dict / set. */
/* s.encode(enc, errors) / b.decode(...) / str(b, enc) / bytes(s, enc): the codecs of codecs.py (its
   fast path is the built-in utf-8; the others, and errors reported as CPython does, are Python) */
Ty *ck_codec_call(Ck *c, Expr *e, Expr *obj, int decode, int first){
    int line=e->line; Expr *enc=NULL, *errs=NULL;
    for(int i=first;i<e->count;i++){ Expr *a=e->items[i]; if(a->akind) ck_err(c,line,"%s() takes positional arguments here",decode?"decode":"encode");
        if(i==first) enc=a; else if(i==first+1) errs=a; else ck_err(c,line,"%s() takes at most 2 arguments",decode?"decode":"encode"); }
    if(!enc || is_none(enc)) enc=py_front_expr(c->mod->unit->path,"'utf-8'",line);
    if(!errs || is_none(errs)) errs=py_front_expr(c->mod->unit->path,"'strict'",line);
    e->count=0; e->a=name_expr(decode?"__mpy_decode":"__mpy_encode",line); xpush(e,obj); xpush(e,enc); xpush(e,errs);
    xinfo(e)->kind=X_NONE;
    return ck_call_inner(c,e);
}
int codecs_module(Ck *c){ return lookup(c,"__mpy_decode") && strcmp(c->mod->name,"codecs") && strcmp(c->mod->name,"_codecs_more"); }
Ty *ck_tmethod(Ck *c, Expr *e, Ty *t, const char *m){
    XInfo *xi=xinfo(e); xi->kind=X_TMETHOD; xi->name=m; int line=e->line;
    if(((t->k==TY_STR && !strcmp(m,"encode")) || (t->k==TY_BYTES && !strcmp(m,"decode"))) && codecs_module(c)){
        static const char *const n_codec[]={"encoding","errors"}; kw_to_pos(c,e,m,n_codec,2);
        return ck_codec_call(c,e,e->a->a,m[0]=='d',0); }
    #define NARGS(lo,hi) ck_positional(c,e,lo,hi,m)
    if(t->k==TY_STR){
        static const char *const n_split[]={"sep","maxsplit"}, *const n_lines[]={"keepends"}, *const n_repl[]={"old","new","count"}, *const n_tabs[]={"tabsize"};
        if(!strcmp(m,"split")||!strcmp(m,"rsplit")) kw_to_pos(c,e,m,n_split,2);
        else if(!strcmp(m,"splitlines")) kw_to_pos(c,e,m,n_lines,1);
        else if(!strcmp(m,"replace")) kw_to_pos(c,e,m,n_repl,3);
        else if(!strcmp(m,"expandtabs")) kw_to_pos(c,e,m,n_tabs,1);
        if(!strcmp(m,"upper")||!strcmp(m,"lower")||!strcmp(m,"capitalize")||!strcmp(m,"title")||!strcmp(m,"swapcase")||!strcmp(m,"casefold")){ NARGS(0,0); return TY_STR_T; }
        if(!strcmp(m,"encode")){ static const char *const n_enc[]={"encoding","errors"}; kw_to_pos(c,e,m,n_enc,2); NARGS(0,2);
            for(int i=0;i<e->count;i++) if(!is_none(e->items[i])) expect(c,TY_STR_T,arg(c,e,i),line,i?"errors":"the encoding"); return TY_BYTES_T; }
        if(!strcmp(m,"strip")||!strcmp(m,"lstrip")||!strcmp(m,"rstrip")){ NARGS(0,1); if(e->count && !is_none(e->items[0])) expect(c,TY_STR_T,arg(c,e,0),line,"the characters"); return TY_STR_T; }
        if(!strcmp(m,"startswith")||!strcmp(m,"endswith")){ NARGS(1,3);
            Ty *a=ty_find(arg(c,e,0));
            if(a->k==TY_TUPLE){ for(int i=0;i<a->nelems;i++) if(!unify(c,a->elems[i],TY_STR_T)) ck_err(c,line,"%s() takes a str or a tuple of str, not %s",m,ty_name(a)); }
            else expect(c,TY_STR_T,a,line,"the argument");
            ck_range_args(c,e,1); return TY_BOOL_T; }
        if(!strcmp(m,"find")||!strcmp(m,"rfind")||!strcmp(m,"index")||!strcmp(m,"rindex")||!strcmp(m,"count")){ NARGS(1,3); expect(c,TY_STR_T,arg(c,e,0),line,"the argument"); ck_range_args(c,e,1); return TY_INT_T; }
        if(!strcmp(m,"replace")){ NARGS(2,3); expect(c,TY_STR_T,arg(c,e,0),line,"the argument"); expect(c,TY_STR_T,arg(c,e,1),line,"the argument"); if(e->count==3) expect(c,TY_INT_T,arg(c,e,2),line,"the count"); return TY_STR_T; }
        if(!strcmp(m,"split")||!strcmp(m,"rsplit")){ NARGS(0,2); if(e->count && !is_none(e->items[0])) expect(c,TY_STR_T,arg(c,e,0),line,"the separator"); if(e->count==2) expect(c,TY_INT_T,arg(c,e,1),line,"maxsplit"); return ty_new(TY_LIST,TY_STR_T,NULL); }
        if(!strcmp(m,"join")){ NARGS(1,1); genexp_as_list(e,0); gen_as_list(c,e,0); Ty *a=ty_find(arg(c,e,0)); if(a->k==TY_VAR) return pending(c,line,"the argument of join()"); if(a->k==TY_STR) return TY_STR_T; if((a->k!=TY_LIST&&a->k!=TY_SET)||!unify(c,a->elem,TY_STR_T)) ck_err(c,line,"join() needs a list of str, not %s",ty_name(a)); return TY_STR_T; }
        if(!strcmp(m,"format")){
            Expr *recv=e->a->a;
            if(recv->kind!=EXPR_LITERAL||recv->tok->kind!=T_STRING) ck_err(c,line,"str.format() needs a literal format string in compiled code");
            for(int i=0;i<e->count;i++){ if(e->items[i]->akind==1||e->items[i]->akind==2) ck_err(c,line,"format(*args) is not supported"); no_void(c,ck_expr(c,e->items[i]),line); }
            return TY_STR_T; }
        if(!strcmp(m,"ljust")||!strcmp(m,"rjust")||!strcmp(m,"center")){ NARGS(1,2); expect(c,TY_INT_T,arg(c,e,0),line,"the width"); if(e->count==2) expect(c,TY_STR_T,arg(c,e,1),line,"the fill character"); return TY_STR_T; }
        if(!strcmp(m,"zfill")){ NARGS(1,1); expect(c,TY_INT_T,arg(c,e,0),line,"the width"); return TY_STR_T; }
        if(!strcmp(m,"splitlines")){ NARGS(0,1); if(e->count){ Ty *k=ty_find(arg(c,e,0)); if(k->k!=TY_BOOL&&k->k!=TY_INT) ck_err(c,line,"splitlines() takes a bool, not %s",ty_name(k)); } return ty_new(TY_LIST,TY_STR_T,NULL); }
        if(!strcmp(m,"partition")||!strcmp(m,"rpartition")){ NARGS(1,1); expect(c,TY_STR_T,arg(c,e,0),line,"the separator"); Ty *three[3]={TY_STR_T,TY_STR_T,TY_STR_T}; return ty_tuple(three,3); }
        if(!strcmp(m,"removeprefix")||!strcmp(m,"removesuffix")){ NARGS(1,1); expect(c,TY_STR_T,arg(c,e,0),line,"the argument"); return TY_STR_T; }
        if(!strcmp(m,"expandtabs")){ NARGS(0,1); if(e->count) expect(c,TY_INT_T,arg(c,e,0),line,"the tab size"); return TY_STR_T; }
        if(str_is_mode(m)>=0){ NARGS(0,0); return TY_BOOL_T; }
    } else if(t->k==TY_LIST){
        Ty *el=t->elem;
        if(!strcmp(m,"append")){ NARGS(1,1); expect(c,el,arg(c,e,0),line,"a list element"); return TY_VOID_T; }
        if(!strcmp(m,"pop")){ NARGS(0,1); if(e->count) expect(c,TY_INT_T,arg(c,e,0),line,"pop() index"); return el; }
        if(!strcmp(m,"insert")){ NARGS(2,2); expect(c,TY_INT_T,arg(c,e,0),line,"insert() index"); expect(c,el,arg(c,e,1),line,"a list element"); return TY_VOID_T; }
        if(!strcmp(m,"remove")){ NARGS(1,1); expect(c,el,arg(c,e,0),line,"a list element"); return TY_VOID_T; }
        if(!strcmp(m,"index")||!strcmp(m,"count")){ NARGS(1,1); expect(c,el,arg(c,e,0),line,"a list element"); return TY_INT_T; }
        if(!strcmp(m,"reverse")||!strcmp(m,"clear")){ NARGS(0,0); return TY_VOID_T; }
        if(!strcmp(m,"sort")){ take_sort_kwargs(c,e,1); NARGS(0,0);
            if(xi->rev) expect(c,TY_BOOL_T,ck_expr(c,xi->rev),line,"reverse=");
            if(xi->key){ ck_sort_key(c,xi->key,el,line); return TY_VOID_T; }
            Ty *f=ty_find(el); if(f->k==TY_VAR) pending(c,line,"the elements"); else if(!orderable(f)) ck_err(c,line,"sort() needs values that can be ordered, not %s",ty_name(f)); return TY_VOID_T; }
        if(!strcmp(m,"copy")){ NARGS(0,0); return t; }
        if(!strcmp(m,"extend")){ NARGS(1,1); genexp_as_list(e,0); gen_as_list(c,e,0); Ty *a=ty_find(arg(c,e,0)); if(a->k==TY_VAR) pending(c,line,"the argument"); else if(a->k!=TY_LIST||!unify(c,a->elem,el)) ck_err(c,line,"extend() needs a %s",ty_name(t)); return TY_VOID_T; }
    } else if(t->k==TY_DICT){
        Ty *v=t->elem, *k=ty_dkey(t);
        if(!strcmp(m,"get")){ NARGS(1,2); expect(c,k,arg(c,e,0),line,"a dictionary key"); if(e->count==2) expect(c,v,arg(c,e,1),line,"the default");
            else { Ty *nv=ty_var(); nv->opt=1; unify(c,v,nv); }       /* a missing key: None */
            return v; }
        if(!strcmp(m,"pop")){ NARGS(1,2); expect(c,k,arg(c,e,0),line,"a dictionary key"); if(e->count==2) expect(c,v,arg(c,e,1),line,"the default"); return v; }
        if(!strcmp(m,"setdefault")){ NARGS(2,2); expect(c,k,arg(c,e,0),line,"a dictionary key"); expect(c,v,arg(c,e,1),line,"the default"); return v; }
        if(!strcmp(m,"keys")){ NARGS(0,0); Ty *r=ty_new(TY_LIST,k,NULL); r->view=1; return r; }
        if(!strcmp(m,"values")){ NARGS(0,0); Ty *r=ty_new(TY_LIST,v,NULL); r->view=2; return r; }
        if(!strcmp(m,"clear")){ NARGS(0,0); return TY_VOID_T; }
        if(!strcmp(m,"copy")){ NARGS(0,0); return t; }
        if(!strcmp(m,"update")){ NARGS(1,1); expect(c,t,arg(c,e,0),line,"the argument"); return TY_VOID_T; }
        if(!strcmp(m,"items")){ NARGS(0,0); Ty *kv[2]={k,v}; Ty *r=ty_new(TY_LIST,ty_tuple(kv,2),NULL); r->view=3; return r; }
    } else if(t->k==TY_FILE){
        Ty *el=t->elem?t->elem:TY_STR_T;                       /* str, or bytes ("rb") */
        if(!strcmp(m,"read")||!strcmp(m,"readline")){ NARGS(0,0); return el; }
        if(!strcmp(m,"readlines")){ NARGS(0,0); return ty_new(TY_LIST,el,NULL); }
        if(!strcmp(m,"write")){ NARGS(1,1); expect(c,el,arg(c,e,0),line,"the text"); return TY_INT_T; }
        if(!strcmp(m,"close")){ NARGS(0,0); return TY_VOID_T; }
    } else if(t->k==TY_GEN){                                    /* g.send(v), g.close(), g.throw(exc) */
        if(!strcmp(m,"send")){ NARGS(1,1); Ty *st=aot_gen_part(t,0);
            if(!is_none(e->items[0])){ Ty *a=ck_expr_want(c,e->items[0],st); no_void(c,a,line); expect(c,t->elems[0],a,line,"the value sent"); }
            stop_value(c,t,line); return t->elem; }
        if(!strcmp(m,"__anext__")){ NARGS(0,0); if(!t->tup) ck_err(c,line,"'generator' object has no attribute '__anext__'"); stop_value(c,t,line); return t->elem; }
        if(!strcmp(m,"asend")){ NARGS(1,1); if(!t->tup) ck_err(c,line,"'generator' object has no attribute 'asend'");
            if(!is_none(e->items[0])){ Ty *a=ck_expr_want(c,e->items[0],aot_gen_part(t,0)); no_void(c,a,line); expect(c,t->elems[0],a,line,"the value sent"); }
            return t->elem; }
        if(!strcmp(m,"aclose")){ NARGS(0,0); if(!t->tup) ck_err(c,line,"'generator' object has no attribute 'aclose'"); return TY_VOID_T; }
        if(!strcmp(m,"athrow")){ NARGS(1,1); if(!t->tup) ck_err(c,line,"'generator' object has no attribute 'athrow'"); Ty *a=ty_find(arg(c,e,0));
            if(a->k==TY_VAR) return pending(c,line,"the exception");
            if(a->k!=TY_OBJ || !aot_is_exception(a->cls)) ck_err(c,line,"athrow() takes an exception, not %s",ty_name(a));
            return t->elem; }
        if(t->tup) ck_err(c,line,"'async_generator' object has no attribute '%s'",m);
        if(!strcmp(m,"close")){ NARGS(0,0); return TY_VOID_T; }
        if(!strcmp(m,"throw")){ NARGS(1,1); Ty *a=ty_find(arg(c,e,0));
            if(a->k==TY_VAR) return pending(c,line,"the exception");
            if(a->k!=TY_OBJ || !aot_is_exception(a->cls)) ck_err(c,line,"throw() takes an exception, not %s",ty_name(a));
            stop_value(c,t,line); return t->elem; }
    } else if(t->k==TY_TASK){
        if(!strcmp(m,"done")){ NARGS(0,0); return TY_BOOL_T; }
        if(!strcmp(m,"result")){ NARGS(0,0); return t->elem; }
    } else if(t->k==TY_BYTES){
        static const char *const n_split[]={"sep","maxsplit"}, *const n_lines[]={"keepends"}, *const n_repl[]={"old","new","count"}, *const n_dec[]={"encoding","errors"};
        if(!strcmp(m,"split")||!strcmp(m,"rsplit")) kw_to_pos(c,e,m,n_split,2);
        else if(!strcmp(m,"splitlines")) kw_to_pos(c,e,m,n_lines,1);
        else if(!strcmp(m,"replace")) kw_to_pos(c,e,m,n_repl,3);
        else if(!strcmp(m,"decode")) kw_to_pos(c,e,m,n_dec,2);
        if(!strcmp(m,"decode")){ NARGS(0,2); for(int i=0;i<e->count;i++) if(!is_none(e->items[i])) expect(c,TY_STR_T,arg(c,e,i),line,i?"errors":"the encoding"); return TY_STR_T; }
        if(!strcmp(m,"hex")){ static const char *const n_hex[]={"sep","bytes_per_sep"}; kw_to_pos(c,e,m,n_hex,2); NARGS(0,2);
            if(e->count){ Ty *a=ty_find(arg(c,e,0)); if(a->k!=TY_BYTES) expect(c,TY_STR_T,a,line,"the separator"); }
            if(e->count>1) expect(c,TY_INT_T,arg(c,e,1),line,"bytes_per_sep");
            return TY_STR_T; }
        if(bytes_case_mode(m)>=0){ NARGS(0,0); return TY_BYTES_T; }
        if(bytes_is_mode(m)>=0){ NARGS(0,0); return TY_BOOL_T; }
        if(!strcmp(m,"strip")||!strcmp(m,"lstrip")||!strcmp(m,"rstrip")){ NARGS(0,1); if(e->count && !is_none(e->items[0])) expect(c,TY_BYTES_T,arg(c,e,0),line,"the bytes to strip"); return TY_BYTES_T; }
        if(!strcmp(m,"startswith")||!strcmp(m,"endswith")){ NARGS(1,3);
            Ty *a=ty_find(arg(c,e,0));
            if(a->k==TY_TUPLE){ for(int i=0;i<a->nelems;i++) if(!unify(c,a->elems[i],TY_BYTES_T)) ck_err(c,line,"%s first arg must be bytes or a tuple of bytes, not %s",m,ty_name(a)); }
            else expect(c,TY_BYTES_T,a,line,"the argument");
            ck_range_args(c,e,1); return TY_BOOL_T; }
        if(!strcmp(m,"find")||!strcmp(m,"rfind")||!strcmp(m,"index")||!strcmp(m,"rindex")||!strcmp(m,"count")){ NARGS(1,3);
            Ty *a=ty_find(arg(c,e,0)); if(a->k!=TY_BYTES&&a->k!=TY_INT&&a->k!=TY_BOOL) expect(c,TY_BYTES_T,a,line,"the argument");
            ck_range_args(c,e,1); return TY_INT_T; }
        if(!strcmp(m,"replace")){ NARGS(2,3); expect(c,TY_BYTES_T,arg(c,e,0),line,"the argument"); expect(c,TY_BYTES_T,arg(c,e,1),line,"the argument"); if(e->count==3) expect(c,TY_INT_T,arg(c,e,2),line,"the count"); return TY_BYTES_T; }
        if(!strcmp(m,"split")||!strcmp(m,"rsplit")){ NARGS(0,2); if(e->count && !is_none(e->items[0])) expect(c,TY_BYTES_T,arg(c,e,0),line,"the separator"); if(e->count==2) expect(c,TY_INT_T,arg(c,e,1),line,"maxsplit"); return ty_new(TY_LIST,TY_BYTES_T,NULL); }
        if(!strcmp(m,"join")){ NARGS(1,1); genexp_as_list(e,0); gen_as_list(c,e,0); Ty *a=ty_find(arg(c,e,0)); if(a->k==TY_VAR) return pending(c,line,"the argument of join()");
            if((a->k!=TY_LIST&&a->k!=TY_SET)||!unify(c,a->elem,TY_BYTES_T)) ck_err(c,line,"join() needs a list of bytes, not %s",ty_name(a)); return TY_BYTES_T; }
        if(!strcmp(m,"partition")||!strcmp(m,"rpartition")){ NARGS(1,1); expect(c,TY_BYTES_T,arg(c,e,0),line,"the separator"); Ty *three[3]={TY_BYTES_T,TY_BYTES_T,TY_BYTES_T}; return ty_tuple(three,3); }
        if(!strcmp(m,"removeprefix")||!strcmp(m,"removesuffix")){ NARGS(1,1); expect(c,TY_BYTES_T,arg(c,e,0),line,"the argument"); return TY_BYTES_T; }
        if(!strcmp(m,"splitlines")){ NARGS(0,1); if(e->count){ Ty *k=ty_find(arg(c,e,0)); if(k->k!=TY_BOOL&&k->k!=TY_INT) ck_err(c,line,"splitlines() takes a bool, not %s",ty_name(k)); } return ty_new(TY_LIST,TY_BYTES_T,NULL); }
    } else if(t->k==TY_INT||t->k==TY_BOOL){
        if(!strcmp(m,"to_bytes")){ static const char *const n_tb[]={"length","byteorder","signed"}; kw_to_pos(c,e,m,n_tb,3); NARGS(0,3);
            if(e->count>0 && !is_none(e->items[0])) expect(c,TY_INT_T,arg(c,e,0),line,"length");
            if(e->count>1 && !is_none(e->items[1])) expect(c,TY_STR_T,arg(c,e,1),line,"byteorder");
            if(e->count>2) expect(c,TY_BOOL_T,arg(c,e,2),line,"signed");
            return TY_BYTES_T; }
        if(!strcmp(m,"bit_length")||!strcmp(m,"bit_count")){ NARGS(0,0); return TY_INT_T; }
    } else if(t->k==TY_FLOAT){
        if(!strcmp(m,"is_integer")){ NARGS(0,0); return TY_BOOL_T; }
    } else if(t->k==TY_SET){
        Ty *el=t->elem;
        if(!strcmp(m,"add")||!strcmp(m,"remove")||!strcmp(m,"discard")){ NARGS(1,1); expect(c,el,arg(c,e,0),line,"a set element"); return TY_VOID_T; }
        if(!strcmp(m,"clear")){ NARGS(0,0); return TY_VOID_T; }
        if(!strcmp(m,"copy")){ NARGS(0,0); return t; }
        if(!strcmp(m,"pop")){ NARGS(0,0); return el; }
        if(!strcmp(m,"union")||!strcmp(m,"intersection")||!strcmp(m,"difference")||!strcmp(m,"symmetric_difference")){ NARGS(1,1); expect(c,t,arg(c,e,0),line,"the argument"); return t; }
        if(!strcmp(m,"update")||!strcmp(m,"intersection_update")||!strcmp(m,"difference_update")||!strcmp(m,"symmetric_difference_update")){ NARGS(1,1); expect(c,t,arg(c,e,0),line,"the argument"); return TY_VOID_T; }
        if(!strcmp(m,"issubset")||!strcmp(m,"issuperset")||!strcmp(m,"isdisjoint")){ NARGS(1,1); expect(c,t,arg(c,e,0),line,"the argument"); return TY_BOOL_T; }
    }
    #undef NARGS
    ck_err(c,line,"%s has no method '%s'",ty_name(t),m);
}

