# error messages: NameError / AttributeError / ImportError attributes and CPython's "Did you mean" suggestions
import math
counter = 1
def f():
    local_value = 3
    return local_vlue
class K:
    def __init__(self):
        self.amount = 1
        self._secret = 2
    def m(self):
        return amount
    def p(self):
        return self._secrt
import traceback
for fn in [lambda: pritn("x"), lambda: countr, f, lambda: K().m(), lambda: K().p(), lambda: math.sqroot(1), lambda: sys.argv, lambda: os]:
    try:
        fn()
    except Exception as e:
        print(traceback.format_exc().splitlines()[-1])
        print(traceback.format_exception_only(e)[-1].rstrip())
try:
    from math import sqroot
except ImportError as e:
    print(traceback.format_exc().splitlines()[-1].split(" (")[0], traceback.format_exc().splitlines()[-1].endswith("?"))
try:
    undefined_x
except NameError as e:
    print(repr(e.name))
class A: pass
a = A()
try:
    a.zz
except AttributeError as e:
    print(repr(e.name), e.obj is a)
try:
    from os import nothing_here
except ImportError as e:
    print(repr(e.name), repr(e.name_from) if hasattr(e, "name_from") else "-", str(e).split(" (")[0])
try:
    import no_such_mod
except ImportError as e:
    print(type(e).__name__, repr(e.name))
e = AttributeError("m", name="n", obj=5)
print(e.name, e.obj, e.args)
print(NameError("q", name="nm").name)
