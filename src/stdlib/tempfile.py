"""Temporary files and folders (CPython's tempfile): mkstemp, mkdtemp, gettempdir,
TemporaryDirectory, NamedTemporaryFile, TemporaryFile.

The folder is TMPDIR, TEMP or TMP, else the first usable of /tmp, /var/tmp,
/usr/tmp (KolibriOS: /tmp0/1, the RAM disk /rd/1), else the current one. The
files of NamedTemporaryFile and TemporaryFile go when they are closed."""
import os
import sys
import io
import shutil

TMP_MAX = 10000
template = "tmp"
tempdir: str | None = None

_CHARS = "abcdefghijklmnopqrstuvwxyz0123456789_"


def _rand_name() -> str:
    out: list[str] = []
    for b in os.urandom(8):
        out.append(_CHARS[b % len(_CHARS)])
    return "".join(out)


def _candidates() -> list[str]:
    out: list[str] = []
    for env in ("TMPDIR", "TEMP", "TMP"):
        d = os.environ.get(env)
        if d:
            out.append(d)
    if sys.platform == "kolibrios":
        out.extend(["/tmp0/1", "/rd/1"])
    else:
        out.extend(["/tmp", "/var/tmp", "/usr/tmp"])
    try:
        out.append(os.getcwd())
    except OSError:
        out.append(os.curdir)
    return out


def _usable(d: str) -> bool:
    """d is a folder a file can be made in (one is made and removed)."""
    for k in range(100):
        name = os.path.join(d, _rand_name())
        try:
            fd = os.open(name, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600)
        except FileExistsError:
            continue
        except OSError:
            return False
        try:
            os.write(fd, b"blat")
        finally:
            os.close(fd)
            os.unlink(name)
        return True
    return False


def gettempdir() -> str:
    """The folder temporary files go to (found once)."""
    global tempdir
    if tempdir is None:
        for d in _candidates():
            if d != os.curdir:
                d = os.path.abspath(d)
            if _usable(d):
                tempdir = d
                return d
        raise FileNotFoundError(2, "No usable temporary directory found in " + repr(_candidates()))
    return tempdir


def gettempprefix() -> str:
    return template


def _parts(suffix: str | None, prefix: str | None, dir: str | None) -> tuple[str, str, str]:
    return ("" if suffix is None else suffix, template if prefix is None else prefix, gettempdir() if dir is None else dir)


def mkstemp(suffix: str | None = None, prefix: str | None = None, dir: str | None = None, text: bool = False) -> tuple[int, str]:
    """(descriptor, absolute path) of a new file only this user can read and write."""
    s, p, d = _parts(suffix, prefix, dir)
    for k in range(TMP_MAX):
        name = os.path.join(d, p + _rand_name() + s)
        try:
            fd = os.open(name, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600)
        except FileExistsError:
            continue
        return (fd, os.path.abspath(name))
    raise FileExistsError(17, "No usable temporary file name found")


def mkdtemp(suffix: str | None = None, prefix: str | None = None, dir: str | None = None) -> str:
    """The absolute path of a new folder only this user can use."""
    s, p, d = _parts(suffix, prefix, dir)
    for k in range(TMP_MAX):
        name = os.path.join(d, p + _rand_name() + s)
        try:
            os.mkdir(name, 0o700)
        except FileExistsError:
            continue
        return os.path.abspath(name)
    raise FileExistsError(17, "No usable temporary directory name found")


def mktemp(suffix: str = "", prefix: str = "tmp", dir: str | None = None) -> str:
    """A path no file has now (unsafe: another may take it; mkstemp makes the file)."""
    d = gettempdir() if dir is None else dir
    for k in range(TMP_MAX):
        name = os.path.join(d, prefix + _rand_name() + suffix)
        if not os.path.exists(name):
            return name
    raise FileExistsError(17, "No usable temporary filename found")


