# a dict literal with str keys and values of different types is a record:
# a fixed-shape dict (indexed by its keys, printed and compared like a dict)
from typing import Union

def read_item(item_id: int, q: Union[str, None] = None):
    return {"item_id": item_id, "q": q}

r = read_item(5, "x")
print(r)
print(r["item_id"] + 1, r["q"].upper(), len(r))
items = [read_item(1, "a"), read_item(2, "b")]
print(items, items[0] == read_item(1, "a"), items[0] != items[1])
nested = {"user": {"name": "ann", "age": 7}, "tags": ["x", "y"], "ok": True}
print(nested["user"], nested["user"]["age"], nested["tags"][1], nested["ok"])
same = {"a": "1", "b": "2"}                # one value type: an ordinary dict
same["c"] = "3"
print(same, len(same))
print(str(read_item(9)), repr(read_item(10, "z")))
q: Union[str, None] = None
print(q, str(q), [q], "q=" + str(q))
