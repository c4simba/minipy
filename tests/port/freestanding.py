# A program for the freestanding check of tests/run_port_tests.sh: built from
# its C with no C library headers or functions, only ppy_port_* may be left
# to link (floats printed and parsed: the program's own code, not libc's).
def gen(n: int):
    for i in range(n):
        yield i * i

class Point:
    def __init__(self, x: float, y: float) -> None:
        self.x = x
        self.y = y

    def __repr__(self) -> str:
        return "Point(%r, %r)" % (self.x, self.y)

d: dict[str, list[int]] = {"squares": list(gen(6))}
p = Point(0.1 + 0.2, float("2.5e-3"))
print(d, p, round(3.14159, 2), int(7.9), abs(-3.5), 7.5 % 2, f"{1/3:.5f}")
try:
    [1, 2][5]
except IndexError as e:
    print("IndexError:", e)
