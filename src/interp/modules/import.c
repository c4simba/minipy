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
/* every line number of a tree moved by d (eval's wrapper adds a line before the expression) */
static void shift_lines(PyNode *n, int d){
    if(!n) return;
    n->line+=d; if(n->line<1) n->line=1;
    n->end_line+=d; if(n->end_line<1) n->end_line=1;
    for(int i=0;i<4;i++) shift_lines(n->n[i],d);
    for(int i=0;i<5;i++) for(int j=0;j<n->L[i].n;j++) shift_lines(n->L[i].v[j],d);
}
/* (the tree and the compiler's scopes are made in a region, freed with it when the code is made;
   strip: what mp_strip adds - the carets' places of a module a file holds are not kept: a traceback
   compiles its text again to show them) */
static CodeObj *compile_source_at(const char *src, const char *file, const char *modname, int shift, int strip){
    PyParse pp; PpyRegion reg={0}, *outer=ppy_region;
    int strip0=mp_strip;
    ppy_region=&reg;
    if(py_parse(&pp,file,src,strlen(src))){
        char m[512]; snprintf(m,sizeof m,"%s",pp.error); free(pp.error);
        ppy_region=outer; ppy_region_free(&reg);
        mp_raise(syntax_error(file,pp.error_line+shift>0?pp.error_line+shift:1,m)); }
    if(shift) shift_lines(pp.mod,shift);
    char *err=NULL; int line=0; CodeObj *co=NULL;
    Catch c;
    mp_strip|=strip;
    if(!CATCH_BEGIN(c)){ co=mp_compile(pp.mod,file,modname,&err,&line); CATCH_END(c); }
    else { Value e=mp_catch_exc(&c); mp_strip=strip0; ppy_region=outer; ppy_region_free(&reg); mp_raise(e); }
    mp_strip=strip0; ppy_region=outer; ppy_region_free(&reg);
    if(!co){ char m[512]; snprintf(m,sizeof m,"%s",err); free(err); mp_raise(syntax_error(file,line,m)); }
    return co;
}
static CodeObj *compile_source(const char *src, const char *file, const char *modname){ return compile_source_at(src,file,modname,0,0); }
/* the code of a module's text compiled again with the carets' places (traceback.c: for those its code
   left out), or NULL (a SyntaxError: the file changed) */
