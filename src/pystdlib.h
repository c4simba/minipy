#ifndef MPY_PYSTDLIB_H
#define MPY_PYSTDLIB_H

/* ========================= The standard library written in Python =========================
   The modules in src/stdlib/ (os, socket, heapq, ...) are compiled into minipy
   (tools/embed_stdlib.sh): the interpreter imports them from here and the
   compiler compiles them with the programs that import them, so both run the
   same code. Platform work goes through sys.syscall: the i386 Linux system
   calls (emulated on other hosts) or KolibriOS's int 0x40 functions. */

/* The source of module `name` (dotted), or NULL; *package = 1 for a package
   (its __init__); package may be NULL. */
const char *mpy_stdlib_source(const char *name, int *package);
/* The module that `name` is another name of (os.path -> posixpath), or NULL. */
const char *mpy_stdlib_alias(const char *name);

#endif /* MPY_PYSTDLIB_H */
