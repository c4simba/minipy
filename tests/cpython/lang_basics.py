# expressions, literals, operators, control flow, unpacking
import math
print(1 + 2 * 3 ** 2, 7 // 2, -7 // 2, 7 % -3, 2 ** -1, 0x1F, 0o17, 0b101, 1_000_000, 1e3, 1.5e-3, 3j * 2, 10 / 4)
print(True + True, not 0, 5 > 3 > 1, 1 < 2 > 3, 2 in [1, 2], 3 not in (1, 2), None is None, [] is not [])
print(~5, -(-3), +7, 6 & 3, 6 | 3, 6 ^ 3, 1 << 10, 1024 >> 3, divmod(-7, 2), abs(-2.5), round(2.675, 2))
print("a" "b" 'c', b"x\x00y", "\N{GREEK SMALL LETTER ALPHA}", "tab\there", r"raw\n", "é", len("日本語"))
s = "hello world"
print(s[1:5], s[::-1], s[-3:], s[::2], s.title(), s.split(), "-".join(["a", "b"]), f"{s!r:>15}|{3.14159:.2f}|{42:08b}|{'x':^5}|")
w = 10; print(f"{w=}", f"{w + 1 = }", f"{'nested {}'.format(1)}", f"{[i for i in range(3)]}", f"{{literal}}")
a, b, *c = [1, 2, 3, 4, 5]; (d, e), f = (6, 7), 8; *g, h = "xyz"
print(a, b, c, d, e, f, g, h)
x = y = z = 0; x += 5; y -= 2; z ^= 7; print(x, y, z)
lst = [3, 1, 2]; lst[1:2] = [9, 9]; del lst[0]; print(lst)
d = {"k": 1, **{"j": 2}}; d["m"] = [*range(3), *"ab"]; print(d, {*d, "z"} >= {"k"})
for i in range(3):
    if i == 1:
        continue
    print("loop", i)
else:
    print("no break")
n = 0
while True:
    n += 1
    if n > 3:
        break
else:
    print("never")
print("n", n, [i * j for i in range(3) for j in range(3) if i != j], {i: i * i for i in range(4) if i % 2})
print((lambda p, *q, r=3, **s: (p, q, r, s))(1, 2, 3, r=4, t=5))
print(sorted([("b", 2), ("a", 1)], key=lambda t: t[1]), max(range(10), key=lambda v: -v), min([], default="empty"))
print([y := 10, y * 2], math.floor(-2.5), isinstance(3, (int, float)), type(3.0).__name__)
print(*[1, 2], sep="-", end="!\n")
cond = 5 if not [] else 6; print(cond, "yes" if 0 else "no", [] or [0] and "and")
