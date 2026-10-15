/* ========================= Interpreter, modules: import =========================
   Module objects, compiling and running source, finding and loading
   modules (packages, relative imports), the main script. */

#include "modules.h"

Value mp_new_module(const char *name){
    ModuleObj *m=(ModuleObj*)mp_alloc(T_module,sizeof(ModuleObj));
    m->name=AS_STR(mp_intern(name)); m->dict=AS_DICT(mp_dict());
    mp_dict_set_s(m->dict,"__name__",v_obj(m->name));
    mp_dict_set_s(m->dict,"__doc__",v_none());
    return v_obj(m);
}
DictObj *mdict(Value m){ return ((ModuleObj*)m.u.o)->dict; }
void set_fn(Value m, const char *name, NFn fn){ mp_dict_set_s(mdict(m),name,mp_native(name,fn)); }
Value register_module(const char *name){ Value m=mp_new_module(name); mp_dict_set_s(mp_modules,name,m); return m; }

/* ---------------------------------------------------------------- running source */
static Value syntax_error(const char *file, int line, const char *msg){
    Value args=mp_tuple(2,NULL);
    AS_TUPLE(args)->items[0]=mp_str(msg);
    Value det[4]={mp_str(file),v_int(line),v_int(0),v_none()};
    AS_TUPLE(args)->items[1]=mp_tuple(4,det);
    Value e=mp_exc_args(E_SyntaxError,args);
    ExcObj *x=AS_EXC(e); x->tb=mp_list(0,NULL);
    Value ent[3]={mp_str(file),v_int(line),mp_str("<module>")}; mp_list_append(x->tb,mp_tuple(3,ent));
    return e;
}
static CodeObj *compile_source(const char *src, const char *file, const char *modname){
    PyParse pp;
    if(py_parse(&pp,file,src,strlen(src))){ char m[512]; snprintf(m,sizeof m,"%s",pp.error); mp_raise(syntax_error(file,pp.error_line,m)); }
    char *err=NULL; int line=0;
    CodeObj *co=mp_compile(pp.mod,file,modname,&err,&line);
    if(!co) mp_raise(syntax_error(file,line,err));
    return co;
}
/* compile(source, filename, mode): a code object; mode "eval": the expression's value is left in
   __mp_eval__ (CO_EVAL marks it), "exec" / "single": statements */
#define CO_EVAL 0x40000000
Value mp_compile_text(Value src, const char *file, const char *mode){
    if(IS(src,T_code)) return src;
    const char *s;
    if(IS_STR(src)) s=mp_cstr(src);
    else if(IS(src,T_bytes)){ Value t=mp_callmethod(src,"decode",0,NULL); s=mp_cstr(t); }
    else mp_raise_t(E_TypeError,"compile() arg 1 must be a string, bytes or AST object");
    if(strlen(s)!=(size_t)AS_STR(IS_STR(src)?src:mp_str(s))->len) mp_raise_t(E_SyntaxError,"source code string cannot contain null bytes");
    int ev=!strcmp(mode,"eval");
    if(!ev && strcmp(mode,"exec") && strcmp(mode,"single")) mp_raise_t(E_ValueError,"compile() mode must be 'exec', 'eval' or 'single'");
    char *text;
    if(ev){ while(*s==' '||*s=='\t') s++;                       /* (eval: leading spaces and tabs are allowed) */
        size_t n=strlen(s); text=(char*)xmalloc(n+32); snprintf(text,n+32,"__mp_eval__ = (\n%s\n)\n",s); }
    else text=xstrdup2(s);
    mp_remember_source(file,text);
    CodeObj *co=compile_source(text,file,"__main__");
    free(text);
    if(ev) co->flags|=CO_EVAL;
    return v_obj(co);
}
/* exec / eval of a code object (or a source) in globals and locals (NULL: the caller's) */
Value mp_exec_code(Value code, DictObj *globals, DictObj *locals, int is_eval, int locals_given){
    (void)locals_given;
    Frame *f=mp_ts->frame;
    DictObj *g= globals ? globals : f ? f->globals : mp_builtins;
    if(!mp_dict_get_s(g,"__builtins__",NULL)){ Value bn=mp_str("builtins"); mp_dict_set_s(g,"__builtins__",mp_builtin_import(1,&bn,NULL)); }
    DictObj *l=locals;
    if(!l && !globals && f && !(f->code->flags&CO_MODULE)) l=AS_DICT(mp_frame_locals(f));   /* (a function's: a snapshot) */
    if(!l && !globals && f && f->locals) l=f->locals;                                          /* (a class body's) */
    if(l==g) l=NULL;
    CodeObj *co=(CodeObj*)code.u.o;
    if(is_eval && !(co->flags&CO_EVAL)) mp_raise_t(E_TypeError,"eval() of an exec code object is not supported here");
    DictObj *run=g;                          /* separate locals: the code runs in globals + locals, its new names go to locals */
    if(l){ run=AS_DICT(mp_dict()); int64_t pos=0; Value k,v;
        while(mp_dict_next(g,&pos,&k,&v)) mp_dict_set(run,k,v);
        pos=0; while(mp_dict_next(l,&pos,&k,&v)) mp_dict_set(run,k,v); }
    Value keep=v_obj(run); (void)keep;
    mp_run_code(co,run,NULL);
    Value r=v_none();
    if(co->flags&CO_EVAL){ if(!mp_dict_get_s(run,"__mp_eval__",&r)) r=v_none(); mp_dict_del(run,mp_str("__mp_eval__")); }
    if(l){ int64_t pos=0; Value k,v;
        while(mp_dict_next(run,&pos,&k,&v)){ Value old; int ing=mp_dict_get(g,k,&old), inl=mp_dict_get(l,k,NULL);
            if(inl || !ing || old.k!=v.k || old.u.i!=v.u.i) mp_dict_set(l,k,v); } }
    return is_eval ? r : v_none();
}
/* `from __future__ import annotations` in a module: its annotations stay strings */
static int future_annotations(DictObj *g){
    Value fm, a, mine;
    if(!mp_dict_get_s(mp_modules,"__future__",&fm) || !mp_dict_get_s(g,"annotations",&mine)) return 0;
    return mp_dict_get_s(mdict(fm),"annotations",&a) && a.k==mine.k && a.u.o==mine.u.o && mine.k!=V_NONE;
}
/* What an annotation evaluates to when __annotations__ is read (CPython 3.14: lazily), in the
   defining module's globals (and a class's namespace) */
