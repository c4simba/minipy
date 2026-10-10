# collections and heapq in the interpreter: the dict and tuple subclasses as CPython's
import heapq
import json
from collections import ChainMap, Counter, OrderedDict, defaultdict, deque, namedtuple

# defaultdict
d = defaultdict(list)
for k, v in [("a", 1), ("b", 2), ("a", 3)]:
    d[k].append(v)
print(d, isinstance(d, dict), d.default_factory is list)
print(sorted(d.items()), len(d), "c" in d, d.get("c"), d["c"], len(d))
e = d.copy()
e["z"].append(9)
print(type(e).__name__, e["z"], "z" in d)
dd = defaultdict(lambda: defaultdict(int))
dd["x"]["y"] += 2
print(dd["x"]["y"], json.dumps(dd))
try:
    defaultdict()["missing"]
except KeyError as ex:
    print("KeyError", ex)
try:
    defaultdict(5)
except TypeError as ex:
    print("TypeError", ex)
print(defaultdict(int, {"a": 1}) | {"b": 2})

# Counter
c = Counter("abracadabra")
print(c, c.most_common(3), c.total())
print(c["a"], c["zz"], "zz" in c, sorted(c.elements())[:6])
c.update("aaz")
c.subtract({"b": 5})
print(c)
print(Counter(a=3, b=1) + Counter(a=1, b=2), Counter(a=3, b=1) - Counter(a=1, b=2))
print(Counter(a=3, b=1) | Counter(a=1, b=2), Counter(a=3, b=1) & Counter(a=1, b=2))
print(+Counter(a=2, b=-1), -Counter(a=2, b=-1))
x = Counter(a=1)
x += Counter(a=2, b=-5)
print(x, Counter(a=1) == Counter(a=1, b=0), Counter(a=1) <= Counter(a=2), Counter() < Counter(a=1))
print(Counter(), json.dumps(Counter("hello")), dict(Counter("aab")))
del c["nothere"]
print(isinstance(c, dict), isinstance(c, Counter), Counter.__mro__[1] is dict)
try:
    Counter.fromkeys("ab")
except NotImplementedError as ex:
    print("NotImplementedError", ex)
words = "the cat and the hat and the bat".split()
print(Counter(words).most_common(2), len(Counter(words)))

# OrderedDict
o = OrderedDict([("x", 1), ("y", 2), ("z", 3)])
o.move_to_end("x")
print(o, list(o), o.popitem(), o.popitem(last=False), o)
o["v"] = 5
o.move_to_end("v", last=False)
o["w"] = 0
print(o, list(reversed(o)), isinstance(o, dict))
print(OrderedDict(a=1, b=2) == OrderedDict(b=2, a=1), OrderedDict(a=1, b=2) == {"b": 2, "a": 1})
print(OrderedDict(), OrderedDict.fromkeys("ab"), OrderedDict(a=1) | {"b": 2})

# namedtuple
Point = namedtuple("Point", "x y")
p = Point(1, y=2)
print(p, p.x, p.y, p[0], p[-1], tuple(p), len(p), isinstance(p, tuple))
a, b = p
print(a + b, p == (1, 2), p._asdict(), Point._fields, Point._make([3, 4]))
print(p._replace(x=10), hash(p) == hash((1, 2)), sorted([Point(2, 1), Point(1, 5)]))
print(Point.__name__, Point.__doc__, Point.x.__doc__, Point.__match_args__)
P3 = namedtuple("P3", ["a", "b", "c"], defaults=[0, 1])
print(P3(5), P3(5, 6, 7), P3._field_defaults)
print(namedtuple("R", "a def a", rename=True)._fields)
for bad in [lambda: Point(1), lambda: Point(1, 2, 3), lambda: Point(1, x=2), lambda: Point(1, 2, z=3),
            lambda: P3(), lambda: Point._make([1])]:
    try:
        bad()
    except TypeError as ex:
        print("TypeError", ex)
for args in [("P", "a _b"), ("P", "a a"), ("P", "a class"), ("1P", "a")]:
    try:
        namedtuple(*args)
    except ValueError as ex:
        print("ValueError", ex)
match p:
    case Point(x, y):
        print("match", x, y)
print(json.dumps(p), Point.__module__)


class Vec(namedtuple("Vec", "x y")):
    __slots__ = ()

    def __abs__(self):
        return (self.x ** 2 + self.y ** 2) ** 0.5


print(abs(Vec(3, 4)), Vec(3, 4), Vec._make((1, 2)))

# ChainMap
base = {"color": "red", "user": "guest"}
over = {"user": "admin"}
cm = ChainMap(over, base)
print(cm["user"], cm["color"], len(cm), sorted(cm), "color" in cm, cm.get("nope", 0))
child = cm.new_child({"color": "blue"})
print(child["color"], child.parents["color"], dict(child.items()) == {"color": "blue", "user": "admin"})
cm["new"] = 1
print(over, cm.maps[1] is base)
try:
    del cm["color"]
except KeyError as ex:
    print("KeyError", ex)

# deque
q = deque([1, 2, 3], maxlen=4)
q.appendleft(0)
q.append(4)
q.rotate(1)
print(q, q.maxlen, q[0], list(reversed(q)), q.popleft(), q.pop(), len(q))

# heapq
h = [5, 1, 8, 3, 9, 2]
heapq.heapify(h)
print([heapq.heappop(h) for _ in range(3)], h[0])
print(heapq.nlargest(3, [5, 1, 8, 3, 9, 2]), heapq.nsmallest(2, [5, 1, 8, 3, 9, 2]))
print(heapq.nlargest(2, ["bb", "a", "ccc"], key=len), heapq.nsmallest(5, [3, 1]), heapq.nlargest(0, [1]))
print(list(heapq.merge([1, 4, 7], [2, 5, 8], [3, 6])), list(heapq.merge(["c", "a"], ["b"], reverse=True)))
print(list(heapq.merge(["aa", "b"], ["c", "ddd"], key=len)))
mh = [3, 1, 4, 1, 5, 9, 2, 6]
heapq.heapify_max(mh)
heapq.heappush_max(mh, 7)
print([heapq.heappop_max(mh) for _ in range(4)], heapq.heappushpop_max(mh, 10), heapq.heapreplace_max(mh, 0))
