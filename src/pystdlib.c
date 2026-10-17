/* The standard library modules written in Python, from plainpy.lib (see
   pystdlib.h; tools/pack.c writes the file and says how it is laid out). */
#include "pystdlib.h"
#include "targets/platform.h"
#include "inflate.h"
#include "fs.h"
#include "libid.h"
#if !defined(PPY_KOLIBRI) && !defined(PPY_WASM) && !defined(_WIN32)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#define LIB_MMAP 1
#endif

#define LIB_NAME "plainpy.lib"
enum { LIB_MODULE=0, LIB_PACKAGE=1, LIB_TEXT=2, LIB_MODULE_BC=3, LIB_PACKAGE_BC=4 };   /* (_BC: bytecode, stored as it is: plainpy --bytecode-lib) */
#ifdef PPY_LIB_COMPILER
#define LIB_ME 2                                     /* (who this program is, for the entries' "for" byte) */
#define LIB_PROGRAM "plainpyc"
#else
#define LIB_ME 1
#define LIB_PROGRAM "plainpy"
#endif

typedef struct { const char *name; int namelen, kind, who; PpyPacked p; } LibEntry;   /* (name: in the index, not NUL-terminated) */
static const unsigned char *lib_ix, *lib_data; static int nlib=-1;   /* the index and the data, where the file is: read there at each lookup (nothing copied) */

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

#ifdef PPY_LIB_EMBED
/* the library linked into the program (a microcontroller's flash: the C file
   `plainpy --bytecode-lib lib.c` writes), unless PLAINPYLIB names a file */
extern const unsigned char ppy_lib_data[];
extern const unsigned long ppy_lib_size;
#endif
/* the file mapped where it is (the system reads its pages when they are
   needed; they are not the heap's), else read */
static unsigned char *lib_map(const char *path, size_t *n){
#ifdef PPY_LIB_EMBED
    if(!getenv("PLAINPYLIB")){ *n=(size_t)ppy_lib_size; return (unsigned char*)ppy_lib_data; }
#endif
#ifdef LIB_MMAP
    int fd=open(path,O_RDONLY);
    if(fd>=0){
        struct stat st; void *m=MAP_FAILED;
        if(!fstat(fd,&st) && st.st_size>0) m=mmap(NULL,(size_t)st.st_size,PROT_READ,MAP_PRIVATE,fd,0);
        close(fd);
        if(m!=MAP_FAILED){ *n=(size_t)st.st_size; return (unsigned char*)m; }
    }
#endif
    char *err=NULL;
    unsigned char *b=(unsigned char*)ppy_fs_try_read_file(path,&err);
    if(!b){ if(err) lib_fail(NULL,err); lib_fail(path,"cannot read it"); }
    *n=ppy_fs_last_len;
    return b;
}

static void lib_load(void){
    if(nlib>=0) return;
    char *path=lib_path(); size_t n=0;
    unsigned char *b=lib_map(path,&n);
    if(n<20 || memcmp(b,"PPYLIB\0\1",8)) lib_fail(path,"not a plainpy library");
    if(rd32(b+8)!=PPY_LIB_ID) lib_fail(path,"the library of another build");
    unsigned count=rd32(b+12), isize=rd32(b+16);
    if(20+(size_t)isize>n) lib_fail(path,"damaged");
    const unsigned char *q=b+20, *end=q+isize, *data=end;
    for(unsigned i=0;i<count;i++){                    /* (checked once: the lookups read it as it is) */
        if(q+16>end) lib_fail(path,"damaged");
        unsigned l=q[2]|(unsigned)q[3]<<8;
        if(q+16+l>end) lib_fail(path,"damaged");
        q+=4+l;
        unsigned off=rd32(q), clen=rd32(q+4); q+=12;
        if((size_t)(data-b)+off+clen>n) lib_fail(path,"damaged");
    }
    lib_ix=b+20; lib_data=data;
    nlib=(int)count;                                  /* (the file stays: the entries point into it) */
    free(path);
}
static void lib_entry(const unsigned char **q, LibEntry *e){
    const unsigned char *p=*q; unsigned l=p[2]|(unsigned)p[3]<<8;
    e->kind=p[0]; e->who=p[1]; e->name=(const char*)p+4; e->namelen=(int)l; p+=4+l;
    e->p.data=lib_data+rd32(p); e->p.clen=rd32(p+4); e->p.len=rd32(p+8); e->p.text=NULL;
    *q=p+12;
}
static int lib_find(const char *name, int text, LibEntry *e){
    lib_load();
    size_t n=strlen(name); const unsigned char *q=lib_ix;
    for(int i=0;i<nlib;i++){ lib_entry(&q,e); if((e->kind==LIB_TEXT)==text && (size_t)e->namelen==n && !memcmp(e->name,name,n)) return 1; }
    return 0;
}
static const char *lib_unpack(LibEntry *e){
    const char *t=ppy_unpack(&e->p);
    if(!t){ char *path=lib_path(); lib_fail(path,"damaged"); }
    return t;
}

