"""codecs: str.encode / bytes.decode as CPython's (codecs.encode, codecs.decode, lookup). utf-8, ascii and latin-1 are
built in (this module looks at them only when they fail, to report the error as
CPython does); the code pages (cp866 of KolibriOS's text files, cp1251, koi8-r,
iso8859-x ...), utf-16 and utf-32 are in _codecs_more, which registers itself
here when a program has it (the compiler adds it to programs that name an
encoding; the interpreter loads it when one is asked for). Error handlers:
strict, ignore, replace, backslashreplace, xmlcharrefreplace (encoding). The
compiler sends every encode()/decode() of a program here."""
import sys
from typing import Callable

BOM_UTF8 = b"\xef\xbb\xbf"
BOM_UTF16_LE = b"\xff\xfe"
BOM_UTF16_BE = b"\xfe\xff"
BOM_UTF32_LE = b"\xff\xfe\x00\x00"
BOM_UTF32_BE = b"\x00\x00\xfe\xff"
BOM_LE = BOM_UTF16_LE
BOM_BE = BOM_UTF16_BE
BOM_UTF16 = BOM_UTF16_LE
BOM_UTF32 = BOM_UTF32_LE
BOM = BOM_UTF16_LE

_more_decode: Callable[[bytes, str, str], str] | None = None
_more_encode: Callable[[str, str, str], bytes] | None = None
_more_lookup: Callable[[str], str] | None = None


class CodecInfo:
    """What lookup() gives: the codec's name (encode/decode: this module's)."""

    def __init__(self, name: str):
        self.name = name

    def __repr__(self) -> str:
        return "<codecs.CodecInfo object for encoding " + self.name + ">"


def lookup(encoding: str) -> CodecInfo:
    name = _native(encoding)
    if not name:
        more = _more_lookup
        if more is None:
            raise LookupError("unknown encoding: " + encoding)
        name = more(encoding)
    return CodecInfo(name)


def _register(decode_fn: Callable[[bytes, str, str], str], encode_fn: Callable[[str, str, str], bytes], lookup_fn: Callable[[str], str]) -> None:
    global _more_decode, _more_encode, _more_lookup
    _more_decode = decode_fn
    _more_encode = encode_fn
    _more_lookup = lookup_fn


def _ascii_lower(encoding: str) -> str:
    e = ""
    for ch in encoding:                           # (ASCII lower case: no Unicode tables needed)
        if ch >= "A" and ch <= "Z":
            ch = chr(ord(ch) + 32)
        elif ch == "_" or ch == " ":
            ch = "-"
        e += ch
    return e


def _native(encoding: str) -> str:
    """utf-8, utf-8-sig, ascii or latin-1 for those names, else ""."""
    e = _ascii_lower(encoding)
    if e == "utf-8" or e == "utf8" or e == "u8" or e == "utf" or e == "cp65001":
        return "utf-8"
    if e == "utf-8-sig" or e == "utf8-sig":
        return "utf-8-sig"
    if e == "ascii" or e == "us-ascii" or e == "646":
        return "ascii"
    if e in ("latin-1", "latin1", "iso-8859-1", "iso8859-1", "8859", "l1", "latin", "cp819"):
        return "latin-1"
    return ""


def _check_errors(errors: str) -> None:
    if errors not in ("strict", "ignore", "replace", "backslashreplace", "xmlcharrefreplace", "surrogatepass",
                      "surrogateescape"):
        raise LookupError("unknown error handler name '" + errors + "'")


def _hex2(b: int) -> str:
    return "0123456789abcdef"[b >> 4] + "0123456789abcdef"[b & 15]


def _escape(c: int) -> str:
    if c <= 0xFF:
        return "\\x" + format(c, "02x")
    if c <= 0xFFFF:
        return "\\u" + format(c, "04x")
    return "\\U" + format(c, "08x")


def _bad_decode(errors: str, encoding: str, data: bytes, start: int, end: int, reason: str, out: list[str]) -> None:
    if errors == "strict" or errors == "surrogatepass":
        raise UnicodeDecodeError(encoding, data, start, end, reason)
    if errors == "replace":
        out.append("\ufffd")
    elif errors == "backslashreplace":
        for k in range(start, end):
            out.append("\\x" + _hex2(data[k]))
    elif errors == "xmlcharrefreplace":
        raise TypeError("don't know how to handle UnicodeDecodeError in error callback")
    elif errors == "surrogateescape":                  # (bytes 0x80-0xFF as U+DC80-U+DCFF)
        for k in range(start, end):
            if data[k] < 128:
                raise UnicodeDecodeError(encoding, data, start, end, reason)
        for k in range(start, end):
            out.append(chr(0xDC00 + data[k]))


