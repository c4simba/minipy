"""select: wait until sockets (objects with fileno()) are ready.

Linux: poll(). KolibriOS: each socket is asked (a non-blocking peek at what it
has received) every 10 ms until one is ready or the time is up; it counts as
writable at once."""
import sys
import _os
from typing import TypeVar

T = TypeVar("T")

POLLIN = 1
POLLPRI = 2
POLLOUT = 4
POLLERR = 8
POLLHUP = 16
POLLNVAL = 32
error = OSError


def _kreadable(fd: int) -> bool:
    b = sys.buffer(1)
    r = sys.syscall(75, 7, fd, b, 1, 0x42)            # fn 75.7 with MSG_PEEK | MSG_DONTWAIT
    return r[0] != -1 or (r[1] & 0xFFFFFFFF) != 6


def select(rlist: list[T], wlist: list[T], xlist: list[T], timeout: float | None = None) -> tuple[list[T], list[T], list[T]]:
    """(ready to read, ready to write, with an exceptional condition), waiting at most timeout seconds."""
    if timeout is not None and timeout < 0:
        raise ValueError("timeout must be non-negative")
    if sys.platform == "kolibrios":
        started = sys.syscall(26, 9)[0]
        while True:
            r = [x for x in rlist if _kreadable(x.fileno())]
            w = list(wlist)
            if r or w or (timeout is not None and (sys.syscall(26, 9)[0] - started) / 100.0 >= timeout):
                return (r, w, [])
            sys.syscall(5, 1)
    n = len(rlist) + len(wlist) + len(xlist)
    pfd = sys.buffer(8 * n + 8)
    i = 0
    for x in rlist:
        sys.poke(pfd, 8 * i, x.fileno(), 4)
        sys.poke(pfd, 8 * i + 4, POLLIN, 2)
        i += 1
    for x in wlist:
        sys.poke(pfd, 8 * i, x.fileno(), 4)
        sys.poke(pfd, 8 * i + 4, POLLOUT, 2)
        i += 1
    for x in xlist:
        sys.poke(pfd, 8 * i, x.fileno(), 4)
        sys.poke(pfd, 8 * i + 4, POLLPRI, 2)
        i += 1
    ms = -1
    if timeout is not None:
        ms = int(timeout * 1000)
    while True:
        got = sys.syscall(168, pfd, n, ms)[0]
        if got != -4:
            break
    _os._check(got, None)
    r: list[T] = []
    w: list[T] = []
    e: list[T] = []
    i = 0
    for x in rlist:
        if sys.peek(pfd, 8 * i + 6, 2) & (POLLIN | POLLHUP | POLLERR):
            r.append(x)
        i += 1
    for x in wlist:
        if sys.peek(pfd, 8 * i + 6, 2) & (POLLOUT | POLLERR):
            w.append(x)
        i += 1
    for x in xlist:
        if sys.peek(pfd, 8 * i + 6, 2) & POLLPRI:
            e.append(x)
        i += 1
    return (r, w, e)
