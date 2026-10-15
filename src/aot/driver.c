/* ========================= AOT driver: minipyc (minipy --compile) =========================
   Options, then the import pass, the listing (aot_compile), and the
   tools that make the executable: fasm (and kpack) or, for macos, the C
   translation and the C compiler. */

#include "driver.h"

/* macos: the C compiler; override with --cc/--cc-args or MPY_CC/MPY_CC_ARGS. */
#ifndef MPY_AOT_CC
#  define MPY_AOT_CC "cc"
#endif
#ifndef MPY_AOT_CC_ARGS            /* {in} {out} {dir} are substituted */
#  define MPY_AOT_CC_ARGS "-O1 -w {in} -o {out} -lm"
#endif

/* Defaults; override with --fasm/--fasm-args or MPY_FASM/MPY_FASM_ARGS. */
#ifndef MPY_AOT_FASM
#  if defined(MPY_KOLIBRI)
#    define MPY_AOT_FASM "/sys/develop/fasm"
#  else
#    define MPY_AOT_FASM "fasm"
#  endif
#endif
#ifndef MPY_AOT_FASM_ARGS          /* {in} {out} {dir} are substituted */
#  if defined(MPY_KOLIBRI)
#    define MPY_AOT_FASM_ARGS "{in},{out},{dir}"
#  else
#    define MPY_AOT_FASM_ARGS "-m 262144 {in} {out}"
#  endif
#endif

static void usage(const char *program){
    fprintf(stderr,
        "usage: %s [options] file.py   (minipy --compile [options] file.py)\n"
        "  -o FILE          output executable (default: the script name without .py)\n"
        "  --target T       linux (i386 ELF), kolibri (KolibriOS application) or macos\n"
        "                   (native: the listing translated to C, FILE.c, built by cc);\n"
        "                   default: the system minipy runs on\n"
        "  -S               only write the assembly listing (FILE.asm)\n"
        "  --fasm PATH      fasm executable                       [env MPY_FASM]\n"
        "  --fasm-args A    fasm arguments, {in} {out} {dir} replaced  [env MPY_FASM_ARGS]\n"
        "  --cc PATH        macos: C compiler                     [env MPY_CC]\n"
        "  --cc-args A      macos: its arguments, {in} {out} {dir} replaced  [env MPY_CC_ARGS]\n"
        "  --stack BYTES    kolibri: application stack size (default 65536)\n"
        "  --no-pack        kolibri: leave the executable unpacked (by default it is\n"
        "                   LZMA-packed as kpack does it; KolibriOS unpacks it on loading)\n"
        "  -v               print the commands being run\n"
        "  --count-allocs   debugging: the program reports its live heap blocks at exit\n"
        "  --check MODULE.. check modules (packages) for compiled programs instead:\n"
        "                   every error, and the functions it cannot check (`minipy check`)\n"
        "Compiled programs are statically typed: every variable, parameter, field and\n"
        "container element keeps one type (annotated or inferred), None is the zero\n"
        "value of that type, and imports must be at the top of a module.\n",
        program);
}

/* ---------------------------------------------------------------- running tools */

static char *strip_ext(const char *path){
    size_t n=strlen(path); const char *slash=strrchr(path,'/'), *dot=strrchr(path,'.');
    if(dot && (!slash || dot>slash) && strcmp(dot,".py")==0) n=(size_t)(dot-path);
    return xstrndup2(path,(int)n);
}
static char *concat(const char *a, const char *b){ size_t la=strlen(a), lb=strlen(b); char *r=(char*)xmalloc(la+lb+1); memcpy(r,a,la); memcpy(r+la,b,lb+1); return r; }

