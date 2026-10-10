/* ========================= Interpreter: modules =========================
   Importing (the importer's folder, the main script's, MINIPYPATH, <minipy>/lib;
   packages, relative imports, a module in sys.modules before its body runs),
   the modules written in C (sys, math, time, random, json, thread, _ctypes)
   and those written in Python, compiled from the text below when first
   imported (asyncio with a real event loop, minipy.endpoint, functools,
   typing, abc, collections.abc, dataclasses). random is xorshift32, the same numbers as
   compiled programs get. */

#include "interp.h"
#include "pystdlib.h"
#include "fs.h"
#include <math.h>
#include <time.h>
#if !defined(MPY_KOLIBRI)
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#endif

const char *mp_main_dir=".";
void mp_remember_source(const char *file, const char *src);
Value mp_std_stream(int err);
void mp_repr_into(SBuf *b, Value v);
int mp_parse_int(const char *s, int64_t n, int base, int64_t *out);
int mp_parse_float(const char *s, int64_t n, double *out);
static Value argv_list;

static int npos(int argc, TupleObj *kw){ return argc-(kw?(int)kw->len:0); }
static Value kwarg(int argc, Value *argv, TupleObj *kw, const char *name, Value dflt){
    if(!kw) return dflt;
    int np=npos(argc,kw);
    for(int64_t i=0;i<kw->len;i++) if(!strcmp(mp_cstr(kw->items[i]),name)) return argv[np+i];
    return dflt;
}
static void no_kw(const char *fn, TupleObj *kw){ if(kw && kw->len) mp_raise_t(E_TypeError,"%s() takes no keyword arguments",fn); }
static void nargs(const char *fn, int n, int lo, int hi){
    if(n<lo || n>hi){ if(lo==hi) mp_raise_t(E_TypeError,"%s() takes exactly %d argument%s (%d given)",fn,lo,lo==1?"":"s",n);
        mp_raise_t(E_TypeError,"%s() takes from %d to %d arguments (%d given)",fn,lo,hi,n); }
}

/* ---------------------------------------------------------------- module objects */
Value mp_new_module(const char *name){
    ModuleObj *m=(ModuleObj*)mp_alloc(T_module,sizeof(ModuleObj));
    m->name=AS_STR(mp_intern(name)); m->dict=AS_DICT(mp_dict());
    mp_dict_set_s(m->dict,"__name__",v_obj(m->name));
    mp_dict_set_s(m->dict,"__doc__",v_none());
    return v_obj(m);
}
static DictObj *mdict(Value m){ return ((ModuleObj*)m.u.o)->dict; }
static void set_fn(Value m, const char *name, NFn fn){ mp_dict_set_s(mdict(m),name,mp_native(name,fn)); }
static Value register_module(const char *name){ Value m=mp_new_module(name); mp_dict_set_s(mp_modules,name,m); return m; }

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
Value mp_builtin_import(int argc, Value *argv, TupleObj *kw);
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
Value mp_builtin_import(int argc, Value *argv, TupleObj *kw);
static void exec_into(Value mod, const char *src, const char *file){
    mp_remember_source(file,src);
    CodeObj *co=compile_source(src,file,mp_cstr(v_obj(((ModuleObj*)mod.u.o)->name)));
    Value keep=v_obj(co); (void)keep;
    if(!mp_dict_get_s(mdict(mod),"__builtins__",NULL)){ Value bn=mp_str("builtins"); mp_dict_set_s(mdict(mod),"__builtins__",mp_builtin_import(1,&bn,NULL)); }
    mp_run_code(co,mdict(mod),NULL);
}

/* ---------------------------------------------------------------- import */
static const char *py_source(const char *name);
static Value native_module(const char *name);
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
    char *full=resolve_name(name,g,level);
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
    int np=npos(argc,kw);
    if(np<1 || !IS_STR(argv[0])) mp_raise_t(E_TypeError,"__import__() argument 1 must be str");
    Value g= np>1 ? argv[1] : kwarg(argc,argv,kw,"globals",v_none());
    Value fl= np>3 ? argv[3] : kwarg(argc,argv,kw,"fromlist",v_none());
    Value lv= np>4 ? argv[4] : kwarg(argc,argv,kw,"level",v_int(0));
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

/* ---------------------------------------------------------------- sys */
static Value sys_exit(int argc, Value *argv, TupleObj *kw){
    no_kw("exit",kw); nargs("exit",argc,0,1);
    mp_raise(mp_exc_args(E_SystemExit,argc?mp_tuple(1,argv):mp_tuple(0,NULL)));
}
static Value sys_exc_info(int argc, Value *argv, TupleObj *kw){
    (void)argc; (void)argv; (void)kw;
    Thread *ts=mp_ts;
    if(!ts->nhandled){ Value n[3]={v_none(),v_none(),v_none()}; return mp_tuple(3,n); }
    Value e=ts->handled[ts->nhandled-1]; ExcObj *x=AS_EXC(e);
    Value r[3]={v_obj(TYPE(e)),e,x->tb.k==V_UNDEF?v_none():x->tb}; return mp_tuple(3,r);
}
static Value sys_exception(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; Thread *ts=mp_ts; return ts->nhandled ? ts->handled[ts->nhandled-1] : v_none(); }
static Value sys_getrecursionlimit(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; return v_int(1000); }
static Value sys_setrecursionlimit(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; return v_none(); }
static Value sys_intern(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("intern",argc,1,1); return mp_intern(mp_cstr(argv[0])); }
/* sys.audit(event, *args): each hook sys.addaudithook() added is called with (event, args) */
static Value audit_hooks;
static Value sys_addaudithook(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("addaudithook",argc,1,1);
    if(audit_hooks.k!=V_OBJ){ audit_hooks=mp_list(0,NULL); mp_gc_add_root(&audit_hooks); }
    mp_list_append(audit_hooks,argv[0]); return v_none(); }
static Value sys_audit(int argc, Value *argv, TupleObj *kw){ (void)kw;
    if(argc<1) mp_raise_t(E_TypeError,"audit() missing 1 required positional argument: 'event'");
    if(!IS_STR(argv[0])) mp_raise_t(E_TypeError,"expected str for argument 'event', not %s",mp_type_name(argv[0]));
    if(audit_hooks.k!=V_OBJ || !AS_LIST(audit_hooks)->len) return v_none();
    Value a[2]={argv[0],mp_tuple(argc-1,argv+1)};
    ListObj *h=AS_LIST(audit_hooks); for(int64_t i=0;i<h->len;i++) mp_call(h->items[i],2,a,NULL);
    return v_none(); }
/* sys._getframemodulename(depth=0): the __name__ of the module of the function depth calls up */
static Value sys_getframemodulename(int argc, Value *argv, TupleObj *kw){
    int64_t depth=0;
    for(int i=0;i<argc;i++) depth=mp_index(argv[i],"depth");
    (void)kw;
    Frame *f=mp_ts?mp_ts->frame:NULL;
    for(int64_t i=0;i<depth && f;i++) f=f->back;
    Value n;
    if(f && f->globals && mp_dict_get_s(f->globals,"__name__",&n)) return n;
    return v_none();
}
/* syscalls and raw memory: the platform's gateway. KolibriOS: int 0x40. Elsewhere
   the i386 Linux system calls (int 0x80) a compiled program for Linux makes, done
   with the host's calls as the macos target's programs do them: the same numbers,
   flags and results (-errno) on every host. */
#if !defined(MPY_KOLIBRI)
#define LX_WORD intptr_t
#define LX_PTR(a) ((void*)(a))
#define LX_BLOCKING_BEGIN Thread *lx_self_=mp_ts; mp_gil_release()
#define LX_BLOCKING_END mp_gil_acquire(lx_self_)
#define LX_FLUSH() mp_flush_stdout()
#include "lx_emul.c"
#endif
static Value sys_syscall(int argc, Value *argv, TupleObj *kw){
    no_kw("syscall",kw);
    if(argc<1||argc>7) mp_raise_t(E_TypeError,"syscall() takes 1 to 7 register arguments (eax..edi, ebp)");
    intptr_t in[7]={0};
    for(int i=0;i<argc;i++){
        Value a=argv[i];
        if(IS_INTLIKE(a)) in[i]=(intptr_t)a.u.i;
        else if(IS_STR(a)) in[i]=(intptr_t)AS_STR(a)->s;
        else if(IS_BYTES(a)) in[i]=(intptr_t)AS_BYTES(a)->s;
        else if(IS(a,T_buffer)) in[i]=(intptr_t)((BufferObj*)a.u.o)->data;
        else if(IS_BA(a)) in[i]=(intptr_t)((ByteArrayObj*)a.u.o)->data;
        else if(IS_NONE(a)) in[i]=0;
        else mp_raise_t(E_TypeError,"syscall() arguments must be int, str, buffer or None");
    }
    Value l=mp_list(6,NULL);
#if defined(MPY_KOLIBRI)
    uint32_t in32[7], out[6]={0};
    for(int i=0;i<7;i++) in32[i]=(uint32_t)in[i];
    mpy_platform_syscall(in32,out);
    for(int i=0;i<6;i++) AS_LIST(l)->items[i]=v_int((int64_t)(int32_t)out[i]);   /* the registers, as compiled code reads them */
#else
    AS_LIST(l)->items[0]=v_int(lx_syscall(in));
    for(int i=1;i<6;i++) AS_LIST(l)->items[i]=v_int((int64_t)(int32_t)in[i]);
#endif
    return l;
}
static BufferObj *buf_arg(Value v, const char *who){ if(!IS(v,T_buffer)) mp_raise_t(E_TypeError,"%s expects a buffer",who); return (BufferObj*)v.u.o; }
static Value sys_buffer(int argc, Value *argv, TupleObj *kw){
    no_kw("buffer",kw); nargs("buffer",argc,1,1);
    Value a=argv[0]; BufferObj *b=(BufferObj*)mp_alloc(T_buffer,sizeof(BufferObj));
    if(IS_INTLIKE(a)){ if(a.u.i<0||a.u.i>(1<<26)) mp_raise_t(E_ValueError,"buffer() size out of range"); b->len=a.u.i; b->data=(unsigned char*)xmalloc((size_t)b->len+1); memset(b->data,0,(size_t)b->len+1); return v_obj(b); }
    if(IS_STR(a)){ b->len=AS_STR(a)->len; b->data=(unsigned char*)xmalloc((size_t)b->len+1); memcpy(b->data,AS_STR(a)->s,(size_t)b->len+1); return v_obj(b); }
    if(IS_BYTES(a)){ b->len=AS_BYTES(a)->len; b->data=(unsigned char*)xmalloc((size_t)b->len+1); memcpy(b->data,AS_BYTES(a)->s,(size_t)b->len+1); return v_obj(b); }
    mp_raise_t(E_TypeError,"buffer() expects an int size or a str");
}
static int nbytes(Value v, const char *who){ int64_t n=mp_index(v,who); if(n!=1&&n!=2&&n!=4) mp_raise_t(E_ValueError,"%s nbytes must be 1, 2, or 4",who); return (int)n; }
static Value sys_poke(int argc, Value *argv, TupleObj *kw){ no_kw("poke",kw); nargs("poke",argc,4,4);
    BufferObj *b=buf_arg(argv[0],"poke()"); int64_t off=mp_index(argv[1],"poke"); uint32_t val=(uint32_t)mp_index(argv[2],"poke"); int nb=nbytes(argv[3],"poke()");
    if(off<0||off+nb>b->len) mp_raise_t(E_IndexError,"buffer offset out of range");
    for(int i=0;i<nb;i++) b->data[off+i]=(unsigned char)((val>>(8*i))&0xFF);
    return v_none(); }
static Value sys_peek(int argc, Value *argv, TupleObj *kw){ no_kw("peek",kw); nargs("peek",argc,3,3);
    BufferObj *b=buf_arg(argv[0],"peek()"); int64_t off=mp_index(argv[1],"peek"); int nb=nbytes(argv[2],"peek()");
    if(off<0||off+nb>b->len) mp_raise_t(E_IndexError,"buffer offset out of range");
    uint32_t v=0; for(int i=0;i<nb;i++) v|=((uint32_t)b->data[off+i])<<(8*i);
    return v_int((int64_t)v); }
static Value sys_poke_str(int argc, Value *argv, TupleObj *kw){ no_kw("poke_str",kw); nargs("poke_str",argc,3,3);
    BufferObj *b=buf_arg(argv[0],"poke_str()"); int64_t off=mp_index(argv[1],"poke_str");
    if(!IS_STR(argv[2])) mp_raise_t(E_TypeError,"poke_str() third argument must be a str");
    StrObj *s=AS_STR(argv[2]);
    if(off<0||off+s->len>b->len) mp_raise_t(E_IndexError,"buffer offset out of range");
    memcpy(b->data+off,s->s,(size_t)s->len); return v_none(); }
static Value sys_peek_str(int argc, Value *argv, TupleObj *kw){ no_kw("peek_str",kw); nargs("peek_str",argc,3,3);
    BufferObj *b=buf_arg(argv[0],"peek_str()"); int64_t off=mp_index(argv[1],"peek_str"), n=mp_index(argv[2],"peek_str");
    if(off<0||n<0||off+n>b->len) mp_raise_t(E_IndexError,"peek_str() range out of bounds");
    return mp_strn((const char*)b->data+off,n); }
static Value sys_peek_bytes(int argc, Value *argv, TupleObj *kw){ no_kw("peek_bytes",kw); nargs("peek_bytes",argc,3,3);
    BufferObj *b=buf_arg(argv[0],"peek_bytes()"); int64_t off=mp_index(argv[1],"peek_bytes"), n=mp_index(argv[2],"peek_bytes");
    if(off<0||n<0||off+n>b->len) mp_raise_t(E_IndexError,"buffer offset out of range");
    return mp_bytes((const char*)b->data+off,n); }
static Value sys_poke_bytes(int argc, Value *argv, TupleObj *kw){ no_kw("poke_bytes",kw); nargs("poke_bytes",argc,3,3);
    BufferObj *b=buf_arg(argv[0],"poke_bytes()"); int64_t off=mp_index(argv[1],"poke_bytes");
    if(!IS_BYTES(argv[2])) mp_raise_t(E_TypeError,"poke_bytes() third argument must be bytes");
    int64_t n=AS_BYTES(argv[2])->len;
    if(off<0||off+n>b->len) mp_raise_t(E_IndexError,"buffer offset out of range");
    memcpy(b->data+off,AS_BYTES(argv[2])->s,(size_t)n); return v_none(); }
#if !defined(MPY_KOLIBRI)
extern char **environ;
#endif
/* sys._rawargs(0): the arguments; (1): the environment as "NAME=value" strings (os.environ) */
static Value ti_sleep(int argc, Value *argv, TupleObj *kw);
static Value sys_rawargs(int argc, Value *argv, TupleObj *kw){ no_kw("_rawargs",kw); nargs("_rawargs",argc,1,1);
    Value l=mp_list(0,NULL);
    if(mp_index(argv[0],"_rawargs")==0){ if(argv_list.k==V_OBJ) for(int64_t i=0;i<AS_LIST(argv_list)->len;i++) mp_list_append(l,AS_LIST(argv_list)->items[i]); return l; }
#if !defined(MPY_KOLIBRI)
    for(char **e=environ;e && *e;e++) mp_list_append(l,mp_str(*e));
#endif
    return l; }
static Value sys_addr(int argc, Value *argv, TupleObj *kw){ no_kw("addr",kw); nargs("addr",argc,1,1); return v_int((int64_t)(uintptr_t)buf_arg(argv[0],"addr()")->data); }
static unsigned char *addr_of(Value v){ return (unsigned char*)(uintptr_t)(uint64_t)mp_index(v,"address"); }
static Value sys_peek_at(int argc, Value *argv, TupleObj *kw){ no_kw("peek_at",kw); nargs("peek_at",argc,2,2);
    unsigned char *p=addr_of(argv[0]); int nb=nbytes(argv[1],"peek_at()"); uint32_t v=0; for(int i=0;i<nb;i++) v|=((uint32_t)p[i])<<(8*i); return v_int((int64_t)v); }
static Value sys_poke_at(int argc, Value *argv, TupleObj *kw){ no_kw("poke_at",kw); nargs("poke_at",argc,3,3);
    unsigned char *p=addr_of(argv[0]); uint32_t val=(uint32_t)mp_index(argv[1],"poke_at"); int nb=nbytes(argv[2],"poke_at()");
    for(int i=0;i<nb;i++) p[i]=(unsigned char)((val>>(8*i))&0xFF); return v_none(); }
