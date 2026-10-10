"""Conversions between Python values and C structs packed in bytes (CPython's struct).

Formats: an optional first character for the byte order, size and alignment
('@' native, the default; '=' native order, standard sizes; '<' little-endian;
'>' and '!' big-endian), then items with optional repeat counts:
x pad byte, c char, b/B (un)signed char, ? bool, h/H short, i/I int, l/L long,
q/Q long long, n/N ssize_t/size_t (native only), e half float, f float,
d double, s bytes, p Pascal string, P pointer (native only).

Native sizes are those of the machine minipy runs on: a compiled program is an
i386 one (long, size_t and pointers of 4 bytes, 8-byte values aligned to 4).
In compiled programs the format of pack, unpack, unpack_from, iter_unpack and
calcsize is given as a literal (the compiler lays the items out and types the
values); Struct objects and pack_into are for the interpreter."""
import sys
import math
from typing import Callable, Generic, Iterator, TypeVar

T = TypeVar("T")
V = TypeVar("V")


class error(Exception):
    pass


# ---------------------------------------------------------------- one item each

_MIN = ["", "-128", "-32768", "", "-2147483648", "", "", "", "-9223372036854775808"]
_MAX = ["", "127", "32767", "", "2147483647", "", "", "", "9223372036854775807"]
_UMAX = ["", "255", "65535", "", "4294967295", "", "", "", "18446744073709551615"]


def _p_int(v: int, size: int, little: bool, signed: bool, code: str) -> bytes:
    """v as a size-byte integer."""
    bad = False
    if signed:
        if size < 8:
            half = 1 << (8 * size - 1)
            bad = v < -half or v >= half
    else:
        bad = v < 0 or (size < 8 and v >= (1 << (8 * size)))
    if bad:
        if signed:
            raise error("'" + code + "' format requires " + _MIN[size] + " <= number <= " + _MAX[size])
        raise error("'" + code + "' format requires 0 <= number <= " + _UMAX[size])
    out: list[int] = []
    for i in range(size):
        out.append((v >> (8 * i)) & 255)
    if not little:
        out.reverse()
    return bytes(out)


def _u_int(b: bytes, off: int, size: int, little: bool, signed: bool) -> int:
    """The size-byte integer at b[off:]."""
    first = off + size - 1 if little else off
    step = -1 if little else 1
    v = b[first]
    if signed and v >= 128:
        v -= 256
    for k in range(1, size):
        v = (v << 8) | b[first + step * k]
    return v


def _round_even(x: float) -> int:
    """x (>= 0, exact) rounded to an integer, halves to the even one."""
    n = math.floor(x)
    d = x - n
    if d > 0.5 or (d == 0.5 and n % 2 == 1):
        n += 1
    return n


def _float_bits(v: float, mbits: int, ebits: int, code: str) -> tuple[int, int]:
    """(sign, the other bits) of v as an IEEE 754 binary float with mbits stored mantissa
    bits and ebits exponent bits (the other bits are below 2**63: 8-byte ones go in two)."""
    sign = 1 if v < 0 or (v == 0 and math.copysign(1.0, v) < 0) else 0
    emax = (1 << ebits) - 1
    if v != v:
        return (sign, (emax << mbits) | (1 << (mbits - 1)))
    x = abs(v)
    if x == math.inf:
        return (sign, emax << mbits)
    if x == 0:
        return (sign, 0)
    bias = (1 << (ebits - 1)) - 1
    m, e = math.frexp(x)
    if e - 1 + bias >= 1:
        r = _round_even(math.ldexp(m, mbits + 1))
        if r == 1 << (mbits + 1):
            r >>= 1
            e += 1
        ex = e - 1 + bias
        if ex >= emax:
            raise OverflowError("float too large to pack with " + code + " format")
        return (sign, (ex << mbits) | (r - (1 << mbits)))
    return (sign, _round_even(math.ldexp(x, bias - 1 + mbits)))     # subnormal (or 0, or the smallest normal)


def _p_float(v: float, size: int, little: bool, code: str) -> bytes:
    """v as a size-byte (2, 4 or 8) IEEE 754 float."""
    out: list[int] = []
    if size == 8:
        sign, bits = _float_bits(v, 52, 11, code)
        lo = bits & 0xFFFFFFFF
        hi = (bits >> 32) | (sign << 31)
        for i in range(4):
            out.append((lo >> (8 * i)) & 255)
        for i in range(4):
            out.append((hi >> (8 * i)) & 255)
    else:
        if size == 4:
            sign, bits = _float_bits(v, 23, 8, code)
        else:
            sign, bits = _float_bits(v, 10, 5, code)
        bits |= sign << (8 * size - 1)
        for i in range(size):
            out.append((bits >> (8 * i)) & 255)
    if not little:
        out.reverse()
    return bytes(out)


