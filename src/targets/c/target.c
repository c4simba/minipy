/* ========================= Target c: the program as portable C, for a port =========================
   The listing is the Linux one; aot/x2c.c translates it into one C file
   whose runtime is x2c_port.c here: memory from an arena, the system through
   the functions of ppy_port.h, no C library needed. A port is a C file that
   gives those functions for a board (or a system); the program and the port
   build with any C compiler for any 32- or 64-bit little-endian processor.

     plainpy --compile --target c app.py                  app.c, ppy_port.h
     plainpy --compile --target c --port template app.py  ... and ppy_port_template.c to fill in
     plainpy --compile --target c --port posix app.py     ... and ppy_port_posix.c, built into ./app

   See README.md (the porting guide). */

#include "targets/targets.h"
#include "util.h"
#include "fs.h"

int aot_write_c(AotBuild *b, const X2cOptions *xo, const char *c_path, size_t *clen);   /* targets/macos/target.c */
int aot_cc(AotBuild *b, const char *c_path, const char *more, size_t clen_hint);

/* "dir/" for a file's folder ("" for the current one) */
char *aot_dir_prefix(const char *path){
    char *d=ppy_fs_dirname(path);
    if(!strcmp(d,".")){ d[0]=0; return d; }
    size_t n=strlen(d); char *r=(char*)xmalloc(n+2); memcpy(r,d,n+1);
    if(n && r[n-1]!='/'){ r[n]='/'; r[n+1]=0; }
    free(d); return r;
}

/* the port's files in dir ("" or "dir/"): ppy_port.h always, a port's C file
   when one is named (its path: *port_path) */
int aot_write_port_files(AotBuild *b, const char *dir, const char *port, char **port_path){
    char *h=aot_concat(dir,"ppy_port.h");
    const char *t=aot_lib_text("port_h");
    int rc=aot_write_file(h,t,strlen(t),b->verbose); free(h);
    if(rc) return 1;
    if(port_path) *port_path=NULL;
    if(!port) return 0;
    const char *text= !strcmp(port,"posix") ? "port_posix" : !strcmp(port,"template") ? "port_template" : NULL;
    if(!text){ fprintf(stderr,"plainpy: unknown port '%s' (posix, template)\n",port); return 1; }
    char name[64]; snprintf(name,sizeof name,"ppy_port_%s.c",port);
    char *p=aot_concat(dir,name);
    if(!strcmp(port,"template") && ppy_fs_exists(p)) printf("plainpy: %s is there already: left as it is\n",p);   /* (a port being written) */
    else { t=aot_lib_text(text); if(aot_write_file(p,t,strlen(t),b->verbose)){ free(p); return 1; } }
    if(port_path) *port_path=p; else free(p);
    return 0;
}

static int build(AotBuild *b){
    X2cOptions xo; memset(&xo,0,sizeof xo); xo.mode=X2C_PORT; xo.stack=b->stack; xo.arena=b->arena;
    char *c_path=aot_concat(b->base,".c"), *dir=aot_dir_prefix(c_path), *port_path=NULL; size_t clen=0;
    if(aot_write_c(b,&xo,c_path,&clen) || aot_write_port_files(b,dir,b->port,&port_path)) return 1;
    int rc=0;
    if(b->port && !strcmp(b->port,"posix")){               /* a program for this computer: built */
        char *q=aot_tool_args("{in}",port_path,b->base);
        rc=aot_cc(b,c_path,q,clen);
        free(q);
    } else printf("wrote %s, %sppy_port.h%s%s: build them with a port (src/targets/c/README.md)\n",c_path,dir,port_path?", ":"",port_path?port_path:"");
    free(c_path); free(dir); free(port_path);
    return rc;
}

const AotTargetDef aot_target_c={
    AOT_TARGET_C, "c", "", "baremetal", AOT_ABI_LINUX, AOT_OUT_C, AOT_CFN_STATIC,
    16384, 8192, 16384,
    "portable C (FILE.c + ppy_port.h) for a port: a microcontroller, any system",
    build
};