CodeObj *mp_compile_places(const char *src, const char *file){
    CodeObj *co=NULL; int strip0=mp_strip;
    mp_strip=0;
    Catch c;
    if(!CATCH_BEGIN(c)){ co=compile_source(src,file,"__main__"); CATCH_END(c); }
    else mp_catch_exc(&c);
    mp_strip=strip0;
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
    mp_remember_source(file,ev?s:text);                      /* (eval: its lines as given: the wrapper is not shown) */
    CodeObj *co=compile_source_at(text,file,"__main__",ev?-1:0,0);
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
/* the source of a file a traceback names, read again: a module of the standard
   library, or a file; NULL if there is none (the caller frees it) */
char *mp_file_source(const char *file){
    if(!strncmp(file,"<plainpy:",9)){
        char name[200]; size_t n=strlen(file+9);
        if(n<2 || n>=sizeof name || file[8+n]!='>') return NULL;
        memcpy(name,file+9,n-1); name[n-1]=0;
        const char *ps=py_source(name);
        return ps ? xstrdup2(ps) : ppy_stdlib_source(name,NULL);
    }
    if(file[0]=='<') return NULL;
    char *err=NULL, *t=ppy_fs_try_read_file(file,&err);
    free(err);
    return t;
}
static void exec_code_into(Value mod, CodeObj *co){
    Value keep=v_obj(co); (void)keep;
    if(!mp_dict_get_s(mdict(mod),"__builtins__",NULL)){ Value bn=mp_str("builtins"); mp_dict_set_s(mdict(mod),"__builtins__",mp_builtin_import(1,&bn,NULL)); }
    mp_run_code(co,mdict(mod),NULL);
}
static int is_bytecode_file(const char *path){ size_t n=strlen(path); return n>5 && !strcmp(path+n-5,".ppyc"); }
static void exec_into(Value mod, const char *src, const char *file){
    if(file[0]=='<' && strncmp(file,"<plainpy:",9)) mp_remember_source(file,src);   /* (no file holds it) */
    exec_code_into(mod,compile_source_at(src,file,mp_cstr(v_obj(((ModuleObj*)mod.u.o)->name)),0,file[0]=='<' && strncmp(file,"<plainpy:",9) ? 0 : MP_STRIP_PLACES));
}

/* ---------------------------------------------------------------- import */
static char *dir_of_globals(DictObj *g){
    Value f;
    if(g && mp_dict_get_s(g,"__file__",&f) && IS_STR(f)) return ppy_fs_dirname(mp_cstr(f));
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
    { const char *al=ppy_stdlib_alias(full);                          /* os.path: posixpath */
      if(al){ m=load_module(al,importer,1); mp_dict_set_s(mp_modules,full,m); return m; } }
    const char *ps=py_source(full);
    if(ps){
        m=register_module(full);
        char file[128]; snprintf(file,sizeof file,"<plainpy:%s>",full);
        Catch c;
        if(!CATCH_BEGIN(c)){ exec_into(m,ps,file); CATCH_END(c); }
        else { Value e=mp_catch_exc(&c); mp_dict_del(mp_modules,mp_str(full)); mp_raise(e); }
        return m;
    }
    m=native_module(full);
    if(m.k!=V_UNDEF) return m;
    { int pkg=0; size_t bclen=0; char *ss=NULL;
      const void *bc=ppy_stdlib_bytecode(full,&bclen,&pkg);           /* the standard library written in Python: compiled (a library of bytecode), */
      if(!bc) ss=ppy_stdlib_source(full,&pkg);                        /* or its source */
      if(ss || bc){
        m=register_module(full);
        char file[160]; snprintf(file,sizeof file,"<plainpy:%s>",full);
        mp_dict_set_s(mdict(m),"__package__",mp_str(pkg?full:""));
        if(pkg){ char pk[160]; const char *d=strrchr(full,'.'); (void)d; snprintf(pk,sizeof pk,"<plainpy:%s>",full); Value pl=mp_list(0,NULL); mp_list_append(pl,mp_str(pk)); mp_dict_set_s(mdict(m),"__path__",pl); }
        else { const char *d=strrchr(full,'.'); char pk[160]; snprintf(pk,sizeof pk,"%.*s",d?(int)(d-full):0,full); mp_dict_set_s(mdict(m),"__package__",mp_str(pk)); }
        Catch c;
        if(!CATCH_BEGIN(c)){ if(bc) exec_code_into(m,mp_bytecode_load(bc,bclen,file,1)); else exec_into(m,ss,file); CATCH_END(c); free(ss); }
        else { Value e=mp_catch_exc(&c); free(ss); mp_dict_del(mp_modules,mp_str(full)); mp_raise(e); }
        if(!strcmp(full,"io")){ Value o; if(mp_dict_get_s(mp_builtins,"open",&o)) mp_dict_set_s(mdict(m),"open",o); }   /* io.open is open */
        return m;
      } }
    /* a file: inside the parent package's folder, else the importer's, the main script's, PLAINPYPATH, lib */
    char *path=NULL;
    const char *dot=strrchr(full,'.');
    if(dot){
        char parent[256]; snprintf(parent,sizeof parent,"%.*s",(int)(dot-full),full);
        Value pm, pathv;
        if(mp_dict_get_s(mp_modules,parent,&pm) && IS(pm,T_module) && mp_dict_get_s(mdict(pm),"__path__",&pathv) && IS_LIST(pathv) && AS_LIST(pathv)->len)
            path=ppy_fs_find_module(mp_cstr(AS_LIST(pathv)->items[0]),dot+1);
    }
    if(!path){ char *dir=dir_of_globals(importer); path=ppy_fs_find_module(dir,full); free(dir); }
    if(!path) path=ppy_fs_find_module(mp_main_dir,full);
    if(!path){
        if(!leaf){ m=register_module(full); return m; }        /* a namespace package */
        Value nm=mp_str(full);
        Value e=mp_exc(E_ModuleNotFoundError,"No module named '%s'",full);
        ExcObj *x=AS_EXC(e); if(!x->dict) x->dict=AS_DICT(mp_dict()); mp_dict_set_s(x->dict,"name",nm);
        mp_raise(e);
    }
    char *err=NULL;
    char *src=ppy_fs_try_read_file(path,&err);
    if(!src){ free(err); Value e=mp_exc(E_ImportError,"cannot read %s",path); free(path); mp_raise(e); }
    size_t srclen=ppy_fs_last_len;
    m=register_module(full);
    mp_dict_set_s(mdict(m),"__file__",mp_str(path));
    if(is_package_file(path)){
        char *d=ppy_fs_dirname(path); Value pl=mp_list(0,NULL); mp_list_append(pl,mp_str(d)); free(d);
        mp_dict_set_s(mdict(m),"__path__",pl);
        mp_dict_set_s(mdict(m),"__package__",mp_str(full));
    } else {
        char pk[256]; snprintf(pk,sizeof pk,"%.*s",dot?(int)(dot-full):0,full);
        mp_dict_set_s(mdict(m),"__package__",mp_str(pk));
    }
    Catch c;
    if(!CATCH_BEGIN(c)){ if(is_bytecode_file(path)) exec_code_into(m,mp_bytecode_load(src,srclen,NULL,0)); else exec_into(m,src,path); CATCH_END(c); }
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
    char *src=ppy_fs_try_read_file(path,&err);
    if(!src){ fprintf(stderr,"plainpy: can't open file '%s': %s\n",path,err?err:"no such file"); free(err); exit(2); }
    Value m=register_module("__main__");
    mp_dict_set_s(mdict(m),"__file__",mp_str(path));
    mp_dict_set_s(mdict(m),"__package__",v_none());
    { Value bn=mp_str("builtins"); mp_dict_set_s(mdict(m),"__builtins__",mp_builtin_import(1,&bn,NULL)); }
    if(mp_is_bytecode(src,ppy_fs_last_len)) exec_code_into(m,mp_bytecode_load(src,ppy_fs_last_len,NULL,0));   /* (plainpy name.ppyc) */
    else exec_into(m,src,path);
    free(src);
    return m;
}

/* ---------------------------------------------------------------- writing bytecode (plainpy --bytecode) */
static Value compile_for_bytecode(const char *src, const char *file, int strip){
    CodeObj *co=compile_source_at(src,file,"__main__",0,strip|MP_STRIP_PLACES);
    Value keep=v_obj(co); (void)keep;
    int strip0=mp_strip; mp_strip|=strip|MP_STRIP_PLACES;
    Value b=mp_bytecode_dump(co,file);
    mp_strip=strip0;
    return b;
}
/* file.py -> out (NULL: file.ppyc next to it); strip: MP_STRIP_DOCS / MP_STRIP_LINES. Raises on errors. */
void mp_write_bytecode(const char *path, const char *out, int strip){
    char *err=NULL, *src=ppy_fs_try_read_file(path,&err);
    if(!src){ Value e=mp_exc(E_OSError,"cannot read %s: %s",path,err?err:"no such file"); free(err); mp_raise(e); }
    Value b=compile_for_bytecode(src,path,strip);
    free(src);
    char *o;
    if(out) o=xstrdup2(out);
    else { size_t n=strlen(path); o=(char*)xmalloc(n+8); memcpy(o,path,n+1); if(n>3 && !strcmp(o+n-3,".py")) o[n-3]=0; strcat(o,".ppyc"); }
    if(ppy_fs_write_file(o,(const char*)((BytesObj*)b.u.o)->s,(size_t)((BytesObj*)b.u.o)->len,&err)){
        Value e=mp_exc(E_OSError,"cannot write %s: %s",o,err?err:"?"); free(err); free(o); mp_raise(e); }
    free(o);
}
/* plainpy.lib -> out: its modules (this program's) as bytecode, no sources (plainpy --bytecode-lib) */
int mp_write_bytecode_lib(const char *out, int strip){
    int n=ppy_stdlib_count(), k=0;
    PpyLibModule *m=(PpyLibModule*)xmalloc(sizeof(PpyLibModule)*(size_t)(n+1));
    Value keep=mp_list(0,NULL);
    for(int i=0;i<n;i++){
        int pkg=0; char *name=ppy_stdlib_module(i,&pkg);    /* (kept until the library is written) */
        if(!name) continue;
        char *src=ppy_stdlib_source(name,NULL);
        char file[200]; snprintf(file,sizeof file,"<plainpy:%s>",name);
        Value b=compile_for_bytecode(src,file,strip);
        free(src);
        mp_list_append(keep,b);
        m[k].name=name; m[k].package=pkg; m[k].data=((BytesObj*)b.u.o)->s; m[k].len=(size_t)((BytesObj*)b.u.o)->len; k++;
    }
    char *err=NULL;
    int rc=ppy_lib_write_bytecode(out,m,k,&err);
    if(rc){ Value e=mp_exc(E_OSError,"cannot write %s: %s",out,err?err:"?"); free(err); free(m); mp_raise(e); }
    free(m);
    return k;
}

