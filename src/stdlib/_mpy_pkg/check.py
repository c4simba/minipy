"""The check for compiled programs.

minipy compiles a program only with every type known: one type per variable,
parameter, field and element, `T | None` the only union. Before a package's
module may go into a compiled program, the compiler goes through all of it
(`minipy --compile --check`, src/aot/pkgcheck.c) and reports:

    error     what it cannot compile (a Union, an unknown type, a dynamic feature ...)
    unclear   a function it cannot check: nobody calls it and its parameters
              have no types (they would come from a call)

Before that every module is compiled for the interpreter (compile()): source
minipy cannot run at all (an int literal beyond 64 bits ...) is "syntax"; such
a package does not run in minipy, not even in the interpreter.

A module passes when its file has neither, and every module it imports (its
package's __init__ too) passes. The compiler then takes only modules that
passed (.minipy/index); the interpreter runs them all."""
import os
import subprocess
import sys
from . import store
from .project import write_text

MAX_SHOWN = 6


class Finding:
    def __init__(self, kind, path, line, message):
        self.kind = kind                              # "error" | "unclear" | "syntax"
        self.path = path
        self.line = line
        self.message = message

    def text(self):
        return "%s:%d: %s: %s" % (self.path, self.line, self.kind, self.message)


class Result:
    """What the check found for one package."""

    def __init__(self, pkg):
        self.pkg = pkg
        self.ok = []                                  # modules that pass
        self.bad = {}                                 # module -> why it does not
        self.broken = {}                              # module -> what the interpreter cannot run (Finding)
        self.findings = []

    def status(self):
        if self.broken:
            return "broken"
        if not self.bad:
            return "ok"
        return "partial" if self.ok else "fail"


def minipy_exe():
    exe = sys.argv[0] if sys.argv else "minipy"
    if "/" in exe or "\\" in exe:
        return os.path.abspath(exe)
    return exe


def _parse_diag(line):
    for kind in ("error", "unclear"):
        mark = ": %s: " % kind
        k = line.find(mark)
        if k > 0:
            loc = line[:k]
            c = loc.rfind(":")
            if c > 0 and loc[c + 1:].isdigit():
                return Finding(kind, loc[:c], int(loc[c + 1:]), line[k + len(mark):])
    return None


def _norm(path, site):
    if path.startswith("./"):
        path = path[2:]
    if os.path.isabs(path) and path.startswith(site + os.sep):
        path = path[len(site) + 1:]
    return path


def syntax(folder, modules):
    """{module: Finding} for the modules whose source the interpreter cannot compile."""
    out = {}
    for name, rel in modules:
        try:
            with open(os.path.join(folder, rel), "rb") as f:
                src = f.read().decode("utf-8")
            compile(src, rel, "exec")
        except SyntaxError as e:
            out[name] = Finding("syntax", rel, e.lineno or 0, e.msg or str(e))
        except UnicodeDecodeError:
            out[name] = Finding("syntax", rel, 0, "not UTF-8 text")
    return out


def run(project, packages, folders, target=""):
    """Check every module of the packages (folders: package name -> its folder
    in the store); {package name: Result}."""
    owner = {}                                        # module -> package
    results = {pkg.name: Result(pkg) for pkg in packages}
    for pkg in packages:
        mods = store.modules(folders[pkg.name])
        for name, rel in mods:
            owner[name] = pkg
        res = results[pkg.name]
        res.broken = syntax(folders[pkg.name], mods)
        res.findings.extend(res.broken[m] for m in sorted(res.broken))
    if not owner:
        return results
    cmd = [minipy_exe(), "--compile", "--check"] + (["--target", target] if target else []) + sorted(owner)
    env = dict(os.environ)
    env["MINIPYPATH"] = project.site
    try:
        r = subprocess.run(cmd, cwd=project.site, env=env, capture_output=True)
        out = r.stdout.decode("utf-8", "replace")
        err = r.stderr.decode("utf-8", "replace")
    except OSError as e:
        out = ""
        err = "cannot run %s: %s" % (cmd[0], e)
    path_of = {}                                      # module -> its file (as the check names it)
    module_at = {}
    imports = {}
    failed = set()
    for line in out.split("\n"):
        f = line.split("\t")
        if f[0] == "module" and len(f) == 3:
            p = _norm(f[2], project.site)
            path_of[f[1]] = p
            module_at[p] = f[1]
        elif f[0] == "imports" and len(f) == 3:
            imports[f[1]] = [m for m in f[2].split(",") if m]
        elif f[0] == "failed" and len(f) == 2:
            failed.add(f[1])
    own = {}                                          # module -> its findings
    stray = []
    for line in err.split("\n"):
        d = _parse_diag(line)
        if d is None:
            if line.strip():
                stray.append(line.strip())
            continue
        d.path = _norm(d.path, project.site)
        m = module_at.get(d.path)
        if m is None:
            stray.append(d.text())
            continue
        own.setdefault(m, []).append(d)
        if m in owner:
            results[owner[m].name].findings.append(d)
    if not path_of:                                   # the check itself did not run
        why = "the check could not run: " + (" ".join(stray[-3:]) or "no output")
        for m, pkg in owner.items():
            results[pkg.name].bad[m] = why
        return results
    memo = {}

    def why_not(m, seen):
        """'' when module m passes, else why it does not"""
        if m in memo:
            return memo[m]
        if m in seen:
            return ""                                 # (an import cycle: decided by the others)
        seen.add(m)
        reason = ""
        mine = own.get(m, [])
        errors = [d for d in mine if d.kind == "error"]
        unclear = [d for d in mine if d.kind == "unclear"]
        if m not in path_of and m in owner:
            reason = "not reached by the check"
        elif errors:
            reason = "%d error%s" % (len(errors), "s" if len(errors) > 1 else "")
        elif m in failed:
            reason = "rejected (see the errors)"
        elif unclear:
            reason = "%d function%s with unclear types" % (len(unclear), "s" if len(unclear) > 1 else "")
        else:
            deps = list(imports.get(m, []))
            if "." in m:
                deps.append(m.rsplit(".", 1)[0])      # (its package's __init__ runs first)
            for d in deps:
                if d == m or d not in path_of:
                    continue
                if why_not(d, seen):
                    reason = "imports %s, which does not pass" % d
                    break
        seen.discard(m)
        memo[m] = reason
        return reason

    for m in sorted(owner):
        reason = why_not(m, set())
        res = results[owner[m].name]
        if m in res.broken:
            reason = "does not run in minipy: " + res.broken[m].message
        if reason:
            res.bad[m] = reason
        else:
            res.ok.append(m)
    if stray:
        for res in results.values():
            res.findings.extend(Finding("error", "?", 0, s) for s in stray[:3] if s and res.bad)
    return results


