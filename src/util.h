#ifndef MPY_UTIL_H
#define MPY_UTIL_H

#include "platform/platform.h"

/* Fatal error: print message and exit(1). */
void  die(const char *m);

/* Checked allocation wrappers (die() on failure). */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrndup2(const char *s, int n);
char *xstrdup2(const char *s);

#endif /* MPY_UTIL_H */
