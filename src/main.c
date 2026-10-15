/* ========================= Entry point ========================= */

#include "platform/platform.h"
#include "interp/interp.h"
#include "fs.h"
#include "py_ast.h"

#include "version.h"                  /* MPY_VERSION: make writes it (build/gen) from VERSION */

/* The program's exit status for an exception that ended it (SystemExit: its code). */
static int exit_status(Value e){
    if(!mp_isinstance(e,E_SystemExit)){
        Value sysm, hook;                                         /* a sys.excepthook of the program's own */
        if(mp_dict_get(mp_modules,mp_str("sys"),&sysm) && mp_dict_get(((ModuleObj*)sysm.u.o)->dict,mp_str("excepthook"),&hook) && !IS(hook,T_native)){
            Catch c;
            if(!CATCH_BEGIN(c)){ Value tb=AS_EXC(e)->tb; Value a[3]={v_obj(TYPE(e)),e,tb.k==V_UNDEF?v_none():tb}; mp_call(hook,3,a,NULL); CATCH_END(c); return 1; }
            else { mp_catch_exc(&c); } }
        mp_print_exception(e); return 1; }
    Value code=mp_getattr_s(e,"code");
    if(IS_NONE(code)) return 0;
    if(IS_INTLIKE(code)) return (int)code.u.i;
    Value s=mp_tostr(code);
    mp_write_err(mp_cstr(s),AS_STR(s)->len); mp_write_err("\n",1);
    return 1;
}

/* `minipy add requests`, `minipy sync` ...: the package manager, written in
   Python (src/stdlib/_mpy_pkg), run by the interpreter. */
static const char *const pkg_commands[]={"add","remove","lock","sync","check","list","vendor",NULL};
static int is_pkg_command(const char *a){
    for(int i=0;pkg_commands[i];i++) if(!strcmp(a,pkg_commands[i])) return !mpy_fs_exists(a);   /* (not a script of that name) */
    return 0;
}
static int run_pkg_command(int argc,char **argv){
    mp_init();
    mp_set_argv(argc,argv);                                   /* sys.argv: minipy, the command, its arguments */
    int rc=0;
    Catch c;
    if(!CATCH_BEGIN(c)){ Value m=mp_import("_mpy_pkg",NULL,0,1); mp_call0(mp_getattr_s(m,"main")); CATCH_END(c); }
    else rc=exit_status(mp_catch_exc(&c));
    mp_flush_stdout();
    return rc;
}

/* `minipy --compile ...`: the typed compiler is a program of its own, minipyc
   (next to minipy, else on the PATH); this process becomes it. */
static int run_compiler(int argc, char **argv, const char *program){
    const char *dir=mpy_fs_program_dir();
    char *path= dir ? mpy_fs_join_path(dir,"minipyc") : xstrdup2("minipyc");
    char **v=(char**)xmalloc(sizeof(char*)*(size_t)(argc+2));
    v[0]=path; for(int i=0;i<argc;i++) v[i+1]=argv[i]; v[argc+1]=NULL;
    fflush(stdout);
    int rc=mpy_platform_exec(path,v);
    if(rc<0){ fprintf(stderr,"%s: cannot run the compiler, %s (it is built with minipy: make)\n",program,path); return 127; }
    return rc;
}

/* All program logic. Every exit path RETURNS a status code so main() can
   guarantee platform teardown (closing the KolibriOS console buffer) on all of
   them -- including -v, usage, and error paths. */
