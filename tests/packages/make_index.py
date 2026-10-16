"""A package index for the tests of `plainpy add` (tests/run_pkg_tests.sh):
a folder laid out like PyPI's JSON API, with wheels made here.

    python make_index.py OUT      OUT/index/<name>/json, OUT/index/<name>/<version>/json, OUT/wheels/*.whl

The packages:
    okpkg       typed: passes the check (releases 1.0, 1.2, 2.0b1, 3.0 - only for Python 2)
    unionpkg    a parameter Union[int, str]: an error
    untypedpkg  a function without types nobody calls: unclear
    mixpkg      needs okpkg>=1.1; mixpkg passes, mixpkg.dyn (int | str) does not
    cpkg        only a wheel for one platform (a C extension)"""
import hashlib
import json
import os
import sys
import zipfile

OK_INIT = '''"""A small typed package."""
from .shapes import Rect


def area(w: int, h: int) -> int:
    return w * h


def greet(name: str) -> str:
    return "hello, " + name
'''

OK_SHAPES = '''class Rect:
    def __init__(self, w: int, h: int) -> None:
        self.w = w
        self.h = h

    def area(self) -> int:
        return self.w * self.h

    def scaled(self, k: int) -> "Rect":
        return Rect(self.w * k, self.h * k)
'''

UNION_INIT = '''from typing import Union


def size(x: Union[int, str]) -> int:
    if isinstance(x, str):
        return len(x)
    return x


def double(n: int) -> int:
    return n * 2
'''

UNTYPED_INIT = '''def add(a, b):
    return a + b


def twice(s: str) -> str:
    return s + s
'''

MIX_INIT = '''from okpkg import area
from okpkg.shapes import Rect


def total(rs: list[Rect]) -> int:
    t = 0
    for r in rs:
        t += r.area()
    return t


def square(n: int) -> int:
    return area(n, n)
'''

MIX_DYN = '''def pick(flag: bool) -> int | str:
    if flag:
        return 1
    return "one"
'''

PACKAGES = [
    # name, version, files, requires_dist, requires_python, platform wheel
    ("okpkg", "1.0", {"okpkg/__init__.py": OK_INIT, "okpkg/shapes.py": OK_SHAPES}, [], ">=3.8", False),
    ("okpkg", "1.2", {"okpkg/__init__.py": OK_INIT, "okpkg/shapes.py": OK_SHAPES}, [], ">=3.8", False),
    ("okpkg", "2.0b1", {"okpkg/__init__.py": OK_INIT, "okpkg/shapes.py": OK_SHAPES}, [], ">=3.8", False),
    ("okpkg", "3.0", {"okpkg/__init__.py": OK_INIT, "okpkg/shapes.py": OK_SHAPES}, [], ">=2.7,<3", False),
    ("unionpkg", "1.0", {"unionpkg/__init__.py": UNION_INIT}, [], "", False),
    ("untypedpkg", "1.0", {"untypedpkg/__init__.py": UNTYPED_INIT}, [], "", False),
    ("mixpkg", "2.0", {"mixpkg/__init__.py": MIX_INIT, "mixpkg/dyn.py": MIX_DYN},
     ["okpkg>=1.1", 'fancy>=1; extra == "fancy"', 'winonly; sys_platform == "win32"'], ">=3.10", False),
    ("cpkg", "1.0", {"cpkg/__init__.py": "x = 1\n"}, [], "", True),
]


def wheel(out, name, version, files, requires, platform):
    tag = "cp314-cp314-macosx_11_0_arm64" if platform else "py3-none-any"
    fn = "%s-%s-%s.whl" % (name, version, tag)
    path = os.path.join(out, "wheels", fn)
    z = zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED)
    for f in sorted(files):
        z.writestr(f, files[f])
    meta = "Metadata-Version: 2.1\nName: %s\nVersion: %s\n" % (name, version) + "".join("Requires-Dist: %s\n" % r for r in requires)
    di = "%s-%s.dist-info/" % (name, version)
    z.writestr(di + "METADATA", meta)
    z.writestr(di + "WHEEL", "Wheel-Version: 1.0\nRoot-Is-Purelib: true\nTag: %s\n" % tag)
    z.writestr(di + "RECORD", "")
    z.close()
    with open(path, "rb") as f:
        data = f.read()
    return {"filename": fn, "url": "file://" + os.path.abspath(path), "digests": {"sha256": hashlib.sha256(data).hexdigest()},
            "size": len(data), "packagetype": "bdist_wheel", "yanked": False}


def main():
    out = sys.argv[1]
    os.makedirs(os.path.join(out, "wheels"), exist_ok=True)
    projects = {}
    for name, version, files, requires, pyreq, platform in PACKAGES:
        entry = wheel(out, name, version, files, requires, platform)
        entry["requires_python"] = pyreq or None
        p = projects.setdefault(name, {"info": {}, "releases": {}})
        p["releases"][version] = [entry]
        info = {"name": name, "version": version, "requires_python": pyreq or None, "requires_dist": requires or None}
        if "b" not in version:
            p["info"] = info
        d = os.path.join(out, "index", name, version)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "json"), "w") as f:
            f.write(json.dumps({"info": info}))
    for name in projects:
        with open(os.path.join(out, "index", name, "json"), "w") as f:
            f.write(json.dumps(projects[name], indent=1))


main()
