def scale(x: int, factor: int) -> int:
    return x * factor
f = scale
print(f(2, factor=3))
def greet(name: str, *, greeting: str = "Hello", punct: str = "!") -> str:
    return f"{greeting}, {name}{punct}"
g = greet
print(g("a", greeting="Hi"), g("b", punct="?"), g(name="c"))
handlers = [greet, lambda name, greeting="Yo", punct=".": greeting + " " + name + punct]
print(handlers[0]("d", punct="!!"))
