"""High-level file operations (CPython's shutil): copying files and trees, removing
trees, moving, which. The contents are copied through the descriptors (os.read /
os.write); on KolibriOS too."""
import os
import sys
import stat
import fnmatch

COPY_BUFSIZE = 256 * 1024


class Error(OSError):
    pass


class SameFileError(Error):
    """Raised when source and destination are the same file."""


class SpecialFileError(OSError):
    """Raised when trying to do a kind of operation (e.g. copying) which is not supported on a special file."""


class ExecError(OSError):
    """Raised when a command could not be executed."""


class ReadError(OSError):
    """Raised when an archive cannot be read."""


class _TreeError(Error):
    """copytree's errors ([(src, dst, why), ...]) as a compiled program has them: e.errors."""

    def __init__(self, errors: list[tuple[str, str, str]]):
        super().__init__(str(errors))
        self.errors = errors


def _tree_error(errors: list[tuple[str, str, str]]) -> Error:
    if sys._compiled:
        return _TreeError(errors)
    return Error(errors)


def _tree_errors(e: Error) -> list[tuple[str, str, str]]:
    if sys._compiled:
        if isinstance(e, _TreeError):
            return e.errors
        return [("", "", str(e))]
    return e.args[0]


def copyfileobj(fsrc, fdst, length: int = 0) -> None:
    """Copy what file object fsrc reads to file object fdst."""
    if length <= 0:
        length = COPY_BUFSIZE
    while True:
        buf = fsrc.read(length)
        if not buf:
            break
        fdst.write(buf)


def _samefile(src: str, dst: str) -> bool:
    try:
        return os.path.samefile(src, dst)
    except OSError:
        return os.path.normcase(os.path.abspath(src)) == os.path.normcase(os.path.abspath(dst))


def _copy_fd(src: str, dst: str) -> None:
    fi = os.open(src, os.O_RDONLY)
    try:
        st = os.fstat(fi)
        if stat.S_ISDIR(st.st_mode):
            raise IsADirectoryError(21, os.strerror(21), src)
        fo = os.open(dst, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o666)
        try:
            while True:
                buf = os.read(fi, COPY_BUFSIZE)
                if not buf:
                    break
                n = 0
                while n < len(buf):
                    n += os.write(fo, buf[n:])
        finally:
            os.close(fo)
    finally:
        os.close(fi)


def copyfile(src: str, dst: str, *, follow_symlinks: bool = True) -> str:
    """Copy the contents of file src to file dst (made or replaced); dst."""
    if _samefile(src, dst):
        raise SameFileError(repr(src) + " and " + repr(dst) + " are the same file")
    if not follow_symlinks and os.path.islink(src):
        os.symlink(os.readlink(src), dst)
        return dst
    if os.path.isdir(dst):
        raise IsADirectoryError(21, os.strerror(21), dst)
    _copy_fd(src, dst)
    return dst


def copymode(src: str, dst: str, *, follow_symlinks: bool = True) -> None:
    """Copy the permission bits of src to dst."""
    st = os.stat(src) if follow_symlinks else os.lstat(src)
    os.chmod(dst, stat.S_IMODE(st.st_mode))


def copystat(src: str, dst: str, *, follow_symlinks: bool = True) -> None:
    """Copy the permission bits and the access and modification times of src to dst."""
    st = os.stat(src) if follow_symlinks else os.lstat(src)
    os.utime(dst, ns=(st.st_atime_ns, st.st_mtime_ns))
    os.chmod(dst, stat.S_IMODE(st.st_mode))


def copy(src: str, dst: str, *, follow_symlinks: bool = True) -> str:
    """Copy file src to dst (a folder: into it, the same name) with its permission bits; the new file's path."""
    if os.path.isdir(dst):
        dst = os.path.join(dst, os.path.basename(src))
    copyfile(src, dst, follow_symlinks=follow_symlinks)
    copymode(src, dst, follow_symlinks=follow_symlinks)
    return dst


