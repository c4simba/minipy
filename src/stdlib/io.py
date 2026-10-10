"""File objects: what open() returns (CPython's io, the parts minipy has).

Files are descriptors of _os (KolibriOS: file positions kept by _os over its file
system functions), so this module is the same on every platform. Reading is
buffered; writing goes straight to the file (compiled programs have no
finalizers that could flush a buffer a program forgets to close).

Text files are UTF-8 unless an encoding is given, with universal newlines on
reading (newline=None: \\r\\n and \\r read as \\n). The text layer works on
the bytes themselves, so tell() and seek() are byte positions, as CPython's
cookies are for these encodings. StringIO and BytesIO are files in memory."""
import sys
import _os

DEFAULT_BUFFER_SIZE = 8192


def text_encoding(encoding: str | None, stacklevel: int = 2) -> str:
    """The encoding open() uses for text when none is given: "locale" (UTF-8 here)."""
    if encoding is None:
        return "locale"
    return encoding

SEEK_SET = 0
SEEK_CUR = 1
SEEK_END = 2


class UnsupportedOperation(OSError):
    pass


def _check_mode(mode: str) -> tuple[str, bool, bool]:
    """mode -> (kind 'r'/'w'/'a'/'x', +, binary), with CPython's ValueErrors."""
    kind = ""
    plus = False
    binary = False
    text = False
    for ch in mode:
        if ch in "rwax":
            if kind:
                raise ValueError("must have exactly one of create/read/write/append mode")
            kind = ch
        elif ch == "+":
            if plus:
                raise ValueError("invalid mode: '" + mode + "'")
            plus = True
        elif ch == "b":
            if binary:
                raise ValueError("invalid mode: '" + mode + "'")
            binary = True
        elif ch == "t":
            if text:
                raise ValueError("invalid mode: '" + mode + "'")
            text = True
        else:
            raise ValueError("invalid mode: '" + mode + "'")
    if binary and text:
        raise ValueError("can't have text and binary mode at once")
    if not kind:
        raise ValueError("Must have exactly one of create/read/write/append mode and at most one plus")
    return (kind, plus, binary)


def _flags(kind: str, plus: bool) -> int:
    if kind == "r":
        flags = 0
    elif kind == "w":
        flags = _os.O_CREAT | _os.O_TRUNC
    elif kind == "a":
        flags = _os.O_CREAT | _os.O_APPEND
    else:
        flags = _os.O_CREAT | _os.O_EXCL
    if plus:
        return flags | _os.O_RDWR
    if kind == "r":
        return flags | _os.O_RDONLY
    return flags | _os.O_WRONLY


def _open_fd(name: str, kind: str, plus: bool) -> int:
    fd = _os.open(name, _flags(kind, plus), 0o666)
    try:
        st = _os.fstat(fd)
    except OSError:
        return fd
    if _os.S_ISDIR(st.st_mode):
        _os.close(fd)
        raise IsADirectoryError(21, _os.strerror(21), name)
    return fd


