# coroutines as methods, producer/consumer through a shared list
import asyncio

class Counter:
    def __init__(self, name: str):
        self.name = name
        self.value = 0

    async def run(self, steps: int, pause: float) -> int:
        for _ in range(steps):
            self.value += 1
            await asyncio.sleep(pause)
        return self.value

queue: list[int] = []
finished = False

async def producer(n: int):
    global finished
    for i in range(n):
        queue.append(i * i)
        await asyncio.sleep(0.002)
    finished = True

async def consumer() -> int:
    total = 0
    while not finished or len(queue) > 0:
        if len(queue) > 0:
            total += queue.pop(0)
        else:
            await asyncio.sleep(0.001)
    return total

async def main():
    a = Counter("a")
    b = Counter("b")
    ta = asyncio.create_task(a.run(3, 0.01))
    tb = asyncio.create_task(b.run(5, 0.001))
    print(await ta, await tb, a.value, b.value)
    p = asyncio.create_task(producer(10))
    total = await consumer()
    await p
    print("consumed", total)

asyncio.run(main())
print("bye")
