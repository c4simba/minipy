"""pylock.toml (PEP 751): every package of the project pinned to a version and
to the one wheel plainpy uses, with its sha256 - the same files on every
machine, checked when downloaded. What the check found is kept per package in
[packages.tool.plainpy] (so a diff of the lock file shows it changing)."""
import json
import tomllib
from .project import write_text

HEADER = "# Written by `plainpy lock` (PEP 751 lock file); do not edit.\n"


def _s(v):
    return json.dumps(v)                              # a TOML basic string


def _list(vs):
    return "[" + ", ".join(_s(v) for v in vs) + "]"


class Locked:
    """A package of the lock file."""

    def __init__(self, name, version, url, filename, sha256, size, dependencies, tool):
        self.name = name
        self.version = version
        self.url = url
        self.filename = filename
        self.sha256 = sha256
        self.size = size
        self.dependencies = dependencies             # canonical names
        self.tool = tool                              # [packages.tool.plainpy]: top-level, check, modules-ok ...


def from_pins(pins):
    out = []
    for key in sorted(pins):
        p = pins[key]
        w = p.wheel
        deps = sorted(set(r.key for r in p.requires))
        out.append(Locked(p.name, str(p.version), w["url"], w["filename"], w["digests"]["sha256"], int(w.get("size") or 0), deps, {}))
    return out


def write(path, packages, index_url, environment, requirements):
    """(requirements: the project's dependencies the lock was made for)"""
    t = [HEADER, 'lock-version = "1.0"\n', 'requires-python = ">=3.14"\n', 'created-by = "plainpy"\n']
    t.append("\n[tool.plainpy]\nindex-url = %s\nsys-platform = %s\nrequirements = %s\n" % (
        _s(index_url), _s(environment.get("sys_platform", "")), _list(requirements)))
    for p in packages:
        t.append("\n[[packages]]\nname = %s\nversion = %s\n" % (_s(p.name), _s(p.version)))
        if p.dependencies:
            t.append("dependencies = [%s]\n" % ", ".join("{ name = %s }" % _s(d) for d in p.dependencies))
        t.append("\n[[packages.wheels]]\nname = %s\nurl = %s\n" % (_s(p.filename), _s(p.url)))
        if p.size:
            t.append("size = %d\n" % p.size)
        t.append("hashes = { sha256 = %s }\n" % _s(p.sha256))
        if p.tool:
            t.append("\n[packages.tool.plainpy]\n")
            for k in sorted(p.tool):
                v = p.tool[k]
                if isinstance(v, list):
                    t.append("%s = %s\n" % (k, _list(v)))
                elif isinstance(v, bool):
                    t.append("%s = %s\n" % (k, "true" if v else "false"))
                elif isinstance(v, int):
                    t.append("%s = %d\n" % (k, v))
                else:
                    t.append("%s = %s\n" % (k, _s(str(v))))
    write_text(path, "".join(t))


def read(path):
    """The packages of a lock file ([] when there is none)."""
    try:
        with open(path, "rb") as f:
            data = tomllib.loads(f.read().decode("utf-8"))
    except OSError:
        return []
    out = []
    for p in data.get("packages", []):
        w = (p.get("wheels") or [{}])[0]
        deps = [d.get("name", "") for d in p.get("dependencies", [])]
        out.append(Locked(p["name"], p["version"], w.get("url", ""), w.get("name", ""), w.get("hashes", {}).get("sha256", ""),
                          int(w.get("size", 0)), deps, dict(p.get("tool", {}).get("plainpy", {}))))
    return out


def _top(path):
    try:
        with open(path, "rb") as f:
            return tomllib.loads(f.read().decode("utf-8")).get("tool", {}).get("plainpy", {})
    except OSError:
        return {}


def requirements(path):
    """The project's dependencies the lock file was made for."""
    return list(_top(path).get("requirements", []))


def write_tool(path, packages):
    """The lock file again with the packages' [packages.tool.plainpy] as they are now."""
    top = _top(path)
    write(path, packages, top.get("index-url", ""), {"sys_platform": top.get("sys-platform", "")}, list(top.get("requirements", [])))
