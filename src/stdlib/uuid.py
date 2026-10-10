"""UUID objects (universally unique identifiers) per RFC 9562 (CPython's uuid): UUID, uuid1,
uuid3, uuid4, uuid5, uuid6, uuid7, uuid8, getnode, NAMESPACE_*, NIL, MAX.

A UUID keeps its 16 bytes. Compiled programs have 64-bit ints: there u.int (and UUID(int=...))
works for values below 2**63 and raises OverflowError beyond."""
import hashlib
import os
import sys
import time
from enum import Enum
from typing import TypeVar

__all__ = ["UUID", "uuid1", "uuid3", "uuid4", "uuid5", "uuid6", "uuid7", "uuid8", "getnode", "NAMESPACE_DNS",
           "NAMESPACE_URL", "NAMESPACE_OID", "NAMESPACE_X500", "NIL", "MAX", "SafeUUID", "RESERVED_NCS",
           "RFC_4122", "RESERVED_MICROSOFT", "RESERVED_FUTURE"]

_T = TypeVar("_T")

RESERVED_NCS, RFC_4122, RESERVED_MICROSOFT, RESERVED_FUTURE = [
    'reserved for NCS compatibility', 'specified in RFC 4122',
    'reserved for Microsoft compatibility', 'reserved for future definition']

_HEX = "0123456789abcdef"


class SafeUUID(Enum):
    _value_: int | None
    safe = 0
    unsafe = -1
    unknown = None


def _hexval(c: str) -> int:
    o = ord(c)
    if 48 <= o <= 57:
        return o - 48
    if 97 <= o <= 102:
        return o - 87
    if 65 <= o <= 70:
        return o - 55
    return -1


def _from_int(n: int, size: int) -> bytes:
    out = b""
    for _ in range(size):
        out = bytes([n & 0xFF]) + out
        n >>= 8
    return out


def _to_int(b: bytes) -> int:
    n = 0
    for x in b:
        n = (n << 8) | x
    return n


