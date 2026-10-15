# subclasses of str, bytes, tuple, list, dict, set, int and float; __prepare__ namespaces; types
class S(str):
    def shout(self):
        return self.upper() + "!"

s = S("hi")
print(type(s).__name__, s, s.shout(), s + "x", isinstance(s, str), len(s), repr(s), s == "hi", {s: 1}["hi"])


class I(int):
    def __repr__(self):
        return "I(" + str(int(self)) + ")"

    def double(self):
        return self * 2

i = I(5)
print(type(i).__name__, i, repr(i), i + 1, i.double(), isinstance(i, int), [10, 20, 30, 40, 50, 60][i], f"{i}", "%d" % i, hash(i) == hash(5), i == 5, i < 6, range(i), str(i), format(i, "03d"))


class T(tuple):
    @property
    def first(self):
        return self[0]

t = T((1, 2))
print(type(t).__name__, t, t.first, len(t), t + (3,))

class D(dict):
    def __missing__(self, key):
        return "missing:" + key


d = D(a=1)
print(d["a"], d["zz"], isinstance(d, dict), len(d), d, type(d).__name__, dict(d), d.get("q"), "a" in d)
d.extra = 5
print(d.extra, d.__dict__ if hasattr(d, "__dict__") else None)


class L(list):
    def total(self):
        return sum(self)


lst = L([1, 2, 3])
lst.append(4)
lst += [5]
print(lst, lst.total(), type(lst).__name__, lst[1:3], len(lst), lst + [6], isinstance(lst, list), sorted(lst, reverse=True))


class Lower(dict):
    def __setitem__(self, k, v):
        super().__setitem__(k.lower(), v)

    def __getitem__(self, k):
        return super().__getitem__(k.lower())


lw = Lower()
lw["HeLLo"] = 1
print(lw["HELLO"], dict(lw), list(lw.keys()))


class Point(tuple):
    def __new__(cls, x, y):
        return super().__new__(cls, (x, y))

    @property
    def x(self):
        return self[0]


p = Point(3, 4)
print(p, p.x, p[1], isinstance(p, tuple), p == (3, 4), hash(p) == hash((3, 4)), len(p), list(p))


class Flags(int):
    def __new__(cls, v):
        obj = super().__new__(cls, v)
        obj.label = "f" + str(v)
        return obj

    def __or__(self, other):
        return Flags(int(self) | int(other))


