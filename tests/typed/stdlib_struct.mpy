"""struct: pack/unpack/unpack_from/iter_unpack/calcsize and Struct objects (literal formats)."""
import struct
from struct import pack, unpack

print(struct.pack("<IHb", 1, 2, -3), struct.pack(">IHb", 1, 2, -3), struct.pack("!q", -2), struct.pack("<Q", 9223372036854775807))
print(struct.unpack("<IHb", b"\x01\x00\x00\x00\x02\x00\xfd"), struct.unpack(">h", b"\xff\xfe"), struct.unpack("<q", b"\xfe" + b"\xff" * 7))
a, b, c = struct.unpack("<ihB", struct.pack("<ihB", -100000, -300, 200))
print(a, b, c)
print(pack("<d", 1.5), pack(">d", -0.1), pack("<f", 3.14), pack("<e", 1.5), pack("<e", 65504.0), pack(">e", 0.1), pack("<e", 6e-8), pack("<e", 1e-8))
print(pack("<f", float("nan")), pack("<e", float("inf")), pack("<e", -0.0), pack("<d", float("-inf")), pack("<d", 5e-324), pack("<f", 1e-45), pack("<f", 3.4028234663852886e38))
for x in [1.5, -0.1, 3.14, 1e-310, 5e-324, 1.7976931348623157e308, 0.0, -0.0, 123456.789, 2.0**-149, 2.0**-126]:
    print(unpack("<d", pack("<d", x))[0] == x, unpack(">f", pack(">f", min(x, 1e38))), unpack("<e", pack("<e", min(x, 60000.0))))
print(unpack("<f", b"\x00\x00\xc0\x7f"), unpack("<e", b"\x00\x7c"), unpack("<e", b"\x00\xfc"), unpack("<e", b"\x01\x00"), unpack("<e", b"\xff\x7b"))
print(struct.pack("<c?3s5px", b"A", True, b"hello", b"ab"), struct.unpack("<c?3s5px", b"A\x01hel\x02ab\x00\x00\x00"))
print(struct.pack("@bhid", 1, 2, 3, 4.5), struct.pack("=bihq", 1, 2, 3, 4), struct.calcsize("@bhid"), struct.calcsize("<bihq"), struct.calcsize("@ci"), struct.calcsize("3s2xh"))
print(struct.unpack_from("<H", b"abcdef", 2), struct.unpack_from("<H", b"abcdef", offset=-2), struct.unpack_from(">HH", b"abcdef"))
for t in struct.iter_unpack("<HB", b"abcdefghi"):
    print(t)
try:
    print(struct.pack("<b", 200))
except struct.error as e:
    print("error", e)
try:
    print(struct.pack("<B", -1))
except struct.error as e:
    print("error", e)
try:
    print(struct.pack("<h", 40000))
except struct.error as e:
    print("error", e)
try:
    print(struct.pack("<I", -1))
except struct.error as e:
    print("error", e)
try:
    print(struct.pack("<i", 2147483648))
except struct.error as e:
    print("error", e)
try:
    struct.pack("<f", 1e300)
except OverflowError as e:
    print("OverflowError", e)
try:
    struct.pack("<e", 1e10)
except OverflowError as e:
    print("OverflowError", e)
try:
    struct.unpack("<h", b"abc")
except struct.error as e:
    print("error", e)
try:
    struct.pack("<c", b"ab")
except struct.error as e:
    print("error", e)
for off in [3, -1, -5, 5]:
    try:
        print(struct.unpack_from("<h", b"abcd", off))
    except struct.error as e:
        print("error", e)
try:
    list(struct.iter_unpack("<H", b"abc"))
except struct.error as e:
    print("error", e)
fmt = "<hh"
print(struct.calcsize(fmt))


hdr = struct.Struct("<IHb")
print(hdr.size, hdr.format, repr(hdr))
data = hdr.pack(7, 513, -2)
print(data)
print(hdr.unpack(data))
a, b, c = hdr.unpack_from(b"xx" + data, 2)
print(a, b, c)
pt = struct.Struct(">ff")
for x, y in pt.iter_unpack(pt.pack(1.5, -2.0) + pt.pack(0.25, 8.0)):
    print(x, y)
try:
    hdr.unpack(b"abc")
except struct.error as e:
    print("error", e)
flag = struct.Struct("?c3s")
print(flag.pack(True, b"z", b"abcd"), flag.unpack(b"\x00qxyz"))
