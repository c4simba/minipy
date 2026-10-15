class JsonMixin:
    def to_json(self) -> str:
        return "{" + ", ".join(f'"{k}": {v!r}' for k, v in self.fields()) + "}"

class ReprMixin:
    def __repr__(self) -> str:
        return type(self).__name__ + "(" + ", ".join(f"{k}={v!r}" for k, v in self.fields()) + ")"

class Base:
    def __init__(self, name: str):
        self.name = name
    def fields(self) -> list[tuple[str, str]]:
        return [("name", self.name)]

class User(JsonMixin, ReprMixin, Base):
    def __init__(self, name: str, email: str):
        super().__init__(name)
        self.email = email
    def fields(self) -> list[tuple[str, str]]:
        return super().fields() + [("email", self.email)]

u = User("ann", "a@x")
print(u.to_json(), repr(u), isinstance(u, JsonMixin), isinstance(u, Base), isinstance(Base("b"), JsonMixin))
print([c.__name__ for c in User.__mro__])

class A:
    def hello(self) -> str:
        return "A"
class B(A):
    def hello(self) -> str:
        return "B>" + super().hello()
class C(A):
    def hello(self) -> str:
        return "C>" + super().hello()
class D(B, C):
    def hello(self) -> str:
        return "D>" + super().hello()
print(D().hello(), B().hello(), C().hello(), [k.__name__ for k in D.__mro__])

class Counted:
    count = 0
    def bump(self) -> int:
        Counted.count += 1
        return Counted.count

class Logger:
    prefix = "[log]"
    def log(self, msg: str) -> str:
        return self.prefix + " " + msg

class Service(Logger, Counted):
    def __init__(self, n: str):
        self.n = n
    def run(self) -> str:
        return self.log(self.n + " #" + str(self.bump()))

s1 = Service("a"); s2 = Service("b")
print(s1.run(), s2.run(), s1.run(), Service.prefix)

class Readable:
    def __init__(self) -> None:
        self.data = "r"
class Writable:
    def __init__(self) -> None:
        self.out = "w"
class Stream(Readable, Writable):
    def __init__(self) -> None:
        Readable.__init__(self)
        Writable.__init__(self)
st = Stream()
print(st.data, st.out, isinstance(st, Writable))
