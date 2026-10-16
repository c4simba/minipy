/* ========================= Target macos: a native executable through C =========================
   The listing is the Linux one; aot/x2c.c translates it into one C file
   (static recompilation: the i386 machine kept in C variables) whose runtime
   is x2c_host.c here - a 4 GiB guest address space, the Linux system calls
   done with the host's (targets/posix/lx_emul.c), C functions (ctypes) found
   with dlopen. The system C compiler builds it into a native program (arm64
   on Apple silicon). See README.md. */

#include "targets/targets.h"
#include "util.h"
#include "fs.h"

/* the C compiler: override with --cc/--cc-args or PPY_CC/PPY_CC_ARGS */
#ifndef PPY_AOT_CC
#  define PPY_AOT_CC "cc"
#endif
#ifndef PPY_AOT_CC_ARGS              /* {in} {out} {dir} are substituted */
#  define PPY_AOT_CC_ARGS "-O1 -w {in} -o {out} -lm"
#endif

/* The listing as C (xo->mode) -> c_path (its length: *clen); 0, or 1 after an error. */
int aot_write_c(AotBuild *b, const X2cOptions *xo, const char *c_path, size_t *clen){
    char *csrc=NULL; size_t n=0;
    if(aot_x2c(b->listing,b->len,xo,&csrc,&n)) return 1;
    int rc=aot_write_file(c_path,csrc,n,b->verbose);
    free(csrc);
    if(clen) *clen=n;
    return rc;
}

/* cc: base.c (and more C files, space-separated, already quoted) -> base */
int aot_cc(AotBuild *b, const char *c_path, const char *more, size_t clen_hint){
    const char *cc=b->cc?b->cc:PPY_AOT_CC, *tmpl=b->cc_args?b->cc_args:PPY_AOT_CC_ARGS;
    char big[256], with[1024];
    #if defined(__APPLE__)
    if(!b->cc_args && clen_hint>400000){                  /* a big program: one huge C function, whose register */
        snprintf(big,sizeof big,"%s -mllvm -join-liveintervals=false -mllvm -enable-misched=false",PPY_AOT_CC_ARGS); tmpl=big; }   /* coalescing and instruction scheduling take clang most of the time */
    #else
    (void)big; (void)clen_hint;
    #endif
    if(more && *more){ snprintf(with,sizeof with,"%s %s",tmpl,more); tmpl=with; }
    ppy_fs_remove(b->base);
    char *args=aot_tool_args(tmpl,c_path,b->base);
    int rc=aot_run_tool(cc,args,"--cc",b->verbose); free(args);
    if(rc) return 1;
    if(!ppy_fs_exists(b->base)){ fprintf(stderr,"plainpy: %s did not produce %s\n",cc,b->base); return 1; }
    return 0;
}

static int build(AotBuild *b){
    X2cOptions xo; memset(&xo,0,sizeof xo); xo.mode=X2C_HOST;
    char *c_path=aot_concat(b->base,".c"); size_t clen=0;
    if(aot_write_c(b,&xo,c_path,&clen)) return 1;
    int rc=aot_cc(b,c_path,NULL,clen);
    free(c_path);
    return rc;
}

const AotTargetDef aot_target_macos={
    AOT_TARGET_MACOS, "macos", "darwin", "darwin", AOT_ABI_LINUX, AOT_OUT_C, AOT_CFN_DYNAMIC,
    65536, 65536, 0,
    "native macOS executable: the listing as C (FILE.c), built by cc",
    build
};