static Value sys_peek_str_at(int argc, Value *argv, TupleObj *kw){ no_kw("peek_str_at",kw); nargs("peek_str_at",argc,2,2);
    int64_t n=mp_index(argv[1],"peek_str_at"); if(n<0) mp_raise_t(E_ValueError,"peek_str_at() length must not be negative"); return mp_strn((const char*)addr_of(argv[0]),n); }
static Value sys_poke_str_at(int argc, Value *argv, TupleObj *kw){ no_kw("poke_str_at",kw); nargs("poke_str_at",argc,2,2);
    if(!IS_STR(argv[1])) mp_raise_t(E_TypeError,"poke_str_at() second argument must be a str");
    memcpy(addr_of(argv[0]),AS_STR(argv[1])->s,(size_t)AS_STR(argv[1])->len); return v_none(); }
static Value sys_cstr_at(int argc, Value *argv, TupleObj *kw){ no_kw("cstr_at",kw); nargs("cstr_at",argc,2,2);
    const char *p=(const char*)addr_of(argv[0]); int64_t max=mp_index(argv[1],"cstr_at"), n=0; while(n<max && p[n]) n++; return mp_strn(p,n); }
void mp_set_argv(int argc, char **argv){
    argv_list=mp_list(0,NULL); mp_gc_add_root(&argv_list);
    for(int i=0;i<argc;i++) mp_list_append(argv_list,mp_str(argv[i]));
}
static Value sys_format_exception(int argc, Value *argv, TupleObj *kw){ no_kw("_format_exception",kw); nargs("_format_exception",argc,1,1); return mp_format_exception(argv[0]); }
static Value sys_excepthook(int argc, Value *argv, TupleObj *kw){ no_kw("excepthook",kw); nargs("excepthook",argc,3,3); mp_print_exception(argv[1]); return v_none(); }
static Value sys_stack(int argc, Value *argv, TupleObj *kw){ (void)argv; no_kw("_stack",kw); nargs("_stack",argc,0,0); return mp_stack_list(); }
static Value sys_getline(int argc, Value *argv, TupleObj *kw){ no_kw("_getline",kw); nargs("_getline",argc,2,2);
    if(!IS_STR(argv[0])) return mp_str(""); return mp_source_getline(mp_cstr(argv[0]),(int)mp_index(argv[1],"_getline")); }
/* sys._getframe(depth): a snapshot of the running frames (f_code, f_lineno, f_back, f_globals, f_locals ...) */
static Type *T_frame;
static Value sys_getframe(int argc, Value *argv, TupleObj *kw){
    no_kw("_getframe",kw); nargs("_getframe",argc,0,1);
    int64_t depth= argc ? mp_index(argv[0],"_getframe") : 0;
    Frame *f=mp_ts->frame;
    for(int64_t i=0;i<depth && f;i++) f=f->back;
    if(!f || depth<0) mp_raise_t(E_ValueError,"call stack is not deep enough");
    if(!T_frame) T_frame=AS_TYPE(mp_new_type("frame",T_object,LY_INSTANCE,0));
    int n=0; for(Frame *g=f;g;g=g->back) n++;
    Frame **fs=(Frame**)xmalloc(sizeof(Frame*)*(size_t)n); n=0; for(Frame *g=f;g;g=g->back) fs[n++]=g;
    Value back=v_none();
    for(int i=n-1;i>=0;i--){ Frame *g=fs[i];
        InstObj *o=(InstObj*)mp_alloc(T_frame,sizeof(InstObj)); o->dict=AS_DICT(mp_dict()); Value fo=v_obj(o);
        int ip= g->ip>0 ? g->ip-1 : 0;
        int line= g->code->ncode ? g->code->lines[ip<g->code->ncode?ip:g->code->ncode-1] : g->code->firstline;
        mp_dict_set_s(o->dict,"f_code",v_obj(g->code)); mp_dict_set_s(o->dict,"f_lineno",v_int(line)); mp_dict_set_s(o->dict,"f_lasti",v_int(ip*2));
        mp_dict_set_s(o->dict,"f_globals",v_obj(g->globals)); mp_dict_set_s(o->dict,"f_builtins",v_obj(g->builtins?g->builtins:mp_builtins));
        mp_dict_set_s(o->dict,"f_locals",mp_frame_locals(g)); mp_dict_set_s(o->dict,"f_back",back); mp_dict_set_s(o->dict,"f_trace",v_none());
        mp_dict_set_s(o->dict,"f_generator",v_none());
        back=fo; }
    free(fs);
    return back;
}
static Value sys_suggestion(int argc, Value *argv, TupleObj *kw){ no_kw("_suggestion",kw); nargs("_suggestion",argc,2,2);
    if(!mp_isinstance(argv[0],E_BaseException)) return mp_str(""); return mp_exc_suggestion(argv[0],mp_truth(argv[1])); }
/* sys._builtin(f): f stands for a C function of CPython's (not bound as a method; its repr) */
static Value sys_builtin(int argc, Value *argv, TupleObj *kw){ no_kw("_builtin",kw); nargs("_builtin",argc,1,1);
    if(IS(argv[0],T_function)) ((FuncObj*)argv[0].u.o)->builtin=1;
    return argv[0]; }
static Value sys_frame_source(int argc, Value *argv, TupleObj *kw){ no_kw("_frame_source",kw); nargs("_frame_source",argc,4,4);
    if(!IS_STR(argv[0])) return mp_str(""); return mp_frame_source(mp_cstr(argv[0]),(int)mp_index(argv[1],"line"),argv[2],(int)mp_index(argv[3],"ip")); }
static Value sys_eval_annotation(int argc, Value *argv, TupleObj *kw){ no_kw("_eval_annotation",kw); nargs("_eval_annotation",argc,2,3);
    if(!IS_STR(argv[0]) || !IS_DICT(argv[1])) mp_raise_t(E_TypeError,"_eval_annotation(text, globals[, locals])");
    return mp_eval_annotation(mp_cstr(argv[0]),AS_DICT(argv[1]),argc>2&&IS_DICT(argv[2])?AS_DICT(argv[2]):NULL); }
static Value sys_getdefaultencoding(int argc, Value *argv, TupleObj *kw){ (void)argv; (void)argc; (void)kw; return mp_str("utf-8"); }
static Value make_sys(void){
    Value m=register_module("sys");
#if defined(__APPLE__)
    const char *os="darwin";
#elif defined(__linux__)
    const char *os="linux";
#elif defined(_WIN32)
    const char *os="win32";
#else
    const char *os="host";
#endif
    mp_dict_set_s(mdict(m),"platform",mp_str(mpy_platform_has_syscall()?"kolibrios":os));
    mp_dict_set_s(mdict(m),"argv",argv_list.k==V_OBJ?argv_list:mp_list(0,NULL));
    mp_dict_set_s(mdict(m),"modules",v_obj(mp_modules));
    mp_dict_set_s(mdict(m),"path",mp_list(0,NULL));
    mp_dict_set_s(mdict(m),"warnoptions",mp_list(0,NULL));
    mp_dict_set_s(mdict(m),"maxsize",v_int(INT64_MAX));
    mp_dict_set_s(mdict(m),"byteorder",mp_str("little"));
    mp_dict_set_s(mdict(m),"_compiled",v_bool(0));          /* (compiled programs: True, folded by the compiler) */
    mp_dict_set_s(mdict(m),"version",mp_str("3.14.0 (minipy)"));
    { Value vi[5]={v_int(3),v_int(14),v_int(0),mp_str("final"),v_int(0)}; mp_dict_set_s(mdict(m),"version_info",mp_tuple(5,vi)); }
    mp_dict_set_s(mdict(m),"stdout",mp_std_stream(0)); mp_dict_set_s(mdict(m),"stderr",mp_std_stream(1));
    mp_dict_set_s(mdict(m),"__stdout__",mp_std_stream(0)); mp_dict_set_s(mdict(m),"__stderr__",mp_std_stream(1));
    set_fn(m,"exit",sys_exit); set_fn(m,"exc_info",sys_exc_info); set_fn(m,"exception",sys_exception);
    set_fn(m,"getrecursionlimit",sys_getrecursionlimit); set_fn(m,"setrecursionlimit",sys_setrecursionlimit); set_fn(m,"intern",sys_intern); set_fn(m,"_getframemodulename",sys_getframemodulename);
    set_fn(m,"audit",sys_audit); set_fn(m,"addaudithook",sys_addaudithook);
    set_fn(m,"syscall",sys_syscall); set_fn(m,"buffer",sys_buffer); set_fn(m,"poke",sys_poke); set_fn(m,"peek",sys_peek);
    set_fn(m,"poke_str",sys_poke_str); set_fn(m,"peek_str",sys_peek_str); set_fn(m,"peek_bytes",sys_peek_bytes); set_fn(m,"poke_bytes",sys_poke_bytes); set_fn(m,"_rawargs",sys_rawargs); set_fn(m,"_sleep",ti_sleep); set_fn(m,"addr",sys_addr); set_fn(m,"peek_at",sys_peek_at);
    set_fn(m,"poke_at",sys_poke_at); set_fn(m,"peek_str_at",sys_peek_str_at); set_fn(m,"poke_str_at",sys_poke_str_at); set_fn(m,"cstr_at",sys_cstr_at);
    set_fn(m,"_format_exception",sys_format_exception); set_fn(m,"excepthook",sys_excepthook); set_fn(m,"_stack",sys_stack); set_fn(m,"_getline",sys_getline); set_fn(m,"_suggestion",sys_suggestion); set_fn(m,"_frame_source",sys_frame_source); set_fn(m,"_eval_annotation",sys_eval_annotation); set_fn(m,"_getframe",sys_getframe); set_fn(m,"_builtin",sys_builtin);
    mp_dict_set_s(mdict(m),"__excepthook__",mp_dict_get_s(mdict(m),"excepthook",NULL)?mp_native("excepthook",sys_excepthook):v_none());
    set_fn(m,"getdefaultencoding",sys_getdefaultencoding); set_fn(m,"getfilesystemencoding",sys_getdefaultencoding);
    mp_dict_set_s(mdict(m),"maxunicode",v_int(0x10FFFF)); mp_dict_set_s(mdict(m),"hexversion",v_int(0x030E00F0));
    mp_dict_set_s(mdict(m),"executable",mp_str(mpy_platform_exe_path()?mpy_platform_exe_path():"minipy"));
    return m;
}

/* ---------------------------------------------------------------- math */
static double farg(Value v){ return mp_float_of(v); }
#define M1(name,expr) static Value ma_##name(int argc, Value *argv, TupleObj *kw){ no_kw(#name,kw); nargs(#name,argc,1,1); double x=farg(argv[0]); (void)x; return v_float(expr); }
static void domain(int bad){ if(bad) mp_raise_t(E_ValueError,"math domain error"); }
static Value ma_sqrt(int argc, Value *argv, TupleObj *kw){ no_kw("sqrt",kw); nargs("sqrt",argc,1,1); double x=farg(argv[0]); domain(x<0); return v_float(sqrt(x)); }
static Value ma_log(int argc, Value *argv, TupleObj *kw){ no_kw("log",kw); nargs("log",argc,1,2); double x=farg(argv[0]); domain(x<=0);
    if(argc==2){ double b=farg(argv[1]); domain(b<=0||b==1); return v_float(log(x)/log(b)); } return v_float(log(x)); }
static Value ma_log10(int argc, Value *argv, TupleObj *kw){ no_kw("log10",kw); nargs("log10",argc,1,1); double x=farg(argv[0]); domain(x<=0); return v_float(log10(x)); }
static Value ma_log2(int argc, Value *argv, TupleObj *kw){ no_kw("log2",kw); nargs("log2",argc,1,1); double x=farg(argv[0]); domain(x<=0); return v_float(log2(x)); }
static Value ma_log1p(int argc, Value *argv, TupleObj *kw){ no_kw("log1p",kw); nargs("log1p",argc,1,1); double x=farg(argv[0]); domain(x<=-1); return v_float(log1p(x)); }
static Value ma_asin(int argc, Value *argv, TupleObj *kw){ no_kw("asin",kw); nargs("asin",argc,1,1); double x=farg(argv[0]); domain(x<-1||x>1); return v_float(asin(x)); }
static Value ma_acos(int argc, Value *argv, TupleObj *kw){ no_kw("acos",kw); nargs("acos",argc,1,1); double x=farg(argv[0]); domain(x<-1||x>1); return v_float(acos(x)); }
static Value ma_exp(int argc, Value *argv, TupleObj *kw){ no_kw("exp",kw); nargs("exp",argc,1,1); double r=exp(farg(argv[0])); if(isinf(r) && isfinite(farg(argv[0]))) mp_raise_t(E_OverflowError,"math range error"); return v_float(r); }
M1(sin,sin(x)) M1(cos,cos(x)) M1(tan,tan(x)) M1(atan,atan(x)) M1(sinh,sinh(x)) M1(cosh,cosh(x)) M1(tanh,tanh(x))
M1(asinh,asinh(x)) M1(fabs,fabs(x)) M1(expm1,expm1(x)) M1(degrees,x*(180.0/M_PI)) M1(radians,x*(M_PI/180.0)) M1(erf,erf(x)) M1(erfc,erfc(x))
M1(cbrt,cbrt(x)) M1(exp2,exp2(x))
/* gamma and lgamma: CPython's own (Lanczos, N=13, g=6.0246...), not the C library's */
static const double ma_lanczos_g=6.024680040776729583740234375, ma_lanczos_gmh=5.524680040776729583740234375;
static const double ma_lanczos_num[13]={23531376880.410759688572007674451636754734846804940,42919803642.649098768957899047001988850926355848959,
    35711959237.355668049440185451547166705960488635843,17921034426.037209699919755754458931112671403265390,6039542586.3520280050642916443072979210699388420708,
    1439720407.3117216736632230727949123939715485786772,248874557.86205415651146038641322942321632125127801,31426415.585400194380614231628318205362874684987640,
    2876370.6289353724412254090516208496135991145378768,186056.26539522349504029498971604569928220784236328,8071.6720023658162106380029022722506138218516325024,
    210.82427775157934587250973392071336271166969580291,2.5066282746310002701649081771338373386264310793408};
static const double ma_lanczos_den[13]={0.0,39916800.0,120543840.0,150917976.0,105258076.0,45995730.0,13339535.0,2637558.0,357423.0,32670.0,1925.0,66.0,1.0};
static const double ma_gamma_int[23]={1.0,1.0,2.0,6.0,24.0,120.0,720.0,5040.0,40320.0,362880.0,3628800.0,39916800.0,479001600.0,6227020800.0,87178291200.0,
    1307674368000.0,20922789888000.0,355687428096000.0,6402373705728000.0,121645100408832000.0,2432902008176640000.0,51090942171709440000.0,1124000727777607680000.0};
