# random: CPython's generator and algorithms (the same numbers for a seed)
import random


def r12(x: float) -> float:
    return round(x, 12)


def main() -> None:
    random.seed(42)
    print(random.random(), random.randint(1, 100), random.choice("abcdef"), random.uniform(1, 2))
    for s in [0, 1, -42, 2**40 + 7, 2**62]:
        random.seed(s)
        print(s, random.random(), random.getrandbits(40))
    random.seed(3.5)
    print(random.random())
    random.seed("hello")
    print(random.random())
    random.seed(b"bytes")
    print(random.random())
    random.seed(12345)
    print([random.randint(1, 6) for _ in range(10)], random.randrange(10), random.randrange(5, 50, 7), random.randrange(50, 5, -3))
    print(random.choice([1, 2, 3]), random.choice("xyz"), random.choice([4.5, 5.5]))
    xs = list(range(20))
    random.shuffle(xs)
    print(xs)
    words = ["a", "b", "c", "d"]
    random.shuffle(words)
    print(words)
    print(random.sample(list(range(1000)), 10), random.sample([1, 2, 3, 4], 4), random.sample(["red", "blue"], 2, counts=[4, 2]))
    print(random.choices(["a", "b", "c"], k=8), random.choices([1, 2, 3], weights=[10, 1, 1], k=6))
    print(random.choices(["x", "y"], cum_weights=[1.0, 5.0], k=4), random.choices([0, 1, 2], [0.1, 0.2, 0.7], k=3))
    print(random.uniform(1, 10), random.triangular(), random.triangular(0, 10, 2))
    # (x87 log/exp/pow on i386 may differ from libm in the last digit: 12 places)
    print(r12(random.gauss()), r12(random.gauss(10, 2)), r12(random.normalvariate()), r12(random.normalvariate(5, 0.5)))
    print(r12(random.lognormvariate(0, 1)), r12(random.expovariate()), r12(random.expovariate(3.0)))
    print(r12(random.vonmisesvariate(1.0, 0.5)), r12(random.gammavariate(0.5, 1.0)), r12(random.gammavariate(3.0, 1.5)), r12(random.betavariate(2, 5)))
    print(r12(random.paretovariate(3.0)), r12(random.weibullvariate(1.0, 1.5)))
    print([random.binomialvariate(10, 0.3) for _ in range(5)], random.binomialvariate(100, 0.75), random.binomialvariate(1000, 0.4))
    print(random.randbytes(7), random.randbytes(4), random.getrandbits(0), random.getrandbits(63))
    r = random.Random(99)
    r2 = random.Random(99)
    print(r.random() == r2.random(), r.randint(0, 100), r2.choice("abcdef"), random.Random("seed").random())
    print(random.Random().random() < 1.0)
    try:
        random.randrange(0)
    except ValueError as e:
        print("ValueError", e)
    try:
        random.randint(5, 1)
    except ValueError as e:
        print("ValueError", e)
    try:
        empty: list[int] = []
        random.choice(empty)
    except IndexError as e:
        print("IndexError", e)
    try:
        random.sample([1, 2], 3)
    except ValueError as e:
        print("ValueError", e)
    sr = random.SystemRandom()
    v = sr.random()
    print(0.0 <= v < 1.0, 1 <= sr.randint(1, 5) <= 5, len(sr.randbytes(5)))


main()
