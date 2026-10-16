# Target macos: a native executable through C

```sh
plainpy --compile app.py           # on a Mac: ./app, native arm64 (app.c is kept)
```

The listing is the Linux one; `aot/x2c.c` translates it into C - static
recompilation: each i386 instruction becomes a few C statements on 32-bit
"registers", lazy flags and a double-precision x87 stack, in one function
with computed gotos - and the system C compiler builds it.

[x2c_host.c](x2c_host.c) is the start of every such C file: the machine
(`aot/x2c_machine.c`), exact float text (`aot/x2c_float.c`), a 4 GiB guest
address space (`mmap`), the Linux system calls done with the host's
(`targets/posix/lx_emul.c`: flags, structures and socket options
converted), C functions (`ctypes`) found with `dlopen` and called with the
signature the listing gives each call. `sys.platform` is `"darwin"`: the
standard library uses macOS's errno numbers and constants.
