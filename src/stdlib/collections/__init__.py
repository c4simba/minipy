"""Container datatypes: deque, defaultdict, Counter, OrderedDict, ChainMap, namedtuple
(CPython's collections).

In the interpreter defaultdict, Counter and OrderedDict are subclasses of dict and
namedtuple makes subclasses of tuple, as in CPython. Compiled programs get classes
of their own that do the same (isinstance(Counter(), dict) is False there); they
have no namedtuple or ChainMap (a dataclass or typing.NamedTuple instead)."""
import sys
from typing import Callable, Generic, TypeVar

T = TypeVar("T")
K = TypeVar("K")
V = TypeVar("V")


# ---------------------------------------------------------------- deque

class deque(Generic[T]):
    """A double-ended queue: append and pop at both ends in O(1)."""

    def __init__(self, iterable=None, maxlen: int | None = None):
        if maxlen is not None and maxlen < 0:
            raise ValueError("maxlen must be non-negative")
        self._items: list[T] = []
        self._head = 0
        self.maxlen = maxlen
        if iterable is not None:
            for x in iterable:
                self.append(x)

    def __len__(self) -> int:
        return len(self._items) - self._head

    def _compact(self) -> None:
        if self._head > 16 and self._head * 2 > len(self._items):
            del self._items[:self._head]
            self._head = 0

    def append(self, x: T) -> None:
        if self.maxlen is not None:
            if self.maxlen == 0:
                return
            if len(self) == self.maxlen:
                self.popleft()
        self._items.append(x)

    def appendleft(self, x: T) -> None:
        if self.maxlen is not None:
            if self.maxlen == 0:
                return
            if len(self) == self.maxlen:
                self.pop()
        if self._head == 0:
            n = len(self._items)
            if n < 8:
                n = 8
            self._items = [x] * n + self._items
            self._head = n
        self._head -= 1
        self._items[self._head] = x

    def pop(self) -> T:
        if len(self) == 0:
            raise IndexError("pop from an empty deque")
        x = self._items.pop()
        if len(self._items) == self._head:
            self._items = []
            self._head = 0
        return x

    def popleft(self) -> T:
        if len(self) == 0:
            raise IndexError("pop from an empty deque")
        x = self._items[self._head]
        self._head += 1
        if self._head == len(self._items):
            self._items = []
            self._head = 0
        else:
            self._compact()
        return x

    def extend(self, iterable) -> None:
        for x in list(iterable):
            self.append(x)

    def extendleft(self, iterable) -> None:
        for x in list(iterable):
            self.appendleft(x)

    def clear(self) -> None:
        self._items = []
        self._head = 0

    def _index(self, i: int) -> int:
        n = len(self)
        if i < 0:
            i += n
        if i < 0 or i >= n:
            raise IndexError("deque index out of range")
        return self._head + i

    def __getitem__(self, i: int) -> T:
        return self._items[self._index(i)]

    def __setitem__(self, i: int, x: T) -> None:
        self._items[self._index(i)] = x

    def __delitem__(self, i: int) -> None:
        del self._items[self._index(i)]

    def __iter__(self):
        for i in range(self._head, len(self._items)):
            yield self._items[i]

    def __reversed__(self):
        for i in range(len(self._items) - 1, self._head - 1, -1):
            yield self._items[i]

    def __contains__(self, x: T) -> bool:
        for i in range(self._head, len(self._items)):
            if self._items[i] == x:
                return True
        return False

    def count(self, x: T) -> int:
        n = 0
        for i in range(self._head, len(self._items)):
            if self._items[i] == x:
                n += 1
        return n

    def index(self, x: T, start: int = 0, stop: int = -1) -> int:
        n = len(self)
        if stop < 0 or stop > n:
            stop = n
        for i in range(start, stop):
            if self._items[self._head + i] == x:
                return i
        raise ValueError(repr(x) + " is not in deque")

    def remove(self, value: T) -> None:
        for i in range(self._head, len(self._items)):
            if self._items[i] == value:
                del self._items[i]
                return
        raise ValueError("deque.remove(x): x not in deque")

    def insert(self, i: int, x: T) -> None:
        if self.maxlen is not None and len(self) == self.maxlen:
            raise IndexError("deque already at its maximum size")
        n = len(self)
        if i < 0:
            i += n
            if i < 0:
                i = 0
        if i > n:
            i = n
        self._items.insert(self._head + i, x)

    def reverse(self) -> None:
        rest = self._items[self._head:]
        rest.reverse()
        self._items = rest
        self._head = 0

    def rotate(self, n: int = 1) -> None:
        size = len(self)
        if size <= 1:
            return
        n %= size
        if n == 0:
            return
        rest = self._items[self._head:]
        self._items = rest[size - n:] + rest[:size - n]
        self._head = 0

    def copy(self) -> "deque[T]":
        d: deque[T] = deque(None, self.maxlen)
        d._items = self._items[self._head:]
        return d

    def __eq__(self, other: "deque[T]") -> bool:
        return list(self) == list(other)

    def __ne__(self, other: "deque[T]") -> bool:
        return list(self) != list(other)

    def __repr__(self) -> str:
        if self.maxlen is None:
            return "deque(" + repr(list(self)) + ")"
        return "deque(" + repr(list(self)) + ", maxlen=" + str(self.maxlen) + ")"


