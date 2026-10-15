# Lists and ints: a sieve of Eratosthenes, then the gaps between the primes
# counted in a dict - indexing, slices of steps, append, dict updates; ROUNDS
# times over.
LIMIT = 5_000_000
ROUNDS = 12


def sieve(n: int) -> list[int]:
    flags = [True] * (n + 1)
    flags[0] = False
    flags[1] = False
    i = 2
    while i * i <= n:
        if flags[i]:
            for j in range(i * i, n + 1, i):
                flags[j] = False
        i += 1
    primes: list[int] = []
    for k in range(n + 1):
        if flags[k]:
            primes.append(k)
    return primes


def main() -> None:
    primes: list[int] = []
    gaps: dict[int, int] = {}
    for r in range(ROUNDS):
        primes = sieve(LIMIT - r)
        for k in range(1, len(primes)):
            g = primes[k] - primes[k - 1]
            gaps[g] = gaps.get(g, 0) + 1
    common = 0
    for g in gaps:
        if common == 0 or gaps[g] > gaps[common]:
            common = g
    print("primes", len(primes), primes[-1], common, gaps[common])


main()
