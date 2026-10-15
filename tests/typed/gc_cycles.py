# Garbage cycles among objects that also refer to live ones: the cycle
# collector frees the cycles (gc.collect(), or on its own after enough of
# them) and nothing live - a tree kept in a global, an object several cycles
# refer to, the strings and methods they hold - may go with them.
import gc
from typing import Callable


class Node:
    def __init__(self, name: str):
        self.name = name
        self.links: dict[str, "Node"] = {}
        self.parent: "Node | None" = None
        self.kids: list["Node"] = []
        self.on_click: Callable[[], str] | None = None

    def click(self) -> str:
        return "clicked " + self.name

    def add(self, kid: "Node") -> "Node":
        kid.parent = self
        self.kids.append(kid)
        return kid


def tree(depth: int, name: str) -> Node:
    root = Node(name)
    if depth > 0:
        for k in range(2):
            root.add(tree(depth - 1, name + str(k)))
    return root


def paths(n: Node) -> list[str]:
    out = [n.name + ("^" + n.parent.name if n.parent is not None else "")]
    for k in n.kids:
        out.extend(paths(k))
    return out


def garbage(rounds: int, keep: Node, shared: Node) -> int:
    made = 0
    for i in range(rounds):
        a = Node("a" + str(i))
        b = Node("b" + str(i))
        a.links["b"] = b
        b.links["a"] = a
        a.links["shared"] = shared              # garbage referring to a live object
        b.on_click = b.click
        t = tree(2, "g" + str(i))
        t.kids[0].links["root"] = t
        t.on_click = keep.kids[1].click         # ... and to a live object's bound method
        made += len(paths(t))
    return made


keep = tree(3, "t")
shared = Node("shared")
shared.links["self"] = shared
print(gc.isenabled(), garbage(20, keep, shared))
gc.collect()
print(gc.collect())
print(garbage(3000, keep, shared))             # more than it waits for: it collects on its own too
print(gc.collect() >= 0, paths(keep))
print(shared.name, len(shared.links), shared.links["self"] is shared)
h = keep.kids[1].click
print(h(), keep.kids[0].kids[1].parent is keep.kids[0])
keep.kids[0].parent = None
keep.kids.pop()
gc.collect()
print(paths(keep))
