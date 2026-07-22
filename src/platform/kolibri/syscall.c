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

int mpy_platform_syscall(const uint32_t in[6], uint32_t out[6]){
    uint32_t r_eax, r_ebx, r_ecx, r_edx, r_esi, r_edi;
    __asm__ __volatile__(
        "int $0x40"
        : "=a"(r_eax), "=b"(r_ebx), "=c"(r_ecx), "=d"(r_edx), "=S"(r_esi), "=D"(r_edi)
        : "a"(in[0]), "b"(in[1]), "c"(in[2]), "d"(in[3]), "S"(in[4]), "D"(in[5])
        : "memory", "cc");
    out[0]=r_eax; out[1]=r_ebx; out[2]=r_ecx; out[3]=r_edx; out[4]=r_esi; out[5]=r_edi;
    return 1;
}