class TemporaryDirectory:
    """A new folder (mkdtemp) removed with its contents by cleanup() or at the end of a with block."""

    def __init__(self, suffix: str | None = None, prefix: str | None = None, dir: str | None = None,
                 ignore_cleanup_errors: bool = False, *, delete: bool = True):
        self.name = mkdtemp(suffix, prefix, dir)
        self._ignore_cleanup_errors = ignore_cleanup_errors
        self._delete = delete

    def __repr__(self) -> str:
        return "<TemporaryDirectory " + repr(self.name) + ">"

    def __enter__(self) -> str:
        return self.name

    def __exit__(self, exc_type, exc, tb) -> None:
        if self._delete:
            self.cleanup()

    def cleanup(self) -> None:
        if os.path.exists(self.name):
            shutil.rmtree(self.name, ignore_errors=self._ignore_cleanup_errors)


def _new_file(suffix: str | None, prefix: str | None, dir: str | None) -> str:
    fd, name = mkstemp(suffix, prefix, dir)
    os.close(fd)
    return name


if sys._compiled:
    # (a binary and a text file are of different types: the compiler calls the one the mode says)
    def _NamedTemporaryFileB(mode: str = "w+b", buffering: int = -1, encoding: str | None = None, newline: str | None = None,
                             suffix: str | None = None, prefix: str | None = None, dir: str | None = None, delete: bool = True,
                             *, errors: str | None = None, delete_on_close: bool = True) -> io.BufferedReader:
        name = _new_file(suffix, prefix, dir)
        f = io._open_binary(name, mode, buffering, encoding, errors, newline)
        f._delete = delete
        return f

    def _NamedTemporaryFileT(mode: str = "w+", buffering: int = -1, encoding: str | None = None, newline: str | None = None,
                             suffix: str | None = None, prefix: str | None = None, dir: str | None = None, delete: bool = True,
                             *, errors: str | None = None, delete_on_close: bool = True) -> io.TextIOWrapper:
        name = _new_file(suffix, prefix, dir)
        f = io._open_text(name, mode, buffering, encoding, errors, newline)
        f._delete = delete
        return f

    def _TemporaryFileB(mode: str = "w+b", buffering: int = -1, encoding: str | None = None, newline: str | None = None,
                        suffix: str | None = None, prefix: str | None = None, dir: str | None = None,
                        *, errors: str | None = None) -> io.BufferedReader:
        return _NamedTemporaryFileB(mode, buffering, encoding, newline, suffix, prefix, dir, True, errors=errors)

    def _TemporaryFileT(mode: str = "w+", buffering: int = -1, encoding: str | None = None, newline: str | None = None,
                        suffix: str | None = None, prefix: str | None = None, dir: str | None = None,
                        *, errors: str | None = None) -> io.TextIOWrapper:
        return _NamedTemporaryFileT(mode, buffering, encoding, newline, suffix, prefix, dir, True, errors=errors)

    NamedTemporaryFile = _NamedTemporaryFileB
    TemporaryFile = _TemporaryFileB
else:
    def NamedTemporaryFile(mode="w+b", buffering=-1, encoding=None, newline=None, suffix=None, prefix=None, dir=None,
                           delete=True, *, errors=None, delete_on_close=True):
        """A new file (mkstemp) opened in mode: its name is .name; it goes when closed (delete)."""
        name = _new_file(suffix, prefix, dir)
        if "b" in mode:
            f = io._open_binary(name, mode, buffering, encoding, errors, newline)
        else:
            f = io._open_text(name, mode, buffering, encoding, errors, newline)
        f._delete = delete
        return f

    def TemporaryFile(mode="w+b", buffering=-1, encoding=None, newline=None, suffix=None, prefix=None, dir=None, *,
                      errors=None):
        """A new file opened in mode, gone when closed."""
        return NamedTemporaryFile(mode, buffering, encoding, newline, suffix, prefix, dir, True, errors=errors)