def _u_float(b: bytes, off: int, size: int, little: bool) -> float:
    """The size-byte IEEE 754 float at b[off:]."""
    if size == 8:
        lo = _u_int(b, off if little else off + 4, 4, little, False)
        hi = _u_int(b, off + 4 if little else off, 4, little, False)
        sign = hi >> 31
        ex = (hi >> 20) & 0x7FF
        m = ((hi & 0xFFFFF) << 32) | lo
        mbits = 52
        emax = 0x7FF
    else:
        bits = _u_int(b, off, size, little, False)
        mbits = 23 if size == 4 else 10
        ebits = 8 if size == 4 else 5
        sign = bits >> (8 * size - 1)
        emax = (1 << ebits) - 1
        ex = (bits >> mbits) & emax
        m = bits & ((1 << mbits) - 1)
    bias = (emax >> 1)
    if ex == emax:
        x = math.inf if m == 0 else math.nan
    elif ex == 0:
        x = math.ldexp(float(m), 1 - bias - mbits)
    else:
        x = math.ldexp(float(m | (1 << mbits)), ex - bias - mbits)
    return -x if sign else x


def _p_bool(v: V) -> bytes:
    return b"\x01" if v else b"\x00"


def _u_bool(b: bytes, off: int) -> bool:
    return b[off] != 0


def _p_char(v: bytes) -> bytes:
    if len(v) != 1:
        raise error("char format requires a bytes object of length 1")
    return v


def _u_char(b: bytes, off: int) -> bytes:
    return b[off:off + 1]


def _p_str(v: bytes, n: int) -> bytes:
    """'Ns': v cut or padded with zero bytes to n bytes."""
    if len(v) >= n:
        return v[:n]
    return v + b"\x00" * (n - len(v))


def _u_str(b: bytes, off: int, n: int) -> bytes:
    return b[off:off + n]


def _p_pascal(v: bytes, n: int) -> bytes:
    """'Np': a length byte, then v cut or padded to n - 1 bytes."""
    if n == 0:
        return b""
    k = min(len(v), n - 1, 255)
    return bytes([k]) + v[:n - 1] + b"\x00" * (n - 1 - min(len(v), n - 1))


def _u_pascal(b: bytes, off: int, n: int) -> bytes:
    if n == 0:
        return b""
    k = min(b[off], n - 1)
    return b[off + 1:off + 1 + k]


def _pad(n: int) -> bytes:
    return b"\x00" * n


def _join(parts: list[bytes]) -> bytes:
    return b"".join(parts)


# ---------------------------------------------------------------- what compiled calls become

def _unpack1(buf: bytes, size: int, f: Callable[[bytes], T]) -> T:
    if len(buf) != size:
        raise error("unpack requires a buffer of " + str(size) + " bytes")
    return f(buf)


def _from_offset(n: int, size: int, offset: int) -> int:
    """unpack_from's offset into an n-byte buffer: checked, from the start."""
    if offset < 0:
        if offset + size > 0:
            raise error("not enough data to unpack " + str(size) + " bytes at offset " + str(offset))
        if offset + n < 0:
            raise error("offset " + str(offset) + " out of range for " + str(n) + "-byte buffer")
        offset += n
    if n - offset < size:
        raise error("unpack_from requires a buffer of at least " + str(size + offset) + " bytes for unpacking " + str(size)
                    + " bytes at offset " + str(offset) + " (actual buffer size is " + str(n) + ")")
    return offset


def _unpack_from1(buf: bytes, size: int, offset: int, f: Callable[[bytes, int], T]) -> T:
    return f(buf, _from_offset(len(buf), size, offset))


def _iter_unpack1(buf: bytes, size: int, f: Callable[[bytes, int], T]) -> Iterator[T]:
    if size == 0:
        raise error("cannot iteratively unpack with a struct of length 0")
    if len(buf) % size:
        raise error("iterative unpacking requires a buffer of a multiple of " + str(size) + " bytes")
    for off in range(0, len(buf), size):
        yield f(buf, off)


