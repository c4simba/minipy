# warnings: filters, registries, catch_warnings, deprecated; exit/quit; __builtins__
import warnings, sys, io
for n in ["UserWarning", "DeprecationWarning", "PendingDeprecationWarning", "ImportWarning", "ResourceWarning", "RuntimeWarning", "SyntaxWarning", "FutureWarning", "UnicodeWarning", "BytesWarning", "EncodingWarning"]:
    print(n, n in dir(__builtins__) if not isinstance(__builtins__, dict) else n in __builtins__)
log = []
def show(message, category, filename, lineno, file=None, line=None):
    log.append((str(message), category.__name__, filename.split("/")[-1], lineno, line))
warnings.showwarning = show
def f():
    warnings.warn("deep", stacklevel=50)
f()
warnings.warn("plain")
warnings.warn("plain")
for i in range(2):
    warnings.warn("loop")
warnings.warn(DeprecationWarning("dep in main"))
def g():
    warnings.warn("in g", DeprecationWarning, stacklevel=2)
g()
warnings.warn("rt", RuntimeWarning)
for x in log: print(x)
print(warnings.formatwarning("msg", UserWarning, "file.py", 3, "code line"), end="")
with warnings.catch_warnings(record=True) as w:
    warnings.simplefilter("always")
    warnings.warn("rec1"); warnings.warn("rec1"); warnings.warn("fut", FutureWarning)
    print(len(w), [str(x.message) for x in w], w[-1].category.__name__, w[0].lineno > 0)
with warnings.catch_warnings():
    warnings.simplefilter("error")
    try:
        warnings.warn("boom", UserWarning)
    except UserWarning as e:
        print("raised", e)
with warnings.catch_warnings():
    warnings.filterwarnings("ignore", message="ign.*")
    log.clear()
    warnings.warn("ignored one"); warnings.warn("shown one")
    print(log[-1][0] if log else None, len(log))
with warnings.catch_warnings(action="once"):
    log.clear()
    def h(): warnings.warn("once msg")
    h(); h(); warnings.warn("once msg")
    print(len(log))
print(warnings.filters[:5] == [('default', None, DeprecationWarning, '__main__', 0), ('ignore', None, DeprecationWarning, None, 0), ('ignore', None, PendingDeprecationWarning, None, 0), ('ignore', None, ImportWarning, None, 0), ('ignore', None, ResourceWarning, None, 0)])
@warnings.deprecated("use new")
def old(): return 1
with warnings.catch_warnings(record=True) as w:
    warnings.simplefilter("always")
    print(old(), str(w[0].message), w[0].category.__name__, old.__deprecated__)
try:
    warnings.warn("x", int)
except TypeError as e:
    print("TypeError", e)
try:
    warnings.simplefilter("bogus")
except ValueError as e:
    print("ValueError", e)
warnings.showwarning = warnings._showwarning_orig
buf = io.StringIO()
warnings.showwarning("to file", UserWarning, "f.py", 9, file=buf, line="the line")
print(repr(buf.getvalue()))
import builtins
print(__builtins__ is builtins, "exit" in dir(builtins), callable(exit))
try:
    exit(3)
except SystemExit as e:
    print("SystemExit", e.code)
