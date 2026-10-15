# // and % by a constant (src/aot/peep.c: a shift or a mask for a power of two,
# rt_idivc for the others) against the general division, on values around the
# word and sign boundaries; x / <literal> (no zero check).
VALUES = [0, 1, -1, 5, -5, 59, -59, 60, -60, 86399, 86400, -86400, -86401, 2147483647, -2147483648,
          4294967295, 4294967296, -4294967296, 1700000000123456789, -1700000000123456789,
          9223372036854775807, -9223372036854775807 - 1, 4611686014132420609, -98765432109876]


def by_constants(a: int) -> list[int]:
    return [a // 1, a % 1, a // 2, a % 2, a // 3, a % 3, a // 8, a % 8, a // 10, a % 10, a // 60, a % 60,
            a // 1024, a % 1024, a // 86400, a % 86400, a // 146097, a % 146097, a // 1000000000,
            a % 1000000000, a // 1073741824, a % 1073741824, a // 2147483647, a % 2147483647]


def by_variables(a: int) -> list[int]:
    out: list[int] = []
    for c in [1, 2, 3, 8, 10, 60, 1024, 86400, 146097, 1000000000, 1073741824, 2147483647]:
        out.append(a // c)
        out.append(a % c)
    return out


def augmented(a: int) -> list[int]:
    q = a
    q //= 7
    r = a
    r %= 7
    s = a
    s //= 16
    t = a
    t %= 16
    return [q, r, s, t]


bad = 0
for v in VALUES:
    if by_constants(v) != by_variables(v):
        bad += 1
        print("differ:", v, by_constants(v), by_variables(v))
    print(v, augmented(v), v // 86400, v % 60)
print("bad", bad)
print(7 / 2, 1 / 3.0, 1e9 / 1e9, -5 / 4, 3 / 1e-300, 2.5 / -0.5)
