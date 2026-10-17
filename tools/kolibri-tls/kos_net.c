/* mbedTLS's network I/O over KolibriOS's sockets (function 75: 6 send, 7
   receive, 1 close) - the port's net_sockets.c goes through libc.obj and
   network.obj; a socket is the number function 75.0 gave (the fd of plainpy's
   socket.socket there). The *_nb ones do not wait (MSG_DONTWAIT): for a socket
   with a timeout, the caller waits between WANT_READ / WANT_WRITE. */

#include <stddef.h>
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

static inline int k75(int sub, int a, int b, int c, int d, int *err){
    int r, e;
    __asm__ volatile("int $0x40" : "=a"(r), "=b"(e) : "a"(75), "b"(sub), "c"(a), "d"(b), "S"(c), "D"(d) : "memory");
    if(err) *err=e;
    return r;
}
static int failed(int err, int sending){
    if(err==6) return sending ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_SSL_WANT_READ;   /* (would block) */
    if(err==52 || err==53) return MBEDTLS_ERR_NET_CONN_RESET;
    return sending ? MBEDTLS_ERR_NET_SEND_FAILED : MBEDTLS_ERR_NET_RECV_FAILED;
}
void mbedtls_net_init(mbedtls_net_context *ctx){ ctx->fd=-1; }
void mbedtls_net_free(mbedtls_net_context *ctx){
    if(ctx->fd<0) return;
    k75(1,ctx->fd,0,0,0,0);
    ctx->fd=-1;
}
static int net_send(void *ctx, const unsigned char *buf, size_t len, int flags){
    int fd=((mbedtls_net_context*)ctx)->fd, err;
    if(fd<0) return MBEDTLS_ERR_NET_INVALID_CONTEXT;
    int r=k75(6,fd,(int)buf,(int)len,flags,&err);
    return r<0 ? failed(err,1) : r;
}
static int net_recv(void *ctx, unsigned char *buf, size_t len, int flags){
    int fd=((mbedtls_net_context*)ctx)->fd, err;
    if(fd<0) return MBEDTLS_ERR_NET_INVALID_CONTEXT;
    int r=k75(7,fd,(int)buf,(int)len,flags,&err);
    return r<0 ? failed(err,0) : r;
}
int mbedtls_net_send(void *ctx, const unsigned char *buf, size_t len){ return net_send(ctx,buf,len,0); }
int mbedtls_net_recv(void *ctx, unsigned char *buf, size_t len){ return net_recv(ctx,buf,len,0); }
int ppy_net_send_nb(void *ctx, const unsigned char *buf, size_t len){ return net_send(ctx,buf,len,0x40); }
int ppy_net_recv_nb(void *ctx, unsigned char *buf, size_t len){ return net_recv(ctx,buf,len,0x40); }
