#ifndef KOS_STDIO_H
#define KOS_STDIO_H
#include <stddef.h>
#include <stdarg.h>
typedef struct KOS_FILE FILE;
extern FILE *stdout, *stderr;
int snprintf(char *s, size_t n, const char *fmt, ...);
int vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
int sprintf(char *s, const char *fmt, ...);
int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int puts(const char *s);
#endif
