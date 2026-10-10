"""Reading and writing gzip files (CPython's gzip): open(), GzipFile, compress(), decompress().

A GzipFile reads the whole member(s) when opened for reading and writes its data when closed."""
import io
import os
import sys
import struct
import time
import zlib
from typing import Iterator, TypeVar

__all__ = ["BadGzipFile", "GzipFile", "open", "compress", "decompress"]

_T = TypeVar("_T")
_S = TypeVar("_S")

FTEXT, FHCRC, FEXTRA, FNAME, FCOMMENT = 1, 2, 4, 8, 16
READ, WRITE = "rb", "wb"
_COMPRESS_LEVEL_FAST = 1
_COMPRESS_LEVEL_TRADEOFF = 6
_COMPRESS_LEVEL_BEST = 9
READ_BUFFER_SIZE = 128 * 1024


class BadGzipFile(OSError):
    """Exception raised in some cases for invalid gzip files."""


def _header(compresslevel: int, mtime: int, fname: bytes) -> bytes:
    flags = FNAME if fname else 0
    xfl = 2 if compresslevel == _COMPRESS_LEVEL_BEST else 4 if compresslevel == _COMPRESS_LEVEL_FAST else 0
    out = b"\037\213\010" + bytes([flags]) + struct.pack("<L", mtime & 0xFFFFFFFF) + bytes([xfl, 255])
    if fname:
        out += fname + b"\000"
    return out


def _read_member(data: bytes, pos: int) -> tuple[bytes, int]:
    """(the data of the gzip member at pos, where the next one starts)."""
    if data[pos:pos + 2] != b"\037\213":
        raise BadGzipFile("Not a gzipped file (" + repr(data[pos:pos + 2]) + ")")
    if len(data) < pos + 10:
        raise EOFError("Compressed file ended before the end-of-stream marker was reached")
    if data[pos + 2] != 8:
        raise BadGzipFile("Unknown compression method")
    flag = data[pos + 3]
    p = pos + 10
    if flag & FEXTRA:
        xlen = data[p] | (data[p + 1] << 8)
        p += 2 + xlen
    if flag & FNAME:
        z = data.find(b"\000", p)
        p = z + 1
    if flag & FCOMMENT:
        z = data.find(b"\000", p)
        p = z + 1
    if flag & FHCRC:
        p += 2
    d = zlib.decompressobj(-zlib.MAX_WBITS)
    out = d.decompress(data[p:])
    if not d.eof:
        raise EOFError("Compressed file ended before the end-of-stream marker was reached")
    rest = d.unused_data
    if len(rest) < 8:
        raise EOFError("Compressed file ended before the end-of-stream marker was reached")
    crc = rest[0] | (rest[1] << 8) | (rest[2] << 16) | (rest[3] << 24)
    isize = rest[4] | (rest[5] << 8) | (rest[6] << 16) | (rest[7] << 24)
    if crc != zlib.crc32(out):
        raise BadGzipFile("CRC check failed " + hex(crc) + " != " + hex(zlib.crc32(out)))
    if isize != (len(out) & 0xFFFFFFFF):
        raise BadGzipFile("Incorrect length of data produced")
    end = len(data) - len(rest) + 8
    while end < len(data) and data[end] == 0:                 # (zero padding between members)
        end += 1
    return out, end


def compress(data: bytes, compresslevel: int = _COMPRESS_LEVEL_BEST, *, mtime: int | None = 0) -> bytes:
    """data compressed into a gzip member (mtime: the time in the header; None: now)."""
    t = int(time.time()) if mtime is None else mtime
    body = zlib.compress(data, compresslevel, -zlib.MAX_WBITS)
    return _header(compresslevel, t, b"") + body + struct.pack("<LL", zlib.crc32(data), len(data) & 0xFFFFFFFF)


def decompress(data: bytes) -> bytes:
    """The data of all the gzip members in data."""
    out: list[bytes] = []
    pos = 0
    while pos < len(data):
        chunk, pos = _read_member(data, pos)
        out.append(chunk)
    return b"".join(out)


