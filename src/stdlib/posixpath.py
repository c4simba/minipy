"""Path names on POSIX: os.path (CPython's posixpath and genericpath, for str paths).

KolibriOS paths have the same form ("/sys/lib/x.obj", relative ones from the
current folder), so this module serves there too."""
import _os
import sys
from typing import TypeVar

_RS = TypeVar("_RS")

curdir = "."
pardir = ".."
extsep = "."
sep = "/"
pathsep = ":"
defpath = "/bin:/usr/bin"
altsep: str | None = None
devnull = "/dev/null"
supports_unicode_filenames = False
if sys.platform == "darwin":
    _ELOOP = 62
else:
    _ELOOP = 40


def normcase(s: str) -> str:
    return s


def isabs(s: str) -> bool:
    return s.startswith(sep)


def join(a: str, *p: str) -> str:
    """Join one or more path segments; an absolute one discards what came before."""
    path = a
    for b in p:
        if b.startswith(sep):
            path = b
        elif not path or path.endswith(sep):
            path += b
        else:
            path += sep + b
    return path


def split(p: str) -> tuple[str, str]:
    """(head, tail): tail is everything after the last slash."""
    i = p.rfind(sep) + 1
    head = p[:i]
    tail = p[i:]
    if head and head != sep * len(head):
        head = head.rstrip(sep)
    return (head, tail)


def splitext(p: str) -> tuple[str, str]:
    """(root, ext): ext is the last dot and what follows it, leading dots of a name are not one."""
    sep_index = p.rfind(sep)
    dot_index = p.rfind(extsep)
    if dot_index > sep_index:
        name_index = sep_index + 1
        while name_index < dot_index:
            if p[name_index] != extsep:
                return (p[:dot_index], p[dot_index:])
            name_index += 1
    return (p, "")


def splitdrive(p: str) -> tuple[str, str]:
    return ("", p)


def splitroot(p: str) -> tuple[str, str, str]:
    if p[:1] != sep:
        return ("", "", p)
    if p[1:2] != sep or p[2:3] == sep:
        return ("", sep, p[1:])
    return ("", p[:2], p[2:])


def basename(p: str) -> str:
    i = p.rfind(sep) + 1
    return p[i:]


def dirname(p: str) -> str:
    i = p.rfind(sep) + 1
    head = p[:i]
    if head and head != sep * len(head):
        head = head.rstrip(sep)
    return head


def normpath(path: str) -> str:
    """Collapse redundant separators and up-level references (A//B, A/./B and A/foo/../B all become A/B)."""
    if not path:
        return curdir
    initial_slashes = 0
    if path.startswith(sep):
        initial_slashes = 1
        if path.startswith(sep * 2) and not path.startswith(sep * 3):
            initial_slashes = 2
    new_comps: list[str] = []
    for comp in path.split(sep):
        if comp == "" or comp == curdir:
            continue
        if comp != pardir or (not initial_slashes and not new_comps) or (new_comps and new_comps[-1] == pardir):
            new_comps.append(comp)
        elif new_comps:
            new_comps.pop()
    path = sep.join(new_comps)
    if initial_slashes:
        path = sep * initial_slashes + path
    if not path:
        return curdir
    return path


def abspath(path: str) -> str:
    if not isabs(path):
        path = join(_os.getcwd(), path)
    return normpath(path)


def _readlink(path: str) -> str | None:
    """The target of symbolic link path, or None when it is not one."""
    try:
        st = _os.lstat(path)
    except OSError:
        return None
    if not _os.S_ISLNK(st.st_mode):
        return None
    return _os.readlink(path)


class _AllowMissing:
    """realpath(strict=ALLOW_MISSING): only a missing part of the path is no error."""

    def __repr__(self) -> str:
        return "os.path.ALLOW_MISSING"


ALLOW_MISSING = _AllowMissing()


def realpath(filename: str, strict: _RS = False) -> str:
    """The canonical path: symbolic links resolved (on KolibriOS: the absolute path); strict: an error for
    a part that is not there (ALLOW_MISSING: only for other errors)."""
    allow_missing = False
    strict_on = False
    if isinstance(strict, _AllowMissing):
        allow_missing = True
    elif strict:
        strict_on = True
    path = filename
    if not isabs(path):
        path = join(_os.getcwd(), path)
    parts = [x for x in path.split(sep) if x and x != curdir]
    resolved = ""
    i = 0
    seen = 0
    while i < len(parts):
        name = parts[i]
        i += 1
        if name == pardir:
            resolved = dirname(resolved)
            if resolved == sep:
                resolved = ""
            continue
        candidate = resolved + sep + name
        target = _readlink(candidate)
        if target is None:
            if allow_missing:
                try:
                    _os.stat(candidate)
                except FileNotFoundError:
                    pass
            elif strict_on:
                _os.stat(candidate)
            resolved = candidate
            continue
        seen += 1
        if seen > 40:
            if strict_on or allow_missing:
                raise OSError(_ELOOP, _os.strerror(_ELOOP), candidate)
            resolved = candidate
            continue
        rest = parts[i:]
        if target.startswith(sep):
            resolved = ""
        parts = [x for x in target.split(sep) if x and x != curdir] + rest
        i = 0
    if not resolved:
        return sep
    return resolved


