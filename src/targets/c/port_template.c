/* ========================= ppy_port_template.c: the port of a new board, to fill in =========================
   plainpy --compile --target c --port template app.py writes app.c,
   ppy_port.h and this file. Every function here works as it is - the
   program runs, prints into nowhere and knows no time - so the steps are:

     1. ppy_port_write: send the bytes to your UART (or your debugger's
        console); now print() works.
     2. ppy_port_clock_ns / ppy_port_sleep_ns: a timer; now time works.
     3. ppy_port_arena: where the program's memory is (the static array is
        fine; PPY_ARENA_BYTES).
     4. Device calls (lib/machine.py) in ppy_port_syscall, as you need them.
     5. Call ppy_main() from your startup code (see board_main at the end).

   Build with the board's C compiler, for example
     arm-none-eabi-gcc -mcpu=cortex-m4 -O2 -ffreestanding -c app.c ppy_port_template.c
   and link as the board links its programs (-lgcc for the 64-bit and double
   arithmetic; -lm only when the program calls exp, log, sin, pow ...).
   Without any C library define PPY_PORT_NO_LIBC: then memcpy, memset and
   memmove are here (the C compiler may call them for its own copies).
   src/targets/c/README.md is the guide. */

#include "ppy_port.h"
#include <stdint.h>
#include <stddef.h>

/* ---- memory: the program's data, heap and stacks */
#ifndef PPY_ARENA_BYTES
#define PPY_ARENA_BYTES (96u*1024u)
#endif
static uint64_t ppy_arena_mem[PPY_ARENA_BYTES/8];       /* (uint64_t: aligned) */
void *ppy_port_arena(uint32_t *size){
    *size=(uint32_t)sizeof ppy_arena_mem;               /* (what plainpy --arena asked is in *size: here it is the array's size) */
    return ppy_arena_mem;
}

/* ---- output and input */
static void board_putchar(char c){ (void)c; /* TODO: write c to the UART's data register */ }
static int board_getchar(void){ return -1; /* TODO: a received byte, or -1 when there is none */ }

int32_t ppy_port_write(int fd, const void *buf, uint32_t n){
    (void)fd;                                           /* stdout and stderr both to the console */
    const char *p=(const char*)buf;
    for(uint32_t i=0;i<n;i++){ if(p[i]=='\n') board_putchar('\r'); board_putchar(p[i]); }
    return (int32_t)n;
}
int32_t ppy_port_read(int fd, void *buf, uint32_t n){
    (void)fd;
    char *p=(char*)buf; uint32_t k=0;
    while(k<n){                                         /* a line, as a terminal gives it */
        int c=board_getchar();
        if(c<0){ if(k) break; continue; }
        if(c=='\r') c='\n';
        p[k++]=(char)c;
        if(c=='\n') break;
    }
    return (int32_t)k;
}

/* ---- time */
static volatile uint64_t board_ticks_ms;                /* TODO: a timer interrupt adds 1 every millisecond */
uint64_t ppy_port_clock_ns(int clock){ (void)clock; return board_ticks_ms*1000000u; }
void ppy_port_sleep_ns(uint64_t ns){
    uint64_t until=board_ticks_ms+ns/1000000u;
    while(board_ticks_ms<until){ /* TODO: sleep until the next interrupt (wfi) */ if(!board_ticks_ms) break; }
}

/* ---- random bytes: a hardware RNG if the chip has one */
void ppy_port_random(void *buf, uint32_t n){
    static uint32_t x=2463534242u;
    unsigned char *p=(unsigned char*)buf;
    for(uint32_t i=0;i<n;i++){ x^=x<<13; x^=x>>17; x^=x<<5; p[i]=(unsigned char)x; }
}

/* ---- everything else: files, sockets, and the device calls of lib/machine.py */
int64_t ppy_port_syscall(const uint32_t r[7]){
    switch(r[0]){
    case PPY_DEV_PIN_INIT:  /* r[1] pin, r[2] mode (0 in, 1 out, 2 open drain), r[3] pull (0, 1 up, 2 down) */
        return -PPY_ENOSYS; /* TODO: configure the GPIO; return 0 */
    case PPY_DEV_PIN_WRITE: /* r[1] pin, r[2] value */
        return -PPY_ENOSYS; /* TODO */
    case PPY_DEV_PIN_READ:  /* r[1] pin -> 0/1 */
        return -PPY_ENOSYS; /* TODO */
    /* ... PPY_DEV_ADC_*, PWM_*, I2C_*, SPI_*, UART_* as ppy_port.h describes them;
       buffers: unsigned char *b=ppy_guest_ptr(r[3]) */
    default:
        return -PPY_ENOSYS;
    }
}

/* ---- the program cannot go on */
void ppy_port_panic(const char *msg){
    ppy_port_write(2,"panic: ",7);
    uint32_t n=0; while(msg[n]) n++;
    ppy_port_write(2,msg,n); ppy_port_write(2,"\n",1);
    for(;;){}                                           /* TODO: or reset the chip */
}

#ifdef PPY_PORT_NO_LIBC
void *memcpy(void *d, const void *s, size_t n){ unsigned char *a=(unsigned char*)d; const unsigned char *b=(const unsigned char*)s; while(n--) *a++=*b++; return d; }
void *memmove(void *d, const void *s, size_t n){ unsigned char *a=(unsigned char*)d; const unsigned char *b=(const unsigned char*)s;
    if(a<b){ while(n--) *a++=*b++; } else { a+=n; b+=n; while(n--) *--a=*--b; } return d; }
void *memset(void *d, int c, size_t n){ unsigned char *a=(unsigned char*)d; while(n--) *a++=(unsigned char)c; return d; }
#endif

/* ---- start: call this from the board's startup code (main(), a task ...) */
void board_main(void){
    static char name[]="app", *argv[]={name,0};
    int status=ppy_main(1,argv);
    (void)status;                                       /* the program ended: its exit status */
    for(;;){}
}
