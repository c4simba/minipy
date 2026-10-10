"""Random numbers fit for secrets (CPython's secrets): token_bytes / token_hex / token_urlsafe,
choice, randbelow, randbits, compare_digest - from the system's random source."""
import base64
import binascii
import os
from hmac import compare_digest
from random import SystemRandom
from typing import Sequence, TypeVar

__all__ = ["choice", "randbelow", "randbits", "SystemRandom", "token_bytes", "token_hex", "token_urlsafe",
           "compare_digest"]

_T = TypeVar("_T")

_sysrand = SystemRandom()

DEFAULT_ENTROPY = 32                    # bytes


def randbelow(exclusive_upper_bound: int) -> int:
    """A random int in [0, exclusive_upper_bound)."""
    if exclusive_upper_bound <= 0:
        raise ValueError("Upper bound must be positive.")
    return _sysrand._randbelow(exclusive_upper_bound)


def randbits(k: int) -> int:
    """A random int of k bits."""
    return _sysrand.getrandbits(k)


def choice(seq: Sequence[_T]) -> _T:
    """A random item of seq."""
    return _sysrand.choice(seq)


def token_bytes(nbytes: int | None = None) -> bytes:
    """nbytes random bytes (32 without nbytes)."""
    if nbytes is None:
        nbytes = DEFAULT_ENTROPY
    return os.urandom(nbytes)


def token_hex(nbytes: int | None = None) -> str:
    """Random text of hexadecimal digits (two per byte)."""
    return binascii.hexlify(token_bytes(nbytes)).decode("ascii")


def token_urlsafe(nbytes: int | None = None) -> str:
    """Random URL-safe text (base64 of the bytes, no padding)."""
    tok = token_bytes(nbytes)
    return base64.urlsafe_b64encode(tok).rstrip(b"=").decode("ascii")
