# dicts keyed by int, tuple and objects (not only str)
squares = {n: n * n for n in range(6)}
print(squares, squares[4], 7 in squares, 3 in squares, len(squares))
counts: dict[int, int] = {}
for x in [3, 1, 3, 2, 3, 1]:
    counts[x] = counts.get(x, 0) + 1
print(counts, sorted(counts), sorted(counts.items()), max(counts, key=lambda k: counts[k]))
del counts[2]
print(counts, list(counts.keys()), list(counts.values()), counts.pop(1), counts)

memo: dict[int, int] = {}
def fib(n: int) -> int:
    if n < 2:
        return n
    if n in memo:
        return memo[n]
    r = fib(n - 1) + fib(n - 2)
    memo[n] = r
    return r
print(fib(40), len(memo))

grid: dict[tuple[int, int], str] = {}
grid[(0, 0)] = "origin"
grid[(1, 2)] = "a"
grid[(1, 2)] = "b"
print(grid, grid[(1, 2)], (0, 0) in grid, (5, 5) in grid, grid.get((9, 9), "none"))
for (x, y) in [(0, 0), (1, 2)]:
    print(x, y, grid[(x, y)])
for k, v in sorted(grid.items()):
    print(k, v)

class Key:
    def __init__(self, n: int):
        self.n = n

a = Key(1)
b = Key(1)
ids = {a: "a", b: "b"}
print(len(ids), ids[a], ids[b])
names = {1: "one", 2: "two"}
print({v: k for k, v in names.items()}, set(names), dict(), {True: 1})
try:
    print(names[3])
except KeyError:
    print("missing int key")
