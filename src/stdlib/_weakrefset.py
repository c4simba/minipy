"""WeakSet (CPython's _weakrefset): a set of objects (here: ordinary references to them)."""

__all__ = ["WeakSet"]


class WeakSet:
    def __init__(self, data=None):
        self.data = set()
        if data is not None:
            self.update(data)

    def __iter__(self):
        return iter(list(self.data))

    def __len__(self):
        return len(self.data)

    def __contains__(self, item):
        try:
            return item in self.data
        except TypeError:
            return False

    def __reduce__(self):
        return (self.__class__, (list(self),), self.__getstate__() if hasattr(self, "__getstate__") else None)

    def add(self, item):
        self.data.add(item)

    def clear(self):
        self.data.clear()

    def copy(self):
        return self.__class__(self)

    def pop(self):
        return self.data.pop()

    def remove(self, item):
        self.data.remove(item)

    def discard(self, item):
        self.data.discard(item)

    def update(self, other):
        for element in other:
            self.add(element)

    def __ior__(self, other):
        self.update(other)
        return self

    def difference(self, other):
        newset = self.copy()
        newset.difference_update(other)
        return newset

    __sub__ = difference

    def difference_update(self, other):
        self.__isub__(other)

    def __isub__(self, other):
        if self is other:
            self.data.clear()
        else:
            self.data.difference_update(other)
        return self

    def intersection(self, other):
        return self.__class__(item for item in other if item in self)

    __and__ = intersection

    def intersection_update(self, other):
        self.__iand__(other)

    def __iand__(self, other):
        self.data.intersection_update(other)
        return self

    def issubset(self, other):
        return self.data.issubset(other)

    __le__ = issubset

    def __lt__(self, other):
        return self.data < set(other)

    def issuperset(self, other):
        return self.data.issuperset(other)

    __ge__ = issuperset

    def __gt__(self, other):
        return self.data > set(other)

    def __eq__(self, other):
        if not isinstance(other, self.__class__):
            return NotImplemented
        return self.data == other.data

    def symmetric_difference(self, other):
        newset = self.copy()
        newset.symmetric_difference_update(other)
        return newset

    __xor__ = symmetric_difference

    def symmetric_difference_update(self, other):
        self.__ixor__(other)

    def __ixor__(self, other):
        if self is other:
            self.data.clear()
        else:
            self.data.symmetric_difference_update(other)
        return self

    def union(self, other):
        return self.__class__(e for s in (self, other) for e in s)

    __or__ = union

    def isdisjoint(self, other):
        return len(self.intersection(other)) == 0

    def __repr__(self):
        return repr(self.data)
