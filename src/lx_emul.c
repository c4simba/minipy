/* ========================= i386 Linux system calls on another host =========================
   A program's sys.syscall (and the runtime of compiled programs) speaks the i386 Linux
   system call interface: int 0x80 numbers, Linux's flags and structures. On any other
   host (and on a 64-bit Linux) the calls are done here with the host's own, converting
   what differs: flag bits, socket options and address families, struct stat64 /
   linux_dirent64 / sockaddr layouts. Errors stay the host's errno values (-errno), as
   CPython reports them there: the standard library picks its tables by sys.platform.

   This file is #included twice: by the interpreter (i_modules.c, sys.syscall) and,
   as text, by the C programs of the macos target (aot_x2c_rt.c). The includer defines
     LX_WORD                 the type of a register value (an address is one of them)
     LX_PTR(a)               the host pointer for an address a program passed
     LX_BLOCKING_BEGIN/END   around a call that may wait (the interpreter lets its other
                             threads run meanwhile)
     LX_FLUSH()              before output to fd 1 or 2 and before exit (buffered print)
   and calls lx_syscall(r) with r[0..6] = eax, ebx, ecx, edx, esi, edi, ebp: the result
   (eax), negative for -errno. */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <poll.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#define LX_RET(x) do{ int64_t x_=(int64_t)(x); return x_<0 ? -(int64_t)errno : x_; }while(0)

/* ---- flags */
static int lx_open_flags(LX_WORD f){
    int r=(int)(f&3);
    if(f&0x40) r|=O_CREAT;
    if(f&0x80) r|=O_EXCL;
    if(f&0x100) r|=O_NOCTTY;
    if(f&0x200) r|=O_TRUNC;
    if(f&0x400) r|=O_APPEND;
    if(f&0x800) r|=O_NONBLOCK;
    if(f&0x10000) r|=O_DIRECTORY;
    if(f&0x80000) r|=O_CLOEXEC;
    return r;
}
static uint32_t lx_from_open_flags(int f){
    uint32_t r=(uint32_t)(f&O_ACCMODE);
    if(f&O_APPEND) r|=0x400;
    if(f&O_NONBLOCK) r|=0x800;
    return r;
}
static void lx_put32(void *at, uint32_t v){ if(at) memcpy(at,&v,4); }
/* signal numbers: Linux's (i386) <-> the host's */
static const int lx_sigs[][2]={{1,SIGHUP},{2,SIGINT},{3,SIGQUIT},{4,SIGILL},{5,SIGTRAP},{6,SIGABRT},{7,SIGBUS},{8,SIGFPE},{9,SIGKILL},
    {10,SIGUSR1},{11,SIGSEGV},{12,SIGUSR2},{13,SIGPIPE},{14,SIGALRM},{15,SIGTERM},{17,SIGCHLD},{18,SIGCONT},{19,SIGSTOP},{20,SIGTSTP},
    {21,SIGTTIN},{22,SIGTTOU},{23,SIGURG},{24,SIGXCPU},{25,SIGXFSZ},{26,SIGVTALRM},{27,SIGPROF},{28,SIGWINCH},{29,SIGIO},{31,SIGSYS}};
static int lx_signal(int s){ for(size_t i=0;i<sizeof lx_sigs/sizeof lx_sigs[0];i++) if(lx_sigs[i][0]==s) return lx_sigs[i][1]; return s; }
static int lx_from_signal(int s){ for(size_t i=0;i<sizeof lx_sigs/sizeof lx_sigs[0];i++) if(lx_sigs[i][1]==s) return lx_sigs[i][0]; return s; }
static void lx_put64(void *at, uint64_t v){ if(at) memcpy(at,&v,8); }
static uint32_t lx_get32(const void *at){ uint32_t v=0; if(at) memcpy(&v,at,4); return v; }