/* Quote one argument for the platform's command line. */
static char *quote_arg(const char *s){
#if defined(MPY_KOLIBRI)
    return xstrdup2(s);                                   /* KolibriOS passes the parameter string verbatim */
#else
    size_t n=strlen(s), k=2; for(size_t i=0;i<n;i++) k+= s[i]=='\'' ? 4 : 1;
    char *r=(char*)xmalloc(k+1), *o=r; *o++='\'';
    for(size_t i=0;i<n;i++){ if(s[i]=='\''){ memcpy(o,"'\\''",4); o+=4; } else *o++=s[i]; }
    *o++='\''; *o=0; return r;
#endif
}
/* Expand {in} {out} {dir} in a fasm argument template. */
static char *fasm_args(const char *tmpl, const char *in, const char *out){
    char *dir=mpy_fs_dirname(in), *qi=quote_arg(in), *qo=quote_arg(out), *qd=quote_arg(dir);
    size_t cap=strlen(tmpl)+strlen(qi)+strlen(qo)+strlen(qd)*4+16; char *r=(char*)xmalloc(cap); size_t k=0;
    for(const char *p=tmpl;*p;){
        const char *rep=NULL; size_t skip=0;
        if(!strncmp(p,"{in}",4)){ rep=qi; skip=4; } else if(!strncmp(p,"{out}",5)){ rep=qo; skip=5; } else if(!strncmp(p,"{dir}",5)){ rep=qd; skip=5; }
        if(rep){ size_t l=strlen(rep); if(k+l+1>cap){ cap=(k+l+1)*2; r=(char*)xrealloc(r,cap); } memcpy(r+k,rep,l); k+=l; p+=skip; }
        else { if(k+2>cap){ cap*=2; r=(char*)xrealloc(r,cap); } r[k++]=*p++; }
    }
    r[k]=0; free(dir); free(qi); free(qo); free(qd);
    return r;
}
static int run_tool(const char *program, const char *args, const char *option, int verbose){
    if(verbose) printf("%s %s\n",program,args);
    int rc=mpy_platform_run(program,args);
    if(rc==127) fprintf(stderr,"minipy: cannot run `%s`; is it installed? (choose another with %s)\n",program,option);
    else if(rc!=0) fprintf(stderr,"minipy: `%s` failed (exit status %d)\n",program,rc);
    return rc;
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
      if(any){ if(!find_unit(us,"io")){ int pkg; add_unit(us,"io","<stdlib>/io.py",xstrdup2(mpy_stdlib_source("io",&pkg)));
                   for(int i=0;i<us->n;i++) if(us->v[i]->ast && !scan_unit(us,us->v[i])) return 1; }
               main_imports(us->v[0],"io"); } }
    { int any=0, more=0;                                                    /* encode() / decode(): codecs.py; other encodings than */
      for(int i=0;i<us->n;i++) if(us->v[i]->src){                            /* utf-8 / ascii / latin-1 named: _codecs_more.py too */
          const char *t=us->v[i]->src;
          if(strstr(t,"encode(") || strstr(t,"decode(") || strstr(t,"str(") || strstr(t,"bytes(") || strstr(t,"ascii(") || strstr(t,"!a}") || strstr(t,"!a:")) any=1;   /* (ascii(x): repr(x).encode("ascii", "backslashreplace")) */
          if(us->v[i]->path && !strncmp(us->v[i]->path,"<stdlib>",8)) continue;
          if(names_codec(t)) more=1; }
      if(any || more){ int pkg;
          if(!find_unit(us,"codecs")) add_unit(us,"codecs","<stdlib>/codecs.py",xstrdup2(mpy_stdlib_source("codecs",&pkg)));
          main_imports(us->v[0],"codecs");
          if(more && !find_unit(us,"_codecs_more")){ add_unit(us,"_codecs_more","<stdlib>/_codecs_more.py",xstrdup2(mpy_stdlib_source("_codecs_more",&pkg))); main_imports(us->v[0],"_codecs_more"); } } }
    { int any=0; for(int i=0;i<us->n && !any;i++) if(us->v[i]->src && strcmp(us->v[i]->name,"_sysio") && (strstr(us->v[i]->src,".stdout") || strstr(us->v[i]->src,".stdin") ||
                                                                strstr(us->v[i]->src,".__std") || has_stderr_value(us->v[i]->src))) any=1;     /* sys.stdout ...: _sysio.py */
      if(any && !find_unit(us,"_sysio")){ int pkg; add_unit(us,"_sysio","<stdlib>/_sysio.py",xstrdup2(mpy_stdlib_source("_sysio",&pkg)));
          if(!scan_unit(us,us->v[us->n-1])) return 1; main_imports(us->v[0],"_sysio"); } }
    { int eg=0; for(int i=0;i<us->n;i++) if(us->v[i]->src && (strstr(us->v[i]->src,"ExceptionGroup") || strstr(us->v[i]->src,"except*") || strstr(us->v[i]->src,"except *"))) eg=1;
      if(eg) add_unit(us,"__mpy_eg","<exceptiongroup>",xstrdup2(EG_PRELUDE)); }      /* ExceptionGroup, except*: written in Python */
    { static const char *const oserr[]={"OSError","IOError","EnvironmentError","FileNotFoundError","FileExistsError","PermissionError","IsADirectoryError",
          "NotADirectoryError","ConnectionError","ConnectionRefusedError","ConnectionResetError","ConnectionAbortedError","BrokenPipeError","BlockingIOError",
          "TimeoutError","InterruptedError","ProcessLookupError","ChildProcessError","UnicodeDecodeError","UnicodeEncodeError",NULL};
      int any=0; for(int i=0;i<us->n && !any;i++) if(us->v[i]->src) for(int k=0;oserr[k];k++) if(strstr(us->v[i]->src,oserr[k])){ any=1; break; }
      if(any && !find_unit(us,"_oserror")){ int pkg; add_unit(us,"_oserror","<stdlib>/_oserror.py",xstrdup2(mpy_stdlib_source("_oserror",&pkg))); } }   /* OSError(errno, strerror) */
    { int any=0; for(int i=0;i<us->n && !any;i++) if(us->v[i]->src && strstr(us->v[i]->src,"math") && strcmp(us->v[i]->name,"_mathx")) any=1;   /* math's functions in Python: _mathx.py */
      if(any && !find_unit(us,"_mathx")){ int pkg; add_unit(us,"_mathx","<stdlib>/_mathx.py",xstrdup2(mpy_stdlib_source("_mathx",&pkg))); main_imports(us->v[0],"_mathx"); } }
    { int any=0; for(int i=0;i<us->n && !any;i++) if(us->v[i]->src && strcmp(us->v[i]->name,"_mpy_exit") && (strstr(us->v[i]->src,"SystemExit") || strstr(us->v[i]->src,"exit("))) any=1;   /* SystemExit's code: _mpy_exit.py */
      if(any && !find_unit(us,"_mpy_exit")){ int pkg; add_unit(us,"_mpy_exit","<stdlib>/_mpy_exit.py",xstrdup2(mpy_stdlib_source("_mpy_exit",&pkg))); if(!scan_unit(us,us->v[us->n-1])) return 1; } }
    { int any=0; for(int i=0;i<us->n && !any;i++) if(us->v[i]->src && strstr(us->v[i]->src,"json") && (strstr(us->v[i]->src,"load") || strstr(us->v[i]->src,"JSONDecodeError"))) any=1;   /* json.loads: _mpy_jsonr.py */
      if(any && !find_unit(us,"_mpy_jsonr")){ int pkg; add_unit(us,"_mpy_jsonr","<stdlib>/_mpy_jsonr.py",xstrdup2(mpy_stdlib_source("_mpy_jsonr",&pkg))); if(!scan_unit(us,us->v[us->n-1])) return 1;
          main_imports(us->v[0],"_mpy_jsonr"); } }
    { int any=0; for(int i=0;i<us->n && !any;i++) if(us->v[i]->src && strstr(us->v[i]->src,"__next__")) any=1;   /* iterator objects: _mpy_iter.py */
      if(any && !find_unit(us,"_mpy_iter")){ int pkg; add_unit(us,"_mpy_iter","<stdlib>/_mpy_iter.py",xstrdup2(mpy_stdlib_source("_mpy_iter",&pkg))); } }
    if(find_unit(us,"threading")) main_epilogue(us->v[0],"import threading as __mpy_threading\n__mpy_threading._shutdown()\n");   /* (non-daemon threads waited for) */
    if(find_unit(us,"atexit")) main_epilogue(us->v[0],"import atexit as __mpy_atexit\n__mpy_atexit._run_exitfuncs()\n");
    { int any=0; for(int i=0;i<us->n && !any;i++) if(us->v[i]->src && strstr(us->v[i]->src,"argv")) any=1;     /* sys.argv: _sysargs.py */
      if(any && !find_unit(us,"_sysargs")){ int pkg; add_unit(us,"_sysargs","<stdlib>/_sysargs.py",xstrdup2(mpy_stdlib_source("_sysargs",&pkg)));
          main_imports(us->v[0],"_sysargs"); } }
    { int ck=0; for(int i=0;i<us->n;i++) if(us->v[i]->src && strstr(us->v[i]->src,"cmp_to_key")) ck=1;
      if(ck){ add_unit(us,"__mpy_cmpkey","<cmp_to_key>",xstrdup2(CMPKEY_PRELUDE)); if(!scan_unit(us,us->v[us->n-1])) return 1; } }

    { AotUnit *ty=find_unit(us,"types");                          /* types: the program's SimpleNamespace class */
      if(ty && ty->src && !strstr(ty->src,"class SimpleNamespace:\n        \"\"\"An object whose attributes are the keyword arguments (those")){
          char *t=types_with_namespace(us,ty->src); ty->src=t; ty->ast=py_front(ty->path,t); if(!ty->ast) return 1; } }
    { AotUnit *op=find_unit(us,"optparse");                       /* optparse: the program's Values class */
      if(op && op->src){ SBuf err={0}; char *vs=optparse_with_values(us,op->src,&err);
          if(err.s){ fputs(err.s,stderr); return 1; }
          if(getenv("MPY_OPDEBUG")) fputs(vs+strlen(op->src),stderr);
          op->src=vs; op->ast=py_front(op->path,vs); if(!op->ast) return 1; } }
    { AotUnit *ap=find_unit(us,"argparse");                       /* argparse: the program's Namespace class */
      if(ap && ap->src){ SBuf err={0}; char *ns=argparse_with_namespace(us,ap->src,&err);
          if(err.s){ fputs(err.s,stderr); return 1; }
          ap->src=ns; ap->ast=py_front(ap->path,ns); if(!ap->ast) return 1; } }
    return 0;
}


