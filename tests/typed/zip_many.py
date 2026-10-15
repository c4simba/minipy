# zip of several iterables, tuples as loop variables, nested targets with enumerate
names = ["a", "b", "c"]
ages = [1, 2, 3]
ok = [True, False, True]
for n, a, o in zip(names, ages, ok):
    print(n, a, o, end="; ")
print()
print(list(zip(names, ages, ok)), list(zip(names)), list(zip([1, 2], "xyz")))
for t in zip(names, ages):
    print(t, end=" ")
print()
for p in enumerate("hi"):
    print(p, end=" ")
print()
d = {"x": 1, "y": 2}
for kv in d.items():
    print(kv, end=" ")
print()
pairs = [(1, 2), (3, 4)]
for i, (a, b) in enumerate(pairs):
    print(i, a + b, end="; ")
print()
print([i * x for i, (x, _) in enumerate(pairs)], dict(zip(names, ages)), sum(a * b for a, b in zip(ages, ages)))
print(list(map(lambda a, b, c: a + b + c, [1], [2], [3])))
