/* KolibriOS platform hooks: runtime relocation, process-heap init, page
   allocator, console lifecycle. */

#include "platform/platform.h"

/* f68.12 page allocator (libc malloc crashes without TLS setup).
   f68.12: eax=68, ebx=12, ecx=pages (1 page = 4096 bytes). */
void *kos_malloc(size_t n){
    int pages = (int)((n + 4095) / 4096);
    void *p;
    __asm__ __volatile__("int $0x40" : "=a"(p) : "a"(68), "b"(12), "c"(pages) : "memory");
    if(!p || (int)p <= 0) die("out of memory");
    return p;
}

extern void _pei386_runtime_relocator(void);

void mpy_platform_init(void){
    fprintf(stderr, "[minipy] start\n"); fflush(stderr);
    fprintf(stderr, "[minipy] calling reloc\n"); fflush(stderr);
    _pei386_runtime_relocator();
    fprintf(stderr, "[minipy] reloc ok\n"); fflush(stderr);
    /* Initialize process heap (required before any malloc) - f68.11 */
    {
        int heap_ptr;
        __asm__ __volatile__("int $0x40"
            : "=a"(heap_ptr)
            : "a"(68), "b"(11)
            : "memory");
        fprintf(stderr, "[minipy] heap init: ptr=0x%x\n", heap_ptr);
        fflush(stderr);
    }
}

void mpy_platform_shutdown(void){
    kol_console_deinit();
}

#ifndef MPY_FS_DEFAULT_IMPORT_DIR
#define MPY_FS_DEFAULT_IMPORT_DIR "."
#endif
#ifndef MPY_DEFAULT_SCRIPT
#define MPY_DEFAULT_SCRIPT MPY_FS_DEFAULT_IMPORT_DIR "/main.mpy"
#endif

const char *mpy_platform_default_script(void){ return MPY_DEFAULT_SCRIPT; }

void mpy_platform_banner(const char *script_path){
    kol_console_init();
    kol_console_puts("MiniPy - KolibriOS\n\r");
    kol_console_puts(script_path ? script_path : "(default)");
    kol_console_puts("\n\r");
}
