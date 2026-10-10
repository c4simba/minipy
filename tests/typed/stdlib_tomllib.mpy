# tomllib: a TOML document read into typed values (dataclasses, dicts, lists)
import tomllib


doc = """
# config
title = "TOML \\"Example\\" \\u00e9\\t!"
count = 1_000
ratio = 0.5
flag = true
hexv = 0xff
octv = 0o17
binv = 0b101
big = 6.02e23
neg = -17
words = ["a", 'b\\c', \"\"\"multi
line\"\"\", '''raw
text''']

[owner]
name = "Tom"   # trailing comment

[database]
ports = [ 8000,
  8001,   # second
]
inline = { x = 1, y = 2 }

[servers.alpha]
ip = "10.0.0.1"

[servers.beta]
ip = "10.0.0.2"
role = "frontend"

[[products]]
name = "Hammer"
sku = 738594937

[[products]]
name = "Nail"
tags = ["small", "metal"]
"""

top: dict[str, str] = tomllib.loads(doc.split("[owner]")[0].split("count")[0])
print(top)
nums: dict[str, int] = tomllib.loads("count = 1_000\nhexv = 0xff\noctv = 0o17\nbinv = 0b101\nneg = -17\npos = +5\n")
print(nums)
words: dict[str, list[str]] = tomllib.loads(doc.split("[owner]")[0].split("neg = -17")[1])
print(words)
owner: dict[str, dict[str, str]] = tomllib.loads(doc.split("[database]")[0].split("line\"\"\", \'\'\'raw\ntext\'\'\']")[1])
print(owner)
db: dict[str, dict[str, list[int]]] = tomllib.loads("[database]\nports = [ 8000,\n  8001,   # second\n]\nmore = [[1, 2], [3]][0]\n" if False else "[database]\nports = [ 8000,\n  8001,   # second\n]\n")
print(db)
servers: dict[str, dict[str, dict[str, str]]] = tomllib.loads(doc.split("[[products]]")[0].split("inline = { x = 1, y = 2 }")[1])
print(servers)
products: dict[str, list[dict[str, list[str]]]] = tomllib.loads("[[products]]\ntags = []\n\n[[products]]\ntags = [\"small\", \"metal\"]\n")
print(products)

# a plain dict of one value type
d: dict[str, int] = tomllib.loads("a = 1\nb = 2\n\"c d\" = 3\n")
print(d)
nested: dict[str, dict[str, list[int]]] = tomllib.loads("[x]\ny = [1, 2]\nz = []\n[w]\n")
print(nested)
dotted: dict[str, dict[str, dict[str, str]]] = tomllib.loads("a.b.c = 'x'\na.b.d = 'y'\n[q]\nr.s = 'z'\nr.t = 'w'\n")
print(dotted)
fl: dict[str, float] = tomllib.loads("i = inf\nn = -inf\nf = 1e3\ng = 3.5e-2\n")
print(fl)
empty: dict[str, int] = tomllib.loads("")
print(empty)

# errors
for bad in ("x = ", "a = 1\na = 2", "[t]\n[t]", "s = 'unterminated", "a = 1 b = 2", "= 1",
            "a = [1, 2", "a = {x = 1", "[a]\nb = 1\n[a.b]", "s = \"\\q\"", "s = \"\\u12\"",
            "a = { x = 1 }\n[a]", "[[t]]\n[t]", "[a]\nb.c = 1\n[a.b]", "a.b = 1\n[a]", "a = [1]\n[[a]]",
            "a = {}\na.b = 1", "[a]\nb = 1\n[a.b.c]", "t = 1\n[[t]]", "x.y = 1\n[z]\nw = 1\n[x.y.q]"):
    try:
        tomllib.loads(bad)
    except tomllib.TOMLDecodeError as e:
        print("TOMLDecodeError:", e, "|", repr(e.msg), e.lineno, e.colno, e.pos)

# load() from a binary file
with open("build/typed_toml_test.toml", "w") as f:
    f.write("name = \"file\"\r\nsku = '5'\n")
with open("build/typed_toml_test.toml", "rb") as fb:
    p: dict[str, str] = tomllib.load(fb)
print(p)