int aot_main(int argc, char **argv, const char *program){
    const char *src_path=NULL, *out=NULL, *fasm=getenv("MPY_FASM"), *fasm_tmpl=getenv("MPY_FASM_ARGS");
    const char *cc=getenv("MPY_CC"), *cc_tmpl=getenv("MPY_CC_ARGS");
    int only_asm=0, verbose=0; unsigned stack=0;
    int count_allocs=0, pack=1, check=0;
    char **inputs=(char**)xmalloc(sizeof(char*)*(size_t)(argc+1)); int ninputs=0;
#if defined(MPY_KOLIBRI)
    AotTarget target=AOT_TARGET_KOLIBRI;
#elif defined(__APPLE__)
    AotTarget target=AOT_TARGET_MACOS;
#else
    AotTarget target=AOT_TARGET_LINUX;
#endif
    for(int i=0;i<argc;i++){
        const char *a=argv[i];
        #define NEXT() (i+1<argc ? argv[++i] : (usage(program), (char*)NULL))
        if(!strcmp(a,"-o")){ if(!(out=NEXT())) return 2; }
        else if(!strcmp(a,"--target")){ const char *t=NEXT(); if(!t) return 2;
            if(!strcmp(t,"linux")) target=AOT_TARGET_LINUX; else if(!strcmp(t,"kolibri")||!strcmp(t,"kolibrios")) target=AOT_TARGET_KOLIBRI;
            else if(!strcmp(t,"macos")||!strcmp(t,"darwin")) target=AOT_TARGET_MACOS;
            else { fprintf(stderr,"minipy: unknown target '%s' (linux, kolibri, macos)\n",t); return 2; } }
        else if(!strcmp(a,"-S")) only_asm=1;
        else if(!strcmp(a,"--fasm")){ if(!(fasm=NEXT())) return 2; }
        else if(!strcmp(a,"--fasm-args")){ if(!(fasm_tmpl=NEXT())) return 2; }
        else if(!strcmp(a,"--cc")){ if(!(cc=NEXT())) return 2; }
        else if(!strcmp(a,"--cc-args")){ if(!(cc_tmpl=NEXT())) return 2; }
        else if(!strcmp(a,"--stack")){ const char *v=NEXT(); if(!v) return 2; stack=(unsigned)strtoul(v,NULL,0); }
        else if(!strcmp(a,"-v")) verbose=1;
        else if(!strcmp(a,"--count-allocs")) count_allocs=1;
        else if(!strcmp(a,"--no-pack")) pack=0;
        else if(!strcmp(a,"--check")) check=1;
        else if(!strcmp(a,"-h")||!strcmp(a,"--help")){ usage(program); return 0; }
        else if(a[0]=='-'){ fprintf(stderr,"minipy: unknown option '%s'\n",a); usage(program); return 2; }
        else inputs[ninputs++]=(char*)a;
        #undef NEXT
    }
    if(check) return aot_check_modules(inputs,ninputs,target);   /* modules to check, not a program */
    if(ninputs>1){ fprintf(stderr,"minipy: more than one input file\n"); return 2; }
    if(ninputs==1) src_path=inputs[0];
    if(!src_path){ usage(program); return 2; }
    if(!fasm) fasm=MPY_AOT_FASM;
    if(!fasm_tmpl) fasm_tmpl=MPY_AOT_FASM_ARGS;
    if(!cc) cc=MPY_AOT_CC;
    if(!cc_tmpl) cc_tmpl=MPY_AOT_CC_ARGS;

    /* 1. the entry script and, transitively, its imports */
    mpy_fs_use_project(src_path);                          /* (the project's packages: those that passed `minipy check`) */
    Units us; memset(&us,0,sizeof us);
    char *err=NULL, *src=mpy_fs_try_read_file(src_path,&err);
    if(!src){ fprintf(stderr,"minipy: %s\n",err?err:"cannot read input"); free(err); return 1; }
    if(load_program(&us,src_path,src)) return 1;
    /* 2. types, then one listing for the whole program */
    AotCodegenOptions co; co.target=target; co.stack_size=stack; co.count_allocs=count_allocs;
    char *listing=NULL; size_t len=0;
    if(aot_compile(&co,us.v,us.n,&listing,&len)) return 1;
    char *base=out?xstrdup2(out):strip_ext(src_path);
    char *asm_path=concat(base,".asm");
    char *werr=NULL;
    if(mpy_fs_write_file(asm_path,listing,len,&werr)){ fprintf(stderr,"minipy: %s\n",werr?werr:"cannot write listing"); return 1; }
    if(verbose) printf("wrote %s (%d module%s)\n",asm_path,us.n,us.n==1?"":"s");
    if(only_asm) return 0;

    if(target==AOT_TARGET_MACOS){                          /* 3. C, then the C compiler */
        char *csrc=NULL; size_t clen=0;
        if(aot_x2c(listing,len,&csrc,&clen)) return 1;
        char *c_path=concat(base,".c");
        if(mpy_fs_write_file(c_path,csrc,clen,&werr)){ fprintf(stderr,"minipy: %s\n",werr?werr:"cannot write the C file"); return 1; }
        free(csrc);
        if(verbose) printf("wrote %s\n",c_path);
        mpy_fs_remove(base);
        const char *tmpl=cc_tmpl; char big[256];
        #if defined(__APPLE__)
        if(cc_tmpl==(const char*)MPY_AOT_CC_ARGS && clen>400000){     /* a big program: one huge C function, whose register */
            snprintf(big,sizeof big,"%s -mllvm -join-liveintervals=false -mllvm -enable-misched=false",MPY_AOT_CC_ARGS); tmpl=big; }   /* coalescing and instruction scheduling take clang most of the time */
        #else
        (void)big;
        #endif
        char *args=fasm_args(tmpl,c_path,base);
        int rc=run_tool(cc,args,"--cc",verbose); free(args);
        if(rc) return 1;
        if(!mpy_fs_exists(base)){ fprintf(stderr,"minipy: %s did not produce %s\n",cc,base); return 1; }
        return 0;
    }

    /* 3. fasm writes the executable */
    mpy_fs_remove(base);                                   /* fasm's exit status is not visible on KolibriOS */
    char *args=fasm_args(fasm_tmpl,asm_path,base);
    int rc=run_tool(fasm,args,"--fasm",verbose); free(args);
    if(rc) return 1;
    if(!mpy_fs_exists(base)){ fprintf(stderr,"minipy: fasm did not produce %s (see its output)\n",base); return 1; }
    if(target==AOT_TARGET_LINUX) mpy_platform_make_executable(base);   /* (fasm leaves it 0644) */
    if(target==AOT_TARGET_KOLIBRI && pack){                /* 4. kpack: LZMA, unpacked by the kernel as it loads the program */
        char *rerr=NULL, *raw=mpy_fs_try_read_file(base,&rerr); size_t rawn=mpy_fs_last_len;
        if(!raw){ fprintf(stderr,"minipy: %s\n",rerr?rerr:"cannot read the executable"); return 1; }
        unsigned char *packed=NULL; size_t pn=0;
        if(!aot_kpack((const unsigned char*)raw,rawn,&packed,&pn)){
            if(mpy_fs_write_file(base,(const char*)packed,pn,&werr)){ fprintf(stderr,"minipy: %s\n",werr?werr:"cannot write the executable"); return 1; }
            if(verbose) printf("packed %s: %lu -> %lu bytes\n",base,(unsigned long)rawn,(unsigned long)pn);
            free(packed);
        }
        free(raw);
    }
    return 0;
}