/* ---- struct stat64 (96 bytes) */
static void lx_stat64(void *out, const struct stat *st){
    unsigned char b[96]; memset(b,0,sizeof b);
    uint64_t dev=(uint64_t)st->st_dev, rdev=(uint64_t)st->st_rdev, ino=(uint64_t)st->st_ino;
    memcpy(b+0,&dev,8);
    uint32_t ino32=(uint32_t)ino; memcpy(b+12,&ino32,4);
    uint32_t mode=(uint32_t)st->st_mode, nlink=(uint32_t)st->st_nlink, uid=(uint32_t)st->st_uid, gid=(uint32_t)st->st_gid;
    memcpy(b+16,&mode,4); memcpy(b+20,&nlink,4); memcpy(b+24,&uid,4); memcpy(b+28,&gid,4);
    memcpy(b+32,&rdev,8);
    int64_t size=(int64_t)st->st_size; memcpy(b+44,&size,8);
    uint32_t blksize=(uint32_t)st->st_blksize; memcpy(b+52,&blksize,4);
    uint64_t blocks=(uint64_t)st->st_blocks; memcpy(b+56,&blocks,8);
#if defined(__APPLE__)
    uint32_t at=(uint32_t)st->st_atimespec.tv_sec, atn=(uint32_t)st->st_atimespec.tv_nsec, mt=(uint32_t)st->st_mtimespec.tv_sec, mtn=(uint32_t)st->st_mtimespec.tv_nsec,
             ct=(uint32_t)st->st_ctimespec.tv_sec, ctn=(uint32_t)st->st_ctimespec.tv_nsec;
#else
    uint32_t at=(uint32_t)st->st_atim.tv_sec, atn=(uint32_t)st->st_atim.tv_nsec, mt=(uint32_t)st->st_mtim.tv_sec, mtn=(uint32_t)st->st_mtim.tv_nsec,
             ct=(uint32_t)st->st_ctim.tv_sec, ctn=(uint32_t)st->st_ctim.tv_nsec;
#endif
    memcpy(b+64,&at,4); memcpy(b+68,&atn,4); memcpy(b+72,&mt,4); memcpy(b+76,&mtn,4); memcpy(b+80,&ct,4); memcpy(b+84,&ctn,4);
    memcpy(b+88,&ino,8);
    memcpy(out,b,sizeof b);
}

/* ---- getdents64: a directory stream per descriptor */
typedef struct { int fd; DIR *d; } LxDir;
static LxDir lx_dirs[64];
static int64_t lx_getdents64(int fd, unsigned char *out, uint32_t count){
    LxDir *e=NULL;
    for(int i=0;i<64;i++) if(lx_dirs[i].d && lx_dirs[i].fd==fd) e=&lx_dirs[i];
    if(!e){
        for(int i=0;i<64 && !e;i++) if(!lx_dirs[i].d) e=&lx_dirs[i];
        if(!e) return -EMFILE;                              /* EMFILE */
        int dfd=dup(fd); if(dfd<0) return -(int64_t)errno;
        DIR *d=fdopendir(dfd); if(!d){ int er=errno; close(dfd); return -(int64_t)er; }
        e->fd=fd; e->d=d;
    }
    uint32_t n=0;
    for(;;){
        long pos=telldir(e->d);
        errno=0;
        struct dirent *de=readdir(e->d);
        if(!de){ if(errno) return -(int64_t)errno; break; }
        size_t nl=strlen(de->d_name);
        uint32_t reclen=(uint32_t)((19+nl+1+7)&~(size_t)7);
        if(n+reclen>count){ seekdir(e->d,pos); if(!n) return -EINVAL; break; }
        memset(out+n,0,reclen);
        uint64_t ino=(uint64_t)de->d_ino; int64_t off=(int64_t)(n+reclen);
        memcpy(out+n,&ino,8); memcpy(out+n+8,&off,8);
        uint16_t rl=(uint16_t)reclen; memcpy(out+n+16,&rl,2);
        unsigned char ty=0;
#ifdef DT_DIR
        switch(de->d_type){ case DT_REG: ty=8; break; case DT_DIR: ty=4; break; case DT_LNK: ty=10; break; case DT_FIFO: ty=1; break;
            case DT_SOCK: ty=12; break; case DT_CHR: ty=2; break; case DT_BLK: ty=6; break; default: ty=0; }
#endif
        out[n+18]=ty;
        memcpy(out+n+19,de->d_name,nl+1);
        n+=reclen;
    }
    return n;
}
static void lx_dir_closed(int fd){ for(int i=0;i<64;i++) if(lx_dirs[i].d && lx_dirs[i].fd==fd){ closedir(lx_dirs[i].d); lx_dirs[i].d=NULL; } }

