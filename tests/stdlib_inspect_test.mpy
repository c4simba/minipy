# inspect (interpreter): signatures, predicates, members, docs, source, frames, generator states
import inspect
import functools

def f(a, b: int = 2, *args, c, d=4, **kw) -> str:
    """Doc of f.

        indented
    """
    return "x"

class C:
    """A class."""
    def __init__(self, x, y=1):
        self.x = x
    def m(self, z, *, k=3):
        return z
    @staticmethod
    def s(q):
        pass
    @classmethod
    def c(cls, w):
        pass
    @property
    def p(self):
        return 1

def gen():
    yield 1

async def co():
    pass

print(inspect.signature(f), inspect.signature(C), inspect.signature(C.m), inspect.signature(C(1).m), inspect.signature(C.s), inspect.signature(C.c))
sig = inspect.signature(f)
for name, p in sig.parameters.items():
    print(name, p.kind, p.kind.description, p.default, p.annotation, str(p))
print(sig.return_annotation, sig.bind(1, 2, 3, c=5, z=9), sig.bind_partial(1).arguments)
ba = sig.bind(1, c=7)
ba.apply_defaults()
print(ba.arguments, ba.args, ba.kwargs)
try:
    sig.bind()
except TypeError as e:
    print("TypeError", e)
print(inspect.isfunction(f), inspect.isclass(C), inspect.ismethod(C(1).m), inspect.isgeneratorfunction(gen), inspect.iscoroutinefunction(co),
      inspect.isgenerator(gen()), inspect.isbuiltin(len), inspect.isroutine(len), inspect.ismodule(inspect))
print(inspect.getdoc(f), repr(inspect.cleandoc("  a\n    b\n  c")), inspect.getdoc(C))
print([n for n, v in inspect.getmembers(C) if not n.startswith("__")], inspect.getmro(C))
print(inspect.getsource(f))
print(inspect.getsource(C.m))
lines, start = inspect.getsourcelines(gen)
print(lines, start)
print(inspect.getfile(f) == __file__, inspect.getmodule(f).__name__, inspect.getmodulename("/a/b/x.py"))
fr = inspect.currentframe()
print(fr.f_code.co_name, inspect.getframeinfo(fr).function, [i.function for i in inspect.stack()][:1])
g = gen()
print(inspect.getgeneratorstate(g))
next(g)
print(inspect.getgeneratorstate(g))
p = functools.partial(f, 1, c=2)
print(inspect.signature(p))
print(inspect.Parameter("x", inspect.Parameter.KEYWORD_ONLY, default=1), inspect.Signature([inspect.Parameter("a", inspect.Parameter.POSITIONAL_ONLY)]))
print(inspect.formatannotation(int), inspect.formatannotation(list[int]), inspect.unwrap(functools.wraps(f)(lambda: 1)) is f)
