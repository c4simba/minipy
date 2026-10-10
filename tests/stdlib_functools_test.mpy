# functools (CPython's): singledispatch, partialmethod, Placeholder, lru_cache, cached_property ...; UserDict / UserList / UserString
import functools
from functools import singledispatch, partial, partialmethod, lru_cache, cache, reduce, wraps, total_ordering, cmp_to_key, cached_property, Placeholder
@singledispatch
def show(x): return "obj " + repr(x)
@show.register
def _(x: int): return "int " + str(x)
@show.register(list)
def _(x): return "list of " + str(len(x))
print(show(1), show([1, 2]), show("s"), show(True), show.registry.keys().__len__() >= 3)
class C:
    def __init__(self): self.v = 1
    def add(self, a, b): return self.v + a + b
    add5 = partialmethod(add, 5)
    @functools.singledispatchmethod
    def kind(self, x): return "other"
    @kind.register
    def _(self, x: str): return "str"
print(C().add5(2), C().kind("a"), C().kind(1.5))
p = partial(pow, 2); print(p(10), p, partial(print, sep="-").keywords)
p2 = partial(divmod, Placeholder, 3); print(p2(10))
@lru_cache(maxsize=2)
def sq(n): return n * n
print([sq(i) for i in [1, 2, 1, 3, 1]], sq.cache_info())
@cache
def fib(n): return n if n < 2 else fib(n - 1) + fib(n - 2)
print(fib(50), reduce(lambda a, b: a * b, [1, 2, 3, 4]), reduce(lambda a, b: a + b, [], 7))
def deco(f):
    @wraps(f)
    def w(*a): return f(*a)
    return w
@deco
def named(x):
    "doc here"
    return x
print(named.__name__, named.__doc__, named.__wrapped__.__name__)
@total_ordering
class V:
    def __init__(self, v): self.v = v
    def __eq__(self, o): return self.v == o.v
    def __lt__(self, o): return self.v < o.v
print(V(1) <= V(2), V(3) > V(2), sorted([3, 1, 2], key=cmp_to_key(lambda a, b: b - a)))
class K:
    @cached_property
    def heavy(self):
        print("computing"); return 42
k = K(); print(k.heavy, k.heavy)
print(functools.WRAPPER_ASSIGNMENTS, repr(Placeholder))
from collections import UserDict, UserList, UserString
import collections.abc as cabc
class D(UserDict):
    def __setitem__(self, k, v):
        super().__setitem__(k.lower(), v)
d = D({"A": 1}); d["B"] = 2; d.update(C=3)
print(d, d.data, len(d), "a" in d, d.get("z", 0), list(d), isinstance(d, cabc.MutableMapping), d | {"x": 9}, d.copy())
l = UserList([3, 1, 2]); l.append(5); l.sort(); l2 = l + [7]
print(l, l2, l[1:], type(l[1:]).__name__, l * 2, 2 in l, l.index(3), isinstance(l, cabc.MutableSequence), l == [1, 2, 3, 5])
s = UserString("Hello"); print(s.upper(), s + "!", s[1:3], len(s), s.find("l"), "ell" in s, s.split("l"), repr(s), s == "Hello", s * 2, s.replace("l", "L"))
class Upper(UserString):
    def __init__(self, seq): super().__init__(str(seq).upper())
print(Upper("abc"), type(Upper("abc") + "d").__name__)
