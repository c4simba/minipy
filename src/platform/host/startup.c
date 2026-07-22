/* Host platform hooks: nothing special to do at startup/shutdown. */

#include "platform/platform.h"

void mpy_platform_init(void){}
void mpy_platform_shutdown(void){}

const char *mpy_platform_default_script(void){ return NULL; }

void mpy_platform_banner(const char *script_path){
    printf("MiniPy - Mini Python Interpreter\n");
    printf("Script: %s\n\n", script_path ? script_path : "(default)");
}

/* No raw syscalls on a hosted OS: sys.syscall raises "wrong platform". */
int mpy_platform_has_syscall(void){ return 0; }
int mpy_platform_syscall(const uint32_t in[6], uint32_t out[6]){ (void)in; (void)out; return 0; }

/* Host has real argc/argv: return NULL so main() leaves them untouched. */
const char *mpy_platform_cmdline(void){ return NULL; }
const char *mpy_platform_exe_path(void){ return NULL; }
