#ifndef MPY_AOT_H
#define MPY_AOT_H

#include "ast.h"

/* ========================= Typed ahead-of-time compiler =========================
   minipy --compile [options] file.mpy

   source --(lexer, frontparser)--> AST of the entry module and, transitively,
   of every module it imports --(aot_types.c)--> every name, parameter, return
   value, field and element gets one static type (annotated or inferred) --
   (aot_codegen.c)--> one fasm listing for i386 --(fasm)--> a standalone
   executable: a Linux ELF using int 0x80, or a KolibriOS application using
   int 0x40. For macos the listing is translated to C instead (aot_x2c.c) and
   the system C compiler builds a native executable. No libc, no linker, no runtime library: the few runtime routines a
   program needs (allocator, formatting, containers, console) are emitted into
   the listing only when it uses them.

   Restrictions of compiled programs:
     - every import is at the top of its module (they are resolved at compile time);
     - a variable/field/parameter/element keeps one type; None is the zero
       value of that type (0, 0.0, "", empty/null);
     - no generators, closures or lambdas (except as sort keys). */

/* CLI entry (aot_driver.c); argv excludes the `--compile` flag itself. */
int aot_main(int argc, char **argv, const char *program);

/* macos: the Linux listing, translated to C (aot_x2c.c) and built by the
   system C compiler into a native executable. */
typedef enum { AOT_TARGET_LINUX, AOT_TARGET_KOLIBRI, AOT_TARGET_MACOS } AotTarget;

/* One module of the program being compiled. */
typedef struct AotUnit {
    char *name;      /* dotted module name; "__main__" for the entry script */
    char *path;      /* source file; NULL for an implicit namespace package */
    char *src;
    TokVec tv;
    Ast *ast;
} AotUnit;

typedef struct {
    AotTarget target;
    unsigned stack_size;       /* kolibri: application stack size in bytes (0: 64 KiB) */
    int count_allocs;          /* debugging: report the heap blocks still allocated at exit */
} AotCodegenOptions;

/* Type-check units[0..n) (units[0] is __main__) and emit the fasm listing into
   a heap buffer. Returns 0 on success; on error prints "path:line: error: ..."
   to stderr and returns 1. */
int aot_compile(const AotCodegenOptions *opt, AotUnit **units, int nunits, char **out, size_t *outlen);

/* macos: the listing as a C program (aot_x2c.c). Returns 0 on success; on
   error prints "minipy: ..." to stderr and returns 1. */
int aot_x2c(const char *listing, size_t len, char **out, size_t *outlen);

/* Dotted module name of a `from X.Y import ...` statement. */
char *aot_from_import_module(AotUnit *u, Stmt *s);

#endif /* MPY_AOT_H */
