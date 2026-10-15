import asyncio
from typing import AsyncIterator

async def ticker(n: int) -> AsyncIterator[int]:
    for i in range(n):
        await asyncio.sleep(0)
        yield i * 10

class Resource:
    def __init__(self, name: str):
        self.name = name
    async def __aenter__(self) -> "Resource":
        print("open", self.name)
        return self
    async def __aexit__(self, et, ev, tb) -> None:
        print("close", self.name)

async def main() -> None:
    async for v in ticker(3):
        print("tick", v)
    async with Resource("db") as r:
        print("using", r.name)
    xs = [v async for v in ticker(4) if v > 0]
    print(xs)
    g = ticker(2)
    print(await g.__anext__(), await anext(g))

asyncio.run(main())
