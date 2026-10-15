/* ========================= AOT driver: minipy --compile =========================
   1. import pass: parse the entry script and every module it imports. Imports
      are resolved here, at compile time, the same way the interpreter resolves
      them at run time (relative to the importing file, `<name>.py` or
      `<name>/__init__.py`, namespace packages for intermediate components);
      therefore every import must sit at the top of its module (imports.c).
      Library modules the program needs are added (preludes.c, namespaces.c).
   2. type check + code generation: all modules -> one fasm listing
      (src/aot/types, src/aot/codegen), runtime routines included.
   3. fasm turns the listing straight into the executable (Linux ELF or
      KolibriOS application): no linker, no runtime library to install. The
      same works inside KolibriOS with its own fasm (driver.c).

   This header is internal to the driver's files. */
#ifndef MPY_AOT_DRIVER_H
#define MPY_AOT_DRIVER_H

#include "aot.h"
#include "pystdlib.h"
#include "py_front.h"
#include "fs.h"

typedef struct { AotUnit **v; int n, cap; } Units;

typedef struct { char *s; size_t n, cap; } SBuf;

/* ---- imports.c: the import pass */
AotUnit *find_unit(Units *us, const char *name);
AotUnit *add_unit(Units *us, const char *name, const char *path, char *src);
int has_stderr_value(const char *src);
void main_imports(AotUnit *main, const char *name);
void main_epilogue(AotUnit *main, const char *code);
int names_codec(const char *t);
int scan_imports(Units *us, AotUnit *u);

/* ---- preludes.c: preludes */
extern const char EG_PRELUDE[];
extern const char CMATH_PRELUDE[];
extern const char CMPKEY_PRELUDE[];
int uses_complex(const char *s);

/* ---- driver.c: minipy --compile */
int load_program(Units *us, const char *src_path, char *src);

/* ---- namespaces.c: namespace classes */
void sbf(SBuf *b, const char *fmt, ...);   /* append, printf-like */
char *argparse_with_namespace(Units *us, const char *src, SBuf *err);
char *optparse_with_values(Units *us, const char *src, SBuf *err);
char *types_with_namespace(Units *us, const char *src);

#endif /* MPY_AOT_DRIVER_H */
