# metaclasses (__new__, __prepare__, class keywords, metaclass methods on classes) and the
# interpreter-only parts of contextlib (ExitStack, redirect_stdout, ContextDecorator)
class Meta(type):
    def __new__(mcls, name, bases, ns, **kw):
        ns["added"] = 42
        cls = super().__new__(mcls, name, bases, ns)
        return cls

    def __iter__(cls):
        return iter([1, 2, 3])

    def __len__(cls):
        return 3

    def __getitem__(cls, k):
        return k * 2

    def __contains__(cls, x):
        return x == 1

    @classmethod
    def __prepare__(mcls, name, bases, **kw):
        return {"prepared": True}


class A(metaclass=Meta):
    x = 1

print(A.added, A.prepared, list(A), len(A), A[5], 1 in A, type(A).__name__)


class B:
    def __init_subclass__(cls, flag=False, **kw):
        cls.flag = flag
        super().__init_subclass__(**kw)


class C(B, flag=True):
    pass

print(C.flag)


class D:
    def __set_name__(self, owner, name):
        self.name = name

    def __get__(self, obj, typ=None):
        return "get:" + self.name


class E:
    attr = D()

print(E().attr, E.attr)


def gen():
    try:
        yield 1
    except ValueError as e:
        yield "caught " + str(e)
    finally:
        print("fin")

g = gen()
print(next(g), g.throw(ValueError("x")))
g.close()
import sys
o = object.__new__(E)
print(type(o).__name__, isinstance(o, E))
print(type("X", (object,), {"a": 1})().a)

import contextlib
import io
import os
import sys
from contextlib import contextmanager, suppress, closing, nullcontext, ExitStack, redirect_stdout


@contextmanager
def tag(name):
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


@contextmanager
def swallow():
    try:
        yield
    except KeyError as e:
        print("swallowed", repr(e))


with swallow():
    raise KeyError("k")
print("after swallow")


@contextmanager
def bad():
    if False:
        yield


try:
    with bad():
        pass
except RuntimeError as e:
    print("RuntimeError", e)


@contextmanager
def twice():
    yield 1
    yield 2


try:
    with twice():
        pass
except RuntimeError as e:
    print("RuntimeError", e)

with suppress(FileNotFoundError, KeyError):
    os.remove("/nonexistent/file/xyz")
print("suppressed ok")


class Res:
    def close(self):
        print("Res closed")


with closing(Res()) as r:
    print("using", type(r).__name__)
with nullcontext(42) as v:
    print("null", v)

buf = io.StringIO()
with redirect_stdout(buf):
    print("hidden")
print("captured", repr(buf.getvalue()))

with ExitStack() as stack:
    for n in ["x", "y"]:
        stack.enter_context(tag(n))
    stack.callback(print, "callback ran")
    print("body")


@tag("deco")
def decorated():
    print("in decorated")


decorated()
print(isinstance(tag("q"), contextlib.AbstractContextManager), isinstance(Res(), contextlib.AbstractContextManager))
with ExitStack() as st:
    st.push(lambda *exc: print("push exit", exc[0]))
try:
    with ExitStack() as st2:
        st2.callback(print, "cleanup")
        raise ZeroDivisionError("z")
except ZeroDivisionError:
    print("zde")
s3 = ExitStack()
s3.callback(print, "closed later")
s4 = s3.pop_all()
s3.close()
s4.close()
cwd = os.getcwd()
with contextlib.chdir("/"):
    print(os.getcwd())
print(os.getcwd() == cwd)
