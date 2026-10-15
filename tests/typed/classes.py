# classes as structs, inheritance, virtual methods, super()
class Shape:
    name: str = "shape"

    def __init__(self, x: int, y: int):
        self.x = x
        self.y = y

    def area(self) -> float:
        return 0.0

    def describe(self) -> str:
        return self.name + " at " + str(self.x) + "," + str(self.y) + " area " + str(self.area())

class Rect(Shape):
    def __init__(self, x: int, y: int, w: int, h: int):
        super().__init__(x, y)
        self.name = "rect"
        self.w = w
        self.h = h

    def area(self) -> float:
        return float(self.w * self.h)

class Circle(Shape):
    def __init__(self, x: int, y: int, r: float):
        super().__init__(x, y)
        self.name = "circle"
        self.r = r

    def area(self) -> float:
        return 3.0 * self.r * self.r

    def __str__(self) -> str:
        return "Circle(" + str(self.r) + ")"

shapes: list[Shape] = [Rect(0, 0, 2, 3), Circle(1, 1, 2.0), Shape(5, 5)]
for s in shapes:
    print(s.describe())
total = 0.0
for s in shapes:
    total += s.area()
print(total)
print(Circle(0, 0, 1.5))
print(isinstance(shapes[0], Rect), isinstance(shapes[1], Rect), isinstance(shapes[1], Shape))

class Node:
    def __init__(self, value: int):
        self.value = value
        self.next: Node = None

class Stack:
    def __init__(self):
        self.top: Node = None
        self.size = 0

    def push(self, v: int):
        n = Node(v)
        n.next = self.top
        self.top = n
        self.size += 1

    def pop(self) -> int:
        n = self.top
        self.top = n.next
        self.size -= 1
        return n.value

    def __len__(self) -> int:
        return self.size

st = Stack()
for i in range(5):
    st.push(i * 10)
print(len(st), st.pop(), st.pop(), len(st))
while st.top is not None:
    print("left", st.pop())

class Counter:
    count: int = 0

    @staticmethod
    def make() -> "Counter":
        return Counter()

    def inc(self, by: int = 1) -> "Counter":
        self.count += by
        return self

c = Counter.make()
c.inc().inc(5).inc(by=10)
print(c.count)
