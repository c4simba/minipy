"""Conversions between binary data and ASCII (CPython's binascii): hex, base64, CRCs."""
import sys
from typing import TypeVar

S = TypeVar("S")


class Error(ValueError):
    pass


class Incomplete(Exception):
    pass


def _ascii_bytes(s: S) -> bytes:
    """The bytes of an a2b_* argument (an ASCII str is taken too)."""
    if isinstance(s, str):
        if not s.isascii():
            raise ValueError("string argument should contain only ASCII characters")
        return s.encode("ascii")
    elif isinstance(s, bytes):
        return s
    else:
        if not sys._compiled:
            if type(s).__name__ in ("bytearray", "memoryview"):
                return bytes(s)
        raise TypeError("argument should be bytes, buffer or ASCII string, not '" + type(s).__name__ + "'")


def hexlify(data: bytes, sep=None, bytes_per_sep: int = 1) -> bytes:
    """Each byte as two hex digits (sep between groups of bytes_per_sep bytes)."""
    if sep is None:
        return data.hex().encode("ascii")
    else:
        return data.hex(sep, bytes_per_sep).encode("ascii")


def b2a_hex(data: bytes, sep=None, bytes_per_sep: int = 1) -> bytes:
    if sep is None:
        return data.hex().encode("ascii")
    else:
        return data.hex(sep, bytes_per_sep).encode("ascii")


def unhexlify(hexstr: S) -> bytes:
    """The bytes two hex digits each stand for."""
    return _a2b_hex(_ascii_bytes(hexstr))


def a2b_hex(hexstr: S) -> bytes:
    return _a2b_hex(_ascii_bytes(hexstr))


def _a2b_hex(s: bytes) -> bytes:
    if len(s) % 2:
        raise Error("Odd-length string")
    out: list[int] = []
    for i in range(0, len(s), 2):
        hi = _hexval(s[i])
        lo = _hexval(s[i + 1])
        if hi < 0 or lo < 0:
            raise Error("Non-hexadecimal digit found")
        out.append(hi * 16 + lo)
    return bytes(out)


def _hexval(c: int) -> int:
    if c >= 48 and c <= 57:
        return c - 48
    if c >= 97 and c <= 102:
        return c - 87
    if c >= 65 and c <= 70:
        return c - 55
    return -1


_B64 = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"


def b2a_base64(data: bytes, *, newline: bool = True) -> bytes:
    """data in base64 (and a newline)."""
    out: list[int] = []
    n = len(data)
    i = 0
    while i + 2 < n:
        v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2]
        out.append(_B64[v >> 18])
        out.append(_B64[(v >> 12) & 63])
        out.append(_B64[(v >> 6) & 63])
        out.append(_B64[v & 63])
        i += 3
    rest = n - i
    if rest == 1:
        v = data[i] << 16
        out.append(_B64[v >> 18])
        out.append(_B64[(v >> 12) & 63])
        out.append(61)
        out.append(61)
    elif rest == 2:
        v = (data[i] << 16) | (data[i + 1] << 8)
        out.append(_B64[v >> 18])
        out.append(_B64[(v >> 12) & 63])
        out.append(_B64[(v >> 6) & 63])
        out.append(61)
    if newline:
        out.append(10)
    return bytes(out)


def _b64val(c: int) -> int:
    if c >= 65 and c <= 90:
        return c - 65
    if c >= 97 and c <= 122:
        return c - 71
    if c >= 48 and c <= 57:
        return c + 4
    if c == 43:
        return 62
    if c == 47:
        return 63
    return -1


def a2b_base64(data: S, *, strict_mode: bool = False) -> bytes:
    """The bytes of base64 data. Characters outside the alphabet are skipped
    (strict_mode: an error, as is padding anywhere but at the end)."""
    return _a2b_base64(_ascii_bytes(data), strict_mode)


def _a2b_base64(s: bytes, strict: bool) -> bytes:
    out: list[int] = []
    left = 0
    quad = 0
    pads = 0
    for i in range(len(s)):
        c = s[i]
        if c == 61:
            pads += 1
            if quad >= 2 and quad + pads <= 4:
                continue
            if not strict:
                continue
            if quad == 1:
                break
            raise Error("Leading padding not allowed" if quad == 0 and i == 0 else "Excess padding not allowed")
        v = _b64val(c)
        if v < 0:
            if strict:
                raise Error("Only base64 data is allowed")
            continue
        if pads and strict:
            raise Error("Excess data after padding" if quad + pads == 4 else "Discontinuous padding not allowed")
        pads = 0
        if quad == 0:
            left = v
            quad = 1
        elif quad == 1:
            out.append(((left << 2) | (v >> 4)) & 255)
            left = v & 15
            quad = 2
        elif quad == 2:
            out.append(((left << 4) | (v >> 2)) & 255)
            left = v & 3
            quad = 3
        else:
            out.append(((left << 6) | v) & 255)
            left = 0
            quad = 0
    if quad == 1:
        raise Error("Invalid base64-encoded string: number of data characters (" + str(len(out) // 3 * 4 + 1)
                    + ") cannot be 1 more than a multiple of 4")
    if quad and quad + pads < 4:
        raise Error("Incorrect padding")
    return bytes(out)


_CRC32: list[int] = []


def crc32(data: bytes, value: int = 0) -> int:
    """The CRC-32 of data (continuing from value)."""
    if not _CRC32:
        for k in range(256):
            c = k
            for j in range(8):
                if c & 1:
                    c = (c >> 1) ^ 0xEDB88320
                else:
                    c >>= 1
            _CRC32.append(c)
    crc = (value & 0xFFFFFFFF) ^ 0xFFFFFFFF
    for b in data:
        crc = _CRC32[(crc ^ b) & 255] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


def crc_hqx(data: bytes, value: int) -> int:
    """The CRC-CCITT (XModem) of data, starting from value."""
    crc = value & 0xFFFF
    for b in data:
        crc ^= b << 8
        for j in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc
