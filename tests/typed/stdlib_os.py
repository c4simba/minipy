# os and os.path: folders, descriptors, stat, walk, scandir, errors (Linux: system calls;
# KolibriOS: function 70 behind the same module)
import os
import os.path
from os.path import join, splitext

base = join("build", "os_test_dir")


def clean(top: str) -> None:
    if not os.path.exists(top):
        return
    for d, dirs, files in os.walk(top, topdown=False):
        for f in files:
            os.remove(os.path.join(d, f))
        for sub in dirs:
            os.rmdir(os.path.join(d, sub))
    os.rmdir(top)


clean(base)
os.makedirs(join(base, "a", "b"))
os.makedirs(join(base, "a", "b"), exist_ok=True)
fd = os.open(join(base, "a", "f.txt"), os.O_WRONLY | os.O_CREAT | os.O_TRUNC)
print(os.write(fd, b"hello\nworld\n"))
os.close(fd)
fd = os.open(join(base, "a", "f.txt"), os.O_RDONLY)
print(os.read(fd, 5), os.lseek(fd, 0, os.SEEK_CUR), os.read(fd, 100), os.read(fd, 10))
print(os.lseek(fd, -6, os.SEEK_END), os.read(fd, 3))
st = os.fstat(fd)
os.close(fd)
print(st.st_size, os.path.getsize(join(base, "a", "f.txt")), os.path.isfile(join(base, "a", "f.txt")), os.path.isdir(join(base, "a")))
print(sorted(os.listdir(base)), sorted(os.listdir(join(base, "a"))))
print([(t[len(base):], sorted(d), sorted(f)) for t, d, f in sorted(os.walk(base))])
os.rename(join(base, "a", "f.txt"), join(base, "g.txt"))
print(sorted(e.name + ("/" if e.is_dir() else "") for e in os.scandir(base)))
with os.scandir(base) as it:
    print(sorted(e.name for e in it if e.is_file()))


def report(e: OSError) -> None:
    print(type(e).__name__, e.strerror, e.filename is not None and e.filename.startswith(base))


try:
    os.mkdir(base)
except OSError as e:
    report(e)
try:
    os.remove(join(base, "nope"))
except OSError as e:
    report(e)
try:
    os.listdir(join(base, "g.txt"))
except OSError as e:
    report(e)
try:
    os.rmdir(join(base, "a"))
except OSError as e:
    print(type(e).__name__, e.strerror)
print(splitext("x/y.tar.gz"), os.path.split("/a/b/"), os.path.normpath("a//b/../c/./d"), os.path.basename("/p/q.py"))
print(os.path.join("a", "b", "/c", "d"), os.path.dirname("a/b/c"), os.path.isabs("/x"), os.path.relpath("/a/b/c", "/a/x"))
print(os.path.commonprefix(["/usr/lib", "/usr/local"]), os.path.commonpath(["/usr/lib", "/usr/local"]), os.path.splitext(".bashrc"))
print(os.path.abspath(base) == os.path.join(os.getcwd(), base), os.path.exists("/definitely/not/here"), os.path.isdir("."))
print(os.sep, os.linesep == "\n", os.name, os.getenv("NO_SUCH_VAR_X", "dflt"), len(os.urandom(8)), os.getpid() > 0)
print(os.path.expanduser("x/~"), os.path.normpath("/../a"), os.path.splitroot("//x/y"))
os.remove(join(base, "g.txt"))
os.removedirs(join(base, "a", "b"))
print(os.path.exists(join(base, "a")))
