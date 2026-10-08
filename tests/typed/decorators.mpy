# decorators: registration (UI-style), wrappers, factories with arguments,
# stacked decorators, decorated methods and nested functions
from typing import Callable
from functools import wraps

class Widget:
    def __init__(self, kind: str, label: str, x: int, y: int, handler: Callable[[], str]):
        self.kind = kind
        self.label = label
        self.x = x
        self.y = y
        self.handler = handler

class Window:
    def __init__(self, title: str):
        self.title = title
        self.widgets: list[Widget] = []
        self.keys: dict[str, Callable[[], str]] = {}

    def button(self, label: str, x: int = 0, y: int = 0):
        def register(fn: Callable[[], str]) -> Callable[[], str]:
            self.widgets.append(Widget("button", label, x, y, fn))
            return fn
        return register

    def on_key(self, key: str):
        def register(fn: Callable[[], str]) -> Callable[[], str]:
            self.keys[key] = fn
            return fn
        return register

    def click(self, label: str) -> str:
        for w in self.widgets:
            if w.label == label:
                return w.handler()
        return "no such button"

    def press(self, key: str) -> str:
        if key in self.keys:
            return self.keys[key]()
        return "unbound key " + key

    def draw(self) -> None:
        print("[" + self.title + "]")
        for w in self.widgets:
            print("  " + w.kind, repr(w.label), "at", (w.x, w.y))

win = Window("Demo")
log: list[str] = []

def trace(fn):
    @wraps(fn)
    def wrapper(*args, **kwargs):
        log.append("-> " + fn.__name__)
        result = fn(*args, **kwargs)
        log.append("<- " + fn.__name__)
        return result
    return wrapper

@win.button("OK", x=10, y=20)
@win.on_key("Enter")
def on_ok() -> str:
    return "accepted"

@win.button("Cancel", x=80, y=20)
@win.on_key("Escape")
@trace
def on_cancel() -> str:
    return "cancelled"

win.draw()
print(win.click("OK"), win.click("Cancel"), win.click("Help"))
print(win.press("Escape"), win.press("Enter"), win.press("F1"))
print(on_ok(), on_cancel(), log)

def repeat(times: int):
    def deco(fn: Callable[[str], str]) -> Callable[[str], str]:
        def run(s: str) -> str:
            out = s
            for _ in range(times):
                out = fn(out)
            return out
        return run
    return deco

@repeat(3)
def shout(s: str) -> str:
    return s + "!"

print(shout("hey"))

def memo(fn: Callable[[int], int]) -> Callable[[int], int]:
    cache: dict[int, int] = {}
    def get(n: int) -> int:
        if n not in cache:
            cache[n] = fn(n)
        return cache[n]
    return get

calls = 0

@memo
def fib(n: int) -> int:
    global calls
    calls += 1
    return n if n < 2 else fib(n - 1) + fib(n - 2)

print(fib(40), calls)

@trace
def add(a: int, b: int) -> int:
    return a + b

log.clear()
print(add(2, 3), add(*[4, 5]), log)

def tagged(tag: str):
    def deco(fn):
        def wrapper(text: str) -> str:
            return "<" + tag + ">" + fn(text) + "</" + tag + ">"
        return wrapper
    return deco

@tagged("b")
@tagged("i")
def label(text: str) -> str:
    return text.upper()

print(label("hi"), label.__name__)

handlers: dict[str, Callable[["Game"], str]] = {}

def command(name: str):
    def deco(fn: Callable[["Game"], str]) -> Callable[["Game"], str]:
        handlers[name] = fn
        return fn
    return deco

def logged(fn: Callable[["Game"], str]) -> Callable[["Game"], str]:
    def wrapper(g: "Game") -> str:
        return "[" + fn(g) + "]"
    return wrapper

class Game:
    def __init__(self):
        self.score = 0

    @command("up")
    def go_up(self) -> str:
        self.score += 1
        return "up " + str(self.score)

    @command("reset")
    @logged
    def reset(self) -> str:
        self.score = 0
        return "reset"

g = Game()
for cmd in ["up", "up", "reset", "up"]:
    print(cmd, "->", handlers[cmd](g))
print(g.go_up(), g.reset(), g.score)

def outer(prefix: str) -> list[str]:
    seen: list[str] = []
    def remember(fn: Callable[[str], str]) -> Callable[[str], str]:
        def inner(s: str) -> str:
            seen.append(s)
            return fn(s)
        return inner

    @remember
    def greet(s: str) -> str:
        return prefix + s

    return [greet("a"), greet("b")] + seen

print(outer("hello "))

class Counter:
    def __init__(self, fn: Callable[[], None]):
        self.fn = fn
        self.n = 0

    def __call_it(self) -> None:
        pass

    def run(self) -> int:
        self.fn()
        self.n += 1
        return self.n

@Counter
def tick() -> None:
    pass

tick.run()
print(tick.run())
