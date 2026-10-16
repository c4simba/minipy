#ifndef PPY_AOT_H
#define PPY_AOT_H

#include "ast.h"

/* ========================= Typed ahead-of-time compiler =========================
   plainpyc [options] file.py     (plainpy --compile ... runs it; aot/main.c)

   source --(py_parse, py_front)--> AST of the entry module and, transitively,
   of every module it imports --(aot/types)--> every name, parameter, return
   value, field and element gets one static type (annotated or inferred) --
   (aot/codegen)--> one fasm listing for i386 --(the target's build step,
   src/targets)--> a standalone program: a Linux ELF using int 0x80 or a
   KolibriOS application using int 0x40 (fasm), or C (aot/x2c.c) that a C
   compiler builds - natively on macOS, or for a port (a microcontroller,
   ESP32). No libc, no linker, no runtime library: the few runtime routines a
   program needs (allocator, formatting, containers, console) are emitted into
   the listing only when it uses them.

   Restrictions of compiled programs:
     - every import is at the top of its module (they are resolved at compile time);
     - a variable/field/parameter/element keeps one type; None is the zero
       value of that type (0, 0.0, "", empty/null);
     - no generators, closures or lambdas (except as sort keys). */

/* CLI entry (aot/driver.c): the arguments after the program name. */
int aot_main(int argc, char **argv, const char *program);

/* The targets (src/targets/targets.h describes each). */
typedef enum { AOT_TARGET_LINUX, AOT_TARGET_KOLIBRI, AOT_TARGET_MACOS, AOT_TARGET_C, AOT_TARGET_ESP32 } AotTarget;

/* One module of the program being compiled. */
typedef struct AotUnit {
    char *name;      /* dotted module name; "__main__" for the entry script */
    char *path;      /* source file; NULL for an implicit namespace package */
    char *src;
    Ast *ast;
    int failed;      /* (check mode) its source or imports were rejected: left out */
} AotUnit;

/* plainpy --compile --check: the checker reports every error it finds (one
   per function, class or module) instead of stopping at the first. */
extern int aot_check_mode;
extern int aot_check_errors;     /* (check mode) errors reported so far */

/* plainpy --compile --check MODULE...: those modules (and what they import)
   checked for compiled programs (src/aot/pkgcheck.c; `plainpy check` runs it) */
int aot_check_modules(char **modules, int nmodules, AotTarget target);

typedef struct {
    AotTarget target;
    unsigned stack_size;       /* kolibri: application stack size in bytes (0: the target's) */
    unsigned chunk;            /* memory taken from the system for small blocks at a time (0: the target's) */
    unsigned costack;          /* the stack of a generator or a task (0: the target's) */
    int count_allocs;          /* debugging: report the heap blocks still allocated at exit */
} AotCodegenOptions;

/* Type-check units[0..n) (units[0] is __main__) and emit the fasm listing into
   a heap buffer. Returns 0 on success; on error prints "path:line: error: ..."
   to stderr and returns 1. */
int aot_compile(const AotCodegenOptions *opt, AotUnit **units, int nunits, char **out, size_t *outlen);

/* aot/x2c.c: the listing as a C program.
     X2C_HOST  a program of a hosted system (targets/macos/x2c_host.c): main(),
               a 4 GiB guest address space, the Linux calls done with the
               host's, C functions found with dlopen;
     X2C_PORT  a program for a port (targets/c/x2c_port.c): ppy_main(), memory
               from an arena, the system through ppy_port.h, C functions
               linked with it; no C library needed.
   Returns 0 on success; on error prints "plainpy: ..." to stderr and returns 1. */
typedef enum { X2C_HOST, X2C_PORT } X2cMode;
typedef struct {
    X2cMode mode;
    unsigned stack;            /* X2C_PORT: the main stack (bytes) */
    unsigned arena;            /* X2C_PORT: the memory to ask the port for (0: the port decides) */
} X2cOptions;
int aot_x2c(const char *listing, size_t len, const X2cOptions *opt, char **out, size_t *outlen);

/* aot/peep.c: the text of one generated function, rewritten into shorter
   code with the same effect (a new heap string); PPY_NO_PEEP=1 copies it.
   aot_peephole_report: what it did, on stderr when PPY_PEEP_STATS=1. */
char *aot_peephole(const char *text, size_t len, size_t *outlen);
void aot_peephole_report(void);
/* aot/peep.c: the generated code of a whole program with the call
   sequences it repeats moved into shared routines (PPY_NO_OUTLINE=1: as is) */
char *aot_outline(const char *text, size_t len, size_t *outlen);
/* before a program's functions: forget the last program's; then name each
   compiled function that reads edx on entry (its closure) */
void aot_peephole_reset(void);
void aot_peephole_takes_edx(const char *label);

/* aot/kpack.c: a KolibriOS executable packed as kpack packs one (the kernel
   unpacks it while loading it): 0 and a new heap buffer, or 1 when packing
   would not make it smaller. */
int aot_kpack(const unsigned char *in, size_t n, unsigned char **out, size_t *outn);

/* Dotted module name of a `from X.Y import ...` statement. */
char *aot_from_import_module(AotUnit *u, Stmt *s);

#endif /* PPY_AOT_H */
