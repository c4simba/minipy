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
