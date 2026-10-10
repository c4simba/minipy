g = {"a": 2}
exec("b = a * 3", g)
print(g["b"], sorted(k for k in g if k != "__builtins__"))
print(eval("a + 1", g), eval("x * 2", {"x": 21}), eval("y", {}, {"y": 5}))
def f() -> None:
    z = 10
    print(eval("z + 1"))
    exec("w = z * 2")
f()
code = compile("1 + 2", "<expr>", "eval")
print(eval(code), type(code).__name__)
c2 = compile("q = 9\nprint(q)", "<prog>", "exec")
exec(c2)
print(q)
try:
    eval("1 +")
except SyntaxError as e:
    print("SyntaxError", e.lineno is not None)
try:
    compile("x", "f", "bad")
except ValueError as e:
    print("ValueError:", e)
print(eval("  [i * i for i in range(4)]"), eval("lambda v: v + 1")(1))
ns: dict = {}
exec("def h(n):\n    return n * 2\n", ns)
print(ns["h"](4))
