# default values: evaluated once where the def / lambda is, shared (as CPython)
SEP = "-"


def join(xs: list[str], sep: str = SEP) -> str:
    return sep.join(xs)


def remember(x: int, seen: list[int] = []) -> list[int]:
    seen.append(x)
    return seen


def scale(v: float, k: float = 2.0 * 3) -> float:
    return v * k


print(join(["a", "b"]), join(["a", "b"], "+"))
print(remember(1), remember(2), remember(3, [9]), remember(4))
print(scale(1.5))
adders = [lambda x, i=i: x + i for i in range(3)]
print([f(10) for f in adders], [f(10, 100) for f in adders])
g = lambda *a: len(a)
h = lambda a, *rest, **kw: (a, rest, sorted(kw))
print(g(), g(1, 2, 3), h(1), h(1, 2, 3, z=1, y=2))


class Counter:
    def __init__(self, start: int = len(SEP) * 10, step: int = 1) -> None:
        self.n = start
        self.step = step

    def tick(self, times: int = 1, log: list[int] = []) -> list[int]:
        for _ in range(times):
            self.n += self.step
            log.append(self.n)
        return log


c = Counter()
print(c.tick(), c.tick(2), Counter(step=5).tick(), c.n)


def outer(base: int) -> int:
    def inner(x: int, b: int = base * 2) -> int:
        return x + b
    return inner(1) + inner(1, 0)


print(outer(5), outer(7))
fs = [join, join]
print(fs[0](["x", "y"]), fs[1](["x"], "/"))
