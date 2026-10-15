# del leaves a hole in a dict's entries (as in CPython): lookups go on past
# it, a walk in order (for, keys/values/items, ==, repr, json, update, copy)
# never sees one, and a full table squeezes the holes out - checked against
# CPython over random insertions and deletions of int, str and tuple keys.
import json


class Rand:
    def __init__(self, seed: int):
        self.x = seed

    def next(self, n: int) -> int:
        self.x = (self.x * 1103515245 + 12345) % 2147483648
        return self.x // 65536 % n


def ints(r: Rand) -> str:
    d: dict[int, int] = {}
    sig = 0
    for step in range(4000):
        k = r.next(300)
        op = r.next(10)
        if op < 5:
            d[k] = step
        elif op < 8:
            if k in d:
                del d[k]
        elif op == 8:
            sig += d.pop(k, -1)
        else:
            sig += d.setdefault(k, step)
        if step % 500 == 0:
            sig = sig * 31 % 1000003 + len(d) + sum(d.keys()) % 1000 + sum(d.values()) % 1000
    order = [k for k in d]
    return str(len(d)) + " " + str(sig) + " " + str(order[:10]) + " " + str(list(d.items())[-5:])


def strs(r: Rand) -> str:
    d: dict[str, list[int]] = {}
    for step in range(3000):
        k = "k" + str(r.next(120))
        if r.next(3) == 0:
            if k in d:
                del d[k]
        else:
            if k not in d:
                d[k] = []
            d[k].append(step)
    first = ""
    for k, v in d.items():
        if not first:
            first = k + "=" + str(len(v))
    return str(len(d)) + " " + first + " " + json.dumps(d)[:80]


def tuples(r: Rand) -> str:
    d: dict[tuple[int, str], float] = {}
    for step in range(2000):
        k = (r.next(40), "t" + str(r.next(3)))
        if r.next(2) == 0:
            d[k] = step / 4
        elif k in d:
            del d[k]
    return str(len(d)) + " " + repr(d)[:100]


def drain() -> str:
    d = {i: str(i) for i in range(50)}
    for i in range(0, 50, 2):
        del d[i]
    e = d.copy()
    f: dict[int, str] = {}
    f.update(d)
    out = [str(d == e), str(e == f), str(len(e)), str(list(f)[:5])]
    for i in range(1, 50, 2):
        del d[i]
    empty: dict[int, str] = {}
    out.append(str(d) + str(len(d)) + str(d == empty))
    d[7] = "seven"
    d[3] = "three"
    out.append(str(d))
    for i in range(100):                       # in and out: the holes are squeezed out when the table fills
        d[100 + i] = "x"
        del d[100 + i]
    out.append(str(d) + str(len(d)))
    out.append(d.pop(7) + d.pop(8, "-") + str(d))
    d.clear()
    d[1] = "one"
    out.append(str(d))
    g = {"a": 1, "b": 2, "c": 3}
    del g["a"]
    g["a"] = 4
    del g["c"]
    out.append(str(g) + str(list(g.values())) + str([k + str(v) for k, v in g.items()]))
    try:
        del g["zz"]
    except KeyError as e2:
        out.append("KeyError " + str(e2))
    return " | ".join(out)


r = Rand(7)
print(ints(r))
print(strs(r))
print(tuples(r))
print(drain())
