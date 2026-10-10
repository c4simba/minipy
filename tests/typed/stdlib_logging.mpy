import io
import logging
import os
import sys
import tempfile

# basicConfig: a stream, a format, a level by name
logging.basicConfig(stream=sys.stdout, format="%(levelname)-8s|%(name)s|%(message)s", level="INFO")
logging.debug("not shown")
logging.info("info %s %d", "x", 5)
logging.warning("100%")
logging.error(42)
root = logging.getLogger()
print(root.level, logging.getLevelName(root.level), root.getEffectiveLevel(), repr(root))

# the hierarchy: levels, propagation, children
app = logging.getLogger("app")
db = logging.getLogger("app.db")
deep = logging.getLogger("app.db.pool")
print(db.parent is app, deep.parent is db, app.parent is root, app.getChild("db") is db)
early = logging.getLogger("x.y.z")
mid = logging.getLogger("x.y")
print(early.parent is mid, mid.parent is root)
app.setLevel(logging.WARNING)
db.info("db info: hidden")
db.warning("db warning")
print(db.getEffectiveLevel(), db.isEnabledFor(logging.INFO), db.isEnabledFor(logging.ERROR), repr(db))
db.setLevel(logging.DEBUG)
deep.debug("deep debug %r", [1, 2])

# a handler of its own, its formatter, and propagate
buf = io.StringIO()
h = logging.StreamHandler(buf)
h.setFormatter(logging.Formatter("[%(name)s:%(levelno)d] %(message)s"))
db.addHandler(h)
db.error("to both")
db.propagate = False
db.error("only to buf")
db.propagate = True
print(repr(buf.getvalue()))
print(db.hasHandlers(), logging.getLogger("other").hasHandlers(), len(db.handlers))
db.removeHandler(h)

# levels of handlers, filters
class OnlyEven(logging.Filter):
    def filter(self, record: logging.LogRecord) -> bool:
        return record.lineno % 2 == 0

lg = logging.getLogger("flt")
lg.propagate = False
lg.setLevel(logging.DEBUG)
out = io.StringIO()
h2 = logging.StreamHandler(out)
h2.setLevel(logging.INFO)
h2.setFormatter(logging.Formatter("%(levelname)s %(message)s"))
lg.addHandler(h2)
lg.debug("handler level drops this")
lg.info("kept")
h2.addFilter(logging.Filter("flt.sub"))
lg.info("dropped by the name filter")
lg.getChild("sub").info("child passes")
h2.removeFilter(h2.filters[0])
h2.addFilter(lambda r: "secret" not in r.getMessage())
lg.info("a secret message")
lg.info("a public message")
print(out.getvalue(), end="")

# exceptions
ex = logging.getLogger("ex")
ex.propagate = False
eb = io.StringIO()
eh = logging.StreamHandler(eb)
eh.setFormatter(logging.Formatter("%(levelname)s:%(message)s"))
ex.addHandler(eh)


def work(n: int) -> int:
    try:
        return 10 // n
    except ZeroDivisionError:
        ex.exception("division failed for %d", n)
    try:
        int("zz")
    except ValueError as e:
        ex.error("bad int", exc_info=True)
        ex.warning("again", exc_info=e)
    ex.error("no exception here", exc_info=True)
    return -1


work(0)
print(eb.getvalue(), end="")

# record attributes through a handler of its own
class Collect(logging.Handler):
    def __init__(self) -> None:
        logging.Handler.__init__(self)
        self.records: list[logging.LogRecord] = []

    def emit(self, record: logging.LogRecord) -> None:
        self.records.append(record)


col = Collect()
rl = logging.getLogger("rec")
rl.propagate = False
rl.addHandler(col)


def where() -> None:
    rl.warning("from where %s", "here")


where()
r = col.records[0]
print(r.name, r.levelname, r.levelno, r.getMessage(), r.funcName, r.module, r.filename, r.lineno, r.exc_info is None)
print(r.msecs >= 0, r.created > 0, r.process == os.getpid(), r.threadName)

# styles and specs
rec = logging.LogRecord("fmt", logging.INFO, "/p/m.py", 7, "hello", None, None, "fn")
print(logging.Formatter("%(levelname)8s|%(levelno)05d|%(lineno)-4d|%(name).2s|%(funcName)s").format(rec))
print(logging.Formatter("{levelname:>8}|{levelno:05}|{lineno:<4}|{name!r}|{module}", style="{").format(rec))
print(logging.Formatter("$levelname ${name}: $message $$", style="$").format(rec))
print(logging.Formatter("%(message)s", defaults={"extra": "d"}).format(rec))
print(logging.Formatter("%(message)s %(missing)s", defaults={"missing": "dflt"}).format(rec))
for bad, st in (("no fields", "%"), ("%(x", "%"), ("{x", "{"), ("{1x}", "{"), ("{x!q}", "{"), ("{x:q}", "{"), ("$ x", "$"), ("text", "$")):
    try:
        logging.Formatter(bad, style=st)
    except ValueError as e:
        print("ValueError:", e)
try:
    logging.Formatter("x", style="?")
except ValueError as e:
    print("ValueError:", e)
fm = logging.Formatter("%(asctime)s %(message)s")
print(fm.usesTime(), logging.Formatter("%(message)s").usesTime(), len(fm.formatTime(rec)) == 23)

# extra values and adapters
xl = logging.getLogger("extra")
xl.propagate = False
xb = io.StringIO()
xh = logging.StreamHandler(xb)
xh.setFormatter(logging.Formatter("%(user)s/%(ip)s: %(message)s"))
xl.addHandler(xh)
xl.warning("login", extra={"user": "bob", "ip": "10.0.0.1"})
ad = logging.LoggerAdapter(xl, {"user": "ann", "ip": "::1"})
ad.warning("via adapter %d", 3)
try:
    xl.warning("clash", extra={"message": "m"})
except KeyError as e:
    print("KeyError:", e)
print(xb.getvalue(), end="")

# custom levels, log(), disable
logging.addLevelName(15, "VERBOSE")
print(logging.getLevelName(15), logging.getLevelName(33), logging.getLevelName("VERBOSE"))
db.log(15, "verbose %s", "message")
db.log(logging.ERROR, "error via log")
logging.disable(logging.ERROR)
db.error("disabled")
db.critical("critical passes")
logging.disable(logging.NOTSET)
db.error("enabled again")
db.disabled = True
db.error("logger disabled")
db.disabled = False

# a file handler
path = os.path.join(tempfile.gettempdir(), "minipy_logging_test.log")
if os.path.exists(path):
    os.remove(path)
fh = logging.FileHandler(path, mode="w")
fh.setFormatter(logging.Formatter("%(levelname)s %(message)s"))
fl = logging.getLogger("file")
fl.propagate = False
fl.addHandler(fh)
fl.warning("line one")
fl.error("line %d", 2)
fh.close()
with open(path) as f:
    print(repr(f.read()))
os.remove(path)
print(repr(fh).startswith("<FileHandler "), repr(h), repr(logging.NullHandler()))

# NullHandler, the last resort
nl = logging.getLogger("quiet")
nl.addHandler(logging.NullHandler())
nl.error("nowhere")
setLevelTest = logging.getLogger("lv")
for lv in ("DEBUG", "WARN", "CRITICAL"):
    setLevelTest.setLevel(lv)
    print(setLevelTest.level, end=" ")
print()
try:
    setLevelTest.setLevel("NOPE")
except ValueError as e:
    print("ValueError:", e)
print(sorted(logging.getLevelNamesMapping().items())[:3])
