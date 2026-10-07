# async/await on the built-in event loop (asyncio.run, sleep, create_task, gather)
import asyncio

async def fetch(name: str, delay: float, value: int) -> int:
    print("start", name)
    await asyncio.sleep(delay)
    print("done", name)
    return value * 2

async def ticker(n: int):
    for i in range(n):
        print("tick", i)
        await asyncio.sleep(0.01)

async def add(a: int, b: int) -> int:
    await asyncio.sleep(0)
    return a + b

async def main() -> int:
    total = await add(1, 2)
    print("add", total)
    t1 = asyncio.create_task(fetch("slow", 0.05, 10))
    t2 = asyncio.create_task(fetch("fast", 0.01, 20))
    print("tasks created", t1.done(), t2.done())
    r2 = await t2
    r1 = await t1
    print("results", r1, r2, t1.done(), t1.result())
    results = await asyncio.gather(fetch("a", 0.03, 1), fetch("b", 0.02, 2), fetch("c", 0.01, 3))
    print("gather", results)
    await asyncio.gather(ticker(2), ticker(2))
    names = await asyncio.gather(greet("x"), greet("y"))
    print(names)
    return total + r1 + r2

async def greet(who: str) -> str:
    await asyncio.sleep(0)
    return "hi " + who

print("result", asyncio.run(main()))
