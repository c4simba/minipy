class Box:
    def __init__(self, items: list[int]):
        self.items = items
    def __len__(self) -> int:
        return len(self.items)


class Flag:
    def __init__(self, on: bool):
        self.on = on
    def __bool__(self) -> bool:
        return self.on


class Plain:
    pass


b0 = Box([])
b1 = Box([1, 2])
print(bool(b0), bool(b1), not b0, not b1)
if b0:
    print("b0 true")
else:
    print("b0 false")
f = Flag(False)
t = Flag(True)
print(bool(f), bool(t), bool(Plain()))
n = 0
while not f and n < 3:
    n += 1
print(n)
print([x.on for x in [f, t, f] if x])
r = f or t
print(r.on, (t and f).on, (b0 or b1).items)
print("yes" if b1 else "no", "yes" if f else "no")
assert t
print(bool(0j), bool(1j), not 0j)
