from typing import Generator

def acc() -> Generator[int, int, str]:
    total = 0
    while True:
        x = yield total
        if x < 0:
            return "done " + str(total)
        total += x

g = acc()
print(next(g))
print(g.send(5), g.send(7))
try:
    g.send(-1)
except StopIteration as e:
    print("stop:", e.value)

def inner() -> Generator[int, None, int]:
    yield 1
    yield 2
    return 10

def outer() -> Generator[int, None, None]:
    r = yield from inner()
    print("inner returned", r)
    yield r + 1

print(list(outer()))

def counter(n: int) -> Generator[int, None, None]:
    try:
        for i in range(n):
            yield i
    finally:
        print("closed")

c = counter(5)
print(next(c), next(c))
c.close()
print("after close")
