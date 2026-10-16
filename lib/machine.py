# machine: the pins and buses of a board - MicroPython's API, in part.
#
#     from machine import Pin, PWM, sleep_ms
#     led = Pin(2, Pin.OUT)
#     while True:
#         led.toggle()
#         sleep_ms(500)
#
# The same file works in the interpreter and in compiled programs. Every
# operation is a device call of the board's port, sys.syscall(PPY_DEV_..., ...)
# (src/targets/c/ppy_port.h lists them; ppy_port_esp32.c does them with
# ESP-IDF's drivers). On a computer - the interpreter, the macos target, the
# c target's posix port - a simulated board answers them and says on stdout
# what the program does ("[pin 2] = 1"), so device code runs and can be
# tested without a board (src/targets/posix/devsim.c). A system with no such
# calls (a Linux executable of the linux target) raises OSError(ENOSYS).
#
# Pins and buses are numbered as the board numbers them (GPIO numbers on an
# ESP32). Not here: interrupts (Pin.irq), Signal, RTC, WDT, timers - the
# program loops and sleeps instead.

import sys
import time

_PIN_INIT = 0x7000
_PIN_WRITE = 0x7001
_PIN_READ = 0x7002
_ADC_INIT = 0x7010
_ADC_READ = 0x7011
_ADC_READ_UV = 0x7012
_PWM_INIT = 0x7020
_PWM_DUTY = 0x7021
_PWM_FREQ = 0x7022
_PWM_DEINIT = 0x7023
_I2C_INIT = 0x7030
_I2C_WRITE = 0x7031
_I2C_READ = 0x7032
_I2C_SCAN = 0x7033
_SPI_INIT = 0x7040
_SPI_XFER = 0x7041
_UART_INIT = 0x7050
_UART_WRITE = 0x7051
_UART_READ = 0x7052
_UART_ANY = 0x7053
_RESET = 0x7060
_FREQ = 0x7061
_UNIQUE_ID = 0x7062
_DEEPSLEEP = 0x7063

_ERRORS: dict[int, str] = {5: "Input/output error", 16: "Device or resource busy", 19: "No such device",
                           22: "Invalid argument", 38: "Function not implemented", 110: "Connection timed out"}


def _check(r: int) -> int:
    if r < 0:
        raise OSError(-r, _ERRORS.get(-r, "device error"))
    return r


def _call(n: int, a: int, b: int, c: int, d: int) -> int:
    return _check(sys.syscall(n, a, b, c, d)[0])


# ---------------------------------------------------------------- pins

class Pin:
    IN = 0
    OUT = 1
    OPEN_DRAIN = 2
    PULL_UP = 1
    PULL_DOWN = 2

    def __init__(self, id: int, mode: int = -1, pull: int = 0, value: int = -1) -> None:
        self.id = id
        self._mode = -1
        if mode >= 0:
            self.init(mode, pull, value)

    def init(self, mode: int, pull: int = 0, value: int = -1) -> None:
        if value >= 0 and mode != Pin.IN:      # (the level first: no glitch when it becomes an output)
            _call(_PIN_WRITE, self.id, 1 if value else 0, 0, 0)
        _call(_PIN_INIT, self.id, mode, pull, 0)
        self._mode = mode

    def value(self, v: int = -1) -> int:
        """value(): the level (0 or 1); value(x): set it (and return it)."""
        if v < 0:
            return _call(_PIN_READ, self.id, 0, 0, 0)
        level = 1 if v else 0
        _call(_PIN_WRITE, self.id, level, 0, 0)
        return level

    def on(self) -> None:
        _call(_PIN_WRITE, self.id, 1, 0, 0)

    def off(self) -> None:
        _call(_PIN_WRITE, self.id, 0, 0, 0)

    def toggle(self) -> None:
        self.value(1 - self.value())

    def __repr__(self) -> str:
        return "Pin(%d)" % self.id


# ---------------------------------------------------------------- ADC, PWM

class ADC:
    ATTN_0DB = 0
    ATTN_2_5DB = 1
    ATTN_6DB = 2
    ATTN_11DB = 3

    def __init__(self, pin: Pin, atten: int = 3) -> None:
        self.pin = pin
        self.atten(atten)

    def atten(self, a: int) -> None:
        _call(_ADC_INIT, self.pin.id, a, 0, 0)

    def read_u16(self) -> int:
        """The reading, scaled to 0..65535."""
        return _call(_ADC_READ, self.pin.id, 0, 0, 0)

    def read_uv(self) -> int:
        """The reading in microvolts (calibrated where the board can)."""
        return _call(_ADC_READ_UV, self.pin.id, 0, 0, 0)


class PWM:
    def __init__(self, pin: Pin, freq: int = 1000, duty_u16: int = 32768) -> None:
        self.pin = pin
        self._freq = freq
        self._duty = duty_u16
        _call(_PWM_INIT, pin.id, freq, duty_u16, 0)

    def freq(self, f: int = -1) -> int:
        if f >= 0:
            _call(_PWM_FREQ, self.pin.id, f, 0, 0)
            self._freq = f
        return self._freq

    def duty_u16(self, d: int = -1) -> int:
        if d >= 0:
            _call(_PWM_DUTY, self.pin.id, d, 0, 0)
            self._duty = d
        return self._duty

    def deinit(self) -> None:
        _call(_PWM_DEINIT, self.pin.id, 0, 0, 0)


