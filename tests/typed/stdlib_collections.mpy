# collections: deque, Counter, defaultdict, OrderedDict (classes of the standard library written in Python)
from collections import Counter, OrderedDict, defaultdict, deque

d = deque([1, 2, 3])
d.append(4)
d.appendleft(0)
print(d, len(d), d[0], d[-1], d.popleft(), d.pop(), list(d), 2 in d)
d.rotate(1)
print(d, d.index(2))
d.extendleft([9, 8])
print(d)
q = deque(maxlen=3)
for i in range(5):
    q.append(i)
print(q, q.maxlen)
q.reverse()
print(list(reversed(q)), q.count(3), bool(deque()))
words = deque("abc")
words.remove("b")
print(words, deque())
try:
    deque().pop()
except IndexError as e:
    print("IndexError:", e)

c = Counter("mississippi")
print(c, c["s"], c["z"], c.most_common(2), sorted(c.elements())[:5], c.total(), len(c))
c.update("ss")
c.subtract({"i": 4})
print(c["s"], c["i"], c + Counter("ab"), c - Counter("ssss"))
e1 = Counter("ab")
print(Counter({"a": 2, "b": 1}), e1 == Counter("ba"), Counter([1, 1, 2]) & Counter([1, 2, 2]))

dd: defaultdict[str, list[int]] = defaultdict(list)
for i, w in enumerate(["a", "b", "a"]):
    dd[w].append(i)
print(dd["a"], dd["b"], len(dd), "c" in dd, dd.items())
counts: defaultdict[str, int] = defaultdict(int)
for ch in "hello":
    counts[ch] += 1
print(sorted(counts.items()), counts.get("z"))

od: OrderedDict[str, int] = OrderedDict()
od["x"] = 1
od["y"] = 2
od["z"] = 3
od.move_to_end("x")
print(list(od), od.popitem(), od.popitem(last=False), od)
q2 = deque()
q2.append((1, "a"))
q2 = deque()
q2.append((2, "b"))
print(q2.popleft())