static double ma_sinpi(double x){
    double y=fmod(fabs(x),2.0), r; int n=(int)round(2.0*y);
    switch(n){ case 0: r=sin(M_PI*y); break; case 1: r=cos(M_PI*(y-0.5)); break; case 2: r=sin(M_PI*(1.0-y)); break; case 3: r=-cos(M_PI*(y-1.5)); break; default: r=sin(M_PI*(y-2.0)); }
    return copysign(1.0,x)*r;
}
static double ma_lanczos_sum(double x){
    double num=0.0, den=0.0;
    if(x<5.0) for(int i=13;--i>=0;){ num=num*x+ma_lanczos_num[i]; den=den*x+ma_lanczos_den[i]; }
    else for(int i=0;i<13;i++){ num=num/x+ma_lanczos_num[i]; den=den/x+ma_lanczos_den[i]; }
    return num/den;
}
static Value ma_gamma(int argc, Value *argv, TupleObj *kw){ no_kw("gamma",kw); nargs("gamma",argc,1,1); double x=farg(argv[0]), absx, r, y, z, sqrtpow;
    if(!isfinite(x)){ if(isnan(x) || x>0.0) return v_float(x); mp_raise_t(E_ValueError,"math domain error"); }
    if(x==0.0 || (x==floor(x) && x<0.0)) mp_raise_t(E_ValueError,"expected a noninteger or positive integer, got %s",AS_STR(mp_repr(argv[0]))->s);
    if(x==floor(x) && x<=23) return v_float(ma_gamma_int[(int)x-1]);
    absx=fabs(x);
    if(absx<1e-20){ r=1.0/x; if(isinf(r)) mp_raise_t(E_OverflowError,"math range error"); return v_float(r); }
    if(absx>200.0){ if(x<0.0) return v_float(0.0/ma_sinpi(x)); mp_raise_t(E_OverflowError,"math range error"); }
    y=absx+ma_lanczos_gmh;
    if(absx>ma_lanczos_gmh){ volatile double q=y-absx; z=q-ma_lanczos_gmh; } else { volatile double q=y-ma_lanczos_gmh; z=q-absx; }
    z=z*ma_lanczos_g/y;
    if(x<0.0){ r=-M_PI/ma_sinpi(absx)/absx*exp(y)/ma_lanczos_sum(absx); r-=z*r;
        if(absx<140.0) r/=pow(y,absx-0.5); else { sqrtpow=pow(y,absx/2.0-0.25); r/=sqrtpow; r/=sqrtpow; } }
    else { r=ma_lanczos_sum(absx)/exp(y); r+=z*r;
        if(absx<140.0) r*=pow(y,absx-0.5); else { sqrtpow=pow(y,absx/2.0-0.25); r*=sqrtpow; r*=sqrtpow; } }
    if(isinf(r)) mp_raise_t(E_OverflowError,"math range error");
    return v_float(r);
}
static Value ma_lgamma(int argc, Value *argv, TupleObj *kw){ no_kw("lgamma",kw); nargs("lgamma",argc,1,1); double x=farg(argv[0]), r, absx;
    if(!isfinite(x)) return v_float(isnan(x)?x:INFINITY);
    if(x==floor(x) && x<=2.0){ if(x<=0.0) mp_raise_t(E_ValueError,"math domain error"); return v_float(0.0); }
    absx=fabs(x);
    if(absx<1e-20) return v_float(-log(absx));
    r=log(ma_lanczos_sum(absx))-ma_lanczos_g;
    r+=(absx-0.5)*(log(absx+ma_lanczos_g-0.5)-1);
    if(x<0.0) r=1.144729885849400174143427351353058711647-log(fabs(ma_sinpi(absx)))-log(absx)-r;
    if(isinf(r)) mp_raise_t(E_OverflowError,"math range error");
    return v_float(r);
}
static Value ma_nextafter(int argc, Value *argv, TupleObj *kw){
    int np=argc-(kw?(int)kw->len:0); double x=farg(argv[0]), y=farg(argv[1]);
    if(np<2) mp_raise_t(E_TypeError,"nextafter expected 2 arguments, got %d",np);
    if(kw && kw->len){ int64_t steps=mp_index(argv[np],"steps"); if(steps<0) mp_raise_t(E_ValueError,"steps must be a non-negative integer");
        for(int64_t i=0;i<steps && x!=y;i++) x=nextafter(x,y); return v_float(x); }
    return v_float(nextafter(x,y)); }
static Value ma_ulp(int argc, Value *argv, TupleObj *kw){ no_kw("ulp",kw); nargs("ulp",argc,1,1); double x=farg(argv[0]);
    if(isnan(x)) return v_float(x); x=fabs(x); if(isinf(x)) return v_float(x);
    double x2=nextafter(x,INFINITY); if(isinf(x2)){ x2=nextafter(x,-INFINITY); return v_float(x-x2); } return v_float(x2-x); }
static Value ma_fma(int argc, Value *argv, TupleObj *kw){ no_kw("fma",kw); nargs("fma",argc,3,3);
    double x=farg(argv[0]), y=farg(argv[1]), z=farg(argv[2]), r=fma(x,y,z);
    if(isfinite(r)) return v_float(r);
    if(isnan(r)){ if(!isnan(x) && !isnan(y) && !isnan(z)) mp_raise_t(E_ValueError,"invalid operation in fma"); return v_float(r); }
    if(isfinite(x) && isfinite(y) && isfinite(z)) mp_raise_t(E_OverflowError,"overflow in fma");
    return v_float(r); }
static Value ma_sumprod(int argc, Value *argv, TupleObj *kw){ no_kw("sumprod",kw); nargs("sumprod",argc,2,2);
    Value ip=mp_iter(argv[0]), iq=mp_iter(argv[1]), a, b, total=v_int(0);
    for(;;){ int ha=mp_next(ip,&a), hb=mp_next(iq,&b);
        if(ha!=hb) mp_raise_t(E_ValueError,"Inputs are not the same length");
        if(!ha) break;
        total=mp_binop(OP_Add,total,mp_binop(OP_Mult,a,b)); }
    return total; }
static Value ma_acosh(int argc, Value *argv, TupleObj *kw){ no_kw("acosh",kw); nargs("acosh",argc,1,1); double x=farg(argv[0]); domain(x<1); return v_float(acosh(x)); }
static Value ma_atanh(int argc, Value *argv, TupleObj *kw){ no_kw("atanh",kw); nargs("atanh",argc,1,1); double x=farg(argv[0]); domain(x<=-1||x>=1); return v_float(atanh(x)); }
static Value ma_atan2(int argc, Value *argv, TupleObj *kw){ no_kw("atan2",kw); nargs("atan2",argc,2,2); return v_float(atan2(farg(argv[0]),farg(argv[1]))); }
static Value ma_pow(int argc, Value *argv, TupleObj *kw){ no_kw("pow",kw); nargs("pow",argc,2,2); double x=farg(argv[0]), y=farg(argv[1]);
    domain(x==0 && y<0); domain(x<0 && y!=floor(y) && isfinite(y)); double r=pow(x,y); if(isinf(r) && isfinite(x) && isfinite(y)) mp_raise_t(E_OverflowError,"math range error"); return v_float(r); }
static Value ma_fmod(int argc, Value *argv, TupleObj *kw){ no_kw("fmod",kw); nargs("fmod",argc,2,2); double x=farg(argv[0]), y=farg(argv[1]); domain(y==0 && !isnan(x)); return v_float(fmod(x,y)); }
static Value ma_remainder(int argc, Value *argv, TupleObj *kw){ no_kw("remainder",kw); nargs("remainder",argc,2,2); double y=farg(argv[1]); domain(y==0); return v_float(remainder(farg(argv[0]),y)); }
static Value ma_copysign(int argc, Value *argv, TupleObj *kw){ no_kw("copysign",kw); nargs("copysign",argc,2,2); return v_float(copysign(farg(argv[0]),farg(argv[1]))); }
static Value ma_ldexp(int argc, Value *argv, TupleObj *kw){ no_kw("ldexp",kw); nargs("ldexp",argc,2,2); double x=farg(argv[0]); int64_t i=mp_index(argv[1],"ldexp");
    if(i>100000) i=100000; if(i<-100000) i=-100000;
    double r=ldexp(x,(int)i); if(isinf(r) && isfinite(x)) mp_raise_t(E_OverflowError,"math range error"); return v_float(r); }
static Value ma_frexp(int argc, Value *argv, TupleObj *kw){ no_kw("frexp",kw); nargs("frexp",argc,1,1); int e; double m=frexp(farg(argv[0]),&e); Value r[2]={v_float(m),v_int(e)}; return mp_tuple(2,r); }
static Value ma_modf(int argc, Value *argv, TupleObj *kw){ no_kw("modf",kw); nargs("modf",argc,1,1); double ip; double fp=modf(farg(argv[0]),&ip); Value r[2]={v_float(fp),v_float(ip)}; return mp_tuple(2,r); }
static Value ma_hypot(int argc, Value *argv, TupleObj *kw){ no_kw("hypot",kw); double s=0; double mx=0;
    for(int i=0;i<argc;i++){ double x=fabs(farg(argv[i])); if(isinf(x)) return v_float(INFINITY); if(x>mx) mx=x; }
    if(mx==0) return v_float(0);
    if(argc==2) return v_float(hypot(farg(argv[0]),farg(argv[1])));
    for(int i=0;i<argc;i++){ double x=farg(argv[i])/mx; s+=x*x; } return v_float(mx*sqrt(s)); }
static Value int_result(double r){
    if(isnan(r)) mp_raise_t(E_ValueError,"cannot convert float NaN to integer");
    if(isinf(r)) mp_raise_t(E_OverflowError,"cannot convert float infinity to integer");
    if(r>=9.2233720368547758e18 || r<-9.2233720368547758e18) mp_raise_t(E_OverflowError,"int too large (ints are 64-bit)");
    return v_int((int64_t)r);
}
static Value ma_floor(int argc, Value *argv, TupleObj *kw){ no_kw("floor",kw); nargs("floor",argc,1,1); if(IS_INTLIKE(argv[0])) return v_int(argv[0].u.i);
    if(TYPE(argv[0])->flags&TF_DUNDERS){ Value m=mp_type_lookup_s(TYPE(argv[0]),"__floor__"); if(m.k!=V_UNDEF) return mp_call1(m,argv[0]); } return int_result(floor(farg(argv[0]))); }
static Value ma_ceil(int argc, Value *argv, TupleObj *kw){ no_kw("ceil",kw); nargs("ceil",argc,1,1); if(IS_INTLIKE(argv[0])) return v_int(argv[0].u.i);
    if(TYPE(argv[0])->flags&TF_DUNDERS){ Value m=mp_type_lookup_s(TYPE(argv[0]),"__ceil__"); if(m.k!=V_UNDEF) return mp_call1(m,argv[0]); } return int_result(ceil(farg(argv[0]))); }
static Value ma_trunc(int argc, Value *argv, TupleObj *kw){ no_kw("trunc",kw); nargs("trunc",argc,1,1); if(IS_INTLIKE(argv[0])) return v_int(argv[0].u.i);
    if(TYPE(argv[0])->flags&TF_DUNDERS){ Value m=mp_type_lookup_s(TYPE(argv[0]),"__trunc__"); if(m.k!=V_UNDEF) return mp_call1(m,argv[0]); } return int_result(trunc(farg(argv[0]))); }
static int64_t gcd2(int64_t a, int64_t b){ if(a<0) a=-a; if(b<0) b=-b; while(b){ int64_t t=a%b; a=b; b=t; } return a; }
static Value ma_gcd(int argc, Value *argv, TupleObj *kw){ no_kw("gcd",kw); int64_t g=0; for(int i=0;i<argc;i++) g=gcd2(g,mp_index(argv[i],"gcd")); return v_int(g); }
static Value ma_lcm(int argc, Value *argv, TupleObj *kw){ no_kw("lcm",kw); int64_t l=1; if(!argc) return v_int(1);
    for(int i=0;i<argc;i++){ int64_t x=mp_index(argv[i],"lcm"); if(x==0) return v_int(0); if(x<0) x=-x; l=mp_int_checked('*',l/gcd2(l,x),x); } return v_int(l); }
static Value ma_isqrt(int argc, Value *argv, TupleObj *kw){ no_kw("isqrt",kw); nargs("isqrt",argc,1,1); int64_t n=mp_index(argv[0],"isqrt");
    if(n<0) mp_raise_t(E_ValueError,"isqrt() argument must be nonnegative");
    int64_t r=(int64_t)sqrt((double)n); while(r*r>n) r--; while((r+1)*(r+1)<=n) r++; return v_int(r); }
static Value ma_factorial(int argc, Value *argv, TupleObj *kw){ no_kw("factorial",kw); nargs("factorial",argc,1,1); int64_t n=mp_index(argv[0],"factorial");
    if(n<0) mp_raise_t(E_ValueError,"factorial() not defined for negative values"); int64_t r=1; for(int64_t i=2;i<=n;i++) r=mp_int_checked('*',r,i); return v_int(r); }
static Value ma_comb(int argc, Value *argv, TupleObj *kw){ no_kw("comb",kw); nargs("comb",argc,2,2); int64_t n=mp_index(argv[0],"comb"), k=mp_index(argv[1],"comb");
    if(n<0||k<0) mp_raise_t(E_ValueError,"n must be a non-negative integer"); if(k>n) return v_int(0); if(k>n-k) k=n-k;
    int64_t r=1; for(int64_t i=1;i<=k;i++){ r=mp_int_checked('*',r,n-k+i)/i; } return v_int(r); }
static Value ma_perm(int argc, Value *argv, TupleObj *kw){ no_kw("perm",kw); nargs("perm",argc,1,2); int64_t n=mp_index(argv[0],"perm"), k= argc>1 && !IS_NONE(argv[1]) ? mp_index(argv[1],"perm") : n;
    if(n<0||k<0) mp_raise_t(E_ValueError,"n must be a non-negative integer"); if(k>n) return v_int(0); int64_t r=1; for(int64_t i=0;i<k;i++) r=mp_int_checked('*',r,n-i); return v_int(r); }
static Value ma_prod(int argc, Value *argv, TupleObj *kw){ nargs("prod",npos(argc,kw),1,1); Value acc=kwarg(argc,argv,kw,"start",v_int(1)); Value it=mp_iter(argv[0]), x; while(mp_next(it,&x)) acc=mp_binop(OP_Mult,acc,x); return acc; }
static Value ma_fsum(int argc, Value *argv, TupleObj *kw){
    no_kw("fsum",kw); nargs("fsum",argc,1,1);
    /* Shewchuk's exact summation, as CPython's math.fsum */
    double p[64]; int n=0; double special=0; int inf_nan=0;
    Value it=mp_iter(argv[0]), v;
    while(mp_next(it,&v)){
        double x=farg(v);
        if(!isfinite(x)){ inf_nan=1; special+=x; continue; }
        int i=0;
        for(int j=0;j<n;j++){ double y=p[j]; if(fabs(x)<fabs(y)){ double t=x; x=y; y=t; } double hi=x+y, lo=y-(hi-x); if(lo!=0) p[i++]=lo; x=hi; }
        n=i; if(n<64) p[n++]=x;
    }
    if(inf_nan) return v_float(special);
    double hi=0;
    if(n>0){
        hi=p[--n];
        while(n>0){ double x=hi, y=p[--n]; hi=x+y; double yr=hi-x, lo=y-yr; if(lo!=0){
            if(n>0 && ((lo<0 && p[n-1]<0) || (lo>0 && p[n-1]>0))){ y=lo*2; x=hi+y; yr=x-hi; if(y==yr) hi=x; }
            break; } }
    }
    return v_float(hi);
}
static Value ma_isfinite(int argc, Value *argv, TupleObj *kw){ no_kw("isfinite",kw); nargs("isfinite",argc,1,1); return v_bool(isfinite(farg(argv[0]))); }
static Value ma_isinf(int argc, Value *argv, TupleObj *kw){ no_kw("isinf",kw); nargs("isinf",argc,1,1); return v_bool(isinf(farg(argv[0]))); }
static Value ma_isnan(int argc, Value *argv, TupleObj *kw){ no_kw("isnan",kw); nargs("isnan",argc,1,1); return v_bool(isnan(farg(argv[0]))); }
static Value ma_isclose(int argc, Value *argv, TupleObj *kw){
    nargs("isclose",npos(argc,kw),2,2);
    double a=farg(argv[0]), b=farg(argv[1]);
    double rt=farg(kwarg(argc,argv,kw,"rel_tol",v_float(1e-9))), at=farg(kwarg(argc,argv,kw,"abs_tol",v_float(0)));
    if(a==b) return v_bool(1);
    if(isinf(a)||isinf(b)) return v_bool(0);
    double d=fabs(b-a);
    return v_bool(d<=fabs(rt*b) || d<=fabs(rt*a) || d<=at);
}
static Value ma_dist(int argc, Value *argv, TupleObj *kw){ no_kw("dist",kw); nargs("dist",argc,2,2); Value p=mp_list_of(argv[0]), q=mp_list_of(argv[1]);
    if(AS_LIST(p)->len!=AS_LIST(q)->len) mp_raise_t(E_ValueError,"both points must have the same number of dimensions");
    double s=0; for(int64_t i=0;i<AS_LIST(p)->len;i++){ double d=farg(AS_LIST(p)->items[i])-farg(AS_LIST(q)->items[i]); s+=d*d; } return v_float(sqrt(s)); }
