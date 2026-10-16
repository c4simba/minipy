# Target esp32: Python programs on ESP32 chips

```sh
. $IDF_PATH/export.sh                         # ESP-IDF 5.2 or newer, set up once per shell
plainpy --compile --target esp32 blink.py     # blink/: an ESP-IDF project, built (idf.py build)
cd blink && idf.py flash monitor              # (-p /dev/ttyUSB0 to name the port)
```

`--target esp32s3` (or `--chip esp32s3`) for another chip of the family:
esp32, esp32s2, esp32s3 (Xtensa), esp32c2, esp32c3, esp32c5, esp32c6,
esp32h2, esp32p4 (RISC-V). Without ESP-IDF on the PATH plainpy writes the
project and stops; `--no-build` does that anyway, `--build` insists.

```python
# blink.py
from machine import Pin, sleep_ms

led = Pin(2, Pin.OUT)          # GPIO 2: the LED of many ESP32 boards
for i in range(10):
    led.toggle()
    sleep_ms(250)
print("done")
```

The program is compiled as for any target - typed Python, checked and
compiled ahead of time with the standard library modules it imports - and
runs as C on the chip (the c target: `aot/x2c.c`, [../c/x2c_port.c](../c/x2c_port.c))
with this folder's port, [port_esp32.c](port_esp32.c). No interpreter is on
the chip, and nothing to install there.

## The project

```text
blink/
  CMakeLists.txt         project(blink)
  sdkconfig.defaults     the chip; main task stack 16 KiB, -O2, no idle-task watchdog, a large app partition
  main/
    blink.c              the program (generated: rewritten by every plainpy --compile)
    ppy_port_esp32.c     the port: yours to change
    ppy_port.h           what a port gives (../c/ppy_port.h)
    CMakeLists.txt
```

It is an ordinary ESP-IDF project: `idf.py menuconfig`, other components,
your own C files in `main/` all work. Compiling again rewrites
`main/blink.c`, `ppy_port.h`, `ppy_port_esp32.c` and the CMake files (keep
changes to the port in a copy, or compile with `-o` elsewhere).

## What the program has

| | |
|---|---|
| `print`, `input` | the ESP-IDF console: UART0 or the USB serial port, as sdkconfig chooses (115200 baud) |
| `time` | `time.monotonic()` from esp_timer; `time.time()` counts from boot until SNTP (or your C) sets the clock; `time.sleep()` lets other tasks run |
| `random`, `os.urandom` | the hardware RNG |
| memory | PSRAM when the board has it (up to 8 MiB), else the largest free block of internal RAM less 24 KiB for ESP-IDF; `--arena BYTES` asks for an exact size |
| `machine` | `Pin` (GPIO), `ADC` (oneshot; `read_uv` calibrated where the chip can), `PWM` (LEDC), `I2C` (`i2c_master`, repeated starts for `readfrom_mem`), `SPI` (bus 1: SPI2, 2: SPI3), `UART` (1, 2; 0 is the console's), `reset`, `freq`, `unique_id` (the MAC), `deepsleep` |
| C functions | `ctypes.CDLL(None).esp_get_free_heap_size()`: any function of ESP-IDF or of your C files, linked with the program |
| the end | the exit status is printed and the chip idles (`-DPPY_ESP32_RESTART`: restarts it) |

Not yet: files (an ESP-IDF filesystem and the Linux file calls in
`ppy_port_syscall`), sockets / Wi-Fi (through ctypes for now), interrupts.

```python
from machine import Pin, PWM, ADC, I2C, sleep_ms

pwm = PWM(Pin(5), freq=1000, duty_u16=0)       # an LED fading in
for d in range(0, 65536, 4096):
    pwm.duty_u16(d)
    sleep_ms(50)

adc = ADC(Pin(34), atten=ADC.ATTN_11DB)        # 0..~2.5 V
print("ADC", adc.read_u16(), adc.read_uv() // 1000, "mV")

i2c = I2C(0, scl=Pin(22), sda=Pin(21), freq=400000)
print("I2C devices:", [hex(a) for a in i2c.scan()])
```

## Sizes, and what was run

`examples/esp32/blink.py` built with ESP-IDF 6.1 for the ESP32: a 416 KiB
image, 266 KiB of it code (ESP-IDF's own included). It and a program going
through GPIO, PWM, SPI, UART, I2C (a scan), generators, classes, exceptions,
json and libm's functions ran in Espressif's QEMU of the ESP32 with the
output they give on a computer - but for the ADC, which that QEMU does not
have (and a pin's level read back, which it does not give). CI builds the
examples for esp32, esp32s3 and esp32c3 and runs blink in QEMU.

## Memory and speed

The program's memory is the arena: its data, the main stack (`--stack`,
16 KiB), small objects in 16 KiB chunks (`--chunk`), a stack per live
generator or task (`--costack`, 8 KiB). Without PSRAM the arena is the
largest free block of internal RAM ESP-IDF leaves (the program prints
nothing about it: `ctypes.CDLL(None).heap_caps_get_largest_free_block(...)`
tells, or `--arena` fixes it). The C of a program is the i386 code it was
compiled to, a few C statements per instruction, with memory accessed a
byte at a time (the i386 allows unaligned words, the ESP32 does not).
Floats are doubles, done in software on chips without a double FPU.

## On a computer first

The same program runs on the computer, with a simulated board that prints
what the program does to it - in the interpreter, or compiled:

```text
$ plainpy blink.py
[pin 2] out
[pin 2] = 1
...
```