f = Flags(2) | Flags(4)
print(f, type(f).__name__, f.label, f + 1, f == 6, f > 5, {f: "x"}[6], int(f), float(f), bin(f), f.bit_length(), -f, abs(Flags(-3)), f // 4, divmod(f, 4))


class Name(str):
    def __new__(cls, v):
        return super().__new__(cls, v.title())

    def greet(self):
        return "Hello, " + self


n = Name("ann lee")
print(n, n.greet(), n.upper(), n.split(), ", ".join([n, "x"]), {n: 1}, n == "Ann Lee", type(n).__name__, f"{n:>10}|", n[0], len(n))


class MySet(set):
    pass


ms = MySet([1, 2, 2, 3])
ms.add(4)
print(sorted(ms), len(ms), 3 in ms, isinstance(ms, set), type(ms).__name__)
import json
print(json.dumps(D(a=1)), json.dumps(L([1, 2])), json.dumps(Point(1, 2)), json.dumps({"k": Flags(3)}), json.dumps(Name("x y")))

class Registry(dict):
    def __init__(self, name, **kw):
        super().__init__(**kw)
        self.name = name

    def __repr__(self):
        return "Registry(" + self.name + ", " + dict.__repr__(self) + ")"

    def register(self, key):
        def deco(fn):
            self[key] = fn
            return fn
        return deco


r = Registry("ops", base=0)


@r.register("add")
def add(a, b):
    return a + b


print(sorted(r), r["add"](2, 3), len(r), sorted(r), r.name, type(r) is dict, isinstance(r, dict), r == {"base": 0, "add": add})


class Stack(list):
    def push(self, x):
        self.append(x)
        return self

    def peek(self):
        return self[-1]

    def __repr__(self):
        return "Stack" + super().__repr__()


s = Stack()
s.push(1).push(2).push(3)
print(s, s.peek(), s.pop(), s, len(s), s * 2, list(reversed(s)), s[::-1], [x * 10 for x in s], 2 in s, s.index(2), s.count(1))
s2 = Stack([5, 4, 6])
s2.sort()
print(s2, sorted(s2), max(s2), s2 + s, type(s2 + s).__name__, s2 == [4, 5, 6], s2 != [4], s2 < [5])


class Upper(str):
    def __new__(cls, text):
        return super().__new__(cls, text.upper())

    def __eq__(self, other):
        return str.__eq__(self.lower(), other.lower()) if isinstance(other, str) else NotImplemented

    __hash__ = str.__hash__


u = Upper("hello")
print(u, repr(u), u == "HeLLo", u + "!", u * 2, u[1:3], u.lower(), "%s|%r" % (u, u), f"{u}", f"{u!r}", "L" in u, u.startswith("HE"), str(u), type(str(u)).__name__)
print(sorted([Upper("b"), Upper("a")]), {u: 1}, ",".join([u, u]), u.split("L"), len(u), u.encode(), hash(u) == hash("HELLO"))


class Celsius(float):
    def __new__(cls, v):
        return super().__new__(cls, v)

    def to_f(self):
        return self * 9 / 5 + 32

    def __repr__(self):
        return "Celsius(" + float.__repr__(self) + ")"


c = Celsius(25)
print(c, repr(c), c.to_f(), c + 1, round(c), int(c), c > 20, c == 25.0, f"{c:.1f}", str(c), abs(Celsius(-3.5)), c.is_integer(), math_ok := __import__("math").sqrt(c))


class Pair(tuple):
    __slots__ = ()

    def __new__(cls, a, b):
        return tuple.__new__(cls, (a, b))

    def swap(self):
        return Pair(self[1], self[0])


p = Pair(1, 2)
a, b = p
print(p, p.swap(), a, b, p + (3,), p[0], sorted([Pair(2, 1), Pair(1, 5)]), {p: "x"}[(1, 2)], tuple(p), list(p), p.count(1), max(p))


class OrderedSet(dict):
    def add(self, x):
        self[x] = None

    def __iter__(self):
        return iter(list(super().__iter__()))

    def __repr__(self):
        return "OrderedSet(" + repr(list(self)) + ")"


o = OrderedSet()
for x in "banana":
    o.add(x)
print(o, list(o), len(o), "n" in o)


class Counted(int):
    count = 0

    def __new__(cls, v):
        Counted.count += 1
        return super().__new__(cls, v)


xs = [Counted(3), Counted(1), Counted(2)]
print(sorted(xs), sum(xs), Counted.count, xs[0] * xs[1], [10, 20, 30, 40][xs[0]], "%d %x %.1f" % (xs[0], xs[0], xs[0]), range(xs[0]), max(xs), str(xs[0]), repr(xs[0]))
print(isinstance(xs[0], int), isinstance(True, int), type(xs[0]).__mro__, issubclass(Counted, int))

class Tracking(dict):
    def __init__(self):
        super().__init__()
        self.order = []

    def __setitem__(self, k, v):
        if not k.startswith("__"):
            if k in self:
                raise TypeError(repr(k) + " already defined")
            self.order.append(k)
        super().__setitem__(k, v)


class Meta(type):
    @classmethod
    def __prepare__(mcls, name, bases):
        return Tracking()

    def __new__(mcls, name, bases, ns):
        cls = super().__new__(mcls, name, bases, dict(ns))
        cls._order = ns.order
        return cls


class A(metaclass=Meta):
    z = 1
    a = 2
    m = z + a

    def f(self):
        pass


print(A._order, A.m)
try:
    class B(metaclass=Meta):
        x = 1
        x = 2
except TypeError as e:
    print("TypeError", e)

import types
print(repr(types.MappingProxyType({'a': 1})), types.SimpleNamespace(a=1, b='x'), types.FunctionType, types.NoneType, types.ModuleType, types.GeneratorType, types.MethodType, types.BuiltinFunctionType)
ns = types.SimpleNamespace(x=1)
ns.y = 2
print(ns, ns == types.SimpleNamespace(x=1, y=2), isinstance(lambda: 0, types.FunctionType), isinstance(len, types.BuiltinFunctionType))
C = types.new_class("C", (), {}, lambda ns: ns.update(v=5))
print(C, C.v, C().v)
mp = types.MappingProxyType({"k": 1})
try:
    mp["z"] = 2
except TypeError as e:
    print("TypeError", e)
print(mp["k"], len(mp), list(mp), "k" in mp, mp.get("q", 0), dict(mp))
