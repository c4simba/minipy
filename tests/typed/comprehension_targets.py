pairs = [((1, 2), "a"), ((3, 4), "b")]
print([x + y for (x, y), _ in pairs])
print({s: x * y for (x, y), s in pairs})
print(sorted({x for (x, _), _ in pairs}))
print(list(y for (_, y), _ in pairs))
rows = [[1, 2, 3], [4, 5, 6, 7]]
print([(head, tail) for head, *tail in rows])
print([a + b + c for a, b, c in [(1, 2, 3), (4, 5, 6)]])
print(sum(a * b for a, b, *_ in [[1, 2, 0], [3, 4, 0, 0]]))
print(any(q > 4 for _, q in [(1, 2), (3, 5)]), all(p < 4 for p, _ in [(1, 2), (3, 5)]))