# ---------------------------------------------------------------- defaultdict

class defaultdict(Generic[K, V]):
    """A dict whose missing keys get default_factory()."""

    def __init__(self, default_factory: Callable[[], V] | None = None, mapping=None):
        self.default_factory = default_factory
        self._d: dict[K, V] = {}
        if mapping is not None:
            for k in mapping:
                self._d[k] = mapping[k]

    def __getitem__(self, key: K) -> V:
        if key in self._d:
            return self._d[key]
        return self.__missing__(key)

    def __missing__(self, key: K) -> V:
        f = self.default_factory
        if f is None:
            raise KeyError(key)
        v = f()
        self._d[key] = v
        return v

    def __setitem__(self, key: K, value: V) -> None:
        self._d[key] = value

    def __delitem__(self, key: K) -> None:
        del self._d[key]

    def __contains__(self, key: K) -> bool:
        return key in self._d

    def __len__(self) -> int:
        return len(self._d)

    def __iter__(self):
        for k in list(self._d):
            yield k

    def keys(self):
        return self._d.keys()

    def values(self):
        return self._d.values()

    def items(self):
        return self._d.items()

    def get(self, key: K, default: V | None = None) -> V | None:
        if key in self._d:
            return self._d[key]
        return default

    def pop(self, key: K) -> V:
        return self._d.pop(key)

    def setdefault(self, key: K, default: V) -> V:
        if key not in self._d:
            self._d[key] = default
        return self._d[key]

    def clear(self) -> None:
        self._d.clear()

    def copy(self) -> "defaultdict[K, V]":
        d: defaultdict[K, V] = defaultdict(self.default_factory)
        for k in self._d:
            d._d[k] = self._d[k]
        return d

    def __eq__(self, other: "defaultdict[K, V]") -> bool:
        return self._d == other._d

    def __repr__(self) -> str:
        return "defaultdict(" + repr(self.default_factory) + ", " + repr(self._d) + ")"


# ---------------------------------------------------------------- Counter

class Counter(Generic[K]):
    """Counts of hashable things: Counter("hello")["l"] == 2."""

    def __init__(self, iterable=None):
        self._d: dict[K, int] = {}
        if iterable is not None:                  # (update's work, here: Counter() never calls it)
            if isinstance(iterable, dict):
                for k in iterable:
                    self._d[k] = self._d.get(k, 0) + iterable[k]
            else:
                for x in iterable:
                    self._d[x] = self._d.get(x, 0) + 1

    def update(self, iterable) -> None:
        """Add counts: of the items of an iterable, or the counts of a dict / Counter."""
        if isinstance(iterable, dict):
            for k in iterable:
                self._d[k] = self._d.get(k, 0) + iterable[k]
        else:
            for x in iterable:
                self._d[x] = self._d.get(x, 0) + 1

    def subtract(self, iterable) -> None:
        if isinstance(iterable, dict):
            for k in iterable:
                self._d[k] = self._d.get(k, 0) - iterable[k]
        else:
            for x in iterable:
                self._d[x] = self._d.get(x, 0) - 1

    def __getitem__(self, key: K) -> int:
        if key in self._d:
            return self._d[key]
        return 0

    def __setitem__(self, key: K, count: int) -> None:
        self._d[key] = count

    def __delitem__(self, key: K) -> None:
        if key in self._d:
            del self._d[key]

    def __contains__(self, key: K) -> bool:
        return key in self._d

    def __len__(self) -> int:
        return len(self._d)

    def __iter__(self):
        for k in list(self._d):
            yield k

    def keys(self):
        return self._d.keys()

    def values(self):
        return self._d.values()

    def items(self):
        return self._d.items()

    def get(self, key: K, default: int | None = None) -> int | None:
        if key in self._d:
            return self._d[key]
        return default

    def pop(self, key: K) -> int:
        return self._d.pop(key)

    def clear(self) -> None:
        self._d.clear()

    def total(self) -> int:
        return sum(self._d.values())

    def most_common(self, n: int | None = None) -> list[tuple[K, int]]:
        """(item, count) pairs, the most common first (ties: in insertion order)."""
        pairs = sorted(self._d.items(), key=lambda kv: kv[1], reverse=True)
        if n is None:
            return pairs
        return pairs[:n]

    def elements(self):
        """Each item as many times as its count (positive counts only)."""
        for k in self._d:
            for i in range(self._d[k]):
                yield k

    def copy(self) -> "Counter[K]":
        c: Counter[K] = Counter()
        for k in self._d:
            c._d[k] = self._d[k]
        return c

    def __add__(self, other: "Counter[K]") -> "Counter[K]":
        out: Counter[K] = Counter()
        for k in self._d:
            n = self._d[k] + other[k]
            if n > 0:
                out._d[k] = n
        for k in other._d:
            if k not in self._d and other._d[k] > 0:
                out._d[k] = other._d[k]
        return out

    def __sub__(self, other: "Counter[K]") -> "Counter[K]":
        out: Counter[K] = Counter()
        for k in self._d:
            n = self._d[k] - other[k]
            if n > 0:
                out._d[k] = n
        for k in other._d:
            if k not in self._d and other._d[k] < 0:
                out._d[k] = -other._d[k]
        return out

    def __or__(self, other: "Counter[K]") -> "Counter[K]":
        out: Counter[K] = Counter()
        for k in self._d:
            n = max(self._d[k], other[k])
            if n > 0:
                out._d[k] = n
        for k in other._d:
            if k not in self._d and other._d[k] > 0:
                out._d[k] = other._d[k]
        return out

    def __and__(self, other: "Counter[K]") -> "Counter[K]":
        out: Counter[K] = Counter()
        for k in self._d:
            n = min(self._d[k], other[k])
            if n > 0:
                out._d[k] = n
        return out

    def __eq__(self, other: "Counter[K]") -> bool:
        for k in self._d:
            if self._d[k] != other[k]:
                return False
        for k in other._d:
            if other._d[k] != self[k]:
                return False
        return True

    def __repr__(self) -> str:
        if not self._d:
            return "Counter()"
        parts = [repr(k) + ": " + repr(v) for k, v in self.most_common()]
        return "Counter({" + ", ".join(parts) + "})"


