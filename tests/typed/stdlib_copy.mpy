# copy and deepcopy
import copy

a = [[1, 2], [3]]
b = copy.copy(a)
c = copy.deepcopy(a)
a[0].append(9)
print(a, b, c, b is a, b[0] is a[0], c[0] is a[0])
d = {"k": [1], "j": [2]}
e = copy.deepcopy(d)
d["k"].append(5)
print(d, e, copy.copy(d) == d)
s = {1, 2}
print(copy.copy(s), copy.copy("str"), copy.copy(5), copy.deepcopy((1, 2)), copy.copy(3.5))


class Pt:
    def __init__(self, x: int, ys: list[int]):
        self.x = x
        self.ys = ys

    def __copy__(self) -> "Pt":
        return Pt(self.x, self.ys)

    def __deepcopy__(self, memo) -> "Pt":
        return Pt(self.x, list(self.ys))

    def __repr__(self) -> str:
        return "Pt(" + str(self.x) + ", " + str(self.ys) + ")"


p = Pt(1, [2])
q = copy.copy(p)
r = copy.deepcopy(p)
p.ys.append(3)
print(p, q, r, q is p)
nested = {"a": [[1], [2]]}
dn = copy.deepcopy(nested)
nested["a"][0].append(7)
print(nested, dn)
