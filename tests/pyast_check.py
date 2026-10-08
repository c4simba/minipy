"""Compare minipy's Python parser (minipy --pyast) with CPython's ast.

    python3 tests/pyast_check.py [--minipy ./minipy] [--show N] FILE_OR_DIR ...

Every .py file is parsed by both; the trees are printed in the canonical form
of src/py_dump.c and compared. Files CPython itself cannot parse are skipped.
Prints the files that differ (the first difference of each) and a summary.
"""
import ast
import os
import struct
import subprocess
import sys
import warnings

SKIP_FIELDS = {"type_comment", "type_ignores"}


def esc(s):
    out = []
    for ch in s:
        c = ord(ch)
        if 32 <= c < 127 and ch not in "\\'":
            out.append(ch)
        else:
            out.append("\\u{%x}" % c)
    return "'" + "".join(out) + "'"


def bits(f):
    return struct.pack(">d", f).hex()


def const(v):
    if v is None:
        return "None"
    if v is True:
        return "True"
    if v is False:
        return "False"
    if v is Ellipsis:
        return "Ellipsis"
    if isinstance(v, int):
        return "int:%x" % v
    if isinstance(v, float):
        return "float:" + bits(v)
    if isinstance(v, complex):
        return "complex:" + bits(v.imag)
    if isinstance(v, bytes):
        return "b'" + v.hex() + "'"
    if isinstance(v, str):
        return esc(v)
    raise TypeError(type(v))


def dump(n):
    if n is None:
        return "None"
    if isinstance(n, list):
        return "[" + ", ".join(dump(x) for x in n) + "]"
    if isinstance(n, (ast.operator, ast.unaryop, ast.boolop, ast.cmpop, ast.expr_context)):
        return type(n).__name__
    name = type(n).__name__
    parts = []
    for f in n._fields:
        if f in SKIP_FIELDS:
            continue
        v = getattr(n, f, None)
        if name in ("Constant", "MatchSingleton") and f == "value":
            s = const(v)
        elif name == "Constant" and f == "kind":
            s = "'u'" if v == "u" else "None"
        elif name == "Interpolation" and f == "str":
            s = esc(v)
        elif isinstance(v, str):
            s = esc(v)
        elif isinstance(v, int) and not isinstance(v, bool):
            s = str(v)
        elif isinstance(v, list) and v and isinstance(v[0], str):
            s = "[" + ", ".join(esc(x) for x in v) + "]"
        else:
            s = dump(v)
        parts.append("%s=%s" % (f, s))
    return "%s(%s)" % (name, ", ".join(parts))


def files(args):
    for a in args:
        if os.path.isdir(a):
            for root, dirs, names in os.walk(a):
                dirs.sort()
                for nm in sorted(names):
                    if nm.endswith(".py"):
                        yield os.path.join(root, nm)
        else:
            yield a


def first_diff(a, b):
    i = 0
    while i < min(len(a), len(b)) and a[i] == b[i]:
        i += 1
    lo = max(0, i - 80)
    return "  minipy: ...%s\n  python: ...%s" % (a[lo:i + 80], b[lo:i + 80])


def main():
    argv = sys.argv[1:]
    minipy = "./minipy"
    show = 20
    while argv and argv[0].startswith("--"):
        if argv[0] == "--minipy":
            minipy = argv[1]
            argv = argv[2:]
        elif argv[0] == "--show":
            show = int(argv[1])
            argv = argv[2:]
        else:
            break
    warnings.simplefilter("ignore")
    ok = bad = skipped = errors = 0
    shown = 0
    paths = list(files(argv))
    expect = {}
    for p in paths:
        try:
            with open(p, "rb") as f:
                src = f.read()
            expect[p] = dump(ast.parse(src, p)) + "\n"
        except Exception:
            skipped += 1
    todo = [p for p in paths if p in expect]
    for i in range(0, len(todo), 200):
        chunk = todo[i:i + 200]
        for p in chunk:
            r = subprocess.run([minipy, "--pyast", p], capture_output=True, text=True, errors="replace")
            got = r.stdout
            if r.returncode != 0 or got.startswith(p + ":"):
                errors += 1
                if shown < show:
                    print("ERROR", got.strip() or r.stderr.strip()[:300])
                    shown += 1
                continue
            if got == expect[p]:
                ok += 1
            else:
                bad += 1
                if shown < show:
                    print("DIFF", p)
                    print(first_diff(got, expect[p]))
                    shown += 1
    print("pyast: %d same, %d different, %d parse errors, %d skipped (CPython cannot parse)" % (ok, bad, errors, skipped))
    return 0 if bad == 0 and errors == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
