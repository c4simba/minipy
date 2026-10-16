#ifndef PPY_AOT_H
#define PPY_AOT_H

#include "ast.h"

/* ========================= Typed ahead-of-time compiler =========================
   plainpyc [options] file.py     (plainpy --compile ... runs it; aot/main.c)

   source --(py_parse, py_front)--> AST of the entry module and, transitively,
   of every module it imports --(aot/types)--> every name, parameter, return
   value, field and element gets one static type (annotated or inferred) --
   (aot/codegen)--> one fasm listing for i386 --(fasm)--> a standalone
   executable: a Linux ELF using int 0x80, or a KolibriOS application using
   int 0x40. For macos the listing is translated to C instead (aot/x2c.c) and
   the system C compiler builds a native executable. No libc, no linker, no runtime library: the few runtime routines a
   program needs (allocator, formatting, containers, console) are emitted into
   the listing only when it uses them.

   Restrictions of compiled programs:
     - every import is at the top of its module (they are resolved at compile time);
     - a variable/field/parameter/element keeps one type; None is the zero
       value of that type (0, 0.0, "", empty/null);
     - no generators, closures or lambdas (except as sort keys). */

/* CLI entry (aot/driver.c): the arguments after the program name. */
int aot_main(int argc, char **argv, const char *program);

/* macos: the Linux listing, translated to C (aot/x2c.c) and built by the
   system C compiler into a native executable. */
typedef enum { AOT_TARGET_LINUX, AOT_TARGET_KOLIBRI, AOT_TARGET_MACOS } AotTarget;

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
    unsigned stack_size;       /* kolibri: application stack size in bytes (0: 64 KiB) */
    int count_allocs;          /* debugging: report the heap blocks still allocated at exit */
} AotCodegenOptions;

/* Type-check units[0..n) (units[0] is __main__) and emit the fasm listing into
   a heap buffer. Returns 0 on success; on error prints "path:line: error: ..."
   to stderr and returns 1. */
int aot_compile(const AotCodegenOptions *opt, AotUnit **units, int nunits, char **out, size_t *outlen);

/* macos: the listing as a C program (aot/x2c.c). Returns 0 on success; on
   error prints "plainpy: ..." to stderr and returns 1. */
int aot_x2c(const char *listing, size_t len, char **out, size_t *outlen);

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
