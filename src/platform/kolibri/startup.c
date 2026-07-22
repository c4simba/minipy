/* KolibriOS platform hooks: runtime relocation, process-heap init, page
   allocator, console lifecycle. */

#include "platform/platform.h"
#include "util.h"

/* Page allocator over f68 (libc malloc crashes without TLS setup).
   f68.12: eax=68, ebx=12, ecx=pages          -> allocate (1 page = 4096 bytes)
   f68.13: eax=68, ebx=13, ecx=block          -> free a block from f68.12
   Layout: [ magic(4) | size(4) | pad(8) ][ payload ... ]. The returned pointer
   is payload = base+16 (16-aligned, since f68.12 blocks are page-aligned). The
   header lets realloc copy the right amount and free release the right block. */
#define KOS_HEAD   16u
#define KOS_MAGIC  0x484f534bu   /* 'KOSH' */

void *kos_malloc(size_t n){
    /* f68.12: ecx = size in BYTES (the kernel rounds up to a page internally).
       Passing a page COUNT here under-allocated everything over 4 KB. */
    size_t total = n + KOS_HEAD;
    unsigned *base;
    __asm__ __volatile__("int $0x40" : "=a"(base) : "a"(68), "b"(12), "c"(total) : "memory");
    if(!base || (int)base <= 0) die("out of memory");
    base[0] = KOS_MAGIC;
    base[1] = (unsigned)n;
    return (char*)base + KOS_HEAD;
}

void kos_free(void *p){
    if(!p) return;
    unsigned *base = (unsigned*)((char*)p - KOS_HEAD);
    if(base[0] != KOS_MAGIC) return;   /* not ours (foreign/libc pointer): leave it */
    base[0] = 0;                       /* poison so a double free is a no-op */
    __asm__ __volatile__("int $0x40" :: "a"(68), "b"(13), "c"(base) : "memory");
}

void *kos_realloc(void *p, size_t n){
    void *np = kos_malloc(n);
    if(p){
        unsigned *base = (unsigned*)((char*)p - KOS_HEAD);
        size_t old = (base[0] == KOS_MAGIC) ? base[1] : n;
        memcpy(np, p, old < n ? old : n);
        kos_free(p);
    }
    return np;
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
    /* Bring the console up now (needs the heap) so *all* output -- including
       early exits like -v/--version and usage -- is visible, not just script
       output printed after the banner. Idempotent; the banner may call it too. */
    kol_console_init();
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

/* MENUET header buffers (see kos-app-fix.lds): the kernel copies the launch
   command line into ___kosapp_cmdline and the program path into ___kosapp_name.
   The PE toolchain prepends one underscore, so the C names carry two. */
extern char __kosapp_cmdline[];
extern char __kosapp_name[];
const char *mpy_platform_cmdline(void){ return __kosapp_cmdline; }
const char *mpy_platform_exe_path(void){ return __kosapp_name; }

void mpy_platform_banner(const char *script_path){
    kol_console_init();
    kol_console_puts("MiniPy - KolibriOS\n\r");
    kol_console_puts(script_path ? script_path : "(default)");
    kol_console_puts("\n\r");
}
