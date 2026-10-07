# the sys module: platform, raw system calls and byte buffers (Linux: int 0x80)
import sys
import time
print(sys.platform)
msg = "written by sys.syscall\n"
r = sys.syscall(4, 1, msg, len(msg))        # write(1, msg, len)
print(r[0])
pid = sys.syscall(20)[0]                     # getpid()
print(pid > 0)
b = sys.buffer(16)
sys.poke(b, 0, 0x12345678, 4)
sys.poke(b, 4, 0xAB, 1)
sys.poke_str(b, 8, "hi")
print(sys.peek(b, 0, 4) == 0x12345678, sys.peek(b, 0, 2) == 0x5678, sys.peek(b, 4, 1), sys.peek_str(b, 8, 2), len(b))
print(sys.addr(b) != 0)
sys.syscall(4, 1, b, 0)
hello = sys.buffer("hey\n")
sys.syscall(4, 1, hello, len(hello))
print(time.time() > 1000000000)          # seconds since the epoch (KolibriOS: since boot)
