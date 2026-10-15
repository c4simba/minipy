import ctypes

name = "libc.so.6"
libc = ctypes.CDLL(name)
