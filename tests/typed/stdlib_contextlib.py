# contextlib: @contextmanager (functions, methods), closing, nullcontext, suppress, chdir
from contextlib import contextmanager, closing, nullcontext
import contextlib


@contextmanager
def tag(name: str):
    print("<" + name + ">")
    try:
        yield name.upper()
    finally:
        print("</" + name + ">")


with tag("b") as t:
    print("inside", t)

try:
    with tag("i"):
        raise ValueError("boom")
except ValueError as e:
    print("caught", e)


@contextlib.contextmanager
def swallow():
    try:
        yield 0
    except KeyError as e:
        print("swallowed", repr(e))


with swallow():
    raise KeyError("k")
print("after swallow")


@contextmanager
def twice():
    yield 1
    yield 2


try:
    with twice():
        pass
except RuntimeError as e:
    print("RuntimeError", e)


class Res:
    def __init__(self, name: str):
        self.name = name

    def close(self) -> None:
        print("Res closed", self.name)

    @contextmanager
    def opened(self, mode: str):
        print("open", self.name, mode)
        yield len(self.name)
        print("close", self.name)


with closing(Res("r1")) as r:
    print("using", r.name)
with nullcontext(42) as v:
    print("null", v)
with Res("file").opened("rb") as n:
    print("n =", n)
with tag("outer") as o, tag("inner") as i:
    print(o, i)

import os
with contextlib.suppress(FileNotFoundError, KeyError):
    os.remove("/nonexistent/file/xyz")
print("suppressed ok")
try:
    with contextlib.suppress(KeyError):
        raise IndexError("idx")
except IndexError as e2:
    print("not suppressed", e2)
cwd = os.getcwd()
with contextlib.chdir("/"):
    print(os.getcwd())
print(os.getcwd() == cwd)
