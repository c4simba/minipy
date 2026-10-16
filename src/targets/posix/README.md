# posix: plainpy on Linux and macOS

- `startup.c`, `fs_host.c`, `thread.c` - plainpy's and plainpyc's own
  backend on a POSIX system: the start, files through stdio, threads
  through pthreads, running other programs.
- `lx_emul.c` - the i386 Linux system call interface on another host: what
  `sys.syscall` does in the interpreter (everywhere but on KolibriOS), and
  the system calls of programs of the macos target and of the c target's
  posix port. Linux's numbers, flags and structures in; the host's calls
  made; Linux's layouts out.
- `devsim.c` - the device calls of `lib/machine.py` (`0x7000..`) on a
  computer: a simulated board that prints what the program does with it.
