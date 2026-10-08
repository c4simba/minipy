# functions: defaults, keyword-only, positional-only, *args/**kwargs, closures, decorators, recursion
import functools, inspect

def f(a, b=2, /, c=3, *args, d, e=5, **kw):
    """doc of f"""
    return (a, b, c, args, d, e, kw)
print(f(1, d=4), f(1, 2, 3, 4, 5, d=6, z=7))
print(inspect.signature(f), f.__doc__, f.__name__, f.__qualname__, f.__defaults__, f.__kwdefaults__)
try:
    f(1, b=2, d=3)
except TypeError as ex:
    print("TypeError:", ex)

def counter(start=0):
    n = start
    def inc(by=1):
        nonlocal n
        n += by
        return n
    def get():
        return n
    return inc, get
inc, get = counter(10); inc(); inc(5); print(get())

def deco(tag):
    def wrap(fn):
        @functools.wraps(fn)
        def inner(*a, **k):
            return f"<{tag}>{fn(*a, **k)}</{tag}>"
        return inner
    return wrap
@deco("b")
@deco("i")
def hello(name):
    "says hello"
    return "hello " + name
print(hello("bob"), hello.__name__, hello.__doc__, hello.__wrapped__.__wrapped__.__name__)

@functools.lru_cache(maxsize=None)
def fib(n):
    return n if n < 2 else fib(n - 1) + fib(n - 2)
print(fib(80), fib.cache_info().hits > 0)

def gen_closures():
    return [lambda: i for i in range(3)], [lambda i=i: i for i in range(3)]
late, early = gen_closures(); print([c() for c in late], [c() for c in early])

x = "global"
def scopes():
    x = "enclosing"
    def inner():
        global x
        x = "changed"
        return x
    return inner(), x
print(scopes(), x)
def outer():
    v = 1
    def mid():
        def inner():
            return v
        return inner()
    return mid()
print(outer(), (lambda: (lambda: 42)())(), len(f.__code__.co_varnames) > 0)
def kwonly(*, a, b=2): return a + b
print(kwonly(a=1), kwonly(a=1, b=5), functools.partial(kwonly, b=10)(a=1))
def rec(n): return 1 if n == 0 else n * rec(n - 1)
print(rec(20), sum(map(lambda q: q * q, filter(None, [0, 1, 2, 3]))))
def ann(a: int, b: "str" = "x") -> list[int]: pass
print(ann.__annotations__, inspect.get_annotations(ann))
