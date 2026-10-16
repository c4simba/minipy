"""Strings for tests/port/float_check.c --strings: the exact midpoint between
two neighbouring doubles (a tie: rounds to the even one), a hair above and
below it, and long digit strings - each with the double CPython's float()
gives (correctly rounded).

usage: python3 tests/port/midpoints.py N > FILE"""
import random
import struct
import sys
from decimal import Decimal, getcontext

getcontext().prec = 2000
rnd = random.Random(1234)
n = int(sys.argv[1]) if len(sys.argv) > 1 else 20000


def bits(x):
    return struct.unpack("<Q", struct.pack("<d", x))[0]


def frombits(u):
    return struct.unpack("<d", struct.pack("<Q", u))[0]


def emit(text):
    print("%s\t%016x" % (text, bits(float(text))))


for i in range(n):
    if i % 4 == 0:
        u = rnd.getrandbits(52) | (rnd.randrange(0, 3) << 52)          # subnormal and the smallest normals
    else:
        u = rnd.getrandbits(63)
    if (u >> 52) >= 0x7FE:
        continue
    lo, hi = frombits(u), frombits(u + 1)
    mid = (Decimal(lo) + Decimal(hi)) / 2
    emit(format(mid, "e"))
    eps = Decimal(10) ** (mid.adjusted() - 60)
    emit(format(mid + eps, "e"))
    emit(format(mid - eps, "e"))
    if i % 10 == 0:
        digits = "".join(rnd.choice("0123456789") for _ in range(rnd.randrange(20, 900)))
        emit("0." + digits + "e" + str(rnd.randrange(-330, 300)))
