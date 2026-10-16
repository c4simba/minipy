"""plainpy's package manager: packages of PyPI for a project (USAGE below:
docstrings are left out of the modules plainpy carries)."""
USAGE = """plainpy's package manager: packages of PyPI for a project.

    plainpy add PACKAGE[SPEC]...   add to pyproject.toml, then lock and sync
    plainpy remove PACKAGE...      take out of pyproject.toml, then lock and sync
    plainpy lock                   choose the versions: pylock.toml
    plainpy sync                   the locked packages into the store and .plainpy/site, then the check
    plainpy check [PACKAGE...]     check them again for compiled programs: the reports
    plainpy list                   the packages and what the check found
    plainpy vendor                 .plainpy/site as copies (a project to take elsewhere: KolibriOS)

options: --index URL (PyPI's JSON API or a folder laid out like it; also
[tool.plainpy] index-url, PLAINPY_INDEX_URL), --pre (pre-releases too),
--target linux|kolibri|macos (what the check compiles for).

No virtual environment: packages are unpacked once per machine in the store
(~/.plainpy/store) and a project links to them. Only pure-Python wheels are
used and nothing of a package runs while it is installed. Every package goes
through the check for compiled programs; the interpreter runs every package,
compiled programs only the modules that passed."""
import os
import sys
from . import check, lockfile, store
from .index import Index, FetchError
from .project import ProjectError, find
from .reqs import Requirement, RequirementError, canonical, environment
from .resolve import ResolveError, Resolver


class Options:
    def __init__(self):
        self.index = ""
        self.pre = False
        self.target = ""
        self.names = []


def _options(args):
    o = Options()
    i = 0
    while i < len(args):
        a = args[i]
        if a in ("--index", "--target") and i + 1 < len(args):
            if a == "--index":
                o.index = args[i + 1]
            else:
                o.target = args[i + 1]
            i += 2
            continue
        if a == "--pre":
            o.pre = True
        elif a in ("-h", "--help"):
            print(USAGE)
            sys.exit(0)
        elif a.startswith("-"):
            raise ProjectError("unknown option " + a)
        else:
            o.names.append(a)
        i += 1
    return o


def _index_url(project, o):
    return o.index or project.settings().get("index-url", "") or os.environ.get("PLAINPY_INDEX_URL", "")


def lock(project, o, quiet=False):
    reqs = project.requirements()
    index = Index(_index_url(project, o))
    env = environment()
    pins = Resolver(index, env, o.pre).resolve(reqs) if reqs else {}
    old = {canonical(p.name): p for p in lockfile.read(project.lockfile)}
    packages = lockfile.from_pins(pins)
    for p in packages:
        q = old.get(canonical(p.name))
        if q is not None and q.version == p.version and q.sha256 == p.sha256:
            p.tool = q.tool                           # (its check results, until it is checked again)
        if not quiet:
            if q is None:
                print("+ %s %s" % (p.name, p.version))
            elif q.version != p.version:
                print("~ %s %s -> %s" % (p.name, q.version, p.version))
    if not quiet:
        new = set(canonical(p.name) for p in packages)
        for k in sorted(old):
            if k not in new:
                print("- %s %s" % (old[k].name, old[k].version))
    lockfile.write(project.lockfile, packages, index.url, env, project.dependencies())
    return packages


def _vendored(project):
    return os.path.exists(os.path.join(project.dot, "vendored"))


def sync(project, o, copy=False):
    """The locked packages into the store and the project's site; then the check."""
    if not os.path.exists(project.lockfile) or lockfile.requirements(project.lockfile) != project.dependencies():
        lock(project, o)
    packages = lockfile.read(project.lockfile)
    folders = {}
    entries = []
    seen = {}
    for p in packages:
        fresh = not os.path.exists(os.path.join(store.path_of(p), ".plainpy-store"))
        if fresh:
            print("  downloading %s %s" % (p.name, p.version))
        folders[p.name] = store.fetch(p)
        ext = store.extensions(folders[p.name])
        if ext:
            print("  %s: C extension modules left out: %s" % (p.name, ", ".join(ext[:3])))
        for name, entry in store.top_levels(folders[p.name]):
            if name in seen:
                raise store.StoreError("%s and %s both install '%s'" % (seen[name], p.name, name))
            seen[name] = p.name
            entries.append((entry, os.path.join(folders[p.name], entry)))   # (a module: name.py)
    os.makedirs(project.dot, exist_ok=True)
    copy = copy or _vendored(project)
    store.link_site(project.site, entries, copy)
    run_check(project, packages, folders, o)