Value mp_eval_annotation(const char *text, DictObj *globals, DictObj *overlay){
    if(future_annotations(globals)) return mp_str(text);
    size_t n=strlen(text); char *src=(char*)xmalloc(n+32);
    snprintf(src,n+32,"__mp_ann__ = (%s)\n",text);
    CodeObj *co=compile_source(src,"<annotate>","__main__"); free(src);
    Value keep=v_obj(co); (void)keep;
    Value tmp=mp_dict(); int64_t pos=0; Value k,v;
    while(mp_dict_next(globals,&pos,&k,&v)) mp_dict_set(AS_DICT(tmp),k,v);
    if(overlay){ pos=0; while(mp_dict_next(overlay,&pos,&k,&v)) mp_dict_set(AS_DICT(tmp),k,v); }
    mp_run_code(co,AS_DICT(tmp),NULL);
    Value r; if(!mp_dict_get_s(AS_DICT(tmp),"__mp_ann__",&r)) r=v_none();
    return r;
}
static void exec_into(Value mod, const char *src, const char *file){
    mp_remember_source(file,src);
    CodeObj *co=compile_source(src,file,mp_cstr(v_obj(((ModuleObj*)mod.u.o)->name)));
    Value keep=v_obj(co); (void)keep;
    if(!mp_dict_get_s(mdict(mod),"__builtins__",NULL)){ Value bn=mp_str("builtins"); mp_dict_set_s(mdict(mod),"__builtins__",mp_builtin_import(1,&bn,NULL)); }
    mp_run_code(co,mdict(mod),NULL);
}

