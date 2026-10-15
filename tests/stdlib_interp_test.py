# the standard library's parts only the interpreter has (tuples of a length known
# only when running): itertools' combinatorics, zip_longest, starmap, tee, batched;
# bisect's key=
import bisect
import itertools as it

print(list(it.product("ab", [1, 2])), list(it.product("ab", repeat=2))[:3], list(it.permutations([1, 2, 3], 2)), len(list(it.permutations(range(4)))))
print(list(it.combinations("abcd", 2)), list(it.combinations_with_replacement("ab", 2)), list(it.combinations([1], 3)))
print(list(it.zip_longest("ab", [1, 2, 3], fillvalue="-")), list(it.starmap(pow, [(2, 3), (3, 2)])), [list(x) for x in it.tee([1, 2], 2)])
print(list(it.batched("abcdefg", 3)), list(it.chain.from_iterable(["ab", "cd"])))
pairs = [(1, "a"), (3, "c"), (5, "e")]
print(bisect.bisect(pairs, 3, key=lambda p: p[0]), bisect.bisect_left(pairs, 3, key=lambda p: p[0]))
bisect.insort(pairs, (4, "d"), key=lambda p: p[0])
print(pairs)

from collections import namedtuple

Point = namedtuple("Point", ["x", "y"])
p = Point(1, y=2)
print(p, p.x + p.y, p[0], len(p), p == (1, 2), p._asdict(), p._replace(x=5), Point._fields)
a, b = p
print(a, b, list(p), Point._make([3, 4]))
P3 = namedtuple("P3", "a b c", defaults=[0])
print(P3(1, 2), P3(1, 2, 3))
try:
    Point(1)
except TypeError as e:
    print("TypeError:", e)
try:
    p.x = 3
except AttributeError as e:
    print("AttributeError")