class GzipFile:
    """A gzip file: open for reading ('rb') or writing ('wb', 'ab', 'xb')."""

    def __init__(self, filename: str | None = None, mode: str | None = None, compresslevel: int = _COMPRESS_LEVEL_BEST,
                 fileobj: _S = None, mtime: int | None = None) -> None:
        m = mode if mode is not None else "rb"
        if "t" in m:
            raise ValueError("Invalid mode: " + repr(m))
        if "b" not in m:
            m += "b"
        self.mode = READ if m.startswith("r") else WRITE
        self.name = filename if filename is not None else ""
        self._level = compresslevel
        self._mtime = mtime
        self._pos = 0
        self._data = b""
        self._out: list[bytes] = []
        self._closed = False
        self._fname = filename if filename is not None else ""
        self._file_mode = m
        self._raw = b""
        self._loaded = True                 # (reading: the data is decompressed when first needed)
        if fileobj is None:
            if filename is None:
                raise TypeError("filename or fileobj needed")
            if self.mode == READ:
                with io.open(filename, "rb") as f:
                    self._raw = f.read()
                self._loaded = False
        else:
            if self.mode == READ:
                self._raw = fileobj.read()
                self._loaded = False
        self._fileobj_given = fileobj is not None
        self._writer = self._write_file
        if fileobj is not None:
            if self.mode == WRITE:
                def write_out(b: bytes) -> None:
                    fileobj.write(b)
                self._writer = write_out

    def _write_file(self, b: bytes) -> None:
        m = self._file_mode
        if m.startswith("a"):
            with io.open(self._fname, "ab") as f:
                f.write(b)
        elif m.startswith("x"):
            with io.open(self._fname, "xb") as f:
                f.write(b)
        else:
            with io.open(self._fname, "wb") as f:
                f.write(b)

    @property
    def closed(self) -> bool:
        return self._closed

    def _check_read(self) -> None:
        if self._closed:
            raise ValueError("I/O operation on closed file.")
        if self.mode != READ:
            raise OSError("read() on write-only GzipFile object")
        if not self._loaded:
            self._loaded = True
            raw = self._raw
            self._raw = b""
            self._data = decompress(raw)

    def read(self, size: int = -1) -> bytes:
        self._check_read()
        if size < 0:
            out = self._data[self._pos:]
            self._pos = len(self._data)
            return out
        out = self._data[self._pos:self._pos + size]
        self._pos += len(out)
        return out

    def read1(self, size: int = -1) -> bytes:
        return self.read(size)

    def peek(self, n: int) -> bytes:
        self._check_read()
        return self._data[self._pos:self._pos + max(n, 1)]

    def readline(self, size: int = -1) -> bytes:
        self._check_read()
        nl = self._data.find(b"\n", self._pos)
        end = len(self._data) if nl < 0 else nl + 1
        if size >= 0:
            end = min(end, self._pos + size)
        out = self._data[self._pos:end]
        self._pos = end
        return out

    def readlines(self) -> list[bytes]:
        out: list[bytes] = []
        while True:
            line = self.readline()
            if not line:
                return out
            out.append(line)

    def __iter__(self) -> Iterator[bytes]:
        while True:
            line = self.readline()
            if not line:
                return
            yield line

    def write(self, data: bytes) -> int:
        if self._closed:
            raise ValueError("write() on closed GzipFile object")
        if self.mode != WRITE:
            raise OSError("write() on read-only GzipFile object")
        self._out.append(bytes(data))
        return len(data)

    def seekable(self) -> bool:
        return self.mode == READ

    def readable(self) -> bool:
        return self.mode == READ

    def writable(self) -> bool:
        return self.mode == WRITE

    def tell(self) -> int:
        if self.mode == READ:
            self._check_read()
            return self._pos
        return sum(len(b) for b in self._out)

    def seek(self, offset: int, whence: int = 0) -> int:
        if self.mode != READ:
            raise OSError("Negative seek in write mode not supported")
        self._check_read()
        if whence == 1:
            offset += self._pos
        elif whence == 2:
            raise ValueError("Seek from end not supported")
        self._pos = max(0, min(offset, len(self._data)))
        return self._pos

    def flush(self) -> None:
        pass

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        if self.mode == WRITE:
            data = b"".join(self._out)
            t = int(time.time()) if self._mtime is None else self._mtime
            fname = b""
            if self._fname:
                base = os.path.basename(self._fname)
                if base.endswith(".gz"):
                    base = base[:-3]
                fname = base.encode("latin-1", "replace")
            body = zlib.compress(data, self._level, -zlib.MAX_WBITS)
            self._writer(_header(self._level, t, fname) + body + struct.pack("<LL", zlib.crc32(data), len(data) & 0xFFFFFFFF))

    def __enter__(self) -> "GzipFile":
        return self

    def __exit__(self, t, v, tb) -> None:
        self.close()

    def __repr__(self) -> str:
        return "<gzip " + repr(self.name) + " " + hex(id(self)) + ">"


