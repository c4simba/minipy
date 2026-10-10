"""Object-oriented filesystem paths (CPython's pathlib): PurePath / PurePosixPath
(text only) and Path / PosixPath (the files themselves). Paths are POSIX ones
(KolibriOS's /rd/1/... included).

In compiled programs the parts given to a path (Path(a, b), p / x, joinpath)
are str."""
import os
import sys
import io
import fnmatch
from typing import Callable


def _fs(p) -> str:
    """The text of a path given as a str or a path object (os.PathLike)."""
    if isinstance(p, str):
        return p
    else:
        return p.__fspath__()


def _parse(raw: str) -> tuple[str, list[str]]:
    """(root, the parts after it) of a path: '' or '/' (or '//', two leading slashes exactly)."""
    if not raw:
        return ("", [])
    root = ""
    if raw.startswith("/"):
        root = "//" if raw.startswith("//") and not raw.startswith("///") else "/"
    return (root, [x for x in raw.split("/") if x and x != "."])


def _join_raw(segments: list[str]) -> str:
    """The path made of segments (a later absolute one starts over)."""
    out = ""
    for s in segments:
        if s.startswith("/") or not out:
            out = s
        elif out.endswith("/"):
            out = out + s
        else:
            out = out + "/" + s
    return out


def _format(root: str, tail: list[str]) -> str:
    return root + "/".join(tail)


def _quote_uri(s: str) -> str:
    safe = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-~/"
    out: list[str] = []
    for b in s.encode("utf-8"):
        c = chr(b)
        if b < 128 and c in safe:
            out.append(c)
        else:
            out.append("%" + "0123456789ABCDEF"[b >> 4] + "0123456789ABCDEF"[b & 15])
    return "".join(out)


def _match_parts(path: list[str], pat: list[str]) -> bool:
    """path's parts match pattern parts pat (** stands for any parts)."""
    if not pat:
        return not path
    if pat[0] == "**":
        for k in range(len(path) + 1):
            if _match_parts(path[k:], pat[1:]):
                return True
        return False
    if not path:
        return False
    return fnmatch.fnmatchcase(path[0], pat[0]) and _match_parts(path[1:], pat[1:])


