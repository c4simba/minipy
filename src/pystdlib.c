/* The standard library modules written in Python, from plainpy.lib (see
   pystdlib.h; tools/pack.c writes the file and says how it is laid out). */
#include "pystdlib.h"
#include "targets/platform.h"
#include "inflate.h"
#include "fs.h"
#include "libid.h"

#define LIB_NAME "plainpy.lib"
enum { LIB_MODULE=0, LIB_PACKAGE=1, LIB_TEXT=2 };
#ifdef PPY_LIB_COMPILER
#define LIB_ME 2                                     /* (who this program is, for the entries' "for" byte) */
#define LIB_PROGRAM "plainpyc"
#else
#define LIB_ME 1
#define LIB_PROGRAM "plainpy"
#endif

typedef struct { const char *name; int kind, who; PpyPacked p; } LibEntry;
static LibEntry *lib; static int nlib=-1;

static unsigned rd32(const unsigned char *b){ return b[0]|(unsigned)b[1]<<8|(unsigned)b[2]<<16|(unsigned)b[3]<<24; }

PPY_NORETURN static void lib_fail(const char *path, const char *why){   /* (path NULL: why names it) */
    if(path) fprintf(stderr,"%s: %s: %s\n",LIB_PROGRAM,path,why); else fprintf(stderr,"%s: %s\n",LIB_PROGRAM,why);
    fprintf(stderr,"%s needs the plainpy.lib of its own build next to it or in ../lib/plainpy (or PLAINPYLIB naming it)\n",LIB_PROGRAM);
    exit(1);
}

/* where the file is: PLAINPYLIB, else next to the program (or, installed, in
   <prefix>/lib/plainpy: ppy_fs_data_dir) */
static char *lib_path(void){
    const char *env=getenv("PLAINPYLIB");
    if(env && *env) return xstrdup2(env);
    const char *dir=ppy_fs_data_dir();
    return dir ? ppy_fs_join_path(dir,LIB_NAME) : xstrdup2(LIB_NAME);
}

static void lib_load(void){
    if(nlib>=0) return;
    char *path=lib_path(), *err=NULL;
    unsigned char *b=(unsigned char*)ppy_fs_try_read_file(path,&err);
    if(!b){ if(err) lib_fail(NULL,err); lib_fail(path,"cannot read it"); }
    size_t n=ppy_fs_last_len;
    if(n<20 || memcmp(b,"PPYLIB\0\1",8)) lib_fail(path,"not a plainpy library");
    if(rd32(b+8)!=PPY_LIB_ID) lib_fail(path,"the library of another build");
    unsigned count=rd32(b+12), isize=rd32(b+16);
    if(20+(size_t)isize>n) lib_fail(path,"damaged");
    const unsigned char *q=b+20, *end=q+isize, *data=end;
    lib=(LibEntry*)xmalloc(sizeof(LibEntry)*(size_t)(count+1));
    for(unsigned i=0;i<count;i++){
        if(q+16>end) lib_fail(path,"damaged");
        unsigned l=q[2]|(unsigned)q[3]<<8;
        if(q+16+l>end) lib_fail(path,"damaged");
        LibEntry *e=&lib[i];
        e->kind=q[0]; e->who=q[1]; e->name=xstrndup2((const char*)q+4,(int)l); q+=4+l;
        unsigned off=rd32(q), clen=rd32(q+4), len=rd32(q+8); q+=12;
        if((size_t)(data-b)+off+clen>n) lib_fail(path,"damaged");
        e->p.data=data+off; e->p.clen=clen; e->p.len=len; e->p.text=NULL;
    }
    nlib=(int)count;                                  /* (the file stays: the entries point into it) */
    free(path);
}

static LibEntry *lib_find(const char *name, int text){
    lib_load();
    for(int i=0;i<nlib;i++) if((lib[i].kind==LIB_TEXT)==text && !strcmp(lib[i].name,name)) return &lib[i];
    return NULL;
}
static const char *lib_unpack(LibEntry *e){
    const char *t=ppy_unpack(&e->p);
    if(!t){ char *path=lib_path(); lib_fail(path,"damaged"); }
    return t;
}

int ppy_stdlib_left_out(const char *name){
    LibEntry *e=lib_find(name,0);
    return e && !(e->who&LIB_ME);
}
const char *ppy_stdlib_alias(const char *name){
    static const char *const aliases[][2]={{"os.path","posixpath"},{NULL,NULL}};
    for(int i=0;aliases[i][0];i++) if(!strcmp(aliases[i][0],name)) return aliases[i][1];
    return NULL;
}
const char *ppy_stdlib_source(const char *name, int *package){
    LibEntry *e=lib_find(name,0);
    if(!e || !(e->who&LIB_ME)) return NULL;
    if(package) *package= e->kind==LIB_PACKAGE;
    return lib_unpack(e);
}
const char *ppy_lib_text(const char *name){
    LibEntry *e=lib_find(name,1);
    return e ? lib_unpack(e) : NULL;
}
