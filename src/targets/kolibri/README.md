# KolibriOS: plainpy there, and the kolibri target

Two things live here:

- **plainpy and plainpyc on KolibriOS** (`make kolibrios`): `startup.c`
  (the application's start, its command line, the heap), `console.c`
  (the shell's console), `fs_kolibri.c` (files through function 70),
  `syscall.c` (`sys.syscall`: `int 0x40`), `thread.c`, `ports_libc.c` (what
  the KolibriOS/ports newlib leaves out). See "KolibriOS build" in the main
  README.

- **The kolibri target** (`plainpy --compile --target kolibri app.py`):
  the listing is a flat MENUET01 binary for fasm whose runtime routines use
  their KolibriOS versions (`int 0x40`: function 68 for memory, 70 for files,
  75 for sockets) and `console.asm` here to talk to the shell's console.
  fasm writes the application; [target.c](target.c) then packs it as kpack
  does (LZMA, `aot/kpack.c`) - the kernel unpacks it as it loads it. On
  KolibriOS itself plainpyc runs the system's `/sys/develop/fasm`.
  `sys.platform` is `"kolibrios"`; `lib/kolibri.py` is the system's API.

  MENUET01, not MENUET02: a MENUET02 header has the kernel set up the
  application's heap and a TLS page per thread (FS), and its version 2
  starts it through `/sys/lib/dll.obj`; a compiled program has its own heap
  (function 68.11), its threads switch in the runtime, and it links its
  libraries itself - MENUET01 asks for nothing more and every kernel knows it.

- **Libraries** (`ctypes.CDLL("x.obj")`): COFF objects function 68.19
  loads (once per process; the kernel resolves their relocations and unpacks
  a kpacked one) and returns the `EXPORTS` table of - name / address pairs,
  ending in 0. A compiled program looks for a library when it first calls
  one of its functions (runtime `rt_clazy kolibri`, `aot/runtime/system.asm`):
  the name as given, the program's folder (the current folder of a program
  started by the system is `/RD/1`, so a relative name alone is not enough),
  `/sys/lib/`, `/kolibrios/lib/`; the interpreter's `ctypes`
  (`interp/modules/ctypes.c`) looks in the script's folder, plainpy's and the
  same two. The calls are cdecl. `ssl`'s library, mbedtls.obj, is built by
  [tools/kolibri-tls](../../../tools/kolibri-tls/build.sh) (`make kolibri-tls`).

- **Clocks** (`src/stdlib/time.py`, for plainpy and compiled programs alike):
  the counters are function 26.10's nanoseconds where the kernel has them
  (HPET; checked against 26.9 once), else 26.9's 1/100 s; `time.time()` adds
  the CMOS clock (functions 3 and 29, slow to read) read again at most once a
  second.