class PurePath:
    """A path as text: its parts, name, suffix, parent ... (no file system access)."""

    def __init__(self, *args: str):
        if not sys._compiled:
            args = tuple(_fs(a) for a in args)
        raw = _join_raw(list(args))
        self._root, self._tail = _parse(raw)
        self._str = _format(self._root, self._tail) or "."

    def with_segments(self, *args: str) -> "PurePath":
        return PurePath(*args)

    def _from(self, root: str, tail: list[str]) -> "PurePath":
        return PurePath(_format(root, tail))

    def __str__(self) -> str:
        return self._str

    def __fspath__(self) -> str:
        return self._str

    def as_posix(self) -> str:
        return self._str

    def __bytes__(self) -> bytes:
        return self._str.encode("utf-8")

    def __repr__(self) -> str:
        return type(self).__name__ + "(" + repr(self.as_posix()) + ")"

    def __hash__(self) -> int:
        return hash(self._str)

    def __eq__(self, other: "PurePath") -> bool:
        if not sys._compiled:
            if not isinstance(other, PurePath):
                return NotImplemented
        return self._str == other._str

    def __ne__(self, other: "PurePath") -> bool:
        return not self == other

    def _key(self) -> list[str]:
        return self._str.split("/")

    def __lt__(self, other: "PurePath") -> bool:
        return self._key() < other._key()

    def __le__(self, other: "PurePath") -> bool:
        return self._key() <= other._key()

    def __gt__(self, other: "PurePath") -> bool:
        return self._key() > other._key()

    def __ge__(self, other: "PurePath") -> bool:
        return self._key() >= other._key()

    @property
    def drive(self) -> str:
        return ""

    @property
    def root(self) -> str:
        return self._root

    @property
    def anchor(self) -> str:
        return self._root

    @property
    def parts(self) -> tuple[str, ...]:
        if self._root:
            return tuple([self._root] + self._tail)
        return tuple(self._tail)

    @property
    def name(self) -> str:
        return self._tail[-1] if self._tail else ""

    @property
    def suffix(self) -> str:
        name = self.name.lstrip(".")
        i = name.rfind(".")
        if i != -1:
            return name[i:]
        return ""

    @property
    def suffixes(self) -> list[str]:
        return ["." + ext for ext in self.name.lstrip(".").split(".")[1:]]

    @property
    def stem(self) -> str:
        name = self.name
        i = name.rfind(".")
        if i != -1:
            stem = name[:i]
            if stem.lstrip("."):
                return stem
        return name

    def _with_name(self, name: str) -> str:
        if not name or "/" in name or name == ".":
            raise ValueError("Invalid name " + repr(name))
        if not self._tail:
            raise ValueError(repr(self) + " has an empty name")
        return _format(self._root, self._tail[:-1] + [name])

    def _with_suffix(self, suffix: str) -> str:
        stem = self.stem
        if not stem:
            raise ValueError(repr(self) + " has an empty name")
        if suffix and not suffix.startswith("."):
            raise ValueError("Invalid suffix " + repr(suffix))
        return self._with_name(stem + suffix)

    def _with_stem(self, stem: str) -> str:
        suffix = self.suffix
        if not suffix:
            return self._with_name(stem)
        if not stem:
            raise ValueError(repr(self) + " has a non-empty suffix")
        return self._with_name(stem + suffix)

    def _parent_str(self) -> str:
        if not self._tail:
            return self._str
        return _format(self._root, self._tail[:-1]) or "."

    def _parent_strs(self) -> list[str]:
        out: list[str] = []
        for k in range(len(self._tail) - 1, -1, -1):
            out.append(_format(self._root, self._tail[:k]) or ".")
        return out

    def _relative_str(self, other: str, walk_up: bool) -> str:
        o_root, o_tail = _parse(other)
        o_str = _format(o_root, o_tail) or "."
        step = 0
        base: list[str] = []
        found = False
        cands = [o_tail[:k] for k in range(len(o_tail), -1, -1)]
        for k in range(len(cands)):
            cand = cands[k]
            if o_root == self._root and len(cand) <= len(self._tail) and self._tail[:len(cand)] == cand:
                step = k
                base = cand
                found = True
                break
            if not walk_up:
                raise ValueError(repr(self._str) + " is not in the subpath of " + repr(o_str))
            if cand and cand[-1] == "..":
                raise ValueError("'..' segment in " + repr(o_str) + " cannot be walked")
        if not found:
            raise ValueError(repr(self._str) + " and " + repr(o_str) + " have different anchors")
        return "/".join([".."] * step + self._tail[len(base):]) or "."

    def is_relative_to(self, other: str) -> bool:
        o_root, o_tail = _parse(_fs(other))
        return o_root == self._root and self._tail[:len(o_tail)] == o_tail

    def is_absolute(self) -> bool:
        return self._root != ""

    def is_reserved(self) -> bool:
        return False

    def as_uri(self) -> str:
        if not self.is_absolute():
            raise ValueError("relative path can't be expressed as a file URI")
        return "file://" + _quote_uri(self._str)

    def match(self, path_pattern: str, *, case_sensitive: bool | None = None) -> bool:
        """The path ends with parts matching path_pattern (all of it when the pattern is absolute)."""
        p_root, p_tail = _parse(path_pattern)
        pat = ([p_root] if p_root else []) + p_tail
        path = list(self.parts)
        if not pat:
            raise ValueError("empty pattern")
        if len(path) < len(pat):
            return False
        if len(path) > len(pat) and p_root:
            return False
        for k in range(1, len(pat) + 1):
            if not fnmatch.fnmatchcase(path[-k], pat[-k]):
                return False
        return True

    def full_match(self, pattern: str, *, case_sensitive: bool | None = None) -> bool:
        """The whole path matches pattern (** for any parts)."""
        path = self._str.split("/") if self.parts else []
        p = PurePath(pattern)
        pat = p._str.split("/") if p.parts else []
        return _match_parts(path, pat)

    # (the paths these give are of the class of the path: Path overrides them)
    def __truediv__(self, key: str) -> "PurePath":
        return PurePath(self._str, key)

    def __rtruediv__(self, key: str) -> "PurePath":
        return PurePath(key, self._str)

    def joinpath(self, *segments: str) -> "PurePath":
        return PurePath(self._str, *segments)

    @property
    def parent(self) -> "PurePath":
        return PurePath(self._parent_str())

    @property
    def parents(self) -> list["PurePath"]:
        return [PurePath(x) for x in self._parent_strs()]

    def with_name(self, name: str) -> "PurePath":
        return PurePath(self._with_name(name))

    def with_stem(self, stem: str) -> "PurePath":
        return PurePath(self._with_stem(stem))

    def with_suffix(self, suffix: str) -> "PurePath":
        return PurePath(self._with_suffix(suffix))

    def relative_to(self, other: str, *, walk_up: bool = False) -> "PurePath":
        return PurePath(self._relative_str(_fs(other), walk_up))