class UUID:
    """A UUID: UUID('12345678-1234-5678-1234-567812345678'), UUID(bytes=...), UUID(int=...) ..."""

    def __init__(self, hex: str | None = None, bytes: bytes | None = None, bytes_le: bytes | None = None,
                 fields: tuple[int, int, int, int, int, int] | None = None, int: int | None = None,
                 version: int | None = None, *, is_safe: SafeUUID = SafeUUID.unknown) -> None:
        given = 0
        for x in (hex is not None, bytes is not None, bytes_le is not None, fields is not None, int is not None):
            if x:
                given += 1
        if given != 1:
            raise TypeError('one of the hex, bytes, bytes_le, fields, or int arguments must be given')
        data = b""
        if hex is not None:
            h = hex.replace('urn:', '').replace('uuid:', '')
            h = h.strip('{}').replace('-', '')
            if len(h) != 32:
                raise ValueError('badly formed hexadecimal UUID string')
            parts: list[int] = []
            for k in range(16):
                a = _hexval(h[2 * k])
                b2 = _hexval(h[2 * k + 1])
                if a < 0 or b2 < 0:
                    raise ValueError("invalid literal for int() with base 16: " + repr(h))
                parts.append(a * 16 + b2)
            data = _bytes_of(parts)
        elif bytes_le is not None:
            if len(bytes_le) != 16:
                raise ValueError('bytes_le is not a 16-char string')
            data = (bytes_le[4 - 1::-1] + bytes_le[6 - 1:4 - 1:-1] + bytes_le[8 - 1:6 - 1:-1] + bytes_le[8:])
        elif bytes is not None:
            if len(bytes) != 16:
                raise ValueError('bytes is not a 16-char string')
            data = bytes
        elif fields is not None:
            if len(fields) != 6:
                raise ValueError('fields is not a 6-tuple')
            time_low, time_mid, time_hi_version, clock_seq_hi_variant, clock_seq_low, node = fields
            if not 0 <= time_low < (1 << 32):
                raise ValueError('field 1 out of range (need a 32-bit value)')
            if not 0 <= time_mid < (1 << 16):
                raise ValueError('field 2 out of range (need a 16-bit value)')
            if not 0 <= time_hi_version < (1 << 16):
                raise ValueError('field 3 out of range (need a 16-bit value)')
            if not 0 <= clock_seq_hi_variant < (1 << 8):
                raise ValueError('field 4 out of range (need an 8-bit value)')
            if not 0 <= clock_seq_low < (1 << 8):
                raise ValueError('field 5 out of range (need an 8-bit value)')
            if not 0 <= node < (1 << 48):
                raise ValueError('field 6 out of range (need a 48-bit value)')
            data = (_from_int(time_low, 4) + _from_int(time_mid, 2) + _from_int(time_hi_version, 2) +
                    _from_int(clock_seq_hi_variant, 1) + _from_int(clock_seq_low, 1) + _from_int(node, 6))
        elif int is not None:
            if int < 0:
                raise ValueError('int is out of range (need a 128-bit value)')
            data = _from_int(int, 16)
            if _to_int(data) != int:
                raise ValueError('int is out of range (need a 128-bit value)')
        if version is not None:
            if not 1 <= version <= 8:
                raise ValueError('illegal version number')
            b = list(data)
            b[8] = (b[8] & 0x3F) | 0x80             # the variant: RFC 4122 / 9562
            b[6] = (b[6] & 0x0F) | (version << 4)
            data = _bytes_of(b)
        self._bytes = data
        self.is_safe = is_safe

    @property
    def bytes(self) -> bytes:
        return self._bytes

    @property
    def bytes_le(self) -> bytes:
        b = self._bytes
        return b[4 - 1::-1] + b[6 - 1:4 - 1:-1] + b[8 - 1:6 - 1:-1] + b[8:]

    @property
    def int(self) -> int:
        if sys._compiled and self._bytes[0] >= 0x80 or sys._compiled and _to_int(self._bytes[:8]) != 0:
            raise OverflowError("a UUID's int needs 128 bits (compiled ints have 64)")
        return _to_int(self._bytes)

    @property
    def hex(self) -> str:
        out = ""
        for x in self._bytes:
            out += _HEX[x >> 4] + _HEX[x & 15]
        return out

    @property
    def fields(self) -> tuple[int, int, int, int, int, int]:
        return (self.time_low, self.time_mid, self.time_hi_version, self.clock_seq_hi_variant, self.clock_seq_low,
                self.node)

    @property
    def time_low(self) -> int:
        return _to_int(self._bytes[0:4])

    @property
    def time_mid(self) -> int:
        return _to_int(self._bytes[4:6])

    @property
    def time_hi_version(self) -> int:
        return _to_int(self._bytes[6:8])

    @property
    def clock_seq_hi_variant(self) -> int:
        return self._bytes[8]

    @property
    def clock_seq_low(self) -> int:
        return self._bytes[9]

    @property
    def time(self) -> int:
        if self.version == 6:
            return (self.time_low << 28) | (self.time_mid << 12) | (self.time_hi_version & 0x0fff)
        if self.version == 7:
            return _to_int(self._bytes[0:6])
        return ((self.time_hi_version & 0x0fff) << 48) | (self.time_mid << 32) | self.time_low

    @property
    def clock_seq(self) -> int:
        return ((self.clock_seq_hi_variant & 0x3f) << 8) | self.clock_seq_low

    @property
    def node(self) -> int:
        return _to_int(self._bytes[10:16])

    @property
    def urn(self) -> str:
        return 'urn:uuid:' + str(self)

    @property
    def variant(self) -> str:
        v = self._bytes[8]
        if not v & 0x80:
            return RESERVED_NCS
        elif not v & 0x40:
            return RFC_4122
        elif not v & 0x20:
            return RESERVED_MICROSOFT
        else:
            return RESERVED_FUTURE

    @property
    def version(self) -> int | None:
        if self.variant == RFC_4122:
            return self._bytes[6] >> 4
        return None

    def __str__(self) -> str:
        h = self.hex
        return h[:8] + '-' + h[8:12] + '-' + h[12:16] + '-' + h[16:20] + '-' + h[20:]

    def __repr__(self) -> str:
        return type(self).__name__ + "(" + repr(str(self)) + ")"

    def __eq__(self, other: "UUID") -> bool:
        return self._bytes == other._bytes

    def __ne__(self, other: "UUID") -> bool:
        return self._bytes != other._bytes

    def __lt__(self, other: "UUID") -> bool:
        return self._bytes < other._bytes

    def __le__(self, other: "UUID") -> bool:
        return self._bytes <= other._bytes

    def __gt__(self, other: "UUID") -> bool:
        return self._bytes > other._bytes

    def __ge__(self, other: "UUID") -> bool:
        return self._bytes >= other._bytes

    def __hash__(self) -> int:
        return hash(self._bytes)


def _bytes_of(parts: list[int]) -> bytes:
    return bytes(parts)


_node: list[int] = []


