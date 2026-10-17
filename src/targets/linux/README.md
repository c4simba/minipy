# Target linux: an i386 Linux executable

```sh
plainpy --compile --target linux app.py     # ./app: an ELF for any x86 Linux (needs fasm)
```

The listing (`aot/codegen`) is a whole fasm program: `format ELF
executable`, the program's code, and the runtime routines it uses in their
Linux versions - system calls through `int 0x80`, no C library, no linker.
fasm writes the executable directly ([target.c](target.c) runs it, then
makes the file executable). 64-bit x86 Linux runs it as it is.

The clocks (`time.time()`, `monotonic()`, `datetime.now()` ...) are read as
the C library reads them: at the start the program finds
`__vdso_clock_gettime` in the vDSO the kernel maps into it
(`AT_SYSINFO_EHDR`, runtime `time.asm`: `rt_vdso_init`) and calls it - no
trap into the kernel, which a 32-bit program pays dearly for on a 64-bit
kernel with its mitigations; without a vDSO it is `int 0x80`.

A program that calls C functions (`ctypes`) gets an ELF dynamic section
(not a static executable): the system's loader links the C library's
libraries (`libc.so.6`, `libm.so.6`, `libdl.so.2`; on a 64-bit system they
are the i386 ones: `libc6:i386`). Any other library - `libssl.so.3` for
`ssl` - is loaded when one of its functions is first called (`dlopen`,
`dlsym`: runtime `rt_clazy`), so a program that does not reach it runs
without it; a missing one stops the program at that call with the loader's
message (exit status 127).

The default `--target` on Linux (x86_64 and aarch64 alike: the program is
i386 either way). `sys.platform` is `"linux"`.
