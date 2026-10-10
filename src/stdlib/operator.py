"""The operators as functions (CPython's operator): add(a, b) is a + b, itemgetter(1)
is lambda x: x[1], ... In compiled programs attrgetter and methodcaller (which look
attributes up by name) are not available."""
import sys
from typing import TypeVar

A = TypeVar("A")
B = TypeVar("B")


def lt(a: A, b: B) -> bool:
    return a < b


def le(a: A, b: B) -> bool:
    return a <= b


def eq(a: A, b: B) -> bool:
    return a == b


def ne(a: A, b: B) -> bool:
    return a != b


def ge(a: A, b: B) -> bool:
    return a >= b


def gt(a: A, b: B) -> bool:
    return a > b


def not_(a: A) -> bool:
    return not a


def truth(a: A) -> bool:
    return True if a else False


def is_(a: A, b: B) -> bool:
    return a is b


def is_not(a: A, b: B) -> bool:
    return a is not b


def is_none(a: A) -> bool:
    return a is None


def is_not_none(a: A) -> bool:
    return a is not None


def abs(a: A):
    return a if a >= 0 else -a


def add(a: A, b: B):
    return a + b


def and_(a: A, b: B):
    return a & b


def floordiv(a: A, b: B):
    return a // b


def index(a: A) -> int:
    if isinstance(a, int):
        return a
    else:
        return a.__index__()


def inv(a: A):
    return ~a


invert = inv


def lshift(a: A, b: B):
    return a << b


def mod(a: A, b: B):
    return a % b


def mul(a: A, b: B):
    return a * b


def matmul(a: A, b: B):
    return a @ b


def neg(a: A):
    return -a


def or_(a: A, b: B):
    return a | b


def pos(a: A):
    return +a


def pow(a: A, b: B):
    return a ** b


def rshift(a: A, b: B):
    return a >> b


def sub(a: A, b: B):
    return a - b


def truediv(a: A, b: B):
    return a / b


def xor(a: A, b: B):
    return a ^ b


def concat(a: A, b: B):
    return a + b


def contains(a: A, b: B) -> bool:
    return b in a


def _same(x: A, b: B) -> bool:
    if isinstance(x, (int, float, str, bytes, bool)):
        return x == b
    else:
        return x is b or x == b


def countOf(a: A, b: B) -> int:
    n = 0
    for x in a:
        if _same(x, b):
            n += 1
    return n


def indexOf(a: A, b: B) -> int:
    i = 0
    for x in a:
        if _same(x, b):
            return i
        i += 1
    raise ValueError("sequence.index(x): x not in sequence")


def getitem(a: A, b: B):
    return a[b]


def setitem(a: A, b: B, c) -> None:
    a[b] = c


def delitem(a: A, b: B) -> None:
    del a[b]


def length_hint(obj: A, default: int = 0) -> int:
    try:
        return len(obj)
    except TypeError:
        return default


def iadd(a: A, b: B):
    a += b
    return a


def iand(a: A, b: B):
    a &= b
    return a


def iconcat(a: A, b: B):
    a += b
    return a


def ifloordiv(a: A, b: B):
    a //= b
    return a


def ilshift(a: A, b: B):
    a <<= b
    return a


def imod(a: A, b: B):
    a %= b
    return a


def imul(a: A, b: B):
    a *= b
    return a


def ior(a: A, b: B):
    a |= b
    return a


def ipow(a: A, b: B):
    a **= b
    return a


def irshift(a: A, b: B):
    a >>= b
    return a


def isub(a: A, b: B):
    a -= b
    return a


def itruediv(a: A, b: B):
    a /= b
    return a


def ixor(a: A, b: B):
    a ^= b
    return a


def itemgetter(item: A):
    """A function giving obj[item] (several items: a tuple of them, interpreter only)."""
    def get(obj):
        return obj[item]
    return get


if not sys._compiled:

    class itemgetter:
        """itemgetter(item, ...)(obj): obj[item] (several: a tuple)."""

        def __init__(self, item, *items):
            self._items = (item,) + items

        def __call__(self, obj):
            if len(self._items) == 1:
                return obj[self._items[0]]
            return tuple(obj[i] for i in self._items)

        def __repr__(self):
            return "operator.itemgetter(" + ", ".join(repr(i) for i in self._items) + ")"

    class attrgetter:
        """attrgetter('a.b', ...)(obj): obj.a.b (several: a tuple)."""

        def __init__(self, attr, *attrs):
            for a in (attr,) + attrs:
                if not isinstance(a, str):
                    raise TypeError("attribute name must be a string")
            self._attrs = (attr,) + attrs

        def _one(self, obj, name):
            for part in name.split("."):
                obj = getattr(obj, part)
            return obj

        def __call__(self, obj):
            if len(self._attrs) == 1:
                return self._one(obj, self._attrs[0])
            return tuple(self._one(obj, a) for a in self._attrs)

        def __repr__(self):
            return "operator.attrgetter(" + ", ".join(repr(a) for a in self._attrs) + ")"

    class methodcaller:
        """methodcaller('name', *args, **kwargs)(obj): obj.name(*args, **kwargs)."""

        def __init__(self, name, *args, **kwargs):
            if not isinstance(name, str):
                raise TypeError("method name must be a string")
            self._name = name
            self._args = args
            self._kwargs = kwargs

        def __call__(self, obj):
            return getattr(obj, self._name)(*self._args, **self._kwargs)

        def __repr__(self):
            args = [repr(self._name)] + [repr(a) for a in self._args] + [k + "=" + repr(v) for k, v in self._kwargs.items()]
            return "operator.methodcaller(" + ", ".join(args) + ")"

__lt__ = lt
__le__ = le
__eq__ = eq
__ne__ = ne
__ge__ = ge
__gt__ = gt
__not__ = not_
__abs__ = abs
__add__ = add
__and__ = and_
__floordiv__ = floordiv
__index__ = index
__inv__ = inv
__invert__ = inv
__lshift__ = lshift
__mod__ = mod
__mul__ = mul
__matmul__ = matmul
__neg__ = neg
__or__ = or_
__pos__ = pos
__pow__ = pow
__rshift__ = rshift
__sub__ = sub
__truediv__ = truediv
__xor__ = xor
__concat__ = concat
__contains__ = contains
__getitem__ = getitem
__setitem__ = setitem
__delitem__ = delitem

if sys._compiled:
    # (compiled: attrgetter('a') and methodcaller('m', ...) are only for key= of sorted, min, max ...
    # where the compiler makes them lambdas; they cannot look names up when running)
    def attrgetter(attr: str) -> None:
        raise TypeError("operator.attrgetter is only available as a key= in compiled code")

    def methodcaller(name: str) -> None:
        raise TypeError("operator.methodcaller is only available as a key= in compiled code")
