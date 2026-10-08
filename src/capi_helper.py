# minipy --compile --target cpython: the build-time helper, run with the
# target Python (minipy carries this file as a string; capi_driver.c).
#
#   helper.py config                      the Python's paths and build flags
#   helper.py compile OUT SRC...          for each SRC (i = 0, 1, ...): OUT/m<i>.code (its
#                                         code objects, marshalled) and OUT/m<i>.meta
#   helper.py stubs OUT SRC STUB...       OUT/s<i>.code: STUB (Python text) compiled as SRC
#
# .meta lines (tab-separated; strings as hex of UTF-8):
#   C qualname firstlineno name flags cellvars(,) freevars(,) parent   every code object (parent: index)
#   A line name text                                              future annotations of module / class bodies
#   D line col doc                                                docstrings, cleaned as the
#                                                                 compiler does (line 0: module)
import ast
import dis
import marshal
import sys
import sysconfig


def hx(s):
    return s.encode("utf-8", "surrogatepass").hex()


def walk(code, out, parents=None, parent=-1):
    out.append(code)
    if parents is not None:
        parents.append(parent)
    me = len(out) - 1
    for c in code.co_consts:
        if hasattr(c, "co_code"):
            walk(c, out, parents, me)


def cleaned_doc(node):
    """The docstring of a module / class / def as CPython stores it (3.13+ strips indentation)."""
    doc = node.body[0]
    fn = ast.FunctionDef(name="f", args=ast.arguments(posonlyargs=[], args=[], kwonlyargs=[], kw_defaults=[], defaults=[]),
                         body=[doc], decorator_list=[], returns=None, type_params=[])
    m = ast.Module(body=[fn], type_ignores=[])
    ast.fix_missing_locations(m)
    ns = {}
    exec(compile(m, "<doc>", "exec"), ns)
    return ns["f"].__doc__


def has_doc(node):
    b = getattr(node, "body", None)
    return (isinstance(b, list) and b and isinstance(b[0], ast.Expr) and isinstance(b[0].value, ast.Constant)
            and isinstance(b[0].value.value, str))


def main():
    mode = sys.argv[1]
    if mode == "config":
        # what python3-config --cflags / --ldflags --embed would say
        g = sysconfig.get_config_var
        v = sys.version_info
        inc = {sysconfig.get_path("include"), sysconfig.get_path("platinclude")}
        libs = ["-lpython%d.%d%s" % (v[0], v[1], getattr(sys, "abiflags", ""))]
        libs += (g("LIBS") or "").split() + (g("SYSLIBS") or "").split()
        if not g("Py_ENABLE_SHARED"):
            libs.insert(0, "-L" + (g("LIBPL") or ""))
        elif g("LIBDIR"):
            libs.insert(0, "-L" + g("LIBDIR"))
            if sys.platform != "darwin":
                libs.append("-Wl,-rpath," + g("LIBDIR"))
        if not g("Py_ENABLE_SHARED") and sys.platform != "darwin":
            libs += (g("LINKFORSHARED") or "").split()   # extension modules link against the executable
        print(sys.executable)
        print("%d.%d" % (v[0], v[1]))
        print(" ".join("-I" + p for p in sorted(x for x in inc if x)))
        print(" ".join(libs))
        print(sysconfig.get_path("stdlib") or "")
        print(sysconfig.get_path("purelib") or "")
        print(sysconfig.get_path("platlib") or "")
        print("\x1f".join(p for p in sys.path[1:] if p))
        return 0
    out = sys.argv[2]
    if mode == "compile":
        for i, path in enumerate(sys.argv[3:]):
            with open(path, "rb") as f:
                src = f.read()
            code = compile(src, path, "exec", dont_inherit=True)
            with open("%s/m%d.code" % (out, i), "wb") as f:
                f.write(marshal.dumps(code))
            lines = []
            codes = []
            parents = []
            walk(code, codes, parents)
            for c, p in zip(codes, parents):
                lines.append("C\t%s\t%d\t%s\t%d\t%s\t%s\t%d" % (hx(c.co_qualname), c.co_firstlineno, hx(c.co_name), c.co_flags,
                                                       ",".join(hx(x) for x in c.co_cellvars), ",".join(hx(x) for x in c.co_freevars), p))
            # `from __future__ import annotations`: the strings module / class bodies store
            for c in codes:
                if "__annotations__" not in c.co_names:
                    continue
                ins = [x for x in dis.get_instructions(c) if x.opname != "EXTENDED_ARG"]
                for k in range(len(ins) - 3):
                    a, b, n, st = ins[k:k + 4]
                    if (a.opname == "LOAD_CONST" and isinstance(a.argval, str) and b.opname == "LOAD_NAME" and b.argval == "__annotations__"
                            and n.opname == "LOAD_CONST" and isinstance(n.argval, str) and st.opname == "STORE_SUBSCR"):
                        line = a.positions.lineno if a.positions else 0
                        lines.append("A\t%d\t%s\t%s" % (line or 0, hx(n.argval), hx(a.argval)))
            tree = ast.parse(src, path)
            for node in ast.walk(tree):
                if isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)) and has_doc(node):
                    d = cleaned_doc(node)
                    line = 0 if isinstance(node, ast.Module) else node.lineno
                    col = 0 if isinstance(node, ast.Module) else node.col_offset
                    lines.append("D\t%d\t%d\t%s" % (line, col, hx(d)))
            with open("%s/m%d.meta" % (out, i), "w") as f:
                f.write("\n".join(lines) + "\n")
        return 0
    if mode == "stubs":
        args = sys.argv[3:]
        n = len(args) // 2
        for i in range(n):
            path, stub = args[i], args[n + i]
            with open(stub, "r", encoding="utf-8") as f:
                text = f.read()
            code = compile(text, path, "exec", dont_inherit=True)
            with open("%s/s%d.code" % (out, i), "wb") as f:
                f.write(marshal.dumps(code))
        return 0
    return 2


sys.exit(main())
