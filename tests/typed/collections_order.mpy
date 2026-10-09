# containers as CPython has them: set order (its hash table), dict order,
# equality of nested values, error messages, sorting, sums
def mk(xs: list[int]) -> set[int]:
    s: set[int] = set()
    for x in xs:
        s.add(x)
    return s

print({3, 1, 2}, {-1, -2, 5, 100, 1000}, {0 - 1, -2, 5, 100, 1000})
print({1.5, 2.5, 0.5}, {(1, 2), (0, 1)}, {True, False})
a = mk(list(range(0, 40, 3)))
b = mk(list(range(0, 60, 5)))
print(a | b)
print(a & b, a - b)
print(a ^ b, b - a, b ^ a)
x = mk(list(range(50)))
x -= mk(list(range(0, 50, 2)))
print(x)
y = mk(list(range(50)))
y ^= mk(list(range(25, 75)))
print(y)
z = mk(list(range(50)))
z &= mk(list(range(10, 20)))
print(z)
u = mk(list(range(100)))
u -= mk(list(range(1, 100)))
print(u, len(u))
w = mk([5, 9, 1])
w |= mk([100, 2])
print(w, 9 in w, 7 in w, 7 not in w)
w.add(64)
w.discard(9)
w.remove(1)
print(w, sorted(w), len(w))
try:
    w.remove(12345)
except KeyError as e:
    print("KeyError", e)
c = w.copy()
print(c == w, c != w, mk([1, 2]) <= mk([1, 2, 3]), mk([1, 2]) < mk([1, 2]), mk([4]).isdisjoint(mk([5])))
print({n * n for n in range(10) if n % 3 != 0})
print(set([7, 3, 7, 1]), set("hello") == set("olleh"), sorted(set("hello")))
words = {"pear", "apple", "fig"}
print(len(words), "fig" in words, sorted(words))
for n in mk([33, 64, 17]):
    print(n, end=" ")
print()
p = mk([1, 2, 3, 4, 5])
total = 0
while len(p) > 0:
    total += p.pop()
print(total, p)

d: dict[int, str] = {}
for i in range(20):
    d[i * 7 % 20] = str(i)
print(d)
print(d[14], d.get(99, "none"), d.setdefault(3, "x"), d.setdefault(100, "new"), len(d))
del d[0]
print(d.pop(7), d.pop(1000, "missing"), list(d.keys())[:5], list(d.values())[:5])
try:
    print(d[12345])
except KeyError as e:
    print("KeyError", e)
s2i = {"one": 1, "two": 2, "three": 3}
try:
    print(s2i["four"])
except KeyError as e:
    print("KeyError", e, repr(e))
t2i = {(1, 2): "a", (3, 4): "b"}
print(t2i[(3, 4)], (1, 2) in t2i, (2, 1) in t2i)
try:
    print(t2i[(5, 6)])
except KeyError as e:
    print("KeyError", e)
f2s = {0.5: "half", 1.5: "one and a half"}
print(f2s[0.5], f2s)
e2 = dict(s2i)
e2.update({"four": 4})
print(e2, e2 == s2i, s2i == {"three": 3, "two": 2, "one": 1})
print(sorted(e2.items()), [k for k, v in e2.items() if v % 2 == 0])
big: dict[int, int] = {}
for i in range(1000):
    big[i] = i * i
for i in range(0, 1000, 2):
    del big[i]
print(len(big), big[999], 500 in big, 501 in big, sum(big.values()))

nested = [[1, 2], [3], []]
print(nested == [[1, 2], [3], []], [3] in nested, [4] in nested, nested.index([3]))
print(sorted([[2, 1], [1, 5], [1, 2, 3], []]), [1, 2] < [1, 3], [1, 2] < [1, 2, 0])
pairs = [(1, "b"), (0, "z"), (1, "a"), (0, "a")]
print(sorted(pairs), min(pairs), max(pairs), sorted(pairs, key=lambda p: p[1]))
print(sorted(["b", "A", "a", "B"]), sorted([3, 1, 2], reverse=True), sorted([1.5, -2.0, 0.25]))
try:
    [1, 2, 3].index(4)
except ValueError as e:
    print("ValueError", e)
try:
    [1, 2, 3].remove(4)
except ValueError as e:
    print("ValueError", e)
try:
    print(max([1][1:]))
except ValueError as e:
    print("ValueError", e)
print(sum([0.1] * 10), sum([1e100, 1.0, -1e100]), sum([0.1, 0.2, 0.3]))

class P:
    def __init__(self, x: int, y: int):
        self.x = x
        self.y = y
    def __eq__(self, other: "P") -> bool:
        return self.x == other.x and self.y == other.y
    def __hash__(self) -> int:
        return self.x * 31 + self.y
    def __lt__(self, other: "P") -> bool:
        return (self.x, self.y) < (other.x, other.y)
    def __repr__(self) -> str:
        return "P(" + str(self.x) + ", " + str(self.y) + ")"

pts = [P(2, 1), P(1, 5), P(2, 0), P(1, 5)]
print(sorted(pts), min(pts), max(pts), P(1, 5) in pts, pts.count(P(1, 5)), pts.index(P(2, 0)))
ps = {P(1, 1), P(2, 2), P(1, 1)}
print(len(ps), P(2, 2) in ps, P(3, 3) in ps)
where = {P(0, 0): "origin"}
print(where[P(0, 0)])
try:
    print(where[P(9, 9)])
except KeyError as e:
    print("KeyError", e)