# ---------------------------------------------------------------- buses

def _pin_id(p: Pin | None) -> int:
    if p is None:
        return -1
    return p.id


class I2C:
    def __init__(self, id: int, scl: Pin | None = None, sda: Pin | None = None, freq: int = 400000) -> None:
        self.id = id
        _call(_I2C_INIT, id, _pin_id(scl), _pin_id(sda), freq)

    def scan(self) -> list[int]:
        """The addresses that answer."""
        b = sys.buffer(128)
        _check(sys.syscall(_I2C_SCAN, self.id, b)[0])
        found = sys.peek_bytes(b, 0, 128)
        return [a for a in range(0x08, 0x78) if found[a]]

    def writeto(self, addr: int, buf: bytes, stop: bool = True) -> int:
        b = sys.buffer(buf)
        return _check(sys.syscall(_I2C_WRITE, self.id, addr, b, len(buf), 1 if stop else 0)[0])

    def readfrom(self, addr: int, nbytes: int, stop: bool = True) -> bytes:
        b = sys.buffer(nbytes)
        n = _check(sys.syscall(_I2C_READ, self.id, addr, b, nbytes, 1 if stop else 0)[0])
        return sys.peek_bytes(b, 0, n)

    def writeto_mem(self, addr: int, memaddr: int, buf: bytes) -> None:
        self.writeto(addr, bytes([memaddr & 0xFF]) + buf)

    def readfrom_mem(self, addr: int, memaddr: int, nbytes: int) -> bytes:
        self.writeto(addr, bytes([memaddr & 0xFF]), False)
        return self.readfrom(addr, nbytes)


class SPI:
    def __init__(self, id: int, baudrate: int = 1000000, polarity: int = 0, phase: int = 0,
                 sck: Pin | None = None, mosi: Pin | None = None, miso: Pin | None = None) -> None:
        self.id = id
        mode = (polarity & 1) | ((phase & 1) << 1)
        _check(sys.syscall(_SPI_INIT, id, _pin_id(sck), _pin_id(mosi), _pin_id(miso), baudrate, mode)[0])

    def write(self, buf: bytes) -> None:
        b = sys.buffer(buf)
        _check(sys.syscall(_SPI_XFER, self.id, b, 0, len(buf))[0])

    def read(self, nbytes: int) -> bytes:
        b = sys.buffer(nbytes)
        _check(sys.syscall(_SPI_XFER, self.id, 0, b, nbytes)[0])
        return sys.peek_bytes(b, 0, nbytes)

    def write_read(self, buf: bytes) -> bytes:
        """Sends buf and returns what came back meanwhile (as many bytes)."""
        t = sys.buffer(buf)
        r = sys.buffer(len(buf))
        _check(sys.syscall(_SPI_XFER, self.id, t, r, len(buf))[0])
        return sys.peek_bytes(r, 0, len(buf))


class UART:
    def __init__(self, id: int, baudrate: int = 115200, tx: Pin | None = None, rx: Pin | None = None) -> None:
        self.id = id
        _call(_UART_INIT, id, baudrate, _pin_id(tx), _pin_id(rx))

    def write(self, buf: bytes) -> int:
        b = sys.buffer(buf)
        return _check(sys.syscall(_UART_WRITE, self.id, b, len(buf))[0])

    def read(self, nbytes: int, timeout_ms: int = 0) -> bytes:
        b = sys.buffer(nbytes)
        n = _check(sys.syscall(_UART_READ, self.id, b, nbytes, timeout_ms)[0])
        return sys.peek_bytes(b, 0, n)

    def any(self) -> int:
        return _call(_UART_ANY, self.id, 0, 0, 0)


# ---------------------------------------------------------------- the board

def freq() -> int:
    """The CPU's frequency in Hz."""
    return _call(_FREQ, 0, 0, 0, 0)


def unique_id() -> bytes:
    b = sys.buffer(16)
    n = _check(sys.syscall(_UNIQUE_ID, b, 16)[0])
    return sys.peek_bytes(b, 0, n)


def reset() -> None:
    _call(_RESET, 0, 0, 0, 0)


def deepsleep(ms: int = 0) -> None:
    """Sleeps with the chip (nearly) off; it starts from the beginning when it wakes."""
    _call(_DEEPSLEEP, ms, 0, 0, 0)


def call(n: int, a: int = 0, b: int = 0, c: int = 0, d: int = 0) -> int:
    """One of the port's own device calls (0x7800 + n; see ppy_port.h)."""
    return _call(0x7800 + n, a, b, c, d)


# ---------------------------------------------------------------- time, as MicroPython has it

def sleep_ms(ms: int) -> None:
    time.sleep(ms / 1000)


def sleep_us(us: int) -> None:
    time.sleep(us / 1000000)


def ticks_ms() -> int:
    return time.monotonic_ns() // 1000000


def ticks_us() -> int:
    return time.monotonic_ns() // 1000


def ticks_diff(end: int, start: int) -> int:
    return end - start
