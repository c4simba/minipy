# bytes: literals, operators, methods, codecs
b = b"  Hello, World  "
t = b.strip()
print(t.find(b"o"), t.rfind(b"o"), t.index(b"W"), t.count(b"l"), t.find(111), t.find(b"o", 5), t.rfind(b"l", 0, 5), t.count(b"", 2, 4))
print(t.startswith(b"He"), t.endswith((b"x", b"ld")), t.startswith(b"ell", 1), t.split(b", "), b"a b  c".split(), b"a b c".rsplit(None, 1), t.split(maxsplit=1))
print(b.lstrip(), b.rstrip(), b"xxhixx".strip(b"x"), b"-".join([b"a", b"b"]), t.replace(b"l", b"L", 2), b"ab".replace(b"", b"-"))
print(t.upper(), t.lower(), t.swapcase(), b"hello world".title(), b"hELLO".capitalize(), t.partition(b", "), t.rpartition(b"o"), t.removeprefix(b"He"), t.removesuffix(b"ld"))
print(b"123".isdigit(), b"abc".isalpha(), b"a1".isalnum(), b" \t".isspace(), b"AB".isupper(), b"ab".islower(), b"Ab Cd".istitle(), b"\x80".isascii(), b"a\nb\r\nc".splitlines(), b"a\nb".splitlines(True))
print(t[0], t[-1], t[1:4], t[::-1], list(t[:3]), 72 in t, b"ll" in t, 300 - 228 not in t, len(t), t + b"!", t * 2, 2 * b"ab", b"ab" < b"b", b"a" == b"a", hash(b"ab") == hash("ab"))
print(bytes(3), bytes([65, 66]), bytes("é", "utf-8"), bytes.fromhex("41 42"), b"\x01\xab".hex(), int.from_bytes(b"\x01\x02", "big"), int.from_bytes(b"\x01\x02", "little"), (258).to_bytes(2, "big"), (-1).to_bytes(2, "little", signed=True))
print(int.from_bytes(b"\xff\xfe", "big", signed=True), int.from_bytes(b""), (5).to_bytes(), (65535).to_bytes(3, "little"), (1000).bit_length(), (-1000).bit_count(), (0).bit_length())
print(str(b"x"), f"{b'x'}", "%s" % b"x", repr(b"'"), [b"a"], {b"k": 1}, sorted([b"b", b"a"]), min(b"az"), max(b"az"), sum(b"\x01\x02"), sorted(b"cab"), set(b"aab") == {97, 98})
print(b"\xc3\xa9".decode(), "é".encode(), "é".encode("latin-1"), b"\xe9".decode("latin-1"), "aé".encode("ascii", "replace"), b"a\xffb".decode("utf-8", "replace"), b"a\xffb".decode(errors="ignore"), str(b"\xc3\xa9", "utf-8"))
for x in b"hi":
    print(x, end=" ")
print()
for i, x in enumerate(b"ok"):
    print(i, x, end="; ")
print()
d = {b"k": "v"}
print(d[b"k"], b"k" in d)
s = {b"a", b"b", b"a"}
print(len(s), b"a" in s)
def bad(n: int) -> None:
    try:
        if n == 0:
            print(bytes([256]))
        elif n == 1:
            print(b"abc"[5])
        elif n == 2:
            print(b"abc".index(b"z"))
        elif n == 3:
            print("aé€".encode("ascii"))
        elif n == 4:
            print(b"\xe2\x82".decode())
        elif n == 5:
            print(b"\x80\x81".decode("ascii"))
        elif n == 6:
            print(bytes.fromhex("4"))
        elif n == 7:
            print((300).to_bytes(1, "big"))
        elif n == 8:
            print("x".encode("koi8"))
        elif n == 9:
            print(b"\xff".decode("utf-8", "bogus"))
        elif n == 10:
            print(bytes.fromhex("4g"))
        elif n == 11:
            print((-1).to_bytes(1, "big"))
        elif n == 12:
            print(int.from_bytes(b"\x01", "middle"))
        elif n == 13:
            print(b"x".split(b""))
        elif n == 14:
            print(256 in b"x")
        elif n == 15:
            print(b"\xed\xa0\x80".decode(), b"")
    except (ValueError, IndexError, OverflowError, LookupError) as e:
        print(n, type(e).__name__, e)
for k in range(16):
    bad(k)
