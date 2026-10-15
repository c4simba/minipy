class Tree:
    class Node:
        kind = "node"
        def __init__(self, value: int):
            self.value = value
            self.children: list[Tree.Node] = []

        def add(self, v: int) -> "Tree.Node":
            n = Tree.Node(v)
            self.children.append(n)
            return n

    def __init__(self):
        self.root = Tree.Node(0)

    def total(self) -> int:
        def walk(n: Tree.Node) -> int:
            return n.value + sum(walk(c) for c in n.children)
        return walk(self.root)

t = Tree()
a = t.root.add(1)
a.add(5)
t.root.add(2)
print(t.total(), Tree.Node.kind, Tree.Node.__name__, Tree.Node.__qualname__)
print(Tree.Node)

def make_counter(start: int):
    class Counter:
        base = start * 10
        def __init__(self):
            self.n = Counter.base
        def step(self) -> int:
            self.n += 1
            return self.n
    return Counter()

c = make_counter(3)
print(c.step(), c.step())
d = make_counter(5)
print(d.step(), c.step())

def shapes() -> list[str]:
    class Shape:
        def area(self) -> float:
            return 0.0
        def name(self) -> str:
            return type(self).__name__
    class Sq(Shape):
        def __init__(self, s: float):
            self.s = s
        def area(self) -> float:
            return self.s * self.s
    class Circle(Shape):
        def __init__(self, r: float):
            self.r = r
        def area(self) -> float:
            return 3.0 * self.r * self.r
    items: list[Shape] = [Sq(2.0), Circle(1.0), Shape()]
    return [f"{x.name()}={x.area()}" for x in items]

print(shapes())
print(type(c).__qualname__)
