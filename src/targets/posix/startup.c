/* Host platform hooks: at startup SIGPIPE is ignored (as in CPython); nothing else to do. */

#include "targets/platform.h"
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

void ppy_platform_init(void){ signal(SIGPIPE,SIG_IGN); }   /* (as CPython: writing to a closed socket or pipe is an EPIPE error) */
void ppy_platform_shutdown(void){}

const char *ppy_platform_default_script(void){ return NULL; }

void ppy_platform_banner(const char *script_path){ (void)script_path; }   /* (as python: the program's output only) */

/* No raw syscalls on a hosted OS: sys.syscall raises "wrong platform". */
int ppy_platform_has_syscall(void){ return 0; }
int ppy_platform_syscall(const uint32_t in[7], uint32_t out[6]){ (void)in; (void)out; return 0; }

/* Host has real argc/argv: return NULL so main() leaves them untouched. */
const char *ppy_platform_cmdline(void){ return NULL; }
void ppy_platform_make_executable(const char *path){
    struct stat st;
    if(stat(path,&st)==0) chmod(path,st.st_mode|((st.st_mode&0444)>>2));
}

/* the running executable, symbolic links resolved (plainpy.lib is next to it), or NULL */
const char *ppy_platform_exe_path(void){
    static char path[PATH_MAX]; static int done;
    if(done) return path[0] ? path : NULL;
    done=1;
    char raw[PATH_MAX]; raw[0]=0;
#if defined(__APPLE__)
    uint32_t n=sizeof raw;
    if(_NSGetExecutablePath(raw,&n)!=0) raw[0]=0;
#elif defined(__linux__)
    ssize_t n=readlink("/proc/self/exe",raw,sizeof raw-1);
    raw[n>0?n:0]=0;
#endif
    if(raw[0] && !realpath(raw,path)) snprintf(path,sizeof path,"%s",raw);
    return path[0] ? path : NULL;
}

int ppy_platform_exec(const char *path, char **argv){
    if(strchr(path,'/')) execv(path,argv); else execvp(path,argv);
    return -1;
}

int ppy_platform_run(const char *program, const char *args){
    size_t n=strlen(program)+strlen(args)+2;
    char *cmd=(char*)malloc(n);
    if(!cmd) return -1;
    snprintf(cmd,n,"%s %s",program,args);
    int rc=system(cmd);
    free(cmd);
    if(rc==-1) return -1;
    if(WIFEXITED(rc)) return WEXITSTATUS(rc);   /* 127: the shell could not find the program */
    return 128+(WIFSIGNALED(rc)?WTERMSIG(rc):0);
}