static Value make_math(void){
    Value m=register_module("math");
    mp_dict_set_s(mdict(m),"pi",v_float(M_PI)); mp_dict_set_s(mdict(m),"e",v_float(M_E)); mp_dict_set_s(mdict(m),"tau",v_float(2*M_PI));
    mp_dict_set_s(mdict(m),"inf",v_float(INFINITY)); mp_dict_set_s(mdict(m),"nan",v_float(NAN));
    struct { const char *n; NFn f; } fs[]={{"sqrt",ma_sqrt},{"log",ma_log},{"log10",ma_log10},{"log2",ma_log2},{"log1p",ma_log1p},{"exp",ma_exp},{"expm1",ma_expm1},
        {"sin",ma_sin},{"cos",ma_cos},{"tan",ma_tan},{"asin",ma_asin},{"acos",ma_acos},{"atan",ma_atan},{"atan2",ma_atan2},{"sinh",ma_sinh},{"cosh",ma_cosh},
        {"tanh",ma_tanh},{"asinh",ma_asinh},{"acosh",ma_acosh},{"atanh",ma_atanh},{"fabs",ma_fabs},{"pow",ma_pow},{"fmod",ma_fmod},{"remainder",ma_remainder},
        {"copysign",ma_copysign},{"ldexp",ma_ldexp},{"frexp",ma_frexp},{"modf",ma_modf},{"hypot",ma_hypot},{"floor",ma_floor},{"ceil",ma_ceil},{"trunc",ma_trunc},
        {"gcd",ma_gcd},{"lcm",ma_lcm},{"isqrt",ma_isqrt},{"factorial",ma_factorial},{"comb",ma_comb},{"perm",ma_perm},{"prod",ma_prod},{"fsum",ma_fsum},
        {"isfinite",ma_isfinite},{"isinf",ma_isinf},{"isnan",ma_isnan},{"isclose",ma_isclose},{"degrees",ma_degrees},{"radians",ma_radians},{"erf",ma_erf},
        {"erfc",ma_erfc},{"cbrt",ma_cbrt},{"exp2",ma_exp2},{"gamma",ma_gamma},{"lgamma",ma_lgamma},{"dist",ma_dist},
        {"nextafter",ma_nextafter},{"ulp",ma_ulp},{"fma",ma_fma},{"sumprod",ma_sumprod},{NULL,NULL}};
    for(int i=0;fs[i].n;i++) set_fn(m,fs[i].n,fs[i].f);
    return m;
}

/* ---------------------------------------------------------------- time */
static Value ti_sleep(int argc, Value *argv, TupleObj *kw){
    no_kw("sleep",kw); nargs("sleep",argc,1,1);
    double s=farg(argv[0]);
    if(s<0) mp_raise_t(E_ValueError,"sleep length must be non-negative");
    Thread *self=mp_ts; mp_flush_stdout(); mp_gil_release();
    mpy_thread_sleep_ms((int)(s*1000+0.5));
    mp_gil_acquire(self);
    return v_none();
}

/* ---------------------------------------------------------------- json */
typedef struct { const char *isep, *ksep; int ascii, sort; Value indent; int depth; } JOpt;
static void j_string(SBuf *b, const char *s, int64_t len, int ascii){
    sb_putc(b,'"');
    int64_t i=0;
    while(i<len){
        unsigned char ch=(unsigned char)s[i];
        if(ch=='"'){ sb_puts(b,"\\\""); i++; continue; }
        if(ch=='\\'){ sb_puts(b,"\\\\"); i++; continue; }
        if(ch<0x20){ const char *e= ch=='\n'?"\\n":ch=='\r'?"\\r":ch=='\t'?"\\t":ch=='\b'?"\\b":ch=='\f'?"\\f":NULL; if(e) sb_puts(b,e); else sb_printf(b,"\\u%04x",ch); i++; continue; }
        if(ch<0x7f || !ascii){ sb_putc(b,(char)ch); i++; continue; }      /* (ensure_ascii: DEL too, as \u007f) */
        int64_t p=i; uint32_t cp=(uint32_t)mp_utf8_decode(s,len,&p); i=p;
        if(cp>=0x10000){ cp-=0x10000; sb_printf(b,"\\u%04x\\u%04x",0xD800+(cp>>10),0xDC00+(cp&0x3FF)); }
        else sb_printf(b,"\\u%04x",cp);
    }
    sb_putc(b,'"');
}
static void j_newline(SBuf *b, JOpt *o, int depth){
    if(IS_NONE(o->indent)) return;
    sb_putc(b,'\n');
    for(int i=0;i<depth;i++){ if(IS_STR(o->indent)) sb_puts(b,mp_cstr(o->indent)); else for(int64_t k=0;k<o->indent.u.i;k++) sb_putc(b,' '); }
}
static void j_key(SBuf *b, Value k, JOpt *o){
    k=mp_unbox(k);
    if(IS_STR(k)){ j_string(b,AS_STR(k)->s,AS_STR(k)->len,o->ascii); return; }
    if(k.k==V_BOOL){ sb_puts(b,k.u.i?"\"true\"":"\"false\""); return; }
    if(k.k==V_NONE){ sb_puts(b,"\"null\""); return; }
    if(k.k==V_INT){ sb_printf(b,"\"%lld\"",(long long)k.u.i); return; }
    if(k.k==V_FLOAT){ char t[64]; mp_float_repr(t,sizeof t,k.u.f); sb_printf(b,"\"%s\"",t); return; }
    mp_raise_t(E_TypeError,"keys must be str, int, float, bool or None, not %s",mp_type_name(k));
}
static void j_write(SBuf *b, Value v, JOpt *o, int depth){
    if(depth>400) mp_raise_t(E_ValueError,"Circular reference detected");
    v=mp_unbox(v);                                         /* (an int/float subclass: its value) */
    switch(v.k){
        case V_NONE: sb_puts(b,"null"); return;
        case V_BOOL: sb_puts(b,v.u.i?"true":"false"); return;
        case V_INT: sb_printf(b,"%lld",(long long)v.u.i); return;
        case V_FLOAT:{ double f=v.u.f; char t[64]; if(isnan(f)) strcpy(t,"NaN"); else if(isinf(f)) strcpy(t,f<0?"-Infinity":"Infinity"); else mp_float_repr(t,sizeof t,f); sb_puts(b,t); return; }
        default: break;
    }
    if(IS_STR(v)){ j_string(b,AS_STR(v)->s,AS_STR(v)->len,o->ascii); return; }
    if(IS_LIST(v)||IS_TUPLE(v)){
        int64_t n= IS_LIST(v)?AS_LIST(v)->len:AS_TUPLE(v)->len;
        sb_putc(b,'[');
        if(n){
            for(int64_t i=0;i<n;i++){ if(i) sb_puts(b,o->isep); j_newline(b,o,depth+1); j_write(b,IS_LIST(v)?AS_LIST(v)->items[i]:AS_TUPLE(v)->items[i],o,depth+1); }
            j_newline(b,o,depth);
        }
        sb_putc(b,']'); return;
    }
    DictObj *d=NULL; int inst=0;
    if(v.k==V_OBJ && v.u.o->type->layout==LY_DICT) d=AS_DICT(v);
    else if(v.k==V_OBJ && v.u.o->type->layout==LY_INSTANCE && !(mp_type_lookup_s(TYPE(v),"__json__").k!=V_UNDEF)){ d=AS_INST(v)->dict; inst=1; }   /* an object: its fields (as compiled programs) */
    if(d){
        Value keys=mp_list(0,NULL); int64_t pos=0; Value k, val;
        while(mp_dict_next(d,&pos,&k,&val)){ if(inst && IS_STR(k) && mp_cstr(k)[0]=='_') continue; mp_list_append(keys,k); }
        if(o->sort) mp_sort(keys,v_none(),0);
        sb_putc(b,'{');
        ListObj *kl=AS_LIST(keys);
        for(int64_t i=0;i<kl->len;i++){
            if(i) sb_puts(b,o->isep);
            j_newline(b,o,depth+1);
            j_key(b,kl->items[i],o); sb_puts(b,o->ksep);
            Value x; mp_dict_get(d,kl->items[i],&x);
            j_write(b,x,o,depth+1);
        }
        if(kl->len) j_newline(b,o,depth);
        sb_putc(b,'}'); return;
    }
    mp_raise_t(E_TypeError,"Object of type %s is not JSON serializable",mp_type_name(v));
}
static Value js_dumps(int argc, Value *argv, TupleObj *kw){
    int np=npos(argc,kw);
    if(np!=1) mp_raise_t(E_TypeError,"dumps() takes 1 positional argument but %d were given",np);
    JOpt o; o.indent=kwarg(argc,argv,kw,"indent",v_none()); o.ascii=mp_truth(kwarg(argc,argv,kw,"ensure_ascii",v_bool(1)));
    o.sort=mp_truth(kwarg(argc,argv,kw,"sort_keys",v_bool(0)));
    Value seps=kwarg(argc,argv,kw,"separators",v_none());
    o.isep= IS_NONE(o.indent) ? ", " : ","; o.ksep=": ";
    if(IS_INTLIKE(o.indent) && o.indent.u.i<0) o.indent=v_int(0);
    if(!IS_NONE(seps)){
        if(!IS_TUPLE(seps) || AS_TUPLE(seps)->len!=2 || !IS_STR(AS_TUPLE(seps)->items[0]) || !IS_STR(AS_TUPLE(seps)->items[1])) mp_raise_t(E_TypeError,"separators must be a tuple of two strings");
        o.isep=mp_cstr(AS_TUPLE(seps)->items[0]); o.ksep=mp_cstr(AS_TUPLE(seps)->items[1]);
    }
    SBuf b={0};
    Catch c;
    if(!CATCH_BEGIN(c)){ j_write(&b,argv[0],&o,0); CATCH_END(c); }
    else { free(b.s); mp_raise(mp_catch_exc(&c)); }
    return sb_value(&b);
}
typedef struct { const char *s; int64_t i, n; } JIn;
static MPY_NORETURN void j_fail(JIn *in, const char *what){
    int64_t line=1, col=1; for(int64_t k=0;k<in->i && k<in->n;k++){ if(in->s[k]=='\n'){ line++; col=1; } else col++; }
    mp_raise_t(E_ValueError,"%s: line %lld column %lld (char %lld)",what,(long long)line,(long long)col,(long long)in->i);
}
static void j_ws(JIn *in){ while(in->i<in->n && (in->s[in->i]==' '||in->s[in->i]=='\t'||in->s[in->i]=='\n'||in->s[in->i]=='\r')) in->i++; }
static int j_hex4(JIn *in){
    if(in->i+4>in->n) j_fail(in,"Invalid \\uXXXX escape");
    int v=0; for(int k=0;k<4;k++){ char c=in->s[in->i++]; v*=16; if(c>='0'&&c<='9') v+=c-'0'; else if(c>='a'&&c<='f') v+=c-'a'+10; else if(c>='A'&&c<='F') v+=c-'A'+10; else j_fail(in,"Invalid \\uXXXX escape"); }
    return v;
}
static Value j_value(JIn *in, int depth);
static Value j_str(JIn *in){
    SBuf b={0}; int64_t start=in->i; in->i++;
    for(;;){
        if(in->i>=in->n){ in->i=start; free(b.s); j_fail(in,"Unterminated string starting at"); }
        char c=in->s[in->i++];
        if(c=='"') break;
        if((unsigned char)c<0x20){ in->i--; free(b.s); j_fail(in,"Invalid control character at"); }
        if(c!='\\'){ sb_putc(&b,c); continue; }
        if(in->i>=in->n){ free(b.s); j_fail(in,"Unterminated string starting at"); }
        c=in->s[in->i++];
        switch(c){
            case '"': case '\\': case '/': sb_putc(&b,c); break;
            case 'n': sb_putc(&b,'\n'); break; case 'r': sb_putc(&b,'\r'); break; case 't': sb_putc(&b,'\t'); break;
            case 'b': sb_putc(&b,'\b'); break; case 'f': sb_putc(&b,'\f'); break;
            case 'u':{ uint32_t cp=(uint32_t)j_hex4(in);
                if(cp>=0xD800 && cp<0xDC00 && in->i+6<=in->n && in->s[in->i]=='\\' && in->s[in->i+1]=='u'){ int64_t save=in->i; in->i+=2; uint32_t lo=(uint32_t)j_hex4(in);
                    if(lo>=0xDC00 && lo<0xE000) cp=0x10000+((cp-0xD800)<<10)+(lo-0xDC00); else in->i=save; }
                char t[4]; int m=mp_utf8_encode(t,cp); sb_put(&b,t,m); break; }
            default: in->i-=2; free(b.s); j_fail(in,"Invalid \\escape");
        }
    }
    return sb_value(&b);
}
static Value j_value(JIn *in, int depth){
    if(depth>500) j_fail(in,"Too deeply nested");
    j_ws(in);
    if(in->i>=in->n) j_fail(in,"Expecting value");
    char c=in->s[in->i];
    if(c=='"') return j_str(in);
    if(c=='{'){
        in->i++; Value d=mp_dict(); j_ws(in);
        if(in->i<in->n && in->s[in->i]=='}'){ in->i++; return d; }
        for(;;){
            j_ws(in);
            if(in->i>=in->n || in->s[in->i]!='"') j_fail(in,"Expecting property name enclosed in double quotes");
            Value k=j_str(in); j_ws(in);
            if(in->i>=in->n || in->s[in->i]!=':') j_fail(in,"Expecting ':' delimiter");
            in->i++;
            mp_dict_set(AS_DICT(d),k,j_value(in,depth+1));
            j_ws(in);
            if(in->i<in->n && in->s[in->i]==','){ in->i++; continue; }
            if(in->i<in->n && in->s[in->i]=='}'){ in->i++; return d; }
            j_fail(in,"Expecting ',' delimiter");
        }
    }
    if(c=='['){
        in->i++; Value l=mp_list(0,NULL); j_ws(in);
        if(in->i<in->n && in->s[in->i]==']'){ in->i++; return l; }
        for(;;){
            mp_list_append(l,j_value(in,depth+1));
            j_ws(in);
            if(in->i<in->n && in->s[in->i]==','){ in->i++; continue; }
            if(in->i<in->n && in->s[in->i]==']'){ in->i++; return l; }
            j_fail(in,"Expecting ',' delimiter");
        }
    }
    static const struct { const char *w; int k; } words[]={{"true",1},{"false",2},{"null",3},{"NaN",4},{"Infinity",5},{"-Infinity",6},{NULL,0}};
    for(int w=0;words[w].w;w++){ int64_t L=(int64_t)strlen(words[w].w);
        if(in->i+L<=in->n && !strncmp(in->s+in->i,words[w].w,(size_t)L)){ in->i+=L;
            switch(words[w].k){ case 1: return v_bool(1); case 2: return v_bool(0); case 3: return v_none(); case 4: return v_float(NAN); case 5: return v_float(INFINITY); default: return v_float(-INFINITY); } } }
    if(c=='-'||(c>='0'&&c<='9')){
        int64_t st=in->i; int isf=0;
        if(in->s[in->i]=='-') in->i++;
        if(in->i>=in->n || !(in->s[in->i]>='0'&&in->s[in->i]<='9')){ in->i=st; j_fail(in,"Expecting value"); }
        if(in->s[in->i]=='0') in->i++; else while(in->i<in->n && in->s[in->i]>='0' && in->s[in->i]<='9') in->i++;
        if(in->i+1<in->n && in->s[in->i]=='.' && in->s[in->i+1]>='0' && in->s[in->i+1]<='9'){ isf=1; in->i++; while(in->i<in->n && in->s[in->i]>='0' && in->s[in->i]<='9') in->i++; }
        if(in->i<in->n && (in->s[in->i]=='e'||in->s[in->i]=='E')){ int64_t save=in->i; in->i++; if(in->i<in->n && (in->s[in->i]=='+'||in->s[in->i]=='-')) in->i++;
            if(in->i<in->n && in->s[in->i]>='0' && in->s[in->i]<='9'){ isf=1; while(in->i<in->n && in->s[in->i]>='0' && in->s[in->i]<='9') in->i++; } else in->i=save; }
        char t[128]; int64_t L=in->i-st; if(L>127) L=127; memcpy(t,in->s+st,(size_t)L); t[L]=0;
        if(isf) return v_float(strtod(t,NULL));
        int64_t v; if(mp_parse_int(t,L,10,&v)!=1) mp_raise_t(E_OverflowError,"int too large (ints are 64-bit)");
        return v_int(v);
    }
    j_fail(in,"Expecting value");
}
static Value js_loads(int argc, Value *argv, TupleObj *kw){
    (void)kw; nargs("loads",npos(argc,kw),1,1);
    Value s=argv[0];
    if(IS_BYTES(s)) s=mp_strn((const char*)AS_BYTES(s)->s,AS_BYTES(s)->len);
    if(!IS_STR(s)) mp_raise_t(E_TypeError,"the JSON object must be str, bytes or bytearray, not %s",mp_type_name(s));
    JIn in={AS_STR(s)->s,0,AS_STR(s)->len};
    Value v=j_value(&in,0);
    j_ws(&in);
    if(in.i<in.n) j_fail(&in,"Extra data");
    return v;
}
static Value make_json(void){
    Value m=register_module("_mpy_json");
    set_fn(m,"dumps",js_dumps); set_fn(m,"loads",js_loads);
    Value err=mp_dict_get_s(mp_builtins,"ValueError",NULL) ? v_obj(E_ValueError) : v_none();
    mp_dict_set_s(mdict(m),"JSONDecodeError",err);
    return m;
}