/* ---- sockets: Linux's constants and sockaddr layout */
static int lx_family(uint32_t f){ return f==1?AF_UNIX:f==2?AF_INET:f==10?AF_INET6:f==0?AF_UNSPEC:-1; }
static uint32_t lx_from_family(int f){ return f==AF_UNIX?1:f==AF_INET?2:f==AF_INET6?10:(uint32_t)f; }
/* a Linux sockaddr (family: 2 bytes, then the same fields as the host's) -> the host's */
static int lx_sockaddr_in(const unsigned char *a, uint32_t len, struct sockaddr_storage *out, socklen_t *outlen){
    if(!a || len<2 || len>sizeof *out) return -EINVAL;
    memset(out,0,sizeof *out);
    int fam=lx_family((uint32_t)(a[0]|a[1]<<8)); if(fam<0) return -EAFNOSUPPORT;
    memcpy((unsigned char*)out+2,a+2,len-2);
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    out->ss_len=(unsigned char)len;
#endif
    out->ss_family=(sa_family_t)fam;
    *outlen=(socklen_t)len;
    return 0;
}
static void lx_sockaddr_out(const struct sockaddr_storage *a, socklen_t alen, unsigned char *out, void *outlenp){
    if(!out || !outlenp) return;
    uint32_t cap=lx_get32(outlenp), n=(uint32_t)alen;
    unsigned char b[sizeof(struct sockaddr_storage)]; memset(b,0,sizeof b);
    uint16_t fam=(uint16_t)lx_from_family(a->ss_family); memcpy(b,&fam,2);
    if(n>2) memcpy(b+2,(const unsigned char*)a+2,n-2);
    memcpy(out,b,n<cap?n:cap);
    lx_put32(outlenp,n);
}
static int lx_msg_flags(LX_WORD f){
    int r=0;
    if(f&1) r|=MSG_OOB;
    if(f&2) r|=MSG_PEEK;
    if(f&0x40) r|=MSG_DONTWAIT;
    if(f&0x100) r|=MSG_WAITALL;
#ifdef MSG_NOSIGNAL
    if(f&0x4000) r|=MSG_NOSIGNAL;
#endif
    return r;
}
/* (level, name) of Linux -> the host's; 0 when the option is unknown */
static int lx_sockopt(uint32_t level, uint32_t name, int *hlevel, int *hname){
    if(level==1){                                       /* SOL_SOCKET */
        static const int m[][2]={{1,SO_DEBUG},{2,SO_REUSEADDR},{3,SO_TYPE},{4,SO_ERROR},{5,SO_DONTROUTE},{6,SO_BROADCAST},{7,SO_SNDBUF},
            {8,SO_RCVBUF},{9,SO_KEEPALIVE},{10,SO_OOBINLINE},{13,SO_LINGER},{18,SO_RCVLOWAT},{19,SO_SNDLOWAT},{20,SO_RCVTIMEO},{21,SO_SNDTIMEO},
#ifdef SO_REUSEPORT
            {15,SO_REUSEPORT},
#endif
            {0,0}};
        for(int i=0;m[i][0];i++) if((uint32_t)m[i][0]==name){ *hlevel=SOL_SOCKET; *hname=m[i][1]; return 1; }
        return 0;
    }
    if(level==6){                                       /* IPPROTO_TCP */
        if(name==1){ *hlevel=IPPROTO_TCP; *hname=TCP_NODELAY; return 1; }
#if defined(TCP_KEEPIDLE)
        if(name==4){ *hlevel=IPPROTO_TCP; *hname=TCP_KEEPIDLE; return 1; }
#elif defined(TCP_KEEPALIVE)
        if(name==4){ *hlevel=IPPROTO_TCP; *hname=TCP_KEEPALIVE; return 1; }
#endif
#ifdef TCP_KEEPINTVL
        if(name==5){ *hlevel=IPPROTO_TCP; *hname=TCP_KEEPINTVL; return 1; }
#endif
#ifdef TCP_KEEPCNT
        if(name==6){ *hlevel=IPPROTO_TCP; *hname=TCP_KEEPCNT; return 1; }
#endif
        return 0;
    }
    if(level==0){                                       /* IPPROTO_IP */
        if(name==1){ *hlevel=IPPROTO_IP; *hname=IP_TOS; return 1; }
        if(name==2){ *hlevel=IPPROTO_IP; *hname=IP_TTL; return 1; }
        return 0;
    }
    if(level==41){                                      /* IPPROTO_IPV6 */
#ifdef IPV6_V6ONLY
        if(name==26){ *hlevel=IPPROTO_IPV6; *hname=IPV6_V6ONLY; return 1; }
#endif
        return 0;
    }
    if(level==17 || level==255) return 0;
    return 0;
}
static int64_t lx_new_socket(int fd, LX_WORD type){
    if(fd<0) return -(int64_t)errno;
#ifdef SO_NOSIGPIPE
    { int one=1; setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof one); }   /* EPIPE, not a SIGPIPE */
