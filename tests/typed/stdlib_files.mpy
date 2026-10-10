"""Files and folders: fnmatch, glob, shutil, stat, tempfile, pathlib (relative paths under build/,
so that KolibriOS runs it too)."""
import os
import glob
import fnmatch
import shutil
import stat
import tempfile
from tempfile import NamedTemporaryFile
from pathlib import Path, PurePath, PurePosixPath


for name, pat in [("abc.txt", "*.txt"), ("abc.txt", "a?c.*"), ("abc", "[ab]bc"), ("cbc", "[!ab]bc"), ("-", "[a-]"), ("b", "[a-c]"),
                  ("]", "[]]"), ("a]", "[!]]]"), ("x", "[z-a]"), ("[", "["), ("[x", "[x"), ("a.TXT", "*.txt"), ("", "*"),
                  ("aaaab", "*a*b"), ("abcabd", "*abd"), ("abc", "a**c"), ("ab/c", "a*c"), (".hidden", "*"), ("e", "[--0]"), ("/", "[--0]")]:
    print(repr(name), repr(pat), fnmatch.fnmatch(name, pat), fnmatch.fnmatchcase(name, pat))
print(fnmatch.filter(["a.py", "b.txt", "c.py"], "*.py"), fnmatch.filterfalse(["a.py", "b.txt", "c.py"], "*.py"))

base = "build/glob_test"
for d in ["", "/sub", "/sub/deep", "/.hid", "/other"]:
    os.makedirs(base + d, exist_ok=True)
for gname in ["/a.txt", "/b.py", "/.dot.txt", "/sub/c.txt", "/sub/deep/d.txt", "/sub/deep/e.py", "/.hid/f.txt", "/other/g.txt", "/x[1].txt"]:
    fh = open(base + gname, "w")
    fh.write(gname)
    fh.close()
def rel(ps: list[str]) -> list[str]:
    return sorted(p[len(base):] for p in ps)


print(rel(glob.glob(base + "/*.txt")))
print(rel(glob.glob(base + "/*")))
print(rel(glob.glob(base + "/*/*.txt")))
print(rel(glob.glob(base + "/**/*.txt", recursive=True)))
print(rel(glob.glob(base + "/**", recursive=True)))
print(rel(glob.glob(base + "/**/", recursive=True)))
print(rel(glob.glob(base + "/*", include_hidden=True)))
print(sorted(glob.glob("*.txt", root_dir=base)), sorted(glob.glob("**/*.py", root_dir=base, recursive=True)))
print(rel(glob.glob(base + "/s?b/")), glob.glob(base + "/nothing*"), rel(glob.glob(base + "/a.txt")), glob.glob(base + "/zzz.txt"))
print(glob.escape("x[1]*?.txt"), rel(glob.glob(base + "/" + glob.escape("x[1].txt"))), glob.has_magic("a[b"), glob.has_magic("abc"))
print(rel(list(glob.iglob(base + "/sub/**/*", recursive=True))))
for gname in ["/a.txt", "/b.py", "/.dot.txt", "/sub/c.txt", "/sub/deep/d.txt", "/sub/deep/e.py", "/.hid/f.txt", "/other/g.txt", "/x[1].txt"]:
    os.remove(base + gname)
for d in ["/sub/deep", "/sub", "/.hid", "/other", ""]:
    os.rmdir(base + d)


base = os.path.join("build", "shutil_test")
if os.path.exists(base):
    shutil.rmtree(base)
os.makedirs(os.path.join(base, "src", "sub", "deep"))
for name, text in [("src/a.txt", "alpha"), ("src/b.py", "print(1)"), ("src/sub/c.txt", "gamma"), ("src/sub/deep/d.log", "delta" * 1000)]:
    f = open(os.path.join(base, name), "w")
    f.write(text)
    f.close()
os.utime(os.path.join(base, "src", "a.txt"), (1000000000, 1100000000))

