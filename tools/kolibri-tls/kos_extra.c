/* What plainpy's mbedtls.obj exports besides mbedTLS's functions */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "mbedtls/sha256.h"

/* the port's mbedtls.obj resolves its imports (libc.obj, network.obj) here; this one has none */
int mbedtls_init(void){ return 0; }

static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d){
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}
static inline uint64_t rdtsc(void){ uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi<<32)|lo; }
static int rdrand(uint32_t *v){ unsigned char ok; __asm__ volatile("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok)); return ok; }
static inline uint32_t k26_10(void){ uint32_t r; __asm__ volatile("int $0x40" : "=a"(r) : "a"(26), "b"(10) : "edx", "memory"); return r; }

/* len bytes to seed a random generator with (0: done): SHA-256 of what the
   CPU's RDRAND gives, where it has one, and of the time stamp counter's jitter
   (thousands of timings of a little work: the KolibriOS kernel has no source of
   its own), the high-precision counter (function 26.10) and the stack's place */
int ppy_entropy(unsigned char *out, size_t len){
    static uint32_t pool[1024];
    uint32_t a, b, c, d, has_rdrand=0, have_tsc=0;
    cpuid(0,&a,&b,&c,&d);
    if(a>=1){ cpuid(1,&a,&b,&c,&d); has_rdrand=(c>>30)&1; have_tsc=(d>>4)&1; }
    volatile uint32_t work=0;
    for(int i=0;i<1024;i++){
        uint32_t v=0;
        if(has_rdrand){ for(int k=0;k<10 && !rdrand(&v);k++); }
        uint64_t t0= have_tsc ? rdtsc() : 0;
        for(int k=0;k<(int)(t0&15)+8;k++) work+=k*(uint32_t)t0;
        uint64_t t1= have_tsc ? rdtsc() : 0;
        pool[i]=v^(uint32_t)(t1-t0)^((uint32_t)t1<<11)^k26_10()^(uint32_t)(uintptr_t)&v^work;
    }
    unsigned char block[32]; uint32_t ctr=0;
    while(len){
        unsigned char in[sizeof pool+4];
        memcpy(in,pool,sizeof pool); memcpy(in+sizeof pool,&ctr,4); ctr++;
        if(mbedtls_sha256_ret(in,sizeof in,block,0)!=0) return -1;
        size_t n= len<32 ? len : 32;
        memcpy(out,block,n); out+=n; len-=n;
    }
    return 0;
}
