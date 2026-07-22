#ifndef MPY_UTIL_H
#define MPY_UTIL_H

#include "platform/platform.h"

/* ========================= Small utility macros ========================= */

#if defined(__GNUC__) || defined(__clang__)
#define MPY_NORETURN __attribute__((noreturn))
#else
#define MPY_NORETURN
#endif

#define MPY_MIN(a,b)          ((a) < (b) ? (a) : (b))
#define MPY_MAX(a,b)          ((a) > (b) ? (a) : (b))
#define MPY_ARRAY_SIZE(a)     (sizeof(a) / sizeof((a)[0]))
#define MPY_CONTAINER_OF(ptr,type,member) ((type*)((char*)(ptr) - offsetof(type,member)))

/* Typed allocation wrappers over xmalloc (all die() on failure). */
#define MPY_NEW(T)            ((T*)xmalloc(sizeof(T)))
#define MPY_NEW0(T)           ((T*)memset(xmalloc(sizeof(T)), 0, sizeof(T)))
#define MPY_NEW_ARR(T,n)      ((T*)xmalloc(sizeof(T) * (size_t)(n)))

/* Fatal error: print message and exit(1). */
MPY_NORETURN void die(const char *m);

/* Checked allocation wrappers (die() on failure). */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrndup2(const char *s, int n);
char *xstrdup2(const char *s);

#endif /* MPY_UTIL_H */
