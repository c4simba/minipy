# classes: inheritance, super, properties, class/static methods, dunder methods, slots, metaclasses, mangling
import dataclasses, enum, abc
from typing import NamedTuple, Generic, TypeVar, Protocol

class Base:
    count = 0
    def __init__(self, x):
        self.x = x
        self.__private = x * 2
        Base.count += 1
    def priv(self): return self.__private
    @property
    def double(self): return self.x * 2
    @double.setter
    def double(self, v): self.x = v // 2
    @classmethod
    def make(cls, v): return cls(v)
    @staticmethod
    def helper(a, b): return a + b
    def __repr__(self): return f"{type(self).__name__}({self.x})"
    def __eq__(self, o): return isinstance(o, Base) and o.x == self.x
    def __hash__(self): return hash(self.x)
    def __add__(self, o): return type(self)(self.x + o.x)
    def __len__(self): return self.x
    def __getitem__(self, i): return i * self.x
    def __contains__(self, v): return v == self.x
    def __iter__(self): return iter(range(self.x))
    def __call__(self, *a): return sum(a) + self.x

class Child(Base):
    def __init__(self, x, y=0):
        super().__init__(x)
        self.y = y
    def priv(self): return ("child", super().priv())
    def __repr__(self): return "Child:" + super().__repr__()

b = Base(3); c = Child.make(4)
print(b, c, b.priv(), c.priv(), b.double, Base.helper(1, 2), Base.count, b + Base(1), len(b), b[5], 3 in b, list(b), b(1, 2))
c.double = 20; print(c.x, c == Child(10), {Base(1), Base(1)}, Child.__mro__[1].__name__, hasattr(b, "_Base__private"))
class Meta(type):
    def __new__(m, name, bases, ns, **kw):
        ns["meta_tag"] = kw.get("tag", "none")
        return super().__new__(m, name, bases, ns)
    def __call__(cls, *a, **k):
        obj = super().__call__(*a, **k); obj.created_by_meta = True; return obj
class WithMeta(metaclass=Meta, tag="T"):
    def __init__(self): self.v = 1
o = WithMeta(); print(o.meta_tag, o.created_by_meta, type(WithMeta).__name__)
class Slotted:
    __slots__ = ("a", "b")
    def __init__(self): self.a = 1
s = Slotted(); s.b = 2
try:
    s.c = 3
except AttributeError as e:
    print("slots:", type(e).__name__)
class Registry:
    subs = []
    def __init_subclass__(cls, /, key=None, **kw):
        super().__init_subclass__(**kw); Registry.subs.append((cls.__name__, key))
class R1(Registry, key="one"): pass
class R2(Registry): pass
print(Registry.subs)
@dataclasses.dataclass(order=True, frozen=True)
class Point:
    x: int
    y: int = 0
    tags: list = dataclasses.field(default_factory=list, compare=False)
    def norm(self): return (self.x ** 2 + self.y ** 2) ** 0.5
p = Point(3, 4); print(p, p.norm(), Point(1) < Point(2), dataclasses.asdict(p), dataclasses.fields(Point)[1].name)
class Color(enum.Enum):
    RED = 1
    GREEN = enum.auto()
print(Color.RED, Color(2), [c.name for c in Color], Color.GREEN.value)
class Pair(NamedTuple):
    a: int
    b: str = "x"
pr = Pair(1); print(pr, pr._asdict(), Pair.__module__, type(pr).__name__)
T = TypeVar("T")
class Box(Generic[T]):
    def __init__(self, v: T): self.v = v
    def get(self) -> T: return self.v
print(Box[int](5).get(), Box.__parameters__, T.__module__ if hasattr(T, "__module__") else "")
class Shape(abc.ABC):
    @abc.abstractmethod
    def area(self): ...
class Sq(Shape):
    def __init__(self, s): self.s = s
    def area(self): return self.s ** 2
try:
    Shape()
except TypeError as e:
    print("abstract:", "abstract" in str(e))
print(Sq(3).area(), isinstance(Sq(1), Shape))
class Desc:
    def __set_name__(self, owner, name): self.name = name
    def __get__(self, obj, typ=None): return f"desc {self.name}" if obj else self
class UsesDesc:
    attr = Desc()
print(UsesDesc().attr, UsesDesc.__dict__["attr"].name)
class Dyn:
    def __getattr__(self, n): return n.upper()
print(Dyn().hello, Base.__qualname__, Child.priv.__qualname__, Base.__doc__, Base.__module__)
class Outer:
    class Inner:
        def m(self): return __class__.__qualname__
    def get(self): return self.Inner().m()
print(Outer().get(), Outer.Inner.__qualname__)
class Cell:
    def who(self): return __class__.__name__, super().__repr__ is not None
print(Cell().who())