static int mpy_run(int argc,char **argv){
    const char *volatile script_path = NULL;               /* (volatile: kept across the longjmps below) */
    const char *program = (argc>0 && argv && argv[0]) ? argv[0] : "minipy";

    if(argc>=2 && (strcmp(argv[1],"-v")==0 || strcmp(argv[1],"--version")==0)){
        printf("minipy %s (%s)\n",MPY_VERSION,mpy_fs_backend_name());
        return 0;
    }

    /* Typed ahead-of-time compilation to a standalone executable: minipyc. */
    if(argc>=2 && strcmp(argv[1],"--compile")==0) return run_compiler(argc-2,argv+2,program);
    /* The full Python parser: its tree, as CPython's ast (tests/pyast_check.py) */
    if(argc>=2 && (strcmp(argv[1],"--pyast")==0 || strcmp(argv[1],"--dump-ast")==0)) return py_dump_main(argc-2,argv+2);
    /* The package manager: minipy add | remove | lock | sync | check | list | vendor */
    if(argc>=2 && is_pkg_command(argv[1])) return run_pkg_command(argc,argv);

    if(argc<2){
        script_path = mpy_platform_default_script();
        if(!script_path){
            fprintf(stderr,"usage: %s [-v|--version] [--dump-ast|--dump-bytecode|--fs-info] file.py [args...]\n",program);
            fprintf(stderr,"       %s --compile [options] file.py   (see --compile --help)\n",program);
            fprintf(stderr,"       %s add|remove|lock|sync|check|list|vendor ...   (packages of PyPI: %s add --help)\n",program,program);
            return 2;
        }
    }
    if(!script_path && strcmp(argv[1],"--fs-info")==0){
        printf("%s\n",mpy_fs_backend_name());
        return 0;
    }
    mp_init();
    if(!script_path && strcmp(argv[1],"--dump-bytecode")==0){
        if(argc<3){ fprintf(stderr,"usage: %s %s file.py\n",program,argv[1]); return 2; }
        char *src=mpy_fs_read_file(argv[2]);
        PyParse pp;
        if(py_parse(&pp,argv[2],src,strlen(src))){ fprintf(stderr,"%s:%d: SyntaxError: %s\n",argv[2],pp.error_line,pp.error); return 1; }
        char *err=NULL; int line=0;
        CodeObj *co=mp_compile(pp.mod,argv[2],"__main__",&err,&line);
        if(!co){ fprintf(stderr,"%s:%d: SyntaxError: %s\n",argv[2],line,err); return 1; }
        mp_dump_code(co);
        free(src);
        return 0;
    }
    int first=1;
    if(!script_path) script_path = argv[1];
    else first=0;
    mpy_platform_banner(script_path);
    mp_set_argv(argc-first,argv+first);
    { char *dir=mpy_fs_dirname(script_path); mp_main_dir=dir; }
    mpy_fs_use_project(script_path);                          /* (its packages: <project>/.minipy/site) */
    volatile int rc=0;
    Catch c;
    if(!CATCH_BEGIN(c)){ mp_run_main(script_path); CATCH_END(c); }
    else rc=exit_status(mp_catch_exc(&c));
    const char *hooks[][2]={{"threading","_shutdown"},{"atexit","_run_exitfuncs"}};   /* (as CPython ends: non-daemon threads joined, then atexit) */
    for(int k=0;k<2;k++){ Value mod;
        if(!mp_dict_get(mp_modules,mp_str(hooks[k][0]),&mod)) continue;
        Catch h;
        if(!CATCH_BEGIN(h)){ mp_call0(mp_getattr_s(mod,hooks[k][1])); CATCH_END(h); }
        else { int r=exit_status(mp_catch_exc(&h)); if(!rc) rc=r; } }
    mp_flush_stdout();
    return rc;
}

int main(int argc,char **argv){
    mpy_platform_init();
    atexit(mpy_platform_shutdown);   /* closes the console on exit() paths */
    static Thread main_thread;
    mpy_lock_init(&mp_gil);
    mpy_lock_acquire(&mp_gil);           /* main holds the GIL while it runs */
    mp_ts=&main_thread;
    mp_thread_add(&main_thread);
    main_thread.cstack_base=&argc;

    /* KolibriOS delivers the launch arguments as one header string, not argv;
       rebuild argc/argv from it so all the option handling just works. */
    char cmdbuf[256]; char *kargv[32];
    const char *kcmd = mpy_platform_cmdline();
    if(kcmd){ argc=mpy_split_cmdline(kcmd,mpy_platform_exe_path(),"minipy",cmdbuf,(int)sizeof(cmdbuf),kargv,32); argv=kargv; }
    mpy_fs_set_program(argv[0]);          /* modules are also looked for in <minipy's folder>/lib */

    int rc = mpy_run(argc,argv);
    mpy_platform_shutdown();          /* closes the console on every return from mpy_run */
    return rc;
}