/* ---------------------------------------------------------------- thread: OS threads under the GIL */
typedef struct { Thread th; volatile int done; Value fn, args; } PyThread;
static PyThread *pthreads[MP_MAX_THREADS];
static mpy_lock user_locks[256]; static int lock_used[256];
static void thread_entry(void *p){
    PyThread *pt=(PyThread*)p;
    mpy_lock_acquire(&mp_gil);
    mp_ts=&pt->th;
    int anchor; pt->th.cstack_base=&anchor;
    Catch c;
    if(!CATCH_BEGIN(c)){
        Value a=pt->args;
        if(IS_TUPLE(a)) mp_call(pt->fn,(int)AS_TUPLE(a)->len,AS_TUPLE(a)->items,NULL);
        else mp_call0(pt->fn);
        CATCH_END(c);
    } else {
        Value e=mp_catch_exc(&c);
        if(!mp_isinstance(e,E_SystemExit)) mp_print_exception(e);
    }
    pt->th.roots[0]=v_undef(); pt->th.roots[1]=v_undef();
    pt->done=1;
    mp_thread_remove(&pt->th);
    mpy_lock_release(&mp_gil);
}
static Value th_start(int argc, Value *argv, TupleObj *kw){
    no_kw("start",kw); nargs("start",argc,1,2);
    if(argc==2 && !IS_TUPLE(argv[1])) mp_raise_t(E_TypeError,"thread.start args must be a tuple");
    int h=-1; for(int i=0;i<MP_MAX_THREADS;i++) if(!pthreads[i] || pthreads[i]->done){ h=i; break; }
    if(h<0) mp_raise_t(E_RuntimeError,"too many threads");
    PyThread *pt=(PyThread*)xmalloc(sizeof(PyThread)); memset(pt,0,sizeof *pt);
    pt->fn=argv[0]; pt->args= argc==2 ? argv[1] : v_none();
    pt->th.roots[0]=pt->fn; pt->th.roots[1]=pt->args; pt->th.id=h+1;
    pthreads[h]=pt;
    mp_thread_add(&pt->th);
    if(mpy_thread_spawn(thread_entry,pt)!=0){ mp_thread_remove(&pt->th); pthreads[h]=NULL; free(pt); mp_raise_t(E_RuntimeError,"cannot start a thread"); }
    return v_int(h);
}
static Value th_join(int argc, Value *argv, TupleObj *kw){
    no_kw("join",kw); nargs("join",argc,1,1);
    int64_t h=mp_index(argv[0],"join");
    if(h<0||h>=MP_MAX_THREADS||!pthreads[h]) mp_raise_t(E_ValueError,"no such thread");
    PyThread *pt=pthreads[h];
    Thread *self=mp_ts;
    while(!pt->done){ mp_gil_release(); mpy_thread_sleep_ms(1); mp_gil_acquire(self); }
    return v_none();
}
static Value th_lock(int argc, Value *argv, TupleObj *kw){ (void)argv; no_kw("lock",kw); nargs("lock",argc,0,0);
    for(int i=0;i<256;i++) if(!lock_used[i]){ lock_used[i]=1; mpy_lock_init(&user_locks[i]); return v_int(i); }
    mp_raise_t(E_RuntimeError,"too many locks"); }
static Value th_acquire(int argc, Value *argv, TupleObj *kw){ no_kw("acquire",kw); nargs("acquire",argc,1,1);
    int64_t i=mp_index(argv[0],"acquire"); if(i<0||i>=256||!lock_used[i]) mp_raise_t(E_ValueError,"no such lock");
    if(mpy_lock_try(&user_locks[i])) return v_none();
    Thread *self=mp_ts; mp_gil_release();
    mpy_lock_acquire(&user_locks[i]);
    mp_gil_acquire(self);
    return v_none(); }
static Value th_release(int argc, Value *argv, TupleObj *kw){ no_kw("release",kw); nargs("release",argc,1,1);
    int64_t i=mp_index(argv[0],"release"); if(i<0||i>=256||!lock_used[i]) mp_raise_t(E_ValueError,"no such lock");
    mpy_lock_release(&user_locks[i]); return v_none(); }
/* ---------------------------------------------------------------- _thread: locks any thread may release, threads (threading.py's) */
typedef struct { Obj h; volatile int locked; } LockObj;
static Type *T_lock;
static double mono_now(void){
#if defined(MPY_KOLIBRI)
    uint32_t in[7]={26,10,0,0,0,0,0}, out[6]={0}; mpy_platform_syscall(in,out); return ((double)out[1]*4294967296.0+(double)out[0])/1e9;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (double)ts.tv_sec+ts.tv_nsec/1e9;
#endif
}
/* takes the lock (a flag the GIL guards): waiting with the GIL released; 0 when timeout runs out */
static int lock_take(LockObj *l, int blocking, double timeout){
    if(!l->locked){ l->locked=1; return 1; }
    if(!blocking) return 0;
    double deadline= timeout>=0 ? mono_now()+timeout : 0;
    Thread *self=mp_ts; int spins=0;
    for(;;){
        mp_gil_release();
        if(spins<20){ mpy_thread_yield(); spins++; } else mpy_thread_sleep_ms(1);
        mp_gil_acquire(self);
        if(!l->locked){ l->locked=1; return 1; }
        if(timeout>=0 && mono_now()>=deadline) return 0;
    }
}
static Value lk_acquire(int argc, Value *argv, TupleObj *kw){
    int np=npos(argc,kw);
    Value b= np>1 ? argv[1] : kwarg(argc,argv,kw,"blocking",v_bool(1));
    Value t= np>2 ? argv[2] : kwarg(argc,argv,kw,"timeout",v_int(-1));
    int blocking=mp_truth(b); double timeout= IS_INTLIKE(t) ? (double)t.u.i : farg(t);
    if(!blocking && timeout!=-1) mp_raise_t(E_ValueError,"can't specify a timeout for a non-blocking call");
    if(timeout<0 && timeout!=-1) mp_raise_t(E_ValueError,"timeout value must be a non-negative number");
    return v_bool(lock_take((LockObj*)argv[0].u.o,blocking,timeout));
}
static Value lk_release(int argc, Value *argv, TupleObj *kw){ no_kw("release",kw); nargs("release",argc,1,1);
    LockObj *l=(LockObj*)argv[0].u.o; if(!l->locked) mp_raise_t(E_RuntimeError,"release unlocked lock"); l->locked=0; return v_none(); }
static Value lk_locked(int argc, Value *argv, TupleObj *kw){ no_kw("locked",kw); nargs("locked",argc,1,1); return v_bool(((LockObj*)argv[0].u.o)->locked); }
static Value lk_enter(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; lock_take((LockObj*)argv[0].u.o,1,-1); return v_bool(1); }
static Value lk_exit(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; ((LockObj*)argv[0].u.o)->locked=0; return v_none(); }
static Value lk_repr(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; char b[96];
    snprintf(b,sizeof b,"<%s _thread.lock object at %p>",((LockObj*)argv[0].u.o)->locked?"locked":"unlocked",(void*)argv[0].u.o); return mp_str(b); }
/* _thread.RLock: the holder may take it again (count), only it may release it */
typedef struct { Obj h; volatile int locked; int64_t owner, count; } RLockObj;
static Type *T_rlock;
static Value rl_acquire(int argc, Value *argv, TupleObj *kw){
    RLockObj *l=(RLockObj*)argv[0].u.o; int64_t me=mp_ts->id;
    if(l->count && l->owner==me){ l->count++; return v_bool(1); }
    Value r=lk_acquire(argc,argv,kw);
    if(mp_truth(r)){ l->owner=me; l->count=1; }
    return r; }
static Value rl_release(int argc, Value *argv, TupleObj *kw){ no_kw("release",kw); nargs("release",argc,1,1);
    RLockObj *l=(RLockObj*)argv[0].u.o;
    if(!l->count || l->owner!=mp_ts->id) mp_raise_t(E_RuntimeError,"cannot release un-acquired lock");
    if(--l->count==0){ l->owner=-1; l->locked=0; }
    return v_none(); }
static Value rl_enter(int argc, Value *argv, TupleObj *kw){ (void)kw; Value a[1]={argv[0]}; (void)argc; return rl_acquire(1,a,NULL); }
static Value rl_exit(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; Value a[1]={argv[0]}; return rl_release(1,a,NULL); }
static Value rl_is_owned(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; RLockObj *l=(RLockObj*)argv[0].u.o; return v_bool(l->count && l->owner==mp_ts->id); }
static Value rl_count(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; RLockObj *l=(RLockObj*)argv[0].u.o; return v_int(l->owner==mp_ts->id ? l->count : 0); }
static Value rl_release_save(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; RLockObj *l=(RLockObj*)argv[0].u.o;
    if(!l->count) mp_raise_t(E_RuntimeError,"cannot release un-acquired lock");
    Value st[2]={v_int(l->count),v_int(l->owner)}; l->count=0; l->owner=-1; l->locked=0; return mp_tuple(2,st); }
static Value rl_acquire_restore(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("_acquire_restore",argc,1,1); RLockObj *l=(RLockObj*)argv[0].u.o;
    Value a[1]={argv[0]}; lk_acquire(1,a,NULL);
    if(IS_TUPLE(argv[1]) && AS_TUPLE(argv[1])->len==2){ l->count=AS_TUPLE(argv[1])->items[0].u.i; l->owner=AS_TUPLE(argv[1])->items[1].u.i; }
    return v_none(); }
static Value rl_repr(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; RLockObj *l=(RLockObj*)argv[0].u.o; char b[160];
    snprintf(b,sizeof b,"<%s _thread.RLock object owner=%lld count=%lld at %p>",l->locked?"locked":"unlocked",(long long)(l->count?l->owner:0),(long long)l->count,(void*)l); return mp_str(b); }
static Value make_rlock(Type *t, int argc, Value *argv, TupleObj *kw){ (void)t; (void)argc; (void)argv; (void)kw;
    RLockObj *l=(RLockObj*)mp_alloc(T_rlock,sizeof(RLockObj)); l->locked=0; l->owner=-1; l->count=0; return v_obj(l); }
static Value tl_allocate(int argc, Value *argv, TupleObj *kw){ (void)argv; no_kw("allocate_lock",kw); nargs("allocate_lock",argc,0,0);
    LockObj *l=(LockObj*)mp_alloc(T_lock,sizeof(LockObj)); l->locked=0; return v_obj(l); }
static Value make_lock_type(Type *t, int argc, Value *argv, TupleObj *kw){ (void)t; return tl_allocate(argc,argv,kw); }
static Value tl_start(int argc, Value *argv, TupleObj *kw){
    no_kw("start_new_thread",kw); nargs("start_new_thread",argc,2,3);
    if(!IS_TUPLE(argv[1])) mp_raise_t(E_TypeError,"2nd arg must be a tuple");
    Value fn=argv[0], args=argv[1];
    if(argc==3 && !IS_NONE(argv[2])){
        if(!IS_DICT(argv[2])) mp_raise_t(E_TypeError,"optional 3rd arg must be a dictionary");
        if(AS_DICT(argv[2])->used){                                   /* (keywords: functools.partial does the call) */
            Value ft=mp_str("functools"); Value fm=mp_builtin_import(1,&ft,NULL);
            Value pa[1]={fn}; Value names=mp_tuple(AS_DICT(argv[2])->used,NULL); Value *vals=(Value*)xmalloc(sizeof(Value)*(size_t)(AS_DICT(argv[2])->used+1));
            int64_t pos=0, i=0; Value k, v; vals[0]=fn;
            while(mp_dict_next(AS_DICT(argv[2]),&pos,&k,&v)){ AS_TUPLE(names)->items[i]=k; vals[1+i]=v; i++; }
            (void)pa; fn=mp_call(mp_getattr_s(fm,"partial"),(int)(1+i),vals,AS_TUPLE(names)); free(vals); } }
    Value a[2]={fn,args};
    Value h=th_start(2,a,NULL);
    return v_int(h.u.i+1);                                           /* (the thread's id) */
}
static Value tl_get_ident(int argc, Value *argv, TupleObj *kw){ (void)argv; (void)argc; (void)kw; return v_int(mp_ts->id); }
static Value tl_count(int argc, Value *argv, TupleObj *kw){ (void)argv; (void)argc; (void)kw;
    int n=0; for(int i=0;i<MP_MAX_THREADS;i++) if(pthreads[i] && !pthreads[i]->done) n++; return v_int(n); }
static Value tl_stack_size(int argc, Value *argv, TupleObj *kw){ (void)argv; (void)argc; (void)kw; return v_int(0); }
static Value make__thread(void){
    Value m=register_module("_thread");
    if(!T_lock){ T_lock=AS_TYPE(mp_new_type("lock",T_object,LY_OBJECT,0)); T_lock->make=make_lock_type;
        mp_type_add(T_lock,"acquire",lk_acquire); mp_type_add(T_lock,"release",lk_release); mp_type_add(T_lock,"locked",lk_locked);
        mp_type_add(T_lock,"__enter__",lk_enter); mp_type_add(T_lock,"__exit__",lk_exit); mp_type_add(T_lock,"__repr__",lk_repr);
        mp_type_add(T_lock,"acquire_lock",lk_acquire); mp_type_add(T_lock,"release_lock",lk_release); mp_type_add(T_lock,"locked_lock",lk_locked); }
    set_fn(m,"allocate_lock",tl_allocate); set_fn(m,"allocate",tl_allocate); set_fn(m,"start_new_thread",tl_start); set_fn(m,"start_new",tl_start);
    set_fn(m,"get_ident",tl_get_ident); set_fn(m,"get_native_id",tl_get_ident); set_fn(m,"_count",tl_count); set_fn(m,"stack_size",tl_stack_size);
    if(!T_rlock){ T_rlock=AS_TYPE(mp_new_type("RLock",T_object,LY_OBJECT,0)); T_rlock->make=make_rlock;
        mp_type_add(T_rlock,"acquire",rl_acquire); mp_type_add(T_rlock,"release",rl_release); mp_type_add(T_rlock,"__enter__",rl_enter);
        mp_type_add(T_rlock,"__exit__",rl_exit); mp_type_add(T_rlock,"_is_owned",rl_is_owned); mp_type_add(T_rlock,"_recursion_count",rl_count);
        mp_type_add(T_rlock,"_release_save",rl_release_save); mp_type_add(T_rlock,"_acquire_restore",rl_acquire_restore);
        mp_type_add(T_rlock,"locked",lk_locked); mp_type_add(T_rlock,"__repr__",rl_repr); }
    mp_dict_set_s(mdict(m),"RLock",v_obj(T_rlock));
    mp_dict_set_s(mdict(m),"LockType",v_obj(T_lock));
    mp_dict_set_s(mdict(m),"error",v_obj(E_RuntimeError));
    mp_dict_set_s(mdict(m),"TIMEOUT_MAX",v_float(4294967.0));
    return m;
}
static Value make_thread(void){
    Value m=register_module("thread");
    set_fn(m,"start",th_start); set_fn(m,"join",th_join); set_fn(m,"sleep",ti_sleep); set_fn(m,"lock",th_lock);
    set_fn(m,"acquire",th_acquire); set_fn(m,"release",th_release); set_fn(m,"current",tl_get_ident);
    return m;
}