def copy2(src: str, dst: str, *, follow_symlinks: bool = True) -> str:
    """copy() keeping the times too."""
    if os.path.isdir(dst):
        dst = os.path.join(dst, os.path.basename(src))
    copyfile(src, dst, follow_symlinks=follow_symlinks)
    copystat(src, dst, follow_symlinks=follow_symlinks)
    return dst


if sys._compiled:
    # (onexc gets functions of one type in a compiled program: these, named as os's)
    def lstat(path: str) -> None:
        os.lstat(path)

    def scandir(path: str) -> None:
        os.listdir(path)

    def unlink(path: str) -> None:
        os.unlink(path)

    def rmdir(path: str) -> None:
        os.rmdir(path)

    def islink(path: str) -> None:
        os.path.islink(path)

    def _copy2(src: str, dst: str) -> str:
        return copy2(src, dst)

    _F_LSTAT = lstat
    _F_SCANDIR = scandir
    _F_UNLINK = unlink
    _F_RMDIR = rmdir
    _F_ISLINK = islink
    _COPY2 = _copy2
else:
    _F_LSTAT = os.lstat
    _F_SCANDIR = os.scandir
    _F_UNLINK = os.unlink
    _F_RMDIR = os.rmdir
    _F_ISLINK = os.path.islink
    _COPY2 = copy2


def ignore_patterns(*patterns):
    """A function for copytree(ignore=...) leaving out the names matching any of patterns."""
    def _ignore(path: str, names: list[str]) -> set[str]:
        ignored: set[str] = set()
        for pattern in patterns:
            for name in fnmatch.filter(names, pattern):
                ignored.add(name)
        return ignored
    return _ignore


def _no_ignore(path: str, names: list[str]) -> set[str]:
    return set()


def copytree(src: str, dst: str, symlinks: bool = False, ignore=None, copy_function=_COPY2,
             ignore_dangling_symlinks: bool = False, dirs_exist_ok: bool = False) -> str:
    """Copy the folder tree src to dst (made, unless dirs_exist_ok); dst."""
    names = sorted(os.listdir(src))
    if ignore is None:
        ignored = _no_ignore(src, names)
    else:
        ignored = ignore(src, names)
    os.makedirs(dst, exist_ok=dirs_exist_ok)
    errors: list[tuple[str, str, str]] = []
    for name in names:
        if name in ignored:
            continue
        s = os.path.join(src, name)
        d = os.path.join(dst, name)
        try:
            if os.path.islink(s) and symlinks:
                os.symlink(os.readlink(s), d)
                copystat(s, d, follow_symlinks=False)
            elif os.path.isdir(s):
                copytree(s, d, symlinks, ignore, copy_function, ignore_dangling_symlinks, dirs_exist_ok)
            elif os.path.islink(s) and not os.path.exists(s):
                if not ignore_dangling_symlinks:
                    raise FileNotFoundError(2, os.strerror(2), s)
            else:
                copy_function(s, d)
        except Error as e:
            errors.extend(_tree_errors(e))
        except OSError as e:
            errors.append((s, d, str(e)))
    try:
        copystat(src, dst)
    except OSError as e:
        errors.append((src, dst, str(e)))
    if errors:
        raise _tree_error(errors)
    return dst


def _report(onexc, func, path: str, e: OSError) -> None:
    if onexc is None:
        raise e
    else:
        onexc(func, path, e)


def _rmtree(path: str, top: bool, onexc) -> None:
    """As CPython's: the files go while the folder is read, then the folders in it (the last first), then it."""
    try:
        os.lstat(path)
    except FileNotFoundError as e:
        if top:
            _report(onexc, _F_LSTAT, path, e)
        return
    except OSError as e:
        _report(onexc, _F_LSTAT, path, e)
        return
    subdirs: list[str] = []
    try:
        names = os.listdir(path)
        for name in names:
            full = os.path.join(path, name)
            if os.path.isdir(full) and not os.path.islink(full):
                subdirs.append(full)
                continue
            try:
                os.unlink(full)
            except FileNotFoundError:
                continue
            except OSError as e:
                _report(onexc, _F_UNLINK, full, e)
    except OSError as e:
        _report(onexc, _F_SCANDIR, path, e)
    for k in range(len(subdirs) - 1, -1, -1):
        _rmtree(subdirs[k], False, onexc)
    try:
        os.rmdir(path)
    except FileNotFoundError as e:
        if top:
            _report(onexc, _F_RMDIR, path, e)
    except OSError as e:
        _report(onexc, _F_RMDIR, path, e)


