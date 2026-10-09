import asyncio
from typing import AsyncIterator

async def counter(name: str, n: int) -> AsyncIterator[str]:
    try:
        for i in range(n):
            await asyncio.sleep(0)
            yield f"{name}{i}"
    finally:
        print("cleanup", name)

async def consume(name: str, n: int) -> int:
    count = 0
    async for item in counter(name, n):
        print("got", item)
        count += 1
    return count

class Countdown:
    def __init__(self, start: int):
        self.n = start
    def __aiter__(self) -> "Countdown":
        return self
    async def __anext__(self) -> int:
        if self.n <= 0:
            raise StopAsyncIteration
        await asyncio.sleep(0)
        self.n -= 1
        return self.n

class Guard:
    async def __aenter__(self) -> "Guard":
        print("enter")
        return self
    async def __aexit__(self, et, ev, tb) -> bool:
        print("exit", et is not None)
        return True

async def main() -> None:
    results = await asyncio.gather(consume("a", 2), consume("b", 3))
    print(results)
    async for v in Countdown(3):
        print("countdown", v)
    else:
        print("countdown done")
    async with Guard():
        raise ValueError("ignored")
    print("after guard")
    g = counter("z", 5)
    print(await anext(g))
    await g.aclose()
    print(await anext(g, "default"))
    total = sum([len(s) async for s in counter("q", 3)])
    print("total", total)

asyncio.run(main())
