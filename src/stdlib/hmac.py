"""HMAC (keyed hashing for message authentication, RFC 2104) over hashlib's hashes
(CPython's hmac)."""
import hashlib
from typing import TypeVar

_B = TypeVar("_B")
_C = TypeVar("_C")

trans_5C = bytes([x ^ 0x5C for x in range(256)])
trans_36 = bytes([x ^ 0x36 for x in range(256)])
digest_size = None


def _name_of(digestmod) -> str:
    if isinstance(digestmod, str):
        return digestmod
    else:
        return digestmod().name


class HMAC:
    """A running HMAC: update(), digest(), hexdigest(), copy()."""

    def __init__(self, key: bytes, msg: bytes | None = None, digestmod: str = ""):
        if not digestmod:
            raise TypeError("Missing required argument 'digestmod'.")
        self._name = digestmod.lower()
        inner = hashlib.new(self._name)
        self.block_size = inner.block_size
        self.digest_size = inner.digest_size
        if len(key) > self.block_size:
            key = hashlib.new(self._name, key).digest()
        key = key + b"\x00" * (self.block_size - len(key))
        self._inner = hashlib.new(self._name, bytes([k ^ 0x36 for k in key]))
        self._outer = hashlib.new(self._name, bytes([k ^ 0x5C for k in key]))
        if msg is not None:
            self.update(msg)

    @property
    def name(self) -> str:
        return "hmac-" + self._inner.name

    def update(self, msg: bytes) -> None:
        self._inner.update(msg)

    def copy(self) -> "HMAC":
        c = HMAC(b"", None, self._name)
        c._inner = self._inner.copy()
        c._outer = self._outer.copy()
        return c

    def digest(self) -> bytes:
        h = self._outer.copy()
        h.update(self._inner.digest())
        return h.digest()

    def hexdigest(self) -> str:
        return self.digest().hex()


def new(key: bytes, msg: bytes | None = None, digestmod=None) -> HMAC:
    """An HMAC of key and msg with hash digestmod (a name such as 'sha256', or hashlib.sha256)."""
    if digestmod is None:
        raise TypeError("Missing required argument 'digestmod'.")
    else:
        return HMAC(key, msg, _name_of(digestmod))


def digest(key: bytes, msg: bytes, digest) -> bytes:
    """The HMAC of key and msg at once."""
    return HMAC(key, msg, _name_of(digest)).digest()


def _digest_bytes(x: _B) -> bytes:
    if isinstance(x, str):
        if not x.isascii():
            raise TypeError("comparing strings with non-ASCII characters is not supported")
        return x.encode("ascii")
    else:
        return bytes(x)


def compare_digest(a: _B, b: _C) -> bool:
    """a == b (two str, or two bytes-like), in a time that does not depend on where they differ."""
    if isinstance(a, str) != isinstance(b, str):
        raise TypeError("unsupported operand types(s) or combination of types: '" + type(a).__name__ + "' and '" +
                        type(b).__name__ + "'")
    x = _digest_bytes(a)
    y = _digest_bytes(b)
    if len(x) != len(y):
        return False
    r = 0
    for i in range(len(x)):
        r |= x[i] ^ y[i]
    return r == 0