def getnode() -> int:
    """This computer's 48-bit node (here: a random one, with the multicast bit, as RFC 4122 allows)."""
    if not _node:
        _node.append(_to_int(os.urandom(6)) | (1 << 40))
    return _node[0]


_last_timestamp: list[int] = []


def uuid1(node: int | None = None, clock_seq: int | None = None) -> UUID:
    """A UUID from the time, a node (getnode()) and a clock sequence."""
    nanoseconds = time.time_ns()
    timestamp = nanoseconds // 100 + 0x01b21dd213814000
    if _last_timestamp and timestamp <= _last_timestamp[0]:
        timestamp = _last_timestamp[0] + 1
    if _last_timestamp:
        _last_timestamp[0] = timestamp
    else:
        _last_timestamp.append(timestamp)
    if clock_seq is None:
        clock_seq = _to_int(os.urandom(2)) & 0x3fff
    time_low = timestamp & 0xffffffff
    time_mid = (timestamp >> 32) & 0xffff
    time_hi_version = (timestamp >> 48) & 0x0fff
    clock_seq_low = clock_seq & 0xff
    clock_seq_hi_variant = (clock_seq >> 8) & 0x3f
    if node is None:
        node = getnode()
    return UUID(fields=(time_low, time_mid, time_hi_version, clock_seq_hi_variant, clock_seq_low, node), version=1)


def _name_bytes(name: _T) -> bytes:
    if isinstance(name, str):
        return name.encode("utf-8")
    else:
        return bytes(name)


def uuid3(namespace: UUID, name: _T) -> UUID:
    """A UUID from the MD5 hash of a namespace UUID and a name."""
    h = hashlib.md5(namespace.bytes + _name_bytes(name)).digest()
    return UUID(bytes=h[:16], version=3)


def uuid4() -> UUID:
    """A random UUID."""
    return UUID(bytes=os.urandom(16), version=4)


def uuid5(namespace: UUID, name: _T) -> UUID:
    """A UUID from the SHA-1 hash of a namespace UUID and a name."""
    h = hashlib.sha1(namespace.bytes + _name_bytes(name)).digest()
    return UUID(bytes=h[:16], version=5)


def uuid6(node: int | None = None, clock_seq: int | None = None) -> UUID:
    """A UUID from the time (most significant first: they sort by time), a node and a clock sequence."""
    timestamp = time.time_ns() // 100 + 0x01b21dd213814000
    if clock_seq is None:
        clock_seq = _to_int(os.urandom(2)) & 0x3fff
    if node is None:
        node = _to_int(os.urandom(6))
    time_hi_and_mid = (timestamp >> 12) & 0xffff_ffff_ffff
    time_lo = timestamp & 0x0fff
    data = _from_int(time_hi_and_mid, 6) + _from_int(time_lo, 2) + _from_int(clock_seq & 0x3fff, 2) + _from_int(node, 6)
    return UUID(bytes=data, version=6)


_last7: list[int] = []


def uuid7() -> UUID:
    """A UUID from the Unix time in milliseconds and random bits (they sort by time)."""
    ms = time.time_ns() // 1_000_000
    rand = os.urandom(10)
    data = _from_int(ms & 0xffff_ffff_ffff, 6) + rand
    return UUID(bytes=data, version=7)


def uuid8(a: int | None = None, b: int | None = None, c: int | None = None) -> UUID:
    """A custom UUID from three blocks: a (48 bits), b (12 bits), c (62 bits)."""
    if a is None:
        a = _to_int(os.urandom(6))
    if b is None:
        b = _to_int(os.urandom(2))
    if c is None:
        c = _to_int(os.urandom(8))
    hi = ((a & 0xffff_ffff_ffff) << 16) | (b & 0xfff)
    lo = c & 0x3fff_ffff_ffff_ffff
    return UUID(bytes=_from_int(hi, 8) + _from_int(lo, 8), version=8)


NAMESPACE_DNS = UUID('6ba7b810-9dad-11d1-80b4-00c04fd430c8')
NAMESPACE_URL = UUID('6ba7b811-9dad-11d1-80b4-00c04fd430c8')
NAMESPACE_OID = UUID('6ba7b812-9dad-11d1-80b4-00c04fd430c8')
NAMESPACE_X500 = UUID('6ba7b814-9dad-11d1-80b4-00c04fd430c8')

NIL = UUID('00000000-0000-0000-0000-000000000000')
MAX = UUID('ffffffff-ffff-ffff-ffff-ffffffffffff')
