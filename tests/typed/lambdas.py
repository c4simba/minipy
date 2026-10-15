# lambdas and functions as values: variables, arguments, containers, results
from typing import Callable
from functools import reduce

def apply(fn: Callable[[int], int], x: int) -> int:
    return fn(x)

def twice(fn, x):
    return fn(fn(x))

def square(n: int) -> int:
    return n * n

def compose(f: Callable[[int], int], g: Callable[[int], int]) -> Callable[[int], int]:
    return lambda x: f(g(x))

inc = lambda x: x + 1
print(inc(41), apply(inc, 1), apply(lambda v: v * 10, 5), twice(square, 3))
add = lambda a, b: a + b
print(add(2, 3), add(-1, 1))
half = lambda x: x / 2
print(half(5.0), half(2.5))

ops = {"+": lambda a, b: a + b, "-": lambda a, b: a - b, "*": lambda a, b: a * b}
for name in sorted(ops):
    print(name, ops[name](7, 3))

fs = [square, inc, lambda n: -n]
print([f(4) for f in fs])
print(compose(square, inc)(4), compose(inc, square)(4))

words = ["pear", "fig", "banana", "kiwi"]
key = lambda w: (len(w), w)
print(sorted(words, key=key))
print(max(words, key=len), min(words, key=lambda w: w[-1]))
print(list(map(lambda w: w.upper(), words)))
print(list(map(str, [1, 2, 3])), list(map(len, words)))
print(list(filter(lambda w: "a" in w, words)), list(filter(None, [0, 3, 0, 5])))
print(list(map(lambda a, b: a * b, [1, 2, 3], [4, 5, 6])))
print(",".join(map(str, range(5))))
print(sum(map(lambda x: x * x, range(4))))

print(reduce(lambda a, b: a * b, [1, 2, 3, 4, 5]), reduce(lambda acc, w: acc + len(w), words, 0))
print(reduce(add, [10, 20, 30]))

class Button:
    def __init__(self, label: str):
        self.label = label
        self.on_click: Callable[[str], str] = None

    def click(self) -> str:
        if self.on_click is None:
            return "nothing"
        return self.on_click(self.label)

b = Button("ok")
print(b.click())
b.on_click = lambda s: "clicked " + s
print(b.click())

def pick(n: int) -> Callable[[float], float]:
    if n == 0:
        return lambda x: x / 2
    return lambda x: x * 2

print(pick(0)(5.0), pick(1)(5.0))
print((lambda: "called")())
log: list[str] = []
remember = lambda s: log.append(s)
remember("a")
remember("b")
nothing = lambda: None
nothing()
print(log, fs[0] is square, fs[1] is not None)

class Scaler:
    def __init__(self, k: int):
        self.k = k

    def apply(self, x: int) -> int:
        return x * self.k

    def total(self, a: int, b: float) -> float:
        return (a + b) * self.k

s3 = Scaler(3)
f3 = s3.apply
print(f3(5), list(map(Scaler(2).apply, [1, 2, 3])), Scaler.apply(s3, 7), s3.total(1, 0.5))
tot = s3.total
unbound = Scaler.apply
print(tot(2, 1.5), unbound(Scaler(10), 4), f3.__name__, inc.__name__, square.__name__)
print(sorted([3, 1, 2], key=Scaler(-1).apply))
callbacks: list[Callable[[int], int]] = [s3.apply, Scaler(5).apply, square]
print([cb(2) for cb in callbacks])
