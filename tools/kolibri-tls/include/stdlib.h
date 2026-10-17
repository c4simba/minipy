#ifndef KOS_STDLIB_H
#define KOS_STDLIB_H
#include <stddef.h>
void *malloc(size_t n);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t n);
void free(void *p);
int rand(void);
void srand(unsigned seed);
void abort(void);
void exit(int status);
int abs(int x);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
int atoi(const char *s);
#define RAND_MAX 0x7fff
#endif
