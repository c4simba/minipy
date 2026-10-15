# interpreter features: multi-argument print, bytes, soft keywords, str
# methods, lazy conditional expressions, sort keys, dict order, float repr,
# typing, annotated attributes, async functions, exception classes - the
# output is CPython's
from typing import Callable, Optional

print("a", 1, [2, 3], sep=" | ", end=" ;\n")
print()
print((b"bytes" + b"!").decode(), "café".encode().decode(), len(b"abc"))

def match(x):
    return x + 1
case = match(1)
print("soft keywords", case)

print("/a/b/c".rfind("/"), "ab".center(6, "*"), "7".zfill(3), "k=v".partition("="), "x\ny".splitlines())
print("Ab1".isalnum(), "ABC".isupper(), "abc".islower())

calls: list[str] = []
def f(tag):
    calls.append(tag)
    return tag
print(f("yes") if True else f("no"), f("a") if 0 else f("b"), calls)

words = ["pear", "Apple", "fig", "banana"]
print(sorted(words, key=lambda w: w.lower()), sorted(words, key=len, reverse=True), min(words, key=len), max(words, key=len))
words.sort(key=lambda w: w[-1])
print(words)
print({"k": 1, "z": 2, "a": 3}, {"x": [1], "y": {"n": None}} == {"x": [1], "y": {"n": None}})
print(0.1 + 0.2, 1e16, 1e15, 1e-05, 0.0001, 2.5, -0.0, 1 / 3, 1e100)

class Box:
    def __init__(self):
        self.items: list[str] = []
        self.handler: Optional[Callable[[], None]] = None
b = Box()
b.items.append("x")
print(b.items, b.handler)

async def double(x):
    return 2 * x
async def main():
    return [await double(2), await double(5)]
import asyncio
print(asyncio.run(main()))

class AppError(Exception):
    def __init__(self, code, detail=""):
        super().__init__(detail)
        self.code = code
class NotFound(AppError):
    pass
class Bad(ValueError):
    pass
for exc in [AppError(500, "boom"), NotFound(404, "missing"), Bad("bad value")]:
    try:
        raise exc
    except NotFound as e:
        print("NotFound", e.code, str(e), e.args)
    except AppError as e:
        print("AppError", e.code, str(e))
    except ValueError as e:
        print("ValueError", str(e), isinstance(e, Bad))
