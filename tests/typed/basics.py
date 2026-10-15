# ints, bools, floats, strings: arithmetic, comparisons, formatting
a = 7
b: int = 3
print(a + b, a - b, a * b, a // b, a % b, a ** b)
print(-a // b, -a % b, a / b)
print(a & b, a | b, a ^ b, a << 2, a >> 1, ~a)
x = 2.5
y: float = 4
print(x + y, x * y, y / x, x - y)
print(x < y, a == 7, a != b, 1 < a < 10, b >= a)
ok: bool = True
print(ok and a > 3, not ok, ok or False)
s = "hello"
t = s + ", " + "world"
print(t, len(t), t.upper(), s * 2)
print(s[0], s[-1], s[1:3], s[::-1], s[:2] + s[3:])
print("ell" in s, "z" not in s, s.find("l"), s.count("l"))
print(str(42) + "!", int("123") + 1, float("1.5") * 2)
print(abs(-5), abs(-2.5), min(3, 1, 2), max(1.5, 0.5), round(2.6), round(3.14159, 2))
print("%d items at %.2f each, total %8.3f [%-5s] [%5s] %x %05d" % (3, 1.5, 4.5, "ab", "cd", 255, 42))
name = "Ann"
age = 30
print(f"{name} is {age} years old, {age / 4:.1f} quarters")
print(ord("A"), chr(66), bool(0), bool("x"), int(3.9), int(-3.9), float(2))
c = 10
c += 5
c -= 3
c *= 2
c //= 5
print(c)
f = 1.0
f += 1
f *= 3
f /= 2
print(f)
st = "a"
st += "b"
st *= 3
print(st)
print(1e3, 0.1 + 0.2, 1 / 3, 2.0, -0.5, 100.0 * 3)
print(True, False, None)
print(7 if a > 3 else 8, "yes" if not ok else "no")
