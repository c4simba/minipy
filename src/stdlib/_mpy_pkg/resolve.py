"""Choosing a version of every package a project needs.

A depth-first search with backtracking: the newest release of each package
that satisfies everything asked of it so far, its dependencies next; when a
later requirement rules a choice out, the next older release is tried. A
release qualifies when it has a pure-Python wheel and supports Python 3.14."""
from .reqs import Requirement, canonical, marker_true
from .versions import SpecifierSet, Version, VersionError
from .index import FetchError

MINIPY_PYTHON = Version("3.14.0")
MAX_STEPS = 20000


class ResolveError(Exception):
    pass


class Pin:
    """A package chosen: its release, the extras asked for, what it requires."""

    def __init__(self, name, release, extras, requires):
        self.name = name
        self.key = canonical(name)
        self.version = release.version
        self.release = release
        self.extras = extras
        self.requires = requires                      # the Requirements that apply (markers evaluated)
        self.wheel = release.wheel()


def _python_ok(requires_python):
    if not requires_python:
        return True
    try:
        return SpecifierSet(requires_python).contains(MINIPY_PYTHON, True)
    except VersionError:
        return True


class Resolver:
    def __init__(self, index, env, prereleases=False):
        self.index = index
        self.env = env
        self.prereleases = prereleases
        self.steps = 0
        self.why = ""                                 # the last conflict (for the error message)
        self.memo = {}

    def applicable(self, name, version, extras):
        """The requirements of a release that apply here (its markers, the extras asked for)."""
        out = []
        for text in self.index.requires(name, version):
            r = Requirement(text)
            if not r.marker or marker_true(r.marker, self.env, ""):
                out.append(r)
                continue
            for e in extras:
                if marker_true(r.marker, self.env, e):
                    out.append(r)
                    break
        return out

    def candidates(self, req, wanted_by):
        try:
            releases = self.index.releases(req.name)
        except FetchError as e:
            raise ResolveError(str(e))
        out = []
        skipped = []
        for rel in releases:
            pre = self.prereleases or any(s.names_prerelease() for s in req.specifier.specs)
            if not req.specifier.contains(rel.version, pre):
                continue
            w = rel.wheel()
            if w is None:
                skipped.append(str(rel.version))
                continue
            if not _python_ok(w.get("requires_python") or ""):
                continue
            out.append(rel)
        if out:
            self.index.prefetch(req.name, [r.version for r in out[:24]])
        if not out and not self.why:
            if skipped:
                self.why = "%s %s (wanted by %s) has no pure-Python wheel (py3-none-any): minipy runs no C extensions and builds no sources" % (
                    req.name, ", ".join(skipped[:3]), wanted_by)
            else:
                self.why = "no release of %s satisfies %s (wanted by %s)" % (req.name, str(req.specifier) or "any version", wanted_by)
        return out

    def satisfiable(self, req, by):
        """Whether some release could satisfy req at all (cached per requirement)."""
        key = (req.key, str(req.specifier), tuple(req.extras))
        if key not in self.memo:
            why = self.why
            self.why = ""
            ok = len(self.candidates(req, by)) > 0
            self.memo[key] = (ok, self.why)
            self.why = why if ok else self.why
        ok, why = self.memo[key]
        if not ok:
            self.why = why
        return ok

    def resolve(self, requirements):
        """{canonical name: Pin} for the requirements (Requirement objects of the project)."""
        pending = [(r, "the project") for r in requirements if not r.marker or marker_true(r.marker, self.env, "")]
        got = self.solve({}, pending)
        if got is None:
            raise ResolveError("cannot choose versions: " + (self.why or "the requirements conflict"))
        return got

    def solve(self, chosen, pending):
        self.steps += 1
        if self.steps > MAX_STEPS:
            raise ResolveError("cannot choose versions: too many combinations (" + (self.why or "conflicts") + ")")
        i = 0
        while i < len(pending):
            req, by = pending[i]
            i += 1
            if req.url:
                raise ResolveError("%s: direct URL requirements are not supported (%s)" % (by, req.text))
            pin = chosen.get(req.key)
            if pin is None:
                break
            pre = self.prereleases or any(s.names_prerelease() for s in req.specifier.specs)
            if not req.specifier.contains(pin.version, True if pre else pin.version.is_prerelease()):
                self.why = "%s %s was chosen, but %s wants %s" % (pin.name, pin.version, by, req.text)
                return None
            new = [e for e in req.extras if e not in pin.extras]
            if new:                                   # more extras of a package chosen already
                extras = pin.extras + new
                more = Pin(pin.name, pin.release, extras, self.applicable(pin.name, pin.version, extras))
                c2 = dict(chosen)
                c2[req.key] = more
                rest = pending[i:] + [(r, "%s[%s]" % (pin.name, ",".join(new))) for r in more.requires]
                return self.solve(c2, rest)
        else:
            return chosen                             # every requirement satisfied
        for rel in self.candidates(req, by):
            try:
                requires = self.applicable(rel.name, rel.version, req.extras)
            except (FetchError, ValueError) as e:
                self.why = "%s %s: %s" % (rel.name, rel.version, e)
                continue
            if not all(self.satisfiable(r, "%s %s" % (rel.name, rel.version)) for r in requires):
                continue                              # (a dependency no release satisfies: no use going deeper)
            c2 = dict(chosen)
            c2[req.key] = Pin(rel.name, rel, list(req.extras), requires)
            rest = pending[i:] + [(r, "%s %s" % (rel.name, rel.version)) for r in requires]
            got = self.solve(c2, rest)
            if got is not None:
                return got
        return None