class _File:
    """The bytes of a file: a descriptor, a read buffer and the position."""

    def __init__(self, name: str, mode: str, kind: str, plus: bool, fd: int):
        self.name = name
        self.mode = mode
        self._fd = fd
        self._readable = kind == "r" or plus
        self._writable = kind != "r" or plus
        self._closed = False
        self._buf = b""
        self._pos = 0
        self._eof = False
        self._delete = False                # (tempfile: the file goes when closed)

    @property
    def closed(self) -> bool:
        return self._closed

    def _check_open(self) -> None:
        if self._closed:
            raise ValueError("I/O operation on closed file.")

    def _check_readable(self) -> None:
        self._check_open()
        if not self._readable:
            raise UnsupportedOperation("not readable")

    def _check_writable(self) -> None:
        self._check_open()
        if not self._writable:
            raise UnsupportedOperation("not writable")

    def _fill(self) -> bool:
        """More bytes into the read buffer: False at the end of the file."""
        if self._eof:
            return False
        data = _os.read(self._fd, DEFAULT_BUFFER_SIZE)
        if not data:
            self._eof = True
            return False
        if self._pos:
            self._buf = self._buf[self._pos:] + data
            self._pos = 0
        else:
            self._buf = self._buf + data
        return True

    def _drop_buffer(self) -> None:
        """Before writing or seeking: the descriptor back where the reader is."""
        left = len(self._buf) - self._pos
        if left:
            _os.lseek(self._fd, -left, SEEK_CUR)
        self._buf = b""
        self._pos = 0
        self._eof = False

    def _read_bytes(self, n: int) -> bytes:
        self._check_readable()
        if n < 0:
            while self._fill():
                pass
            data = self._buf[self._pos:]
            self._buf = b""
            self._pos = 0
            return data
        while len(self._buf) - self._pos < n and self._fill():
            pass
        data = self._buf[self._pos:self._pos + n]
        self._pos += len(data)
        return data

    def _write_bytes(self, data: bytes) -> int:
        self._check_writable()
        if self._buf:
            self._drop_buffer()
        done = 0
        while done < len(data):
            done += _os.write(self._fd, data[done:])
        return done

    def _line_end(self, start: int, newline: str | None) -> int:
        """Where the line from start ends in the buffer (past its newline), reading more as
        needed; the end of the buffer when the file ends first."""
        i = start
        while True:
            n = len(self._buf)
            while i < n:
                b = self._buf[i]
                if b == 10 and newline != "\r":
                    return i + 1
                if b == 13 and newline != "\n":
                    if newline == "\r":
                        return i + 1
                    if i + 1 < n:
                        if self._buf[i + 1] == 10:
                            return i + 2
                        if newline is None or newline == "":
                            return i + 1
                    else:
                        rel = i - self._pos
                        if not self._fill():
                            return len(self._buf)
                        i = self._pos + rel
                        continue
                i += 1
            rel = i - self._pos
            if not self._fill():
                return len(self._buf)
            i = self._pos + rel

    def tell(self) -> int:
        self._check_open()
        return _os.lseek(self._fd, 0, SEEK_CUR) - (len(self._buf) - self._pos)

    def seek(self, offset: int, whence: int = 0) -> int:
        self._check_open()
        if whence == SEEK_CUR:
            offset += self.tell()
            whence = SEEK_SET
        if whence != SEEK_SET and whence != SEEK_END:
            raise ValueError("invalid whence (" + str(whence) + ", should be 0, 1 or 2)")
        if whence == SEEK_SET and offset < 0:
            raise ValueError("negative seek position " + str(offset))
        self._buf = b""
        self._pos = 0
        self._eof = False
        return _os.lseek(self._fd, offset, whence)

    def truncate(self, size: int | None = None) -> int:
        self._check_writable()
        if size is None:
            size = self.tell()
        self._drop_buffer()
        _os.ftruncate(self._fd, size)
        return size

    def flush(self) -> None:
        self._check_open()

    def close(self) -> None:
        if not self._closed:
            self._closed = True
            _os.close(self._fd)
            if self._delete:
                try:
                    _os.unlink(self.name)
                except OSError:
                    pass

    def fileno(self) -> int:
        self._check_open()
        return self._fd

    def isatty(self) -> bool:
        self._check_open()
        return _os.isatty(self._fd)

    def readable(self) -> bool:
        self._check_open()
        return self._readable

    def writable(self) -> bool:
        self._check_open()
        return self._writable

    def seekable(self) -> bool:
        self._check_open()
        return True


class BufferedReader(_File):
    """A file opened in binary mode: bytes in, bytes out."""

    def read(self, size: int = -1) -> bytes:
        return self._read_bytes(size)

    def read1(self, size: int = -1) -> bytes:
        return self._read_bytes(size)

    if not sys._compiled:
        def readinto(self, b):
            """Reads into the bytearray b; the number of bytes read."""
            data = self._read_bytes(len(b))
            n = len(data)
            b[:n] = data
            return n

    def readline(self, size: int = -1) -> bytes:
        self._check_readable()
        end = self._line_end(self._pos, "\n")
        if size >= 0 and end - self._pos > size:
            end = self._pos + size
        data = self._buf[self._pos:end]
        self._pos = end
        return data

    def readlines(self, hint: int = -1) -> list[bytes]:
        lines: list[bytes] = []
        total = 0
        while True:
            line = self.readline()
            if not line:
                break
            lines.append(line)
            total += len(line)
            if hint > 0 and total >= hint:
                break
        return lines

    def write(self, data: bytes) -> int:
        return self._write_bytes(data)

    def writelines(self, lines: list[bytes]) -> None:
        for line in lines:
            self._write_bytes(line)

    def __iter__(self) -> "BufferedReader":
        self._check_open()
        return self

    def __next__(self) -> bytes:
        line = self.readline()
        if not line:
            raise StopIteration
        return line

    def __enter__(self) -> "BufferedReader":
        self._check_open()
        return self

    def __exit__(self, et, ev, tb) -> bool:
        self.close()
        return False

    def __repr__(self) -> str:
        kind = "BufferedReader"
        if self._readable and self._writable:
            kind = "BufferedRandom"
        elif self._writable:
            kind = "BufferedWriter"
        return "<_io." + kind + " name=" + repr(self.name) + ">"


