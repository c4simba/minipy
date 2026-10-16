# Target c: a program as portable C, and how to port it to a board

```sh
plainpy --compile --target c app.py                    # app.c, ppy_port.h
plainpy --compile --target c --port template app.py    # ... and ppy_port_template.c, a port to fill in
plainpy --compile --target c --port posix app.py       # ... and ppy_port_posix.c, built into ./app (this computer)
```

`app.c` is the whole program: the compiled Python, the runtime routines it
uses and the i386 machine it was compiled for, kept in C variables (the
listing translated by `aot/x2c.c`; the runtime is `x2c_port.c` here). It
talks to the world only through the functions of [ppy_port.h](ppy_port.h),
which a **port** gives - a C file for one board or system. The ESP32 family
has one ([../esp32](../esp32/)); this guide is for writing another.

## What it takes

- **A C compiler: GCC or Clang** (arm-none-eabi-gcc, xtensa-esp-elf-gcc,
  riscv32-esp-elf-gcc, avr-gcc is too small...). The C file uses their
  extensions: computed goto (`goto *table[i]`), `__attribute__((aligned(1),
  may_alias))`, `__builtin_parity`, asm labels for C functions.
- **A 32- or 64-bit little-endian processor** - Cortex-M3 and up, Xtensa,
  RISC-V, x86, AArch64. (The program's memory is i386's: a big-endian
  processor would need every access byte-swapped.)
- **Memory**: the port gives one *arena* (`ppy_port_arena`). In it: the
  program's data (strings, tables, classes: its `.asm` shows how much), the
  main stack (`--stack`, 16 KiB), small objects taken in chunks of `--chunk`
  (16 KiB), a stack per live generator or task (`--costack`, 8 KiB), each
  bigger object rounded up to 4 KiB. A small program runs in 64 KiB; 100-200
  KiB is comfortable. Nothing guards the stacks: give a deeply recursive
  program a bigger `--stack`.
- **Flash** for the code: a few hundred KiB for a typical program (the
  runtime routines it uses are in it; the C is big, its machine code
  less so - see the esp32 README for real sizes).

## No C library needed

`app.c` includes only `<stdint.h>`, `<stddef.h>` and `ppy_port.h`, and calls
no C library function of its own:

- memory copies, string lengths: its own;
- floats to text and back (`print(1.5)`, `float("2.5")`, f-strings): its own,
  exact (`aot/x2c_float.c`), with the same digits as CPython;
- `abs`, `math.floor/ceil/trunc`, `round`, `math.sqrt`, `%`, `math.fmod`,
  `math.ldexp`: its own, exact;
- only `math.sin/cos/tan/asin/acos/atan/atan2`, `math.exp/log/log2/log10`
  and float `**` call the C library's libm - and only when the program uses
  them. Link `-lm` (newlib, picolibc and every embedded toolchain have it),
  or give those functions yourself.

A program without them links with `-nostdlib` and two more things:
**libgcc** (`-lgcc`: 64-bit division, and doubles on chips without a
double FPU) and **memcpy/memset** - the C compiler may call them for its own
copies; `ppy_port_template.c` has them under `PPY_PORT_NO_LIBC`.

```sh
# a program and the template port without any C library (check: only ppy_port_* symbols are left)
plainpy --compile --target c --port template app.py
arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -O2 -ffreestanding -DPPY_PORT_NO_LIBC -c app.c ppy_port_template.c
```

## The Python standard library

Nothing of it is installed on the board: a compiled program carries the
modules it imports. `plainpy --compile` takes them from `plainpy.lib` (the
same modules the interpreter runs), checks and compiles them with the
program; the C file has only what is used.

- Modules that compute - `json`, `re`, `datetime`, `collections`, `struct`,
  `math`, `random`, `heapq`, `itertools`, `string`, `textwrap`, `base64`,
  `hashlib`, `enum`, ... - work on any board.
- `time` works through `ppy_port_clock_ns` / `ppy_port_sleep_ns`; `random`
  and `os.urandom` through `ppy_port_random`.
