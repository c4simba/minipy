"""Miscellaneous operating system interfaces: the parts of CPython's os minipy has.

Files, folders and descriptors work on every platform minipy runs on, KolibriOS
included (see _os for the system calls under them); os.path is posixpath."""
import sys
import _os
from typing import Callable
import posixpath as path
from _os import (O_RDONLY, O_WRONLY, O_RDWR, O_ACCMODE, O_CREAT, O_EXCL, O_NOCTTY, O_TRUNC, O_APPEND, O_NONBLOCK,
                 O_DIRECTORY, O_CLOEXEC, SEEK_SET, SEEK_CUR, SEEK_END, F_OK, R_OK, W_OK, X_OK,
                 stat_result, strerror, open, close, read, write, lseek, ftruncate, truncate, fsync, isatty,
                 stat, lstat, fstat, listdir, mkdir, rmdir, unlink, rename, getcwd, chdir, getpid, getppid,
                 getuid, getgid, kill, urandom, readlink, symlink, chmod, environ, utime, umask, link)
from posixpath import sep, curdir, pardir, extsep, pathsep, defpath, altsep, devnull

name = "posix"
linesep = "\n"


def remove(p: str) -> None:
    """Delete file p (a folder: IsADirectoryError)."""
    unlink(p)


def replace(src: str, dst: str) -> None:
    rename(src, dst)


def access(p: str, mode: int) -> bool:
    return _os.access(p, mode)


def fspath(p: str) -> str:
    """The text of path p (a str, or an object with __fspath__)."""
    if not sys._compiled:
        if not isinstance(p, (str, bytes)):
            f = getattr(type(p), "__fspath__", None)
            if f is None:
                raise TypeError("expected str, bytes or os.PathLike object, not " + type(p).__name__)
            return f(p)
    return p


def fsencode(filename: str) -> bytes:
    return filename.encode()


def fsdecode(filename: bytes) -> str:
    return filename.decode()


def getenv(key: str, default: str | None = None) -> str | None:
    v = environ.get(key)
    if v is None:
        return default
    return v


def putenv(key: str, value: str) -> None:
    environ[key] = value


def unsetenv(key: str) -> None:
    if key in environ:
        del environ[key]


def makedirs(name: str, mode: int = 0o777, exist_ok: bool = False) -> None:
    """Make folder name and the folders above it that are missing."""
    head, tail = path.split(name)
    if not tail:
        head, tail = path.split(head)
    if head and tail and not path.exists(head):
        try:
            makedirs(head, mode, exist_ok)
        except FileExistsError:
            pass
        if tail == curdir:
            return
    try:
        mkdir(name, mode)
    except OSError:
        if not exist_ok or not path.isdir(name):
            raise


def removedirs(name: str) -> None:
    """Remove folder name, then the empty folders above it."""
    rmdir(name)
    head, tail = path.split(name)
    if not tail:
        head, tail = path.split(head)
    while head and tail:
        try:
            rmdir(head)
        except OSError:
            break
        head, tail = path.split(head)


def renames(old: str, new: str) -> None:
    head, tail = path.split(new)
    if head and tail and not path.exists(head):
        makedirs(head)
    rename(old, new)
    head, tail = path.split(old)
    if head and tail:
        try:
            removedirs(head)
        except OSError:
            pass


class DirEntry:
    """An entry scandir() gives: its name, path and kind."""

    def __init__(self, folder: str, name: str, kind: int):
        self.name = name
        self.path = path.join(folder, name)
        self._kind = kind

    def is_dir(self, follow_symlinks: bool = True) -> bool:
        if self._kind == 1:
            return True
        if self._kind == 0 or self._kind == 3 or (self._kind == 2 and not follow_symlinks):
            return False
        return path.isdir(self.path)

    def is_file(self, follow_symlinks: bool = True) -> bool:
        if self._kind == 0:
            return True
        if self._kind == 1 or self._kind == 3 or (self._kind == 2 and not follow_symlinks):
            return False
        return path.isfile(self.path)

    def is_symlink(self) -> bool:
        if self._kind >= 0:
            return self._kind == 2
        return path.islink(self.path)

    def stat(self, follow_symlinks: bool = True) -> stat_result:
        if follow_symlinks:
            return stat(self.path)
        return lstat(self.path)

    def inode(self) -> int:
        return lstat(self.path).st_ino

    def __fspath__(self) -> str:
        return self.path

    def __repr__(self) -> str:
        return "<DirEntry " + repr(self.name) + ">"


class _ScandirIterator:
    def __init__(self, entries: list[DirEntry]):
        self._entries = entries
        self._i = 0

    def __iter__(self) -> "_ScandirIterator":
        return self

    def __next__(self) -> DirEntry:
        if self._i >= len(self._entries):
            raise StopIteration
        e = self._entries[self._i]
        self._i += 1
        return e

    def __enter__(self) -> "_ScandirIterator":
        return self

    def __exit__(self, et, ev, tb) -> bool:
        return False

    def close(self) -> None:
        self._i = len(self._entries)


def scandir(p: str = ".") -> _ScandirIterator:
    """The entries of folder p as DirEntry objects (with statement: closes it)."""
    return _ScandirIterator([DirEntry(p, name, kind) for name, kind in _os.listdir_types(p)])


def walk(top: str, topdown: bool = True, onerror: Callable[[OSError], None] | None = None, followlinks: bool = False):
    """(folder, its folders' names, its other names) for top and every folder below it."""
    try:
        entries = _os.listdir_types(top)
    except OSError as e:
        if onerror is not None:
            onerror(e)
        return
    dirs: list[str] = []
    nondirs: list[str] = []
    for name, kind in entries:
        is_dir = kind == 1
        if kind < 0 or kind == 2:
            is_dir = path.isdir(path.join(top, name))
        if is_dir:
            dirs.append(name)
        else:
            nondirs.append(name)
    if topdown:
        yield (top, dirs, nondirs)
    for d in dirs:
        new_path = path.join(top, d)
        if followlinks or not path.islink(new_path):
            yield from walk(new_path, topdown, onerror, followlinks)
    if not topdown:
        yield (top, dirs, nondirs)


class terminal_size:
    def __init__(self, seq: tuple[int, int]):
        self.columns = seq[0]
        self.lines = seq[1]

    def __getitem__(self, i: int) -> int:
        return [self.columns, self.lines][i]

    def __repr__(self) -> str:
        return "os.terminal_size(columns=" + str(self.columns) + ", lines=" + str(self.lines) + ")"


def get_terminal_size(fd: int = 1) -> terminal_size:
    c, l = _os.terminal_size_of(fd)
    return terminal_size((c, l))


class uname_result:
    def __init__(self, sysname: str, nodename: str, release: str, version: str, machine: str):
        self.sysname = sysname
        self.nodename = nodename
        self.release = release
        self.version = version
        self.machine = machine

    def __getitem__(self, i: int) -> str:
        return [self.sysname, self.nodename, self.release, self.version, self.machine][i]

    def __repr__(self) -> str:
        return ("posix.uname_result(sysname=" + repr(self.sysname) + ", nodename=" + repr(self.nodename) + ", release=" +
                repr(self.release) + ", version=" + repr(self.version) + ", machine=" + repr(self.machine) + ")")


def uname() -> uname_result:
    s, n, r, v, m = _os.uname()
    return uname_result(s, n, r, v, m)


def cpu_count() -> int:
    return 1
