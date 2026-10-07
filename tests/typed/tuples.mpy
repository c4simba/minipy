# tuples: multiple return values, unpacking, lists of tuples, sorting, comparison
def divide(a: int, b: int) -> tuple[int, int]:
    return a // b, a % b

def minmax(xs: list[float]) -> tuple[float, float]:
    lo = xs[0]
    hi = xs[0]
    for x in xs:
        if x < lo:
            lo = x
        if x > hi:
            hi = x
    return lo, hi

q, r = divide(17, 5)
print(q, r, divide(20, 4))
lo, hi = minmax([3.5, -1.0, 7.25])
print(lo, hi)
point = (3, "three", 3.0)
print(point, point[0], point[1], point[-1], len(point))
x, name, f = point
print(x, name, f)
pairs = [("b", 2), ("a", 1), ("c", 3), ("a", 0)]
pairs.sort()
print(pairs)
print(sorted([(2, "x"), (1, "z"), (2, "a")]))
for key, value in pairs:
    print(key, value, end="; ")
print()
print((1, 2) == (1, 2), (1, 2) != (1, 3), (1, 2) < (1, 3), (2, 0) > (1, 9), ("a", 1) <= ("a", 1))
print((1, 2) in [(0, 0), (1, 2)], [(0, 0), (1, 2)].index((1, 2)), [(1, 1), (1, 1)].count((1, 1)))
single = (5,)
print(single, len(single))
d = {"x": 1, "y": 2}
items = d.items()
print(list(items), sorted(d.items()))
for k, v in sorted(d.items()):
    print(k, v)
print(divmod(17, 5), divmod(-7, 2), divmod(7.5, 2))
print("%s has %d" % ("bob", 3), "%d-%d" % divide(9, 4))
coords: list[tuple[int, int]] = []
for i in range(3):
    coords.append((i, i * i))
print(coords, [a + b for a, b in coords], {str(a): b for a, b in coords})
nested = ((1, 2), (3, 4))
print(nested, nested[1][0])
t2 = (1, 2, 3)
total = 0
for v in t2:
    total += v
print(total, 2 in (1, 2, 3), 5 not in (1, 2, 3), max(t2[0], t2[2]))
def swap(p: tuple[str, int]) -> tuple[int, str]:
    a, b = p
    return b, a
print(swap(("one", 1)))
for a, b, c in [(1, "x", 2.5), (2, "y", 3.5)]:
    print(a, b, c)
for (k, v) in [("p", 1)]:
    print(k, v)
