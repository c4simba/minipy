# frozenset, and None narrowing (if x is None: ... else: x is not None)
a = frozenset([3, 1, 2])
b = frozenset()
c = frozenset("hello")
print(a, b, len(c), 2 in a, a == {1, 2, 3}, type(a).__name__, isinstance(a, frozenset), isinstance(a, set), isinstance({1}, frozenset))
print(sorted(a), [a, frozenset([5])], a | {9}, a & {1, 7})
s = {1, 2}
print(s, type(s).__name__)

from typing import TypeVar

T = TypeVar("T")


def kind(x: T) -> str:
    if isinstance(x, str):
        return "str " + x
    else:
        return "other"


def f(a: str | None = None) -> str:
    if a is None:
        return "none"
    return kind(a)


def g(a: str | None = None) -> str:
    if a is not None:
        return kind(a)
    else:
        return "nothing"


def h(n: int | None) -> int:
    if n is None:
        raise ValueError("no")
    return n + 1


print(f(), f("x"), g(), g("y"), h(4))
