/* ========================= AOT driver: plainpyc (plainpy --compile) =========================
   Options, then the import pass, the listing (aot_compile), and the target's
   build step (src/targets: fasm, kpack, the C translation and a C compiler,
   an ESP-IDF project). */

#include "driver.h"
#include "targets/targets.h"

static void usage(const char *program){
    fprintf(stderr,
        "usage: %s [options] file.py   (plainpy --compile [options] file.py)\n"
        "  -o FILE          output (default: the script name without .py)\n"
        "  --target T       what to make (default: for the system plainpy runs on):\n",program);
    aot_targets_help(stderr);
    fprintf(stderr,
        "  -S               only write the assembly listing (FILE.asm)\n"
        "  --fasm PATH      fasm executable                       [env PPY_FASM]\n"
        "  --fasm-args A    fasm arguments, {in} {out} {dir} replaced  [env PPY_FASM_ARGS]\n"
        "  --cc PATH        macos, c: C compiler                  [env PPY_CC]\n"
        "  --cc-args A      its arguments, {in} {out} {dir} replaced  [env PPY_CC_ARGS]\n"
        "  --port P         c: also a port: posix (FILE.c built for this computer),\n"
        "                   template (ppy_port_template.c to fill in for a board)\n"
        "  --chip C         esp32: esp32, esp32s2, esp32s3, esp32c3, esp32c6, ...\n"
        "  --build, --no-build  esp32: run idf.py build (default: when ESP-IDF is found)\n"
        "  --arena BYTES    c, esp32: the memory the program asks the port for\n"
        "  --stack BYTES    kolibri, c, esp32: the main stack\n"
        "  --costack BYTES  the stack of each generator and task\n"
        "  --chunk BYTES    memory the allocator takes from the system at a time\n"
        "  --no-pack        kolibri: leave the executable unpacked (by default it is\n"
        "                   LZMA-packed as kpack does it; KolibriOS unpacks it on loading)\n"
        "  -v               print the commands being run\n"
        "  --count-allocs   debugging: the program reports its live heap blocks at exit\n"
        "  --check MODULE.. check modules (packages) for compiled programs instead:\n"
        "                   every error, and the functions it cannot check (`plainpy check`)\n"
        "Compiled programs are statically typed: every variable, parameter, field and\n"
        "container element keeps one type (annotated or inferred), None is the zero\n"
        "value of that type, and imports must be at the top of a module.\n");
}

static char *strip_ext(const char *path){
    size_t n=strlen(path); const char *slash=strrchr(path,'/'), *dot=strrchr(path,'.');
    if(dot && (!slash || dot>slash) && strcmp(dot,".py")==0) n=(size_t)(dot-path);
    return xstrndup2(path,(int)n);
}

/* ---------------------------------------------------------------- entry */

/* Step 1: the entry script and, transitively, its imports, and the library
   modules a program needs without importing them. 0, or 1 after an error
   (check mode: a module whose source or imports are rejected is marked as
   failed and left out; the others go on). */
