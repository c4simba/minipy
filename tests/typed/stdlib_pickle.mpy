# pickle: protocols 0-5 for the built-in types (compiled: loads() reads into the wanted type)
import pickle
from pickle import loads

data = (1, 'a', b'xy', 2**40, -300, 70000, 1.5, True, None)
for p in range(6):
    print(p, pickle.dumps(data, protocol=p))
d = {"k": [1, 2], "z": []}
print(pickle.dumps(d), pickle.dumps((1, "a", 2.5)), pickle.dumps({1, 2, 3}), pickle.dumps(frozenset([7])))
print(pickle.dumps({3: "x", 4: "y"}, 2), pickle.dumps([[1], [1]], 4), pickle.dumps("a\\b\nc\x00€", 0))
s = "shared"
print(pickle.dumps([s, s, s]))
back: dict[str, list[int]] = pickle.loads(pickle.dumps(d))
print(back)
t: tuple[int, str, float] = loads(pickle.dumps((1, "a", 2.5), 2))
print(t)
for p in range(6):
    xs: list[int] = pickle.loads(pickle.dumps([1, 2**40, -5, 300], p))
    bs: bytes = pickle.loads(pickle.dumps(b"\x00\xffabc", p))
    st: set[int] = pickle.loads(pickle.dumps({5, 6}, p))
    di: dict[int, str] = pickle.loads(pickle.dumps({1: "a", 2: "b"}, p))
    fl: list[float | None] = pickle.loads(pickle.dumps([1.5, None, 2.0], p))
    print(p, xs, bs, sorted(st), di, fl)
big = list(range(3000))
print(len(pickle.dumps(big)), pickle.dumps(big)[:20])
r2: list[int] = pickle.loads(pickle.dumps(big))
print(r2 == big)
try:
    bad: int = pickle.loads(b"\x80\x05K")
except pickle.UnpicklingError as e:
    print("UnpicklingError", e)
for b in [b"", b"\x80\x05", b"\x80\x05K", b"I1"]:
    try:
        v: int = pickle.loads(b)
        print(v)
    except pickle.UnpicklingError as e:
        print("UnpicklingError", e)
    except EOFError as e:
        print("EOFError", e)
print(pickle.HIGHEST_PROTOCOL, pickle.DEFAULT_PROTOCOL)
