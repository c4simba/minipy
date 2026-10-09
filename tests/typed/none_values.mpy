# None: what may be None is Optional[...], as in Python (a null reference; an
# int, float or bool keeps a reserved value for it)
def find(xs: list[str], key: str) -> str:
    for x in xs:
        if x.startswith(key):
            return x
    return None

r = find(["apple", "banana"], "b")
print(r, len(r))
r = find(["apple"], "z")
print(r, r is None, r == None, not r, r is not None)

count: int = None
print(count, count is None)
count = 5
count += 1
print(count, count is None)

best = None
for v in [3, 9, 4]:
    if best is None or v > best:
        best = v
print(best)

ratio: float = None
print(ratio, ratio is None, ratio == None)
ratio = 2.5
print(ratio * 2, ratio is None)

items: list[int] = None
print(items, items is None)
items = []
items.append(1)
print(items, items is None)

table: dict[str, int] = None
print(table)
table = {}
table["k"] = 1
print(table, table.get("k"), table.get("missing"), table.get("missing", 0))

class Box:
    def __init__(self):
        self.label: str = None
        self.size: int = None
        self.inner: Box = None

b = Box()
print(b.label, b.size, b.inner is None, b.inner)
b.inner = Box()
b.inner.size = 3
print(b.inner is None, b.inner.size, b.inner.label)

def maybe_box(make: bool) -> Box:
    if make:
        return Box()
    return None

print(maybe_box(True) is None, maybe_box(False) is None)

def describe(x: int = None) -> str:
    if x is None:
        return "nothing"
    return "number " + str(x)

print(describe(), describe(4), describe(None))

vals = [1, None, 3]
print(vals, vals.count(None), None in vals, 2 in vals, [v for v in vals if v is not None])
opts = {"a": None, "b": 2}
print(opts, opts["a"] is None, list(opts.values()))
print(f"{count} {ratio} {b.label} {None}")
flags = [True, None, False]
print(flags, [f is None for f in flags])
fl = [1.5, None]
print(fl, fl[1] is None, fl[0] + 1)
print(None == None, None is None, 1 == None, None != 0)
print(bool(None), str(None), repr(None), [None], (None, 1))

def first_even(xs: list[int]) -> int:
    for x in xs:
        if x % 2 == 0:
            return x

print(first_even([1, 3, 4]), first_even([1, 3]))
m = first_even([1, 3])
k = first_even([2])
print(m == k, m != k, m == None, k == 2, m is None)

# what Python raises on None
n: int = None
s: str = None
lst: list[int] = None
try:
    print(n + 1)
except TypeError as e:
    print("TypeError:", e)
try:
    print(1 + n)
except TypeError as e:
    print("TypeError:", e)
try:
    print(n < 3)
except TypeError as e:
    print("TypeError:", e)
try:
    print(-n)
except TypeError as e:
    print("TypeError:", e)
try:
    print(s + "x")
except TypeError as e:
    print("TypeError:", e)
try:
    print("x" + s)
except TypeError as e:
    print("TypeError:", e)
try:
    print(len(lst))
except TypeError as e:
    print("TypeError:", e)
try:
    lst.append(1)
except AttributeError as e:
    print("AttributeError:", e)
try:
    print(lst[0])
except TypeError as e:
    print("TypeError:", e)
try:
    for x in lst:
        print(x)
except TypeError as e:
    print("TypeError:", e)
try:
    print(b.inner.inner.size)
except AttributeError as e:
    print("AttributeError:", e)
try:
    print(s.upper())
except AttributeError as e:
    print("AttributeError:", e)
try:
    print(abs(n))
except TypeError as e:
    print("TypeError:", e)
try:
    n += 1
except TypeError as e:
    print("TypeError:", e)
try:
    print(3 in lst)
except TypeError as e:
    print("TypeError:", e)
