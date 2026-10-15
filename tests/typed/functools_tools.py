import functools
from functools import lru_cache, cache, partial, total_ordering

calls = 0

@lru_cache(maxsize=None)
def fib(n: int) -> int:
    global calls
    calls += 1
    return n if n < 2 else fib(n - 1) + fib(n - 2)

print(fib(40), calls)

@cache
def square(x: int) -> int:
    print("computing", x)
    return x * x

print(square(4), square(4), square(5))

@functools.lru_cache(maxsize=2)
def slow(x: str) -> str:
    print("slow", x)
    return x.upper()

for s in ["a", "b", "a", "c", "b", "a"]:
    slow(s)

def power(base: int, exp: int) -> int:
    return base ** exp

sq = partial(power, exp=2)
two_to = partial(power, 2)
print(sq(7), two_to(10), list(map(partial(power, exp=3), [1, 2, 3])))
add = functools.partial(lambda a, b: a + b, 100)
print(add(5))

@total_ordering
class Version:
    def __init__(self, major: int, minor: int):
        self.major = major
        self.minor = minor
    def __eq__(self, other: "Version") -> bool:
        return (self.major, self.minor) == (other.major, other.minor)
    def __lt__(self, other: "Version") -> bool:
        return (self.major, self.minor) < (other.major, other.minor)

a = Version(1, 2); b = Version(1, 3)
print(a < b, a <= b, a > b, a >= b, b >= a, a <= Version(1, 2), a != b)
