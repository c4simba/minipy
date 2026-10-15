"""Versions and version specifiers as PEP 440 defines them.

    Version("1.4.2rc1.post2+local")     parsed, ordered as pip orders them
    SpecifierSet(">=1.4,!=1.5.*")       contains(Version) / filters candidates

Pre-releases match only when a specifier names a pre-release itself (or the
caller allows them)."""


class VersionError(ValueError):
    pass


_PRE = {"a": 0, "alpha": 0, "b": 1, "beta": 1, "c": 2, "rc": 2, "pre": 2, "preview": 2}


class Version:
    def __init__(self, text):
        self.text = text.strip()
        s = self.text.lower()
        if s.startswith("v"):
            s = s[1:]
        self.local = ""
        if "+" in s:
            s, self.local = s.split("+", 1)
        self.epoch = 0
        if "!" in s:
            e, s = s.split("!", 1)
            if not e.isdigit():
                raise VersionError("invalid version: " + text)
            self.epoch = int(e)
        i = 0
        nums = []
        while True:                                   # the release: N(.N)*
            j = i
            while j < len(s) and s[j].isdigit():
                j += 1
            if j == i:
                raise VersionError("invalid version: " + text)
            nums.append(int(s[i:j]))
            i = j
            if i + 1 < len(s) and s[i] == "." and s[i + 1].isdigit():
                i += 1
                continue
            break
        self.release = tuple(nums)
        self.pre = None                               # (0 a / 1 b / 2 rc, n)
        self.post = None
        self.dev = None
        rest = s[i:]
        while rest:
            rest = rest.lstrip("-_.")
            word = ""
            k = 0
            while k < len(rest) and rest[k].isalpha():
                k += 1
            word = rest[:k]
            rest = rest[k:].lstrip("-_.") if word else rest
            k = 0
            while k < len(rest) and rest[k].isdigit():
                k += 1
            num = int(rest[:k]) if k else 0
            rest = rest[k:]
            if word in _PRE and self.pre is None and self.post is None and self.dev is None:
                self.pre = (_PRE[word], num)
            elif word in ("post", "rev", "r") and self.post is None and self.dev is None:
                self.post = num
            elif word == "" and k and self.post is None and self.dev is None:
                self.post = num                       # 1.0-1: a post-release
            elif word == "dev" and self.dev is None:
                self.dev = num
            else:
                raise VersionError("invalid version: " + text)

    def is_prerelease(self):
        return self.pre is not None or self.dev is not None

    def base(self):
        """epoch and release, nothing else"""
        return Version(("%d!" % self.epoch if self.epoch else "") + ".".join(str(n) for n in self.release))

    def key(self):
        rel = list(self.release)
        while len(rel) > 1 and rel[-1] == 0:
            rel.pop()
        if self.pre is None and self.post is None and self.dev is not None:
            pre = (-2, 0)                             # 1.0.dev1 comes before 1.0a1
        elif self.pre is None:
            pre = (3, 0)
        else:
            pre = self.pre
        post = (-1,) if self.post is None else (self.post,)
        dev = (1, 0) if self.dev is None else (0, self.dev)
        local = tuple((1, int(p), "") if p.isdigit() else (0, 0, p) for p in self.local.replace("-", ".").replace("_", ".").split(".")) if self.local else ()
        return (self.epoch, tuple(rel), pre, post, dev, local)

    def __eq__(self, other):
        return isinstance(other, Version) and self.key() == other.key()

    def __ne__(self, other):
        return not self.__eq__(other)

    def __lt__(self, other):
        return self.key() < other.key()

    def __le__(self, other):
        return self.key() <= other.key()

    def __gt__(self, other):
        return self.key() > other.key()

    def __ge__(self, other):
        return self.key() >= other.key()

    def __hash__(self):
        return hash(self.key())

    def __str__(self):
        return self.text

    def __repr__(self):
        return "Version(%r)" % self.text


def parse(text):
    """A Version, or None when text is not one (some old releases)."""
    try:
        return Version(text)
    except VersionError:
        return None


_OPS = ("===", "~=", "==", "!=", "<=", ">=", "<", ">")


class Specifier:
    def __init__(self, text):
        t = text.strip()
        self.op = None
        for op in _OPS:
            if t.startswith(op):
                self.op = op
                t = t[len(op):].strip()
                break
        if self.op is None:
            raise VersionError("invalid version specifier: " + text)
        self.wild = t.endswith(".*")
        if self.wild and self.op not in ("==", "!="):
            raise VersionError("invalid version specifier: " + text)
        self.text = t
        self.version = None if self.op == "===" else Version(t[:-2] if self.wild else t)

    def names_prerelease(self):
        return self.version is not None and self.version.is_prerelease()

    def contains(self, v):
        op = self.op
        if op == "===":
            return str(v) == self.text
        s = self.version
        if self.wild:
            n = len(s.release)
            same = v.epoch == s.epoch and (v.release + (0,) * n)[:n] == s.release
            return same if op == "==" else not same
        if op == "==" or op == "!=":
            vv = v if s.local else Version(str(v).split("+")[0])
            return (vv == s) == (op == "==")
        if op == "~=":
            if len(s.release) < 2:
                raise VersionError("~= needs two release numbers: " + self.text)
            prefix = s.release[:-1]
            n = len(prefix)
            return v >= s and v.epoch == s.epoch and (v.release + (0,) * n)[:n] == prefix
        if op == ">=":
            return v >= s
        if op == "<=":
            return v <= s
        if op == ">":
            if not v > s:
                return False
            if v.post is not None and s.post is None and v.base() == s.base():
                return False                          # >1.7 does not take 1.7.post1
            return not (v.local and v.base() == s.base())
        if op == "<":
            if not v < s:
                return False
            return not (v.is_prerelease() and not s.is_prerelease() and v.base() == s.base())
        return False

    def __str__(self):
        return self.op + self.text


class SpecifierSet:
    def __init__(self, text=""):
        self.specs = [Specifier(p) for p in text.split(",") if p.strip()]

    def contains(self, v, prereleases=False):
        if v.is_prerelease() and not prereleases and not any(s.names_prerelease() for s in self.specs):
            return False
        for s in self.specs:
            if not s.contains(v):
                return False
        return True

    def __str__(self):
        return ",".join(str(s) for s in self.specs)

    def __bool__(self):
        return len(self.specs) > 0