# ---------------------------------------------------------------- OrderedDict

class OrderedDict(Generic[K, V]):
    """A dict that can reorder its keys (move_to_end, popitem(last=False))."""

    def __init__(self, mapping=None):
        self._d: dict[K, V] = {}
        if mapping is not None:
            if isinstance(mapping, dict):
                for k in mapping:
                    self._d[k] = mapping[k]
            else:
                for k, v in mapping:
                    self._d[k] = v

    def __getitem__(self, key: K) -> V:
        return self._d[key]

    def __setitem__(self, key: K, value: V) -> None:
        self._d[key] = value

    def __delitem__(self, key: K) -> None:
        del self._d[key]

    def __contains__(self, key: K) -> bool:
        return key in self._d

    def __len__(self) -> int:
        return len(self._d)

    def __iter__(self):
        for k in list(self._d):
            yield k

    def __reversed__(self):
        ks = list(self._d)
        ks.reverse()
        for k in ks:
            yield k

    def keys(self):
        return self._d.keys()

    def values(self):
        return self._d.values()

    def items(self):
        return self._d.items()

    def get(self, key: K, default: V | None = None) -> V | None:
        if key in self._d:
            return self._d[key]
        return default

    def pop(self, key: K) -> V:
        return self._d.pop(key)

    def setdefault(self, key: K, default: V) -> V:
        if key not in self._d:
            self._d[key] = default
        return self._d[key]

    def clear(self) -> None:
        self._d.clear()

    def move_to_end(self, key: K, last: bool = True) -> None:
        """Move an existing key to the end (or the start: last=False)."""
        v = self._d.pop(key)
        if last:
            self._d[key] = v
            return
        rest = list(self._d.items())
        self._d = {}
        self._d[key] = v
        for k, x in rest:
            self._d[k] = x

    def popitem(self, last: bool = True) -> tuple[K, V]:
        if not self._d:
            raise KeyError("dictionary is empty")
        ks = list(self._d)
        k = ks[-1] if last else ks[0]
        return (k, self._d.pop(k))

    def copy(self) -> "OrderedDict[K, V]":
        o: OrderedDict[K, V] = OrderedDict()
        for k in self._d:
            o._d[k] = self._d[k]
        return o

    def __eq__(self, other: "OrderedDict[K, V]") -> bool:
        return list(self._d.items()) == list(other._d.items())

    def __repr__(self) -> str:
        if not self._d:
            return "OrderedDict()"
        return "OrderedDict(" + repr(self._d) + ")"


# ---------------------------------------------------------------- namedtuple (the interpreter's)

