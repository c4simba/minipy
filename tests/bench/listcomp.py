# List comprehensions: squares, a filter, tuples taken apart, strings, a
# nested comprehension and a flattening one; their lengths and sums checked
# after each round; ROUNDS times over.
N = 200_000
ROUNDS = 150


def main() -> None:
    words = ["w" + str(i % 1000) for i in range(N)]
    total = 0
    for r in range(ROUNDS):
        squares = [i * i for i in range(N)]
        odd = [s for s in squares if s & 1]
        pairs = [(i, s) for i, s in enumerate(odd) if i % 3 == 0]
        sums = [a + b + r for a, b in pairs]
        lens = [len(w) for w in words if w[-1] == "7"]
        grid = [[x * y for x in range(40)] for y in range(500)]
        flat = [v for row in grid for v in row if v % 3 == 0]
        total += len(odd) + sum(sums) % 1000003 + sum(lens) + len(flat) + squares[-1] % 1000
    print("listcomp", total)


main()
