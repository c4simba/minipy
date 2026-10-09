# operator methods: @, in-place, reflected, chained comparisons of objects
class M:
    def __init__(self, v: int) -> None:
        self.v = v

    def __matmul__(self, o: "M") -> "M":
        return M(self.v * o.v)

    def __imatmul__(self, o: "M") -> "M":
        self.v = self.v * o.v + 1
        return self

    def __add__(self, o: int) -> "M":
        return M(self.v + o)

    def __iadd__(self, o: int) -> "M":
        self.v += o * 10
        return self

    def __rmul__(self, k: int) -> "M":
        return M(self.v * k * 100)

    def __radd__(self, k: int) -> "M":
        return M(self.v + k + 1000)

    def __lt__(self, o: "M") -> bool:
        return self.v < o.v

    def __eq__(self, o: "M") -> bool:
        return self.v == o.v


a = M(2)
b = M(3)
print((a @ b).v, (a + 1).v, (3 * a).v, (5 + a).v)
c = M(4)
c @= b
print(c.v)
c += 2
print(c.v)
x, y, z = M(1), M(2), M(3)
print(x < y < z, x < z < y, x == M(1) < y, y < z == M(3), x < y == z)
k = 0
for w in [M(5), M(1)]:
    if x < w < z:
        k += 1
print(k)
print((2,) > (1, 5), (1,) < (1, 0), (1, 2) > (1,), (1, "a") < (1, "a", 2.5), (1,) == (1, 2), (1,) != (1, 2))
xs = [(3, "c"), (1, "a"), (2, "b")]
print(sorted(xs), max(xs), (1, 2, 3) <= (1, 2, 3), (1, 2) >= (1, 2, 0))
