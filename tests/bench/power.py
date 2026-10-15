# Powers: modular exponentiation by squaring (as pow(b, e, m) does it), small
# integer powers with ** and float powers - the arithmetic of a hash or a
# simple cipher.
N = 3_000_000


def modpow(b: int, e: int, m: int) -> int:
    r = 1
    b = b % m
    while e > 0:
        if e & 1:
            r = r * b % m
        b = b * b % m
        e = e >> 1
    return r


def main() -> None:
    m = 1_000_003
    acc = 0
    cubes = 0
    f = 0.0
    for i in range(1, N):
        acc = (acc * 31 + modpow(i, 65537, m)) % 1_000_000_007
        cubes = (cubes + (i % 1000) ** 3) % 1_000_000_007
        f += (i % 100 + 1) ** 0.5 + (i * 0.001) ** 1.5
    print("power", acc, cubes, round(f, 3))


main()