class BufferedWriter(BufferedReader):
    pass


class BufferedRandom(BufferedReader):
    pass


def _norm_encoding(encoding: str) -> str:
    e = ""
    for ch in encoding:                           # (ASCII lower case: no Unicode tables needed)
        if ch >= "A" and ch <= "Z":
            ch = chr(ord(ch) + 32)
        elif ch == "_":
            ch = "-"
        e += ch
    if e == "utf8" or e == "u8" or e == "utf" or e == "locale":      # (the locale's: UTF-8)
        return "utf-8"
    if e == "latin1" or e == "iso-8859-1" or e == "iso8859-1" or e == "l1" or e == "8859" or e == "cp819" or e == "latin":
        return "latin-1"
    if e == "us-ascii" or e == "646":
        return "ascii"
    return e


class TextIOWrapper(_File):
    """A file opened in text mode: str in, str out."""

    def _setup(self, encoding: str | None, errors: str | None, newline: str | None) -> None:
        if newline is not None and newline != "" and newline != "\n" and newline != "\r" and newline != "\r\n":
            raise ValueError("illegal newline value: " + newline)
        self.encoding = "UTF-8"
        if encoding is not None:
            self.encoding = encoding
        self.errors = "strict"
        if errors is not None:
            self.errors = errors
        self._codec = _norm_encoding(self.encoding)
        "x".encode(self._codec)                         # (LookupError: unknown encoding)
        self._newline = newline
        self._utf8 = self._codec == "utf-8" or self._codec == "utf-8-sig"
        self._sig = self._codec == "utf-8-sig"
        if self._sig:
            self._codec = "utf-8"

    def _decode(self, data: bytes) -> str:
        text = data.decode(self._codec, self.errors)
        if self._newline is None and "\r" in text:
            text = text.replace("\r\n", "\n").replace("\r", "\n")
        return text

    def _skip_sig(self) -> None:
        if self._sig and self.tell() == 0:
            while len(self._buf) - self._pos < 3 and self._fill():
                pass
            if self._buf[self._pos:self._pos + 3] == b"\xef\xbb\xbf":
                self._pos += 3

    def read(self, size: int = -1) -> str:
        self._check_readable()
        self._skip_sig()
        if size is None or size < 0:
            return self._decode(self._read_bytes(-1))
        start = self._pos
        i = start
        count = 0
        while count < size:
            if i >= len(self._buf):
                rel = i - self._pos
                start_rel = start - self._pos
                if not self._fill():
                    break
                i = self._pos + rel
                start = self._pos + start_rel
                continue
            b = self._buf[i]
            if self._utf8:
                if b < 0x80:
                    step = 1
                elif b >= 0xF0:
                    step = 4
                elif b >= 0xE0:
                    step = 3
                elif b >= 0xC0:
                    step = 2
                else:
                    step = 1
            else:
                step = 1
            if b == 13 and self._newline is None:      # \r\n: one character
                if i + 1 >= len(self._buf):
                    rel = i - self._pos
                    start_rel = start - self._pos
                    self._fill()
                    i = self._pos + rel
                    start = self._pos + start_rel
                if i + 1 < len(self._buf) and self._buf[i + 1] == 10:
                    step = 2
            while i + step > len(self._buf):
                rel = i - self._pos
                start_rel = start - self._pos
                if not self._fill():
                    break
                i = self._pos + rel
                start = self._pos + start_rel
            i += step
            count += 1
        if i > len(self._buf):
            i = len(self._buf)
        data = self._buf[start:i]
        self._pos = i
        return self._decode(data)

    def readline(self, size: int = -1) -> str:
        self._check_readable()
        self._skip_sig()
        end = self._line_end(self._pos, self._newline)
        line = self._decode(self._buf[self._pos:end])
        self._pos = end
        if size >= 0 and len(line) > size:
            rest = line[size:]
            line = line[:size]
            self._pos -= len(rest.encode(self._codec, self.errors))
        return line

    def readlines(self, hint: int = -1) -> list[str]:
        lines: list[str] = []
        total = 0
        while True:
            line = self.readline()
            if not line:
                break
            lines.append(line)
            total += len(line)
            if hint > 0 and total >= hint:
                break
        return lines

    def write(self, s: str) -> int:
        self._check_writable()
        text = s
        if self._newline is not None and self._newline != "" and self._newline != "\n" and "\n" in text:
            text = text.replace("\n", self._newline)
        data = text.encode(self._codec, self.errors)
        if self._sig and self.tell() == 0:
            data = b"\xef\xbb\xbf" + data
        self._write_bytes(data)
        return len(s)

    def writelines(self, lines: list[str]) -> None:
        for line in lines:
            self.write(line)

    def seek(self, offset: int, whence: int = 0) -> int:
        self._check_open()
        if whence == SEEK_CUR and offset != 0:
            raise UnsupportedOperation("can't do nonzero cur-relative seeks")
        if whence == SEEK_END and offset != 0:
            raise UnsupportedOperation("can't do nonzero end-relative seeks")
        return _File.seek(self, offset, whence)

    @property
    def newlines(self) -> str | None:
        return None

    @property
    def line_buffering(self) -> bool:
        return False

    def __iter__(self) -> "TextIOWrapper":
        self._check_open()
        return self

    def __next__(self) -> str:
        line = self.readline()
        if not line:
            raise StopIteration
        return line

    def __enter__(self) -> "TextIOWrapper":
        self._check_open()
        return self

    def __exit__(self, et, ev, tb) -> bool:
        self.close()
        return False

    def __repr__(self) -> str:
        return "<_io.TextIOWrapper name=" + repr(self.name) + " mode=" + repr(self.mode) + " encoding=" + repr(self.encoding) + ">"


