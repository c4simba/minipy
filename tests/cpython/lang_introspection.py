# what libraries look at: inspect, pickle, copy, typing, frames
import inspect, pickle, copy, sys, typing, functools, dataclasses

def documented(a: int, b: str = "x", *rest, flag: bool = False, **extra) -> dict:
    """A documented function."""
    return {"a": a}

class Model:
    """A model."""
    def __init__(self, v): self.v = v
    def __eq__(self, o): return type(o) is Model and o.v == self.v
    def __reduce__(self): return (Model, (self.v,))

print(inspect.signature(documented))
print(inspect.getdoc(documented), inspect.getdoc(Model))
print(inspect.getsource(documented).splitlines()[0])
print(inspect.isfunction(documented), inspect.ismethod(Model(1).__init__), inspect.isclass(Model))
print(typing.get_type_hints(documented))
print(pickle.loads(pickle.dumps(Model(5))) == Model(5), pickle.loads(pickle.dumps(documented)) is documented)
print(copy.deepcopy([Model(1), {"k": Model(2)}])[1]["k"].v)
print(documented.__module__, documented.__qualname__, Model.__init__.__qualname__, Model.__module__)
def caller_module(): return sys._getframe(1).f_globals["__name__"]
print(caller_module())
@functools.wraps(documented)
def wrapper(*a, **k): return documented(*a, **k)
print(inspect.signature(wrapper), wrapper.__wrapped__ is documented)
@dataclasses.dataclass
class DC:
    name: str
    size: int = 1
print(inspect.signature(DC), DC("n"), [f.type for f in dataclasses.fields(DC)])
print(inspect.iscoroutinefunction(documented), callable(documented), documented.__defaults__, documented.__kwdefaults__)
gen = (x for x in range(3)); print(inspect.isgenerator(gen), list(gen))
print(type(documented).__name__, documented.__code__.co_argcount, documented.__code__.co_kwonlyargcount, documented.__code__.co_varnames[:2])
