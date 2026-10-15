# pickle (interpreter): objects of the program's classes, copyreg, protocols 0-5, errors
import pickle
import copyreg
import io
from collections import OrderedDict


class P:
    def __init__(self, x, y):
        self.x = x
        self.y = y

    def __eq__(self, o):
        return isinstance(o, P) and (self.x, self.y) == (o.x, o.y)

    def __repr__(self):
        return "P(%r, %r)" % (self.x, self.y)


class S:
    __slots__ = ("a", "b")

    def __init__(self, a, b):
        self.a = a
        self.b = b

    def __getstate__(self):
        return (self.a, self.b)

    def __setstate__(self, st):
        self.a, self.b = st

    def __repr__(self):
        return "S(%r, %r)" % (self.a, self.b)


class R:
    def __init__(self, v):
        self.v = v

    def __reduce__(self):
        return (R, (self.v * 2,))

    def __repr__(self):
        return "R(%r)" % self.v


data = [1, -2, 3.5, "héllo", b"\x00\xff", None, True, (1, 2), {"a": [1, 2], 3: {4, 5}}, frozenset([1]), P(1, "two"),
        2 ** 40, -2 ** 62, bytearray(b"ab"), 1e300, S(1, [2]), R(5), OrderedDict(k=1)]
for proto in range(0, 6):
    s = pickle.dumps(data, protocol=proto)
    back = pickle.loads(s)
    print(proto, len(s), back[:13] == data[:13], back)
shared = [1, 2]
pair = pickle.loads(pickle.dumps([shared, shared]))
print(pair[0] is pair[1], pair)
rec = []
rec.append(rec)
r2 = pickle.loads(pickle.dumps(rec))
print(r2[0] is r2)
f = io.BytesIO()
pickle.dump({"x": P(3, 4)}, f, protocol=2)
f.seek(0)
print(pickle.load(f), f.tell() == len(f.getvalue()))
print(pickle.dumps(P(1, 2), 0))
print(P(1, 2).__reduce_ex__(2)[1:3], P(1, 2).__reduce_ex__(2)[0] is copyreg.__newobj__)
for bad in [b"", b"garbage", b"\x80\x05K", b"(I1\n"]:
    try:
        pickle.loads(bad)
    except Exception as e:
        print(type(e).__name__, e)
try:
    pickle.dumps(lambda: 1)
except Exception as e:
    print(type(e).__name__)
from collections import defaultdict, Counter
for obj in [defaultdict(list, a=[1]), defaultdict(None, b=2), Counter("abca"), OrderedDict(z=1, a=2)]:
    for proto in (0, 2, 5):
        s = pickle.dumps(obj, proto)
        print(proto, len(s), pickle.loads(s))
import copy
p2 = copy.deepcopy([P(1, [2]), S(3, 4), R(1)])
print(p2)