int ppy_stdlib_left_out(const char *name){
    LibEntry e;
    return lib_find(name,0,&e) && !(e.who&LIB_ME);
}
const char *ppy_stdlib_alias(const char *name){
    static const char *const aliases[][2]={{"os.path","posixpath"},{NULL,NULL}};
    for(int i=0;aliases[i][0];i++) if(!strcmp(aliases[i][0],name)) return aliases[i][1];
    return NULL;
}
char *ppy_stdlib_source(const char *name, int *package){
    LibEntry ent, *e=&ent;
    if(!lib_find(name,0,e) || !(e->who&LIB_ME) || e->kind>=LIB_MODULE_BC) return NULL;
    if(package) *package= e->kind==LIB_PACKAGE;
    char *t=(char*)malloc((size_t)e->p.len+1);           /* (not kept: compiled once, the text is not needed after) */
    if(!t || ppy_inflate(e->p.data,e->p.clen,(unsigned char*)t,e->p.len)){ char *path=lib_path(); lib_fail(path,t?"damaged":"out of memory"); }
    t[e->p.len]=0;
    return t;
}
const void *ppy_stdlib_bytecode(const char *name, size_t *len, int *package){
    LibEntry ent, *e=&ent;
    if(!lib_find(name,0,e) || !(e->who&LIB_ME) || e->kind<LIB_MODULE_BC) return NULL;
    if(package) *package= e->kind==LIB_PACKAGE_BC;
    *len=e->p.len;
    return e->p.data;
}
int ppy_stdlib_count(void){ lib_load(); return nlib; }
char *ppy_stdlib_module(int i, int *package){
    lib_load();
    if(i<0 || i>=nlib) return NULL;
    LibEntry e; const unsigned char *q=lib_ix;
    for(int k=0;k<=i;k++) lib_entry(&q,&e);
    if(!(e.who&LIB_ME) || (e.kind!=LIB_MODULE && e.kind!=LIB_PACKAGE)) return NULL;
    if(package) *package= e.kind==LIB_PACKAGE;
    return xstrndup2(e.name,e.namelen);
}
static void put32(unsigned char *b, unsigned v){ b[0]=(unsigned char)v; b[1]=(unsigned char)(v>>8); b[2]=(unsigned char)(v>>16); b[3]=(unsigned char)(v>>24); }
int ppy_lib_write_bytecode(const char *path, const PpyLibModule *m, int n, char **err){
    size_t isize=0, dsize=0;
    for(int i=0;i<n;i++){ isize+=16+strlen(m[i].name); dsize+=(m[i].len+3)&~(size_t)3; }
    isize=(isize+3)&~(size_t)3;                           /* (the data 4-aligned: code arrays used where they are) */
    size_t total=20+isize+dsize;
    unsigned char *b=(unsigned char*)xmalloc(total); memset(b,0,total);
    memcpy(b,"PPYLIB\0\1",8); put32(b+8,PPY_LIB_ID); put32(b+12,(unsigned)n); put32(b+16,(unsigned)isize);
    unsigned char *q=b+20, *d=b+20+isize; unsigned off=0;
    for(int i=0;i<n;i++){
        size_t l=strlen(m[i].name);
        q[0]=(unsigned char)(m[i].package?LIB_PACKAGE_BC:LIB_MODULE_BC); q[1]=LIB_ME; q[2]=(unsigned char)l; q[3]=(unsigned char)(l>>8);
        memcpy(q+4,m[i].name,l); q+=4+l;
        put32(q,off); put32(q+4,(unsigned)m[i].len); put32(q+8,(unsigned)m[i].len); q+=12;
        memcpy(d+off,m[i].data,m[i].len); off+=(unsigned)((m[i].len+3)&~(size_t)3);
    }
    int rc;
    size_t pl=strlen(path);
    if(pl>2 && !strcmp(path+pl-2,".c")){                 /* (a C file: the library as the array PPY_LIB_EMBED links in) */
        size_t cap=total*5+512, k=0; char *c=(char*)xmalloc(cap);
        k+=(size_t)snprintf(c+k,cap-k,"/* plainpy.lib as bytecode (plainpy --bytecode-lib): build the interpreter with\n"
                                     "   -DPPY_LIB_EMBED and this file - the modules are run from it where it is (flash) */\n"
                                     "const unsigned long ppy_lib_size=%lu;\n"
                                     "const unsigned char ppy_lib_data[] __attribute__((aligned(4)))={",(unsigned long)total);
        for(size_t i=0;i<total;i++) k+=(size_t)snprintf(c+k,cap-k,"%s%u,",i%24?"":"\n",b[i]);
        k+=(size_t)snprintf(c+k,cap-k,"\n};\n");
        rc=ppy_fs_write_file(path,c,k,err); free(c);
    } else rc=ppy_fs_write_file(path,(const char*)b,total,err);
    free(b);
    return rc;
}
const char *ppy_lib_text(const char *name){              /* (unpacked once: kept for the next asks) */
    static struct { char *name; const char *text; } kept[16]; static int nkept;
    for(int i=0;i<nkept;i++) if(!strcmp(kept[i].name,name)) return kept[i].text;
    LibEntry e;
    if(!lib_find(name,1,&e)) return NULL;
    const char *t=lib_unpack(&e);
    if(nkept<16){ kept[nkept].name=xstrdup2(name); kept[nkept].text=t; nkept++; }
    return t;
}
