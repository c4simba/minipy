"""Base16, Base32, Base64 (RFC 4648), Base85 and Ascii85 data encodings (CPython's base64)."""
import sys
import binascii
from typing import TypeVar

S = TypeVar("S")

__all__ = ["encode", "decode", "encodebytes", "decodebytes", "b64encode", "b64decode", "b32encode", "b32decode",
           "b32hexencode", "b32hexdecode", "b16encode", "b16decode", "b85encode", "b85decode", "a85encode", "a85decode",
           "z85encode", "z85decode", "standard_b64encode", "standard_b64decode", "urlsafe_b64encode", "urlsafe_b64decode"]


def _bytes_from_decode_data(s: S) -> bytes:
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
        raise TypeError("argument should be a bytes-like object or ASCII string, not '" + type(s).__name__ + "'")


def _swap(s: bytes, frm: bytes, to: bytes) -> bytes:
    """s with each byte of frm replaced by the one of to at its place (bytes.translate)."""
    out: list[int] = []
    for c in s:
        k = frm.find(bytes([c]))
        out.append(to[k] if k >= 0 else c)
    return bytes(out)


# ---------------------------------------------------------------- base64

def b64encode(s: bytes, altchars=None) -> bytes:
    """s in base64 (altchars: the two bytes for + and /)."""
    encoded = binascii.b2a_base64(s, newline=False)
    if altchars is None:
        return encoded
    else:
        alt = _bytes_from_decode_data(altchars)
        assert len(alt) == 2, repr(altchars)
        return _swap(encoded, b"+/", alt)


def b64decode(s: S, altchars=None, validate: bool = False) -> bytes:
    """The bytes of base64 s. Characters outside the alphabet are skipped unless validate."""
    data = _bytes_from_decode_data(s)
    if altchars is None:
        return binascii.a2b_base64(data, strict_mode=validate)
    else:
        alt = _bytes_from_decode_data(altchars)
        assert len(alt) == 2, repr(altchars)
        return binascii.a2b_base64(_swap(data, alt, b"+/"), strict_mode=validate)


def standard_b64encode(s: bytes) -> bytes:
    return b64encode(s)


def standard_b64decode(s: S) -> bytes:
    return b64decode(s)


def urlsafe_b64encode(s: bytes) -> bytes:
    """s in base64 with - and _ for + and /."""
    return _swap(b64encode(s), b"+/", b"-_")


def urlsafe_b64decode(s: S) -> bytes:
    return b64decode(_swap(_bytes_from_decode_data(s), b"-_", b"+/"))


