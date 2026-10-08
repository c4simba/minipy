# reference counts the compiler leaves out (borrowed loop variables, moves on
# last use, parameters handed over): cases where doing it wrongly would read
# freed memory - the output must still match CPython
class Node:
    def __init__(self, name: str, kids: list["Node"]):
        self.name = name
        self.kids = kids

    def label(self) -> str:
        return "<" + self.name + ">"

class Bag:
    def __init__(self):
        self.items: list[str] = []

    def add(self, item: str) -> None:
        self.items.append(item)

    def drain(self) -> None:
        while self.items:
            self.items.pop()

def tag(s: str) -> str:
    return "#" + s

def loops() -> list[str]:
    out: list[str] = []
    words = ("one two three " + "four").split()
    for w in words:                      # the body removes items: w keeps its own
        words.pop()
        out.append(w.upper())
    last = ""
    for w2 in ["a" + "1", "b" + "2"]:
        last = w2                        # used after the loop through `last`
    out.append(last)
    for w3 in [tag("x"), tag("y")]:
        pass
    out.append(w3)                       # the loop variable read after its loop
    bag = Bag()
    for i in range(3):
        bag.add("item" + str(i))
    for it in bag.items:                 # user code in the body may change the list
        bag.drain()
        out.append(it + "!")
    for a in ["p", "q"]:
        for a2 in [a + "1", a + "2"]:
            out.append(a2)
    return out

def moves(n: int) -> list[str]:
    keep: list[str] = []
    for i in range(n):
        s = "v" + str(i)                 # moved into the list on its last use
        keep.append(s)
        t = s + "?" if i % 2 else "even" + str(i)
        keep.append(t)
    x = "x" * 3
    y = x                                # x still used below: not moved
    keep.append(x + y)
    z = keep[0] + "z"
    return [z] + keep

def build(depth: int) -> Node:
    if depth == 0:
        return Node("leaf", [])
    kids = [build(depth - 1), build(depth - 1)]
    name = "d" + str(depth)
    return Node(name, kids)              # name and kids handed over

def walk(n: Node) -> int:
    total = len(n.name)
    for k in n.kids:
        total += walk(k)
    return total

def handed(names: list[str]) -> list[Node]:
    nodes: list[Node] = []
    for nm in names:
        node = Node(nm, [])
        nodes.append(node)
        node.kids.append(Node(nm + "-kid", []))
    shared = "shared"
    a = Node(shared, [])
    b = Node(shared, [a])                # `shared` lent twice
    nodes.append(b)
    return nodes

def risky(flag: bool) -> str:
    s = "r" + "isky"
    try:
        if flag:
            raise ValueError("boom " + s)
        return s
    except ValueError as e:
        return str(e) + " " + s

print(loops())
print(moves(4))
root = build(3)
print(walk(root), root.label(), root.kids[0].kids[1].label())
print([n.name + ":" + str(len(n.kids)) for n in handed(["a", "b"])])
print(risky(False), risky(True))

def stress(rounds: int) -> int:
    total = 0
    for r in range(rounds):
        total += walk(build(2)) + len(moves(3)) + len(loops())
    return total

print(stress(200))
