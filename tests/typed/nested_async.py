import asyncio
from typing import Callable

def greet(name: str, *, greeting: str = "Hello", punct: str = "!") -> str:
    return f"{greeting}, {name}{punct}"

f: Callable[..., str] = greet
print(greet("a", greeting="Hi"), f("b", punct="?"))

def apply(fn: Callable[[int, int], int], a: int, b: int) -> int:
    return fn(a, b)
print(apply(lambda a, b: a - b, 10, 3))

async def outer(n: int) -> int:
    async def inner(x: int) -> int:
        await asyncio.sleep(0)
        return x * 2
    total = 0
    for i in range(n):
        total += await inner(i)
    return total
print(asyncio.run(outer(4)))

class Dog:
    def __init__(self, name: str):
        self.name = name
    def speak(self) -> str:
        return self.name + " woofs"

factory = Dog
d = factory("rex")
print(d.speak())
makers = {"dog": Dog}
print(makers["dog"]("fido").speak())
