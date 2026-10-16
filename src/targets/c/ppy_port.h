/* ========================= ppy_port.h: a plainpy program and the board it runs on =========================
   plainpy --compile --target c app.py writes app.c: the whole program as
   portable C - the compiled Python, the runtime it uses, the i386 machine it
   was compiled for kept in C variables - and this header. app.c needs no C
   library and no operating system; it gets what it needs from the board
   through the functions below, which a port gives (a C file you link with
   it: ppy_port_esp32.c, ppy_port_posix.c, or ppy_port_template.c to start
   from). src/targets/c/README.md is the guide.

   The program's side: */
#ifndef PPY_PORT_H
#define PPY_PORT_H

#include <stdint.h>

/* Runs the program from the start (again, if it ran before) and returns its
   exit status: 0 when it ended, 1 after an uncaught exception, or what it
   gave sys.exit(). The guest stack and heap live in the port's arena, so the
   C stack of the caller needs only a few KiB. */
int ppy_main(int argc, char **argv);

/* An address the program passed (a buffer: sys.buffer(), bytes, str) as a
   pointer in the arena; NULL for 0. ppy_guest_addr: back (0 for a pointer
   outside the arena). For ppy_port_syscall's arguments. */
void *ppy_guest_ptr(uint32_t addr);
uint32_t ppy_guest_addr(const void *p);

/* ---------------------------------------------------------------- what a port gives */

/* The program's memory - its data, heap, generator and task stacks and its
   main stack all live here. *size: what `plainpy --arena BYTES` asked for (0:
   as much as you like to give); set it to what you give. 16-byte aligned. A
   small program runs in 64 KiB; see README.md for what takes memory. */
void *ppy_port_arena(uint32_t *size);

/* Output and input of the program: fd 1 stdout (print), 2 stderr; fd 0 stdin
   (input()). The number of bytes written / read (0: the end of the input),
   or -errno. */
int32_t ppy_port_write(int fd, const void *buf, uint32_t n);
int32_t ppy_port_read(int fd, void *buf, uint32_t n);

/* Time in nanoseconds: clock 0 the wall clock (since 1970 if the board knows
   the date, else since it started: time.time()), 1 monotonic (time.monotonic(),
   time.perf_counter()). ppy_port_sleep_ns: time.sleep(). */
uint64_t ppy_port_clock_ns(int clock);
void ppy_port_sleep_ns(uint64_t ns);

/* n random bytes (os.urandom(), random's seed): a hardware RNG, or the best
   you have. */
void ppy_port_random(void *buf, uint32_t n);

/* Every other call the program makes: r[0] is the call - a Linux i386 system
   call number (files: 5 open, 3 read, 4 write, 6 close, 19 lseek, 195 stat64
   ...; sockets: 359 socket ...) or one of the device calls below (lib/machine.py)
   - and r[1..6] its arguments (ebx, ecx, edx, esi, edi, ebp; addresses for
   ppy_guest_ptr). Returns the result, or -errno: -PPY_ENOSYS for a call the
   port does not do. A port that does none returns -PPY_ENOSYS always. */
int64_t ppy_port_syscall(const uint32_t r[7]);

/* The program cannot go on (its machine code jumped where no code is, a
   division by zero in the machine code, too little memory for its data and
   stack): msg says why. Must not return (halt, reset, or exit). */
void ppy_port_panic(const char *msg);

/* Optional: errno after a C function the program called (ctypes.get_errno()),
   as a Linux number. Without it the program sees 0. */
int ppy_port_errno(void);

/* ---------------------------------------------------------------- errno values the program knows (Linux's) */
#define PPY_EPERM      1
#define PPY_ENOENT     2
#define PPY_EINTR      4
#define PPY_EIO        5
#define PPY_ENXIO      6
#define PPY_EBADF      9
#define PPY_EAGAIN    11
#define PPY_ENOMEM    12
#define PPY_EACCES    13
#define PPY_EBUSY     16
#define PPY_EEXIST    17
#define PPY_ENODEV    19
#define PPY_ENOTDIR   20
#define PPY_EISDIR    21
#define PPY_EINVAL    22
#define PPY_ENOSPC    28
#define PPY_ESPIPE    29
#define PPY_EROFS     30
#define PPY_EPIPE     32
#define PPY_ERANGE    34
#define PPY_ENOSYS    38
#define PPY_ENOTEMPTY 39
#define PPY_ETIMEDOUT 110

/* ---------------------------------------------------------------- device calls (lib/machine.py)
   r[0] = one of these; pins and buses are numbered as the board numbers them.
   Results: >= 0, or -errno (-PPY_ENODEV: no such pin or bus, -PPY_EINVAL: a
   wrong argument, -PPY_ENOSYS: the port has no such device). */
#define PPY_DEV_PIN_INIT    0x7000  /* pin, mode (0 in, 1 out, 2 open drain), pull (0 none, 1 up, 2 down) */
#define PPY_DEV_PIN_WRITE   0x7001  /* pin, value (0/1) */
#define PPY_DEV_PIN_READ    0x7002  /* pin -> 0/1 */
#define PPY_DEV_ADC_INIT    0x7010  /* pin, attenuation (0 0 dB, 1 2.5 dB, 2 6 dB, 3 11 dB) */
#define PPY_DEV_ADC_READ    0x7011  /* pin -> raw value 0..65535 (scaled to 16 bits) */
#define PPY_DEV_ADC_READ_UV 0x7012  /* pin -> microvolts */
#define PPY_DEV_PWM_INIT    0x7020  /* pin, frequency (Hz), duty (0..65535) */
#define PPY_DEV_PWM_DUTY    0x7021  /* pin, duty (0..65535) */
#define PPY_DEV_PWM_FREQ    0x7022  /* pin, frequency (Hz) */
#define PPY_DEV_PWM_DEINIT  0x7023  /* pin */
#define PPY_DEV_I2C_INIT    0x7030  /* bus, scl pin, sda pin, frequency (Hz) */
#define PPY_DEV_I2C_WRITE   0x7031  /* bus, address, buffer, length, stop (0/1) -> bytes written */
#define PPY_DEV_I2C_READ    0x7032  /* bus, address, buffer, length, stop (0/1) -> bytes read */
#define PPY_DEV_I2C_SCAN    0x7033  /* bus, buffer of 128 bytes -> how many answered (buffer[a] = 1 for each) */
#define PPY_DEV_SPI_INIT    0x7040  /* bus, sck pin, mosi pin, miso pin, baudrate, mode (polarity | phase << 1) */
#define PPY_DEV_SPI_XFER    0x7041  /* bus, tx buffer (or 0), rx buffer (or 0), length -> length */
#define PPY_DEV_UART_INIT   0x7050  /* uart, baudrate, tx pin, rx pin */
#define PPY_DEV_UART_WRITE  0x7051  /* uart, buffer, length -> bytes written */
#define PPY_DEV_UART_READ   0x7052  /* uart, buffer, length, timeout (ms) -> bytes read */
#define PPY_DEV_UART_ANY    0x7053  /* uart -> bytes waiting */
#define PPY_DEV_RESET       0x7060  /* (machine.reset(): does not return) */
#define PPY_DEV_FREQ        0x7061  /* -> CPU frequency (Hz) */
#define PPY_DEV_UNIQUE_ID   0x7062  /* buffer, length -> bytes of the board's id */
#define PPY_DEV_DEEPSLEEP   0x7063  /* milliseconds (0: until woken) */
#define PPY_DEV_USER        0x7800  /* 0x7800..0x7FFF: the port's own (machine.call(n, ...)) */

#endif /* PPY_PORT_H */