/* ---------------------------------------------------------------- _ctypes: C functions of shared libraries (lib/ctypes.mpy) */
#if !defined(MPY_KOLIBRI)
static int ct_errno;
static Value ct_dlopen(int argc, Value *argv, TupleObj *kw){
    (void)kw;
    const char *name= argc>0 && IS_STR(argv[0]) ? mp_cstr(argv[0]) : NULL;
    void *h=dlopen(name,RTLD_NOW|RTLD_GLOBAL);
#if defined(__APPLE__)
    if(!h && name && (!strncmp(name,"libc.so",7) || !strncmp(name,"libm.so",7))) h=dlopen("libSystem.B.dylib",RTLD_NOW|RTLD_GLOBAL);
#endif
    if(!h){ const char *e=dlerror(); mp_raise_t(E_OSError,"%s",e?e:"cannot load the library"); }
    return v_int((int64_t)(intptr_t)h);
}
static Value ct_dlsym(int argc, Value *argv, TupleObj *kw){
    (void)kw; nargs("dlsym",argc,2,2);
    void *p=dlsym((void*)(intptr_t)mp_index(argv[0],"handle"),mp_cstr(argv[1]));
    if(!p) mp_raise_t(E_AttributeError,"function '%s' not found",mp_cstr(argv[1]));
    return v_int((int64_t)(intptr_t)p);
}
typedef intptr_t (*cfn_i)(intptr_t,intptr_t,intptr_t,intptr_t,intptr_t,intptr_t,intptr_t,intptr_t);
typedef intptr_t (*cfn_v1)(intptr_t,...); typedef intptr_t (*cfn_v2)(intptr_t,intptr_t,...);
typedef intptr_t (*cfn_v3)(intptr_t,intptr_t,intptr_t,...); typedef intptr_t (*cfn_v4)(intptr_t,intptr_t,intptr_t,intptr_t,...);
#define CT_NOINLINE __attribute__((noinline))
static CT_NOINLINE intptr_t ct_call_n(void *f, intptr_t *a){ return ((cfn_i)f)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]); }
static CT_NOINLINE intptr_t ct_call_v1(void *f, intptr_t *a){ return ((cfn_v1)f)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]); }
static CT_NOINLINE intptr_t ct_call_v2(void *f, intptr_t *a){ return ((cfn_v2)f)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]); }
static CT_NOINLINE intptr_t ct_call_v3(void *f, intptr_t *a){ return ((cfn_v3)f)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]); }
static CT_NOINLINE intptr_t ct_call_v4(void *f, intptr_t *a){ return ((cfn_v4)f)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]); }
typedef double (*cfn_d0)(void); typedef double (*cfn_d1)(double); typedef double (*cfn_d2)(double,double); typedef double (*cfn_d3)(double,double,double);
static Value ct_call(int argc, Value *argv, TupleObj *kw){
    (void)kw;
    if(argc<3 || !IS_STR(argv[1])) mp_raise_t(E_TypeError,"call(address, restype, fixed, *args)");
    void *fp=(void*)(intptr_t)mp_index(argv[0],"address"); const char *rk=mp_cstr(argv[1]);
    int fixed=(int)mp_index(argv[2],"fixed");
    int n=argc-3, floats=0; Value *a0=argv+3;
    for(int i=0;i<n;i++) if(a0[i].k==V_FLOAT) floats++;
    errno=0;
    if(!strcmp(rk,"f64")||!strcmp(rk,"f32")){
        if(floats!=n || n>3) mp_raise_t(E_TypeError,"the interpreter calls a C function returning a double with 0 to 3 double arguments");
        double d[3]={0,0,0}; for(int i=0;i<n;i++) d[i]=a0[i].u.f;
        mp_flush_stdout();
        double r= n==0?((cfn_d0)fp)():n==1?((cfn_d1)fp)(d[0]):n==2?((cfn_d2)fp)(d[0],d[1]):((cfn_d3)fp)(d[0],d[1],d[2]);
        ct_errno=errno; return v_float(r);
    }
    if(floats || n>8) mp_raise_t(E_TypeError,"the interpreter calls C functions with up to 8 integer, pointer or str arguments");
    intptr_t a[8]={0};
    for(int i=0;i<n;i++){ Value v=a0[i];
        if(IS_INTLIKE(v)) a[i]=(intptr_t)v.u.i;
        else if(IS_NONE(v)) a[i]=0;
        else if(IS_STR(v)) a[i]=(intptr_t)AS_STR(v)->s;
        else if(IS_BYTES(v)) a[i]=(intptr_t)AS_BYTES(v)->s;
        else if(IS(v,T_buffer)) a[i]=(intptr_t)((BufferObj*)v.u.o)->data;
        else if(IS_BA(v)) a[i]=(intptr_t)((ByteArrayObj*)v.u.o)->data;
        else mp_raise_t(E_TypeError,"a C function takes int, str, bytes, buffers or None");
    }
    if(fixed>=0 && fixed<n && (fixed<1 || fixed>4)) mp_raise_t(E_TypeError,"a variadic C function takes 1 to 4 fixed arguments in the interpreter");
    mp_flush_stdout();
    Thread *self=mp_ts; mp_gil_release();
    intptr_t r= fixed<0 || fixed>=n ? ct_call_n(fp,a) : fixed==1 ? ct_call_v1(fp,a) : fixed==2 ? ct_call_v2(fp,a) : fixed==3 ? ct_call_v3(fp,a) : ct_call_v4(fp,a);
    ct_errno=errno;
    mp_gil_acquire(self);
    if(!strcmp(rk,"void")) return v_none();
    if(!strcmp(rk,"i32")) return v_int((int32_t)r);
    if(!strcmp(rk,"u32")) return v_int((uint32_t)r);
    if(!strcmp(rk,"i16")) return v_int((int16_t)r);
    if(!strcmp(rk,"u16")) return v_int((uint16_t)r);
    if(!strcmp(rk,"i8")) return v_int((int8_t)r);
    if(!strcmp(rk,"u8")) return v_int((uint8_t)r);
    if(!strcmp(rk,"bool")) return v_bool((r&0xFF)!=0);
    if(!strcmp(rk,"str")) return r ? mp_str((const char*)r) : v_none();
    if(!strcmp(rk,"up")) return v_int((int64_t)(uintptr_t)r);
    return v_int((int64_t)r);
}
static Value ct_errno_fn(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; return v_int(ct_errno); }
#else
static Value ct_unsupported(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; mp_raise_t(E_OSError,"ctypes is not available on this platform"); }
#define ct_dlopen ct_unsupported
#define ct_dlsym ct_unsupported
#define ct_call ct_unsupported
#define ct_errno_fn ct_unsupported
#endif
static Value make_ctypes_native(void){
    Value m=register_module("_ctypes");
    set_fn(m,"dlopen",ct_dlopen); set_fn(m,"dlsym",ct_dlsym); set_fn(m,"call",ct_call); set_fn(m,"errno",ct_errno_fn);
    return m;
}

/* ---------------------------------------------------------------- native awaitables (asyncio helpers) */
/* an IterObj of kind IT_NATIVE: aux2 a native taking (it, sent) that yields a value or raises StopIteration(value) */
int mp_native_send(Value it, Value v, Value *out){
    IterObj *io=(IterObj*)it.u.o;
    Catch c; Value r;
    if(!CATCH_BEGIN(c)){ Value a[2]={it,v}; r=mp_call(io->aux2,2,a,NULL); CATCH_END(c); *out=r; return 1; }
    Value e=mp_catch_exc(&c);
    if(mp_isinstance(e,E_StopIteration)){ TupleObj *t=AS_TUPLE(AS_EXC(e)->args); *out= t->len ? t->items[0] : v_none(); return 0; }
    mp_raise(e);
}

/* ---------------------------------------------------------------- modules written in Python */
static const char SRC_ASYNCIO[]=
"import time as _time\n"
"class CancelledError(BaseException):\n    pass\n"
"class InvalidStateError(Exception):\n    pass\n"
"TimeoutError = TimeoutError\n"
"_ready = []\n_timers = []\n_seq = [0]\n_running = [0]\n"
"class Future:\n"
"    def __init__(self):\n"
"        self._done = False\n        self._result = None\n        self._exc = None\n        self._waiters = []\n        self._callbacks = []\n"
"    def done(self):\n        return self._done\n"
"    def cancelled(self):\n        return self._done and isinstance(self._exc, CancelledError)\n"
"    def result(self):\n"
"        if not self._done:\n            raise InvalidStateError('Result is not set.')\n"
"        if self._exc is not None:\n            raise self._exc\n"
"        return self._result\n"
"    def exception(self):\n"
"        if not self._done:\n            raise InvalidStateError('Exception is not set.')\n"
"        return self._exc\n"
"    def set_result(self, value):\n"
"        if self._done:\n            raise InvalidStateError('invalid state')\n"
"        self._result = value\n        self._finish()\n"
"    def set_exception(self, exc):\n"
"        if self._done:\n            raise InvalidStateError('invalid state')\n"
"        if isinstance(exc, type):\n            exc = exc()\n"
"        self._exc = exc\n        self._finish()\n"
"    def _finish(self):\n"
"        self._done = True\n"
"        for t in self._waiters:\n            _ready.append((t, None))\n"
"        self._waiters = []\n"
"        cbs = self._callbacks\n        self._callbacks = []\n"
"        for cb in cbs:\n            cb(self)\n"
"    def add_done_callback(self, cb):\n"
"        if self._done:\n            cb(self)\n        else:\n            self._callbacks.append(cb)\n"
"    def __await__(self):\n"
"        if not self._done:\n            yield self\n"
"        return self.result()\n"
"    __iter__ = __await__\n"
"class Task(Future):\n"
"    def __init__(self, coro, name=None):\n"
"        Future.__init__(self)\n        self._coro = coro\n        self._name = name\n"
"        _ready.append((self, None))\n"
"    def get_name(self):\n        return self._name\n"
"    def get_coro(self):\n        return self._coro\n"
"    def cancel(self, msg=None):\n"
"        if self._done:\n            return False\n"
"        _ready.append((self, CancelledError() if msg is None else CancelledError(msg)))\n"
"        return True\n"
"    def _step(self, exc):\n"
"        if self._done:\n            return\n"
"        try:\n"
"            if exc is not None:\n                y = self._coro.throw(exc)\n"
"            else:\n                y = self._coro.send(None)\n"
"        except StopIteration as e:\n            self.set_result(e.value)\n            return\n"
"        except CancelledError as e:\n            self._exc = e\n            self._finish()\n            return\n"
"        except BaseException as e:\n            self.set_exception(e)\n            return\n"
"        if isinstance(y, Future):\n"
"            if y._done:\n                _ready.append((self, None))\n"
"            else:\n                y._waiters.append(self)\n"
"        elif isinstance(y, _Sleep):\n            _schedule(self, y.delay)\n"
"        else:\n            _ready.append((self, None))\n"
"class _Sleep:\n"
"    def __init__(self, delay):\n        self.delay = delay\n"
"class _SleepAwait:\n"
"    def __init__(self, delay, result):\n        self.delay = delay\n        self.result = result\n"
"    def __await__(self):\n"
"        if self.delay <= 0:\n            yield None\n"
"        else:\n            yield _Sleep(self.delay)\n"
"        return self.result\n"
"def _schedule(task, delay):\n"
"    when = _time.monotonic() + delay\n"
"    _seq[0] += 1\n"
"    entry = (when, _seq[0], task)\n"
"    i = len(_timers)\n"
"    while i > 0 and _timers[i - 1][0] > when:\n        i -= 1\n"
"    _timers.insert(i, entry)\n"
"def sleep(delay, result=None):\n    return _SleepAwait(delay, result)\n"
"def _run_until(fut):\n"
"    while not fut._done:\n"
"        if _ready:\n"
"            batch = _ready[:]\n            del _ready[:]\n"
"            for t, e in batch:\n                t._step(e)\n"
"            continue\n"
"        if _timers:\n"
"            when, seq, t = _timers[0]\n"
"            now = _time.monotonic()\n"
"            if when > now:\n                _time.sleep(when - now)\n"
"            _timers.pop(0)\n            _ready.append((t, None))\n            continue\n"
"        raise RuntimeError('Event loop stopped before Future completed.')\n"
"def iscoroutine(x):\n    return type(x).__name__ == 'coroutine'\n"
"def run(main, debug=None):\n"
"    if _running[0]:\n        raise RuntimeError('asyncio.run() cannot be called from a running event loop')\n"
"    if not iscoroutine(main):\n        raise ValueError('a coroutine was expected, got ' + repr(main))\n"
"    _running[0] = 1\n"
"    try:\n        t = Task(main)\n        _run_until(t)\n        return t.result()\n"
"    finally:\n        _running[0] = 0\n"
"def _run_nested(coro):\n    t = Task(coro)\n    _run_until(t)\n    return t.result()\n"
"def create_task(coro, name=None):\n    return Task(coro, name)\n"
"def ensure_future(x):\n    if isinstance(x, Future):\n        return x\n    return Task(x)\n"
"class _Gather(Future):\n"
"    def __init__(self, tasks, rex):\n"
"        Future.__init__(self)\n        self._tasks = tasks\n        self._rex = rex\n        self._left = len(tasks)\n"
"        if not tasks:\n            self.set_result([])\n"
"        for t in tasks:\n            t.add_done_callback(self._one_done)\n"
"    def _one_done(self, t):\n"
"        if self._done:\n            return\n"
"        if t._exc is not None and not self._rex:\n            self.set_exception(t._exc)\n            return\n"
"        self._left -= 1\n"
"        if self._left == 0:\n"
"            self.set_result([x._exc if x._exc is not None else x._result for x in self._tasks])\n"
"def gather(*aws, return_exceptions=False):\n"
"    return _Gather([ensure_future(a) for a in aws], return_exceptions)\n"
"async def wait_for(aw, timeout):\n    return await aw\n"
"def get_event_loop():\n    return None\n"
"class Lock:\n"
"    def __init__(self):\n        self._locked = False\n        self._waiters = []\n"
"    def locked(self):\n        return self._locked\n"
"    async def acquire(self):\n"
"        while self._locked:\n            f = Future()\n            self._waiters.append(f)\n            await f\n"
"        self._locked = True\n        return True\n"
"    def release(self):\n"
"        self._locked = False\n"
"        if self._waiters:\n            self._waiters.pop(0).set_result(None)\n"
"    async def __aenter__(self):\n        await self.acquire()\n"
"    async def __aexit__(self, *a):\n        self.release()\n"
"class Event:\n"
"    def __init__(self):\n        self._set = False\n        self._waiters = []\n"
"    def is_set(self):\n        return self._set\n"
"    def set(self):\n"
"        self._set = True\n"
"        for f in self._waiters:\n            if not f.done():\n                f.set_result(True)\n"
"        self._waiters = []\n"
"    def clear(self):\n        self._set = False\n"
"    async def wait(self):\n"
"        if self._set:\n            return True\n"
"        f = Future()\n        self._waiters.append(f)\n        await f\n        return True\n"
"class Queue:\n"
"    def __init__(self, maxsize=0):\n        self._items = []\n        self._getters = []\n"
"    def qsize(self):\n        return len(self._items)\n"
"    def empty(self):\n        return not self._items\n"
"    def put_nowait(self, item):\n"
"        self._items.append(item)\n"
"        if self._getters:\n            self._getters.pop(0).set_result(None)\n"
"    async def put(self, item):\n        self.put_nowait(item)\n"
"    async def get(self):\n"
"        while not self._items:\n            f = Future()\n            self._getters.append(f)\n            await f\n"
"        return self._items.pop(0)\n"
"    def get_nowait(self):\n        return self._items.pop(0)\n";

