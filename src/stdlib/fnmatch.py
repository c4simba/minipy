"""Unix shell-style wildcards (CPython's fnmatch): * any run, ? one character,
[seq] one of seq (ranges a-z), [!seq] one not in seq. Names are compared as
they are (posix: case matters)."""
import posixpath


def _set_match(s: str, c: str) -> bool:
    """c is in set s (the text between [ and ]: ranges x-y, a - first or last is itself)."""
    i = 0
    n = len(s)
    while i < n:
        if i + 2 < n and s[i + 1] == "-":
            if s[i] <= c <= s[i + 2]:
                return True
            i += 3
        else:
            if s[i] == c:
                return True
            i += 1
    return False


def _bracket(pat: str, j: int) -> int:
    """The index of the ] closing the [ at pat[j], or -1 (then [ is itself)."""
    k = j + 1
    n = len(pat)
    if k < n and pat[k] == "!":
        k += 1
    if k < n and pat[k] == "]":
        k += 1
    while k < n and pat[k] != "]":
        k += 1
    return k if k < n else -1


def fnmatchcase(name: str, pat: str) -> bool:
    """name matches pattern pat (case and all)."""
    i = 0
    j = 0
    n = len(name)
    m = len(pat)
    star_j = -1
    star_i = 0
    while i < n:
        if j < m:
            c = pat[j]
            if c == "*":
                star_j = j
                star_i = i
                j += 1
                continue
            if c == "?":
                i += 1
                j += 1
                continue
            if c == "[":
                e = _bracket(pat, j)
                if e >= 0:
                    neg = pat[j + 1] == "!"
                    inside = pat[j + 2:e] if neg else pat[j + 1:e]
                    if _set_match(inside, name[i]) != neg:
                        i += 1
                        j = e + 1
                        continue
                elif name[i] == "[":
                    i += 1
                    j += 1
                    continue
            elif c == name[i]:
                i += 1
                j += 1
                continue
        if star_j >= 0:
            star_i += 1
            i = star_i
            j = star_j + 1
            continue
        return False
    while j < m and pat[j] == "*":
        j += 1
    return j == m


def fnmatch(name: str, pat: str) -> bool:
    """name matches pattern pat (both through os.path.normcase)."""
    return fnmatchcase(posixpath.normcase(name), posixpath.normcase(pat))


def filter(names: list[str], pat: str) -> list[str]:
    """The names that match pat."""
    pat = posixpath.normcase(pat)
    out: list[str] = []
    for name in names:
        if fnmatchcase(posixpath.normcase(name), pat):
            out.append(name)
    return out


def filterfalse(names: list[str], pat: str) -> list[str]:
    """The names that do not match pat."""
    pat = posixpath.normcase(pat)
    out: list[str] = []
    for name in names:
        if not fnmatchcase(posixpath.normcase(name), pat):
            out.append(name)
    return out
