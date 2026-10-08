/* ========================= AOT driver: minipy --compile =========================
   1. import pass: parse the entry script and every module it imports. Imports
      are resolved here, at compile time, the same way the interpreter resolves
      them at run time (relative to the importing file, `<name>.mpy` or
      `<name>/__init__.mpy`, namespace packages for intermediate components);
      therefore every import must sit at the top of its module.
   2. type check + code generation: all modules -> one fasm listing
      (aot_types.c, aot_codegen.c), runtime routines included.
   3. fasm turns the listing straight into the executable (Linux ELF or
      KolibriOS application): no linker, no runtime library to install. The
      same works inside KolibriOS with its own fasm. */

#include "aot.h"
#include "frontparser.h"
#include "fs.h"

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
        "usage: %s --compile [options] file.mpy\n"
        "  -o FILE          output executable (default: the script name without .mpy)\n"
        "  --target T       linux (i386 ELF) or kolibri (KolibriOS application)\n"
        "  -S               only write the assembly listing (FILE.asm)\n"
        "  --fasm PATH      fasm executable                       [env MPY_FASM]\n"
        "  --fasm-args A    fasm arguments, {in} {out} {dir} replaced  [env MPY_FASM_ARGS]\n"
        "  --stack BYTES    kolibri: application stack size (default 65536)\n"
        "  -v               print the commands being run\n"
        "  --count-allocs   debugging: the program reports its live heap blocks at exit\n"
        "Compiled programs are statically typed: every variable, parameter, field and\n"
        "container element keeps one type (annotated or inferred), None is the zero\n"
        "value of that type, and imports must be at the top of a module.\n",
        program);
}

/* ---------------------------------------------------------------- import pass */

typedef struct { AotUnit **v; int n, cap; } Units;

static AotUnit *find_unit(Units *us, const char *name){ for(int i=0;i<us->n;i++) if(!strcmp(us->v[i]->name,name)) return us->v[i]; return NULL; }
/* `if __name__ == "__main__":` at the top level of a module */
static int is_main_guard(AotUnit *u, Stmt *s){
    if(s->kind!=STMT_IF || !s->expr || s->expr->end-s->expr->start!=3) return 0;
    Tok *t=u->tv.v+s->expr->start;
    int i=t[0].kind==T_NAME ? 0 : 2;
    return t[1].kind==T_EQ && t[i].kind==T_NAME && !strcmp(t[i].text,"__name__") && t[2-i].kind==T_STRING && !strcmp(t[2-i].text,"__main__");
}
static int is_docstring(AotUnit *u, Stmt *s);
/* The guard's body runs in the program's main module and never in an imported
   one: it is put in place of the if (or its else part is). Imports in it move
   up to the module's other imports - a compiled module imports at its top. */
static void apply_main_guards(AotUnit *u, int is_main){
    Ast *m=u->ast; int n=m->body_count, header=0, any=0;
    for(int i=0;i<n;i++) if(is_main_guard(u,m->body[i])) any=1;
    if(!any) return;
    while(header<n && (m->body[header]->kind==STMT_IMPORT || m->body[header]->kind==STMT_FROM_IMPORT || (header==0 && is_docstring(u,m->body[0])))) header++;
    int cap=n; for(int i=0;i<n;i++) if(is_main_guard(u,m->body[i])) cap+=m->body[i]->body_count+m->body[i]->orelse_count;
    Stmt **out=MPY_NEW_ARR(Stmt*,cap), **rest=MPY_NEW_ARR(Stmt*,cap); int k=0, r=0;
    for(int i=0;i<header;i++) out[k++]=m->body[i];
    for(int i=header;i<n;i++){ Stmt *s=m->body[i];
        if(!is_main_guard(u,s)){ rest[r++]=s; continue; }
        Stmt **b=is_main?s->body:s->orelse; int bn=is_main?s->body_count:s->orelse_count;
        for(int j=0;j<bn;j++){
            if(b[j]->kind==STMT_IMPORT || b[j]->kind==STMT_FROM_IMPORT) out[k++]=b[j];
            else rest[r++]=b[j];
        }
    }
    for(int i=0;i<r;i++) out[k++]=rest[i];
    free(rest);
    m->body=out; m->body_count=k; m->body_cap=cap;
}
static AotUnit *add_unit(Units *us, const char *name, const char *path, char *src){
    AotUnit *u=MPY_NEW0(AotUnit);
    u->name=xstrdup2(name); u->path=path?xstrdup2(path):NULL; u->src=src;
    if(src){ u->tv=lex(src); SymTable st; memset(&st,0,sizeof st); u->ast=build_ast_and_symbols(&u->tv,&st); apply_main_guards(u,!strcmp(name,"__main__")); }
    if(us->n==us->cap){ us->cap=us->cap?us->cap*2:8; us->v=(AotUnit**)xrealloc(us->v,sizeof(AotUnit*)*(size_t)us->cap); }
    us->v[us->n++]=u;
    return u;
}
static int is_builtin_module(const char *name){
    static const char *mods[]={"sys","thread","asyncio","math","time","random","typing","functools","__future__","collections","collections.abc","ctypes","json","minipy",NULL};
    for(int i=0;mods[i];i++) if(!strcmp(mods[i],name)) return 1;
    return 0;
}