def _bad_encode(errors: str, encoding: str, s: str, start: int, end: int, reason: str, out: list[bytes]) -> None:
    if errors == "strict" or (errors == "surrogatepass" and encoding != "utf-8"):
        raise UnicodeEncodeError(encoding, s, start, end, reason)
    if errors == "replace":
        out.append(b"?" * (end - start))
    elif errors == "backslashreplace":
        out.append("".join([_escape(ord(ch)) for ch in s[start:end]]).encode("ascii"))
    elif errors == "xmlcharrefreplace":
        out.append("".join(["&#" + str(ord(ch)) + ";" for ch in s[start:end]]).encode("ascii"))
    elif errors == "surrogatepass" and encoding == "utf-8":
        out.append(s[start:end].encode("utf-8"))      # (surrogates: their UTF-8 form)
    elif errors == "surrogateescape":                  # (U+DC80-U+DCFF: the bytes they stand for)
        raw: list[int] = []
        for ch in s[start:end]:
            c = ord(ch)
            if c < 0xDC80 or c > 0xDCFF:
                raise UnicodeEncodeError(encoding, s, start, end, reason)
            raw.append(c - 0xDC00)
        out.append(bytes(raw))


# ---------------------------------------------------------------- utf-8, ascii, latin-1 (what fails in the built-in ones)

def _utf8_seq(data: bytes, i: int) -> tuple[int, int, str]:
    """(length of the valid sequence at i, or 0; where the bad part ends; why), CPython's decoder."""
    c = data[i]
    if c < 0x80:
        return (1, i + 1, "")
    if c < 0xC2 or c >= 0xF5:
        return (0, i + 1, "invalid start byte")
    lo = 0x80
    hi = 0xBF
    if c < 0xE0:
        need = 1
    elif c < 0xF0:
        need = 2
        if c == 0xE0:
            lo = 0xA0
        elif c == 0xED:
            hi = 0x9F
    else:
        need = 3
        if c == 0xF0:
            lo = 0x90
        elif c == 0xF4:
            hi = 0x8F
    for k in range(1, need + 1):
        if i + k >= len(data):
            return (0, len(data), "unexpected end of data")
        d = data[i + k]
        if k == 1:
            if d < lo or d > hi:
                return (0, i + k, "invalid continuation byte")
        elif d < 0x80 or d > 0xBF:
            return (0, i + k, "invalid continuation byte")
    return (need + 1, i + need + 1, "")


def _decode_utf8(data: bytes, errors: str, name: str) -> str:
    out: list[str] = []
    i = 0
    start = 0
    n = len(data)
    while i < n:
        length, end, why = _utf8_seq(data, i)
        if length:
            i += length
            continue
        if i > start:
            out.append(data[start:i].decode("utf-8"))
        if (errors == "surrogatepass" and data[i] == 0xED and i + 2 < n and 0xA0 <= data[i + 1] <= 0xBF
                and 0x80 <= data[i + 2] <= 0xBF):
            out.append(chr(((data[i] & 0x0F) << 12) | ((data[i + 1] & 0x3F) << 6) | (data[i + 2] & 0x3F)))
            i += 3
            start = i
            continue
        _bad_decode(errors, name, data, i, end, why, out)
        i = end
        start = i
    if start < n:
        out.append(data[start:].decode("utf-8"))
    return "".join(out)


def _decode_limit(data: bytes, errors: str, name: str, limit: int) -> str:
    """ascii (limit 128)."""
    out: list[str] = []
    start = 0
    i = 0
    while i < len(data):
        if data[i] < limit:
            i += 1
            continue
        if i > start:
            out.append(data[start:i].decode("latin-1"))
        _bad_decode(errors, name, data, i, i + 1, "ordinal not in range(" + str(limit) + ")", out)
        i += 1
        start = i
    if start < len(data):
        out.append(data[start:].decode("latin-1"))
    return "".join(out)