/* ---------------------------------------------------------------- import */
static char *dir_of_globals(DictObj *g){
    Value f;
    if(g && mp_dict_get_s(g,"__file__",&f) && IS_STR(f)) return mpy_fs_dirname(mp_cstr(f));
    return xstrdup2(mp_main_dir);
}
static int is_package_file(const char *path){
    const char *s=strrchr(path,'/'); s=s?s+1:path;
    return !strncmp(s,"__init__.",9);
}
/* load module `full` (its parent package already loaded, or none) */
static Value load_module(const char *full, DictObj *importer, int leaf){
    Value m;
    if(mp_dict_get_s(mp_modules,full,&m)) return m;
    { const char *al=mpy_stdlib_alias(full);                          /* os.path: posixpath */
      if(al){ m=load_module(al,importer,1); mp_dict_set_s(mp_modules,full,m); return m; } }
    const char *ps=py_source(full);
    if(ps){
        m=register_module(full);
        char file[128]; snprintf(file,sizeof file,"<minipy:%s>",full);
        Catch c;
        if(!CATCH_BEGIN(c)){ exec_into(m,ps,file); CATCH_END(c); }
        else { Value e=mp_catch_exc(&c); mp_dict_del(mp_modules,mp_str(full)); mp_raise(e); }
        return m;
    }
    m=native_module(full);
    if(m.k!=V_UNDEF) return m;
    { int pkg=0; const char *ss=mpy_stdlib_source(full,&pkg);         /* the standard library written in Python */
      if(ss){
        m=register_module(full);
        char file[160]; snprintf(file,sizeof file,"<minipy:%s>",full);
        mp_dict_set_s(mdict(m),"__package__",mp_str(pkg?full:""));
        if(pkg){ char pk[160]; const char *d=strrchr(full,'.'); (void)d; snprintf(pk,sizeof pk,"<minipy:%s>",full); Value pl=mp_list(0,NULL); mp_list_append(pl,mp_str(pk)); mp_dict_set_s(mdict(m),"__path__",pl); }
        else { const char *d=strrchr(full,'.'); char pk[160]; snprintf(pk,sizeof pk,"%.*s",d?(int)(d-full):0,full); mp_dict_set_s(mdict(m),"__package__",mp_str(pk)); }
        Catch c;
        if(!CATCH_BEGIN(c)){ exec_into(m,ss,file); CATCH_END(c); }
        else { Value e=mp_catch_exc(&c); mp_dict_del(mp_modules,mp_str(full)); mp_raise(e); }
        if(!strcmp(full,"io")){ Value o; if(mp_dict_get_s(mp_builtins,"open",&o)) mp_dict_set_s(mdict(m),"open",o); }   /* io.open is open */
        return m;
      } }
    /* a file: inside the parent package's folder, else the importer's, the main script's, MINIPYPATH, lib */
    char *path=NULL;
    const char *dot=strrchr(full,'.');
    if(dot){
        char parent[256]; snprintf(parent,sizeof parent,"%.*s",(int)(dot-full),full);
        Value pm, pathv;
        if(mp_dict_get_s(mp_modules,parent,&pm) && IS(pm,T_module) && mp_dict_get_s(mdict(pm),"__path__",&pathv) && IS_LIST(pathv) && AS_LIST(pathv)->len)
            path=mpy_fs_find_module(mp_cstr(AS_LIST(pathv)->items[0]),dot+1);
    }
    if(!path){ char *dir=dir_of_globals(importer); path=mpy_fs_find_module(dir,full); free(dir); }
    if(!path) path=mpy_fs_find_module(mp_main_dir,full);
    if(!path){
        if(!leaf){ m=register_module(full); return m; }        /* a namespace package */
        Value nm=mp_str(full);
        Value e=mp_exc(E_ModuleNotFoundError,"No module named '%s'",full);
        ExcObj *x=AS_EXC(e); if(!x->dict) x->dict=AS_DICT(mp_dict()); mp_dict_set_s(x->dict,"name",nm);
        mp_raise(e);
    }
    char *err=NULL;
    char *src=mpy_fs_try_read_file(path,&err);
    if(!src){ free(err); Value e=mp_exc(E_ImportError,"cannot read %s",path); free(path); mp_raise(e); }
    m=register_module(full);
    mp_dict_set_s(mdict(m),"__file__",mp_str(path));
    if(is_package_file(path)){
        char *d=mpy_fs_dirname(path); Value pl=mp_list(0,NULL); mp_list_append(pl,mp_str(d)); free(d);
        mp_dict_set_s(mdict(m),"__path__",pl);
        mp_dict_set_s(mdict(m),"__package__",mp_str(full));
    } else {
        char pk[256]; snprintf(pk,sizeof pk,"%.*s",dot?(int)(dot-full):0,full);
        mp_dict_set_s(mdict(m),"__package__",mp_str(pk));
    }
    Catch c;
    if(!CATCH_BEGIN(c)){ exec_into(m,src,path); CATCH_END(c); }
    else { Value e=mp_catch_exc(&c); free(src); free(path); mp_dict_del(mp_modules,mp_str(full)); mp_raise(e); }
    free(src); free(path);
    return m;
}
static char *resolve_name(const char *name, DictObj *g, int level){
    if(level==0) return xstrdup2(name);
    Value pk;
    char base[512]="";
    if(g && mp_dict_get_s(g,"__package__",&pk) && IS_STR(pk)) snprintf(base,sizeof base,"%s",mp_cstr(pk));
    else if(g && mp_dict_get_s(g,"__name__",&pk) && IS_STR(pk)){ snprintf(base,sizeof base,"%s",mp_cstr(pk)); char *d=strrchr(base,'.'); if(d) *d=0; else base[0]=0; }
    if(!base[0]) mp_raise_t(E_ImportError,"attempted relative import with no known parent package");
    for(int i=1;i<level;i++){ char *d=strrchr(base,'.'); if(!d) mp_raise_t(E_ImportError,"attempted relative import beyond top-level package"); *d=0; }
    char *r=(char*)xmalloc(strlen(base)+strlen(name)+2);
    if(name[0]) sprintf(r,"%s.%s",base,name); else strcpy(r,base);
    return r;
}
Value mp_import(const char *name, DictObj *g, int level, int leaf){
    char *volatile full=resolve_name(name,g,level);         /* (volatile: freed after a longjmp) */
    Value top=v_undef(), parent=v_undef(), m=v_undef();
    char prefix[512];
    for(const char *p=full;;){
        const char *dot=strchr(p,'.');
        int n= dot ? (int)(dot-full) : (int)strlen(full);
        snprintf(prefix,sizeof prefix,"%.*s",n,full);
        int is_leaf= dot==NULL;
        Catch c;
        if(!CATCH_BEGIN(c)){ m=load_module(prefix,g,is_leaf); CATCH_END(c); }
        else { Value e=mp_catch_exc(&c); free(full); mp_raise(e); }
        if(parent.k!=V_UNDEF && IS(parent,T_module)) mp_dict_set_s(mdict(parent),dot?strrchr(prefix,'.')?strrchr(prefix,'.')+1:prefix:(strrchr(prefix,'.')?strrchr(prefix,'.')+1:prefix),m);
        if(top.k==V_UNDEF) top=m;
        parent=m;
        if(!dot) break;
        p=dot+1;
    }
    free(full);
    return leaf || level>0 ? m : top;
}
Value mp_import_from(Value module, Value name){
    Value v;
    if(mp_getattr_opt(module,name,&v)) return v;
    if(IS(module,T_module)){
        char sub[512]; snprintf(sub,sizeof sub,"%s.%s",((ModuleObj*)module.u.o)->name->s,mp_cstr(name));
        Catch c;
        if(!CATCH_BEGIN(c)){ v=load_module(sub,mdict(module),1); CATCH_END(c); mp_dict_set(mdict(module),name,v); return v; }
        Value e=mp_catch_exc(&c);
        if(!mp_isinstance(e,E_ModuleNotFoundError)) mp_raise(e);
        { Value en; if(mp_getattr_opt(e,mp_str("name"),&en) && IS_STR(en) && strcmp(mp_cstr(en),sub)) mp_raise(e); }   /* (another module missing: that error) */
        Value fl=v_none(); mp_dict_get_s(mdict(module),"__file__",&fl);
        Value ie= IS_STR(fl) ? mp_exc(E_ImportError,"cannot import name '%s' from '%s' (%s)",mp_cstr(name),((ModuleObj*)module.u.o)->name->s,mp_cstr(fl))
                             : mp_exc(E_ImportError,"cannot import name '%s' from '%s' (unknown location)",mp_cstr(name),((ModuleObj*)module.u.o)->name->s);
        ExcObj *x=AS_EXC(ie); if(!x->dict) x->dict=AS_DICT(mp_dict());
        mp_dict_set_s(x->dict,"name",v_obj(((ModuleObj*)module.u.o)->name)); mp_dict_set_s(x->dict,"name_from",name); mp_dict_set_s(x->dict,"path",fl);
        mp_raise(ie);
    }
    mp_raise_t(E_ImportError,"cannot import name '%s'",mp_cstr(name));
}
Value mp_builtin_import(int argc, Value *argv, TupleObj *kw){
    int np=mod_npos(argc,kw);
    if(np<1 || !IS_STR(argv[0])) mp_raise_t(E_TypeError,"__import__() argument 1 must be str");
    Value g= np>1 ? argv[1] : mod_kwarg(argc,argv,kw,"globals",v_none());
    Value fl= np>3 ? argv[3] : mod_kwarg(argc,argv,kw,"fromlist",v_none());
    Value lv= np>4 ? argv[4] : mod_kwarg(argc,argv,kw,"level",v_int(0));
    return mp_import(mp_cstr(argv[0]),IS_DICT(g)?AS_DICT(g):NULL,(int)mp_index(lv,"level"),mp_truth(fl));
}
Value mp_run_main(const char *path){
    char *err=NULL;
    char *src=mpy_fs_try_read_file(path,&err);
    if(!src){ fprintf(stderr,"minipy: can't open file '%s': %s\n",path,err?err:"no such file"); free(err); exit(2); }
    Value m=register_module("__main__");
    mp_dict_set_s(mdict(m),"__file__",mp_str(path));
    mp_dict_set_s(mdict(m),"__package__",v_none());
    { Value bn=mp_str("builtins"); mp_dict_set_s(mdict(m),"__builtins__",mp_builtin_import(1,&bn,NULL)); }
    exec_into(m,src,path);
    free(src);
    return m;
}

