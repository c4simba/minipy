# Reference cycles of every kind the cycle collector follows: objects through
# dicts, lists, sets and tuples, an object and itself, kids and their parent,
# an object holding its own bound method, closures and the lists they capture
# (copied or in a cell). Each round makes such garbage; the blocks still live
# at the end must not depend on the number of rounds.
import sys
from typing import Callable


class Node:
    def __init__(self, name: str):
        self.name = name
        self.links: dict[str, "Node"] = {}
        self.parent: "Node | None" = None
        self.kids: list["Node"] = []
        self.peers: set["Node"] = set()
        self.pair: "tuple[Node, str] | None" = None
        self.handler: Callable[[], int] | None = None

    def click(self) -> int:
        return len(self.name)


def mutual(i: int) -> int:                 # a <-> b through dicts
    a = Node("a" + str(i))
    b = Node("b")
    a.links["b"] = b
    b.links["a"] = a
    return len(a.links) + len(b.links)


def selfref(i: int) -> int:                # a -> a
    a = Node("s" + str(i))
    a.links["me"] = a
    return len(a.links)


def tree(i: int) -> int:                   # kids and their parent
    root = Node("root")
    for k in range(3):
        kid = Node("kid" + str(k))
        kid.parent = root
        root.kids.append(kid)
    return len(root.kids)


def through_set_and_tuple(i: int) -> int:  # a set of peers, a tuple naming its owner
    a = Node("p" + str(i))
    b = Node("q")
    a.peers.add(b)
    b.peers.add(a)
    a.pair = (a, "self")
    return len(a.peers) + len(b.peers)


def bound(i: int) -> int:                  # self.handler = self.click
    n = Node("h" + str(i))
    n.handler = n.click
    h = n.handler
    return h() if h is not None else 0


def captured(i: int) -> int:               # a list holding the closure that captures it
    fs: list[Callable[[], int]] = []

    def f() -> int:
        return len(fs) + i
    fs.append(f)
    return fs[0]()


def in_cell(i: int) -> int:                # the same through a cell (assigned after the capture)
    holder: list[Callable[[], int]] = []

    def g() -> int:
        return len(holder)
    holder = [g]
    return holder[0]()


def acyclic(i: int) -> int:                # no cycle: reference counting alone
    a = Node("x" + str(i))
    b = Node("y")
    a.links["b"] = b
    a.kids.append(b)
    return len(a.links)


def dict_holes(i: int) -> int:             # deletions leave holes in a dict, squeezed out later
    d: dict[str, Node] = {}
    for k in range(20):
        d["k" + str(k)] = Node("v")
    for k in range(0, 20, 3):
        del d["k" + str(k)]
    return len(d) + len(list(d.values()))


N = int(sys.argv[1]) if len(sys.argv) > 1 else 100
total = 0
for i in range(N):
    total += mutual(i) + selfref(i) + tree(i) + through_set_and_tuple(i) + bound(i)
    total += captured(i) + in_cell(i) + acyclic(i) + dict_holes(i)
print(total > 0)
