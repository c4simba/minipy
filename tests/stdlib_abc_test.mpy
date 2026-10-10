# abc (ABCMeta: register, __subclasshook__, abstract methods) and collections.abc; subclasses of property / classmethod / bytearray
import collections.abc as cabc
from abc import ABC, ABCMeta, abstractmethod
print(isinstance({}, cabc.Mapping), isinstance([], cabc.Sequence), isinstance([], cabc.MutableSequence), isinstance((), cabc.MutableSequence))
print(isinstance("x", cabc.Sequence), isinstance(b"x", cabc.Sequence), isinstance(set(), cabc.Set), isinstance(frozenset(), cabc.MutableSet))
print(isinstance(iter([]), cabc.Iterator), isinstance([], cabc.Iterable), isinstance(len, cabc.Callable), isinstance(1, cabc.Hashable), isinstance([], cabc.Hashable))
print(isinstance({}.keys(), cabc.KeysView), isinstance({}.items(), cabc.Set), isinstance(range(3), cabc.Sequence), isinstance((x for x in []), cabc.Generator))
print(issubclass(dict, cabc.MutableMapping), issubclass(list, cabc.Collection), issubclass(int, cabc.Sized), isinstance(bytearray(), cabc.MutableSequence))
class MyMap(cabc.Mapping):
    def __init__(self, d): self._d = d
    def __getitem__(self, k): return self._d[k]
    def __iter__(self): return iter(self._d)
    def __len__(self): return len(self._d)
m = MyMap({"a": 1, "b": 2})
print(m.get("a"), m.get("z", 0), "a" in m, list(m.keys()), list(m.items()), m == {"a": 1, "b": 2}, dict(m))
class Base(ABC):
    @abstractmethod
    def f(self): ...
    @abstractmethod
    def g(self): ...
try:
    Base()
except TypeError as e:
    print(e)
class Impl(Base):
    def f(self): return 1
    def g(self): return 2
print(Impl().f(), isinstance(Impl(), Base), Base.__abstractmethods__ == frozenset({"f", "g"}), Impl.__abstractmethods__)
class Duck:
    def __len__(self): return 0
print(isinstance(Duck(), cabc.Sized), issubclass(Duck, cabc.Sized))
class Reg(metaclass=ABCMeta): pass
Reg.register(int)
print(isinstance(3, Reg), issubclass(bool, Reg), isinstance("s", Reg))
class MySeq(cabc.Sequence):
    def __getitem__(self, i): return [1, 2, 3][i]
    def __len__(self): return 3
s = MySeq()
print(list(s), s.index(2), s.count(3), 2 in s, list(reversed(s)))
class MS(cabc.MutableSet):
    def __init__(self): self.d = set()
    def __contains__(self, x): return x in self.d
    def __iter__(self): return iter(self.d)
    def __len__(self): return len(self.d)
    def add(self, x): self.d.add(x)
    def discard(self, x): self.d.discard(x)
ms = MS(); ms |= {1, 2}; ms -= {1}
print(sorted(ms), ms <= {2, 3}, isinstance(ms, cabc.Set))
print(cabc.Mapping.__module__, ABCMeta.__module__)
class P(property):
    pass
class W:
    @P
    def v(self):
        "doc of v"
        return 7
print(W().v, type(W.__dict__["v"]).__name__, W.__dict__["v"].__doc__)
class CM(classmethod):
    pass
class K:
    @CM
    def who(cls): return cls.__name__
print(K.who(), K().who())
class BA(bytearray):
    pass
ba = BA(b"xy"); ba.append(122)
print(ba, len(ba), isinstance(ba, bytearray), bytes(ba))
class AB(ABC):
    @classmethod
    @abstractmethod
    def make(cls): ...
    @property
    @abstractmethod
    def size(self): ...
print(sorted(AB.__abstractmethods__))
print(sorted(c.__name__ for c in Base.__subclasses__()), int.__subclasses__() == [bool] or "bool" in [c.__name__ for c in int.__subclasses__()])
