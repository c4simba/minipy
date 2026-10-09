/* ========================= Entry point ========================= */

#include "platform/platform.h"
#include "interp.h"
#include "fs.h"
#include "aot.h"
#include "py_ast.h"

#ifndef MPY_VERSION
#define MPY_VERSION "0.1"
#endif

/* Split a raw command-line string into argv (argv[0] = program name/path).
   Tokenizes on whitespace into the caller's buffer; returns argc. Used on
   platforms (KolibriOS) that hand over a single string instead of argv. */
static int mpy_split_cmdline(const char *cmd,const char *exe,char *buf,int buflen,char **argv,int maxargv){
    int argc=0;
    argv[argc++]=(char*)((exe && exe[0]) ? exe : "minipy");   /* argv[0], read-only */
    int n=0; if(cmd){ while(cmd[n] && n<buflen-1){ buf[n]=cmd[n]; n++; } } buf[n]=0;
    char *p=buf;
    while(*p && argc<maxargv){
        while(*p==' '||*p=='\t') p++;
        if(!*p) break;
        argv[argc++]=p;
        while(*p && *p!=' ' && *p!='\t') p++;
        if(*p) *p++=0;
    }
    return argc;
}

/* The program's exit status for an exception that ended it (SystemExit: its code). */
static int exit_status(Value e){
    if(!mp_isinstance(e,E_SystemExit)){ mp_print_exception(e); return 1; }
    Value code=mp_getattr_s(e,"code");
    if(IS_NONE(code)) return 0;
    if(IS_INTLIKE(code)) return (int)code.u.i;
    Value s=mp_tostr(code);
    mp_write_err(mp_cstr(s),AS_STR(s)->len); mp_write_err("\n",1);
    return 1;
}

/* All program logic. Every exit path RETURNS a status code so main() can
   guarantee platform teardown (closing the KolibriOS console buffer) on all of
   them -- including -v, usage, and error paths. */
static int mpy_run(int argc,char **argv){
    const char *script_path = NULL;
    const char *program = (argc>0 && argv && argv[0]) ? argv[0] : "minipy";

    if(argc>=2 && (strcmp(argv[1],"-v")==0 || strcmp(argv[1],"--version")==0)){
        printf("minipy %s (%s)\n",MPY_VERSION,mpy_fs_backend_name());
        return 0;
    }

    /* Typed ahead-of-time compilation to a standalone executable (fasm). */
    if(argc>=2 && strcmp(argv[1],"--compile")==0) return aot_main(argc-2,argv+2,program);
    /* The full Python parser: its tree, as CPython's ast (tests/pyast_check.py) */
    if(argc>=2 && (strcmp(argv[1],"--pyast")==0 || strcmp(argv[1],"--dump-ast")==0)) return py_dump_main(argc-2,argv+2);

    if(argc<2){
        script_path = mpy_platform_default_script();
        if(!script_path){
            fprintf(stderr,"usage: %s [-v|--version] [--dump-ast|--dump-bytecode|--fs-info] file.py [args...]\n",program);
            fprintf(stderr,"       %s --compile [options] file.py   (see --compile --help)\n",program);
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
    int rc=0;
    Catch c;
    if(!CATCH_BEGIN(c)){ mp_run_main(script_path); CATCH_END(c); }
    else rc=exit_status(mp_catch_exc(&c));
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
    if(kcmd){ argc=mpy_split_cmdline(kcmd,mpy_platform_exe_path(),cmdbuf,(int)sizeof(cmdbuf),kargv,32); argv=kargv; }
    mpy_fs_set_program(argv[0]);          /* modules are also looked for in <minipy's folder>/lib */

    int rc = mpy_run(argc,argv);
    mpy_platform_shutdown();          /* closes the console on every return from mpy_run */
    return rc;
}
