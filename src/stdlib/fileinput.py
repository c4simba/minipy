"""Helper class to quickly write a loop over all standard input files (CPython's fileinput).

    import fileinput
    for line in fileinput.input():
        process(line)

The files are those named on the command line (sys.argv[1:]), or standard input ("-", or no names).
input(), filename(), lineno(), filelineno(), fileno(), isfirstline(), isstdin(), nextfile(), close();
the FileInput class. No in-place editing (inplace=True) here."""
import sys
from typing import TypeVar

_F = TypeVar("_F")
_H = TypeVar("_H")

__all__ = ["input", "close", "nextfile", "filename", "lineno", "filelineno", "fileno", "isfirstline", "isstdin",
           "FileInput", "hook_compressed", "hook_encoded"]


class FileInput:
    """FileInput([files[, inplace[, backup]]], *, mode="r", openhook=None, encoding=None, errors=None):
    the lines of the files, one after another."""

    def __init__(self, files: _F = None, inplace: bool = False, backup: str = "", *,
                 mode: str = "r", openhook: _H = None, encoding: str | None = None,
                 errors: str | None = None) -> None:
        names: list[str] = []
        if isinstance(files, str):
            names = [files]
        elif files is None:
            names = list(sys.argv[1:])
        else:
            names = list(files)
        if not names:
            names = ['-']
        self._files = names
        if inplace:
            raise ValueError("FileInput cannot use in-place editing here")
        if mode not in ('r', 'rb'):
            raise ValueError("FileInput opening mode must be 'r' or 'rb'")
        self._mode = mode
        self._encoding = encoding
        self._errors = errors
        self._index = 0
        self._filename: str | None = None
        self._lineno = 0
        self._filelineno = 0
        self._lines: list[str] = []
        self._pos = 0
        self._isstdin = False
        self._open = False
        self._startlineno = 0

    def __del__(self) -> None:
        pass

    def close(self) -> None:
        self._index = len(self._files)
        self.nextfile()

    def __enter__(self) -> "FileInput":
        return self

    def __exit__(self, type, value, traceback) -> None:
        self.close()

    def __iter__(self):
        while True:
            line = self._readline()
            if not line:
                return
            yield line

    def __next__(self) -> str:
        line = self._readline()
        if not line:
            raise StopIteration
        return line

    def nextfile(self) -> None:
        """Close the current file so that the next iteration will read the first line from the next file."""
        self._lines = []
        self._pos = 0
        self._open = False
        self._isstdin = False
        self._filelineno = 0

    def readline(self) -> str:
        return self._readline()

    def _start_next(self) -> bool:
        if self._index >= len(self._files):
            return False
        name = self._files[self._index]
        self._index += 1
        self._filename = name
        self._filelineno = 0
        self._pos = 0
        if name == '-':
            self._filename = '<stdin>'
            self._isstdin = True
            self._lines = sys.stdin.readlines()
        else:
            self._isstdin = False
            with open(name, "r", encoding=self._encoding, errors=self._errors) as f:
                self._lines = f.readlines()
        self._open = True
        return True

    def _readline(self) -> str:
        while True:
            if self._open and self._pos < len(self._lines):
                line = self._lines[self._pos]
                self._pos += 1
                self._lineno += 1
                self._filelineno += 1
                return line
            if not self._start_next():
                return ""

    def filename(self) -> str | None:
        return self._filename

    def lineno(self) -> int:
        return self._lineno

    def filelineno(self) -> int:
        return self._filelineno

    def fileno(self) -> int:
        return 0 if self._open and self._isstdin else -1

    def isfirstline(self) -> bool:
        return self._filelineno == 1

    def isstdin(self) -> bool:
        return self._isstdin


_state: list[FileInput] = []


def input(files: _F = None, inplace: bool = False, backup: str = "", *, mode: str = "r",
          openhook: _H = None, encoding: str | None = None, errors: str | None = None) -> FileInput:
    """Return an instance of the FileInput class (the one the module's functions ask)."""
    if _state and _state[0]._open:
        raise RuntimeError("input() already active")
    f = FileInput(files, inplace, backup, mode=mode, openhook=openhook, encoding=encoding, errors=errors)
    _state.clear()
    _state.append(f)
    return f


def _current() -> FileInput:
    if not _state:
        raise RuntimeError("no active input()")
    return _state[0]


def close() -> None:
    """Close the sequence."""
    if _state:
        _state[0].close()
        _state.clear()


def nextfile() -> None:
    """Close the current file so that the next iteration will read the first line from the next file."""
    _current().nextfile()


def filename() -> str | None:
    """Return the name of the file currently being read (None before the first line)."""
    return _current().filename()


def lineno() -> int:
    """Return the cumulative line number of the line that has just been read."""
    return _current().lineno()


def filelineno() -> int:
    """Return the line number in the current file."""
    return _current().filelineno()


def fileno() -> int:
    """Return the file number of the current file (-1: none open)."""
    return _current().fileno()


def isfirstline() -> bool:
    """Returns true the line just read is the first line of its file."""
    return _current().isfirstline()


def isstdin() -> bool:
    """Returns true if the last line was read from sys.stdin."""
    return _current().isstdin()


def hook_compressed(filename: str, mode: str, *, encoding: str | None = None, errors: str | None = None) -> None:
    raise NotImplementedError("hook_compressed is not available here")


def hook_encoded(encoding: str, errors: str | None = None) -> None:
    raise NotImplementedError("hook_encoded is not available here (FileInput(encoding=...) is)")