def _open_text(file: str, mode: str = "r", buffering: int = -1, encoding: str | None = None, errors: str | None = None,
               newline: str | None = None, closefd: bool = True) -> TextIOWrapper:
    """open() in text mode."""
    kind, plus, binary = _check_mode(mode)
    if binary:
        raise ValueError("binary mode doesn't take an encoding argument")
    f = TextIOWrapper(file, mode, kind, plus, _open_fd(file, kind, plus))
    try:
        f._setup(encoding, errors, newline)
    except LookupError:
        f.close()
        raise
    return f


def _open_binary(file: str, mode: str = "rb", buffering: int = -1, encoding: str | None = None, errors: str | None = None,
                 newline: str | None = None, closefd: bool = True) -> BufferedReader:
    """open() in binary mode."""
    kind, plus, binary = _check_mode(mode)
    if encoding is not None:
        raise ValueError("binary mode doesn't take an encoding argument")
    if errors is not None:
        raise ValueError("binary mode doesn't take an errors argument")
    if newline is not None:
        raise ValueError("binary mode doesn't take a newline argument")
    fd = _open_fd(file, kind, plus)
    if plus:
        return BufferedRandom(file, mode, kind, plus, fd)
    if kind == "r":
        return BufferedReader(file, mode, kind, plus, fd)
    return BufferedWriter(file, mode, kind, plus, fd)


