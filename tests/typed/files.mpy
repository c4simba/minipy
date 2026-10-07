# files: open / write / read / readline / readlines / iteration / with / append
path = "build/typed_io_test.txt"
f = open(path, "w")
n = f.write("first line\n")
f.write("second line\n")
f.close()
print(n)
with open(path, "a") as f:
    f.write("third line\n")
with open(path) as f:
    print(repr(f.read()))
f = open(path, "r")
print(repr(f.readline()), repr(f.readline()))
print(f.readlines())
print(repr(f.readline()))
f.close()
count = 0
for line in open(path):
    count += 1
    print(count, line.strip())
with open(path, encoding="utf-8") as f:
    lines = [l.rstrip("\n") for l in f.readlines()]
print(lines, len(lines))
try:
    open("build/no_such_file.txt")
except FileNotFoundError as e:
    print("missing:", e)
except OSError:
    print("other error")
