# The built-in `sys` module: importable everywhere. syscall is the platform's:
# KolibriOS int 0x40; elsewhere the i386 Linux calls (int 0x80) of compiled programs.

import sys
print(sys.platform in ("linux", "darwin", "win32", "host"))   # the host's, as in CPython

r = sys.syscall(20)                 # Linux getpid(): the result in eax, r[0]
print(len(r), r[0] > 0)

# `sys` resolves from the module cache without touching the filesystem
from sys import platform
print(platform == sys.platform)

# raw byte buffers: build a struct and read the fields back (little-endian)
b = sys.buffer(16)
sys.poke(b, 0, 0x12345678, 4)
sys.poke(b, 4, 0xAB, 1)
sys.poke_str(b, 8, "hi")
print(sys.peek(b, 0, 4))
print(sys.peek(b, 0, 2))
print(sys.peek(b, 4, 1))
print(sys.peek_str(b, 8, 2))
print(sys.addr(b) != 0)
try:
    sys.peek(b, 14, 4)
except IndexError:
    print("bounds enforced")
