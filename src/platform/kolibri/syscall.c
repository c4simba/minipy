/* KolibriOS raw syscall gateway (int 0x40), backing the built-in sys.syscall.
 *
 * KolibriOS syscalls use registers, not a byte protocol: eax = function number,
 * ebx/ecx/edx/esi/edi = arguments (often packed coordinates, colours, or a
 * pointer to a buffer/ASCIIZ string), and results come back in the same
 * registers. So the gateway is register-in / register-out.
 *
 * See https://wiki.kolibrios.org/wiki/SysFn (function reference).
 */

#include "platform/platform.h"

int mpy_platform_has_syscall(void){ return 1; }

/* ebp is GCC's frame pointer, so it cannot be an asm operand: the block's
   address comes in eax and the asm loads every register itself, keeping ebp. */
int mpy_platform_syscall(const uint32_t in[7], uint32_t out[6]){
    uint32_t r_eax, r_ebx, r_ecx, r_edx, r_esi, r_edi;
    __asm__ __volatile__(
        "pushl %%ebp\n\t"
        "movl 24(%%eax),%%ebp\n\t"
        "movl 4(%%eax),%%ebx\n\t"
        "movl 8(%%eax),%%ecx\n\t"
        "movl 12(%%eax),%%edx\n\t"
        "movl 16(%%eax),%%esi\n\t"
        "movl 20(%%eax),%%edi\n\t"
        "movl (%%eax),%%eax\n\t"
        "int $0x40\n\t"
        "popl %%ebp"
        : "=a"(r_eax), "=b"(r_ebx), "=c"(r_ecx), "=d"(r_edx), "=S"(r_esi), "=D"(r_edi)
        : "a"(in)
        : "memory", "cc");
    out[0]=r_eax; out[1]=r_ebx; out[2]=r_ecx; out[3]=r_edx; out[4]=r_esi; out[5]=r_edi;
    return 1;
}
