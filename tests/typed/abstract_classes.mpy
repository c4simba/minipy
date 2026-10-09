from abc import ABC, abstractmethod
import abc

class Shape(ABC):
    @abstractmethod
    def area(self) -> float: ...
    @abstractmethod
    def name(self) -> str:
        return "shape"
    def describe(self) -> str:
        return f"{self.name()} with area {self.area()}"

class Square(Shape):
    def __init__(self, s: float):
        self.s = s
    def area(self) -> float:
        return self.s * self.s
    def name(self) -> str:
        return "square(" + super().name() + ")"

class Half(Shape):
    def area(self) -> float:
        return 0.5

class Base(abc.ABC):
    @abc.abstractmethod
    def run(self) -> int: ...

class Impl(Base):
    def run(self) -> int:
        return 7

print(Square(3.0).describe(), Impl().run())
try:
    Shape()
except TypeError as e:
    print(e)
try:
    Half()
except TypeError as e:
    print(e)
shapes: list[Shape] = [Square(1.0), Square(2.0)]
print(sum(s.area() for s in shapes), isinstance(shapes[0], ABC))
