/* ========================= The targets of plainpyc =========================
   `plainpy --compile --target NAME` picks one of these. Every target is a
   folder of src/targets with its target.c (the AotTargetDef below and its
   build step) and what its programs run on: the system interface of the
   runtime, the C runtime of a translated listing, a port (README.md there).

   The compiler itself is the same for every target: it writes one i386
   listing (aot/codegen) whose runtime routines talk to the system through
   one of two interfaces - Linux's int 0x80 or KolibriOS's int 0x40 - and
   then the target's build step makes the program: fasm writes it (linux,
   kolibri), or the listing becomes C (aot/x2c.c) that a C compiler builds
   (macos, c, esp32), with a C runtime that answers the int 0x80 calls.

   This header belongs to plainpyc (the type checker, the code generator,
   the driver and the targets' build steps). */
#ifndef PPY_TARGETS_H
#define PPY_TARGETS_H

#include "aot/aot.h"
#include <stdio.h>

/* the system calls the listing makes */
typedef enum { AOT_ABI_LINUX, AOT_ABI_KOLIBRI } AotAbi;
/* what becomes of the listing: fasm assembles it / aot/x2c.c translates it to C */
typedef enum { AOT_OUT_FASM, AOT_OUT_C } AotOut;
/* C functions (ctypes): none / found when first called (dlopen, the ELF's
   dynamic linking) / linked with the program (extern functions of the C file) */
typedef enum { AOT_CFN_NONE, AOT_CFN_DYNAMIC, AOT_CFN_STATIC } AotCfn;

/* What a build step gets: the listing is written already (base.asm). */
typedef struct AotBuild {
    const struct AotTargetDef *t;
    const char *target_name;        /* as given to --target (an alias: a chip, say) */
    const char *src_path;           /* the program's .py */
    const char *base;               /* the output, without an extension: -o, or the script's name */
    const char *asm_path;           /* base.asm */
    const char *listing; size_t len;
    const char *fasm, *fasm_args;   /* --fasm, --fasm-args (or the environment, or the defaults) */
    const char *cc, *cc_args;       /* --cc, --cc-args */
    const char *port;               /* c: --port NAME (NULL: only the C file) */
    const char *chip;               /* esp32: --chip NAME (NULL: from target_name) */
    unsigned arena;                 /* c, esp32: --arena BYTES (0: the port decides) */
    unsigned stack;                 /* the main stack (0: the target's) */
    int verbose;                    /* -v: the commands being run */
    int pack;                       /* kolibri: kpack the executable */
    int run_build;                  /* esp32: -1 when ESP-IDF is found, 1 --build, 0 --no-build */
} AotBuild;

typedef struct AotTargetDef {
    AotTarget id;
    const char *name;               /* --target NAME */
    const char *aliases;            /* other names for it, space-separated */
    const char *platform;           /* sys.platform in its programs (folded at compile time) */
    AotAbi abi;
    AotOut out;
    AotCfn cfn;
    unsigned chunk;                 /* the allocator takes memory from the system in chunks of this many bytes */
    unsigned costack;               /* the stack of a generator or a task */
    unsigned stack;                 /* the main stack (kolibri, c, esp32; the others: the system's) */
    const char *summary;            /* a line of --help */
    int (*build)(AotBuild *b);      /* 0, or 1 after an error (reported) */
} AotTargetDef;

extern const AotTargetDef aot_target_linux, aot_target_kolibri, aot_target_macos, aot_target_c, aot_target_esp32;

const AotTargetDef *aot_target(AotTarget id);
const AotTargetDef *aot_target_find(const char *name);   /* by name or alias; NULL when unknown */
AotTarget aot_target_host(void);                         /* the system plainpyc runs on */
void aot_targets_help(FILE *f);                          /* the list for --help */

/* ---- for build steps (targets/targets.c) */
char *aot_concat(const char *a, const char *b);
/* {in} {out} {dir} of an argument template, quoted for the command line */
char *aot_tool_args(const char *tmpl, const char *in, const char *out);
/* run a tool: 0, or its exit status after saying what went wrong (option: how to choose another) */
int aot_run_tool(const char *program, const char *args, const char *option, int verbose);
/* write a file: 0, or 1 after an error message */
int aot_write_file(const char *path, const char *data, size_t n, int verbose);
/* a text of plainpy.lib (tools/pack.c -t): it must be there */
const char *aot_lib_text(const char *name);

#endif /* PPY_TARGETS_H */
