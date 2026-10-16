# Target linux: an i386 Linux executable

```sh
plainpy --compile --target linux app.py     # ./app: an ELF for any x86 Linux (needs fasm)
```

The listing (`aot/codegen`) is a whole fasm program: `format ELF
executable`, the program's code, and the runtime routines it uses in their
Linux versions - system calls through `int 0x80`, no C library, no linker.
fasm writes the executable directly ([target.c](target.c) runs it, then
makes the file executable). 64-bit x86 Linux runs it as it is.

A program that calls C functions (`ctypes`) gets an ELF dynamic section
instead: the system's loader links `libc.so.6` and the other libraries
(those need the i386 C library on a 64-bit system: `libc6:i386`).

The default `--target` on Linux (x86_64 and aarch64 alike: the program is
i386 either way). `sys.platform` is `"linux"`.
