/* ========================= cpython target: the driver =========================
   minipy --compile --target cpython [--python PY] [--libs] [--stdlib] app.py

   1. ask PY (default python3: a venv's python gives that venv's site-packages)
      for its headers, libraries and sys.path (capi_helper.py config);
   2. follow the imports of app.py (wherever they are in the code): modules
      found next to it are compiled, so are site-packages ones with --libs and
      the standard library's with --stdlib; everything else (extension
      modules, ...) is imported by the embedded Python as usual;
   3. PY compiles their sources too (capi_helper.py compile): the code
      objects CPython keeps for us and what it knows about them;
   4. each module -> C (capi_codegen.c); one whose top level this compiler
      cannot translate is left to CPython;
   5. trampolines (capi_helper.py stubs), runtime, main, a Makefile in
      app.build/; make builds ./app linked with libpython. */

#include "capi.h"
#include "fs.h"
#include "platform/platform.h"
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>

static const char helper_py[] =
#include "capi_helper.inc"
;
static const char rt_c[] =
#include "capi_rt_c.inc"
;
static const char rt_h[] =
#include "capi_rt_h.inc"
;

typedef struct { char *s; size_t len, cap; } DB;
static void db_put(DB *b, const char *s, size_t n){
    if(b->len+n+1>b->cap){ b->cap=(b->len+n+1)*2; b->s=(char*)xrealloc(b->s,b->cap); }
    memcpy(b->s+b->len,s,n); b->len+=n; b->s[b->len]=0;
}
static void db_f(DB *b, const char *fmt, ...){
    char tmp[4096]; va_list ap; va_start(ap,fmt); int n=vsnprintf(tmp,sizeof tmp,fmt,ap); va_end(ap);
    if(n<(int)sizeof tmp){ db_put(b,tmp,(size_t)n); return; }
    char *big=(char*)xmalloc((size_t)n+1); va_start(ap,fmt); vsnprintf(big,(size_t)n+1,fmt,ap); va_end(ap); db_put(b,big,(size_t)n); free(big);
}

