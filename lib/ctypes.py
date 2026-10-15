# ctypes for the interpreter: C functions of shared libraries through dlopen
# (the _ctypes module of src/vm_stdlib.c). Compiled programs do not read this
# file - the compiler has ctypes built in (calls linked by ld-linux.so.2).
#
#   libc = ctypes.CDLL("libc.so.6")
#   libc.strlen.restype = ctypes.c_size_t
#   libc.strlen(b"hello")
#
# Arguments: int, bool, str / bytes (char *), buffers, None (NULL); floats
# with argtypes c_double. The interpreter calls with up to 8 integer or
# pointer arguments, or up to 3 doubles for a function returning a double.
# A variadic function (printf) needs argtypes for its fixed arguments, as
# CPython's ctypes on arm64 Macs.
import sys
import _ctypes

class _CType:
    def __init__(self, name, kind):
        self.name = name
        self.kind = kind

    def __repr__(self):
        return "<class 'ctypes." + self.name + "'>"

c_int = _CType("c_int", "i32")
c_int32 = _CType("c_int32", "i32")
c_uint = _CType("c_uint", "u32")
c_uint32 = _CType("c_uint32", "u32")
c_long = _CType("c_long", "ip")
c_ulong = _CType("c_ulong", "up")
c_ssize_t = _CType("c_ssize_t", "ip")
c_size_t = _CType("c_size_t", "up")
c_longlong = _CType("c_longlong", "i64")
c_int64 = _CType("c_int64", "i64")
c_ulonglong = _CType("c_ulonglong", "i64")
c_uint64 = _CType("c_uint64", "i64")
c_short = _CType("c_short", "i16")
c_int16 = _CType("c_int16", "i16")
c_ushort = _CType("c_ushort", "u16")
c_uint16 = _CType("c_uint16", "u16")
c_byte = _CType("c_byte", "i8")
c_int8 = _CType("c_int8", "i8")
c_char = _CType("c_char", "i8")
c_ubyte = _CType("c_ubyte", "u8")
c_uint8 = _CType("c_uint8", "u8")
c_bool = _CType("c_bool", "bool")
c_double = _CType("c_double", "f64")
c_longdouble = _CType("c_longdouble", "f64")
c_float = _CType("c_float", "f32")
c_char_p = _CType("c_char_p", "str")
c_void_p = _CType("c_void_p", "ptr")

class _CFunction:
    def __init__(self, handle, name):
        self.__name__ = name
        self._address = _ctypes.dlsym(handle, name)
        self.restype = c_int
        self.argtypes = None

    def __call__(self, *args):
        values = list(args)
        if self.argtypes is not None:
            for i in range(len(values)):
                if i < len(self.argtypes) and self.argtypes[i].kind in ("f64", "f32"):
                    values[i] = float(values[i])
        kind = "void"
        if self.restype is not None:
            kind = self.restype.kind
        fixed = -1                       # more arguments than argtypes: variadic (printf)
        if self.argtypes is not None and len(values) > len(self.argtypes):
            fixed = len(self.argtypes)
        return _ctypes.call(self._address, kind, fixed, *values)

class CDLL:
    def __init__(self, name, mode=0, use_errno=False):
        self._name = name
        self._handle = _ctypes.dlopen(name)
        self._functions = {}

    def __getattr__(self, name):
        if name.startswith("_"):
            raise AttributeError(name)
        if name not in self._functions:
            self._functions[name] = _CFunction(self._handle, name)
        return self._functions[name]

class _LibraryLoader:
    def LoadLibrary(self, name):
        return CDLL(name)

cdll = _LibraryLoader()

def create_string_buffer(init, size=None):
    """A zeroed buffer: of `init` bytes, or holding the str init and a 0 byte."""
    if isinstance(init, int):
        return sys.buffer(init)
    if size is None:
        size = len(init) + 1
    b = sys.buffer(size)
    sys.poke_str(b, 0, init)
    return b

def addressof(buf):
    return sys.addr(buf)

def string_at(address, size=-1):
    if size < 0:
        return sys.cstr_at(address, 0x7FFFFFFF)
    return sys.peek_str_at(address, size)

def get_errno():
    return _ctypes.errno()
