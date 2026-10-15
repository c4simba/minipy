# Integer loops: the Collatz sequences of the numbers below LIMIT - while
# loops, %, //, *, + and comparisons on ints, nothing else.
LIMIT = 700_000


def collatz_steps(limit: int) -> int:
    total = 0
    longest = 0
    start = 1
    for n in range(1, limit):
        x = n
        steps = 0
        while x != 1:
            if x % 2 == 0:
                x = x // 2
            else:
                x = 3 * x + 1
            steps += 1
        total += steps
        if steps > longest:
            longest = steps
            start = n
    return total * 1000003 + start


print("collatz", collatz_steps(LIMIT))
