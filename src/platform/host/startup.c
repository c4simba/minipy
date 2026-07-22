/* Host platform hooks: nothing special to do at startup/shutdown. */

#include "platform/platform.h"

void mpy_platform_init(void){}
void mpy_platform_shutdown(void){}

const char *mpy_platform_default_script(void){ return NULL; }

void mpy_platform_banner(const char *script_path){
    printf("MiniPy - Mini Python Interpreter\n");
    printf("Script: %s\n\n", script_path ? script_path : "(default)");
}