# ---------------------------------------------------------------- formats (the interpreter's side)

_STD = {"x": 1, "c": 1, "b": 1, "B": 1, "?": 1, "h": 2, "H": 2, "i": 4, "I": 4, "l": 4, "L": 4, "q": 8, "Q": 8,
        "e": 2, "f": 4, "d": 8, "s": 1, "p": 1}


def _native_size(c: str) -> int:
    if c in "lLnNP":
        if sys._compiled:
            return 4
        return 8
    return _STD[c]


def _native_align(c: str) -> int:
    if c in "xcbB?sp":
        return 1
    n = _native_size(c)
    if sys._compiled:
        if n > 4:
            return 4
    return n


_cache: dict[str, tuple[list[tuple[str, int, int]], int, bool]] = {}


def _layout(fmt: str) -> tuple[list[tuple[str, int, int]], int, bool]:
    """([(code, offset, size), ...] of the values, the total size, little-endian)."""
    if fmt in _cache:
        return _cache[fmt]
    i = 0
    native = True
    little = True
    if fmt and fmt[0] in "@=<>!":
        native = fmt[0] == "@"
        little = fmt[0] in "@=<"
        i = 1
    items: list[tuple[str, int, int]] = []
    size = 0
    n = len(fmt)
    while i < n:
        c = fmt[i]
        if c.isspace():
            i += 1
            continue
        num = 1
        if c >= "0" and c <= "9":
            num = 0
            while i < n and fmt[i] >= "0" and fmt[i] <= "9":
                num = num * 10 + ord(fmt[i]) - 48
                i += 1
            if i >= n:
                raise error("repeat count given without format specifier")
            c = fmt[i]
        i += 1
        if c not in _STD and not (native and c in "nNP"):
            raise error("bad char in struct format")
        if native:
            sz = _native_size(c)
            al = _native_align(c)
            size = (size + al - 1) // al * al
        else:
            sz = _STD[c]
        if c == "s" or c == "p":
            items.append((c, size, num))
            size += num
        elif c == "x":
            size += num
        else:
            for k in range(num):
                items.append((c, size, sz))
                size += sz
    got = (items, size, little)
    _cache[fmt] = got
    return got


def calcsize(fmt: str) -> int:
    """The size of the struct (the bytes) of format fmt."""
    return _layout(fmt)[1]