class _TextGzip:
    """gzip.open(..., 'rt' / 'wt'): text through a GzipFile."""

    def __init__(self, g: GzipFile, encoding: str | None, errors: str | None, newline: str | None) -> None:
        self._g = g
        self._encoding = encoding if encoding is not None else "utf-8"
        self._errors = errors if errors is not None else "strict"
        self._text = ""
        self._pos = 0
        if g.mode == READ:
            self._text = g.read().decode(self._encoding, self._errors)
            if newline is None:
                self._text = self._text.replace("\r\n", "\n").replace("\r", "\n")

    def read(self, size: int = -1) -> str:
        if size < 0:
            out = self._text[self._pos:]
        else:
            out = self._text[self._pos:self._pos + size]
        self._pos += len(out)
        return out

    def readline(self) -> str:
        nl = self._text.find("\n", self._pos)
        end = len(self._text) if nl < 0 else nl + 1
        out = self._text[self._pos:end]
        self._pos = end
        return out

    def readlines(self) -> list[str]:
        out: list[str] = []
        while True:
            line = self.readline()
            if not line:
                return out
            out.append(line)

    def __iter__(self) -> Iterator[str]:
        while True:
            line = self.readline()
            if not line:
                return
            yield line

    def write(self, s: str) -> int:
        self._g.write(s.encode(self._encoding, self._errors))
        return len(s)

    def close(self) -> None:
        self._g.close()

    def __enter__(self) -> "_TextGzip":
        return self

    def __exit__(self, t, v, tb) -> None:
        self.close()


if not sys._compiled:
    def open(filename, mode="rb", compresslevel=_COMPRESS_LEVEL_BEST, encoding=None, errors=None, newline=None):
        """A gzip file: binary ("rb", "wb", "ab", "xb") or text ("rt", "wt" ...)."""
        if "t" in mode:
            return _open_t(filename, mode, compresslevel, encoding, errors, newline)
        return _open_b(filename, mode, compresslevel, encoding, errors, newline)


def _open_b(filename: str, mode: str = "rb", compresslevel: int = _COMPRESS_LEVEL_BEST, encoding: str | None = None,
            errors: str | None = None, newline: str | None = None) -> GzipFile:
    if encoding is not None or errors is not None or newline is not None:
        raise ValueError("Argument 'encoding' not supported in binary mode")
    return GzipFile(filename, mode, compresslevel)


def _open_t(filename: str, mode: str = "rt", compresslevel: int = _COMPRESS_LEVEL_BEST, encoding: str | None = None,
            errors: str | None = None, newline: str | None = None) -> _TextGzip:
    return _TextGzip(GzipFile(filename, mode.replace("t", "") + "b" if "b" not in mode else mode, compresslevel),
                     encoding, errors, newline)
