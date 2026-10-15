# reference counting under load: lots of short-lived strings, lists, dicts and objects
class Item:
    def __init__(self, name: str, tags: list[str]):
        self.name = name
        self.tags = tags
        self.next: Item = None

def churn(rounds: int) -> int:
    total = 0
    for r in range(rounds):
        s = "item" + str(r)
        parts = s.split("m")
        d = {s: len(s), "x": r}
        it = Item(s, parts)
        it.next = Item(s + "!", [s, s])
        total += len(it.next.tags) + d["x"] % 7 + len(parts)
        words = [w.upper() for w in parts]
        total += len("".join(words))
    return total

print(churn(20000))
chain: Item = None
for i in range(1000):
    node = Item("n" + str(i), [])
    node.next = chain
    chain = node
n = 0
while chain is not None:
    n += 1
    chain = chain.next
print(n)
big: list[str] = []
for i in range(5000):
    big.append(str(i) * 3)
print(len(big), big[4999])
big = []
print(len(big))
