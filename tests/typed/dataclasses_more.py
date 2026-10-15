import dataclasses
from dataclasses import dataclass, field, InitVar

@dataclass(frozen=True)
class Money:
    amount: int
    currency: str = "USD"
    def __post_init__(self):
        if self.amount < 0:
            raise ValueError("negative")
        object.__setattr__(self, "currency", self.currency.upper())

@dataclass
class Inventory:
    name: str
    items: dict[str, int] = field(default_factory=dict)
    total: int = field(init=False, default=0)
    secret: str = field(default="s", repr=False)
    scale: InitVar[int] = 1
    def __post_init__(self, scale: int):
        self.total = sum(self.items.values()) * scale
    def add(self, k: str, n: int) -> None:
        self.items[k] = self.items.get(k, 0) + n
        self.total += n

def local_dc() -> str:
    @dataclass
    class Pair:
        a: int
        b: int
        def s(self) -> int:
            return self.a + self.b
    p = Pair(3, 4)
    return f"{p} {p.s()} {type(p).__qualname__}"

m = Money(5, "eur")
print(m, m == Money(5, "EUR"))
try:
    Money(-1)
except ValueError as e:
    print("error:", e)
try:
    m.amount = 3
except dataclasses.FrozenInstanceError as e:
    print(type(e).__name__, e, isinstance(e, AttributeError))
inv = Inventory("shop", {"a": 2}, scale=3)
inv.add("b", 5)
print(inv, inv.total)
print(Inventory("x"))
print(local_dc())
d = {Money(1): "one", Money(2, "eur"): "two"}
print(d[Money(1, "usd")], d[Money(2, "EUR")])
