#ifndef MPY_PLATFORM_H
#define MPY_PLATFORM_H

/* Platform layer.
 *
 * This header MUST be the first thing every translation unit sees (it is
 * pulled in transitively through util.h and every module header). It provides
 * the C standard headers and, on KolibriOS, the macro overrides for
 * setjmp/longjmp/malloc/printf that the whole codebase relies on. If those
 * overrides are not established before any use, host and kolibri builds would
 * silently diverge -- hence the "include me first" rule.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <setjmp.h>
#include <stddef.h>

/* Unify the three legacy kolibri feature macros into one internal flag. */
#if defined(MPY_PLATFORM_KOLIBRI) || defined(MPY_TARGET_KOLIBRIOS) || defined(MPY_FS_KOLIBRI)
#define MPY_KOLIBRI 1
#endif

#if defined(MPY_KOLIBRI)

/* Replace libc setjmp/longjmp with GCC builtins - the DLL-based libc setjmp
   requires TLS/reent setup that we don't have. */
#undef setjmp
#undef longjmp
#define setjmp(buf)      __builtin_setjmp((void**)(buf))
#define longjmp(buf,val) __builtin_longjmp((void**)(buf),1)

/* KolibriOS console interface (implemented in platform/kolibri/console.c). */
int  kol_console_init(void);
void kol_console_deinit(void);
void kol_console_printf(const char *fmt, ...);
void kol_console_puts(const char *s);
void kol_console_gets(char *buf, int maxlen);
void kol_console_cls(void);

/* Dual printf: format to a buffer, echo to stderr AND the console.
   Defined in platform/kolibri/console.c. */
void _dual_printf(const char *fmt, ...);
#undef printf
#define printf(...) _dual_printf(__VA_ARGS__)

/* Page allocator over f68.12/f68.13 (libc malloc crashes without TLS setup).
   Every block carries a 16-byte header (magic + usable size) so realloc copies
   exactly the old contents (not the larger new size, which used to overread the
   old block and fault) and free can hand the pages back. Defined in startup.c. */
void *kos_malloc(size_t n);
void *kos_realloc(void *p, size_t n);
void  kos_free(void *p);
#define malloc(n)    kos_malloc(n)
#define realloc(p,n) kos_realloc(p,n)
#define free(p)      kos_free(p)

#endif /* MPY_KOLIBRI */

/* Platform lifecycle / entry hooks. Implemented per platform under
   platform/host/ and platform/kolibri/. */
void        mpy_platform_init(void);              /* early startup (reloc, heap) */
void        mpy_platform_shutdown(void);          /* teardown */
const char *mpy_platform_default_script(void);    /* NULL when none (host) */
void        mpy_platform_banner(const char *script_path);

/* Command line as delivered by the platform. The host has a real argv and
   returns NULL (main() uses argc/argv unchanged). KolibriOS has no argv: the
   kernel copies the launch arguments into a header buffer, returned here, and
   main() splits it into argc/argv itself. exe_path backs argv[0] if available. */
const char *mpy_platform_cmdline(void);
const char *mpy_platform_exe_path(void);

/* Raw syscall gateway (backs the built-in `sys.syscall`).
   mpy_platform_has_syscall(): 1 only on the KolibriOS build, 0 elsewhere.
   mpy_platform_syscall(): loads eax..edi from in[0..5], executes `int 0x40`,
   and stores the resulting eax..edi into out[0..5]. (ebp is not exposed --
   GCC owns it as the frame pointer -- which covers the normal KolibriOS ABI.) */
int         mpy_platform_has_syscall(void);
int         mpy_platform_syscall(const uint32_t in[6], uint32_t out[6]);

#endif /* MPY_PLATFORM_H */