def report_text(res, target):
    pkg = res.pkg
    t = ["%s %s: %s (target %s)" % (pkg.name, pkg.version,
         {"ok": "passes the check for compiled programs", "partial": "passes the check for compiled programs in part",
          "fail": "does not pass the check for compiled programs", "broken": "does not run in minipy"}[res.status()], target or "default")]
    if res.ok:
        t.append("modules that pass: " + ", ".join(res.ok))
    if res.bad:
        t.append("modules that do not (interpreter only):")
        for m in sorted(res.bad):
            t.append("  %s: %s" % (m, res.bad[m]))
    errs = [d for d in res.findings if d.kind == "error"]
    unclear = [d for d in res.findings if d.kind == "unclear"]
    if res.broken:
        t.append("what minipy cannot run (not even the interpreter):")
        t.extend("  " + res.broken[m].text() for m in sorted(res.broken))
    if errs:
        t.append("errors:")
        t.extend("  " + d.text() for d in errs)
    if unclear:
        t.append("unclear types (functions the check cannot go through):")
        t.extend("  " + d.text() for d in unclear)
    return "\n".join(t) + "\n"


def summary(res, target, report_path):
    """A few lines for the terminal."""
    pkg = res.pkg
    st = res.status()
    if st == "ok":
        return "  %s %s: passes the check - usable in compiled programs" % (pkg.name, pkg.version)
    if st == "broken":
        lines = ["  %s %s: does not run in minipy (modules %s)" % (pkg.name, pkg.version, ", ".join(sorted(res.broken)))]
        lines.extend("    " + res.broken[m].text() for m in sorted(res.broken)[:MAX_SHOWN])
        lines.append("    (report: %s)" % report_path)
        return "\n".join(lines)
    lines = ["  %s %s: %s - %s" % (pkg.name, pkg.version, "passes in part" if st == "partial" else "does not pass the check",
             "only modules %s in compiled programs" % ", ".join(res.ok) if res.ok else "interpreter only")]
    shown = [d for d in res.findings if d.kind == "error"][:MAX_SHOWN]
    shown += [d for d in res.findings if d.kind == "unclear"][:max(0, MAX_SHOWN - len(shown))]
    for d in shown:
        lines.append("    " + d.text())
    more = len(res.findings) - len(shown)
    if more > 0:
        lines.append("    ... %d more in %s" % (more, report_path))
    elif not shown:
        for m in sorted(res.bad)[:MAX_SHOWN]:
            lines.append("    %s: %s" % (m, res.bad[m]))
    return "\n".join(lines)


def write_index(project, packages, folders, results):
    """.minipy/index: a line per module of every package, what the compiler reads."""
    lines = ["# module package version status (written by minipy sync / minipy check)"]
    for pkg in packages:
        res = results.get(pkg.name)
        for name, rel in store.modules(folders[pkg.name]):
            st = "unchecked" if res is None else ("ok" if name in res.ok else "fail")
            lines.append("%s %s %s %s" % (name, pkg.name, pkg.version, st))
    os.makedirs(project.dot, exist_ok=True)
    write_text(project.index, "\n".join(lines) + "\n")


def write_reports(project, results, target):
    os.makedirs(project.reports, exist_ok=True)
    keep = set()
    for res in results.values():
        name = "%s-%s.txt" % (res.pkg.name, res.pkg.version)
        keep.add(name)
        write_text(os.path.join(project.reports, name), report_text(res, target))
    for f in os.listdir(project.reports):
        if f.endswith(".txt") and f not in keep:
            os.remove(os.path.join(project.reports, f))
