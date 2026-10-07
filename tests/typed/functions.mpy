# functions: defaults, keywords, recursion, globals, early returns, containers in and out
def add(a: int, b: int = 10) -> int:
    return a + b

def scale(v: float, k: float = 2) -> float:
    return v * k

def fib(n: int) -> int:
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

def fact(n):
    r = 1
    for i in range(2, n + 1):
        r *= i
    return r

def first_negative(xs: list[int]) -> int:
    for i, x in enumerate(xs):
        if x < 0:
            return i
    return -1

def count_words(text: str) -> dict[str, int]:
    out: dict[str, int] = {}
    for w in text.split():
        out[w] = out.get(w, 0) + 1
    return out

def evens(limit: int) -> list[int]:
    return [i for i in range(limit) if i % 2 == 0]

total = 0
calls = 0

def bump(by: int):
    global total, calls
    total += by
    calls = calls + 1

def greet(name: str, greeting: str = "Hello", punct: str = "!") -> str:
    return greeting + ", " + name + punct

def describe(x: int) -> str:
    if x < 0:
        return "negative"
    elif x == 0:
        return "zero"
    elif x < 10:
        return "small"
    return "large"

def swap_ends(xs: list[str]):
    xs[0], xs[-1] = xs[-1], xs[0]

def nothing():
    pass

def maybe(flag: bool) -> str:
    if flag:
        return "set"
    return None

print(add(1), add(1, 2), add(b=5, a=1))
print(scale(1.5), scale(2, 0.5), scale(k=3, v=1.0))
print(fib(20), fact(10), fact(0))
print(first_negative([3, 4, -1, 5]), first_negative([]))
wc = count_words("a b a c a b")
print(wc, len(wc))
print(evens(9), sum(evens(100)))
for i in range(5):
    bump(i)
print(total, calls)
print(greet("Bob"), greet("Ann", "Hi"), greet("Joe", punct="?"))
for v in [-5, 0, 3, 42]:
    print(v, describe(v))
names = ["a", "b", "c"]
swap_ends(names)
print(names)
nothing()
print(maybe(True), maybe(False) is None, maybe(True) is not None)

def collatz(n: int) -> int:
    steps = 0
    while n != 1:
        if n % 2 == 0:
            n //= 2
        else:
            n = 3 * n + 1
        steps += 1
    return steps

print([collatz(n) for n in range(1, 11)])

def gcd(a: int, b: int) -> int:
    while b:
        a, b = b, a % b
    return a

print(gcd(48, 18), gcd(17, 5), gcd(100, 75))

def mean(xs: list[float]) -> float:
    if not xs:
        return 0.0
    return sum(xs) / len(xs)

print(mean([1.0, 2.0, 4.0]), mean([]))

def primes(n: int) -> list[int]:
    sieve = [True] * (n + 1)
    sieve[0] = False
    sieve[1] = False
    i = 2
    while i * i <= n:
        if sieve[i]:
            for j in range(i * i, n + 1, i):
                sieve[j] = False
        i += 1
    return [k for k in range(n + 1) if sieve[k]]

print(primes(50))
