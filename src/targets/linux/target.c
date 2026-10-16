/* ========================= Target linux: an i386 ELF executable =========================
   The listing (aot/codegen) is a whole program for fasm: `format ELF
   executable`, the runtime routines' Linux blocks (int 0x80), and - when the
   program calls C functions (ctypes) - the ELF's dynamic section, so the
   system's loader links libc and the other libraries. fasm writes the
   executable; nothing else is needed to run it on any x86 Linux (64-bit
   ones run i386 programs; ctypes needs the i386 C library there).
   See README.md. */

#include "targets/targets.h"
#include "targets/platform.h"
#include "util.h"
#include "fs.h"

#ifndef PPY_AOT_FASM                 /* override with --fasm/--fasm-args or PPY_FASM/PPY_FASM_ARGS */
#  if defined(PPY_KOLIBRI)
#    define PPY_AOT_FASM "/sys/develop/fasm"
#  else
#    define PPY_AOT_FASM "fasm"
#  endif
#endif
#ifndef PPY_AOT_FASM_ARGS            /* {in} {out} {dir} are substituted */
#  if defined(PPY_KOLIBRI)
#    define PPY_AOT_FASM_ARGS "{in},{out},{dir}"
#  else
#    define PPY_AOT_FASM_ARGS "-m 262144 {in} {out}"
#  endif
#endif

/* fasm: the listing -> base. (Also the kolibri target's first step.) */
int aot_fasm(AotBuild *b){
    const char *fasm=b->fasm?b->fasm:PPY_AOT_FASM, *tmpl=b->fasm_args?b->fasm_args:PPY_AOT_FASM_ARGS;
    ppy_fs_remove(b->base);                                 /* (fasm's exit status is not visible on KolibriOS) */
    char *args=aot_tool_args(tmpl,b->asm_path,b->base);
    int rc=aot_run_tool(fasm,args,"--fasm",b->verbose); free(args);
    if(rc) return 1;
    if(!ppy_fs_exists(b->base)){ fprintf(stderr,"plainpy: fasm did not produce %s (see its output)\n",b->base); return 1; }
    return 0;
}

static int build(AotBuild *b){
    if(aot_fasm(b)) return 1;
    ppy_platform_make_executable(b->base);                  /* (fasm leaves it 0644) */
    return 0;
}

const AotTargetDef aot_target_linux={
    AOT_TARGET_LINUX, "linux", "", "linux", AOT_ABI_LINUX, AOT_OUT_FASM, AOT_CFN_DYNAMIC,
    65536, 65536, 0,
    "i386 Linux executable (ELF, int 0x80), written by fasm",
    build
};
