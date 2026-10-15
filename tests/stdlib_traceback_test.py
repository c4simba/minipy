# traceback and linecache (whole tracebacks: the interpreter's own text)
import traceback, sys, linecache, io

def inner(x):
    return 1 / x

def outer(x):
    return inner(x)

try:
    outer(0)
except ZeroDivisionError as e:
    lines = traceback.format_exception(e)
    print(len(lines), lines[0], lines[-1], end="")
    tbl = traceback.extract_tb(e.__traceback__)
    print([(f.name, f.lineno, f.line) for f in tbl], len(tbl))
    print(traceback.format_exception_only(e), [l.split(", ", 1)[-1] for l in traceback.format_tb(e.__traceback__)[-1].splitlines()[:2]])
    s = traceback.format_exc()
    print(s.splitlines()[-1], s.count("File"))
    print("".join(traceback.format_exception(e, limit=1)).count("File"), "".join(traceback.format_exception(e, limit=-1)).splitlines()[1].split(", in ")[1])
    buf = io.StringIO()
    traceback.print_exc(file=buf)
    print(buf.getvalue() == s)
    te = traceback.TracebackException.from_exception(e)
    print(str(te), te.exc_type_str, "".join(te.format()) == s, list(te.format_exception_only()))

def chained():
    try:
        {}["k"]
    except KeyError as e:
        raise ValueError("bad") from e

try:
    chained()
except ValueError as e:
    t = "".join(traceback.format_exception(e))
    print("direct cause" in t, t.count("Traceback"), t.splitlines()[-1])
    t2 = "".join(traceback.format_exception(e, chain=False))
    print(t2.count("Traceback"))
try:
    try:
        1 / 0
    except ZeroDivisionError:
        raise TypeError("during")
except TypeError as e:
    print("During handling" in "".join(traceback.format_exception(e)))
e = ValueError("noted")
e.add_note("a note")
print(traceback.format_exception_only(e), traceback.format_exception_only(KeyError("k")), traceback.format_exception_only(ValueError()))
print(traceback.format_exc() if False else "", traceback.format_exception_only(None))

def where():
    return traceback.extract_stack()
st = where()
print([f.name for f in st][-2:], st[-1].line)
print(traceback.format_stack(limit=1)[0].split(", in ")[1].strip())
print(linecache.getline(__file__, 3), repr(linecache.getline(__file__, 100000)), linecache.getline("nofile.py", 1) == "")
class MyErr(Exception): pass
print(traceback.format_exception_only(MyErr("x")))
try:
    raise MyErr("deep")
except MyErr as e:
    print(traceback.format_exception(type(e), e, e.__traceback__)[-1], end="")
fs = traceback.FrameSummary("f.py", 3, "fn", line="x = 1")
print(fs, fs.line, tuple(fs), fs == ("f.py", 3, "fn", "x = 1"))
print(traceback.format_list([("a.py", 1, "f", "pass"), ("b.py", 2, "g", None)]))
def rec(n):
    if n == 0:
        raise RuntimeError("bottom")
    rec(n - 1)
try:
    rec(10)
except RuntimeError as e:
    ft = traceback.format_tb(e.__traceback__)
    print(len(ft), ft[-2].strip())