/* Make `dotted` and each of its prefixes available, as vm_import_dotted would. */
static int resolve_import(Units *us, AotUnit *from, const char *dotted, int line){
    char *dir=mpy_fs_dirname(from->path);
    int ok=1;
    for(const char *p=dotted;;){
        const char *dot=strchr(p,'.');
        char *prefix=xstrndup2(dotted,dot?(int)(dot-dotted):(int)strlen(dotted));
        int leaf=(dot==NULL);
        if(!find_unit(us,prefix)){
            if(is_builtin_module(prefix)){
                if(!leaf && strcmp(dotted,"collections.abc")){ fprintf(stderr,"%s:%d: error: built-in module '%s' has no submodules\n",from->path,line,prefix); ok=0; }
            } else {
                char *path=mpy_fs_find_module(dir,prefix), *err=NULL;   /* the importer's folder, MINIPYPATH, <minipy>/lib */
                char *src=path?mpy_fs_try_read_file(path,&err):NULL;
                free(err);
                if(src) add_unit(us,prefix,path,src);
                else if(!leaf) add_unit(us,prefix,NULL,NULL);          /* namespace package */
                else { fprintf(stderr,"%s:%d: error: No module named '%s'\n",from->path,line,prefix); ok=0; }
                free(path);
            }
        }
        free(prefix);
        if(leaf || !ok) break;
        p=dot+1;
    }
    free(dir);
    return ok;
}

/* What an import nested in statement `s` would be inside of. */
static const char *construct_name(Stmt *s, const char *outer){
    switch(s->kind){
        case STMT_FUNCTION_DEF: return "a function";
        case STMT_CLASS_DEF: return "a class";
        case STMT_IF: return "an if statement";
        case STMT_WHILE: case STMT_FOR: return "a loop";
        case STMT_TRY: return "a try statement";
        case STMT_WITH: return "a with statement";
        default: return outer;
    }
}
static int nested_import_error(AotUnit *u, Stmt **b, int n, const char *where){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_IMPORT || s->kind==STMT_FROM_IMPORT){
            fprintf(stderr,"%s:%d: error: import inside %s; in compiled programs every import must be at the top of the module\n",u->path,s->line,where);
            return 1; }
        const char *w=construct_name(s,where);
        if(nested_import_error(u,s->body,s->body_count,w) || nested_import_error(u,s->orelse,s->orelse_count,w)) return 1;
    }
    return 0;
}
static int is_docstring(AotUnit *u, Stmt *s){
    return s->kind==STMT_EXPR && s->expr && s->expr->end==s->expr->start+1 && u->tv.v[s->expr->start].kind==T_STRING;
}
/* The import restriction, then the imported modules. */
static int scan_imports(Units *us, AotUnit *u){
    Stmt **b=u->ast->body; int n=u->ast->body_count, header=1;
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_IMPORT || s->kind==STMT_FROM_IMPORT){
            if(!header){ fprintf(stderr,"%s:%d: error: import after other statements; in compiled programs every import must be at the top of the module\n",u->path,s->line); return 0; }
            char *mod=s->kind==STMT_IMPORT ? xstrdup2(s->name2?s->name2:s->name) : aot_from_import_module(u,s);
            int ok=resolve_import(us,u,mod,s->line); free(mod);
            if(!ok) return 0;
            continue;
        }
        if(!(i==0 && is_docstring(u,s))) header=0;
        const char *w=construct_name(s,"a block");
        if(nested_import_error(u,s->body,s->body_count,w) || nested_import_error(u,s->orelse,s->orelse_count,w)) return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- running tools */

