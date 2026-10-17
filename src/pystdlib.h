#ifndef PPY_PYSTDLIB_H
#define PPY_PYSTDLIB_H

#include <stddef.h>

/* ========================= The standard library written in Python =========================
   The modules in src/stdlib/ (os, socket, heapq, ...) are carried in
   plainpy.lib, the file of data next to plainpy and plainpyc (tools/pack.c
   writes it: the modules without comments and docstrings, deflated, and the
   runtime routines of compiled programs). The interpreter imports the modules
   from it and the compiler compiles them with the programs that import them,
   so both run the same code. Platform work goes through sys.syscall: the i386
   Linux system calls (emulated on other hosts) or KolibriOS's int 0x40 functions.

   The file is found next to the program (or PLAINPYLIB names it) and mapped
   (read, where there is no mmap) the first time something of it is needed;
   a missing file, or one of another build (its id is not PPY_LIB_ID), ends
   the program with a message. */

/* The source of module `name` (dotted), unpacked anew (the caller frees it),
   or NULL; *package = 1 for a package (its __init__); package may be NULL. */
char *ppy_stdlib_source(const char *name, int *package);
/* 1 for a module of the standard library this program goes without: plainpy
   uses none of the modules only compiled programs use, plainpyc none only the
   interpreter runs (Makefile: INTERP_ONLY_MODULES, COMPILE_ONLY_MODULES). */
int ppy_stdlib_left_out(const char *name);
/* The module that `name` is another name of (os.path -> posixpath), or NULL. */
const char *ppy_stdlib_alias(const char *name);
/* A text of plainpy.lib ("rtlib": the runtime routines of compiled programs,
   "x2c_rt": the start of the C programs of the macos target), or NULL. */
const char *ppy_lib_text(const char *name);

/* A library of bytecode (plainpy --bytecode-lib): the interpreter's modules
   compiled (src/interp/bytecode.c), stored as they are, 4-aligned - the
   interpreter runs them from the mapped file without the sources, and the
   compiler cannot use such a library. */
const void *ppy_stdlib_bytecode(const char *name, size_t *len, int *package);   /* NULL: none */
int         ppy_stdlib_count(void);                       /* the entries */
char       *ppy_stdlib_module(int i, int *package);       /* entry i's name (a new string) if it is a module (source) of this program, or NULL */
typedef struct { const char *name; int package; const void *data; size_t len; } PpyLibModule;
int         ppy_lib_write_bytecode(const char *path, const PpyLibModule *m, int n, char **err);   /* 0: written */

#endif /* PPY_PYSTDLIB_H */
