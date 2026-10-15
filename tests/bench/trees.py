# Objects that refer to each other: trees whose nodes know their parent and
# their kids (a list and a dict by name), built, walked both ways and
# dropped, ROUNDS times over. Every tree left behind is a garbage cycle: the
# compiled program's cycle collector frees it, as CPython's gc does.
ROUNDS = 3000


class Node:
    def __init__(self, name: str, parent: "Node | None"):
        self.name = name
        self.parent = parent
        self.kids: list["Node"] = []
        self.by_name: dict[str, "Node"] = {}


def build(depth: int, name: str, parent: "Node | None") -> Node:
    n = Node(name, parent)
    if depth > 0:
        for k in range(3):
            kid = build(depth - 1, name + str(k), n)
            n.kids.append(kid)
            n.by_name[kid.name] = kid
    return n


def count(n: Node) -> int:
    total = 1
    for k in n.kids:
        total += count(k)
    return total


def depths(n: Node) -> int:
    d = 0
    p = n.parent
    while p is not None:
        d += 1
        p = p.parent
    for k in n.kids:
        d += depths(k)
    return d


def main() -> None:
    total = 0
    for r in range(ROUNDS):
        t = build(6, "n", None)
        total += count(t) + depths(t) + len(t.by_name["n2"].by_name)
    print("trees", total)


main()
