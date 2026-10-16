# Targets

A folder per platform plainpy runs on or compiles for. Each holds what is
particular to it: the system interface its programs use, how `plainpyc`
makes a program for it (`target.c`), and its README.

| target | `--target` | what `plainpy --compile` makes | how | system interface |
|---|---|---|---|---|
| [linux](linux/) | `linux` | an i386 Linux executable (ELF) | fasm | int 0x80, nothing else |
| [kolibri](kolibri/) | `kolibri` | a KolibriOS application (MENUET01, kpacked) | fasm | int 0x40 |
| [macos](macos/) | `macos` | a native macOS executable | C → cc | Linux calls done with the host's |
| [c](c/) | `c` | portable C (`app.c` + `ppy_port.h`) for a port | C | `ppy_port.h` |
| [esp32](esp32/) | `esp32`, `esp32s3`, `esp32c3` ... | an ESP-IDF project | C → idf.py | `ppy_port.h`, `port_esp32.c` |

And [posix](posix/): plainpy and plainpyc themselves on Linux and macOS
(startup, files, threads), the Linux system calls on other hosts
(`lx_emul.c`) and a simulated board for the device calls of
`lib/machine.py` (`devsim.c`). The KolibriOS build of plainpy has its own
in [kolibri](kolibri/), and [wasm](wasm/) is the interpreter compiled to
WebAssembly for the website's "Try it" terminal (`make wasm`).

## One compiler, two ways out

```text
app.py + imports --(aot/types, aot/codegen)--> one i386 listing, app.asm
    the runtime routines (aot/runtime/*.asm) it uses, in their Linux (int 0x80)
    or KolibriOS (int 0x40) versions: AotTargetDef.abi
  --> fasm --> the executable                                   linux, kolibri
  --> aot/x2c.c --> C: the i386 machine kept in C variables      macos, c, esp32
        + a C runtime that answers the int 0x80 calls:
          targets/macos/x2c_host.c  a hosted program: 4 GiB of address space, lx_emul, dlopen
          targets/c/x2c_port.c      a program for a port: an arena, ppy_port.h, no C library
```

Everything a program does with the outside world - print, input, time,
files, sockets, memory, the device calls of `lib/machine.py` - is one of
those system calls. A target is therefore mostly an answer to them: the
Linux kernel, KolibriOS, the C library of a host (`lx_emul.c`), or a port.

## What a target folder holds

- `target.c` - the `AotTargetDef` (`targets.h`): the name and aliases,
  `sys.platform` of its programs (the compiler folds `if sys.platform ==
  ...` at compile time), the ABI, fasm or C, whether C functions (ctypes)
  are there, the memory defaults (the allocator's chunk, generator/task
  stacks, the main stack) and the build step that runs after the listing.
- the runtime of its programs where it is its own (`macos/x2c_host.c`,
  `c/x2c_port.c`, `kolibri/console.asm`) and its ports (`c/port_*.c`,
  `esp32/port_esp32.c`); C files the programs carry are packed into
  plainpy.lib (Makefile: `C_TEXT_SRC`, `RTLIB_ASM`).
- for a platform plainpy itself runs on: its backend (startup, files,
  threads: `posix/`, `kolibri/`).
- README.md.

## Adding a target

1. A folder with `target.c` defining `const AotTargetDef aot_target_NAME`
   (copy the closest one), an entry in `AotTarget` (`aot/aot.h`) and in the
   table of `targets.c`. `target.c` files are found by the Makefile.
2. If it makes C for a new board, that is a port of the c target: a
   `port_NAME.c` giving the functions of `c/ppy_port.h` (see
   [c/README.md](c/README.md)), and its files in `C_TEXT_SRC`.
3. If its programs need other runtime routines, add blocks to the runtime
   (`;;; code name linux|kolibri` blocks choose by ABI) or a file to
   `RTLIB_ASM`.
4. Tests: `TARGET=NAME sh tests/run_typed_tests.sh` runs the typed suite
   for it when its programs can run here.
