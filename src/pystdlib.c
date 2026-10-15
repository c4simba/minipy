/* The standard library modules written in Python, from minipy.lib (see
   pystdlib.h; tools/pack.c writes the file and says how it is laid out). */
#include "pystdlib.h"
#include "platform/platform.h"
#include "inflate.h"
#include "fs.h"
#include "libid.h"

#define LIB_NAME "minipy.lib"
enum { LIB_MODULE=0, LIB_PACKAGE=1, LIB_TEXT=2 };
#ifdef MPY_LIB_COMPILER
#define LIB_ME 2                                     /* (who this program is, for the entries' "for" byte) */
#define LIB_PROGRAM "minipyc"
#else
#define LIB_ME 1
#define LIB_PROGRAM "minipy"
#endif

typedef struct { const char *name; int kind, who; MpyPacked p; } LibEntry;
static LibEntry *lib; static int nlib=-1;

static unsigned rd32(const unsigned char *b){ return b[0]|(unsigned)b[1]<<8|(unsigned)b[2]<<16|(unsigned)b[3]<<24; }

MPY_NORETURN static void lib_fail(const char *path, const char *why){   /* (path NULL: why names it) */
    if(path) fprintf(stderr,"%s: %s: %s\n",LIB_PROGRAM,path,why); else fprintf(stderr,"%s: %s\n",LIB_PROGRAM,why);
    fprintf(stderr,"%s needs the minipy.lib of its own build next to it or in ../lib/minipy (or MINIPYLIB naming it)\n",LIB_PROGRAM);
    exit(1);
}

/* where the file is: MINIPYLIB, else next to the program (or, installed, in
   <prefix>/lib/minipy: mpy_fs_data_dir) */
static char *lib_path(void){
    const char *env=getenv("MINIPYLIB");
    if(env && *env) return xstrdup2(env);
    const char *dir=mpy_fs_data_dir();
    return dir ? mpy_fs_join_path(dir,LIB_NAME) : xstrdup2(LIB_NAME);
}

static void lib_load(void){
    if(nlib>=0) return;
    char *path=lib_path(), *err=NULL;
    unsigned char *b=(unsigned char*)mpy_fs_try_read_file(path,&err);
    if(!b){ if(err) lib_fail(NULL,err); lib_fail(path,"cannot read it"); }
    size_t n=mpy_fs_last_len;
    if(n<20 || memcmp(b,"MPYLIB\0\1",8)) lib_fail(path,"not a minipy library");
    if(rd32(b+8)!=MPY_LIB_ID) lib_fail(path,"the library of another build");
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
    const char *t=mpy_unpack(&e->p);
    if(!t){ char *path=lib_path(); lib_fail(path,"damaged"); }
    return t;
}

int mpy_stdlib_left_out(const char *name){
    LibEntry *e=lib_find(name,0);
    return e && !(e->who&LIB_ME);
}
const char *mpy_stdlib_alias(const char *name){
    static const char *const aliases[][2]={{"os.path","posixpath"},{NULL,NULL}};
    for(int i=0;aliases[i][0];i++) if(!strcmp(aliases[i][0],name)) return aliases[i][1];
    return NULL;
}
const char *mpy_stdlib_source(const char *name, int *package){
    LibEntry *e=lib_find(name,0);
    if(!e || !(e->who&LIB_ME)) return NULL;
    if(package) *package= e->kind==LIB_PACKAGE;
    return lib_unpack(e);
}
const char *mpy_lib_text(const char *name){
    LibEntry *e=lib_find(name,1);
    return e ? lib_unpack(e) : NULL;
}