if not sys._compiled:
    # The interpreter's: real subclasses of dict and tuple, as CPython's (isinstance(Counter(), dict),
    # json.dumps of them ...); compiled programs keep the classes above.
    import heapq as _heapq
    from itertools import chain as _chain, repeat as _repeat, starmap as _starmap
    from operator import itemgetter as _itemgetter

    class defaultdict(dict):
        """A dict that makes a missing key's value with default_factory()."""

        def __init__(self, default_factory=None, /, *args, **kwargs):
            if default_factory is not None and not callable(default_factory):
                raise TypeError("first argument must be callable or None")
            super().__init__(*args, **kwargs)
            self.default_factory = default_factory

        def __missing__(self, key):
            if self.default_factory is None:
                raise KeyError(key)
            self[key] = value = self.default_factory()
            return value

        def __repr__(self):
            return "defaultdict(" + repr(self.default_factory) + ", " + dict.__repr__(self) + ")"

        def __reduce__(self):
            args = () if self.default_factory is None else (self.default_factory,)
            return (type(self), args, None, None, iter(self.items()))

        def copy(self):
            return type(self)(self.default_factory, self)

        __copy__ = copy

        def __or__(self, other):
            if not isinstance(other, dict):
                return NotImplemented
            new = self.copy()
            new.update(other)
            return new

        def __ror__(self, other):
            if not isinstance(other, dict):
                return NotImplemented
            new = type(self)(self.default_factory, other)
            new.update(self)
            return new

    class OrderedDict(dict):
        """A dict that remembers insertion order (move_to_end, popitem(last=False))."""

        def __reduce__(self):
            state = self.__getstate__()
            return (type(self), (), state, None, iter(self.items()))

        def __init__(self, other=(), /, **kwds):
            super().__init__()
            self.update(other, **kwds)

        def update(self, other=(), /, **kwds):
            if isinstance(other, dict) or hasattr(other, "keys"):
                for key in other.keys():
                    self[key] = other[key]
            else:
                for key, value in other:
                    self[key] = value
            for key, value in kwds.items():
                self[key] = value

        def move_to_end(self, key, last=True):
            """Moves key to the end (to the start if not last)."""
            value = dict.pop(self, key)
            if last:
                dict.__setitem__(self, key, value)
            else:
                rest = list(dict.items(self))
                dict.clear(self)
                dict.__setitem__(self, key, value)
                for k, v in rest:
                    dict.__setitem__(self, k, v)

        def popitem(self, last=True):
            """The last (or first) key and value, removed."""
            if not self:
                raise KeyError("dictionary is empty")
            key = next(reversed(list(dict.keys(self)))) if last else next(iter(dict.keys(self)))
            value = dict.pop(self, key)
            return key, value

        def __reversed__(self):
            return reversed(list(dict.keys(self)))

        def __eq__(self, other):
            if isinstance(other, OrderedDict):
                return dict.__eq__(self, other) and list(self) == list(other)
            return dict.__eq__(self, other)

        def __ne__(self, other):
            return not self == other

        __hash__ = None

        def __repr__(self):
            if not self:
                return self.__class__.__name__ + "()"
            return self.__class__.__name__ + "(" + dict.__repr__(self) + ")"

        def copy(self):
            return self.__class__(self)

        __copy__ = copy

        @classmethod
        def fromkeys(cls, iterable, value=None):
            self = cls()
            for key in iterable:
                self[key] = value
            return self

        def setdefault(self, key, default=None):
            if key in self:
                return self[key]
            self[key] = default
            return default

        def __or__(self, other):
            if not isinstance(other, dict):
                return NotImplemented
            new = self.__class__(self)
            new.update(other)
            return new

        def __ror__(self, other):
            if not isinstance(other, dict):
                return NotImplemented
            new = self.__class__(other)
            new.update(self)
            return new

        def __ior__(self, other):
            self.update(other)
            return self

    def _count_elements(mapping, iterable):
        mapping_get = mapping.get
        for elem in iterable:
            mapping[elem] = mapping_get(elem, 0) + 1

    class Counter(dict):
        """A dict of counts: Counter("abracadabra").most_common(2)."""

        def __reduce__(self):
            return (self.__class__, (dict(self),))

        def __init__(self, iterable=None, /, **kwds):
            super().__init__()
            self.update(iterable, **kwds)

        def __missing__(self, key):
            return 0

        def total(self):
            """The sum of the counts."""
            return sum(self.values())

        def most_common(self, n=None):
            """The n most common elements and their counts, most common first."""
            if n is None:
                return sorted(self.items(), key=_itemgetter(1), reverse=True)
            return _heapq.nlargest(n, self.items(), key=_itemgetter(1))

        def elements(self):
            """Each element repeated as many times as its count."""
            return _chain.from_iterable(_starmap(_repeat, self.items()))

        @classmethod
        def fromkeys(cls, iterable, v=None):
            raise NotImplementedError("Counter.fromkeys() is undefined.  Use Counter(iterable) instead.")

        def update(self, iterable=None, /, **kwds):
            """Adds counts (from an iterable of elements, or a mapping of counts)."""
            if iterable is not None:
                if isinstance(iterable, dict) or hasattr(iterable, "items") and hasattr(iterable, "keys"):
                    if self:
                        self_get = self.get
                        for elem, count in iterable.items():
                            self[elem] = count + self_get(elem, 0)
                    else:
                        dict.update(self, iterable)
                else:
                    _count_elements(self, iterable)
            if kwds:
                self.update(kwds)

        def subtract(self, iterable=None, /, **kwds):
            """Subtracts counts (they may go below zero)."""
            if iterable is not None:
                self_get = self.get
                if isinstance(iterable, dict) or hasattr(iterable, "items") and hasattr(iterable, "keys"):
                    for elem, count in iterable.items():
                        self[elem] = self_get(elem, 0) - count
                else:
                    for elem in iterable:
                        self[elem] = self_get(elem, 0) - 1
            if kwds:
                self.subtract(kwds)

        def copy(self):
            return self.__class__(self)

        def __delitem__(self, elem):
            if elem in self:
                super().__delitem__(elem)

        def __repr__(self):
            if not self:
                return self.__class__.__name__ + "()"
            try:
                d = dict(self.most_common())
            except TypeError:
                d = dict(self)
            return self.__class__.__name__ + "(" + repr(d) + ")"

        def __eq__(self, other):
            if not isinstance(other, Counter):
                return NotImplemented
            return all(self[e] == other[e] for c in (self, other) for e in c)

        def __ne__(self, other):
            if not isinstance(other, Counter):
                return NotImplemented
            return not self == other

        def __le__(self, other):
            if not isinstance(other, Counter):
                return NotImplemented
            return all(self[e] <= other[e] for c in (self, other) for e in c)

        def __lt__(self, other):
            if not isinstance(other, Counter):
                return NotImplemented
            return self <= other and self != other

        def __ge__(self, other):
            if not isinstance(other, Counter):
                return NotImplemented
            return all(self[e] >= other[e] for c in (self, other) for e in c)

        def __gt__(self, other):
            if not isinstance(other, Counter):
                return NotImplemented
            return self >= other and self != other

        def __add__(self, other):
            if not isinstance(other, Counter):
                return NotImplemented
            result = Counter()
            for elem, count in self.items():
                newcount = count + other[elem]
                if newcount > 0:
                    result[elem] = newcount
            for elem, count in other.items():
                if elem not in self and count > 0:
                    result[elem] = count
            return result

        def __sub__(self, other):
            if not isinstance(other, Counter):
                return NotImplemented
            result = Counter()
            for elem, count in self.items():
                newcount = count - other[elem]
                if newcount > 0:
                    result[elem] = newcount
            for elem, count in other.items():
                if elem not in self and count < 0:
                    result[elem] = 0 - count
            return result

        def __or__(self, other):
            if not isinstance(other, Counter):
                return NotImplemented
            result = Counter()
            for elem, count in self.items():
                other_count = other[elem]
                newcount = other_count if count < other_count else count
                if newcount > 0:
                    result[elem] = newcount
            for elem, count in other.items():
                if elem not in self and count > 0:
                    result[elem] = count
            return result

        def __and__(self, other):
            if not isinstance(other, Counter):
                return NotImplemented
            result = Counter()
            for elem, count in self.items():
                other_count = other[elem]
                newcount = count if count < other_count else other_count
                if newcount > 0:
                    result[elem] = newcount
            return result

        def __pos__(self):
            result = Counter()
            for elem, count in self.items():
                if count > 0:
                    result[elem] = count
            return result

        def __neg__(self):
            result = Counter()
            for elem, count in self.items():
                if count < 0:
                    result[elem] = 0 - count
            return result

        def _keep_positive(self):
            nonpositive = [elem for elem, count in self.items() if not count > 0]
            for elem in nonpositive:
                del self[elem]
            return self

        def __iadd__(self, other):
            for elem, count in other.items():
                self[elem] += count
            return self._keep_positive()

        def __isub__(self, other):
            for elem, count in other.items():
                self[elem] -= count
            return self._keep_positive()

        def __ior__(self, other):
            for elem, other_count in other.items():
                count = self[elem]
                if other_count > count:
                    self[elem] = other_count
            return self._keep_positive()

        def __iand__(self, other):
            for elem, count in self.items():
                other_count = other[elem]
                if other_count < count:
                    self[elem] = other_count
            return self._keep_positive()

    class ChainMap:
        """Several mappings searched in turn (the first holds the writes)."""

        def __init__(self, *maps):
            self.maps = list(maps) or [{}]

        def __missing__(self, key):
            raise KeyError(key)

        def __getitem__(self, key):
            for mapping in self.maps:
                if key in mapping:
                    return mapping[key]
            return self.__missing__(key)

        def get(self, key, default=None):
            return self[key] if key in self else default

        def __len__(self):
            return len(set().union(*self.maps))

        def __iter__(self):
            d = {}
            for mapping in reversed(self.maps):
                d.update(dict.fromkeys(mapping))
            return iter(d)

        def __contains__(self, key):
            return any(key in m for m in self.maps)

        def __bool__(self):
            return any(self.maps)

        def __repr__(self):
            return self.__class__.__name__ + "(" + ", ".join(map(repr, self.maps)) + ")"

        def keys(self):
            return list(iter(self))

        def values(self):
            return [self[k] for k in self]

        def items(self):
            return [(k, self[k]) for k in self]

        def copy(self):
            return self.__class__(self.maps[0].copy(), *self.maps[1:])

        __copy__ = copy

        def new_child(self, m=None, **kwargs):
            if m is None:
                m = kwargs
            elif kwargs:
                m.update(kwargs)
            return self.__class__(m, *self.maps)

        @property
        def parents(self):
            return self.__class__(*self.maps[1:])

        def __setitem__(self, key, value):
            self.maps[0][key] = value

        def __delitem__(self, key):
            try:
                del self.maps[0][key]
            except KeyError:
                raise KeyError("Key not found in the first mapping: " + repr(key))

        def popitem(self):
            try:
                return self.maps[0].popitem()
            except KeyError:
                raise KeyError("No keys found in the first mapping.")

        def pop(self, key, *args):
            try:
                return self.maps[0].pop(key, *args)
            except KeyError:
                raise KeyError("Key not found in the first mapping: " + repr(key))

        def clear(self):
            self.maps[0].clear()

        def __eq__(self, other):
            return dict(self.items()) == dict(other.items()) if hasattr(other, "items") else NotImplemented

        def __or__(self, other):
            m = self.copy()
            m.maps[0].update(other)
            return m

    _KEYWORDS = frozenset(["False", "None", "True", "and", "as", "assert", "async", "await", "break", "class",
                           "continue", "def", "del", "elif", "else", "except", "finally", "for", "from", "global",
                           "if", "import", "in", "is", "lambda", "nonlocal", "not", "or", "pass", "raise", "return",
                           "try", "while", "with", "yield"])

    def namedtuple(typename, field_names, *, rename=False, defaults=None, module=None):
        """A subclass of tuple whose items are also fields by name: Point(x=1, y=2)."""
        if isinstance(field_names, str):
            field_names = field_names.replace(",", " ").split()
        field_names = list(map(str, field_names))
        typename = sys.intern(str(typename))
        if rename:
            seen = set()
            for index, name in enumerate(field_names):
                if not name.isidentifier() or name in _KEYWORDS or name.startswith("_") or name in seen:
                    field_names[index] = "_" + str(index)
                seen.add(name)
        for name in [typename] + field_names:
            if type(name) is not str:
                raise TypeError("Type names and field names must be strings")
            if not name.isidentifier():
                raise ValueError("Type names and field names must be valid identifiers: " + repr(name))
            if name in _KEYWORDS:
                raise ValueError("Type names and field names cannot be a keyword: " + repr(name))
        seen = set()
        for name in field_names:
            if name.startswith("_") and not rename:
                raise ValueError("Field names cannot start with an underscore: " + repr(name))
            if name in seen:
                raise ValueError("Encountered duplicate field name: " + repr(name))
            seen.add(name)
        field_defaults = {}
        if defaults is not None:
            defaults = tuple(defaults)
            if len(defaults) > len(field_names):
                raise TypeError("Got more default values than field names")
            field_defaults = dict(reversed(list(zip(reversed(field_names), reversed(defaults)))))
        field_names = tuple(field_names)
        num_fields = len(field_names)
        tuple_new = tuple.__new__

        def __new__(_cls, *args, **kwargs):
            if len(args) > num_fields:
                raise TypeError(typename + ".__new__() takes " + str(num_fields + 1) + " positional arguments but "
                                + str(len(args) + 1) + " were given")
            values = list(args)
            for name in field_names[:len(args)]:
                if name in kwargs:
                    raise TypeError(typename + ".__new__() got multiple values for argument " + repr(name))
            missing = []
            for name in field_names[len(args):]:
                if name in kwargs:
                    values.append(kwargs.pop(name))
                elif name in field_defaults:
                    values.append(field_defaults[name])
                else:
                    missing.append(name)
            if kwargs:
                raise TypeError(typename + ".__new__() got an unexpected keyword argument " + repr(next(iter(kwargs))))
            if missing:
                if len(missing) == 1:
                    raise TypeError(typename + ".__new__() missing 1 required positional argument: " + repr(missing[0]))
                names = ", ".join(repr(m) for m in missing[:-1]) + " and " + repr(missing[-1])
                raise TypeError(typename + ".__new__() missing " + str(len(missing)) +
                                " required positional arguments: " + names)
            return tuple_new(_cls, values)

        @classmethod
        def _make(cls, iterable):
            result = tuple_new(cls, iterable)
            if len(result) != num_fields:
                raise TypeError("Expected " + str(num_fields) + " arguments, got " + str(len(result)))
            return result

        def _replace(self, /, **kwds):
            result = self._make(map(kwds.pop, field_names, self))
            if kwds:
                raise TypeError("Got unexpected field names: " + repr(list(kwds)))
            return result

        def __repr__(self):
            return self.__class__.__name__ + "(" + ", ".join(n + "=" + repr(v) for n, v in zip(field_names, self)) + ")"

        def _asdict(self):
            return dict(zip(self._fields, self))

        def __getnewargs__(self):
            return tuple(self)

        class_namespace = {
            "__doc__": typename + "(" + ", ".join(field_names) + ")",
            "__slots__": (),
            "_fields": field_names,
            "_field_defaults": field_defaults,
            "__new__": __new__,
            "_make": _make,
            "__replace__": _replace,
            "_replace": _replace,
            "__repr__": __repr__,
            "_asdict": _asdict,
            "__getnewargs__": __getnewargs__,
            "__match_args__": field_names,
        }
        for index, name in enumerate(field_names):
            class_namespace[name] = property(_itemgetter(index), doc="Alias for field number " + str(index))
        result = type(typename, (tuple,), class_namespace)
        if module is None:
            module = sys._getframemodulename(1) or "__main__"
        result.__module__ = module
        return result

    # UserDict, UserList, UserString (CPython's)
    import collections.abc as _cabc
    from operator import eq as _eq
    import sys as _sys

    class UserDict(_cabc.MutableMapping):

        # Start by filling-out the abstract methods
        def __init__(self, dict=None, /, **kwargs):
            self.data = {}
            if dict is not None:
                self.update(dict)
            if kwargs:
                self.update(kwargs)

        def __len__(self):
            return len(self.data)

        def __getitem__(self, key):
            if key in self.data:
                return self.data[key]
            if hasattr(self.__class__, "__missing__"):
                return self.__class__.__missing__(self, key)
            raise KeyError(key)

        def __setitem__(self, key, item):
            self.data[key] = item

        def __delitem__(self, key):
            del self.data[key]

        def __iter__(self):
            return iter(self.data)

        # Modify __contains__ and get() to work like dict
        # does when __missing__ is present.
        def __contains__(self, key):
            return key in self.data

        def get(self, key, default=None):
            if key in self:
                return self[key]
            return default


        # Now, add the methods in dicts but not in MutableMapping
        def __repr__(self):
            return repr(self.data)

        def __or__(self, other):
            if isinstance(other, UserDict):
                return self.__class__(self.data | other.data)
            if isinstance(other, dict):
                return self.__class__(self.data | other)
            return NotImplemented

        def __ror__(self, other):
            if isinstance(other, UserDict):
                return self.__class__(other.data | self.data)
            if isinstance(other, dict):
                return self.__class__(other | self.data)
            return NotImplemented

        def __ior__(self, other):
            if isinstance(other, UserDict):
                self.data |= other.data
            else:
                self.data |= other
            return self

        def __copy__(self):
            inst = self.__class__.__new__(self.__class__)
            inst.__dict__.update(self.__dict__)
            # Create a copy and avoid triggering descriptors
            inst.__dict__["data"] = self.__dict__["data"].copy()
            return inst

        def copy(self):
            if self.__class__ is UserDict:
                return UserDict(self.data.copy())
            import copy
            data = self.data
            try:
                self.data = {}
                c = copy.copy(self)
            finally:
                self.data = data
            c.update(self)
            return c

        @classmethod
        def fromkeys(cls, iterable, value=None):
            d = cls()
            for key in iterable:
                d[key] = value
            return d



    class UserList(_cabc.MutableSequence):
        """A more or less complete user-defined wrapper around list objects."""

        def __init__(self, initlist=None):
            self.data = []
            if initlist is not None:
                # XXX should this accept an arbitrary sequence?
                if type(initlist) == type(self.data):
                    self.data[:] = initlist
                elif isinstance(initlist, UserList):
                    self.data[:] = initlist.data[:]
                else:
                    self.data = list(initlist)

        def __repr__(self):
            return repr(self.data)

        def __lt__(self, other):
            return self.data < self.__cast(other)

        def __le__(self, other):
            return self.data <= self.__cast(other)

        def __eq__(self, other):
            return self.data == self.__cast(other)

        def __gt__(self, other):
            return self.data > self.__cast(other)

        def __ge__(self, other):
            return self.data >= self.__cast(other)

        def __cast(self, other):
            return other.data if isinstance(other, UserList) else other

        def __contains__(self, item):
            return item in self.data

        def __len__(self):
            return len(self.data)

        def __getitem__(self, i):
            if isinstance(i, slice):
                return self.__class__(self.data[i])
            else:
                return self.data[i]

        def __setitem__(self, i, item):
            self.data[i] = item

        def __delitem__(self, i):
            del self.data[i]

        def __add__(self, other):
            if isinstance(other, UserList):
                return self.__class__(self.data + other.data)
            elif isinstance(other, type(self.data)):
                return self.__class__(self.data + other)
            return self.__class__(self.data + list(other))

        def __radd__(self, other):
            if isinstance(other, UserList):
                return self.__class__(other.data + self.data)
            elif isinstance(other, type(self.data)):
                return self.__class__(other + self.data)
            return self.__class__(list(other) + self.data)

        def __iadd__(self, other):
            if isinstance(other, UserList):
                self.data += other.data
            elif isinstance(other, type(self.data)):
                self.data += other
            else:
                self.data += list(other)
            return self

        def __mul__(self, n):
            return self.__class__(self.data * n)

        __rmul__ = __mul__

        def __imul__(self, n):
            self.data *= n
            return self

        def __copy__(self):
            inst = self.__class__.__new__(self.__class__)
            inst.__dict__.update(self.__dict__)
            # Create a copy and avoid triggering descriptors
            inst.__dict__["data"] = self.__dict__["data"][:]
            return inst

        def append(self, item):
            self.data.append(item)

        def insert(self, i, item):
            self.data.insert(i, item)

        def pop(self, i=-1):
            return self.data.pop(i)

        def remove(self, item):
            self.data.remove(item)

        def clear(self):
            self.data.clear()

        def copy(self):
            return self.__class__(self)

        def count(self, item):
            return self.data.count(item)

        def index(self, item, *args):
            return self.data.index(item, *args)

        def reverse(self):
            self.data.reverse()

        def sort(self, /, *args, **kwds):
            self.data.sort(*args, **kwds)

        def extend(self, other):
            if isinstance(other, UserList):
                self.data.extend(other.data)
            else:
                self.data.extend(other)



    class UserString(_cabc.Sequence):

        def __init__(self, seq):
            if isinstance(seq, str):
                self.data = seq
            elif isinstance(seq, UserString):
                self.data = seq.data[:]
            else:
                self.data = str(seq)

        def __str__(self):
            return str(self.data)

        def __repr__(self):
            return repr(self.data)

        def __int__(self):
            return int(self.data)

        def __float__(self):
            return float(self.data)

        def __complex__(self):
            return complex(self.data)

        def __hash__(self):
            return hash(self.data)

        def __getnewargs__(self):
            return (self.data[:],)

        def __eq__(self, string):
            if isinstance(string, UserString):
                return self.data == string.data
            return self.data == string

        def __lt__(self, string):
            if isinstance(string, UserString):
                return self.data < string.data
            return self.data < string

        def __le__(self, string):
            if isinstance(string, UserString):
                return self.data <= string.data
            return self.data <= string

        def __gt__(self, string):
            if isinstance(string, UserString):
                return self.data > string.data
            return self.data > string

        def __ge__(self, string):
            if isinstance(string, UserString):
                return self.data >= string.data
            return self.data >= string

        def __contains__(self, char):
            if isinstance(char, UserString):
                char = char.data
            return char in self.data

        def __len__(self):
            return len(self.data)

        def __getitem__(self, index):
            return self.__class__(self.data[index])

        def __add__(self, other):
            if isinstance(other, UserString):
                return self.__class__(self.data + other.data)
            elif isinstance(other, str):
                return self.__class__(self.data + other)
            return self.__class__(self.data + str(other))

        def __radd__(self, other):
            if isinstance(other, str):
                return self.__class__(other + self.data)
            return self.__class__(str(other) + self.data)

        def __mul__(self, n):
            return self.__class__(self.data * n)

        __rmul__ = __mul__

        def __mod__(self, args):
            return self.__class__(self.data % args)

        def __rmod__(self, template):
            return self.__class__(str(template) % self)

        # the following methods are defined in alphabetical order:
        def capitalize(self):
            return self.__class__(self.data.capitalize())

        def casefold(self):
            return self.__class__(self.data.casefold())

        def center(self, width, *args):
            return self.__class__(self.data.center(width, *args))

        def count(self, sub, start=0, end=_sys.maxsize):
            if isinstance(sub, UserString):
                sub = sub.data
            return self.data.count(sub, start, end)

        def removeprefix(self, prefix, /):
            if isinstance(prefix, UserString):
                prefix = prefix.data
            return self.__class__(self.data.removeprefix(prefix))

        def removesuffix(self, suffix, /):
            if isinstance(suffix, UserString):
                suffix = suffix.data
            return self.__class__(self.data.removesuffix(suffix))

        def encode(self, encoding='utf-8', errors='strict'):
            encoding = 'utf-8' if encoding is None else encoding
            errors = 'strict' if errors is None else errors
            return self.data.encode(encoding, errors)

        def endswith(self, suffix, start=0, end=_sys.maxsize):
            return self.data.endswith(suffix, start, end)

        def expandtabs(self, tabsize=8):
            return self.__class__(self.data.expandtabs(tabsize))

        def find(self, sub, start=0, end=_sys.maxsize):
            if isinstance(sub, UserString):
                sub = sub.data
            return self.data.find(sub, start, end)

        def format(self, /, *args, **kwds):
            return self.data.format(*args, **kwds)

        def format_map(self, mapping):
            return self.data.format_map(mapping)

        def index(self, sub, start=0, end=_sys.maxsize):
            if isinstance(sub, UserString):
                sub = sub.data
            return self.data.index(sub, start, end)

        def isalpha(self):
            return self.data.isalpha()

        def isalnum(self):
            return self.data.isalnum()

        def isascii(self):
            return self.data.isascii()

        def isdecimal(self):
            return self.data.isdecimal()

        def isdigit(self):
            return self.data.isdigit()

        def isidentifier(self):
            return self.data.isidentifier()

        def islower(self):
            return self.data.islower()

        def isnumeric(self):
            return self.data.isnumeric()

        def isprintable(self):
            return self.data.isprintable()

        def isspace(self):
            return self.data.isspace()

        def istitle(self):
            return self.data.istitle()

        def isupper(self):
            return self.data.isupper()

        def join(self, seq):
            return self.data.join(seq)

        def ljust(self, width, *args):
            return self.__class__(self.data.ljust(width, *args))

        def lower(self):
            return self.__class__(self.data.lower())

        def lstrip(self, chars=None):
            return self.__class__(self.data.lstrip(chars))

        maketrans = str.maketrans

        def partition(self, sep):
            return self.data.partition(sep)

        def replace(self, old, new, maxsplit=-1):
            if isinstance(old, UserString):
                old = old.data
            if isinstance(new, UserString):
                new = new.data
            return self.__class__(self.data.replace(old, new, maxsplit))

        def rfind(self, sub, start=0, end=_sys.maxsize):
            if isinstance(sub, UserString):
                sub = sub.data
            return self.data.rfind(sub, start, end)

        def rindex(self, sub, start=0, end=_sys.maxsize):
            if isinstance(sub, UserString):
                sub = sub.data
            return self.data.rindex(sub, start, end)

        def rjust(self, width, *args):
            return self.__class__(self.data.rjust(width, *args))

        def rpartition(self, sep):
            return self.data.rpartition(sep)

        def rstrip(self, chars=None):
            return self.__class__(self.data.rstrip(chars))

        def split(self, sep=None, maxsplit=-1):
            return self.data.split(sep, maxsplit)

        def rsplit(self, sep=None, maxsplit=-1):
            return self.data.rsplit(sep, maxsplit)

        def splitlines(self, keepends=False):
            return self.data.splitlines(keepends)

        def startswith(self, prefix, start=0, end=_sys.maxsize):
            return self.data.startswith(prefix, start, end)

        def strip(self, chars=None):
            return self.__class__(self.data.strip(chars))

        def swapcase(self):
            return self.__class__(self.data.swapcase())

        def title(self):
            return self.__class__(self.data.title())

        def translate(self, *args):
            return self.__class__(self.data.translate(*args))

        def upper(self):
            return self.__class__(self.data.upper())

        def zfill(self, width):
            return self.__class__(self.data.zfill(width))