def tree(top: str) -> list[str]:
    out: list[str] = []
    for d, dirs, files in os.walk(top):
        for f in files:
            p = os.path.join(d, f)
            out.append(os.path.relpath(p, top) + ":" + str(os.path.getsize(p)))
    return sorted(out)

print(shutil.copyfile(os.path.join(base, "src", "a.txt"), os.path.join(base, "a2.txt")))
print(shutil.copy(os.path.join(base, "src", "b.py"), base))
dst = shutil.copy2(os.path.join(base, "src", "a.txt"), os.path.join(base, "a3.txt"))
print(dst, os.stat(dst).st_mtime, os.stat(os.path.join(base, "a2.txt")).st_mtime != 1100000000.0)
try:
    shutil.copyfile(os.path.join(base, "a2.txt"), os.path.join(base, "a2.txt"))
except shutil.SameFileError as e:
    print("SameFileError", e)
try:
    shutil.copyfile(os.path.join(base, "nope.txt"), os.path.join(base, "x.txt"))
except FileNotFoundError as e:
    print("FileNotFoundError", e.errno)
print(shutil.copytree(os.path.join(base, "src"), os.path.join(base, "copy")))
print(tree(os.path.join(base, "copy")))
print(shutil.copytree(os.path.join(base, "src"), os.path.join(base, "copy2"), ignore=shutil.ignore_patterns("*.log", "b*")))
print(tree(os.path.join(base, "copy2")))
try:
    shutil.copytree(os.path.join(base, "src"), os.path.join(base, "copy"))
except FileExistsError as e:
    print("FileExistsError", e.errno)
print(shutil.copytree(os.path.join(base, "src"), os.path.join(base, "copy"), dirs_exist_ok=True))
print(shutil.move(os.path.join(base, "copy2"), os.path.join(base, "moved")), os.path.exists(os.path.join(base, "copy2")))
print(shutil.move(os.path.join(base, "a3.txt"), os.path.join(base, "moved")))
print(tree(os.path.join(base, "moved")))
try:
    shutil.move(os.path.join(base, "a2.txt"), os.path.join(base, "moved"))
    shutil.copy(os.path.join(base, "src", "a.txt"), os.path.join(base, "a2.txt"))
    shutil.move(os.path.join(base, "a2.txt"), os.path.join(base, "moved"))
except shutil.Error as e:
    print("Error", e)
shutil.rmtree(os.path.join(base, "moved"))
print(os.path.exists(os.path.join(base, "moved")), sorted(os.listdir(base)))
shutil.rmtree(os.path.join(base, "nothing"), ignore_errors=True)
try:
    shutil.rmtree(os.path.join(base, "nothing"))
except FileNotFoundError as e:
    print("FileNotFoundError", e.errno)
errs: list[str] = []
def onexc(func, path: str, exc: OSError) -> None:
    errs.append(func.__name__ + " " + os.path.basename(path) + " " + type(exc).__name__)
shutil.rmtree(os.path.join(base, "nothing"), onexc=onexc)
print(errs)
print(shutil.which("no-such-program-xyz"))
print(stat.filemode(0o100644), stat.filemode(0o040755), stat.filemode(0o120777), stat.filemode(0o104755), stat.filemode(0o041777), stat.S_ISDIR(os.stat(base).st_mode), stat.S_ISREG(os.stat(base).st_mode), stat.S_IMODE(0o104755))
ts = shutil.get_terminal_size((99, 33))
print(type(ts).__name__, ts.columns > 0, ts.lines > 0)
shutil.rmtree(base)
print(os.path.exists(base))
os.makedirs(os.path.join(base, "x"))
f = open(os.path.join(base, "f.txt"), "w")
f.close()
errs2: list[str] = []
def onexc2(func, path: str, exc: OSError) -> None:
    errs2.append(func.__name__ + " " + os.path.basename(path) + " " + type(exc).__name__)
shutil.rmtree(os.path.join(base, "f.txt"), onexc=onexc2)
print(errs2)
try:
    shutil.rmtree(os.path.join(base, "f.txt"))
