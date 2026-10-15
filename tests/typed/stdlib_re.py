# re: typical uses (findall with groups through a module-level, a field and a from-import, sub with functions)
import re
from re import findall, sub, compile as rc

PAIR = re.compile(r"(\w+)=(\w+)")
WORD = re.compile(r"[A-Za-z]+")


class Tokenizer:
    def __init__(self):
        self.num = re.compile(r"(\d+)\.(\d+)")
        self.ident = rc(r"[a-z_]\w*", re.I)

    def numbers(self, s: str) -> list[tuple[str, ...]]:
        return self.num.findall(s)

    def idents(self, s: str) -> list[str]:
        return self.ident.findall(s)


def parse_kv(s: str) -> dict[str, str]:
    d: dict[str, str] = {}
    for k, v in PAIR.findall(s):
        d[k] = v
    return d


print(parse_kv("a=1 b=two c=3"))
print(WORD.findall("Hello, World! 42"))
t = Tokenizer()
print(t.numbers("pi=3.14 e=2.71"), t.idents("x1 = Foo_bar + 2"))
print(findall(r"(\d)(\w)", "1a 2b 3"), findall(r"\d", "1a2b"))
print(sub(r"\s+", " ", "  many   spaces\there  "))
print(re.sub(r"(?P<w>\w+)", lambda m: m.group("w")[::-1], "abc def"))
log = "2024-01-02 ERROR disk full\n2024-01-03 INFO ok\n2024-01-04 ERROR net down"
for line in log.splitlines():
    m = re.match(r"(\d{4})-(\d\d)-(\d\d) (\w+) (.*)", line)
    if m and m.group(4) == "ERROR":
        print(m.group(1), m.group(5).upper())
errs = [m.group(1) for m in re.finditer(r"ERROR (.*)", log)]
print(errs, len(re.findall(r"^\d", log, re.M)))
print(re.split(r"[;,]\s*", "a, b;c,  d"), re.subn(r"o", "0", "foo boo"))
ip = re.compile(r"^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$")
for s in ["192.168.0.1", "1.2.3", "10.0.0.256"]:
    mm = ip.match(s)
    print(s, mm is not None and all(int(x or "0") <= 255 for x in mm.groups()))
html = '<a href="x.html">X</a> <a href="y.html">Y</a>'
print(re.findall(r'href="([^"]+)"', html), re.findall(r'<a href="([^"]+)">([^<]*)</a>', html))
print(re.sub(r"\b(\w)(\w*)\b", lambda m: m.group(1).upper() + m.group(2), "hello big world"))
print(re.fullmatch(r"[\w.+-]+@[\w-]+\.[\w.]+", "a.b+c@ex-ample.co.uk") is not None)
print(re.escape("x.y"), re.compile(re.escape("a+b")).search("1a+b2"))
big = "abc " * 2000 + "needle 123"
print(re.search(r"needle (\d+)", big).group(1), len(re.findall(r"\w+", big)))
