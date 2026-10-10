# open() and io: text and binary files, newlines, seek/tell, modes, errors, StringIO, BytesIO
import io
import os

p = "build/io_test.txt"
with open(p, "w") as f:
    print(f.write("alpha\nbeta\r\ngamma\rdelta"), repr(f), f.closed, f.writable(), f.readable())
print(f.closed)
with open(p) as f:
    print(repr(f.read()))
with open(p, newline="") as f:
    print(f.readlines())
with open(p) as f:
    print(repr(f.readline()), f.tell(), repr(f.read(3)), f.tell(), repr(f.readline()), repr(f.read()))
    f.seek(0)
    print([line.rstrip("\n") for line in f])
with open(p, "rb") as b:
    data = b.read()
    print(data, repr(b), b.tell())
    b.seek(2)
    print(b.read(4), b.readline(), b.read(2), b.tell())
with open(p, "a") as f:
    f.write("\nmore é\n")
with open(p, "r+") as f:
    print(repr(f.read(5)))
    f.seek(0)
    f.write("ALPHA")
with open(p, encoding="utf-8") as f:
    print(f.read().split("\n")[0], f.encoding)
with open(p, "rb") as bf:
    print(len(bf.read()), os.path.getsize(p))
f = open(p, "w")
f.write("x" * 10)
f.truncate(4)
f.close()
print(open(p).read(), open(p, "rb").read())
with open(p, "w") as f:
    print("printed", 1, 2.5, sep=",", file=f)
    print("second", file=f, end="!\n")
print(open(p).read(), end="")
with open(p, "wb") as wb:
    wb.write(bytes(range(250, 256)) + b"\r\n")
with open(p, "rb") as rb:
    print(list(rb.read()))
try:
    open(p, "x")
except FileExistsError as e:
    print("x:", e)
try:
    open("build/no_such_dir/z.txt", "w")
except FileNotFoundError as e:
    print("w:", e)
try:
    open(".")
except IsADirectoryError as e:
    print("dir:", e)
try:
    open(p, "rw")
except ValueError as e:
    print("mode:", e)
f = open(p)
f.close()
try:
    f.read()
except ValueError as e:
    print("closed:", e)
s = io.StringIO()
s.write("one\n")
print("two", file=s)
print(repr(s.getvalue()), s.tell())
s.seek(0)
print(s.readline(), end="")
print(list(io.StringIO("a\nb\n")), io.BytesIO(b"xy\nz").readlines())
os.remove(p)
