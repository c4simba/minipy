def f():
    raise ExceptionGroup("many", [ValueError("v1"), TypeError("t1"), ValueError("v2")])
try:
    f()
except* ValueError as eg:
    print("values:", [str(e) for e in eg.exceptions])
except* TypeError as eg:
    print("types:", [str(e) for e in eg.exceptions], eg.message)
try:
    try:
        f()
    except* TypeError:
        print("handled types")
except ExceptionGroup as rest:
    print("rest:", rest.message, [repr(e) for e in rest.exceptions])
eg = ExceptionGroup("g", [KeyError("k"), ValueError("x")])
m, r = eg.split(ValueError)
print(repr(m), repr(r))
print(eg.subgroup(KeyError).exceptions, len(eg.exceptions), str(eg))
try:
    raise ValueError("plain")
except* ValueError as g:
    print("wrapped:", repr(g))
def boom():
    raise ExceptionGroup("eg", [ValueError(1), TypeError(2), KeyError(3)])
try:
    try:
        boom()
    except* ValueError:
        raise RuntimeError("from handler")
except ExceptionGroup as e:
    print("outer:", repr(e))
try:
    try:
        boom()
    except* TypeError:
        raise
except ExceptionGroup as e:
    print("reraised:", repr(e))
try:
    try:
        raise KeyError("solo")
    except* ValueError:
        print("no")
except KeyError as e:
    print("naked passes through:", repr(e))
def handled() -> str:
    try:
        boom()
    except* (ValueError, TypeError) as g:
        print("vt", len(g.exceptions))
    except* KeyError as g:
        print("k", g.exceptions)
    else:
        print("else")
    return "done"
print(handled())
try:
    raise BaseExceptionGroup("b", [KeyboardInterrupt()])
except BaseExceptionGroup as e:
    print(type(e).__name__, e)
try:
    ExceptionGroup("x", [KeyboardInterrupt()])
except TypeError as e:
    print(e)
eg = ExceptionGroup("n", [ValueError("a"), ExceptionGroup("inner", [TypeError("b"), ValueError("c")])])
print(repr(eg.subgroup(ValueError)), repr(eg.split(lambda e: isinstance(e, TypeError))[0]))
try:
    try:
        boom()
    except* ValueError:
        pass
    finally:
        print("finally runs")
except ExceptionGroup as e:
    print("left:", repr(e))