except NotADirectoryError as e:
    print("NotADirectoryError", e.errno)
shutil.rmtree(base)


d = tempfile.gettempdir()
print(os.path.isdir(d), os.path.isabs(d), tempfile.gettempprefix(), d == tempfile.gettempdir())
fd, path = tempfile.mkstemp(suffix=".dat", prefix="mpy_")
print(os.path.basename(path).startswith("mpy_"), path.endswith(".dat"), len(os.path.basename(path)), os.path.dirname(path) == d)
os.write(fd, b"hello")
os.close(fd)
print(os.path.getsize(path))
os.remove(path)
sub = tempfile.mkdtemp(prefix="dir_")
print(os.path.isdir(sub), os.path.basename(sub)[:4])
fd2, p2 = tempfile.mkstemp(dir=sub, text=True)
os.close(fd2)
print(os.listdir(sub) == [os.path.basename(p2)])
with tempfile.TemporaryDirectory(suffix="_td") as td:
    print(os.path.isdir(td), td.endswith("_td"))
    f = open(os.path.join(td, "x.txt"), "w")
    f.write("x")
    f.close()
print(os.path.exists(td))
t = tempfile.TemporaryDirectory()
print(repr(t).startswith("<TemporaryDirectory '"), os.path.isdir(t.name))
t.cleanup()
print(os.path.exists(t.name))
nf = tempfile.NamedTemporaryFile()
nf.write(b"abc")
nf.seek(0)
print(nf.read(), os.path.exists(nf.name), nf.name.startswith(d))
nf.close()
print(os.path.exists(nf.name))
with NamedTemporaryFile("w+", suffix=".txt", dir=sub) as tf:
    tf.write("text\n")
    tf.seek(0)
    print(tf.read(), tf.name.endswith(".txt"))
print(os.path.exists(tf.name))
keep = tempfile.NamedTemporaryFile(mode="wb", delete=False, dir=sub)
keep.write(b"keep")
keep.close()
print(os.path.getsize(keep.name))
tmp = tempfile.TemporaryFile()
tmp.write(b"zz")
tmp.seek(0)
print(tmp.read())
tmp.close()
shutil.rmtree(sub)
print(os.path.exists(sub), len(tempfile.mktemp()) > 0)


for s in ["", ".", "/", "//", "///", "//a", "a//b/./c/", "./a", "a/..", "../a/./b", "/a/b.tar.gz", "a/.bashrc", "a/foo.", "a/foo..", "a/b c.txt"]:
    p = PurePosixPath(s)
    print(repr(s), repr(p), str(p), p.parts, p.name, p.suffix, p.suffixes, p.stem, p.root, p.anchor, str(p.parent), [str(x) for x in p.parents])
print(PurePosixPath("a/b").with_suffix(".txt"), PurePosixPath("a/b.c").with_suffix(""), PurePosixPath("a/b").with_suffix("."), PurePosixPath("a/b").with_name("c.d"), PurePosixPath("a/b.c").with_stem("z"))
for sfx in ["txt", ""]:
    try:
        print(PurePosixPath("a/b").with_suffix(sfx) if sfx else PurePosixPath("a/b").with_name(sfx))
    except ValueError as e:
        print("ValueError", e)
try:
    PurePosixPath("/").with_name("x")
except ValueError as e:
    print("ValueError", e)
print(PurePosixPath("/a/b/c").relative_to("/a"), PurePosixPath("/a/b").relative_to("/a/c", walk_up=True), PurePosixPath("a/b").relative_to("a"))
for a, b in [("/a/b", "/c"), ("a", "/a")]:
    try:
        print(PurePosixPath(a).relative_to(b))
    except ValueError as e:
        print("ValueError", e)
