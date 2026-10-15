# nested functions and closures: captured values, shared cells, nonlocal, recursion

def make_adder(n: int):
    def add(x: int) -> int:
        return x + n
    return add

def make_counter():
    count = 0
    def step() -> int:
        nonlocal count
        count += 1
        return count
    return step

def make_account(balance: float):
    history: list[str] = []
    def deposit(amount: float) -> float:
        nonlocal balance
        balance += amount
        history.append("+" + str(amount))
        return balance
    def report() -> str:
        return ", ".join(history) + " = " + str(balance)
    deposit(10.0)
    deposit(2.5)
    return report

add5 = make_adder(5)
add10 = make_adder(10)
print(add5(1), add10(1), make_adder(-1)(1))
c1 = make_counter()
c2 = make_counter()
print(c1(), c1(), c1(), c2())
print(make_account(100.0)())

def outer(xs: list[int]) -> list[int]:
    total = 0
    def visit(i: int) -> None:
        nonlocal total
        total += xs[i]
    for i in range(len(xs)):
        visit(i)
    def scaled(k: int) -> list[int]:
        return [x * k + total for x in xs]
    return scaled(2)

print(outer([1, 2, 3]))

def fib_memo(n: int) -> int:
    memo: dict[int, int] = {}
    def fib(k: int) -> int:
        if k < 2:
            return k
        if k in memo:
            return memo[k]
        r = fib(k - 1) + fib(k - 2)
        memo[k] = r
        return r
    return fib(n)

print(fib_memo(30))

def late_binding() -> list[int]:
    fs = []
    for i in range(3):
        fs.append(lambda: i * 10)
    return [f() for f in fs]

def early_binding() -> list[int]:
    fs = []
    for i in range(3):
        fs.append(make_adder(i))
    return [f(100) for f in fs]

print(late_binding(), early_binding())

def assigned_later() -> int:
    get = lambda: value
    value = 7
    return get()

print(assigned_later())

def deep(a: int):
    def middle(b: int):
        def inner(c: int) -> int:
            return a * 100 + b * 10 + c
        return inner
    return middle

print(deep(1)(2)(3))

class Greeter:
    def __init__(self, greeting: str):
        self.greeting = greeting

    def greeters(self, names: list[str]) -> list[str]:
        def one(name: str) -> str:
            return self.greeting + ", " + name
        return [one(n) for n in names]

    def prefixer(self):
        return lambda s: self.greeting + s

g = Greeter("hi")
print(g.greeters(["ann", "bob"]), g.prefixer()("!"))

def apply_all(fs: list, x: int) -> list[int]:
    return [f(x) for f in fs]

print(apply_all([make_adder(1), make_adder(2), lambda v: v * v], 7))

def counter_pair():
    n = 0
    def inc() -> None:
        nonlocal n
        n += 1
    def get() -> int:
        return n
    return inc, get

inc, get = counter_pair()
inc()
inc()
print(get())
