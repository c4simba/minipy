/* The standard library modules written in Python (see pystdlib.h). */
#include "pystdlib.h"
#include "platform/platform.h"

typedef struct { const char *name; int package; const char *src; } StdModule;
static const StdModule std_modules[]={
#include "stdlib.inc"
    {NULL,0,NULL}
};

const char *mpy_stdlib_alias(const char *name){
    static const char *const aliases[][2]={{"os.path","posixpath"},{NULL,NULL}};
    for(int i=0;aliases[i][0];i++) if(!strcmp(aliases[i][0],name)) return aliases[i][1];
    return NULL;
}
const char *mpy_stdlib_source(const char *name, int *package){
    for(int i=0;std_modules[i].name;i++) if(!strcmp(std_modules[i].name,name)){
        if(package) *package=std_modules[i].package;
        return std_modules[i].src;
    }
    return NULL;
}