MAXLINESIZE = 76
MAXBINSIZE = (MAXLINESIZE // 4) * 3


def encodebytes(s: bytes) -> bytes:
    """s in base64 lines of at most 76 characters, each ending with a newline."""
    pieces: list[bytes] = []
    for i in range(0, len(s), MAXBINSIZE):
        pieces.append(binascii.b2a_base64(s[i:i + MAXBINSIZE]))
    return b"".join(pieces)


def decodebytes(s: bytes) -> bytes:
    return binascii.a2b_base64(s)


def encode(input, output) -> None:
    """Base64 lines of the binary file input, written to the binary file output."""
    while True:
        s = input.read(MAXBINSIZE)
        if not s:
            break
        while len(s) < MAXBINSIZE:
            ns = input.read(MAXBINSIZE - len(s))
            if not ns:
                break
            s += ns
        output.write(binascii.b2a_base64(s))


def decode(input, output) -> None:
    """The bytes of the base64 lines of the binary file input, written to output."""
    while True:
        line = input.readline()
        if not line:
            break
        output.write(binascii.a2b_base64(line))


# ---------------------------------------------------------------- base32

_B32 = b"ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"
_B32HEX = b"0123456789ABCDEFGHIJKLMNOPQRSTUV"


def _b32encode(alphabet: bytes, s: bytes) -> bytes:
    out: list[int] = []
    n = len(s)
    for i in range(0, n, 5):
        c = 0
        for k in range(5):
            c = (c << 8) | (s[i + k] if i + k < n else 0)
        for k in range(8):
            out.append(alphabet[(c >> (35 - 5 * k)) & 31])
    left = n % 5
    if left:
        pad = [0, 6, 4, 3, 1][left]
        for k in range(pad):
            out[len(out) - 1 - k] = 61
    return bytes(out)


def _b32decode(alphabet: bytes, s: bytes, casefold: bool, map01: bytes | None) -> bytes:
    if len(s) % 8:
        raise binascii.Error("Incorrect padding")
    if map01 is not None:
        assert len(map01) == 1, repr(map01)
        s = _swap(s, b"01", b"O" + map01)
    if casefold:
        s = s.upper()
    n = len(s)
    s = s.rstrip(b"=")
    padchars = n - len(s)
    out: list[int] = []
    acc = 0
    for i in range(0, len(s), 8):
        acc = 0
        for c in s[i:i + 8]:
            v = alphabet.find(bytes([c]))
            if v < 0:
                raise binascii.Error("Non-base32 digit found")
            acc = (acc << 5) + v
        for k in range(5):
            out.append((acc >> (32 - 8 * k)) & 255)
    if n % 8 or padchars not in (0, 1, 3, 4, 6):
        raise binascii.Error("Incorrect padding")
    if padchars and out:
        acc <<= 5 * padchars
        leftover = (43 - 5 * padchars) // 8
        del out[len(out) - 5:]
        for k in range(leftover):
            out.append((acc >> (32 - 8 * k)) & 255)
    return bytes(out)


def b32encode(s: bytes) -> bytes:
    """s in base32."""
    return _b32encode(_B32, s)


def b32decode(s: S, casefold: bool = False, map01=None) -> bytes:
    """The bytes of base32 s (casefold: lowercase too; map01: what 0 and 1 stand for besides O)."""
    if map01 is None:
        return _b32decode(_B32, _bytes_from_decode_data(s), casefold, None)
    else:
        return _b32decode(_B32, _bytes_from_decode_data(s), casefold, _bytes_from_decode_data(map01))


def b32hexencode(s: bytes) -> bytes:
    """s in base32 with the extended hex alphabet."""
    return _b32encode(_B32HEX, s)


def b32hexdecode(s: S, casefold: bool = False) -> bytes:
    return _b32decode(_B32HEX, _bytes_from_decode_data(s), casefold, None)


# ---------------------------------------------------------------- base16

def b16encode(s: bytes) -> bytes:
    """s in base16 (uppercase hex)."""
    return binascii.hexlify(s).upper()


def b16decode(s: S, casefold: bool = False) -> bytes:
    """The bytes of base16 s (casefold: lowercase digits too)."""
    data = _bytes_from_decode_data(s)
    if casefold:
        data = data.upper()
    for c in data:
        if not ((c >= 48 and c <= 57) or (c >= 65 and c <= 70)):
            raise binascii.Error("Non-base16 digit found")
    return binascii.unhexlify(data)


# ---------------------------------------------------------------- base85, Ascii85, Z85

_B85 = b"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+-;<=>?@^_`{|}~"
_Z85 = b"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-:+=^!/*?&<>()[]{}@%$#"
_A85START = b"<~"
_A85END = b"~>"


def _85encode(b: bytes, chars: bytes, pad: bool, foldnuls: bool, foldspaces: bool) -> bytes:
    padding = (-len(b)) % 4
    if padding:
        b = b + b"\0" * padding
    out: list[int] = []
    last = 0
    for i in range(0, len(b), 4):
        word = (b[i] << 24) | (b[i + 1] << 16) | (b[i + 2] << 8) | b[i + 3]
        last = len(out)
        if foldnuls and not word:
            out.append(122)
        elif foldspaces and word == 0x20202020:
            out.append(121)
        else:
            out.append(chars[word // 52200625])
            out.append(chars[word // 614125 % 85])
            out.append(chars[word // 7225 % 85])
            out.append(chars[word // 85 % 85])
            out.append(chars[word % 85])
    if padding and not pad:
        if out and out[len(out) - 1] == 122 and len(out) - last == 1:
            del out[last:]
            for k in range(5):
                out.append(chars[0])
        del out[len(out) - padding:]
    return bytes(out)


def b85encode(b: bytes, pad: bool = False) -> bytes:
    """b in base85 (as git uses it); pad: to a multiple of 4 bytes first."""
    return _85encode(b, _B85, pad, False, False)


def _85decode(b: bytes, alphabet: bytes, what: str) -> bytes:
    dec = [-1] * 256
    for i in range(len(alphabet)):
        dec[alphabet[i]] = i
    padding = (-len(b)) % 5
    b = b + alphabet[84:85] * padding
    out: list[int] = []
    for i in range(0, len(b), 5):
        acc = 0
        for j in range(5):
            v = dec[b[i + j]]
            if v < 0:
                raise ValueError("bad " + what + " character at position " + str(i + j))
            acc = acc * 85 + v
        if acc > 0xFFFFFFFF:
            raise ValueError(what + " overflow in hunk starting at byte " + str(i))
        out.append(acc >> 24)
        out.append((acc >> 16) & 255)
        out.append((acc >> 8) & 255)
        out.append(acc & 255)
    if padding:
        del out[len(out) - padding:]
    return bytes(out)


def b85decode(b: S) -> bytes:
    """The bytes of base85 b."""
    return _85decode(_bytes_from_decode_data(b), _B85, "base85")


def z85encode(s: bytes, pad: bool = False) -> bytes:
    """s in Z85 (ZeroMQ's base85)."""
    return _85encode(s, _Z85, pad, False, False)


def z85decode(s: S) -> bytes:
    """The bytes of Z85 s."""
    return _85decode(_bytes_from_decode_data(s), _Z85, "z85")


def a85encode(b: bytes, *, foldspaces: bool = False, wrapcol: int = 0, pad: bool = False, adobe: bool = False) -> bytes:
    """b in Ascii85 (z for four zero bytes; foldspaces: y for four spaces; wrapcol: lines at most
    that long; adobe: between <~ and ~>)."""
    result = _85encode(b, b"!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstu",
                       pad, True, foldspaces)
    if adobe:
        result = _A85START + result
    if wrapcol:
        wrapcol = max(2 if adobe else 1, wrapcol)
        chunks: list[bytes] = []
        for i in range(0, len(result), wrapcol):
            chunks.append(result[i:i + wrapcol])
        if adobe:
            if len(chunks[len(chunks) - 1]) + 2 > wrapcol:
                chunks.append(b"")
        result = b"\n".join(chunks)
    if adobe:
        result += _A85END
    return result


def a85decode(b: S, *, foldspaces: bool = False, adobe: bool = False, ignorechars: bytes = b" \t\n\r\v") -> bytes:
    """The bytes of Ascii85 b (adobe: framed by <~ and ~>)."""
    data = _bytes_from_decode_data(b)
    if adobe:
        if not data.endswith(_A85END):
            raise ValueError("Ascii85 encoded byte sequences must end with " + repr(_A85END))
        if data.startswith(_A85START):
            data = data[2:len(data) - 2]
        else:
            data = data[:len(data) - 2]
    out: list[int] = []
    curr: list[int] = []
    for x in data + b"uuuu":
        if x >= 33 and x <= 117:
            curr.append(x)
            if len(curr) == 5:
                acc = 0
                for y in curr:
                    acc = 85 * acc + (y - 33)
                if acc > 0xFFFFFFFF:
                    raise ValueError("Ascii85 overflow")
                out.append(acc >> 24)
                out.append((acc >> 16) & 255)
                out.append((acc >> 8) & 255)
                out.append(acc & 255)
                curr = []
        elif x == 122:
            if curr:
                raise ValueError("z inside Ascii85 5-tuple")
            for k in range(4):
                out.append(0)
        elif foldspaces and x == 121:
            if curr:
                raise ValueError("y inside Ascii85 5-tuple")
            for k in range(4):
                out.append(32)
        elif x in ignorechars:
            continue
        else:
            raise ValueError("Non-Ascii85 digit found: " + chr(x))
    padding = 4 - len(curr)
    if padding:
        del out[len(out) - padding:]
    return bytes(out)
