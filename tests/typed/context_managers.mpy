class Tag:
    def __init__(self, name: str):
        self.name = name
    def __enter__(self) -> str:
        print("<" + self.name + ">")
        return self.name.upper()
    def __exit__(self, et, ev, tb) -> bool:
        print("</" + self.name + ">", et is not None)
        return et is ValueError

with Tag("a") as t:
    print("inside", t)
with Tag("b"), Tag("c") as c2:
    print("both", c2)
try:
    with Tag("d"):
        raise ValueError("swallowed")
    print("after d")
    with Tag("e"):
        raise KeyError("k")
except KeyError as e:
    print("caught", repr(e))
def f() -> int:
    with Tag("f"):
        return 5
print(f())
for i in range(3):
    with Tag("loop" + str(i)):
        if i == 1:
            continue
        if i == 2:
            break
        print("body", i)
