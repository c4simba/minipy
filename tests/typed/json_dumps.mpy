# json.dumps: written by type at compile time; the output is CPython's
import json
from typing import Optional

class Point:
    def __init__(self, x: int, y: float, label: str):
        self.x = x
        self.y = y
        self.label = label

def item(item_id: int, q: Optional[str] = None):
    return {"item_id": item_id, "q": q}       # a record: values of different types

print(json.dumps({"message": "Hello World"}))
print(json.dumps({"Hello": "World"}, separators=(",", ":")))
print(json.dumps([1, 2, 3]), json.dumps("café \"q\" \\ \n\t\x01"), json.dumps("café", ensure_ascii=False))
print(json.dumps(item(5, "x")), json.dumps(item(6)))
print(json.dumps({"a": [1.5, 2.0], "b": {"c": True, "d": False}}))
print(json.dumps((1, "two", 3.0)), json.dumps({1: "one", 2: "two"}))
print(json.dumps([{"name": "ann", "age": 7}, {"name": "bob", "age": 9}], separators=(",", ":")))
print(json.dumps(float("inf")), json.dumps(float("nan")), json.dumps(-float("inf")), json.dumps(1e100), json.dumps(0.1))
print(json.dumps("\U0001F600"), json.dumps(""), json.dumps(None), json.dumps(True), json.dumps(-17))
print(json.dumps([[], [[]]]), json.dumps({}), json.dumps(["x", "y"], indent=None))
nested = {"user": {"name": "ann", "tags": ["a", "b"]}, "ok": True, "score": 9.5}
text = json.dumps(nested)
print(text, len(text))
