"""math.frexp/ldexp, bytes.hex(sep, bytes_per_sep), and the shortest float repr where a tie rounds to the even digit (2**-24) or the value is subnormal."""
import math
for x in [3.5, -0.1, 0.0, -0.0, 1e-310, 5e-324, 1.7976931348623157e308, float("inf"), 1.0, 0.5, 2.0**-1022]:
    m, e = math.frexp(x)
    print(x, m, e, math.ldexp(m, e) == x)
print(math.frexp(float("nan")))
print(math.ldexp(1.0, 1023), math.ldexp(1.0, -1074), math.ldexp(1.0, -1075), math.ldexp(3.0, -1076), math.ldexp(-0.0, 5), math.ldexp(1.5, 10**15 * -1))
try:
    print(math.ldexp(1.0, 1024))
except OverflowError as e:
    print("OverflowError", e)
try:
    print(math.ldexp(1.0, 10**17))
except OverflowError as e:
    print("OverflowError", e)

data = b"abcdefg"
print(data.hex(), data.hex("-"), data.hex(":", 2), data.hex(":", -4), data.hex(b"|", 3), data.hex(":", -3))
print(data.hex("-", 0), b"".hex(":", 2), b"a".hex(":", 2), data.hex(sep="."), data.hex(sep=" ", bytes_per_sep=-2))
for sep in ["é", "ab", ""]:
    try:
        print(data.hex(sep))
    except ValueError as e:
        print("ValueError", e)
print(bytes.fromhex("61 62"))

for k in range(1, 80):
    x = 2.0 ** -k
    print(k, x, repr(x * 3), 2.0 ** k * 1.0000001)
print(5e-324, 1e23, 9007199254740993.0, 0.1 + 0.2, 1 / 3)
