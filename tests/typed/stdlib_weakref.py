import weakref


class Node:
    def __init__(self, name: str) -> None:
        self.name = name


def bye(tag: str) -> None:
    print("finalized", tag)


a = Node("a")
r = weakref.ref(a)
print(r() is a, r().name, weakref.proxy(a).name)
cache: weakref.WeakValueDictionary[str, Node] = weakref.WeakValueDictionary()
cache["a"] = a
b = Node("b")
cache["b"] = b
print(len(cache), "a" in cache, cache["b"].name, sorted(cache.keys()), cache.get("zz") is None)
del cache["b"]
print(len(cache), [k for k in cache])
ws: weakref.WeakSet[Node] = weakref.WeakSet()
ws.add(a)
ws.add(a)
print(len(ws), a in ws)
f = weakref.finalize(a, bye, "early")
print(f.alive)
f()
print(f.alive)
g = weakref.finalize(a, bye, "at exit")
print("end")