print(PurePosixPath("/a/b").is_relative_to("/a"), PurePosixPath("/a/b").is_relative_to("/b"), PurePosixPath("a/b.py").match("*.py"), PurePosixPath("/a/b.py").match("a/*.py"), PurePosixPath("/a/b.py").match("/*.py"))
print(PurePosixPath("/a/b.py").full_match("/a/*.py"), PurePosixPath("a/b/c.py").full_match("**/*.py"), PurePosixPath("c.py").full_match("**/*.py"), PurePosixPath("a/b/c.py").full_match("*.py"))
print(PurePosixPath("a") / "b" / "/c", "x" / PurePosixPath("y"), PurePosixPath("a").joinpath("b", "c"), PurePosixPath("a") == PurePosixPath("a/"), PurePosixPath("a") < PurePosixPath("b"))
print(sorted([PurePosixPath("b"), PurePosixPath("a/c"), PurePosixPath("a")]), PurePosixPath("/a/b").parents[-1], PurePosixPath("a", "b", "/c", "d"))
print(repr(Path("x")), Path("x") / "y", Path("/a b/c").as_uri(), Path("/a/b").is_absolute(), Path("a").is_absolute(), hash(PurePosixPath("q")) == hash(PurePosixPath("q")))

pbase = Path("build") / "pathlib_test"
if pbase.exists():
    for top, subs, names in os.walk(str(pbase), topdown=False):
        for nm in names:
            os.remove(os.path.join(top, nm))
        for x in subs:
            os.rmdir(os.path.join(top, x))
    pbase.rmdir()
(pbase / "sub" / "deep").mkdir(parents=True)
(pbase / "sub").mkdir(exist_ok=True)
try:
    (pbase / "sub").mkdir()
except FileExistsError as e:
    print("FileExistsError", e.errno)
print((pbase / "a.txt").write_text("hello\nworld\n"), (pbase / "b.bin").write_bytes(b"\x00\x01"), (pbase / "sub" / "c.py").write_text("x = 1\n"))
(pbase / "sub" / "deep" / "d.txt").touch()
print((pbase / "a.txt").read_text(), (pbase / "b.bin").read_bytes(), (pbase / "a.txt").exists(), (pbase / "nope").exists(), (pbase / "sub").is_dir(), (pbase / "a.txt").is_file())
print(sorted(p.name for p in pbase.iterdir()), sorted(str(p.relative_to(pbase)) for p in pbase.glob("*.txt")), sorted(str(p.relative_to(pbase)) for p in pbase.rglob("*.txt")))
print(sorted(str(p.relative_to(pbase)) for p in pbase.glob("**/*.py")), (pbase / "a.txt").stat().st_size, (pbase / "a.txt").with_suffix(".md").name)
for wd, wdirs, wfiles in pbase.walk():
    print(str(wd.relative_to(pbase)), sorted(wdirs), sorted(wfiles))
f = (pbase / "a.txt").open()
print(f.readline().strip())
f.close()
g = (pbase / "b.bin").open("rb")
print(g.read())
g.close()
with (pbase / "e.txt").open("w") as h:
    h.write("eee")
print((pbase / "e.txt").read_text(), (pbase / "e.txt").rename(pbase / "f.txt").name, (pbase / "f.txt").exists(), (pbase / "e.txt").exists())
(pbase / "f.txt").unlink()
(pbase / "f.txt").unlink(missing_ok=True)
try:
    (pbase / "f.txt").unlink()
except FileNotFoundError as e:
    print("FileNotFoundError", e.errno)
print(Path.cwd() == Path(os.getcwd()), Path("x").absolute() == Path(os.getcwd()) / "x", (pbase / "sub" / ".." / "a.txt").resolve() == (pbase / "a.txt").resolve())
print((pbase / "a.txt").copy(pbase / "copy.txt").read_text() == "hello\nworld\n", (pbase / "copy.txt").move(pbase / "sub" / "moved.txt").exists())
for top, subs, names in os.walk(str(pbase), topdown=False):
    for nm in names:
        os.remove(os.path.join(top, nm))
    for x in subs:
        os.rmdir(os.path.join(top, x))
pbase.rmdir()
print(pbase.exists())
