"""Filename globbing (CPython's glob): the paths matching a pattern of shell wildcards
(* ? [seq]; ** with recursive=True for any folders in between)."""
import os
import fnmatch


def has_magic(s: str) -> bool:
    return "*" in s or "?" in s or "[" in s


def escape(pathname: str) -> str:
    """pathname with its wildcard characters made literal ([*], [?], [[])."""
    out: list[str] = []
    for c in pathname:
        if c in "*?[":
            out.append("[" + c + "]")
        else:
            out.append(c)
    return "".join(out)


def _ishidden(path: str) -> bool:
    return path[0] == "."


def _isrecursive(pattern: str) -> bool:
    return pattern == "**"


def _join(dirname: str, basename: str) -> str:
    if not dirname or not basename:
        return dirname or basename
    return os.path.join(dirname, basename)


def _lexists(p: str) -> bool:
    return os.path.lexists(p)


def _listdir(dirname: str, dironly: bool) -> list[str]:
    out: list[str] = []
    try:
        for entry in os.scandir(dirname or os.curdir):
            try:
                if not dironly or entry.is_dir():
                    out.append(entry.name)
            except OSError:
                pass
    except OSError:
        pass
    return out


def _glob0(dirname: str, basename: str, dironly: bool) -> list[str]:
    if basename:
        if _lexists(_join(dirname, basename)):
            return [basename]
    elif os.path.isdir(dirname):
        return [basename]
    return []


def _glob1(dirname: str, pattern: str, dironly: bool, include_hidden: bool) -> list[str]:
    names = _listdir(dirname, dironly)
    if not (include_hidden or _ishidden(pattern)):
        names = [x for x in names if not _ishidden(x)]
    return fnmatch.filter(names, pattern)


def _rlistdir(dirname: str, dironly: bool, include_hidden: bool):
    for x in _listdir(dirname, dironly):
        if include_hidden or not _ishidden(x):
            yield x
            path = _join(dirname, x) if dirname else x
            for y in _rlistdir(path, dironly, include_hidden):
                yield _join(x, y)


def _glob2(dirname: str, pattern: str, dironly: bool, include_hidden: bool):
    if not dirname or os.path.isdir(dirname):
        yield pattern[:0]
    for x in _rlistdir(dirname, dironly, include_hidden):
        yield x


def _in_dir(root: str, dirname: str, basename: str, recursive: bool, dironly: bool, include_hidden: bool):
    """The names matching basename in folder dirname (under root)."""
    if has_magic(basename):
        if recursive and _isrecursive(basename):
            for name in _glob2(_join(root, dirname), basename, dironly, include_hidden):
                yield name
        else:
            for name in _glob1(_join(root, dirname), basename, dironly, include_hidden):
                yield name
    else:
        for name in _glob0(_join(root, dirname), basename, dironly):
            yield name


def _iglob(pathname: str, root: str, recursive: bool, dironly: bool, include_hidden: bool):
    dirname, basename = os.path.split(pathname)
    if not has_magic(pathname):
        if basename:
            if _lexists(_join(root, pathname)):
                yield pathname
        elif os.path.isdir(_join(root, dirname)):
            yield pathname
        return
    if not dirname:
        for name in _in_dir(root, dirname, basename, recursive, dironly, include_hidden):
            yield name
        return
    if dirname != pathname and has_magic(dirname):
        for d in _iglob(dirname, root, recursive, True, include_hidden):
            for name in _in_dir(root, d, basename, recursive, dironly, include_hidden):
                yield os.path.join(d, name)
    else:
        for name in _in_dir(root, dirname, basename, recursive, dironly, include_hidden):
            yield os.path.join(dirname, name)


def iglob(pathname: str, *, root_dir: str | None = None, dir_fd: int | None = None, recursive: bool = False,
          include_hidden: bool = False):
    """The paths matching pathname, one at a time (relative to root_dir when given)."""
    root = root_dir if root_dir is not None else ""
    skip_empty = not pathname or (recursive and _isrecursive(pathname[:2]))
    for p in _iglob(pathname, root, recursive, False, include_hidden):
        if skip_empty and not p:
            skip_empty = False
            continue
        skip_empty = False
        yield p


def glob(pathname: str, *, root_dir: str | None = None, dir_fd: int | None = None, recursive: bool = False,
         include_hidden: bool = False) -> list[str]:
    """The paths matching pathname (relative to root_dir when given)."""
    return list(iglob(pathname, root_dir=root_dir, dir_fd=dir_fd, recursive=recursive, include_hidden=include_hidden))