- Modules that need the system - files (`open`, `os`, `pathlib`,
  `tempfile`), `socket`, `subprocess`, time zones (`/etc/localtime`) - make
  Linux system calls the port answers in `ppy_port_syscall`: `open` is 5,
  `read` 3, `write` 4, `close` 6, `lseek` 19, `stat64` 195, `socket` 359
  ... (i386 numbers, Linux's structures and errno values; see
  `targets/posix/lx_emul.c`, which answers all of them with a POSIX C
  library). A port that answers none makes them raise `OSError(ENOSYS)`.
- `sys.platform` is `"baremetal"` for the c target (`"esp32"` for esp32): `if
  sys.platform == ...` is decided when compiling.

## A port

```c
void    *ppy_port_arena(uint32_t *size);                 // the program's memory
int32_t  ppy_port_write(int fd, const void *buf, uint32_t n);   // print (1), errors (2)
int32_t  ppy_port_read(int fd, void *buf, uint32_t n);    // input()
uint64_t ppy_port_clock_ns(int clock);                   // 0 wall clock, 1 monotonic
void     ppy_port_sleep_ns(uint64_t ns);
void     ppy_port_random(void *buf, uint32_t n);
int64_t  ppy_port_syscall(const uint32_t r[7]);          // files, sockets, device calls: or -PPY_ENOSYS
void     ppy_port_panic(const char *msg);                // never returns
```

and the program gives `int ppy_main(int argc, char **argv)` - it runs the
program and returns its exit status - and `ppy_guest_ptr(addr)` for the
buffers the program passes to `ppy_port_syscall`.

Bringing up a board, with `--port template`:

1. `ppy_port_write`: the bytes to the UART (or your debugger's console).
   `print("hello")` works now.
2. `ppy_port_clock_ns`, `ppy_port_sleep_ns`: a timer (SysTick, an RTC).
   `time.sleep(0.5)` works now.
3. `ppy_port_arena`: a static array is fine (`PPY_ARENA_BYTES`), or a heap block.
4. Call `ppy_main(1, argv)` from the board's startup code (or an RTOS task).
5. The device calls you need in `ppy_port_syscall`.

## Devices: lib/machine.py

`machine` is MicroPython's API (Pin, ADC, PWM, I2C, SPI, UART, sleep_ms,
ticks_ms, reset, freq, unique_id, deepsleep, in part). Each operation is a
device call, `sys.syscall(PPY_DEV_..., ...)`, listed with its arguments in
`ppy_port.h`: `PPY_DEV_PIN_INIT pin, mode, pull`, `PPY_DEV_PIN_WRITE pin,
value`, `PPY_DEV_I2C_WRITE bus, address, buffer, length, stop`, ... A port
implements the ones its board has. A board's own: `machine.call(n, a, b, c,
d)` is the call `0x7800 + n`.

C functions are at hand too: with the c and esp32 targets, `ctypes` calls
link to the C file's externs (the library name of `CDLL` does not matter):

```python
import ctypes
sdk = ctypes.CDLL(None)
sdk.board_led_set(1)          # any C function linked with the program
```

## Trying it on a computer

`--port posix` builds the program for the computer it is compiled on:
files and sockets work through `lx_emul.c`, and the device calls through a
simulated board (`targets/posix/devsim.c`) that prints what the program
does - the same lines the interpreter prints for `lib/machine.py`:

```text
$ plainpy --compile --target c --port posix blink.py && ./blink
[pin 2] out
[pin 2] = 1
[pin 2] = 0
```

`TARGET=c sh tests/run_typed_tests.sh` runs the typed test suite this way.

## Limits

- No interrupts and no OS threads: `threading` and `asyncio` are
  cooperative (tasks on their own stacks, switched by the runtime); a
  program polls and sleeps.
- Stack overflows are not detected: size `--stack` and `--costack`.
- `int` is 64-bit (as in every plainpy program); `float` is a double - on a
  chip without a double FPU it is done in software by libgcc.
- One program per C file (`ppy_main` may be called again: it starts over).