#endif
    if(type&0x800){ int fl=fcntl(fd,F_GETFL); fcntl(fd,F_SETFL,fl|O_NONBLOCK); }
    if(type&0x80000) fcntl(fd,F_SETFD,FD_CLOEXEC);
    return fd;
}

static int64_t lx_syscall(const LX_WORD *r){
    #define P(i) ((void*)LX_PTR(r[i]))
    switch((uint32_t)r[0]){
        case 1: case 252: LX_FLUSH(); exit((int)r[1]);
        case 3:{ ssize_t n; LX_BLOCKING_BEGIN; n=read((int)r[1],P(2),(size_t)(uint32_t)r[3]); LX_BLOCKING_END; LX_RET(n); }
        case 4: if(r[1]==1||r[1]==2) LX_FLUSH();
            { ssize_t n; LX_BLOCKING_BEGIN; n=write((int)r[1],P(2),(size_t)(uint32_t)r[3]); LX_BLOCKING_END; LX_RET(n); }
        case 5: LX_RET(open((const char*)P(1),lx_open_flags(r[2]),(int)r[3]));
        case 6: lx_dir_closed((int)r[1]); LX_RET(close((int)r[1]));
        case 9: LX_RET(link((const char*)P(1),(const char*)P(2)));
        case 10: LX_RET(unlink((const char*)P(1)));
        case 12: LX_RET(chdir((const char*)P(1)));
        case 13:{ uint32_t t=(uint32_t)time(NULL); lx_put32(P(1),t); return t; }
        case 15: LX_RET(chmod((const char*)P(1),(mode_t)r[2]));
        case 19: LX_RET(lseek((int)r[1],(off_t)(int32_t)r[2],(int)r[3]));
        case 20: return getpid();
        case 24: case 199: return getuid();
        case 47: case 200: return getgid();
        case 49: case 201: return geteuid();
        case 50: case 202: return getegid();
        case 33: LX_RET(access((const char*)P(1),(int)r[2]));
        case 37: LX_RET(kill((pid_t)r[1],lx_signal((int)r[2])));      /* kill */
        case 2: case 190:{ LX_FLUSH(); pid_t p=fork(); LX_RET(p); }    /* fork, vfork (a fork) */
        case 11:{                                                      /* execve: argv, envp - arrays of addresses */
            char *av[1024], *ev[4096]; int n=0;
            const LX_WORD *a=(const LX_WORD*)P(2);
            for(;a && n<1023 && a[n];n++) av[n]=(char*)LX_PTR(a[n]);
            av[n]=NULL; n=0;
            const LX_WORD *e=(const LX_WORD*)P(3);
            for(;e && n<4095 && e[n];n++) ev[n]=(char*)LX_PTR(e[n]);
            ev[n]=NULL;
            LX_FLUSH();
            execve((const char*)P(1),av,ev);
            return -(int64_t)errno; }
        case 7: case 114:{                                             /* waitpid / wait4 (pid, status, options) */
            int st=0, opt=0; if(r[3]&1) opt|=WNOHANG; if(r[3]&2) opt|=WUNTRACED;
            pid_t p; LX_BLOCKING_BEGIN; p=waitpid((pid_t)r[1],&st,opt); LX_BLOCKING_END;
            if(p<0) return -(int64_t)errno;
            if(p>0 && r[2]){ uint32_t ls= WIFEXITED(st) ? (uint32_t)(WEXITSTATUS(st)<<8) : WIFSIGNALED(st) ? (uint32_t)lx_from_signal(WTERMSIG(st)) : WIFSTOPPED(st) ? (uint32_t)((lx_from_signal(WSTOPSIG(st))<<8)|0x7f) : (uint32_t)st;
                lx_put32(P(2),ls); }                                   /* (Linux's encoding) */
            return p; }
        case 38: LX_RET(rename((const char*)P(1),(const char*)P(2)));
        case 39: LX_RET(mkdir((const char*)P(1),(mode_t)r[2]));
        case 40: LX_RET(rmdir((const char*)P(1)));
        case 41: LX_RET(dup((int)r[1]));
        case 42:{ int fds[2]; if(pipe(fds)) return -(int64_t)errno; lx_put32(P(1),(uint32_t)fds[0]); lx_put32((char*)P(1)+4,(uint32_t)fds[1]); return 0; }
        case 54:{                                       /* ioctl */
            int fd=(int)r[1]; uint32_t req=(uint32_t)r[2];
            if(req==0x5401){ if(isatty(fd)) return 0; return errno==EBADF?-EBADF:-ENOTTY; }   /* TCGETS: is it a terminal */
            if(req==0x5413){ struct winsize ws; if(ioctl(fd,TIOCGWINSZ,&ws)) return -(int64_t)errno;   /* TIOCGWINSZ */
                uint16_t w[4]={ws.ws_row,ws.ws_col,ws.ws_xpixel,ws.ws_ypixel}; memcpy(P(3),w,8); return 0; }
            if(req==0x541B){ int n=0; if(ioctl(fd,FIONREAD,&n)) return -(int64_t)errno; lx_put32(P(3),(uint32_t)n); return 0; }   /* FIONREAD */
            return -ENOTTY; }
        case 63: LX_RET(dup2((int)r[1],(int)r[2]));
        case 64: return getppid();
        case 78:{ struct timeval tv; gettimeofday(&tv,NULL);
            lx_put32(P(1),(uint32_t)tv.tv_sec); if(r[1]) lx_put32((char*)P(1)+4,(uint32_t)tv.tv_usec);
            lx_put32(P(2),0); if(r[2]) lx_put32((char*)P(2)+4,0);
            return 0; }
        case 83: LX_RET(symlink((const char*)P(1),(const char*)P(2)));
        case 60: return umask((mode_t)r[1]);
        case 271:{ const int32_t *tv=(const int32_t*)P(2); struct timeval t[2];         /* utimes */
            if(!r[2]) LX_RET(utimes((const char*)P(1),NULL));
            t[0].tv_sec=tv[0]; t[0].tv_usec=tv[1]; t[1].tv_sec=tv[2]; t[1].tv_usec=tv[3]; LX_RET(utimes((const char*)P(1),t)); }
        case 320:{ const int32_t *q=(const int32_t*)P(3); struct timespec ts[2];         /* utimensat: Linux's UTIME_NOW / UTIME_OMIT, AT_FDCWD, AT_SYMLINK_NOFOLLOW */
            int dirfd=(int32_t)r[1]==-100 ? AT_FDCWD : (int)r[1], fl=(r[4]&0x100) ? AT_SYMLINK_NOFOLLOW : 0;
            if(!r[3]) LX_RET(utimensat(dirfd,(const char*)P(2),NULL,fl));
            for(int i=0;i<2;i++){ ts[i].tv_sec=q[2*i]; ts[i].tv_nsec= q[2*i+1]==0x3FFFFFFF ? UTIME_NOW : q[2*i+1]==0x3FFFFFFE ? UTIME_OMIT : q[2*i+1]; }
            LX_RET(utimensat(dirfd,(const char*)P(2),ts,fl)); }
        case 85: LX_RET(readlink((const char*)P(1),(char*)P(2),(size_t)(uint32_t)r[3]));
        case 92: LX_RET(truncate((const char*)P(1),(off_t)(int32_t)r[2]));
        case 93: LX_RET(ftruncate((int)r[1],(off_t)(int32_t)r[2]));
        case 94: LX_RET(fchmod((int)r[1],(mode_t)r[2]));
        case 118: LX_RET(fsync((int)r[1]));
        case 122:{ struct utsname u; if(uname(&u)) return -(int64_t)errno;   /* struct new_utsname: 6 x 65 bytes */
            char *o=(char*)P(1); memset(o,0,6*65);
            const char *f[5]={u.sysname,u.nodename,u.release,u.version,u.machine};
            for(int i=0;i<5;i++){ strncpy(o+65*i,f[i],64); }
            return 0; }
        case 125: case 174: case 175: return 0;        /* mprotect, rt_sigaction, rt_sigprocmask */
        case 140:{ int64_t off=((int64_t)(uint32_t)r[2]<<32)|(uint32_t)r[3];   /* _llseek */
            off_t o=lseek((int)r[1],(off_t)off,(int)r[5]); if(o<0) return -(int64_t)errno;
            lx_put64(P(4),(uint64_t)o); return 0; }
        case 148: LX_RET(fsync((int)r[1]));            /* fdatasync */
        case 162:{ int32_t q[2]; memcpy(q,P(1),8); struct timespec ts, rem; ts.tv_sec=q[0]; ts.tv_nsec=q[1];
            int rc; LX_BLOCKING_BEGIN; rc=nanosleep(&ts,&rem); LX_BLOCKING_END;
            if(rc){ int e=errno; lx_put32(P(2),(uint32_t)rem.tv_sec); if(r[2]) lx_put32((char*)P(2)+4,(uint32_t)rem.tv_nsec); return -(int64_t)e; }
            return 0; }
        case 168:{ int rc; LX_BLOCKING_BEGIN; rc=poll((struct pollfd*)P(1),(nfds_t)(uint32_t)r[2],(int)(int32_t)r[3]); LX_BLOCKING_END; LX_RET(rc); }
        case 183: if(!getcwd((char*)P(1),(size_t)(uint32_t)r[2])) return -(int64_t)errno; return (int64_t)strlen((char*)P(1))+1;
        case 193:{ int64_t len=((int64_t)(uint32_t)r[3]<<32)|(uint32_t)r[2]; LX_RET(truncate((const char*)P(1),(off_t)len)); }
        case 194:{ int64_t len=((int64_t)(uint32_t)r[3]<<32)|(uint32_t)r[2]; LX_RET(ftruncate((int)r[1],(off_t)len)); }
        case 195: case 196: case 197:{ struct stat st; int rc;                   /* stat64, lstat64, fstat64 */
            if(r[0]==195) rc=stat((const char*)P(1),&st); else if(r[0]==196) rc=lstat((const char*)P(1),&st); else rc=fstat((int)r[1],&st);
            if(rc) return -(int64_t)errno;
            lx_stat64(P(2),&st); return 0; }
        case 220: return lx_getdents64((int)r[1],(unsigned char*)P(2),(uint32_t)r[3]);
        case 221:{ int fd=(int)r[1]; uint32_t cmd=(uint32_t)r[2];   /* fcntl64 */
            if(cmd==1){ int v=fcntl(fd,F_GETFD); if(v<0) return -(int64_t)errno; return (v&FD_CLOEXEC)?1:0; }
            if(cmd==2) LX_RET(fcntl(fd,F_SETFD,(r[3]&1)?FD_CLOEXEC:0));
            if(cmd==3){ int v=fcntl(fd,F_GETFL); if(v<0) return -(int64_t)errno; return lx_from_open_flags(v); }
            if(cmd==4){ int v=fcntl(fd,F_GETFL); if(v<0) return -(int64_t)errno;
                v&=~(O_APPEND|O_NONBLOCK); if(r[3]&0x400) v|=O_APPEND; if(r[3]&0x800) v|=O_NONBLOCK; LX_RET(fcntl(fd,F_SETFL,v)); }
            return -EINVAL; }
        case 265:{ struct timespec ts; clockid_t id=r[1]==1?CLOCK_MONOTONIC:r[1]==2?CLOCK_PROCESS_CPUTIME_ID:r[1]==3?CLOCK_THREAD_CPUTIME_ID:CLOCK_REALTIME;
            clock_gettime(id,&ts); lx_put32(P(2),(uint32_t)ts.tv_sec); lx_put32((char*)P(2)+4,(uint32_t)ts.tv_nsec); return 0; }
        case 331:{ int fds[2]; if(pipe(fds)) return -(int64_t)errno;   /* pipe2 */
            if(r[2]&0x800){ fcntl(fds[0],F_SETFL,fcntl(fds[0],F_GETFL)|O_NONBLOCK); fcntl(fds[1],F_SETFL,fcntl(fds[1],F_GETFL)|O_NONBLOCK); }
            if(r[2]&0x80000){ fcntl(fds[0],F_SETFD,FD_CLOEXEC); fcntl(fds[1],F_SETFD,FD_CLOEXEC); }   /* O_CLOEXEC */
            lx_put32(P(1),(uint32_t)fds[0]); lx_put32((char*)P(1)+4,(uint32_t)fds[1]); return 0; }
        case 355:{ unsigned char *b=(unsigned char*)P(1); size_t n=(size_t)(uint32_t)r[2];   /* getrandom */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
            arc4random_buf(b,n); return (int64_t)n;
#else
            int fd=open("/dev/urandom",O_RDONLY); if(fd<0) return -(int64_t)errno;
            size_t got=0; while(got<n){ ssize_t k=read(fd,b+got,n-got); if(k<=0) break; got+=(size_t)k; } close(fd); return (int64_t)got;
#endif
        }
        /* ---- sockets (the direct calls of Linux 4.3+) */
        case 359:{ int fam=lx_family((uint32_t)r[1]); if(fam<0) return -EAFNOSUPPORT;
            return lx_new_socket(socket(fam,(int)(r[2]&0xF),(int)r[3]),r[2]); }
        case 360:{ int fam=lx_family((uint32_t)r[1]); int sv[2]; if(fam<0) return -EAFNOSUPPORT;
            if(socketpair(fam,(int)(r[2]&0xF),(int)r[3],sv)) return -(int64_t)errno;
            lx_new_socket(sv[0],r[2]); lx_new_socket(sv[1],r[2]);
            lx_put32(P(4),(uint32_t)sv[0]); lx_put32((char*)P(4)+4,(uint32_t)sv[1]); return 0; }
        case 361: case 362:{ struct sockaddr_storage a; socklen_t al; int e=lx_sockaddr_in((const unsigned char*)P(2),(uint32_t)r[3],&a,&al); if(e) return e;
            if(r[0]==361) LX_RET(bind((int)r[1],(struct sockaddr*)&a,al));
            int rc; LX_BLOCKING_BEGIN; rc=connect((int)r[1],(struct sockaddr*)&a,al); LX_BLOCKING_END; LX_RET(rc); }
        case 363: LX_RET(listen((int)r[1],(int)r[2]));
        case 364:{ struct sockaddr_storage a; socklen_t al=sizeof a; int fd;      /* accept4 */
            LX_BLOCKING_BEGIN; fd=accept((int)r[1],(struct sockaddr*)&a,&al); LX_BLOCKING_END;
            if(fd<0) return -(int64_t)errno;
            if(r[2]) lx_sockaddr_out(&a,al,(unsigned char*)P(2),P(3));
            return lx_new_socket(fd,r[4]); }
        case 365:{ int hl, hn; if(!lx_sockopt((uint32_t)r[2],(uint32_t)r[3],&hl,&hn)) return -ENOPROTOOPT;   /* getsockopt */
            uint32_t cap=lx_get32(P(5));
            if(hl==SOL_SOCKET && (hn==SO_RCVTIMEO||hn==SO_SNDTIMEO)){ struct timeval tv; socklen_t l=sizeof tv; if(getsockopt((int)r[1],hl,hn,&tv,&l)) return -(int64_t)errno;
                if(cap>=8){ lx_put32(P(4),(uint32_t)tv.tv_sec); lx_put32((char*)P(4)+4,(uint32_t)tv.tv_usec); } lx_put32(P(5),8); return 0; }
            if(hl==SOL_SOCKET && hn==SO_LINGER){ struct linger lg; socklen_t l=sizeof lg; if(getsockopt((int)r[1],hl,hn,&lg,&l)) return -(int64_t)errno;
                if(cap>=8){ lx_put32(P(4),(uint32_t)lg.l_onoff); lx_put32((char*)P(4)+4,(uint32_t)lg.l_linger); } lx_put32(P(5),8); return 0; }
            int v=0; socklen_t l=sizeof v; if(getsockopt((int)r[1],hl,hn,&v,&l)) return -(int64_t)errno;
                        if(hl==SOL_SOCKET && hn==SO_TYPE) v=v==SOCK_STREAM?1:v==SOCK_DGRAM?2:v==SOCK_RAW?3:v;
            if(cap>=4) lx_put32(P(4),(uint32_t)v); lx_put32(P(5),4); return 0; }
        case 366:{ int hl, hn; if(!lx_sockopt((uint32_t)r[2],(uint32_t)r[3],&hl,&hn)) return -ENOPROTOOPT;   /* setsockopt */
            if(hl==SOL_SOCKET && (hn==SO_RCVTIMEO||hn==SO_SNDTIMEO)){ struct timeval tv; tv.tv_sec=(int32_t)lx_get32(P(4)); tv.tv_usec=(int32_t)lx_get32((char*)P(4)+4);
                LX_RET(setsockopt((int)r[1],hl,hn,&tv,sizeof tv)); }
            if(hl==SOL_SOCKET && hn==SO_LINGER){ struct linger lg; lg.l_onoff=(int)lx_get32(P(4)); lg.l_linger=(int)lx_get32((char*)P(4)+4); LX_RET(setsockopt((int)r[1],hl,hn,&lg,sizeof lg)); }
            int v=(int)lx_get32(P(4)); LX_RET(setsockopt((int)r[1],hl,hn,&v,sizeof v)); }
        case 367: case 368:{ struct sockaddr_storage a; socklen_t al=sizeof a;   /* getsockname, getpeername */
            int rc= r[0]==367 ? getsockname((int)r[1],(struct sockaddr*)&a,&al) : getpeername((int)r[1],(struct sockaddr*)&a,&al);
            if(rc) return -(int64_t)errno;
            lx_sockaddr_out(&a,al,(unsigned char*)P(2),P(3)); return 0; }
        case 369:{ struct sockaddr_storage a; socklen_t al=0; ssize_t n;          /* sendto */
            if(r[5]){ int e=lx_sockaddr_in((const unsigned char*)P(5),(uint32_t)r[6],&a,&al); if(e) return e; }
            LX_BLOCKING_BEGIN; n=sendto((int)r[1],P(2),(size_t)(uint32_t)r[3],lx_msg_flags(r[4]),r[5]?(struct sockaddr*)&a:NULL,al); LX_BLOCKING_END;
            LX_RET(n); }
        case 371:{ struct sockaddr_storage a; socklen_t al=sizeof a; ssize_t n;    /* recvfrom */
            LX_BLOCKING_BEGIN; n=recvfrom((int)r[1],P(2),(size_t)(uint32_t)r[3],lx_msg_flags(r[4]),(struct sockaddr*)&a,&al); LX_BLOCKING_END;
            if(n<0) return -(int64_t)errno;
            if(r[5] && al>0) lx_sockaddr_out(&a,al,(unsigned char*)P(5),P(6));
            else if(r[6]) lx_put32(P(6),0);
            return n; }
        case 373: LX_RET(shutdown((int)r[1],(int)r[2]));
        default:
            return -ENOSYS;                                 /* ENOSYS */
    }
    #undef P
}
