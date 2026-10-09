from typing import Generator

def numbers() -> Generator[int, None, None]:
    try:
        yield 1
        yield 2
        yield 3
    finally:
        print("numbers cleanup")

for n in numbers():
    if n == 2:
        break
print("after loop")

def first(xs: list[int]) -> int:
    g = numbers()
    for x in g:
        return x * 10
    return -1
print(first([1]))

def averager() -> Generator[float, float, None]:
    total = 0.0
    count = 0
    avg = 0.0
    while True:
        value = yield avg
        total += value
        count += 1
        avg = total / count

a = averager()
next(a)
print(a.send(10), a.send(20), a.send(60))

def catcher() -> Generator[str, None, None]:
    while True:
        try:
            yield "waiting"
        except ValueError as e:
            print("caught", e)
            yield "recovered"

c = catcher()
print(next(c))
print(c.throw(ValueError("bad")))
print(next(c))
c.close()

def stubborn() -> Generator[int, None, None]:
    try:
        yield 1
    except GeneratorExit:
        yield 2

s = stubborn()
next(s)
try:
    s.close()
except RuntimeError as e:
    print("RuntimeError:", e)

def echo() -> Generator[str, str, None]:
    while True:
        got = yield "ready"
        print("echo got", got)

def relay() -> Generator[str, str, None]:
    yield from echo()

r = relay()
print(next(r))
print(r.send("hello"))
print(r.send("world"))

def fresh() -> Generator[int, int, None]:
    x = yield 1
    print(x)
f = fresh()
try:
    f.send(5)
except TypeError as e:
    print("TypeError:", e)
g2 = numbers()
g2.close()
print(list(g2))
