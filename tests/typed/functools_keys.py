# functools.cmp_to_key and cached_property; sort keys that are objects (__lt__);
# a generic class naming itself (Node[T]) and calling a function-valued field
import functools
from functools import cached_property, cmp_to_key
from typing import Callable, Generic, TypeVar

T = TypeVar("T")


def by_mod3(a: int, b: int) -> int:
    return (a % 3) - (b % 3) or a - b


print(sorted([5, 1, 4, 3, 9, 2], key=cmp_to_key(by_mod3)))
words = ["bb", "a", "ccc", "dd"]
words.sort(key=cmp_to_key(lambda x, y: len(y) - len(x)))
print(words)
pts = [(1, 2), (0, 5), (1, 1)]
print(sorted(pts, key=functools.cmp_to_key(lambda a, b: a[0] - b[0] or b[1] - a[1])))
print(max(["aa", "b", "ccc"], key=cmp_to_key(lambda x, y: len(x) - len(y))), min([3, 1, 2], key=cmp_to_key(by_mod3)))
print(sorted([5, 1, 4], key=cmp_to_key(by_mod3), reverse=True))


class Circle:
    def __init__(self, r: float):
        self.r = r
        self.calls = 0

    @cached_property
    def area(self) -> float:
        self.calls += 1
        return 3.0 * self.r * self.r

    @functools.cached_property
    def label(self):
        self.calls += 10
        return "circle " + str(self.r)


c = Circle(2.0)
print(c.area, c.area, c.calls, c.label, c.label, c.calls)
print(Circle(1.0).area, c.calls)


class Version:
    def __init__(self, major: int, minor: int):
        self.major = major
        self.minor = minor

    def __lt__(self, other: "Version") -> bool:
        return (self.major, self.minor) < (other.major, other.minor)

    def __repr__(self) -> str:
        return f"v{self.major}.{self.minor}"


releases = [("b", Version(1, 10)), ("a", Version(1, 2)), ("c", Version(0, 9))]
print(sorted(releases, key=lambda r: r[1]))
print([n for n, _ in sorted(releases, key=lambda r: r[1], reverse=True)])


class Node(Generic[T]):
    def __init__(self, value: T, rank: Callable[[T], int]):
        self.value = value
        self.rank = rank

    def before(self, other: "Node[T]") -> bool:
        return self.rank(self.value) < other.rank(other.value)


n1 = Node(3, lambda x: -x)
n2 = Node(5, lambda x: -x)
s1 = Node("abc", len)
s2 = Node("z", len)
print(n1.before(n2), s1.before(s2), s2.rank(s2.value), n1.rank(10))
