# a method with TypeVar-annotated parameters: a copy of it per kind of argument (compiled code)
from typing import TypeVar

T = TypeVar("T")


class Box:
    def __init__(self, name: str):
        self.name = name

    def show(self, x: T) -> str:
        return self.name + ":" + str(x)

    def pair(self, a: T, b: int) -> tuple[T, int]:
        return (a, b + 1)


b = Box("b")
print(b.show(1), b.show("s"), b.show(2.5), b.show([1, 2]), b.show(3))
print(b.pair("x", 1), b.pair(1.5, 2))
