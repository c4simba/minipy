# operator overloading, @property, class constants
class Vec:
    ZERO_LEN: float = 0.0
    DIMS = 2

    def __init__(self, x: float, y: float):
        self.x = x
        self.y = y

    def __add__(self, o: "Vec") -> "Vec":
        return Vec(self.x + o.x, self.y + o.y)

    def __sub__(self, o: "Vec") -> "Vec":
        return Vec(self.x - o.x, self.y - o.y)

    def __mul__(self, k: float) -> "Vec":
        return Vec(self.x * k, self.y * k)

    def __neg__(self) -> "Vec":
        return Vec(-self.x, -self.y)

    def __eq__(self, o: "Vec") -> bool:
        return self.x == o.x and self.y == o.y

    def __lt__(self, o: "Vec") -> bool:
        return self.length < o.length

    @property
    def length(self) -> float:
        return (self.x * self.x + self.y * self.y) ** 0.5

    def __repr__(self) -> str:
        return "Vec(" + str(self.x) + ", " + str(self.y) + ")"

a = Vec(1.0, 2.0)
b = Vec(3.0, 4.0)
print(a + b, b - a, a * 3.0, -a, a == Vec(1.0, 2.0), a != b, a < b, b < a)
print(b.length, Vec.DIMS, Vec.ZERO_LEN)
c = a
c += b
print(c, a)

class Grid:
    def __init__(self, w: int, h: int):
        self.w = w
        self.cells = [0] * (w * h)

    def __getitem__(self, pos: tuple[int, int]) -> int:
        return self.cells[pos[1] * self.w + pos[0]]

    def __setitem__(self, pos: tuple[int, int], v: int):
        self.cells[pos[1] * self.w + pos[0]] = v

    def __contains__(self, v: int) -> bool:
        return v in self.cells

    def __len__(self) -> int:
        return len(self.cells)

g = Grid(3, 2)
g[(1, 1)] = 7
g[(0, 0)] = g[(1, 1)] + 1
print(g[(1, 1)], g[(0, 0)], 7 in g, 9 in g, 9 not in g, len(g))
