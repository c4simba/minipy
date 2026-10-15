# itertools and bisect (the parts compiled programs have)
import itertools
from itertools import accumulate, count, islice

print(list(islice(count(5), 3)), list(islice(count(1.5, 0.5), 3)), list(islice(itertools.cycle("ab"), 5)))
print(list(itertools.repeat("x", 3)), list(accumulate([1, 2, 3, 4])), list(accumulate([3, 1, 4], max)), list(accumulate([1, 2], initial=10)))
print(list(itertools.chain([1, 2], [3], [])), list(islice(range(10), 2, 8, 2)), list(islice("abcdef", None, 3)))
print(list(itertools.compress("ABCDEF", [1, 0, 1, 0, 1, 1])), list(itertools.dropwhile(lambda x: x < 3, [1, 2, 3, 1])))
print(list(itertools.takewhile(lambda x: x < 3, [1, 2, 3, 1])), list(itertools.filterfalse(lambda x: x % 2, range(6))), list(itertools.pairwise("abcd")))
print([(k, list(g)) for k, g in itertools.groupby("aaabccd")], [(k, len(list(g))) for k, g in itertools.groupby([1, 3, 2, 4, 5], key=lambda v: v % 2)])
print(list(itertools.filterfalse(None, [0, 1, 0, 2])), list(itertools.filterfalse(lambda s: s, ["", "x", ""])))
total = 0
for i in itertools.count():
    if i > 4:
        break
    total += i
print(total)
import bisect
from bisect import bisect_left, insort

a = [1, 3, 3, 5, 9]
print(bisect.bisect(a, 3), bisect_left(a, 3), bisect.bisect_right(a, 0), bisect.bisect_left(a, 10), bisect.bisect_left(a, 5, 1, 3))
insort(a, 4)
bisect.insort_left(a, 3)
print(a)
names = ["bob", "eve"]
bisect.insort(names, "dan")
print(names, bisect.bisect(names, "zed"))
grades = "FDCBA"
print([grades[bisect.bisect([60, 70, 80, 90], s)] for s in [33, 99, 77, 70, 89, 90, 100]])
try:
    bisect.bisect(a, 1, -1)
except ValueError as e:
    print(e)
