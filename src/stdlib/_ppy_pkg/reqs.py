"""Requirements and environment markers as PEP 508 defines them.

    Requirement('requests[socks]>=2.31; python_version >= "3.8"')
    marker_true(marker, environment(), extra="socks")

plainpy's environment: Python 3.14, implementation "plainpy" (not CPython: a
package's `platform_python_implementation == "CPython"` extras - C speedups
- are left out), the host's sys_platform."""
import sys
from .versions import SpecifierSet, Version, VersionError


class RequirementError(ValueError):
    pass


def canonical(name):
    """PEP 503: lower case, runs of - _ . as one -"""
    out = []
    dash = False
    for ch in name.strip().lower():
        if ch in "-_.":
            if not dash:
                out.append("-")
            dash = True
        else:
            out.append(ch)
            dash = False
    return "".join(out)


def _name_char(ch):
    return ch.isalnum() or ch in "-_."


class Requirement:
    def __init__(self, text):
        self.text = text.strip()
        s = self.text
        i = 0
        while i < len(s) and _name_char(s[i]):
            i += 1
        if i == 0:
            raise RequirementError("invalid requirement: " + text)
        self.name = s[:i]
        self.key = canonical(self.name)
        rest = s[i:].strip()
        self.extras = []
        if rest.startswith("["):
            end = rest.find("]")
            if end < 0:
                raise RequirementError("invalid requirement: " + text)
            self.extras = [canonical(e) for e in rest[1:end].split(",") if e.strip()]
            rest = rest[end + 1:].strip()
        self.marker = ""
        semi = rest.find(";")
        if semi >= 0:
            self.marker = rest[semi + 1:].strip()
            rest = rest[:semi].strip()
        self.url = ""
        if rest.startswith("@"):
            self.url = rest[1:].strip()
            rest = ""
        if rest.startswith("(") and rest.endswith(")"):
            rest = rest[1:-1].strip()
        try:
            self.specifier = SpecifierSet(rest)
        except VersionError as e:
            raise RequirementError(str(e) + " in " + text)
        if self.marker:
            parse_marker(self.marker)                 # (an error now, not when evaluated)

    def __str__(self):
        return self.text


# ---------------------------------------------------------------- markers

def environment(platform=""):
    """The values markers are evaluated with (platform: a sys.platform to
    resolve for, by default the host's)."""
    plat = platform or sys.platform
    system = {"darwin": "Darwin", "linux": "Linux", "kolibrios": "KolibriOS", "win32": "Windows"}.get(plat, plat)
    return {
        "python_version": "3.14", "python_full_version": "3.14.0",
        "implementation_name": "plainpy", "implementation_version": "3.14.0",
        "platform_python_implementation": "PlainPy",
        "os_name": "nt" if plat == "win32" else "posix", "sys_platform": plat, "platform_system": system,
        "platform_machine": "", "platform_release": "", "platform_version": "",
    }


_VERSION_VARS = ("python_version", "python_full_version", "implementation_version", "platform_release")


def _tokens(s):
    out = []
    i = 0
    while i < len(s):
        ch = s[i]
        if ch in " \t":
            i += 1
        elif ch in "()":
            out.append(ch)
            i += 1
        elif ch in "'\"":
            j = s.find(ch, i + 1)
            if j < 0:
                raise RequirementError("unterminated string in marker: " + s)
            out.append(("str", s[i + 1:j]))
            i = j + 1
        elif ch in "<>=!~":
            j = i
            while j < len(s) and s[j] in "<>=!~":
                j += 1
            out.append(("op", s[i:j]))
            i = j
        else:
            j = i
            while j < len(s) and (s[j].isalnum() or s[j] in "_."):
                j += 1
            if j == i:
                raise RequirementError("unexpected character in marker: " + s)
            w = s[i:j]
            if w in ("and", "or", "in", "not"):
                out.append(w)
            else:
                out.append(("var", w))
            i = j
    return out


class _Parser:
    def __init__(self, text):
        self.text = text
        self.t = _tokens(text)
        self.i = 0

    def peek(self):
        return self.t[self.i] if self.i < len(self.t) else None

    def take(self):
        tok = self.peek()
        self.i += 1
        return tok

    def expr(self):
        node = self.conj()
        while self.peek() == "or":
            self.take()
            node = ("or", node, self.conj())
        return node

    def conj(self):
        node = self.atom()
        while self.peek() == "and":
            self.take()
            node = ("and", node, self.atom())
        return node

    def atom(self):
        if self.peek() == "(":
            self.take()
            node = self.expr()
            if self.take() != ")":
                raise RequirementError("missing ) in marker: " + self.text)
            return node
        left = self.value()
        op = self.take()
        if op == "not":
            if self.take() != "in":
                raise RequirementError("expected 'not in' in marker: " + self.text)
            op = "not in"
        elif isinstance(op, tuple) and op[0] == "op":
            op = op[1]
        elif op != "in":
            raise RequirementError("expected a comparison in marker: " + self.text)
        return ("cmp", left, op, self.value())

    def value(self):
        tok = self.take()
        if not isinstance(tok, tuple) or tok[0] == "op":
            raise RequirementError("expected a value in marker: " + self.text)
        return tok


def parse_marker(text):
    p = _Parser(text)
    node = p.expr()
    if p.peek() is not None:
        raise RequirementError("unexpected text in marker: " + text)
    return node


def _value(v, env, extra):
    if v[0] == "str":
        return v[1]
    name = v[1]
    if name == "extra":
        return extra
    if name not in env:
        raise RequirementError("unknown marker variable: " + name)
    return env[name]


def _compare(left, op, right, versions):
    if op == "in":
        return left in right
    if op == "not in":
        return left not in right
    if versions:
        try:
            lv = Version(left)
            if op in ("==", "!=") and right.endswith(".*"):
                from .versions import Specifier
                return Specifier(op + right).contains(lv)
            rv = Version(right)
            if op == "~=":
                from .versions import Specifier
                return Specifier("~=" + right).contains(lv)
            return {"==": lv == rv, "!=": lv != rv, "<": lv < rv, "<=": lv <= rv, ">": lv > rv, ">=": lv >= rv,
                    "===": left == right}.get(op, False)
        except VersionError:
            pass
    return {"==": left == right, "!=": left != right, "<": left < right, "<=": left <= right,
            ">": left > right, ">=": left >= right, "===": left == right}.get(op, False)


def _eval(node, env, extra):
    kind = node[0]
    if kind == "or":
        return _eval(node[1], env, extra) or _eval(node[2], env, extra)
    if kind == "and":
        return _eval(node[1], env, extra) and _eval(node[2], env, extra)
    a = node[1]
    b = node[3]
    op = node[2]
    if (a[0] == "var" and a[1] == "extra") or (b[0] == "var" and b[1] == "extra"):
        left = canonical(_value(a, env, extra))
        right = canonical(_value(b, env, extra))
        return _compare(left, op, right, False)
    versions = (a[0] == "var" and a[1] in _VERSION_VARS) or (b[0] == "var" and b[1] in _VERSION_VARS)
    return _compare(_value(a, env, extra), op, _value(b, env, extra), versions)


def marker_true(marker, env, extra=""):
    """Whether a marker holds in env (extra: the extra being asked for, "" none)."""
    if not marker:
        return True
    return _eval(parse_marker(marker), env, extra)