def relpath(path: str, start: str | None = None) -> str:
    """path relative to start (the current folder by default)."""
    if not path:
        raise ValueError("no path specified")
    if start is None:
        start = curdir
    start_list = [x for x in abspath(start).split(sep) if x]
    path_list = [x for x in abspath(path).split(sep) if x]
    i = 0
    while i < len(start_list) and i < len(path_list) and start_list[i] == path_list[i]:
        i += 1
    rel_list = [pardir] * (len(start_list) - i) + path_list[i:]
    if not rel_list:
        return curdir
    return sep.join(rel_list)


def commonprefix(m: list[str]) -> str:
    """The longest common leading string (character by character)."""
    if not m:
        return ""
    s1 = min(m)
    s2 = max(m)
    for i in range(len(s1)):
        if s1[i] != s2[i]:
            return s1[:i]
    return s1


def commonpath(paths: list[str]) -> str:
    """The longest common sub-path of the paths."""
    if not paths:
        raise ValueError("commonpath() arg is an empty sequence")
    isabs0 = paths[0].startswith(sep)
    for p in paths:
        if p.startswith(sep) != isabs0:
            raise ValueError("Can't mix absolute and relative paths")
    split_paths = [[c for c in p.split(sep) if c and c != curdir] for p in paths]
    s1 = min(split_paths)
    s2 = max(split_paths)
    common = s1
    for i in range(len(s1)):
        if s1[i] != s2[i]:
            common = s1[:i]
            break
    prefix = ""
    if isabs0:
        prefix = sep
    return prefix + sep.join(common)


def exists(path: str) -> bool:
    try:
        _os.stat(path)
    except (OSError, ValueError):
        return False
    return True


def lexists(path: str) -> bool:
    try:
        _os.lstat(path)
    except (OSError, ValueError):
        return False
    return True


def isfile(path: str) -> bool:
    try:
        st = _os.stat(path)
    except (OSError, ValueError):
        return False
    return _os.S_ISREG(st.st_mode)


def isdir(s: str) -> bool:
    try:
        st = _os.stat(s)
    except (OSError, ValueError):
        return False
    return _os.S_ISDIR(st.st_mode)


def islink(path: str) -> bool:
    try:
        st = _os.lstat(path)
    except (OSError, ValueError):
        return False
    return _os.S_ISLNK(st.st_mode)


def ismount(path: str) -> bool:
    if path == sep:
        return True
    try:
        s1 = _os.lstat(path)
    except (OSError, ValueError):
        return False
    if _os.S_ISLNK(s1.st_mode):
        return False
    try:
        s2 = _os.lstat(join(path, pardir))
    except (OSError, ValueError):
        return False
    return s1.st_dev != s2.st_dev or s1.st_ino == s2.st_ino


def getsize(filename: str) -> int:
    return _os.stat(filename).st_size


def getmtime(filename: str) -> float:
    return _os.stat(filename).st_mtime


def getatime(filename: str) -> float:
    return _os.stat(filename).st_atime


def getctime(filename: str) -> float:
    return _os.stat(filename).st_ctime


def samefile(f1: str, f2: str) -> bool:
    s1 = _os.stat(f1)
    s2 = _os.stat(f2)
    if s1.st_ino == 0 and s2.st_ino == 0:
        return abspath(f1) == abspath(f2)          # (KolibriOS has no inode numbers)
    return s1.st_ino == s2.st_ino and s1.st_dev == s2.st_dev


def expanduser(path: str) -> str:
    """~ and ~/... with $HOME (~user is left as it is)."""
    if not path.startswith("~"):
        return path
    i = path.find(sep, 1)
    if i < 0:
        i = len(path)
    if i != 1:
        return path
    home = _os.environ.get("HOME")
    if home is None:
        return path
    if home != sep:
        home = home.rstrip(sep)
    result = home + path[i:]
    if not result:
        return sep
    return result


def expandvars(path: str) -> str:
    """$name and ${name} from the environment (unknown ones are left as they are)."""
    if "$" not in path:
        return path
    out = ""
    i = 0
    n = len(path)
    while i < n:
        ch = path[i]
        if ch != "$" or i + 1 >= n:
            out += ch
            i += 1
            continue
        if path[i + 1] == "{":
            j = path.find("}", i + 2)
            if j < 0:
                out += ch
                i += 1
                continue
            name = path[i + 2:j]
            end = j + 1
        else:
            j = i + 1
            while j < n and (path[j].isalnum() or path[j] == "_") and path[j].isascii():
                j += 1
            name = path[i + 1:j]
            end = j
        value = _os.environ.get(name)
        if not name or value is None:
            out += path[i:end]
        else:
            out += value
        i = end
    return out
