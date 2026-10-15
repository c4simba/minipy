# *args, **kwargs, keyword-only parameters, f(*seq), f(**d), print(*xs)

def total(*nums: int) -> int:
    s = 0
    for n in nums:
        s += n
    return s

def describe(name: str, *tags: str, sep: str = ", ") -> str:
    return name + ": " + sep.join(tags)

def options(title: str, **opts: int) -> str:
    keys = sorted(opts)
    return title + " " + " ".join(k + "=" + str(opts[k]) for k in keys)

def everything(a: int, *rest: int, scale: int = 1, **named: int) -> str:
    return str(a * scale) + " " + str(rest) + " " + str(sorted(named.items()))

def point(x: int, y: int, z: int = 0) -> str:
    return "(" + str(x) + ", " + str(y) + ", " + str(z) + ")"

def kwonly(a: int, *, b: int, c: int = 3) -> int:
    return a * 100 + b * 10 + c

def show(*args: str) -> None:
    print(len(args), args)

print(total(), total(5), total(1, 2, 3, 4))
print(describe("fruit", "apple", "pear"), describe("none"), describe("x", "a", "b", sep="|"))
print(options("cfg"), options("cfg", width=3, height=4, depth=1))
print(everything(1), everything(2, 3, 4, scale=10, k=5, j=6))
nums = [10, 20, 30]
print(total(*nums), total(1, *nums))
p = (1, 2)
print(point(*p), point(*p, z=9), point(*[7, 8, 9]))
cfg = {"width": 1, "height": 2}
print(options("d", **cfg), options("e", depth=5, **cfg))
print(kwonly(1, b=2), kwonly(1, c=5, b=2))
show()
show("one")
show("a", "b")
row = [1, 2, 3]
print(*row)
print(*row, sep="-")
print("start", *["x", "y"], "end")
print(*[])
print(*(4, 5, 6), sep=", ")

class Logger:
    def __init__(self, prefix: str):
        self.prefix = prefix
        self.lines: list[str] = []

    def log(self, *parts: str, **fields: str) -> None:
        line = self.prefix + " ".join(parts)
        for k in sorted(fields):
            line += " " + k + "=" + fields[k]
        self.lines.append(line)

lg = Logger("> ")
lg.log("hello", "world")
lg.log("user", id="7", name="ann")
lg.log()
print(lg.lines)

def apply(fn, *args: int):
    return fn(*args)

print(apply(lambda a, b: a - b, 10, 3))