def _encode_limit(s: str, errors: str, name: str, limit: int) -> bytes:
    """ascii (128) and latin-1 (256)."""
    out: list[bytes] = []
    start = 0
    i = 0
    n = len(s)
    while i < n:
        if ord(s[i]) < limit:
            i += 1
            continue
        if i > start:
            out.append(s[start:i].encode("latin-1"))
        j = i + 1
        while j < n and ord(s[j]) >= limit:
            j += 1
        _bad_encode(errors, name, s, i, j, "ordinal not in range(" + str(limit) + ")", out)
        i = j
        start = i
    if start < n:
        out.append(s[start:].encode("latin-1"))
    return b"".join(out)


def _has_surrogates(b: bytes) -> bool:
    """Whether the UTF-8 form b of a str holds surrogates (ED A0-BF ..)."""
    i = b.find(b"\xed")
    while i >= 0:
        if i + 1 < len(b) and b[i + 1] >= 0xA0:
            return True
        i = b.find(b"\xed", i + 1)
    return False


def _encode_utf8(s: str, errors: str) -> bytes:
    """s in UTF-8; its surrogates (not allowed there) as errors says."""
    b = s.encode("utf-8")
    if not _has_surrogates(b):
        return b
    out: list[bytes] = []
    start = 0
    i = 0
    n = len(s)
    while i < n:
        c = ord(s[i])
        if c < 0xD800 or c > 0xDFFF:
            i += 1
            continue
        if i > start:
            out.append(s[start:i].encode("utf-8"))
        j = i + 1
        while j < n and 0xD800 <= ord(s[j]) <= 0xDFFF:
            j += 1
        _bad_encode(errors, "utf-8", s, i, j, "surrogates not allowed", out)
        i = j
        start = i
    if start < n:
        out.append(s[start:].encode("utf-8"))
    return b"".join(out)


# ---------------------------------------------------------------- the entry points

def decode(data: bytes, encoding: str = "utf-8", errors: str = "strict") -> str:
    """bytes.decode(encoding, errors)."""
    if encoding == "utf-8" and errors == "strict":
        try:
            return data.decode("utf-8")                 # (the common case first)
        except UnicodeDecodeError:
            return _decode_utf8(data, errors, "utf-8")
    if not data:
        return ""                                 # (as CPython: no lookup for nothing)
    name = _native(encoding)
    if not name:
        more = _more_decode
        if more is None:
            raise LookupError("unknown encoding: " + encoding)
        return more(data, encoding, errors)
    if name == "utf-8" or name == "utf-8-sig":
        if name == "utf-8-sig" and data.startswith(b"\xef\xbb\xbf"):
            data = data[3:]
        if errors == "strict":
            try:
                return data.decode("utf-8")
            except UnicodeDecodeError:
                return _decode_utf8(data, errors, "utf-8")
        _check_errors(errors)
        return _decode_utf8(data, errors, "utf-8")
    if name == "ascii":
        if errors == "strict":
            try:
                return data.decode("ascii")
            except UnicodeDecodeError:
                return _decode_limit(data, errors, "ascii", 128)
        _check_errors(errors)
        return _decode_limit(data, errors, "ascii", 128)
    return data.decode("latin-1")


def encode(s: str, encoding: str = "utf-8", errors: str = "strict") -> bytes:
    """str.encode(encoding, errors)."""
    if encoding == "utf-8" and errors == "strict":
        return _encode_utf8(s, errors)                   # (the common case first)
    name = _native(encoding)
    if not name:
        more = _more_encode
        if more is None:
            raise LookupError("unknown encoding: " + encoding)
        return more(s, encoding, errors)
    if name == "utf-8":
        return _encode_utf8(s, errors)
    if name == "utf-8-sig":
        return b"\xef\xbb\xbf" + _encode_utf8(s, errors)
    limit = 128
    if name == "latin-1":
        limit = 256
    if errors == "strict":
        try:
            return s.encode(name)
        except UnicodeEncodeError:
            return _encode_limit(s, errors, name, limit)
    _check_errors(errors)
    return _encode_limit(s, errors, name, limit)


if not sys._compiled:
    encode.__module__ = "_codecs"                       # (as CPython's: pickles of bytes name _codecs.encode)
    decode.__module__ = "_codecs"
