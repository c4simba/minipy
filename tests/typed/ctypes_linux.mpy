# ctypes: functions of the C library, linked dynamically (ld-linux.so.2);
# under tests/x86run.py they come from its small fake C library
import ctypes
from ctypes import c_char_p, c_size_t, create_string_buffer

libc = ctypes.CDLL("libc.so.6")
libc.strlen.restype = c_size_t
libc.strlen.argtypes = [c_char_p]
getenv = libc.getenv                       # an alias shares the declarations
getenv.restype = c_char_p
libc.strerror.restype = ctypes.c_char_p
libc.abs.argtypes = [ctypes.c_int]
libc.snprintf.argtypes = [ctypes.c_void_p, c_size_t, c_char_p]   # the fixed arguments of variadic ones
libc.printf.argtypes = [c_char_p]

def describe(n: int) -> str:
    buf = create_string_buffer(64)
    size = libc.snprintf(buf, 64, "%d|%5s|%-4x|%c|%%", n, "ab", 255, 65)
    return str(size) + " " + ctypes.string_at(ctypes.addressof(buf))

print("pid:", libc.getpid() > 0)
print("strlen:", libc.strlen("hello, libc"), libc.strlen(b"bytes too"), libc.strlen(""))
print("getenv:", getenv("HOME") is not None, repr(getenv("NO_SUCH_VARIABLE")))
print("abs:", libc.abs(-42), libc.abs(7))
print("snprintf:", describe(12), describe(-3))
libc.printf("printf: %s %d\n", "from C", 2026)
libc.fflush(None)                         # C stdio is buffered: before the next print
fd = libc.open("/nonexistent/file", 0)
print("open:", fd, ctypes.get_errno(), libc.strerror(ctypes.get_errno()))
