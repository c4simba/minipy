class Shape:
    def area(self) -> float:
        return 0.0

class Circle(Shape):
    def __init__(self, r: float):
        self.r = r
    def area(self) -> float:
        return 3.0 * self.r * self.r
    def __repr__(self) -> str:
        return f"Circle({self.r})"

class Rect(Shape):
    def __init__(self, w: float, h: float):
        self.w = w
        self.h = h
    def __str__(self) -> str:
        return f"{self.w}x{self.h}"

class Square(Rect):
    def __init__(self, s: float):
        super().__init__(s, s)
    def __repr__(self) -> str:
        return f"Square({self.w})"

shapes: list[Shape] = [Circle(1.0), Rect(2.0, 3.0), Square(4.0), Shape()]
for s in shapes:
    if isinstance(s, Circle):
        print("circle radius", s.r)
    elif isinstance(s, Rect) and s.area() > 0:
        print("rect", s.w, s.h)
    else:
        print("plain", s.area())

def widths(items: list[Shape]) -> list[float]:
    out: list[float] = []
    for it in items:
        if not isinstance(it, Rect):
            continue
        out.append(it.w)
    return out
print(widths(shapes))

def describe(s: Shape) -> str:
    if not isinstance(s, Circle):
        return "not a circle"
    return f"r={s.r}"
print([describe(x) for x in shapes])
print([s for s in shapes if not type(s) is Shape and not type(s) is Rect])
print([str(x) for x in shapes][1:3])
class MyError(Exception):
    def __repr__(self) -> str:
        return "MyError!"
e = MyError("boom")
print(str(e), repr(e), [e])