class StringIO:
    """A text file in memory."""

    def __init__(self, initial_value: str = "", newline: str | None = "\n"):
        self._text = initial_value
        self._pos = 0
        self._closed = False

    @property
    def closed(self) -> bool:
        return self._closed

    def _check_open(self) -> None:
        if self._closed:
            raise ValueError("I/O operation on closed file.")

    def getvalue(self) -> str:
        self._check_open()
        return self._text

    def read(self, size: int = -1) -> str:
        self._check_open()
        if size < 0:
            end = len(self._text)
        else:
            end = min(len(self._text), self._pos + size)
        s = self._text[self._pos:end]
        self._pos = max(self._pos, end)
        return s

    def readline(self, size: int = -1) -> str:
        self._check_open()
        i = self._text.find("\n", self._pos)
        end = len(self._text) if i < 0 else i + 1
        if size >= 0 and end - self._pos > size:
            end = self._pos + size
        s = self._text[self._pos:end]
        self._pos = max(self._pos, end)
        return s

    def readlines(self, hint: int = -1) -> list[str]:
        lines: list[str] = []
        while True:
            line = self.readline()
            if not line:
                return lines
            lines.append(line)

    def write(self, s: str) -> int:
        self._check_open()
        if self._pos > len(self._text):
            self._text += "\0" * (self._pos - len(self._text))
        self._text = self._text[:self._pos] + s + self._text[self._pos + len(s):]
        self._pos += len(s)
        return len(s)

    def writelines(self, lines: list[str]) -> None:
        for line in lines:
            self.write(line)

    def tell(self) -> int:
        self._check_open()
        return self._pos

    def seek(self, pos: int, whence: int = 0) -> int:
        self._check_open()
        if whence == SEEK_CUR:
            if pos != 0:
                raise OSError("Can't do nonzero cur-relative seeks")
            return self._pos
        if whence == SEEK_END:
            if pos != 0:
                raise OSError("Can't do nonzero end-relative seeks")
            self._pos = len(self._text)
            return self._pos
        if pos < 0:
            raise ValueError("Negative seek position " + str(pos))
        self._pos = pos
        return pos

    def truncate(self, size: int | None = None) -> int:
        self._check_open()
        if size is None:
            size = self._pos
        self._text = self._text[:size]
        return size

    def flush(self) -> None:
        self._check_open()

    def close(self) -> None:
        self._closed = True

    def readable(self) -> bool:
        return True

    def writable(self) -> bool:
        return True

    def seekable(self) -> bool:
        return True

    def __iter__(self) -> "StringIO":
        return self

    def __next__(self) -> str:
        line = self.readline()
        if not line:
            raise StopIteration
        return line

    def __enter__(self) -> "StringIO":
        return self

    def __exit__(self, et, ev, tb) -> bool:
        self.close()
        return False


class BytesIO:
    """A binary file in memory."""

    def __init__(self, initial_bytes: bytes = b""):
        if not sys._compiled:
            initial_bytes = bytes(initial_bytes)              # (a bytearray: copied)
        self._data = initial_bytes
        self._pos = 0
        self._closed = False

    if not sys._compiled:
        def readinto(self, b):
            """Reads into the bytearray b; the number of bytes read."""
            data = self.read(len(b))
            n = len(data)
            b[:n] = data
            return n

        def getbuffer(self):
            return bytearray(self.getvalue())

    @property
    def closed(self) -> bool:
        return self._closed

    def _check_open(self) -> None:
        if self._closed:
            raise ValueError("I/O operation on closed file.")

    def getvalue(self) -> bytes:
        self._check_open()
        return self._data

    def read(self, size: int = -1) -> bytes:
        self._check_open()
        if size < 0:
            end = len(self._data)
        else:
            end = min(len(self._data), self._pos + size)
        s = self._data[self._pos:end]
        self._pos = max(self._pos, end)
        return s

    def readline(self, size: int = -1) -> bytes:
        self._check_open()
        i = self._data.find(b"\n", self._pos)
        end = len(self._data) if i < 0 else i + 1
        if size >= 0 and end - self._pos > size:
            end = self._pos + size
        s = self._data[self._pos:end]
        self._pos = max(self._pos, end)
        return s

    def readlines(self, hint: int = -1) -> list[bytes]:
        lines: list[bytes] = []
        while True:
            line = self.readline()
            if not line:
                return lines
            lines.append(line)

    def write(self, b: bytes) -> int:
        self._check_open()
        if self._pos > len(self._data):
            self._data += b"\0" * (self._pos - len(self._data))
        self._data = self._data[:self._pos] + b + self._data[self._pos + len(b):]
        self._pos += len(b)
        return len(b)

    def writelines(self, lines: list[bytes]) -> None:
        for line in lines:
            self.write(line)

    def tell(self) -> int:
        self._check_open()
        return self._pos

    def seek(self, pos: int, whence: int = 0) -> int:
        self._check_open()
        if whence == SEEK_CUR:
            pos += self._pos
        elif whence == SEEK_END:
            pos += len(self._data)
        if pos < 0:
            raise ValueError("negative seek value " + str(pos))
        self._pos = pos
        return pos

    def truncate(self, size: int | None = None) -> int:
        self._check_open()
        if size is None:
            size = self._pos
        self._data = self._data[:size]
        return size

    def flush(self) -> None:
        self._check_open()

    def close(self) -> None:
        self._closed = True

    def readable(self) -> bool:
        return True

    def writable(self) -> bool:
        return True

    def seekable(self) -> bool:
        return True

    def __iter__(self) -> "BytesIO":
        return self

    def __next__(self) -> bytes:
        line = self.readline()
        if not line:
            raise StopIteration
        return line

    def __enter__(self) -> "BytesIO":
        return self

    def __exit__(self, et, ev, tb) -> bool:
        self.close()
        return False
