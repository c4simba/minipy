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

/* f68.12 page allocator (libc malloc crashes without TLS).
   Defined in platform/kolibri/startup.c. */
void *kos_malloc(size_t n);
#define malloc(n)    kos_malloc(n)
#define realloc(p,n) ({ void *_p = kos_malloc(n); if(p){ memcpy(_p, p, n); free(p); } _p; })
#define free(p)      /* no-op - KolibriOS heap manages pages */

#endif /* MPY_KOLIBRI */

/* Platform lifecycle / entry hooks. Implemented per platform under
   platform/host/ and platform/kolibri/. */
void        mpy_platform_init(void);              /* early startup (reloc, heap) */
void        mpy_platform_shutdown(void);          /* teardown */
const char *mpy_platform_default_script(void);    /* NULL when none (host) */
void        mpy_platform_banner(const char *script_path);

#endif /* MPY_PLATFORM_H */
