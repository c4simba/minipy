import io
import json


def total(d: dict[str, int]) -> int:
    return sum(d.values())


a: dict[str, int] = json.loads('{"a": 1, "b": 22}')
print(a, total(json.loads(' {"x": 5, "y": -6} ')), a["b"] + 1)
nested: dict[str, list[str | None]] = json.loads('{"k": ["v", null, "\\u00e9\\n\\ud83d\\ude00"], "e": []}')
print(nested, len(nested["k"][2]))
deep: list[dict[str, list[float]]] = json.loads('[{"p": [0.5, 1.25e1]}, {}]')
print(deep)
flag: bool = json.loads(" true ")
name: str = json.loads('"x\\ty\\"z"')
n: int = json.loads("-120")
f: float = json.loads("3.5")
print(flag, repr(name), n, f)
raw: dict[str, str] = json.loads(b'{"bytes": "ok"}')
print(raw)
for bad in ('', '{"a": }', '[1, 2', '{"a" 1}', '{"a": 1,}', '"abc', '{"a": 1} x', '{"a": "b\\q"}', '{a: 1}', '[1,]', 'nul'):
    try:
        v: dict[str, int] = json.loads(bad)
        print(v)
    except json.JSONDecodeError as e:
        print("JSONDecodeError:", e, e.pos, e.lineno, e.colno)
try:
    json.loads('{\n  "a": 1,\n  "b" 2\n}')
except ValueError as e:
    print("ValueError:", e)

buf = io.StringIO()
json.dump({"k": [1, 2, 3]}, buf)
print(buf.getvalue())
buf.seek(0)
back: dict[str, list[int]] = json.load(buf)
print(back, back["k"][2])