static int scan_unit(Units *us, AotUnit *u){
    if(scan_imports(us,u)) return 1;
    if(!aot_check_mode) return 0;
    u->failed=1; u->ast=NULL; aot_check_errors++;
    return 1;
}
int load_program(Units *us, const char *src_path, char *src){
    add_unit(us,"__main__",src_path,src);
    if(uses_complex(src)) add_unit(us,"cmath","<cmath>",xstrdup2(CMATH_PRELUDE));   /* (complex) */
    for(int i=0;i<us->n;i++) if(us->v[i]->ast && !scan_unit(us,us->v[i])) return 1;
    if(!find_unit(us,"cmath")) for(int i=0;i<us->n;i++) if(us->v[i]->src && uses_complex(us->v[i]->src)){
        add_unit(us,"cmath","<cmath>",xstrdup2(CMATH_PRELUDE)); if(!scan_unit(us,us->v[us->n-1])) return 1; break; }
    { int any=0; for(int i=0;i<us->n && !any;i++) if(us->v[i]->src && strstr(us->v[i]->src,"open(")) any=1;       /* open(): io.py (first: what it uses comes next) */
      if(any){ if(!find_unit(us,"io")){ int pkg; add_unit(us,"io","<stdlib>/io.py",xstrdup2(ppy_stdlib_source("io",&pkg)));
                   for(int i=0;i<us->n;i++) if(us->v[i]->ast && !scan_unit(us,us->v[i])) return 1; }
               main_imports(us->v[0],"io"); } }
    { int any=0, more=0;                                                    /* encode() / decode(): codecs.py; other encodings than */
      for(int i=0;i<us->n;i++) if(us->v[i]->src){                            /* utf-8 / ascii / latin-1 named: _codecs_more.py too */
          const char *t=us->v[i]->src;
          if(strstr(t,"encode(") || strstr(t,"decode(") || strstr(t,"str(") || strstr(t,"bytes(") || strstr(t,"ascii(") || strstr(t,"!a}") || strstr(t,"!a:")) any=1;   /* (ascii(x): repr(x).encode("ascii", "backslashreplace")) */
          if(us->v[i]->path && !strncmp(us->v[i]->path,"<stdlib>",8)) continue;
          if(names_codec(t)) more=1; }
      if(any || more){ int pkg;
          if(!find_unit(us,"codecs")) add_unit(us,"codecs","<stdlib>/codecs.py",xstrdup2(ppy_stdlib_source("codecs",&pkg)));
          main_imports(us->v[0],"codecs");
          if(more && !find_unit(us,"_codecs_more")){ add_unit(us,"_codecs_more","<stdlib>/_codecs_more.py",xstrdup2(ppy_stdlib_source("_codecs_more",&pkg))); main_imports(us->v[0],"_codecs_more"); } } }
    { int any=0; for(int i=0;i<us->n && !any;i++) if(us->v[i]->src && strcmp(us->v[i]->name,"_sysio") && (strstr(us->v[i]->src,".stdout") || strstr(us->v[i]->src,".stdin") ||
                                                                strstr(us->v[i]->src,".__std") || has_stderr_value(us->v[i]->src))) any=1;     /* sys.stdout ...: _sysio.py */
      if(any && !find_unit(us,"_sysio")){ int pkg; add_unit(us,"_sysio","<stdlib>/_sysio.py",xstrdup2(ppy_stdlib_source("_sysio",&pkg)));
          if(!scan_unit(us,us->v[us->n-1])) return 1; main_imports(us->v[0],"_sysio"); } }
    { int eg=0; for(int i=0;i<us->n;i++) if(us->v[i]->src && (strstr(us->v[i]->src,"ExceptionGroup") || strstr(us->v[i]->src,"except*") || strstr(us->v[i]->src,"except *"))) eg=1;
      if(eg) add_unit(us,"__ppy_eg","<exceptiongroup>",xstrdup2(EG_PRELUDE)); }      /* ExceptionGroup, except*: written in Python */
    { static const char *const oserr[]={"OSError","IOError","EnvironmentError","FileNotFoundError","FileExistsError","PermissionError","IsADirectoryError",
          "NotADirectoryError","ConnectionError","ConnectionRefusedError","ConnectionResetError","ConnectionAbortedError","BrokenPipeError","BlockingIOError",
          "TimeoutError","InterruptedError","ProcessLookupError","ChildProcessError","UnicodeDecodeError","UnicodeEncodeError",NULL};
      int any=0; for(int i=0;i<us->n && !any;i++) if(us->v[i]->src) for(int k=0;oserr[k];k++) if(strstr(us->v[i]->src,oserr[k])){ any=1; break; }
      if(any && !find_unit(us,"_oserror")){ int pkg; add_unit(us,"_oserror","<stdlib>/_oserror.py",xstrdup2(ppy_stdlib_source("_oserror",&pkg))); } }   /* OSError(errno, strerror) */
    { int any=0; for(int i=0;i<us->n && !any;i++) if(us->v[i]->src && strstr(us->v[i]->src,"math") && strcmp(us->v[i]->name,"_mathx")) any=1;   /* math's functions in Python: _mathx.py */
      if(any && !find_unit(us,"_mathx")){ int pkg; add_unit(us,"_mathx","<stdlib>/_mathx.py",xstrdup2(ppy_stdlib_source("_mathx",&pkg))); main_imports(us->v[0],"_mathx"); } }
    { int any=0; for(int i=0;i<us->n && !any;i++) if(us->v[i]->src && strcmp(us->v[i]->name,"_ppy_exit") && (strstr(us->v[i]->src,"SystemExit") || strstr(us->v[i]->src,"exit("))) any=1;   /* SystemExit's code: _ppy_exit.py */
      if(any && !find_unit(us,"_ppy_exit")){ int pkg; add_unit(us,"_ppy_exit","<stdlib>/_ppy_exit.py",xstrdup2(ppy_stdlib_source("_ppy_exit",&pkg))); if(!scan_unit(us,us->v[us->n-1])) return 1; } }
    { int any=0; for(int i=0;i<us->n && !any;i++) if(us->v[i]->src && strstr(us->v[i]->src,"json") && (strstr(us->v[i]->src,"load") || strstr(us->v[i]->src,"JSONDecodeError"))) any=1;   /* json.loads: _ppy_jsonr.py */
      if(any && !find_unit(us,"_ppy_jsonr")){ int pkg; add_unit(us,"_ppy_jsonr","<stdlib>/_ppy_jsonr.py",xstrdup2(ppy_stdlib_source("_ppy_jsonr",&pkg))); if(!scan_unit(us,us->v[us->n-1])) return 1;
          main_imports(us->v[0],"_ppy_jsonr"); } }
    { int any=0; for(int i=0;i<us->n && !any;i++) if(us->v[i]->src && strstr(us->v[i]->src,"__next__")) any=1;   /* iterator objects: _ppy_iter.py */
      if(any && !find_unit(us,"_ppy_iter")){ int pkg; add_unit(us,"_ppy_iter","<stdlib>/_ppy_iter.py",xstrdup2(ppy_stdlib_source("_ppy_iter",&pkg))); } }
    if(find_unit(us,"threading")) main_epilogue(us->v[0],"import threading as __ppy_threading\n__ppy_threading._shutdown()\n");   /* (non-daemon threads waited for) */
    if(find_unit(us,"atexit")) main_epilogue(us->v[0],"import atexit as __ppy_atexit\n__ppy_atexit._run_exitfuncs()\n");
    { int any=0; for(int i=0;i<us->n && !any;i++) if(us->v[i]->src && strstr(us->v[i]->src,"argv")) any=1;     /* sys.argv: _sysargs.py */
      if(any && !find_unit(us,"_sysargs")){ int pkg; add_unit(us,"_sysargs","<stdlib>/_sysargs.py",xstrdup2(ppy_stdlib_source("_sysargs",&pkg)));
          main_imports(us->v[0],"_sysargs"); } }
    { int ck=0; for(int i=0;i<us->n;i++) if(us->v[i]->src && strstr(us->v[i]->src,"cmp_to_key")) ck=1;
      if(ck){ add_unit(us,"__ppy_cmpkey","<cmp_to_key>",xstrdup2(CMPKEY_PRELUDE)); if(!scan_unit(us,us->v[us->n-1])) return 1; } }

    { AotUnit *ty=find_unit(us,"types");                          /* types: the program's SimpleNamespace class */
      if(ty && ty->src && !strstr(ty->src,"class SimpleNamespace:\n        \"\"\"An object whose attributes are the keyword arguments (those")){
          char *t=types_with_namespace(us,ty->src); ty->src=t; ty->ast=py_front(ty->path,t); if(!ty->ast) return 1; } }
    { AotUnit *op=find_unit(us,"optparse");                       /* optparse: the program's Values class */
      if(op && op->src){ SBuf err={0}; char *vs=optparse_with_values(us,op->src,&err);
          if(err.s){ fputs(err.s,stderr); return 1; }
          if(getenv("PPY_OPDEBUG")) fputs(vs+strlen(op->src),stderr);
          op->src=vs; op->ast=py_front(op->path,vs); if(!op->ast) return 1; } }
    { AotUnit *ap=find_unit(us,"argparse");                       /* argparse: the program's Namespace class */
      if(ap && ap->src){ SBuf err={0}; char *ns=argparse_with_namespace(us,ap->src,&err);
          if(err.s){ fputs(err.s,stderr); return 1; }
          ap->src=ns; ap->ast=py_front(ap->path,ns); if(!ap->ast) return 1; } }
    return 0;
}


