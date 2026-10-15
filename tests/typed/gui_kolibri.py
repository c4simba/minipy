# a KolibriOS window through raw system calls (sys.syscall -> int 0x40)
import sys

def pk(hi: int, lo: int) -> int:
    return (hi << 16) | (lo & 0xFFFF)

def redraw(title: str):
    sys.syscall(12, 1)
    sys.syscall(0, pk(100, 300), pk(80, 160), 0x14000000 | 0x223344, 0, title)
    msg = "Hello from compiled Python"
    sys.syscall(4, pk(20, 40), 0xFFFFFF, msg, len(msg))
    sys.syscall(8, pk(20, 120), pk(90, 30), 2, 0x5588CC)
    sys.syscall(12, 2)

redraw("Typed MiniPy")
running = True
while running:
    event = sys.syscall(10)[0]
    if event == 1:
        redraw("Typed MiniPy")
    elif event == 3:
        button = sys.syscall(17)[0] >> 8
        print("button", button)
        if button == 1:
            running = False
print("bye")
