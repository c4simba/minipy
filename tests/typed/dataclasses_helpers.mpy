import json
from dataclasses import dataclass, field, asdict, astuple, replace, is_dataclass

@dataclass
class Item:
    name: str
    qty: int = 1

@dataclass
class Order:
    id: int
    items: list[Item] = field(default_factory=list)
    note: str | None = None
    meta: dict[str, int] = field(default_factory=dict)
    best: Item | None = None

o = Order(7, [Item("a", 2), Item("b")], meta={"x": 1})
o.best = o.items[0]
print(asdict(o))
print(astuple(o))
print(asdict(Item("z")), astuple(Item("z", 3)))
o2 = replace(o, id=8, note="n")
print(o2.id, o2.note, o2.items is o.items, o == o2)
def mk() -> Item:
    return Item("m", 5)
print(replace(mk(), qty=9), is_dataclass(o), is_dataclass(5), is_dataclass(Item))
print(json.dumps(asdict(Item("j", 4))))
d = asdict(o)
print(d["id"], len(d["items"]))
