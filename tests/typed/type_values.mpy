# classes and built-in types as values: type(), isinstance(), issubclass()
from typing import Optional


class Animal:
    def __init__(self, name: str) -> None:
        self.name = name


class Dog(Animal):
    pass


def kind(x: Animal) -> str:
    return type(x).__name__


a = Animal("a")
d = Dog("d")
print(kind(a), kind(d), type(d) is Dog, type(d) == Animal, type(a) is Animal, d.__class__.__name__)
print(type(3), type("s"), type(2.5), type(True), type([1]), type((1, "x")), type({1: 2}), type({3}), type(b"x"), type(None))
print(type(d), Dog, Animal.__name__, int, str.__name__)
print(isinstance(d, Animal), isinstance(a, Dog), isinstance(d, (int, Dog)), isinstance(3, int), isinstance(True, int), isinstance(3, (str, float)), isinstance("x", str), isinstance([1], list), isinstance(d, object))
print(issubclass(Dog, Animal), issubclass(Animal, Dog), issubclass(bool, int), issubclass(int, (str, object)), issubclass(Dog, Dog))
n: Optional[int] = None
print(isinstance(n, int), type(n))
n = 4
print(isinstance(n, int), type(n))
kinds = {int: "an int", str: "a str"}
print(kinds[type(5)], kinds[type("x")], int in kinds, float in kinds)
t = type(d)
print(t, t.__name__, t is Dog)
try:
    raise KeyError("k")
except LookupError as e:
    print(type(e).__name__, type(e), isinstance(e, KeyError), isinstance(e, IndexError))
