/* ========================= ppy_port_posix.c: a port for a computer (Linux, macOS, any POSIX) =========================
   Runs a program of `plainpy --compile --target c` on the computer itself -
   to try a board's program before flashing it, or to check the C target:

     plainpy --compile --target c --port posix app.py      (writes app.c, this file, builds ./app)
     cc app.c ppy_port_posix.c -o app -lm                   (the same by hand)

   Files, sockets, processes and the rest go through targets/posix/lx_emul.c
   (the Linux calls done with the host's; its errors given back as Linux's
   numbers, which the program expects), and the device calls of
   lib/machine.py through its simulated board (targets/posix/devsim.c).
   Memory: an arena of PPY_ARENA bytes (environment; default 512 MiB, taken
   from the system only as the program touches it). */

#include "ppy_port.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#define LX_WORD uint32_t
#define LX_PTR(a) ppy_guest_ptr((uint32_t)(a))
#define LX_BLOCKING_BEGIN ((void)0)
#define LX_BLOCKING_END ((void)0)
#define LX_FLUSH() ((void)0)
#include "targets/posix/lx_emul.c"

/* the host's errno -> Linux's */
static const int ppy_errnos[][2]={
    {EPERM,1},{ENOENT,2},{ESRCH,3},{EINTR,4},{EIO,5},{ENXIO,6},{E2BIG,7},{ENOEXEC,8},{EBADF,9},{ECHILD,10},{EAGAIN,11},{ENOMEM,12},
    {EACCES,13},{EFAULT,14},{EBUSY,16},{EEXIST,17},{EXDEV,18},{ENODEV,19},{ENOTDIR,20},{EISDIR,21},{EINVAL,22},{ENFILE,23},{EMFILE,24},
    {ENOTTY,25},{EFBIG,27},{ENOSPC,28},{ESPIPE,29},{EROFS,30},{EMLINK,31},{EPIPE,32},{EDOM,33},{ERANGE,34},{EDEADLK,35},{ENAMETOOLONG,36},
    {ENOLCK,37},{ENOSYS,38},{ENOTEMPTY,39},{ELOOP,40},{ENOTSOCK,88},{EDESTADDRREQ,89},{EMSGSIZE,90},{EPROTOTYPE,91},{ENOPROTOOPT,92},
    {EPROTONOSUPPORT,93},{EOPNOTSUPP,95},{EAFNOSUPPORT,97},{EADDRINUSE,98},{EADDRNOTAVAIL,99},{ENETDOWN,100},{ENETUNREACH,101},
    {ECONNABORTED,103},{ECONNRESET,104},{ENOBUFS,105},{EISCONN,106},{ENOTCONN,107},{ETIMEDOUT,110},{ECONNREFUSED,111},{EHOSTUNREACH,113},
    {EALREADY,114},{EINPROGRESS,115}};
static int64_t ppy_linux_errno(int64_t v){
    if(v>=0 || v<-4095) return v;
    for(size_t i=0;i<sizeof ppy_errnos/sizeof ppy_errnos[0];i++) if(ppy_errnos[i][0]==(int)-v) return -ppy_errnos[i][1];
    return v;
}

void *ppy_port_arena(uint32_t *size){
    uint32_t n=*size;
    if(!n){ const char *e=getenv("PPY_ARENA"); n=e?(uint32_t)strtoul(e,NULL,0):512u<<20; }
    void *p=malloc(n);
    if(!p){ fprintf(stderr,"ppy_port_posix: cannot have %u bytes for the arena\n",n); exit(70); }
    *size=n;
    return p;
}
int32_t ppy_port_write(int fd, const void *buf, uint32_t n){ ssize_t w=write(fd,buf,n); return w<0 ? (int32_t)ppy_linux_errno(-errno) : (int32_t)w; }
int32_t ppy_port_read(int fd, void *buf, uint32_t n){ ssize_t r=read(fd,buf,n); return r<0 ? (int32_t)ppy_linux_errno(-errno) : (int32_t)r; }
uint64_t ppy_port_clock_ns(int clock){
    struct timespec ts; clock_gettime(clock==0?CLOCK_REALTIME:CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000000u+(uint64_t)ts.tv_nsec;
}
void ppy_port_sleep_ns(uint64_t ns){
    struct timespec ts, rem; ts.tv_sec=(time_t)(ns/1000000000u); ts.tv_nsec=(long)(ns%1000000000u);
    while(nanosleep(&ts,&rem) && errno==EINTR) ts=rem;
}
void ppy_port_random(void *buf, uint32_t n){
    uint32_t regs[7]={355,ppy_guest_addr(buf),n,0,0,0,0};       /* getrandom: lx_emul's */
    if(regs[1] && lx_syscall(regs)==(int64_t)n) return;
    for(uint32_t i=0;i<n;i++) ((unsigned char*)buf)[i]=(unsigned char)rand();
}
int64_t ppy_port_syscall(const uint32_t r[7]){ return ppy_linux_errno(lx_syscall(r)); }
int ppy_port_errno(void){ return (int)-ppy_linux_errno(-(int64_t)errno); }
void ppy_port_panic(const char *msg){ (void)msg; exit(70); }

int main(int argc, char **argv){
    signal(SIGPIPE,SIG_IGN);
    return ppy_main(argc,argv);
}
