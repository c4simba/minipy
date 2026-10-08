# generators, coroutines, async (compiled by CPython for now), comprehensions scoping
import asyncio, itertools
def gen(n):
    for i in range(n):
        x = yield i
        if x: print("sent", x)
    return "done"
g = gen(3); print(next(g), g.send("hi"), next(g))
try:
    next(g)
except StopIteration as e:
    print("stop", e.value)
def deleg(): r = yield from gen(2); yield r
print(list(deleg()), sum(x * x for x in range(10)), list(itertools.islice(itertools.count(), 3)))
async def work(n, delay):
    await asyncio.sleep(delay)
    return n * 10
async def main():
    rs = await asyncio.gather(*(work(i, 0.01 * (3 - i)) for i in range(3)))
    async def agen():
        for i in range(3):
            yield i
    return rs, [i async for i in agen()]
print(asyncio.run(main()))
x = "outer"
print([x for x in range(3)], x)
try:
    class K:
        v = 5
        items = [v * i for i in range(3)]       # the class's names are not visible in a comprehension
except NameError as e:
    print("NameError:", e)
