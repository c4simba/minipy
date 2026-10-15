# type aliases, type parameters, TypeVar, Generic
from typing import TypeVar, Generic, Optional

type Vec = list[float]
type Pair[T] = tuple[T, T]
Matrix = list[list[int]]
Name: "TypeAlias" = str
U = TypeVar("U")


def norm(v: Vec) -> float:
    return sum(x * x for x in v) ** 0.5


def first[T](xs: list[T]) -> T:
    return xs[0]


def swap(p: Pair[int]) -> Pair[int]:
    return (p[1], p[0])


def last(xs: list[U]) -> U:
    return xs[-1]


class Box[T]:
    def __init__(self, item: T) -> None:
        self.item = item

    def get(self) -> T:
        return self.item


class Stack(Generic[U]):
    def __init__(self) -> None:
        self.items: list[U] = []

    def push(self, x: U) -> None:
        self.items.append(x)

    def pop(self) -> Optional[U]:
        return self.items.pop() if self.items else None


m: Matrix = [[1, 2], [3, 4]]
print(norm([3.0, 4.0]), first([5, 6]), swap((1, 2)), last(["a", "b"]), m)
b = Box(42)
print(b.get())
s = Stack[str]() if False else Stack()
s.push("x")
s.push("y")
print(s.pop(), s.pop(), s.pop())
