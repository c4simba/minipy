# exceptions: chaining, context, finally semantics, custom exceptions, with statements, assert
import contextlib, sys
class MyError(Exception):
    def __init__(self, code, msg="bad"):
        super().__init__(msg); self.code = code
def raiser(kind):
    if kind == 1: raise MyError(42)
    if kind == 2: raise ValueError("v") from KeyError("k")
    if kind == 3:
        try:
            1 / 0
        except ZeroDivisionError:
            raise RuntimeError("during handling")
    if kind == 4: raise TypeError
    return "ok"
for k in range(5):
    try:
        r = raiser(k)
    except MyError as e:
        print("MyError", e.code, e, e.args)
    except (ValueError, TypeError) as e:
        print(type(e).__name__, repr(e.__cause__), e.__suppress_context__)
    except Exception as e:
        print("other", type(e).__name__, type(e.__context__).__name__)
    else:
        print("else", r)
    finally:
        print("finally", k)
try:
    e
except NameError as ne:
    print("e deleted:", ne)
def fin_return():
    try:
        return "try"
    finally:
        print("finally runs")
def fin_override():
    try:
        return 1
    finally:
        return 2
def fin_loop():
    out = []
    for i in range(5):
        try:
            if i == 1: continue
            if i == 3: break
            out.append(i)
        finally:
            out.append("f%d" % i)
    return out
print(fin_return(), fin_override(), fin_loop())
def nested():
    try:
        try:
            raise ValueError("inner")
        finally:
            print("inner finally", sys.exception())
    except ValueError as e:
        print("caught", e, sys.exception() is e)
    print("after", sys.exception())
nested()
@contextlib.contextmanager
def managed(name):
    print("enter", name)
    try:
        yield name.upper()
    except KeyError:
        print("suppressed in", name)
    finally:
        print("exit", name)
with managed("a") as A, managed("b") as B:
    print("body", A, B)
with managed("c"):
    raise KeyError("x")
class Suppress:
    def __enter__(self): return self
    def __exit__(self, t, v, tb):
        print("exit with", t.__name__ if t else None); return True
with Suppress():
    raise IndexError
print("after suppress")
try:
    assert 1 == 2, "math is broken"
except AssertionError as e:
    print("assert:", e)
try:
    raise MyError(1) from None
except MyError as e:
    print(e.__cause__, e.__suppress_context__)
def reraiser():
    try:
        raise LookupError("orig")
    except LookupError:
        raise
try:
    reraiser()
except LookupError as e:
    print("reraised", e)
try:
    {}["k"]
except KeyError as e:
    print("KeyError", e)
try:
    [].pop()
except IndexError as e:
    print(e)
try:
    int("x")
except ValueError as e:
    print(e)
try:
    raise ExceptionGroup("grp", [ValueError(1), TypeError(2)])
except* ValueError as eg:
    print("star value", eg.exceptions)
except* TypeError as eg:
    print("star type", eg.exceptions)
