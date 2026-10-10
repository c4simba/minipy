import contextvars
v = contextvars.ContextVar("v", default=1)
w: contextvars.ContextVar[int] = contextvars.ContextVar("w")
print(v.get(), v.name)
t = v.set(5)
print(v.get(), t.var is v, t.old_value is contextvars.Token.MISSING)
v.reset(t)
print(v.get())
try:
    w.get()
except LookupError:
    print("LookupError")
print(w.get(7))
ctx = contextvars.copy_context()


def f(x: int) -> int:
    v.set(x)
    return v.get()


print(ctx.run(f, 9), v.get(), ctx[v], v in ctx, w in ctx)
s = contextvars.ContextVar("s", default="x")
tok = s.set("y")
tok2 = s.set("z")
s.reset(tok2)
print(s.get())
try:
    s.reset(tok2)
except RuntimeError:
    print("RuntimeError used")
