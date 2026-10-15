import io
import os
import traceback


class AppError(Exception):
    pass


def fail(n: int) -> int:
    if n == 0:
        raise ValueError("zero")
    return 10 // n


try:
    fail(0)
except ValueError as e:
    print(traceback.format_exception_only(e))
    print(traceback.format_exception_only(type(e), e))
    print(repr(traceback.format_exc().splitlines()[-1]))
    te = traceback.TracebackException.from_exception(e)
    print(te.exc_type_str, str(te), "".join(te.format_exception_only()), end="")

try:
    try:
        {}["k"]
    except KeyError as e:
        raise AppError("wrapped") from e
except AppError as e:
    lines = traceback.format_exception(e)
    for line in lines:
        if not line.startswith("  ") and not line.startswith("Traceback"):
            print(repr(line))
    buf = io.StringIO()
    traceback.print_exception(e, file=buf)
    print(buf.getvalue().splitlines()[-1])

try:
    try:
        int("x")
    except ValueError:
        raise ValueError("zero")
except ValueError as e:
    out = [s for s in traceback.format_exception(e) if not s.startswith("  ") and not s.startswith("Traceback")]
    print(out)
    print([s for s in traceback.format_exception(e, chain=False) if not s.startswith("  ") and not s.startswith("Traceback")])

try:
    try:
        int("y")
    except ValueError:
        raise AppError("no context") from None
except AppError as e:
    print([s for s in traceback.format_exception(e) if not s.startswith("  ") and not s.startswith("Traceback")])

print(repr(traceback.format_exc()))
buf2 = io.StringIO()
traceback.print_exc(file=buf2)
print(repr(buf2.getvalue()))


def where() -> None:
    st = traceback.extract_stack()
    f = st[len(st) - 1]
    print(os.path.basename(f.filename), f.lineno, f.name, f.line)


where()
