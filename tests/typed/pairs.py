# zip / enumerate / range as values, dict(pairs), dict(d), tuple(xs)
names = ["ann", "bob", "cy"]
ages = [31, 25, 40]
print(list(zip(names, ages)), list(enumerate(names, 1)))
people = dict(zip(names, ages))
print(people, dict(people) == people, len(dict(enumerate("xyz"))))
print(sorted(zip(ages, names)), max(zip(ages, names)))
print(tuple(names), tuple(range(3)), tuple(x * 2 for x in ages), tuple())
r = range(2, 10, 3)
print(list(r), len(range(5)), 7 in range(10), sum(range(101)))
by_len = dict((n, len(n)) for n in names)
print(by_len, {v: k for k, v in by_len.items()})
pairs = list(zip(range(3), "abc"))
for i, nc in enumerate(pairs):
    print(i, nc[0], nc[1])
