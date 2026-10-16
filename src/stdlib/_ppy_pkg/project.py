"""The project: pyproject.toml (PEP 621: [project] dependencies) and the
folder plainpy keeps next to it, .plainpy:

    .plainpy/site/      the packages' top-level modules: links into the store (or copies)
    .plainpy/index      a line per module: "module package version status" (the compiler reads it)
    .plainpy/check/     what the check found, a report per package

pyproject.toml is edited in place: only the dependencies array is rewritten."""
import json
import os
import tomllib
from .reqs import Requirement, canonical


class ProjectError(Exception):
    pass


class Project:
    def __init__(self, root):
        self.root = root
        self.pyproject = os.path.join(root, "pyproject.toml")
        self.lockfile = os.path.join(root, "pylock.toml")
        self.dot = os.path.join(root, ".plainpy")
        self.site = os.path.join(self.dot, "site")
        self.index = os.path.join(self.dot, "index")
        self.reports = os.path.join(self.dot, "check")

    def data(self):
        try:
            with open(self.pyproject, "rb") as f:
                return tomllib.loads(f.read().decode("utf-8"))
        except tomllib.TOMLDecodeError as e:
            raise ProjectError("%s: %s" % (self.pyproject, e))

    def name(self):
        return self.data().get("project", {}).get("name", os.path.basename(self.root))

    def dependencies(self):
        """The requirement strings of [project] dependencies."""
        deps = self.data().get("project", {}).get("dependencies", [])
        if not isinstance(deps, list):
            raise ProjectError("%s: [project] dependencies must be an array of strings" % self.pyproject)
        return [str(d) for d in deps]

    def requirements(self):
        return [Requirement(d) for d in self.dependencies()]

    def settings(self):
        """[tool.plainpy]: index-url, ..."""
        return self.data().get("tool", {}).get("plainpy", {})

    def set_dependencies(self, deps):
        with open(self.pyproject, "rb") as f:
            text = f.read().decode("utf-8")
        array = "dependencies = [\n" + "".join("    %s,\n" % json.dumps(d) for d in deps) + "]" if deps else "dependencies = []"
        lines = text.split("\n")
        start = -1                                    # the [project] table: from its header to the next one
        for i in range(len(lines)):
            if lines[i].strip() == "[project]":
                start = i
                break
        if start < 0:
            text = text.rstrip("\n") + ("\n\n" if text.strip() else "") + "[project]\n" + array + "\n"
        else:
            end = len(lines)
            for i in range(start + 1, len(lines)):
                if lines[i].strip().startswith("["):
                    end = i
                    break
            at = -1
            for i in range(start + 1, end):
                if lines[i].strip().startswith("dependencies") and "=" in lines[i]:
                    at = i
                    break
            if at < 0:
                lines.insert(start + 1, array)
            else:
                stop = at                             # the array may span several lines
                depth = 0
                for i in range(at, end):
                    depth += _brackets(lines[i])
                    stop = i
                    if depth <= 0:
                        break
                lines[at:stop + 1] = [array]
            text = "\n".join(lines)
        write_text(self.pyproject, text)
        self.data()                                   # (still valid TOML)


def _brackets(line):
    """[ minus ] outside strings and comments"""
    n = 0
    quote = ""
    for ch in line:
        if quote:
            if ch == quote:
                quote = ""
        elif ch in "\"'":
            quote = ch
        elif ch == "#":
            break
        elif ch == "[":
            n += 1
        elif ch == "]":
            n -= 1
    return n


def write_text(path, text):
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(text.encode("utf-8"))
    os.replace(tmp, path)


def find(start, create=False):
    """The project whose pyproject.toml is in start or a folder above it; with
    create, a new one in start when there is none."""
    d = os.path.abspath(start)
    while True:
        if os.path.exists(os.path.join(d, "pyproject.toml")):
            return Project(d)
        up = os.path.dirname(d)
        if up == d:
            break
        d = up
    if not create:
        raise ProjectError("no pyproject.toml here or above (`plainpy add PACKAGE` makes one)")
    root = os.path.abspath(start)
    name = canonical(os.path.basename(root)) or "project"
    write_text(os.path.join(root, "pyproject.toml"),
               '[project]\nname = %s\nversion = "0.1.0"\nrequires-python = ">=3.14"\ndependencies = []\n' % json.dumps(name))
    return Project(root)
