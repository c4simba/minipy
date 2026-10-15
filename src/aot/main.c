/* ========================= minipyc: the typed compiler's entry point =========================
   minipyc [options] file.py: the program, compiled to a standalone executable
   (aot/driver.c; minipyc --help). A program of its own, without the
   interpreter: `minipy --compile ...` runs it. */

#include "platform/platform.h"
#include "aot/aot.h"
#include "fs.h"

#include "version.h"                  /* MPY_VERSION: make writes it (build/gen) from VERSION */

int main(int argc,char **argv){
    mpy_platform_init();
    atexit(mpy_platform_shutdown);
    char cmdbuf[1024]; char *kargv[64];                     /* (KolibriOS: the arguments as one string) */
    const char *kcmd=mpy_platform_cmdline();
    if(kcmd){ argc=mpy_split_cmdline(kcmd,mpy_platform_exe_path(),"minipyc",cmdbuf,(int)sizeof cmdbuf,kargv,64); argv=kargv; }
    const char *program=(argc>0 && argv && argv[0]) ? argv[0] : "minipyc";
    if(argc>=2 && (!strcmp(argv[1],"-v") || !strcmp(argv[1],"--version"))){ printf("minipyc %s\n",MPY_VERSION); return 0; }
    mpy_fs_set_program(program);                             /* (modules in the lib folder next to it) */
    return aot_main(argc-1,argv+1,program);
}