def run_check(project, packages, folders, o, show=()):
    target = o.target or ""
    if packages:
        print("checking %d package%s for compiled programs..." % (len(packages), "s" if len(packages) > 1 else ""))
    results = check.run(project, packages, folders, target)
    check.write_reports(project, results, target)
    check.write_index(project, packages, folders, results)
    for p in packages:
        res = results[p.name]
        p.tool = {"check": res.status(), "modules-ok": res.ok, "errors": len([d for d in res.findings if d.kind == "error"]),
                  "unclear": len([d for d in res.findings if d.kind == "unclear"]), "broken": sorted(res.broken)}
        report = os.path.join(".plainpy", "check", "%s-%s.txt" % (p.name, p.version))
        if show:
            if canonical(p.name) in show:
                print(check.report_text(res, target).rstrip("\n"))
        else:
            print(check.summary(res, target, report))
    lockfile.write_tool(project.lockfile, packages)


def cmd_add(o):
    if not o.names:
        raise ProjectError("usage: plainpy add PACKAGE[SPEC]...")
    new = [Requirement(n) for n in o.names]
    project = find(os.getcwd(), create=True)
    deps = project.dependencies()
    keys = set(r.key for r in new)
    deps = [d for d in deps if Requirement(d).key not in keys] + [r.text for r in new]
    _change_dependencies(project, deps, o)
    sync(project, o)


def _change_dependencies(project, deps, o):
    """pyproject.toml with these dependencies, and the lock file for them - or,
    when they cannot be locked, pyproject.toml as it was."""
    with open(project.pyproject, "rb") as f:
        before = f.read()
    project.set_dependencies(deps)
    try:
        lock(project, o)
    except BaseException:
        with open(project.pyproject, "wb") as f:
            f.write(before)
        raise


def cmd_remove(o):
    if not o.names:
        raise ProjectError("usage: plainpy remove PACKAGE...")
    project = find(os.getcwd())
    keys = set(canonical(n) for n in o.names)
    deps = project.dependencies()
    left = [d for d in deps if Requirement(d).key not in keys]
    if len(left) == len(deps):
        raise ProjectError("not a dependency of the project: " + ", ".join(o.names))
    _change_dependencies(project, left, o)
    sync(project, o)


def cmd_check(o):
    project = find(os.getcwd())
    packages = lockfile.read(project.lockfile)
    if not packages:
        print("no packages (plainpy add PACKAGE)")
        return
    folders = {p.name: store.fetch(p) for p in packages}
    want = set(canonical(n) for n in o.names)
    unknown = [n for n in o.names if canonical(n) not in set(canonical(p.name) for p in packages)]
    if unknown:
        raise ProjectError("not a package of the project: " + ", ".join(unknown))
    run_check(project, packages, folders, o, want)


def cmd_list(o):
    project = find(os.getcwd())
    packages = lockfile.read(project.lockfile)
    if not packages:
        print("no packages (plainpy add PACKAGE)")
        return
    w = max(len(p.name) for p in packages)
    for p in packages:
        st = p.tool.get("check", "unchecked")
        n = len(p.tool.get("modules-ok", []))
        what = {"ok": "compiled + interpreter", "partial": "compiled (%d module%s) + interpreter" % (n, "" if n == 1 else "s"),
                "fail": "interpreter only", "broken": "does not run in plainpy (%s)" % ", ".join(p.tool.get("broken", []))}.get(st, "not checked (plainpy sync)")
        print("%-*s %-10s %s" % (w, p.name, p.version, what))


def main():
    args = sys.argv[1:]
    if not args:
        print(USAGE)
        return
    cmd = args[0]
    try:
        o = _options(args[1:])
        if cmd == "add":
            cmd_add(o)
        elif cmd == "remove":
            cmd_remove(o)
        elif cmd == "lock":
            lock(find(os.getcwd()), o)
        elif cmd == "sync":
            sync(find(os.getcwd()), o)
        elif cmd == "vendor":
            project = find(os.getcwd())
            os.makedirs(project.dot, exist_ok=True)
            with open(os.path.join(project.dot, "vendored"), "w") as f:
                f.write("copies, not links (plainpy vendor)\n")
            sync(project, o, True)
        elif cmd == "check":
            cmd_check(o)
        elif cmd == "list":
            cmd_list(o)
        else:
            raise ProjectError("unknown command " + cmd)
    except (ProjectError, ResolveError, FetchError, store.StoreError, RequirementError) as e:
        sys.stderr.write("plainpy: error: %s\n" % e)
        sys.exit(1)
