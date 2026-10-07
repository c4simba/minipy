# lists, dicts, sets and their methods
nums = [5, 3, 8, 1]
nums.append(7)
nums.insert(0, 9)
print(nums, len(nums), nums[0], nums[-1])
nums.sort()
print(nums, nums.index(8), nums.count(3), 3 in nums, 4 in nums)
nums.reverse()
print(nums, nums.pop(), nums.pop(0), nums)
nums.remove(5)
print(nums, sum(nums), min(nums), max(nums), sorted([3, 1, 2]))
nums[1] = 100
nums[0] += 1
print(nums, nums[1:], nums[:-1], nums + [0], [0] * 3)
del nums[0]
print(nums)
words = "the quick brown fox".split()
print(words, " ".join(words), "-".join(["a", "b"]))
squares = [i * i for i in range(6)]
evens = [n for n in squares if n % 2 == 0]
print(squares, evens)
d = {"one": 1, "two": 2}
d["three"] = 3
d["one"] += 10
print(d, len(d), d["two"], "two" in d, "four" in d)
print(d.get("four", 4), d.get("one"), list(d.keys()), list(d.values()))
for k, v in d.items():
    print(k, v)
for k in d:
    print(k, end=" ")
print()
print(d.pop("two"), d)
del d["one"]
print(d)
counts: dict[str, int] = {}
for w in "a b a c b a".split():
    counts[w] = counts.get(w, 0) + 1
print(counts)
s = {1, 2, 3}
s.add(2)
s.add(4)
s.discard(1)
print(sorted(s), len(s), 3 in s, sorted(s | {9}), sorted(s & {2, 3, 7}), sorted(s - {2}))
empty: list[int] = []
print(empty, len(empty), set(), {})
grid = [[0] * 3 for _ in range(2)]
grid[1][2] = 5
print(grid)
names = ["b", "a", "c"]
for i, n in enumerate(names):
    print(i, n)
for a, b in zip([1, 2, 3], ["x", "y", "z"]):
    print(a, b)
fl = [1.5, 2.5]
fl.append(3.0)
print(fl, sum(fl), max(fl))
m = {k: len(k) for k in ["aa", "b"]}
print(m)
pairs = [[1, 2], [3, 4]]
for p, q in pairs:
    print(p + q)
x, y = 1, 2
x, y = y, x
print(x, y)
first, second = [10, 20]
print(first, second)
print(list(range(5)), list(range(10, 0, -3)), list("abc"))
