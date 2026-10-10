"""OSError(errno, strerror[, filename[, winerror, filename2]]) in compiled programs.

The compiler turns such a call into one of these functions; the interpreter
does the same natively. As in CPython, str() is "[Errno 2] text: 'file'",
repr() shows (errno, strerror), and OSError(errno, ...) itself makes the
subclass for that error number (FileNotFoundError for 2 ...): the platform's
numbers (Linux's on Linux and KolibriOS, the host's on macOS), as CPython's."""
import sys
from typing import TypeVar

E = TypeVar("E")


def __mpy_oserror(e: E, errno: int, strerror: str, filename: str | None, filename2: str | None) -> E:
    _fill_oserror(e, errno, strerror, filename, filename2)       # (the work in one function: each class's copy is small)
    return e


def _fill_oserror(e: OSError, errno: int, strerror: str, filename: str | None, filename2: str | None) -> None:
    e.errno = errno
    e.strerror = strerror
    e.filename = filename
    e.filename2 = filename2
    msg = "[Errno " + str(errno) + "] " + strerror
    if filename is not None:
        msg += ": " + repr(filename)
        if filename2 is not None:
            msg += " -> " + repr(filename2)
    e._msg = msg
    e._argrepr = str(errno) + ", " + repr(strerror)


def __mpy_oserror_new(errno: int, strerror: str, filename: str | None, filename2: str | None) -> OSError:
    e: OSError = OSError()
    if sys.platform == "darwin":
        if errno == 35 or errno == 37 or errno == 36:
            e = BlockingIOError()
        elif errno == 10:
            e = ChildProcessError()
        elif errno == 32 or errno == 58:
            e = BrokenPipeError()
        elif errno == 53:
            e = ConnectionAbortedError()
        elif errno == 61:
            e = ConnectionRefusedError()
        elif errno == 54:
            e = ConnectionResetError()
        elif errno == 17:
            e = FileExistsError()
        elif errno == 2:
            e = FileNotFoundError()
        elif errno == 21:
            e = IsADirectoryError()
        elif errno == 20:
            e = NotADirectoryError()
        elif errno == 4:
            e = InterruptedError()
        elif errno == 13 or errno == 1 or errno == 107:
            e = PermissionError()
        elif errno == 3:
            e = ProcessLookupError()
        elif errno == 60:
            e = TimeoutError()
    else:
        if errno == 11 or errno == 114 or errno == 115:
            e = BlockingIOError()
        elif errno == 10:
            e = ChildProcessError()
        elif errno == 32 or errno == 108:
            e = BrokenPipeError()
        elif errno == 103:
            e = ConnectionAbortedError()
        elif errno == 111:
            e = ConnectionRefusedError()
        elif errno == 104:
            e = ConnectionResetError()
        elif errno == 17:
            e = FileExistsError()
        elif errno == 2:
            e = FileNotFoundError()
        elif errno == 21:
            e = IsADirectoryError()
        elif errno == 20:
            e = NotADirectoryError()
        elif errno == 4:
            e = InterruptedError()
        elif errno == 13 or errno == 1:
            e = PermissionError()
        elif errno == 3:
            e = ProcessLookupError()
        elif errno == 110:
            e = TimeoutError()
    _fill_oserror(e, errno, strerror, filename, filename2)
    return e


def __mpy_decode_error(e: E, encoding: str, obj: bytes, start: int, end: int, reason: str) -> E:
    """UnicodeDecodeError(encoding, object, start, end, reason)."""
    _fill_decode_error(e, encoding, obj, start, end, reason)
    return e


def _fill_decode_error(e: UnicodeDecodeError, encoding: str, obj: bytes, start: int, end: int, reason: str) -> None:
    e.encoding = encoding
    e.object = obj
    e.start = start
    e.end = end
    e.reason = reason
    if end == start + 1 and start >= 0 and start < len(obj):
        e._msg = "'" + encoding + "' codec can't decode byte 0x" + format(obj[start], "02x") + " in position " + str(start) + ": " + reason
    else:
        e._msg = "'" + encoding + "' codec can't decode bytes in position " + str(start) + "-" + str(end - 1) + ": " + reason
    e._argrepr = repr(encoding) + ", " + repr(obj) + ", " + str(start) + ", " + str(end) + ", " + repr(reason)


def __mpy_encode_error(e: E, encoding: str, obj: str, start: int, end: int, reason: str) -> E:
    """UnicodeEncodeError(encoding, object, start, end, reason)."""
    _fill_encode_error(e, encoding, obj, start, end, reason)
    return e


def _fill_encode_error(e: UnicodeEncodeError, encoding: str, obj: str, start: int, end: int, reason: str) -> None:
    e.encoding = encoding
    e.object = obj
    e.start = start
    e.end = end
    e.reason = reason
    if end == start + 1 and start >= 0 and start < len(obj):
        c = ord(obj[start])
        if c <= 0xFF:
            esc = "\\x" + format(c, "02x")
        elif c <= 0xFFFF:
            esc = "\\u" + format(c, "04x")
        else:
            esc = "\\U" + format(c, "08x")
        e._msg = "'" + encoding + "' codec can't encode character '" + esc + "' in position " + str(start) + ": " + reason
    else:
        e._msg = "'" + encoding + "' codec can't encode characters in position " + str(start) + "-" + str(end - 1) + ": " + reason
    e._argrepr = repr(encoding) + ", " + repr(obj) + ", " + str(start) + ", " + str(end) + ", " + repr(reason)