if not sys._compiled:
    def _pack_one(c, v, sz, little):
        if c == "c":
            if not isinstance(v, bytes):
                raise error("char format requires a bytes object of length 1")
            return _p_char(v)
        if c == "?":
            return _p_bool(v)
        if c == "s" or c == "p":
            if not isinstance(v, bytes):
                raise error("argument for '" + c + "' must be a bytes object")
            return _p_str(v, sz) if c == "s" else _p_pascal(v, sz)
        if c in "efd":
            if not isinstance(v, (int, float)):
                raise error("required argument is not a float")
            return _p_float(float(v), sz, little, c)
        if not isinstance(v, int):
            raise error("required argument is not an integer")
        return _p_int(int(v), sz, little, c.islower() or c == "n", c)

    def _unpack_one(c, b, off, sz, little):
        if c == "c":
            return _u_char(b, off)
        if c == "?":
            return _u_bool(b, off)
        if c == "s":
            return _u_str(b, off, sz)
        if c == "p":
            return _u_pascal(b, off, sz)
        if c in "efd":
            return _u_float(b, off, sz, little)
        return _u_int(b, off, sz, little, c.islower() or c == "n")

    def _bytes_of(buf):
        if isinstance(buf, bytes):
            return buf
        if type(buf).__name__ in ("bytearray", "memoryview"):
            return bytes(buf)
        raise TypeError("a bytes-like object is required, not '" + type(buf).__name__ + "'")

    def pack(fmt, *v):
        """The bytes of values v laid out by format fmt."""
        items, size, little = _layout(fmt)
        if len(v) != len(items):
            raise error("pack expected " + str(len(items)) + " items for packing (got " + str(len(v)) + ")")
        parts = []
        pos = 0
        for (c, off, sz), x in zip(items, v):
            if off > pos:
                parts.append(_pad(off - pos))
            parts.append(_pack_one(c, x, sz, little))
            pos = off + sz
        if size > pos:
            parts.append(_pad(size - pos))
        return b"".join(parts)

    def pack_into(fmt, buffer, offset, *v):
        """Writes values v laid out by format fmt into the writable buffer (a bytearray) at offset."""
        items, size, little = _layout(fmt)
        if type(buffer).__name__ not in ("bytearray", "memoryview"):
            raise TypeError("argument must be read-write bytes-like object, not " + type(buffer).__name__)
        if len(v) != len(items):
            raise error("pack_into expected " + str(len(items)) + " items for packing (got " + str(len(v)) + ")")
        n = len(buffer)
        if offset < 0:
            if offset + size > 0:
                raise error("no space to pack " + str(size) + " bytes at offset " + str(offset))
            if offset + n < 0:
                raise error("offset " + str(offset) + " out of range for " + str(n) + "-byte buffer")
            offset += n
        if n - offset < size:
            raise error("pack_into requires a buffer of at least " + str(size + offset) + " bytes for packing " + str(size) +
                        " bytes at offset " + str(offset) + " (actual buffer size is " + str(n) + ")")
        buffer[offset:offset + size] = pack(fmt, *v)

    def unpack(fmt, buffer):
        """The values in buffer (calcsize(fmt) bytes) laid out by format fmt."""
        items, size, little = _layout(fmt)
        b = _bytes_of(buffer)
        if len(b) != size:
            raise error("unpack requires a buffer of " + str(size) + " bytes")
        return tuple(_unpack_one(c, b, off, sz, little) for c, off, sz in items)

    def unpack_from(fmt, /, buffer, offset=0):
        """The values laid out by format fmt in buffer from offset on."""
        items, size, little = _layout(fmt)
        b = _bytes_of(buffer)
        offset = _from_offset(len(b), size, offset)
        return tuple(_unpack_one(c, b, offset + off, sz, little) for c, off, sz in items)

    def iter_unpack(fmt, buffer):
        """The values of each calcsize(fmt) bytes of buffer, one tuple at a time."""
        items, size, little = _layout(fmt)
        b = _bytes_of(buffer)
        return _iter_unpack1(b, size, lambda b, base: tuple(_unpack_one(c, b, base + off, sz, little) for c, off, sz in items))

    class Struct:
        """A compiled format: format, size, pack, unpack, unpack_from, iter_unpack."""

        def __init__(self, format):
            if isinstance(format, bytes):
                format = format.decode("ascii")
            if not isinstance(format, str):
                raise TypeError("Struct() argument 1 must be a str or bytes object, not " + type(format).__name__)
            self.format = format
            self.size = _layout(format)[1]

        def pack(self, *v):
            return pack(self.format, *v)

        def unpack(self, buffer):
            return unpack(self.format, buffer)

        def pack_into(self, buffer, offset, *v):
            pack_into(self.format, buffer, offset, *v)

        def unpack_from(self, buffer, offset=0):
            return unpack_from(self.format, buffer, offset)

        def iter_unpack(self, buffer):
            return iter_unpack(self.format, buffer)

        def __repr__(self):
            return "Struct(" + repr(self.format) + ")"

if sys._compiled:
    class _CStruct(Generic[T, V]):
        """Struct(format) of a compiled program (format a literal): the compiler gives the function
        unpacking the values at an offset and the one packing them (the field pack)."""

        def __init__(self, format: str, size: int, u: Callable[[bytes, int], T], p: V):
            self.format = format
            self.size = size
            self._u = u
            self.pack = p

        def unpack(self, buffer: bytes) -> T:
            if len(buffer) != self.size:
                raise error("unpack requires a buffer of " + str(self.size) + " bytes")
            return self._u(buffer, 0)

        def unpack_from(self, buffer: bytes, offset: int = 0) -> T:
            return self._u(buffer, _from_offset(len(buffer), self.size, offset))

        def iter_unpack(self, buffer: bytes) -> Iterator[T]:
            return _iter_unpack1(buffer, self.size, self._u)

        def __repr__(self) -> str:
            return "Struct(" + repr(self.format) + ")"

    Struct = _CStruct

    # (the compiler turns the calls with a literal format into the helpers' calls above)
    def pack(fmt, *v):
        raise error("struct.pack: the format must be a literal")

    def unpack(fmt, buffer):
        raise error("struct.unpack: the format must be a literal")

    def unpack_from(fmt, buffer, offset=0):
        raise error("struct.unpack_from: the format must be a literal")

    def iter_unpack(fmt, buffer):
        raise error("struct.iter_unpack: the format must be a literal")
