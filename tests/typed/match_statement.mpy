from dataclasses import dataclass

@dataclass
class Shape:
    pass

@dataclass
class Circle(Shape):
    r: float

@dataclass
class Rect(Shape):
    w: float
    h: float

class Point:
    __match_args__ = ("x", "y")
    def __init__(self, x: int, y: int):
        self.x = x
        self.y = y

def area(s: Shape) -> str:
    match s:
        case Circle(r=0):
            return "dot"
        case Circle(r):
            return f"circle {3.0 * r * r}"
        case Rect(w, h) if w == h:
            return f"square {w * h}"
        case Rect(w=w, h=h):
            return f"rect {w * h}"
        case _:
            return "unknown"

for sh in [Circle(0), Circle(2.0), Rect(3.0, 3.0), Rect(2.0, 5.0), Shape()]:
    print(area(sh))

def command(text: str) -> str:
    match text.split():
        case ["go", direction]:
            return "going " + direction
        case ["pick", "up", *items] if items:
            return "picking " + ", ".join(items)
        case ["look"] | ["l"]:
            return "looking"
        case []:
            return "nothing"
        case [first, *_]:
            return "unknown " + first
    return "?"

for t in ["go north", "pick up sword shield", "pick up", "look", "l", "", "dance now"]:
    print(command(t))

def where(p: Point) -> str:
    match p:
        case Point(0, 0):
            return "origin"
        case Point(x, 0):
            return f"x-axis {x}"
        case Point(0, y=y):
            return f"y-axis {y}"
        case Point(x=x, y=y) if x == y:
            return "diagonal"
        case _:
            return "elsewhere"
for pt in [Point(0, 0), Point(3, 0), Point(0, 4), Point(2, 2), Point(1, 5)]:
    print(where(pt))

def http(code: int) -> str:
    match code:
        case 200 | 201:
            return "ok"
        case 404:
            return "not found"
        case n if n >= 500:
            return "server error " + str(n)
        case _:
            return "other"
print([http(c) for c in [200, 201, 404, 503, 302]])

def cfg(d: dict[str, int]) -> str:
    match d:
        case {"port": port, "debug": 1}:
            return f"debug on {port}"
        case {"port": port, **rest}:
            return f"port {port} rest {sorted(rest)}"
        case _:
            return "no port"
print(cfg({"port": 80, "debug": 1}), cfg({"port": 8080, "a": 1, "b": 2}), cfg({}))

def opt(v: int | None) -> str:
    match v:
        case None:
            return "none"
        case 0:
            return "zero"
        case x:
            return f"value {x}"
print(opt(None), opt(0), opt(7))

def pair(t: tuple[int, str]) -> str:
    match t:
        case (0, s):
            return "zero " + s
        case (n, "x"):
            return f"x {n}"
        case (n, s) as whole:
            return f"{whole} {n} {s}"
print(pair((0, "a")), pair((5, "x")), pair((2, "b")))


@dataclass
class Node:
    left: "Node | None"
    value: int
    right: "Node | None"

def total(n: Node | None) -> int:
    match n:
        case None:
            return 0
        case Node(l, v, r):
            return total(l) + v + total(r)
    return -1

tree = Node(Node(None, 1, None), 2, Node(Node(None, 3, None), 4, None))
print(total(tree))

def describe(n: Node) -> str:
    match n:
        case Node(left=None, right=None, value=v):
            return f"leaf {v}"
        case Node(left=Node(value=lv), right=None):
            return f"left child {lv}"
        case Node(left=None, right=Node(value=rv)):
            return f"right child {rv}"
        case Node(value=v):
            return f"inner {v}"
    return "?"
print(describe(tree), describe(tree.left), describe(tree.right), describe(Node(None, 9, Node(None, 8, None))))

def kind(x: str) -> str:
    match x:
        case "a" | "e" | "i" | "o" | "u":
            return "vowel"
        case str() if len(x) == 1:
            return "consonant"
        case _:
            return "word"
print([kind(s) for s in ["a", "b", "hello"]])

def nested(data: list[list[int]]) -> str:
    match data:
        case [[a, b], [c, d]]:
            return f"2x2 {a + b + c + d}"
        case [[x], *rest]:
            return f"single {x} then {len(rest)}"
        case _:
            return "other"
print(nested([[1, 2], [3, 4]]), nested([[5], [1], [2, 3]]), nested([]))

for i in range(5):
    match i:
        case 0:
            continue
        case 3:
            break
        case n:
            print("n =", n)
print("done")