static const char SRC_MINIPY[]=
"import _mpy_json as _json\n"
"import asyncio as _asyncio\n"
"Endpoint = 'minipy.Endpoint'\n"
"_YES = ('1', 'true', 't', 'yes', 'y', 'on')\n"
"_NO = ('0', 'false', 'f', 'no', 'n', 'off')\n"
"def endpoint(fn):\n"
"    if getattr(fn, '__minipy_endpoint__', False):\n        return fn\n"
"    ann = fn.__minipy_types__\n"
"    code = fn.__code__\n"
"    names = code.co_varnames[:code.co_argcount]\n"
"    defaults = fn.__defaults__ or ()\n"
"    first_default = len(names) - len(defaults)\n"
"    def adapter(values):\n"
"        args = []\n"
"        for i in range(len(names)):\n"
"            name = names[i]\n"
"            typ = ann.get(name, 'str')\n"
"            if name not in values:\n"
"                if i >= first_default:\n                    args.append(defaults[i - first_default])\n                    continue\n"
"                raise ValueError('#endpoint\\nmissing\\n' + name + '\\n' + typ + '\\n')\n"
"            text = values[name]\n"
"            if typ == 'int':\n"
"                try:\n                    args.append(int(text))\n"
"                except ValueError:\n                    raise ValueError('#endpoint\\nparsing\\n' + name + '\\nint\\n' + text)\n"
"            elif typ == 'float':\n"
"                try:\n                    args.append(float(text))\n"
"                except ValueError:\n                    raise ValueError('#endpoint\\nparsing\\n' + name + '\\nfloat\\n' + text)\n"
"            elif typ == 'bool':\n"
"                low = text.lower()\n"
"                if low in _YES:\n                    args.append(True)\n"
"                elif low in _NO:\n                    args.append(False)\n"
"                else:\n                    raise ValueError('#endpoint\\nparsing\\n' + name + '\\nbool\\n' + text)\n"
"            else:\n                args.append(text)\n"
"        r = fn(*args)\n"
"        if type(r).__name__ == 'coroutine':\n            r = _asyncio._run_nested(r)\n"
"        return _json.dumps(r, separators=(',', ':'), ensure_ascii=False)\n"
"    adapter.__minipy_endpoint__ = True\n"
"    adapter.__name__ = fn.__name__\n"
"    return adapter\n";


static const char SRC_DATACLASSES[]=
"class _MissingType:\n"
"    def __repr__(self):\n"
"        return 'MISSING'\n"
"MISSING = _MissingType()\n"
"class _KwOnlyType:\n"
"    def __repr__(self):\n"
"        return 'KW_ONLY'\n"
"KW_ONLY = _KwOnlyType()\n"
"class FrozenInstanceError(AttributeError):\n"
"    pass\n"
"class InitVar:\n"
"    def __init__(self, type):\n"
"        self.type = type\n"
"    def __class_getitem__(cls, t):\n"
"        return InitVar(t)\n"
"class Field:\n"
"    def __init__(self, default, default_factory, init, repr, hash, compare, metadata, kw_only):\n"
"        self.name = None\n"
"        self.type = None\n"
"        self.default = default\n"
"        self.default_factory = default_factory\n"
"        self.init = init\n"
"        self.repr = repr\n"
"        self.hash = hash\n"
"        self.compare = compare\n"
"        self.metadata = {} if metadata is None else metadata\n"
"        self.kw_only = kw_only\n"
"        self._classvar = False\n"
"        self._initvar = False\n"
"    def __repr__(self):\n"
"        return ('Field(name=' + repr(self.name) + ',type=' + repr(self.type) + ',default=' + repr(self.default)\n"
"                + ',default_factory=' + repr(self.default_factory) + ',init=' + repr(self.init) + ',repr=' + repr(self.repr)\n"
"                + ',hash=' + repr(self.hash) + ',compare=' + repr(self.compare) + ',metadata=' + repr(self.metadata)\n"
"                + ',kw_only=' + repr(self.kw_only) + ',_field_type=' + ('_FIELD_CLASSVAR' if self._classvar else '_FIELD') + ')')\n"
"def field(*, default=MISSING, default_factory=MISSING, init=True, repr=True, hash=None, compare=True, metadata=None, kw_only=MISSING):\n"
"    if default is not MISSING and default_factory is not MISSING:\n"
"        raise ValueError('cannot specify both default and default_factory')\n"
"    return Field(default, default_factory, init, repr, hash, compare, metadata, kw_only)\n"
"def _is_classvar(tp):\n"
"    if isinstance(tp, str):\n"
"        return tp == 'ClassVar' or tp.startswith('ClassVar[') or tp.startswith('typing.ClassVar')\n"
"    return getattr(tp, '__name__', None) == 'ClassVar' or repr(tp).startswith('typing.ClassVar')\n"
"def _is_initvar(tp):\n"
"    if isinstance(tp, str):\n"
"        return tp.startswith('InitVar[') or tp.startswith('dataclasses.InitVar')\n"
"    return isinstance(tp, InitVar)\n"
"def _plural(n, word):\n"
"    return str(n) + ' ' + word + ('' if n == 1 else 's')\n"
"def _names(ns):\n"
"    q = [\"'\" + n + \"'\" for n in ns]\n"
"    if len(q) == 1:\n"
"        return q[0]\n"
"    if len(q) == 2:\n"
"        return q[0] + ' and ' + q[1]\n"
"    return ', '.join(q[:-1]) + ', and ' + q[-1]\n"
"def _make_init(cls, flds, initvars, frozen, post_init):\n"
"    pos = [f for f in flds if f.init and not f.kw_only]\n"
"    kwo = [f for f in flds if f.init and f.kw_only]\n"
"    allp = pos + kwo\n"
"    qual = cls.__qualname__ + '.__init__()'\n"
"    def __init__(self, *args, **kwargs):\n"
"        if len(args) > len(pos):\n"
"            raise TypeError(qual + ' takes ' + str(len(pos) + 1) + ' positional argument' + ('' if len(pos) == 0 else 's') + ' but ' + str(len(args) + 1) + ' were given')\n"
"        vals = {}\n"
"        for i in range(len(args)):\n"
"            vals[pos[i].name] = args[i]\n"
"        for k in kwargs:\n"
"            ok = False\n"
"            for f in allp:\n"
"                if f.name == k:\n"
"                    ok = True\n"
"            if not ok:\n"
"                raise TypeError(qual + \" got an unexpected keyword argument '\" + k + \"'\")\n"
"            if k in vals:\n"
"                raise TypeError(qual + \" got multiple values for argument '\" + k + \"'\")\n"
"            vals[k] = kwargs[k]\n"
"        missing = [f.name for f in pos if f.name not in vals and f.default is MISSING and f.default_factory is MISSING]\n"
"        if missing:\n"
"            raise TypeError(qual + ' missing ' + _plural(len(missing), 'required positional argument') + ': ' + _names(missing))\n"
"        missing = [f.name for f in kwo if f.name not in vals and f.default is MISSING and f.default_factory is MISSING]\n"
"        if missing:\n"
"            raise TypeError(qual + ' missing ' + _plural(len(missing), 'required keyword-only argument') + ': ' + _names(missing))\n"
"        extra = []\n"
"        for f in flds:\n"
"            if f.name in vals:\n"
"                v = vals[f.name]\n"
"            elif f.default_factory is not MISSING:\n"
"                v = f.default_factory()\n"
"            elif f.init and f.default is not MISSING:\n"
"                v = f.default\n"
"            else:\n"
"                continue\n"
"            if f._initvar:\n"
"                extra.append(v)\n"
"                continue\n"
"            if frozen:\n"
"                object.__setattr__(self, f.name, v)\n"
"            else:\n"
"                setattr(self, f.name, v)\n"
"        if post_init:\n"
"            self.__post_init__(*extra)\n"
"    __init__.__qualname__ = cls.__qualname__ + '.__init__'\n"
"    return __init__\n"
"def _tuple(obj, flds):\n"
"    return tuple([getattr(obj, f.name) for f in flds])\n"
"def _process(cls, init, repr_, eq, order, unsafe_hash, frozen, match_args, kw_only):\n"
"    fields = {}\n"
"    for b in cls.__mro__[-1:0:-1]:\n"
"        bf = getattr(b, '__dataclass_fields__', None)\n"
"        if bf is not None:\n"
"            for f in bf.values():\n"
"                fields[f.name] = f\n"
"    anns = cls.__annotations__\n"
"    kw = kw_only\n"
"    for name in anns:\n"
"        tp = anns[name]\n"
"        if tp is KW_ONLY or tp == 'KW_ONLY' or tp == 'dataclasses.KW_ONLY':\n"
"            kw = True\n"
"            continue\n"
"        default = cls.__dict__.get(name, MISSING)\n"
"        if isinstance(default, Field):\n"
"            f = default\n"
"        else:\n"
"            if isinstance(default, (list, dict, set)):\n"
"                raise ValueError('mutable default ' + str(type(default)) + ' for field ' + name + ' is not allowed: use default_factory')\n"
"            f = field(default=default)\n"
"        f.name = name\n"
"        f.type = tp\n"
"        f._classvar = _is_classvar(tp)\n"
"        f._initvar = _is_initvar(tp)\n"
"        if f.kw_only is MISSING:\n"
"            f.kw_only = kw\n"
"        if f._classvar:\n"
"            continue\n"
"        if isinstance(default, Field):\n"
"            if f.default is MISSING:\n"
"                delattr(cls, name)\n"
"            else:\n"
"                setattr(cls, name, f.default)\n"
"        fields[name] = f\n"
"    flds = list(fields.values())\n"
"    seen_default = None\n"
"    for f in flds:\n"
"        if f.init and not f.kw_only:\n"
"            if f.default is MISSING and f.default_factory is MISSING:\n"
"                if seen_default is not None:\n"
"                    raise TypeError(\"non-default argument '\" + f.name + \"' follows default argument '\" + seen_default + \"'\")\n"
"            else:\n"
"                seen_default = f.name\n"
"    real = [f for f in flds if not f._initvar]\n"
"    cls.__dataclass_fields__ = {f.name: f for f in real}\n"
"    own = cls.__dict__\n"
"    if init:\n"
"        cls.__init__ = _make_init(cls, flds, [], frozen, hasattr(cls, '__post_init__'))\n"
"    if repr_ and '__repr__' not in own:\n"
"        rf = [f for f in real if f.repr]\n"
"        def __repr__(self):\n"
"            return type(self).__qualname__ + '(' + ', '.join([f.name + '=' + repr(getattr(self, f.name)) for f in rf]) + ')'\n"
"        cls.__repr__ = __repr__\n"
"    cmp = [f for f in real if f.compare]\n"
"    if eq and '__eq__' not in own:\n"
"        def __eq__(self, other):\n"
"            if other.__class__ is self.__class__:\n"
"                return _tuple(self, cmp) == _tuple(other, cmp)\n"
"            return NotImplemented\n"
"        cls.__eq__ = __eq__\n"
"    if order:\n"
"        def __lt__(self, other):\n"
"            if other.__class__ is self.__class__:\n"
"                return _tuple(self, cmp) < _tuple(other, cmp)\n"
"            return NotImplemented\n"
"        def __le__(self, other):\n"
"            if other.__class__ is self.__class__:\n"
"                return _tuple(self, cmp) <= _tuple(other, cmp)\n"
"            return NotImplemented\n"
"        def __gt__(self, other):\n"
"            if other.__class__ is self.__class__:\n"
"                return _tuple(self, cmp) > _tuple(other, cmp)\n"
"            return NotImplemented\n"
"        def __ge__(self, other):\n"
"            if other.__class__ is self.__class__:\n"
"                return _tuple(self, cmp) >= _tuple(other, cmp)\n"
"            return NotImplemented\n"
"        cls.__lt__ = __lt__\n"
"        cls.__le__ = __le__\n"
"        cls.__gt__ = __gt__\n"
"        cls.__ge__ = __ge__\n"
"    hf = [f for f in real if (f.compare if f.hash is None else f.hash)]\n"
"    if unsafe_hash or (eq and frozen):\n"
"        def __hash__(self):\n"
"            return hash(_tuple(self, hf))\n"
"        cls.__hash__ = __hash__\n"
"    elif eq and '__hash__' not in own:\n"
"        cls.__hash__ = None\n"
"    if frozen:\n"
"        def __setattr__(self, name, value):\n"
"            if type(self) is cls or name in cls.__dataclass_fields__:\n"
"                raise FrozenInstanceError(\"cannot assign to field '\" + name + \"'\")\n"
"            object.__setattr__(self, name, value)\n"
"        def __delattr__(self, name):\n"
"            if type(self) is cls or name in cls.__dataclass_fields__:\n"
"                raise FrozenInstanceError(\"cannot delete field '\" + name + \"'\")\n"
"            object.__delattr__(self, name)\n"
"        cls.__setattr__ = __setattr__\n"
"        cls.__delattr__ = __delattr__\n"
"    if match_args and '__match_args__' not in own:\n"
"        cls.__match_args__ = tuple([f.name for f in flds if f.init and not f.kw_only])\n"
"    return cls\n"
"def dataclass(cls=None, *, init=True, repr=True, eq=True, order=False, unsafe_hash=False, frozen=False, match_args=True, kw_only=False, slots=False, weakref_slot=False):\n"
"    def wrap(c):\n"
"        return _process(c, init, repr, eq, order, unsafe_hash, frozen, match_args, kw_only)\n"
"    if cls is None:\n"
"        return wrap\n"
"    return wrap(cls)\n"
"def is_dataclass(obj):\n"
"    c = obj if isinstance(obj, type) else type(obj)\n"
"    return hasattr(c, '__dataclass_fields__')\n"
"def fields(obj):\n"
"    c = obj if isinstance(obj, type) else type(obj)\n"
"    fs = getattr(c, '__dataclass_fields__', None)\n"
"    if fs is None:\n"
"        raise TypeError('must be called with a dataclass type or instance')\n"
"    return tuple(fs.values())\n"
"def _asdict(v, factory):\n"
"    if is_dataclass(v) and not isinstance(v, type):\n"
"        return factory([(f.name, _asdict(getattr(v, f.name), factory)) for f in fields(v)])\n"
"    if isinstance(v, list):\n"
"        return [_asdict(x, factory) for x in v]\n"
"    if isinstance(v, tuple):\n"
"        return tuple([_asdict(x, factory) for x in v])\n"
"    if isinstance(v, dict):\n"
"        return {_asdict(k, factory): _asdict(x, factory) for k, x in v.items()}\n"
"    return v\n"
"def asdict(obj, *, dict_factory=dict):\n"
"    if not is_dataclass(obj) or isinstance(obj, type):\n"
"        raise TypeError('asdict() should be called on dataclass instances')\n"
"    return _asdict(obj, dict_factory)\n"
"def _astuple(v):\n"
"    if is_dataclass(v) and not isinstance(v, type):\n"
"        return tuple([_astuple(getattr(v, f.name)) for f in fields(v)])\n"
"    if isinstance(v, list):\n"
"        return [_astuple(x) for x in v]\n"
"    if isinstance(v, tuple):\n"
"        return tuple([_astuple(x) for x in v])\n"
"    if isinstance(v, dict):\n"
"        return {_astuple(k): _astuple(x) for k, x in v.items()}\n"
"    return v\n"
"def astuple(obj, *, tuple_factory=tuple):\n"
"    if not is_dataclass(obj) or isinstance(obj, type):\n"
"        raise TypeError('astuple() should be called on dataclass instances')\n"
"    return tuple_factory(_astuple(obj))\n"
"def replace(obj, /, **changes):\n"
"    if not is_dataclass(obj) or isinstance(obj, type):\n"
"        raise TypeError('replace() should be called on dataclass instances')\n"
"    for f in fields(obj):\n"
"        if not f.init and f.name in changes:\n"
"            raise ValueError(\"field \" + f.name + \" is declared with init=False, it cannot be specified with replace()\")\n"
"    args = {}\n"
"    for f in fields(obj):\n"
"        if f.init:\n"
"            args[f.name] = changes.pop(f.name) if f.name in changes else getattr(obj, f.name)\n"
"    for k in changes:\n"
"        args[k] = changes[k]\n"
"    return obj.__class__(**args)\n";