def _ignore_all(func, path: str, e: OSError) -> None:
    pass


def rmtree(path: str, ignore_errors: bool = False, onexc=None) -> None:
    """Remove folder path with everything in it (onexc(function, path, exception) for the errors)."""
    if ignore_errors:
        _rmtree_top(path, _ignore_all)
    else:
        _rmtree_top(path, onexc)


def _rmtree_top(path: str, onexc) -> None:
    if os.path.islink(path):
        _report(onexc, _F_ISLINK, path, OSError("Cannot call rmtree on a symbolic link"))
        return
    _rmtree(path, True, onexc)


def _destinsrc(src: str, dst: str) -> bool:
    src = os.path.abspath(src)
    dst = os.path.abspath(dst)
    if not src.endswith(os.path.sep):
        src += os.path.sep
    if not dst.endswith(os.path.sep):
        dst += os.path.sep
    return dst.startswith(src)


def move(src: str, dst: str, copy_function=_COPY2) -> str:
    """Move file or folder src to dst (an existing folder: into it); the new path."""
    real_dst = dst
    if os.path.isdir(dst):
        if _samefile(src, dst) and not os.path.islink(src):
            os.rename(src, dst)
            return dst
        real_dst = os.path.join(dst, os.path.basename(src.rstrip(os.path.sep)))
        if os.path.exists(real_dst):
            raise Error("Destination path '" + real_dst + "' already exists")
    try:
        os.rename(src, real_dst)
    except OSError:
        if os.path.islink(src):
            os.symlink(os.readlink(src), real_dst)
            os.unlink(src)
        elif os.path.isdir(src):
            if _destinsrc(src, dst):
                raise Error("Cannot move a directory '" + src + "' into itself '" + dst + "'.")
            copytree(src, real_dst, symlinks=True)
            rmtree(src)
        else:
            copy_function(src, real_dst)
            os.unlink(src)
    return real_dst


def which(cmd: str, mode: int = os.F_OK | os.X_OK, path: str | None = None) -> str | None:
    """The path of program cmd (searched in the folders of PATH), or None."""
    def _ok(fn: str) -> bool:
        return os.path.exists(fn) and os.access(fn, mode) and not os.path.isdir(fn)
    if os.path.dirname(cmd):
        if _ok(cmd):
            return cmd
        return None
    if path is None:
        p = os.environ.get("PATH")
        if p is None:
            p = os.defpath
    else:
        p = path
    if not p:
        return None
    seen: set[str] = set()
    for d in p.split(os.pathsep):
        nd = os.path.normcase(d)
        if nd not in seen:
            seen.add(nd)
            name = os.path.join(d, cmd)
            if _ok(name):
                return name
    return None


def get_terminal_size(fallback: tuple[int, int] = (80, 24)) -> os.terminal_size:
    """The size of the terminal: COLUMNS and LINES, else what stdout's terminal says, else fallback."""
    columns = 0
    lines = 0
    c = os.environ.get("COLUMNS")
    if c is not None and c.isdigit():
        columns = int(c)
    li = os.environ.get("LINES")
    if li is not None and li.isdigit():
        lines = int(li)
    if columns <= 0 or lines <= 0:
        try:
            size = os.get_terminal_size(1)
            if columns <= 0:
                columns = size.columns
            if lines <= 0:
                lines = size.lines
        except OSError:
            pass
    if columns <= 0:
        columns = fallback[0]
    if lines <= 0:
        lines = fallback[1]
    return os.terminal_size((columns, lines))
