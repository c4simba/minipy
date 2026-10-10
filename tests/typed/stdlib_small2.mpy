# errno, posixpath, reprlib (its own repr(): f-strings still use the built-in one), gc; ascii(), f-string conversions
import errno
import gc
import posixpath
import reprlib
from collections import deque

print(errno.ENOENT, errno.errorcode[errno.EEXIST], errno.EACCES, errno.EAGAIN == errno.EWOULDBLOCK)
print(posixpath.join("a", "b", "c.txt"), posixpath.join("/x", "/y", "z"), posixpath.splitext("x/y.tar.gz"))
print(posixpath.normpath("a//b/../c/./d"), posixpath.normpath("/../x"), posixpath.normpath(""))
print(posixpath.basename("/x/y"), posixpath.dirname("/x/y"), posixpath.isabs("/q"), posixpath.split("a/b"))
print(posixpath.commonprefix(["/usr/lib", "/usr/local"]), posixpath.relpath("/a/b/c", "/a/d"), posixpath.sep)

print(reprlib.repr(list(range(100))), reprlib.repr("x" * 100), reprlib.repr(10 ** 18))
print(reprlib.repr({"k" + str(i): i for i in range(10)}), reprlib.repr({3, 1, 2, 9, 8, 7, 6}))
print(reprlib.repr((1,)), reprlib.repr(()), reprlib.repr(set()), reprlib.repr(frozenset({2, 1})))
print(reprlib.repr([[[[[[[[1]]]]]]]]), reprlib.repr(deque([1, 2, 3, 4, 5, 6, 7, 8])), reprlib.repr([(1, "a")]))
print(reprlib.repr(3.25), reprlib.repr(True), reprlib.repr(None))
r = reprlib.Repr()
r.maxlist = 3
r.maxstring = 10
print(r.repr([1, 2, 3, 4, 5]), r.repr("abcdefghijklmnop"), r.repr(["abcdefghijklmnop"]))
r2 = reprlib.Repr(maxlevel=2, indent="  ")
print(r2.repr([[[1]], [[2, 3]]]), r2.repr({"a": [4]}), r2.repr([]))
r3 = reprlib.Repr(fillvalue="+", maxdict=1)
print(r3.repr({"a": 1, "b": 2}))


class Point:
    def __init__(self, x: int, y: int) -> None:
        self.x = x
        self.y = y

    @reprlib.recursive_repr()
    def __repr__(self) -> str:
        return f"Point({self.x}, {self.y})"


print(reprlib.repr(Point(1, 2)), reprlib.repr([Point(3, 4)]), reprlib.Repr(maxother=8).repr(Point(100, 200)))

print(gc.isenabled(), gc.collect() >= 0)
gc.disable()
print(gc.isenabled())
gc.enable()
print(gc.isenabled())

s = "héllo € \U0001f600 \x07"
print(ascii(s), ascii(["é", "b"]), ascii(5), ascii("plain"))
print(f"{s!a} {s!r} {s!s} {s}")
n = 5
print(f"{n!a:>5}|{n!r}|{'x'!r:>6}|")