static char *strip_ext(const char *path){
    size_t n=strlen(path); const char *slash=strrchr(path,'/'), *dot=strrchr(path,'.');
    if(dot && (!slash || dot>slash) && (strcmp(dot,".mpy")==0 || strcmp(dot,".py")==0)) n=(size_t)(dot-path);
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

int aot_main(int argc, char **argv, const char *program){
    const char *src_path=NULL, *out=NULL, *fasm=getenv("MPY_FASM"), *fasm_tmpl=getenv("MPY_FASM_ARGS");
    int only_asm=0, verbose=0; unsigned stack=0;
    int count_allocs=0;
#if defined(MPY_KOLIBRI)
    AotTarget target=AOT_TARGET_KOLIBRI;
#else
    AotTarget target=AOT_TARGET_LINUX;
#endif
    for(int i=0;i<argc;i++){
        const char *a=argv[i];
        #define NEXT() (i+1<argc ? argv[++i] : (usage(program), (char*)NULL))
        if(!strcmp(a,"-o")){ if(!(out=NEXT())) return 2; }
        else if(!strcmp(a,"--target")){ const char *t=NEXT(); if(!t) return 2;
            if(!strcmp(t,"linux")) target=AOT_TARGET_LINUX; else if(!strcmp(t,"kolibri")||!strcmp(t,"kolibrios")) target=AOT_TARGET_KOLIBRI;
            else { fprintf(stderr,"minipy: unknown target '%s' (linux, kolibri)\n",t); return 2; } }
        else if(!strcmp(a,"-S")) only_asm=1;
        else if(!strcmp(a,"--fasm")){ if(!(fasm=NEXT())) return 2; }
        else if(!strcmp(a,"--fasm-args")){ if(!(fasm_tmpl=NEXT())) return 2; }
        else if(!strcmp(a,"--stack")){ const char *v=NEXT(); if(!v) return 2; stack=(unsigned)strtoul(v,NULL,0); }
        else if(!strcmp(a,"-v")) verbose=1;
        else if(!strcmp(a,"--count-allocs")) count_allocs=1;
        else if(!strcmp(a,"-h")||!strcmp(a,"--help")){ usage(program); return 0; }
        else if(a[0]=='-'){ fprintf(stderr,"minipy: unknown option '%s'\n",a); usage(program); return 2; }
        else if(!src_path) src_path=a;
        else { fprintf(stderr,"minipy: more than one input file\n"); return 2; }
        #undef NEXT
    }
    if(!src_path){ usage(program); return 2; }
    if(!fasm) fasm=MPY_AOT_FASM;
    if(!fasm_tmpl) fasm_tmpl=MPY_AOT_FASM_ARGS;

    /* 1. the entry script and, transitively, its imports */
    Units us; memset(&us,0,sizeof us);
    char *err=NULL, *src=mpy_fs_try_read_file(src_path,&err);
    if(!src){ fprintf(stderr,"minipy: %s\n",err?err:"cannot read input"); free(err); return 1; }
    add_unit(&us,"__main__",src_path,src);
    for(int i=0;i<us.n;i++) if(us.v[i]->ast && !scan_imports(&us,us.v[i])) return 1;

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

    /* 3. fasm writes the executable */
    mpy_fs_remove(base);                                   /* fasm's exit status is not visible on KolibriOS */
    char *args=fasm_args(fasm_tmpl,asm_path,base);
    int rc=run_tool(fasm,args,"--fasm",verbose); free(args);
    if(rc) return 1;
    if(!mpy_fs_exists(base)){ fprintf(stderr,"minipy: fasm did not produce %s (see its output)\n",base); return 1; }
    return 0;
}