class PurePosixPath(PurePath):
    """A POSIX path as text."""

    def with_segments(self, *args: str) -> "PurePosixPath":
        return PurePosixPath(*args)

    def __truediv__(self, key: str) -> "PurePosixPath":
        return PurePosixPath(self._str, key)

    def __rtruediv__(self, key: str) -> "PurePosixPath":
        return PurePosixPath(key, self._str)

    def joinpath(self, *segments: str) -> "PurePosixPath":
        return PurePosixPath(self._str, *segments)

    @property
    def parent(self) -> "PurePosixPath":
        return PurePosixPath(self._parent_str())

    @property
    def parents(self) -> list["PurePosixPath"]:
        return [PurePosixPath(x) for x in self._parent_strs()]

    def with_name(self, name: str) -> "PurePosixPath":
        return PurePosixPath(self._with_name(name))

    def with_stem(self, stem: str) -> "PurePosixPath":
        return PurePosixPath(self._with_stem(stem))

    def with_suffix(self, suffix: str) -> "PurePosixPath":
        return PurePosixPath(self._with_suffix(suffix))

    def relative_to(self, other: str, *, walk_up: bool = False) -> "PurePosixPath":
        return PurePosixPath(self._relative_str(_fs(other), walk_up))


class Path(PurePath):
    """A path of the file system: what is there, reading, writing, listing ..."""

    def __repr__(self) -> str:
        return "PosixPath(" + repr(self._str) + ")"

    def with_segments(self, *args: str) -> "Path":
        return Path(*args)

    def __truediv__(self, key: str) -> "Path":
        return Path(self._str, key)

    def __rtruediv__(self, key: str) -> "Path":
        return Path(key, self._str)

    def joinpath(self, *segments: str) -> "Path":
        return Path(self._str, *segments)

    @property
    def parent(self) -> "Path":
        return Path(self._parent_str())

    @property
    def parents(self) -> list["Path"]:
        return [Path(x) for x in self._parent_strs()]

    def with_name(self, name: str) -> "Path":
        return Path(self._with_name(name))

    def with_stem(self, stem: str) -> "Path":
        return Path(self._with_stem(stem))

    def with_suffix(self, suffix: str) -> "Path":
        return Path(self._with_suffix(suffix))

    def relative_to(self, other: str, *, walk_up: bool = False) -> "Path":
        return Path(self._relative_str(_fs(other), walk_up))

    # ---- the file system
    @staticmethod
    def cwd() -> "Path":
        return Path(os.getcwd())

    @staticmethod
    def home() -> "Path":
        return Path(os.path.expanduser("~"))

    def absolute(self) -> "Path":
        if self._root:
            return self
        return Path(os.getcwd(), self._str)

    def resolve(self, strict: bool = False) -> "Path":
        if strict:
            os.stat(self._str)
        return Path(os.path.realpath(self._str))

    def expanduser(self) -> "Path":
        return Path(os.path.expanduser(self._str))

    def readlink(self) -> "Path":
        return Path(os.readlink(self._str))

    def stat(self, *, follow_symlinks: bool = True) -> os.stat_result:
        return os.stat(self._str) if follow_symlinks else os.lstat(self._str)

    def lstat(self) -> os.stat_result:
        return os.lstat(self._str)

    def exists(self, *, follow_symlinks: bool = True) -> bool:
        return os.path.exists(self._str) if follow_symlinks else os.path.lexists(self._str)

    def is_dir(self, *, follow_symlinks: bool = True) -> bool:
        if follow_symlinks:
            return os.path.isdir(self._str)
        return os.path.isdir(self._str) and not os.path.islink(self._str)

    def is_file(self, *, follow_symlinks: bool = True) -> bool:
        if follow_symlinks:
            return os.path.isfile(self._str)
        return os.path.isfile(self._str) and not os.path.islink(self._str)

    def is_symlink(self) -> bool:
        return os.path.islink(self._str)

    def is_mount(self) -> bool:
        return os.path.ismount(self._str)

    def samefile(self, other_path: str) -> bool:
        return os.path.samefile(self._str, _fs(other_path))

    def iterdir(self):
        """The paths in this folder."""
        for name in os.listdir(self._str):
            yield Path(self._str, name)

    def glob(self, pattern: str, *, case_sensitive: bool | None = None, recurse_symlinks: bool = False):
        """The paths in this folder matching pattern (** for any folders in between)."""
        import glob as _glob
        if pattern.startswith("/"):
            raise NotImplementedError("Non-relative patterns are unsupported")
        for p in _glob.iglob(pattern, root_dir=self._str, recursive=True, include_hidden=True):
            yield Path(self._str, p)

    def rglob(self, pattern: str, *, case_sensitive: bool | None = None, recurse_symlinks: bool = False):
        """glob('**/' + pattern)."""
        for p in self.glob("**/" + pattern):
            yield p

    def walk(self, top_down: bool = True, on_error: Callable[[OSError], None] | None = None, follow_symlinks: bool = False):
        """(folder, its folders' names, its files' names) for this folder and those under it."""
        for d, dirs, files in os.walk(self._str, top_down, on_error, follow_symlinks):
            yield (Path(d), dirs, files)

    def chmod(self, mode: int, *, follow_symlinks: bool = True) -> None:
        os.chmod(self._str, mode)

    def mkdir(self, mode: int = 0o777, parents: bool = False, exist_ok: bool = False) -> None:
        """Make this folder (parents: the missing ones above too)."""
        try:
            os.mkdir(self._str, mode)
        except FileNotFoundError:
            if not parents or self.parent._str == self._str:
                raise
            self.parent.mkdir(mode, True, True)
            self.mkdir(mode, False, exist_ok)
        except OSError:
            if not exist_ok or not self.is_dir():
                raise

    def rmdir(self) -> None:
        os.rmdir(self._str)

    def unlink(self, missing_ok: bool = False) -> None:
        try:
            os.unlink(self._str)
        except FileNotFoundError:
            if not missing_ok:
                raise

    def rename(self, target: str) -> "Path":
        t = _fs(target)
        os.rename(self._str, t)
        return Path(t)

    def replace(self, target: str) -> "Path":
        t = _fs(target)
        os.replace(self._str, t)
        return Path(t)

    def touch(self, mode: int = 0o666, exist_ok: bool = True) -> None:
        if exist_ok:
            try:
                os.utime(self._str)
                return
            except OSError:
                pass
        fd = os.open(self._str, os.O_CREAT | os.O_WRONLY | (0 if exist_ok else os.O_EXCL), mode)
        os.close(fd)

    def symlink_to(self, target: str, target_is_directory: bool = False) -> None:
        os.symlink(_fs(target), self._str)

    def hardlink_to(self, target: str) -> None:
        os.link(_fs(target), self._str)

    def read_bytes(self) -> bytes:
        f = io._open_binary(self._str, "rb")
        try:
            return f.read()
        finally:
            f.close()

    def write_bytes(self, data: bytes) -> int:
        f = io._open_binary(self._str, "wb")
        try:
            return f.write(data)
        finally:
            f.close()

    def read_text(self, encoding: str | None = None, errors: str | None = None, newline: str | None = None) -> str:
        f = io._open_text(self._str, "r", -1, encoding, errors, newline)
        try:
            return f.read()
        finally:
            f.close()

    def write_text(self, data: str, encoding: str | None = None, errors: str | None = None, newline: str | None = None) -> int:
        f = io._open_text(self._str, "w", -1, encoding, errors, newline)
        try:
            return f.write(data)
        finally:
            f.close()

    if sys._compiled:
        # (a binary and a text file are of different types: the compiler calls the one the mode says)
        def _open_b(self, mode: str = "rb", buffering: int = -1, encoding: str | None = None, errors: str | None = None,
                    newline: str | None = None) -> io.BufferedReader:
            return io._open_binary(self._str, mode, buffering, encoding, errors, newline)

        def _open_t(self, mode: str = "r", buffering: int = -1, encoding: str | None = None, errors: str | None = None,
                    newline: str | None = None) -> io.TextIOWrapper:
            return io._open_text(self._str, mode, buffering, encoding, errors, newline)
    else:
        def open(self, mode="r", buffering=-1, encoding=None, errors=None, newline=None):
            """open() of this file."""
            if "b" in mode:
                return io._open_binary(self._str, mode, buffering, encoding, errors, newline)
            return io._open_text(self._str, mode, buffering, encoding, errors, newline)

    def copy(self, target: str, *, follow_symlinks: bool = True, preserve_metadata: bool = False) -> "Path":
        """Copy this file or folder to target; target's path."""
        import shutil
        t = _fs(target)
        if self.is_dir():
            shutil.copytree(self._str, t, symlinks=not follow_symlinks)
        elif preserve_metadata:
            shutil.copy2(self._str, t)
        else:
            shutil.copyfile(self._str, t)
        return Path(t)

    def copy_into(self, target_dir: str, *, follow_symlinks: bool = True, preserve_metadata: bool = False) -> "Path":
        t = _fs(target_dir)
        return self.copy(os.path.join(t, self.name), follow_symlinks=follow_symlinks, preserve_metadata=preserve_metadata)

    def move(self, target: str) -> "Path":
        """Move this file or folder to target; target's path."""
        import shutil
        t = _fs(target)
        shutil.move(self._str, t)
        return Path(t)

    def move_into(self, target_dir: str) -> "Path":
        t = _fs(target_dir)
        return self.move(os.path.join(t, self.name))

    def as_uri(self) -> str:
        if not self.is_absolute():
            raise ValueError("relative paths can't be expressed as file URIs")
        return "file://" + _quote_uri(self._str)


class PosixPath(Path):
    """A POSIX path of the file system."""
