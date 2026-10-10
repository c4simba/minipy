import timeit
t = timeit.timeit(lambda: sum(range(10)), number=100)
print(type(t).__name__, t >= 0)
tm = timeit.Timer(lambda: sum(range(10)))
r = tm.repeat(repeat=3, number=5)
print(len(r), all(x >= 0 for x in r))
n, tt = tm.autorange()
print(n >= 1, tt >= 0.2)
print(len(timeit.repeat(lambda: None, repeat=2, number=3)))
