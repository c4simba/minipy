# Dicts: filled with int and str keys, looked up (in, [], get), counted into,
# walked over (items, values), deleted from, built by a comprehension; the
# sizes and sums checked after each round; ROUNDS times over.
N = 100_000
ROUNDS = 100


def main() -> None:
    keys = ["k" + str(i) for i in range(N)]
    check = 0
    for r in range(ROUNDS):
        by_int: dict[int, int] = {}
        for i in range(N):
            by_int[i * 7 + r] = i
        by_str: dict[str, int] = {}
        for i in range(N):
            by_str[keys[i]] = i
        hits = 0
        for i in range(0, 2 * N, 3):
            if i in by_int:
                hits += by_int[i]
            hits += by_str.get(keys[i % N], 0)
        counts: dict[str, int] = {}
        for k in keys:
            c = k[-1]
            counts[c] = counts.get(c, 0) + 1
        total = 0
        for k, v in by_str.items():
            total += v
        for v in by_int.values():
            total -= v
        squares = {i: i * i for i in range(0, N, 2)}
        for i in range(0, N, 4):
            del squares[i]
        check += hits % 1000003 + len(counts) * counts["7"] + total + len(squares) + len(by_int) + len(by_str)
    print("dicts", check)


main()
