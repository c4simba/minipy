# generators: yield, yield from, generator expressions, next(), iter(), lazy any/all
from typing import Iterator

def countdown(n: int) -> Iterator[int]:
    while n > 0:
        yield n
        n -= 1

def fib():
    a, b = 0, 1
    while True:
        yield a
        a, b = b, a + b

def take(g: Iterator[int], n: int) -> list[int]:
    out: list[int] = []
    for x in g:
        if len(out) == n:
            break
        out.append(x)
    return out

def words(text: str):
    for w in text.split():
        if w:
            yield w.strip(".,").lower()

def chain(*parts: list[int]):
    for p in parts:
        yield from p

def evens(limit: int):
    yield from range(0, limit, 2)

def pairs(xs: list[str]):
    for i, x in enumerate(xs):
        yield i, x

def squares_of(g: Iterator[int]):
    for v in g:
        yield v * v

print(list(countdown(5)), sum(countdown(100)))
print(take(fib(), 12))
print(list(words("The quick, brown fox. The end.")))
print(list(chain([1, 2], [3], [], [4, 5])), list(evens(9)))
for i, w in pairs(["a", "b", "c"]):
    print(i, w, end="; ")
print()
print(list(squares_of(countdown(4))), max(squares_of(iter([3, -7, 2]))))

g = countdown(3)
print(next(g), next(g), next(g), next(g, -1), next(g, -1))
it = iter(["x", "y"])
print(next(it), next(it), next(it, "done"))

squares = (x * x for x in range(6))
print("gen", list(squares), list(squares))
total = sum(x for x in range(10) if x % 3 == 0)
print(total, sorted(w for w in words("b a c")), ",".join(str(x) for x in countdown(3)))
first_big = next((x for x in fib() if x > 1000), 0)
print(first_big)

calls: list[int] = []
def probe(x: int) -> bool:
    calls.append(x)
    return x > 2
print(any(probe(x) for x in range(10)), calls)
calls.clear()
print(all(probe(x) for x in [5, 6, 1, 7]), calls)
print(any(x > 3 for x in countdown(5)), all(x > 3 for x in countdown(5)))

def gen_with_cleanup(path: str):
    lines = path.split("/")
    for line in lines:
        yield line.upper()

for part in gen_with_cleanup("usr/local/bin"):
    if part == "LOCAL":
        break
    print(part)

class Tree:
    def __init__(self, value: int):
        self.value = value
        self.kids: list[Tree] = []

    def add(self, value: int) -> "Tree":
        t = Tree(value)
        self.kids.append(t)
        return t

    def walk(self):
        yield self.value
        for k in self.kids:
            yield from k.walk()

    def __iter__(self):
        return self.walk()

root = Tree(1)
a = root.add(2)
a.add(3)
a.add(4)
root.add(5).add(6)
print(list(root.walk()), [v * 10 for v in root])
for depth, v in enumerate(root):
    print(depth, v, end=" ")
print()

def safe_div(xs: list[int]):
    for x in xs:
        yield 100 // x

try:
    for v in safe_div([5, 2, 0, 4]):
        print(v)
except ZeroDivisionError as e:
    print("caught", e)

def numbered(xs: list[float]):
    n = 0
    for x in xs:
        n += 1
        yield str(n) + ": " + str(x)

print(list(numbered([1.5, 2.25])))
print([(a, b) for a, b in zip(countdown(3), "xyz")])