static int verbose;
static char *path_join(const char *a, const char *b){ DB d={0}; db_f(&d,"%s/%s",a,b); return d.s; }
static int is_file(const char *p){ struct stat st; return stat(p,&st)==0 && S_ISREG(st.st_mode); }
static int is_dir(const char *p){ struct stat st; return stat(p,&st)==0 && S_ISDIR(st.st_mode); }
static char *abspath(const char *p){
    char buf[4096];
    if(realpath(p,buf)) return xstrdup2(buf);
    return xstrdup2(p);
}
static char *read_file(const char *path, size_t *len);
/* (only when it changed: make rebuilds just what did) */
static int write_file(const char *path, const char *data, size_t n){
    size_t old; char *prev=read_file(path,&old);
    if(prev && old==n && !memcmp(prev,data,n)){ free(prev); return 0; }
    free(prev);
    FILE *f=fopen(path,"wb"); if(!f){ fprintf(stderr,"minipy: cannot write %s\n",path); return 1; }
    int ok=fwrite(data,1,n,f)==n; fclose(f);
    if(!ok){ fprintf(stderr,"minipy: cannot write %s\n",path); return 1; }
    return 0;
}
static char *read_file(const char *path, size_t *len){
    FILE *f=fopen(path,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    char *s=(char*)xmalloc((size_t)n+1); size_t got=fread(s,1,(size_t)n,f); fclose(f); s[got]=0;
    if(len) *len=got;
    return s;
}
static char *shq(const char *s){                 /* quoted for sh */
    DB d={0}; db_put(&d,"'",1);
    for(const char *p=s;*p;p++){ if(*p=='\'') db_put(&d,"'\\''",4); else db_put(&d,p,1); }
    db_put(&d,"'",1); return d.s;
}
static int run(const char *cmd){
    if(verbose) printf("%s\n",cmd);
    int rc=system(cmd);
    if(rc==-1) return 127;
    return WEXITSTATUS(rc);
}
static char *run_capture(const char *cmd){
    if(verbose) printf("%s\n",cmd);
    FILE *p=popen(cmd,"r"); if(!p) return NULL;
    DB d={0}; char buf[4096]; size_t n;
    while((n=fread(buf,1,sizeof buf,p))>0) db_put(&d,buf,n);
    int rc=pclose(p);
    if(rc!=0){ free(d.s); return NULL; }
    if(!d.s) d.s=xstrdup2("");
    return d.s;
}

/* ---- the target Python */
typedef struct {
    char *exe, *version, *cflags, *ldflags, *stdlib, *purelib, *platlib;
    char **path; int npath;
} PyCfg;
static char *nextline(char **p){ char *s=*p; char *e=strchr(s,'\n'); if(e){ *e=0; *p=e+1; } else *p=s+strlen(s); return s; }

/* ---- modules */
typedef struct {
    char *name;                 /* dotted */
    char *file;                 /* .py (NULL: not a source module) */
    char *dir;                  /* package directory */
    int kind;                   /* 0 project, 1 site-packages, 2 standard library, 3 other */
    int compile, scanned;
    int index;                  /* number in the build */
} Mod;
typedef struct { Mod *v; int n, cap; } Mods;
static Mod *mod_find(Mods *ms, const char *name){ for(int i=0;i<ms->n;i++) if(!strcmp(ms->v[i].name,name)) return &ms->v[i]; return NULL; }
static int starts_with_dir(const char *path, const char *dir){
    size_t n=strlen(dir); if(!n) return 0;
    return !strncmp(path,dir,n) && (path[n]=='/' || path[n]==0);
}

typedef struct { PyCfg *py; char *script_dir; int libs, stdlib_too; Mods mods; } Ctx;

static int classify(Ctx *c, const char *file){
    if(starts_with_dir(file,c->script_dir)) return 0;
    if((c->py->purelib && starts_with_dir(file,c->py->purelib)) || (c->py->platlib && starts_with_dir(file,c->py->platlib))) return 1;
    if(c->py->stdlib && starts_with_dir(file,c->py->stdlib)) return 2;
    return 3;
}
/* where a top-level name / a submodule is (NULL: not a .py module) */
static Mod *locate(Ctx *c, const char *name);
static Mod *add_mod(Ctx *c, const char *name, const char *file, const char *dir){
    Mods *ms=&c->mods;
    if(ms->n==ms->cap){ ms->cap=ms->cap?ms->cap*2:64; ms->v=(Mod*)xrealloc(ms->v,sizeof(Mod)*(size_t)ms->cap); }
    Mod *m=&ms->v[ms->n++]; memset(m,0,sizeof *m);
    m->name=xstrdup2(name); m->file=file?abspath(file):NULL; m->dir=dir?abspath(dir):NULL; m->index=-1;
    m->kind= m->file ? classify(c,m->file) : 3;
    m->compile= m->file && (m->kind==0 || (m->kind==1 && c->libs) || (m->kind==2 && c->stdlib_too));
    return m;
}
static Mod *search_in(Ctx *c, const char *name, const char *leaf, const char *dir){
    char *py=NULL, *pkg=NULL, *init=NULL; Mod *r=NULL;
    { DB d={0}; db_f(&d,"%s/%s",dir,leaf); pkg=d.s; }
    init=path_join(pkg,"__init__.py");
    { DB d={0}; db_f(&d,"%s/%s.py",dir,leaf); py=d.s; }
    if(is_file(init)) r=add_mod(c,name,init,pkg);
    else if(is_file(py)) r=add_mod(c,name,py,NULL);
    else if(is_dir(pkg)){
        /* a namespace package (or a directory with extension modules): its submodules may be sources */
        r=add_mod(c,name,NULL,pkg);
    }
    free(py); free(pkg); free(init);
    return r;
}
static Mod *locate(Ctx *c, const char *name){
    Mod *m=mod_find(&c->mods,name);
    if(m) return m;
    const char *dot=strrchr(name,'.');
    if(dot){
        char *parent=xstrndup2(name,(int)(dot-name));
        Mod *p=locate(c,parent); free(parent);
        if(!p || !p->dir) return NULL;
        return search_in(c,name,dot+1,p->dir);
    }
    Mod *r=search_in(c,name,name,c->script_dir);
    for(int i=0;!r && i<c->py->npath;i++) if(is_dir(c->py->path[i])) r=search_in(c,name,name,c->py->path[i]);
    return r;
}
/* the imports of a module's source */
static void imports_of(Ctx *c, Mod *m, PyNode *n);
static void import_name(Ctx *c, const char *name){
    /* every prefix: a.b.c imports a and a.b too */
    char buf[1024]; size_t l=strlen(name); if(l>=sizeof buf) return;
    for(size_t i=0;i<=l;i++) if(name[i]=='.' || name[i]==0){ memcpy(buf,name,i); buf[i]=0; locate(c,buf); }
}
static void imports_of(Ctx *c, Mod *m, PyNode *n){
    if(!n) return;
    if(n->kind==PK_Import){ for(int i=0;i<n->L[3].n;i++) import_name(c,n->L[3].v[i]->id[0]); }
    else if(n->kind==PK_ImportFrom){
        char base[1024]; base[0]=0;
        if(n->op>0){                                 /* relative: from the package */
            char pkg[1024]; snprintf(pkg,sizeof pkg,"%s",m->name);
            if(!m->dir){ char *d=strrchr(pkg,'.'); if(d) *d=0; else pkg[0]=0; }
            for(int k=1;k<n->op;k++){ char *d=strrchr(pkg,'.'); if(d) *d=0; else pkg[0]=0; }
            if(n->id[0]) snprintf(base,sizeof base,"%s%s%s",pkg,pkg[0]?".":"",n->id[0]);
            else snprintf(base,sizeof base,"%s",pkg);
        } else if(n->id[0]) snprintf(base,sizeof base,"%s",n->id[0]);
        if(base[0] && strcmp(base,"__future__")){
            import_name(c,base);
            Mod *p=mod_find(&c->mods,base);
            if(p && p->dir) for(int i=0;i<n->L[3].n;i++){
                const char *nm=n->L[3].v[i]->id[0]; if(!strcmp(nm,"*")) continue;
                char sub[1100]; snprintf(sub,sizeof sub,"%s.%s",base,nm);
                if(!mod_find(&c->mods,sub)) search_in(c,sub,nm,p->dir);
            }
        }
    }
    for(int i=0;i<4;i++) if(n->n[i]) imports_of(c,m,n->n[i]);
    for(int k=0;k<5;k++) for(int i=0;i<n->L[k].n;i++) if(n->L[k].v[i]) imports_of(c,m,n->L[k].v[i]);
}

static void usage(void){
    fprintf(stderr,
        "usage: minipy --compile --target cpython [options] app.py\n"
        "  -o FILE          the executable (default: app)\n"
        "  --python PY      the Python to build for (its headers, libpython, sys.path; a venv's\n"
        "                   python brings that venv's packages)   [env MPY_PYTHON, default python3]\n"
        "  --libs           also compile the modules imported from site-packages\n"
        "  --stdlib         also compile the modules imported from the standard library\n"
        "  --fast-calls     call compiled functions without their Python frame (faster;\n"
        "                   sys._getframe() depths then differ from CPython's)\n"
        "  --cc CC          the C compiler                         [env MPY_CC, default cc]\n"
        "  -O LEVEL         C optimization (default 1)\n"
        "  -j N             parallel C compilations (default 8)\n"
        "  -v               show the commands and what is compiled\n");
}

int capi_main(int argc, char **argv){
    const char *script=NULL, *out=NULL, *python=getenv("MPY_PYTHON"), *cc=getenv("MPY_CC"), *opt="1";
    int jobs=8, fast_calls=0;
    Ctx c; memset(&c,0,sizeof c);
    for(int i=0;i<argc;i++){
        const char *a=argv[i];
        if(!strcmp(a,"--target")){ i++; continue; }
        if(!strcmp(a,"-o") && i+1<argc) out=argv[++i];
        else if(!strcmp(a,"--python") && i+1<argc) python=argv[++i];
        else if(!strcmp(a,"--cc") && i+1<argc) cc=argv[++i];
        else if(!strcmp(a,"-O") && i+1<argc) opt=argv[++i];
        else if(!strncmp(a,"-O",2) && a[2]) opt=a+2;
        else if(!strcmp(a,"-j") && i+1<argc) jobs=atoi(argv[++i]);
        else if(!strcmp(a,"--libs")) c.libs=1;
        else if(!strcmp(a,"--stdlib")) c.stdlib_too=1;
        else if(!strcmp(a,"--fast-calls")) fast_calls=1;
        else if(!strcmp(a,"-v")) verbose=1;
        else if(!strcmp(a,"-h")||!strcmp(a,"--help")){ usage(); return 0; }
        else if(a[0]=='-'){ fprintf(stderr,"minipy: unknown option %s\n",a); usage(); return 2; }
        else script=a;
    }
    if(!script){ usage(); return 2; }
    if(!python) python="python3";
    if(!cc) cc="cc";
    char *script_abs=abspath(script);
    if(!is_file(script_abs)){ fprintf(stderr,"minipy: cannot read %s\n",script); return 1; }
    { char *d=xstrdup2(script_abs); char *sl=strrchr(d,'/'); if(sl) *sl=0; c.script_dir=d; }
    char *base;
    if(out) base=abspath(out[0]=='/'?out:out);
    else { base=xstrdup2(script_abs); char *dot=strrchr(base,'.'); char *sl=strrchr(base,'/'); if(dot && (!sl || dot>sl)) *dot=0; }
    if(out && out[0]!='/'){ char cwd[4096]; if(getcwd(cwd,sizeof cwd)){ free(base); base=path_join(cwd,out); } }
    char *bdir; { DB d={0}; db_f(&d,"%s.build",base); bdir=d.s; }
    { DB d={0}; db_f(&d,"mkdir -p %s",shq(bdir)); if(run(d.s)){ fprintf(stderr,"minipy: cannot create %s\n",bdir); return 1; } free(d.s); }
    char *hp=path_join(bdir,"helper.py");
    if(write_file(hp,helper_py,strlen(helper_py))) return 1;

    /* 1. the Python */
    PyCfg py; memset(&py,0,sizeof py); c.py=&py;
    { DB d={0}; db_f(&d,"%s %s config",shq(python),shq(hp));
      char *o=run_capture(d.s); free(d.s);
      if(!o){ fprintf(stderr,"minipy: cannot run the Python %s (choose one with --python)\n",python); return 1; }
      char *p=o;
      py.exe=xstrdup2(nextline(&p)); py.version=xstrdup2(nextline(&p)); py.cflags=xstrdup2(nextline(&p)); py.ldflags=xstrdup2(nextline(&p));
      py.stdlib=abspath(nextline(&p)); py.purelib=abspath(nextline(&p)); py.platlib=abspath(nextline(&p));   /* (resolved, as module paths are) */
      char *sp=nextline(&p);
      for(char *q=sp;*q;){ char *e=strchr(q,'\x1f'); size_t l=e?(size_t)(e-q):strlen(q);
          py.path=(char**)xrealloc(py.path,sizeof(char*)*(size_t)(py.npath+1)); py.path[py.npath++]=xstrndup2(q,(int)l); q+= e?l+1:l; }
      if(strcmp(py.version,"3.14")){ fprintf(stderr,"minipy: the cpython target needs Python 3.14 (%s is %s)\n",py.exe,py.version); return 1; }
      if(verbose) printf("python %s %s\n",py.exe,py.version);
    }

    /* 2. the modules */
    Mod *mainm=add_mod(&c,"__main__",script_abs,NULL);
    mainm->compile=1; mainm->kind=0;
    for(int i=0;i<c.mods.n;i++){
        Mod *m=&c.mods.v[i];
        if(m->scanned || !m->file) continue;
        m->scanned=1;
        if(!m->compile) continue;                  /* the imports of modules CPython compiles are CPython's business */
        size_t len; char *src=read_file(m->file,&len);
        if(!src) continue;
        PyParse pp;
        if(py_parse(&pp,m->file,src,len)){ if(verbose) printf("  %s: %s:%d: %s (left to CPython)\n",m->name,m->file,pp.error_line,pp.error); m->compile=0; free(src); continue; }
        imports_of(&c,m,pp.mod);
        free(src);
        m=&c.mods.v[i];
    }
    /* 3. CPython's code objects */
    DB cmd={0}; db_f(&cmd,"%s %s compile %s",shq(python),shq(hp),shq(bdir));
    int nc=0;
    for(int i=0;i<c.mods.n;i++){ Mod *m=&c.mods.v[i]; if(!m->compile) continue; m->index=nc++; char *q=shq(m->file); db_f(&cmd," %s",q); free(q); }
    if(run(cmd.s)){ fprintf(stderr,"minipy: %s could not compile the sources\n",python); return 1; }
    free(cmd.s);

    /* 4. C */
    DB stubs={0}; db_f(&stubs,"%s %s stubs %s",shq(python),shq(hp),shq(bdir));
    DB stubfiles={0};
    int compiled=0, units=0, islands=0;
    DB objs={0}, mtab={0}, mext={0};
    for(int i=0;i<c.mods.n;i++){
        Mod *m=&c.mods.v[i]; if(!m->compile) continue;
        char mp[64]; snprintf(mp,sizeof mp,"m%d.meta",m->index);
        char *metap=path_join(bdir,mp);
        CpyMeta meta; if(capi_read_meta(metap,&meta)){ fprintf(stderr,"minipy: no %s\n",metap); return 1; }
        size_t len; char *src=read_file(m->file,&len);
        CapiIn in; memset(&in,0,sizeof in); in.modname=m->name; in.path=m->file; in.src=src; in.len=len; in.index=m->index; in.meta=&meta; in.fast_calls=fast_calls;
        CapiOut o;
        if(capi_codegen(&in,&o)){
            if(!strcmp(m->name,"__main__")){ fprintf(stderr,"minipy: %s\n",o.error); return 1; }
            if(verbose) printf("  %s: left to CPython: %s\n",m->name,o.error);
            m->compile=0; free(src); continue;
        }
        compiled++; units+=o.nunits; islands+=o.nislands;
        char nm[64];
        snprintf(nm,sizeof nm,"m%d.c",m->index); { char *p=path_join(bdir,nm); if(write_file(p,o.c,o.clen)) return 1; free(p); }
        snprintf(nm,sizeof nm,"s%d.py",m->index); { char *p=path_join(bdir,nm); if(write_file(p,o.stub,o.stublen)) return 1; free(p); }
        /* code objects -> m<i>_code.inc */
        snprintf(nm,sizeof nm,"m%d.code",m->index);
        { char *p=path_join(bdir,nm); size_t bl; char *bin=read_file(p,&bl); free(p);
          DB inc={0}; for(size_t k=0;k<bl;k++) db_f(&inc,"%u,%s",(unsigned char)bin[k],k%24==23?"\n":"");
          if(!bl) db_put(&inc,"0",1);
          snprintf(nm,sizeof nm,"m%d_code.inc",m->index); p=path_join(bdir,nm); if(write_file(p,inc.s?inc.s:"0",inc.len?inc.len:1)) return 1; free(p); free(inc.s); free(bin); }
        db_f(&stubfiles," %s",shq(m->file));
        db_f(&objs," m%d.o",m->index);
        db_f(&mext,"int mpy_exec_%d(PyObject *module);\n",m->index);
        db_f(&mtab,"    {\"%s\",mpy_exec_%d,%d,",m->name,m->index,m->dir?1:0);
        { DB q={0}; db_f(&q,"%s",m->file); db_put(&mtab,"\"",1); for(char *x=q.s;*x;x++){ if(*x=='"'||*x=='\\') db_put(&mtab,"\\",1); db_put(&mtab,x,1); } db_put(&mtab,"\",",2); free(q.s); }
        if(m->dir){ db_put(&mtab,"\"",1); for(char *x=m->dir;*x;x++){ if(*x=='"'||*x=='\\') db_put(&mtab,"\\",1); db_put(&mtab,x,1); } db_put(&mtab,"\"},\n",4); }
        else db_f(&mtab,"NULL},\n");
        free(src);
        if(verbose) printf("  %s (%s): %d units, %d by CPython\n",m->name,m->file,o.nunits,o.nislands);
    }
    /* the stub sources, in module order */
    { DB order={0};
      for(int i=0;i<c.mods.n;i++){ Mod *m=&c.mods.v[i]; if(m->compile){ char nm[64]; snprintf(nm,sizeof nm,"s%d.py",m->index); char *p=path_join(bdir,nm); db_f(&order," %s",shq(p)); free(p); } }
      /* helper wants: SRC... STUB... in the same order; index = position */
      DB s2={0}; db_f(&s2,"%s %s stubs %s%s%s",shq(python),shq(hp),shq(bdir),stubfiles.s?stubfiles.s:"",order.s?order.s:"");
      if(run(s2.s)){ fprintf(stderr,"minipy: %s could not compile the trampolines\n",python); return 1; }
      free(s2.s); free(order.s);
      int pos=0;
      for(int i=0;i<c.mods.n;i++){ Mod *m=&c.mods.v[i]; if(!m->compile) continue;
          char nm[64]; snprintf(nm,sizeof nm,"s%d.code",pos++); char *p=path_join(bdir,nm); size_t bl; char *bin=read_file(p,&bl); free(p);
          DB inc={0}; for(size_t k=0;k<bl;k++) db_f(&inc,"%u,%s",(unsigned char)bin[k],k%24==23?"\n":"");
          snprintf(nm,sizeof nm,"m%d_stub.inc",m->index); p=path_join(bdir,nm); if(write_file(p,inc.s?inc.s:"0",inc.len?inc.len:1)) return 1; free(p); free(inc.s); free(bin); }
    }
    (void)stubs;
    /* 5. runtime, main, Makefile */
    { char *p=path_join(bdir,"mpy_rt.h"); if(write_file(p,rt_h,strlen(rt_h))) return 1; free(p); }
    { char *p=path_join(bdir,"mpy_rt.c"); if(write_file(p,rt_c,strlen(rt_c))) return 1; free(p); }
    { DB m={0};
      db_f(&m,"/* minipy --compile --target cpython: main */\n#include \"mpy_rt.h\"\n%s\nstatic const MpyModule mods[]={\n%s};\n",mext.s?mext.s:"",mtab.s?mtab.s:"");
      db_f(&m,"int main(int argc, char **argv){\n    return mpy_main(argc,argv,mods,%d,",compiled);
      { db_put(&m,"\"",1); for(char *x=py.exe;*x;x++){ if(*x=='"'||*x=='\\') db_put(&m,"\\",1); db_put(&m,x,1); } db_put(&m,"\",",2); }
      { db_put(&m,"\"",1); for(char *x=c.script_dir;*x;x++){ if(*x=='"'||*x=='\\') db_put(&m,"\\",1); db_put(&m,x,1); } db_put(&m,"\");\n}\n",5); }
      char *p=path_join(bdir,"main.c"); if(write_file(p,m.s,m.len)) return 1; free(p); free(m.s); }
    { DB mk={0};
      db_f(&mk,"# minipy --compile --target cpython\nCC=%s\nCFLAGS=-O%s -w %s\nLDFLAGS=%s\n",cc,opt,py.cflags,py.ldflags);
      db_f(&mk,"OBJS=main.o mpy_rt.o%s\n%s: $(OBJS)\n\t$(CC) $(OBJS) -o %s $(LDFLAGS)\n",objs.s?objs.s:"",shq(base),shq(base));
      db_f(&mk,"%%.o: %%.c mpy_rt.h\n\t$(CC) $(CFLAGS) -c $< -o $@\n");
      char *p=path_join(bdir,"Makefile"); if(write_file(p,mk.s,mk.len)) return 1; free(p); free(mk.s); }
    if(verbose) printf("%d modules compiled (%d units, %d code objects from CPython)\n",compiled,units,islands);
    { DB d={0}; db_f(&d,"make -s -j%d -C %s",jobs,shq(bdir));
      if(run(d.s)){ fprintf(stderr,"minipy: the C build failed (%s)\n",bdir); return 1; }
      free(d.s); }
    return 0;
}