/* BaseExceptionGroup's methods (installed on the built-in types) and except*'s steps */
static const char SRC_EXCGROUP[]=
"def _new(cls, message, exceptions, /):\n"
"    if not isinstance(message, str):\n"
"        raise TypeError(\"argument 1 must be str, not \" + type(message).__name__)\n"
"    try:\n"
"        excs = tuple(exceptions)\n"
"    except TypeError:\n"
"        raise TypeError(\"second argument (exceptions) must be a sequence\")\n"
"    if not excs:\n"
"        raise ValueError(\"second argument (exceptions) must be a non-empty sequence\")\n"
"    for i in range(len(excs)):\n"
"        if not isinstance(excs[i], BaseException):\n"
"            raise ValueError(\"Item \" + str(i) + \" of second argument (exceptions) is not an exception\")\n"
"    if cls is BaseExceptionGroup:\n"
"        plain = True\n"
"        for e in excs:\n"
"            if not isinstance(e, Exception):\n"
"                plain = False\n"
"        if plain:\n"
"            cls = ExceptionGroup\n"
"    elif issubclass(cls, Exception):\n"
"        for e in excs:\n"
"            if not isinstance(e, Exception):\n"
"                if cls is ExceptionGroup:\n"
"                    raise TypeError(\"Cannot nest BaseExceptions in an ExceptionGroup\")\n"
"                raise TypeError(\"Cannot nest BaseExceptions in '\" + cls.__name__ + \"'\")\n"
"    self = BaseException.__new__(cls, message, exceptions)\n"
"    self._excs = excs\n"
"    return self\n"
"def _str(self):\n"
"    n = len(self._excs)\n"
"    return self.args[0] + \" (\" + str(n) + \" sub-exception\" + (\"s\" if n > 1 else \"\") + \")\"\n"
"def _message(self):\n"
"    return self.args[0]\n"
"def _exceptions(self):\n"
"    return self._excs\n"
"def _derive(self, excs):\n"
"    return BaseExceptionGroup(self.args[0], excs)\n"
"def _pred(cond):\n"
"    if isinstance(cond, type) and issubclass(cond, BaseException):\n"
"        return lambda e: isinstance(e, cond)\n"
"    if isinstance(cond, tuple):\n"
"        for c in cond:\n"
"            if not (isinstance(c, type) and issubclass(c, BaseException)):\n"
"                raise TypeError(\"expected an exception type, a tuple of exception types, or a callable (other than a class)\")\n"
"        return lambda e: isinstance(e, cond)\n"
"    if callable(cond) and not isinstance(cond, type):\n"
"        return cond\n"
"    raise TypeError(\"expected an exception type, a tuple of exception types, or a callable (other than a class)\")\n"
"def _copy_meta(src, dst):\n"
"    dst.__cause__ = src.__cause__\n"
"    dst.__context__ = src.__context__\n"
"    dst.__traceback__ = src.__traceback__\n"
"    notes = getattr(src, '__notes__', None)\n"
"    if notes is not None:\n"
"        dst.__notes__ = list(notes)\n"
"def _split_pred(self, pred):\n"
"    if pred(self):\n"
"        return (self, None)\n"
"    match = []\n"
"    rest = []\n"
"    for e in self._excs:\n"
"        if isinstance(e, BaseExceptionGroup):\n"
"            m, r = _split_pred(e, pred)\n"
"            if m is not None:\n"
"                match.append(m)\n"
"            if r is not None:\n"
"                rest.append(r)\n"
"        elif pred(e):\n"
"            match.append(e)\n"
"        else:\n"
"            rest.append(e)\n"
"    m = None\n"
"    r = None\n"
"    if match:\n"
"        m = self.derive(match)\n"
"        _copy_meta(self, m)\n"
"    if rest:\n"
"        r = self.derive(rest)\n"
"        _copy_meta(self, r)\n"
"    return (m, r)\n"
"def _split(self, cond):\n"
"    return _split_pred(self, _pred(cond))\n"
"def _subgroup(self, cond):\n"
"    return _split_pred(self, _pred(cond))[0]\n"
"def _leaves(e, out):\n"
"    if isinstance(e, BaseExceptionGroup):\n"
"        for x in e._excs:\n"
"            _leaves(x, out)\n"
"    else:\n"
"        out.append(e)\n"
"    return out\n"
"# except*: the state is [original, rest, raised, naked, matched]\n"
"def _begin(exc):\n"
"    return [exc, exc, [], not isinstance(exc, BaseExceptionGroup), None]\n"
"def _match(state, cond):\n"
"    if isinstance(cond, type) and issubclass(cond, BaseExceptionGroup):\n"
"        raise TypeError(\"catching ExceptionGroup with except* is not allowed. Use except instead.\")\n"
"    if isinstance(cond, tuple):\n"
"        for c in cond:\n"
"            if isinstance(c, type) and issubclass(c, BaseExceptionGroup):\n"
"                raise TypeError(\"catching ExceptionGroup with except* is not allowed. Use except instead.\")\n"
"    rest = state[1]\n"
"    if rest is None:\n"
"        state[4] = None\n"
"        return None\n"
"    if state[3]:\n"
"        if not _pred(cond)(rest):\n"
"            state[4] = None\n"
"            return None\n"
"        m = BaseExceptionGroup(\"\", (rest,))\n"
"        m.__traceback__ = rest.__traceback__\n"
"        state[1] = None\n"
"        state[4] = m\n"
"        return m\n"
"    m, r = rest.split(cond)\n"
"    state[1] = r\n"
"    state[4] = m\n"
"    return m\n"
"def _raised(state, x):\n"
"    state[2].append((x, x is state[4]))\n"
"def _end(state):\n"
"    orig = state[0]\n"
"    keep = []\n"
"    if state[1] is not None:\n"
"        if state[3]:\n"
"            keep.append(state[1])\n"
"        else:\n"
"            _leaves(state[1], keep)\n"
"    new = []\n"
"    for x, again in state[2]:\n"
"        if again:\n"
"            _leaves(x, keep)\n"
"        else:\n"
"            new.append(x)\n"
"    result = None\n"
"    if keep:\n"
"        if state[3]:\n"
"            result = orig\n"
"        else:\n"
"            ids = [id(e) for e in keep]\n"
"            result = _split_pred(orig, lambda e: id(e) in ids)[0]\n"
"    if new:\n"
"        if result is not None:\n"
"            new.append(result)\n"
"        if len(new) == 1 and result is None:\n"
"            return new[0]\n"
"        return BaseExceptionGroup(\"\", new)\n"
"    return result\n";

/* string and string.templatelib (t-strings) */
static const char SRC_TEMPLATELIB[]=
"class Interpolation:\n"
"    __match_args__ = ('value', 'expression', 'conversion', 'format_spec')\n"
"    def __init__(self, value, expression='', conversion=None, format_spec=''):\n"
"        self.value = value\n"
"        self.expression = expression\n"
"        self.conversion = conversion\n"
"        self.format_spec = format_spec\n"
"    def __repr__(self):\n"
"        return 'Interpolation(' + repr(self.value) + ', ' + repr(self.expression) + ', ' + repr(self.conversion) + ', ' + repr(self.format_spec) + ')'\n"
"class Template:\n"
"    def __init__(self, *args):\n"
"        strings = []\n"
"        interps = []\n"
"        last_str = False\n"
"        for a in args:\n"
"            if isinstance(a, str):\n"
"                if last_str:\n"
"                    strings[-1] = strings[-1] + a\n"
"                else:\n"
"                    strings.append(a)\n"
"                last_str = True\n"
"            elif isinstance(a, Interpolation):\n"
"                if not last_str:\n"
"                    strings.append('')\n"
"                interps.append(a)\n"
"                last_str = False\n"
"            else:\n"
"                raise TypeError('Template.__new__ *args need to be of type \\'str\\' or \\'Interpolation\\', got ' + type(a).__name__)\n"
"        if not last_str:\n"
"            strings.append('')\n"
"        self.strings = tuple(strings)\n"
"        self.interpolations = tuple(interps)\n"
"    @property\n"
"    def values(self):\n"
"        return tuple([i.value for i in self.interpolations])\n"
"    def __iter__(self):\n"
"        out = []\n"
"        for i in range(len(self.strings)):\n"
"            if self.strings[i]:\n"
"                out.append(self.strings[i])\n"
"            if i < len(self.interpolations):\n"
"                out.append(self.interpolations[i])\n"
"        return iter(out)\n"
"    def __repr__(self):\n"
"        return 'Template(strings=' + repr(self.strings) + ', interpolations=' + repr(self.interpolations) + ')'\n"
"    def __add__(self, other):\n"
"        if isinstance(other, Template):\n"
"            return Template(*(list(self._parts()) + list(other._parts())))\n"
"        if isinstance(other, str):\n"
"            raise TypeError('can only concatenate string.templatelib.Template (not \"str\") to string.templatelib.Template')\n"
"        return NotImplemented\n"
"    def __radd__(self, other):\n"
"        if isinstance(other, str):\n"
"            raise TypeError('can only concatenate str (not \"string.templatelib.Template\") to str')\n"
"        return NotImplemented\n"
"    def _parts(self):\n"
"        out = []\n"
"        for i in range(len(self.strings)):\n"
"            out.append(self.strings[i])\n"
"            if i < len(self.interpolations):\n"
"                out.append(self.interpolations[i])\n"
"        return out\n"
"def _from_parts(parts):\n"
"    args = []\n"
"    for p in parts:\n"
"        if isinstance(p, tuple):\n"
"            args.append(Interpolation(p[0], p[1], p[2], p[3]))\n"
"        else:\n"
"            args.append(p)\n"
"    return Template(*args)\n";

/* cmath (complex is built in) */
static const char SRC_CMATH[]=
"import math\n"
"\n"
"pi = math.pi\n"
"e = math.e\n"
"tau = math.tau\n"
"inf = math.inf\n"
"nan = math.nan\n"
"\n"
"infj = complex(0.0, math.inf)\n"
"nanj = complex(0.0, math.nan)\n"
"\n"
"def phase(z):\n"
"    return math.atan2(z.imag, z.real)\n"
"\n"
"def polar(z):\n"
"    return (math.hypot(z.real, z.imag), math.atan2(z.imag, z.real))\n"
"\n"
"def rect(r: float, phi: float):\n"
"    return complex(r * math.cos(phi), r * math.sin(phi))\n"
"\n"
"def sqrt(z):\n"
"    if z.real == 0.0 and z.imag == 0.0:\n"
"        return complex(0.0, z.imag)\n"
"    ax = abs(z.real) / 8.0\n"
"    s = 2.0 * math.sqrt(ax + math.hypot(ax, abs(z.imag) / 8.0))\n"
"    d = abs(z.imag) / (2.0 * s)\n"
"    if z.real >= 0.0:\n"
"        return complex(s, math.copysign(d, z.imag))\n"
"    return complex(d, math.copysign(s, z.imag))\n"
"\n"
"def exp(z):\n"
"    l = math.exp(z.real)\n"
"    return complex(l * math.cos(z.imag), l * math.sin(z.imag))\n"
"\n"
"def log(z):\n"
"    return complex(math.log(math.hypot(z.real, z.imag)), math.atan2(z.imag, z.real))\n"
"\n"
"def log10(z):\n"
"    r = log(z)\n"
"    return complex(r.real / math.log(10.0), r.imag / math.log(10.0))\n"
"\n"
"def isfinite(z):\n"
"    return math.isfinite(z.real) and math.isfinite(z.imag)\n"
"\n"
"def isinf(z):\n"
"    return math.isinf(z.real) or math.isinf(z.imag)\n"
"\n"
"def isnan(z):\n"
"    return math.isnan(z.real) or math.isnan(z.imag)\n";

static const char SRC_FUTURE[]=
"class _Feature:\n    pass\nannotations = _Feature()\ngenerator_stop = None\ndivision = None\nprint_function = None\nabsolute_import = None\nunicode_literals = None\n";

static const char *py_source(const char *name){
    if(!strcmp(name,"asyncio")) return SRC_ASYNCIO;
    if(!strcmp(name,"minipy")) return SRC_MINIPY;
    if(!strcmp(name,"dataclasses")) return SRC_DATACLASSES;
    if(!strcmp(name,"_excgroup")) return SRC_EXCGROUP;
    if(!strcmp(name,"cmath")) return SRC_CMATH;
    if(!strcmp(name,"string.templatelib")) return SRC_TEMPLATELIB;
    if(!strcmp(name,"__future__")) return SRC_FUTURE;
    return NULL;
}
/* BaseExceptionGroup's behavior: from the _excgroup module, the first time a group is made */
static Value excgroup_mod;
Value mp_excgroup(const char *fn){
    if(excgroup_mod.k!=V_OBJ){
        Value nm=mp_str("_excgroup"); excgroup_mod=mp_builtin_import(1,&nm,NULL); mp_gc_add_root(&excgroup_mod);
        DictObj *m=mdict(excgroup_mod), *d=E_BaseExceptionGroup->dict; Value v;
        E_BaseExceptionGroup->flags|=TF_DUNDERS; E_ExceptionGroup->flags|=TF_DUNDERS;      /* (__str__ written in Python) */
        mp_dict_get_s(m,"_new",&v); BoxObj *sm=(BoxObj*)mp_alloc(T_staticmethod,sizeof(BoxObj)); sm->v=v; mp_dict_set_s(d,"__new__",v_obj(sm));
        mp_dict_get_s(m,"_str",&v); mp_dict_set_s(d,"__str__",v);
        mp_dict_get_s(m,"_derive",&v); mp_dict_set_s(d,"derive",v);
        mp_dict_get_s(m,"_split",&v); mp_dict_set_s(d,"split",v);
        mp_dict_get_s(m,"_subgroup",&v); mp_dict_set_s(d,"subgroup",v);
        const char *props[2][2]={{"message","_message"},{"exceptions","_exceptions"}};
        for(int i=0;i<2;i++){ mp_dict_get_s(m,props[i][1],&v); PropertyObj *p=(PropertyObj*)mp_alloc(T_property,sizeof(PropertyObj)); p->get=v; p->set=p->del=p->doc=v_none(); mp_dict_set_s(d,props[i][0],v_obj(p)); }
    }
    Value f; if(!fn || !mp_dict_get_s(mdict(excgroup_mod),fn,&f)) return v_none();
    return f;
}
/* gc: the interpreter's collector (cycles; reference counting is not used) */
extern int mp_gc_enabled;
int64_t mp_gc_objects(void);
static Value gc_enable(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; no_kw("enable",kw); mp_gc_enabled=1; return v_none(); }
static Value gc_disable(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; no_kw("disable",kw); mp_gc_enabled=0; return v_none(); }
static Value gc_isenabled(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; no_kw("isenabled",kw); return v_bool(mp_gc_enabled); }
static Value gc_collect(int argc, Value *argv, TupleObj *kw){ (void)argv; (void)kw; nargs("collect",argc,0,1);
    int64_t before=mp_gc_objects(); mp_gc_collect(); int64_t after=mp_gc_objects(); return v_int(before>after?before-after:0); }
static Value gc_get_count(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; Value t[3]={v_int(mp_gc_objects()%700),v_int(0),v_int(0)}; return mp_tuple(3,t); }
static Value gc_get_threshold(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; Value t[3]={v_int(2000),v_int(10),v_int(0)}; return mp_tuple(3,t); }
static Value gc_none(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; return v_none(); }
static Value gc_zero(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; return v_int(0); }
static Value gc_true(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; return v_bool(1); }
static Value gc_false(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; return v_bool(0); }
static Value gc_list(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; return mp_list(0,NULL); }
static Value make_gc(void){
    Value m=register_module("gc");
    set_fn(m,"enable",gc_enable); set_fn(m,"disable",gc_disable); set_fn(m,"isenabled",gc_isenabled); set_fn(m,"collect",gc_collect);
    set_fn(m,"get_count",gc_get_count); set_fn(m,"get_threshold",gc_get_threshold); set_fn(m,"set_threshold",gc_none);
    set_fn(m,"get_debug",gc_zero); set_fn(m,"set_debug",gc_none); set_fn(m,"freeze",gc_none); set_fn(m,"unfreeze",gc_none);
    set_fn(m,"get_freeze_count",gc_zero); set_fn(m,"is_tracked",gc_true); set_fn(m,"is_finalized",gc_false);
    set_fn(m,"get_objects",gc_list); set_fn(m,"get_referrers",gc_list); set_fn(m,"get_referents",gc_list);
    mp_dict_set_s(mdict(m),"garbage",mp_list(0,NULL)); mp_dict_set_s(mdict(m),"callbacks",mp_list(0,NULL));
    static const char *const dn[]={"DEBUG_STATS","DEBUG_COLLECTABLE","DEBUG_UNCOLLECTABLE","DEBUG_SAVEALL","DEBUG_LEAK"}; static const int dv[]={1,2,4,32,38};
    for(int i=0;i<5;i++) mp_dict_set_s(mdict(m),dn[i],v_int(dv[i]));
    return m;
}
static Value native_module(const char *name){
    if(!strcmp(name,"sys")) return make_sys();
    if(!strcmp(name,"math")) return make_math();
    if(!strcmp(name,"_mpy_json")) return make_json();
    if(!strcmp(name,"thread")) return make_thread();
    if(!strcmp(name,"_thread")) return make__thread();
    if(!strcmp(name,"gc")) return make_gc();
    if(!strcmp(name,"_ctypes")) return make_ctypes_native();
    if(!strcmp(name,"builtins")){ Value m=register_module("builtins"); ((ModuleObj*)m.u.o)->dict=mp_builtins; return m; }
    return v_undef();
}
void mp_modules_init(void){
    native_module("builtins");
}
