#ifndef PPY_UTIL_H
#define PPY_UTIL_H

#include "targets/platform.h"

/* ========================= Small utility macros ========================= */

#if defined(__GNUC__) || defined(__clang__)
#define PPY_NORETURN __attribute__((noreturn))
#else
#define PPY_NORETURN
#endif

/* Debug logging: prints to stderr only in debug builds (-DPPY_DEBUG); expands
   to nothing otherwise, so release builds stay silent and pay no cost.
   On KolibriOS, stderr is mirrored to the console/debug board (see console.c). */
#ifdef PPY_DEBUG
#define PPY_LOG(...)          do{ fprintf(stderr, __VA_ARGS__); fflush(stderr); }while(0)
#else
#define PPY_LOG(...)          do{}while(0)
#endif

#define PPY_MIN(a,b)          ((a) < (b) ? (a) : (b))
#define PPY_MAX(a,b)          ((a) > (b) ? (a) : (b))
#define PPY_ARRAY_SIZE(a)     (sizeof(a) / sizeof((a)[0]))
#define PPY_CONTAINER_OF(ptr,type,member) ((type*)((char*)(ptr) - offsetof(type,member)))

/* Typed allocation wrappers over xmalloc (all die() on failure). */
#define PPY_NEW(T)            ((T*)xmalloc(sizeof(T)))
#define PPY_NEW0(T)           ((T*)memset(xmalloc(sizeof(T)), 0, sizeof(T)))
#define PPY_NEW_ARR(T,n)      ((T*)xmalloc(sizeof(T) * (size_t)(n)))

/* Fatal error: print message and exit(1). */
PPY_NORETURN void die(const char *m);

/* Checked allocation wrappers (die() on failure). */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrndup2(const char *s, int n);
char *xstrdup2(const char *s);

/* A region: many small allocations that all go at once - a parse tree and
   the bytecode compiler's scopes, freed when the code object is made. While
   ppy_region is set the ppy_r* functions take from it (ppy_rfree does
   nothing); with none set they are xmalloc / xrealloc / free. */
typedef struct PpyRegionBlock PpyRegionBlock;
typedef struct { PpyRegionBlock *blocks; } PpyRegion;
extern PpyRegion *ppy_region;
void *ppy_ralloc(size_t n);
void *ppy_ralloc0(size_t n);
void *ppy_rrealloc(void *p, size_t old, size_t n);
void  ppy_rfree(void *p);
char *ppy_rstrndup(const char *s, int n);
char *ppy_rstrdup(const char *s);
void  ppy_region_free(PpyRegion *r);

/* Split a raw command-line string (KolibriOS hands over one) into argv[1..]
   in buf; argv[0] = exe, or argv0_default without one. Returns argc. */
int ppy_split_cmdline(const char *cmd,const char *exe,const char *argv0_default,char *buf,int buflen,char **argv,int maxargv);

#endif /* PPY_UTIL_H */
