def ident(x):
    return x
def pair(a, b):
    return (a, b)
def first[T](xs: list[T]) -> T:
    return xs[0]
def apply(f, v):
    return f(v)
print(ident(1), ident("a"), ident([1, 2]))
print(pair(1, "x"), pair("y", 2.5))
print(first([1, 2]), first(["a"]))
print(apply(abs, -5), apply(len, "abc"))
class Box[T]:
    def __init__(self, v: T):
        self.v = v
    def get(self) -> T:
        return self.v
b1 = Box(1); b2 = Box("s")
print(b1.get() + 1, b2.get() + "!")
b3: Box[float] = Box(2.5)
print(b3.get() * 2, isinstance(b2, Box), isinstance(b3, Box), type(b2).__name__)
def show(b: Box) -> str:
    return "Box(" + str(b.get()) + ")"
print(show(b1))
class Pair:
    def __init__(self, a, b):
        self.a = a
        self.b = b
    def swap(self):
        return Pair(self.b, self.a)
p1 = Pair(1, "x"); p2 = p1.swap()
print(p1.a, p1.b, p2.a, p2.b)
def maxof(xs):
    best = xs[0]
    for x in xs:
        if x > best:
            best = x
    return best
print(maxof([3, 9, 2]), maxof(["b", "z", "a"]), maxof([1.5, 0.5]))
