# tarfile: tar archives (ustar, GNU, pax; gzip), in memory and on disk
import io
import os
import shutil
import tarfile

d = os.path.join("build", "tarfile_test")
if os.path.exists(d):
    shutil.rmtree(d)
os.makedirs(os.path.join(d, "src", "sub"))
with open(os.path.join(d, "src", "a.txt"), "w") as ft:
    ft.write("hello\nworld\n")
with open(os.path.join(d, "src", "sub", "b.bin"), "wb") as fb:
    fb.write(bytes(range(256)) * 3)


def member(name: str, data: bytes, mtime: int = 1700000000) -> tarfile.TarInfo:
    ti = tarfile.TarInfo(name)
    ti.size = len(data)
    ti.mtime = mtime
    ti.mode = 0o640
    ti.uname = "user"
    ti.gname = "group"
    return ti


# archives built in memory: the bytes CPython writes
for fmt in (tarfile.USTAR_FORMAT, tarfile.GNU_FORMAT, tarfile.PAX_FORMAT):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=fmt) as tw:
        tw.addfile(member("short.txt", b"abc"), io.BytesIO(b"abc"))
        tw.addfile(member("d1/" + "x" * 60 + "/" + "y" * 60 + ".txt", b"0123456789" * 60), io.BytesIO(b"0123456789" * 60))
        di = tarfile.TarInfo("adir")
        di.type = tarfile.DIRTYPE
        di.mode = 0o755
        di.mtime = 1700000001
        tw.addfile(di)
        li = tarfile.TarInfo("link")
        li.type = tarfile.SYMTYPE
        li.linkname = "short.txt"
        tw.addfile(li)
    raw = buf.getvalue()
    print(fmt, len(raw), sum(raw) % 100003, raw[:20], raw[156:157], raw[257:265])
    buf.seek(0)
    with tarfile.open(fileobj=buf) as tr:
        for m in tr.getmembers():
            print(" ", m.name[:40], m.size, int(m.mtime), oct(m.mode), m.type, m.uname, m.gname, m.isfile(),
                  m.isdir(), m.issym(), m.linkname, m.offset, m.offset_data)
        ef = tr.extractfile("short.txt")
        if ef is not None:
            print(" ", ef.read())
        print(" ", tr.getnames()[-2:], tr.getmember("adir").isdir())
        lf = tr.extractfile("link")
        if lf is not None:
            print("  via link", lf.read())

pax = io.BytesIO()
with tarfile.open(fileobj=pax, mode="w") as tw:
    tw.addfile(member("ünïcödé/naïve.txt", b"x"), io.BytesIO(b"x"))
    tw.addfile(member("big-uid", b""))
raw = pax.getvalue()
print(len(raw), raw.count(b"path="), raw[512:600])
pax.seek(0)
with tarfile.open(fileobj=pax, mode="r:") as tr:
    print(tr.getnames(), tr.getmembers()[0].pax_headers)

# files on disk, gzip
tp = os.path.join(d, "x.tar")
with tarfile.open(tp, "w") as tw:
    tw.add(os.path.join(d, "src"), arcname="src")
print(os.path.getsize(tp))
with tarfile.open(tp) as tr:
    for m in tr:
        print(m.name, m.size, m.isdir(), m.isfile(), oct(m.mode & 0o777))
    ef = tr.extractfile("src/a.txt")
    if ef is not None:
        print(ef.readline(), ef.readlines(), ef.tell())
    tr.extractall(os.path.join(d, "out"))
print(sorted(os.listdir(os.path.join(d, "out", "src"))))
with open(os.path.join(d, "out", "src", "sub", "b.bin"), "rb") as fb:
    print(len(fb.read()))

gz = os.path.join(d, "x.tar.gz")
with tarfile.open(gz, "w:gz") as tw:
    tw.add(os.path.join(d, "src", "a.txt"), arcname="a.txt")
    tw.add(os.path.join(d, "src", "sub"), arcname="sub", recursive=False)
with open(gz, "rb") as fb:
    print(fb.read(2))
with tarfile.open(gz) as tr:
    print(tr.getnames())
with tarfile.open(gz, "r:gz") as tr:
    tr.extract("a.txt", os.path.join(d, "one"))
with open(os.path.join(d, "one", "a.txt")) as ft:
    print(repr(ft.read()))
print(tarfile.is_tarfile(tp), tarfile.is_tarfile(gz), tarfile.is_tarfile(os.path.join(d, "src", "a.txt")))

# appending
with tarfile.open(tp, "a") as ta:
    ta.addfile(member("added.txt", b"new"), io.BytesIO(b"new"))
with tarfile.open(tp) as tr:
    print(tr.getnames()[-2:])

# filters, errors
evil = io.BytesIO()
with tarfile.open(fileobj=evil, mode="w") as tw:
    tw.addfile(member("../escape.txt", b"bad"), io.BytesIO(b"bad"))
    tw.addfile(member("/abs.txt", b"abs"), io.BytesIO(b"abs"))
evil.seek(0)
with tarfile.open(fileobj=evil) as tr:
    try:
        tr.extract("../escape.txt", os.path.join(d, "evil"))
    except tarfile.FilterError as e:
        print(type(e).__name__, str(e).startswith("'../escape.txt' would be extracted to '"),
              str(e).endswith("escape.txt', which is outside the destination"))
    tr.extract("/abs.txt", os.path.join(d, "evil"))
    print(os.listdir(os.path.join(d, "evil")))
    try:
        tr.getmember("nope")
    except KeyError as e:
        print("KeyError", e)
try:
    tarfile.open(os.path.join(d, "src", "a.txt"))
except tarfile.ReadError as e:
    print("ReadError", str(e).split("\n")[0])
try:
    tarfile.open(tp, "r:bz3")
except tarfile.CompressionError as e:
    print("CompressionError", e)
ti = tarfile.TarInfo("x")
print(ti.name, ti.size, ti.mode, ti.type, ti.isfile(), len(ti.tobuf()), ti.tobuf()[148:156])
shutil.rmtree(d)
