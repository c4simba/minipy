# A tuple of constants is one static object (as in CPython's co_consts), made
# with the program instead of built each time: it must still act as a tuple
# everywhere - stored, returned, a dict key, sorted, compared, iterated - and
# t += u must give a new tuple even when t is one of them.
def pair() -> tuple[int, str]:
    return (7, "seven")


def table() -> list[tuple[str, int, float]]:
    rows: list[tuple[str, int, float]] = []
    for i in range(3):
        rows.append(("row", -1, 2.5))
        rows.append(("neg", -9223372036854775807 - 1, -0.0))
    return rows


def nested() -> tuple[tuple[int, int], tuple[str, bytes], bool]:
    return ((1, -2), ("x", b"\x00y"), True)


def opts() -> tuple[int | None, float | None, str | None, bool | None]:
    return (None, None, None, None)


def floats() -> tuple[float, float]:
    return (1.0, -2e300)


def ints(*args: int) -> tuple[int, ...]:
    return args


def grow() -> list[str]:
    out: list[str] = []
    for k in range(3):
        t: tuple[int, ...] = (1, 2)
        u = t
        u += ints(k)                            # a new tuple: t stays (1, 2)
        out.append(str(t) + str(u))
    return out


def lookups(n: int) -> int:
    hits = 0
    for i in range(n):
        if i % 10 in (1, 3, 5):
            hits += 1
        for w in ("a", "bb", "ccc"):
            hits += len(w)
        if i % 4 not in (0, 2):
            hits += 100
    return hits


d: dict[tuple[int, str], str] = {}
d[(7, "seven")] = "found"
p = pair()
print(p, d[p], d.get((7, "seven"), "?"), p == (7, "seven"), (7, "seven") < (8, ""))
rows = table()
print(rows, len(rows), rows[0] is rows[2])
print(nested(), opts(), floats())
print(sorted(ints(3, 1, 2)), [v * 2 for v in (5, 4)], max(ints(2, 9, 4)), ints(1, 2) + ints(3), ints(1, 2) * 2)
print(grow())
print(lookups(1000))
a, b = (10, "ten")
x, *rest = (1, 2, 3, 4)
print(a, b, x, rest, hash((1, 2)) == hash((1, 2)), str((1.5, "q")), repr(("é", b"\xff")))
for t in [(1, "one"), (2, "two")]:
    print(t)
