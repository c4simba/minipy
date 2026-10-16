/* ========================= Target kolibri: a KolibriOS application =========================
   The listing is a flat MENUET01 binary for fasm; the runtime routines use
   their KolibriOS blocks (int 0x40: fn 68 memory, fn 70 files, fn 75
   sockets) and console.asm here talks to the shell's console. fasm writes
   the application; then it is LZMA-packed as kpack packs one (aot/kpack.c),
   which the kernel unpacks while loading it. On KolibriOS itself plainpyc
   runs the system's /sys/develop/fasm. See README.md. */

#include "targets/targets.h"
#include "util.h"
#include "fs.h"

int aot_fasm(AotBuild *b);           /* targets/linux/target.c */

static int build(AotBuild *b){
    if(aot_fasm(b)) return 1;
    if(!b->pack) return 0;
    char *rerr=NULL, *raw=ppy_fs_try_read_file(b->base,&rerr); size_t rawn=ppy_fs_last_len;
    if(!raw){ fprintf(stderr,"plainpy: %s\n",rerr?rerr:"cannot read the executable"); return 1; }
    unsigned char *packed=NULL; size_t pn=0;
    if(!aot_kpack((const unsigned char*)raw,rawn,&packed,&pn)){
        if(aot_write_file(b->base,(const char*)packed,pn,0)) return 1;
        if(b->verbose) printf("packed %s: %lu -> %lu bytes\n",b->base,(unsigned long)rawn,(unsigned long)pn);
        free(packed);
    }
    free(raw);
    return 0;
}

const AotTargetDef aot_target_kolibri={
    AOT_TARGET_KOLIBRI, "kolibri", "kolibrios", "kolibrios", AOT_ABI_KOLIBRI, AOT_OUT_FASM, AOT_CFN_NONE,
    65536, 65536, 65536,
    "KolibriOS application (MENUET01, int 0x40), written by fasm, kpacked",
    build
};