int aot_main(int argc, char **argv, const char *program){
    const char *src_path=NULL;
    AotBuild b; memset(&b,0,sizeof b);
    b.fasm=getenv("PPY_FASM"); b.fasm_args=getenv("PPY_FASM_ARGS"); b.cc=getenv("PPY_CC"); b.cc_args=getenv("PPY_CC_ARGS");
    b.pack=1; b.run_build=-1;
    const char *out=NULL;
    int only_asm=0, count_allocs=0, check=0;
    unsigned costack=0, chunk=0;
    char **inputs=(char**)xmalloc(sizeof(char*)*(size_t)(argc+1)); int ninputs=0;
    const AotTargetDef *t=aot_target(aot_target_host());
    b.target_name=t->name;
    for(int i=0;i<argc;i++){
        const char *a=argv[i];
        #define NEXT() (i+1<argc ? argv[++i] : (usage(program), (char*)NULL))
        #define BYTES(v) do{ const char *x_=NEXT(); if(!x_) return 2; v=(unsigned)strtoul(x_,NULL,0); }while(0)
        if(!strcmp(a,"-o")){ if(!(out=NEXT())) return 2; }
        else if(!strcmp(a,"--target")){ const char *n=NEXT(); if(!n) return 2;
            if(!(t=aot_target_find(n))){ fprintf(stderr,"plainpy: unknown target '%s'; the targets:\n",n); aot_targets_help(stderr); return 2; }
            b.target_name=n; }
        else if(!strcmp(a,"-S")) only_asm=1;
        else if(!strcmp(a,"--fasm")){ if(!(b.fasm=NEXT())) return 2; }
        else if(!strcmp(a,"--fasm-args")){ if(!(b.fasm_args=NEXT())) return 2; }
        else if(!strcmp(a,"--cc")){ if(!(b.cc=NEXT())) return 2; }
        else if(!strcmp(a,"--cc-args")){ if(!(b.cc_args=NEXT())) return 2; }
        else if(!strcmp(a,"--port")){ if(!(b.port=NEXT())) return 2; }
        else if(!strcmp(a,"--chip")){ if(!(b.chip=NEXT())) return 2; }
        else if(!strcmp(a,"--build")) b.run_build=1;
        else if(!strcmp(a,"--no-build")) b.run_build=0;
        else if(!strcmp(a,"--arena")) BYTES(b.arena);
        else if(!strcmp(a,"--stack")) BYTES(b.stack);
        else if(!strcmp(a,"--costack")) BYTES(costack);
        else if(!strcmp(a,"--chunk")) BYTES(chunk);
        else if(!strcmp(a,"-v")) b.verbose=1;
        else if(!strcmp(a,"--count-allocs")) count_allocs=1;
        else if(!strcmp(a,"--no-pack")) b.pack=0;
        else if(!strcmp(a,"--check")) check=1;
        else if(!strcmp(a,"-h")||!strcmp(a,"--help")){ usage(program); return 0; }
        else if(a[0]=='-'){ fprintf(stderr,"plainpy: unknown option '%s'\n",a); usage(program); return 2; }
        else inputs[ninputs++]=(char*)a;
        #undef NEXT
        #undef BYTES
    }
    b.t=t;
    if(check) return aot_check_modules(inputs,ninputs,t->id);   /* modules to check, not a program */
    if(ninputs>1){ fprintf(stderr,"plainpy: more than one input file\n"); return 2; }
    if(ninputs==1) src_path=inputs[0];
    if(!src_path){ usage(program); return 2; }
    if(b.port && t->id==AOT_TARGET_C && !strcmp(b.port,"posix")){   /* a program for this computer: its memory as on the others */
        if(!costack) costack=65536;
        if(!chunk) chunk=65536;
        if(!b.stack) b.stack=1u<<20;
    }

    /* 1. the entry script and, transitively, its imports */
    ppy_fs_use_project(src_path);                          /* (the project's packages: those that passed `plainpy check`) */
    Units us; memset(&us,0,sizeof us);
    char *err=NULL, *src=ppy_fs_try_read_file(src_path,&err);
    if(!src){ fprintf(stderr,"plainpy: %s\n",err?err:"cannot read input"); free(err); return 1; }
    if(load_program(&us,src_path,src)) return 1;
    /* 2. types, then one listing for the whole program */
    AotCodegenOptions co; memset(&co,0,sizeof co);
    co.target=t->id; co.stack_size=b.stack?b.stack:t->stack; co.count_allocs=count_allocs;
    co.chunk=chunk?chunk:t->chunk; co.costack=costack?costack:t->costack;
    char *listing=NULL; size_t len=0;
    if(aot_compile(&co,us.v,us.n,&listing,&len)) return 1;
    char *base=out?xstrdup2(out):strip_ext(src_path);
    char *asm_path=aot_concat(base,".asm");
    if(aot_write_file(asm_path,listing,len,0)) return 1;
    if(b.verbose) printf("wrote %s (%d module%s)\n",asm_path,us.n,us.n==1?"":"s");
    if(only_asm) return 0;
    /* 3. the target makes the program */
    b.src_path=src_path; b.base=base; b.asm_path=asm_path; b.listing=listing; b.len=len;
    if(!b.stack) b.stack=co.stack_size;
    return t->build(&b);
}
