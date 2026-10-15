import gzip
import os
import tempfile
data = b"line one\nline two\n" * 50
c = gzip.compress(data)
print(c[:4], gzip.decompress(c) == data, gzip.decompress(c + gzip.compress(b"more")) == data + b"more")
cp = bytes.fromhex("1f8b08000000000002ff4b4c4a0600c241243503000000")
print(gzip.decompress(cp))
path = os.path.join(tempfile.gettempdir(), "minipy_gz_test.gz")
with gzip.open(path, "wb") as f:
    f.write(data)
with gzip.open(path, "rb") as g:
    print(g.read(9), g.readline(), len(g.read()))
with gzip.open(path, "wt", encoding="utf-8") as t:
    t.write("héllo\nwörld\n")
with gzip.open(path, "rt", encoding="utf-8") as r:
    print([line for line in r])
os.remove(path)
for bad in (b"xx", cp[:12]):
    try:
        gzip.decompress(bad)
    except (gzip.BadGzipFile, EOFError) as e:
        print(type(e).__name__ + ":", e)
