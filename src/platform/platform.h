#ifndef PPY_PLATFORM_H
#define PPY_PLATFORM_H

/* Platform layer.
 *
 * This header MUST be the first thing every translation unit sees (it is
 * pulled in transitively through util.h and every module header). It provides
 * the C standard headers and, on KolibriOS, the macro overrides for
 * setjmp/longjmp/malloc/printf that the whole codebase relies on. If those
 * overrides are not established before any use, host and kolibri builds would
 * silently diverge -- hence the "include me first" rule.
 */

/* Under -std=c99 glibc and newlib (KolibriOS) hide everything beyond ISO C: M_PI,
   strncasecmp, strtok_r, O_CLOEXEC, st_mtim, utimensat, symlink... (macOS shows
   them anyway). Ask for POSIX.1-2008 and the BSD extras before the first system
   header. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

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
#if defined(PPY_PLATFORM_KOLIBRI) || defined(PPY_TARGET_KOLIBRIOS) || defined(PPY_FS_KOLIBRI)
#define PPY_KOLIBRI 1
#endif

#if defined(PPY_KOLIBRI)

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

#endif /* PPY_KOLIBRI */

/* Platform lifecycle / entry hooks. Implemented per platform under
   platform/host/ and platform/kolibri/. */
void        ppy_platform_init(void);              /* early startup (reloc, heap) */
void        ppy_platform_shutdown(void);          /* teardown */
const char *ppy_platform_default_script(void);    /* NULL when none (host) */
void        ppy_platform_banner(const char *script_path);

/* Command line as delivered by the platform. The host has a real argv and
   returns NULL (main() uses argc/argv unchanged). KolibriOS has no argv: the
   kernel copies the launch arguments into a header buffer, returned here, and
   main() splits it into argc/argv itself. exe_path backs argv[0] if available. */
const char *ppy_platform_cmdline(void);
const char *ppy_platform_exe_path(void);

/* Run another program and wait for it to finish (used by `--compile` to start
   fasm/the linker). `args` is the argument string, already quoted for the
   platform. Returns the program's exit status (host: 127 when it cannot be
   found); KolibriOS reports none, so there 0 only means it ran and ended. */
int         ppy_platform_run(const char *program, const char *args);

/* Become another program (`plainpy --compile ...` runs plainpyc, the compiler):
   the host execs it (searching PATH when path has no '/') and returns -1 only
   when it cannot; KolibriOS runs it and waits (ppy_platform_run): 0, or -1. */
int         ppy_platform_exec(const char *path, char **argv);

/* Let a file be run (the executable fasm wrote: fasm leaves it 0644): the
   host adds x where r is (chmod); KolibriOS has no such bit. */
void        ppy_platform_make_executable(const char *path);

/* Raw syscall gateway (backs the built-in `sys.syscall`).
   ppy_platform_has_syscall(): 1 only on the KolibriOS build, 0 elsewhere.
   ppy_platform_syscall(): loads eax..edi from in[0..5] and ebp from in[6]
   (0 unless given: only a few functions read it, e.g. 65's row offset),
   executes `int 0x40`, and stores the resulting eax..edi into out[0..5]. */
int         ppy_platform_has_syscall(void);
int         ppy_platform_syscall(const uint32_t in[7], uint32_t out[6]);

/* ---- threading (host: pthreads; KolibriOS: int 0x40 fn 51) ----
   spawn runs `entry(arg)` on a new OS thread; returns 0 on success.
   yield hints the scheduler (used to back off on lock contention). */
int         ppy_thread_spawn(void (*entry)(void *), void *arg);
void        ppy_thread_yield(void);
void        ppy_thread_sleep_ms(int ms);

/* Portable lock over compiler atomics; only the back-off (yield) is
   platform-specific. Backs the interpreter lock (GIL) and user locks. */
typedef struct { volatile int held; } ppy_lock;
static inline void ppy_lock_init(ppy_lock *l){ l->held = 0; }
static inline int  ppy_lock_try(ppy_lock *l){ return __atomic_exchange_n(&l->held, 1, __ATOMIC_ACQUIRE) == 0; }
static inline void ppy_lock_acquire(ppy_lock *l){ while(!ppy_lock_try(l)) ppy_thread_yield(); }
static inline void ppy_lock_release(ppy_lock *l){ __atomic_store_n(&l->held, 0, __ATOMIC_RELEASE); }

#endif /* PPY_PLATFORM_H */
