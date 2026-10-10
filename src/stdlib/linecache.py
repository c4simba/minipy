"""Lines of source files (CPython's linecache): getline(filename, lineno), getlines(filename)."""
import sys

if not sys._compiled:
    __all__ = ["getline", "clearcache", "checkcache", "lazycache"]

    cache = {}


    def getline(filename, lineno, module_globals=None):
        """Line lineno of filename (with its newline), or '' when there is none."""
        lines = getlines(filename, module_globals)
        if 1 <= lineno <= len(lines):
            return lines[lineno - 1]
        return ""


    def getlines(filename, module_globals=None):
        """The lines of filename (a file the interpreter ran, or one on disk)."""
        if filename in cache:
            return cache[filename]
        return updatecache(filename, module_globals)


    def updatecache(filename, module_globals=None):
        cache.pop(filename, None)
        if not filename or (filename.startswith("<") and filename.endswith(">") and not filename.startswith("<minipy:")):
            return []
        parts = sys._getline(filename, 0).split("\n")              # (the files the interpreter ran: it keeps their text)
        if parts[-1] == "":
            parts.pop()
        lines = [p + "\n" for p in parts]
        if not lines:
            try:
                with open(filename, encoding="utf-8") as f:
                    lines = f.readlines()
            except (OSError, UnicodeDecodeError, SyntaxError):
                return []
        if lines and not lines[-1].endswith("\n"):
            lines[-1] += "\n"
        cache[filename] = lines
        return lines


    def clearcache():
        """Forgets the lines read."""
        cache.clear()


    def checkcache(filename=None):
        """(The lines read are kept as they are.)"""
        pass


    def lazycache(filename, module_globals):
        return False


if sys._compiled:
    # Compiled programs: the lines of files on disk (UTF-8).
    from typing import TypeVar

    _G = TypeVar("_G")

    cache: dict[str, list[str]] = {}

    def getline(filename: str, lineno: int, module_globals: _G = None) -> str:
        """Line lineno of filename (with its newline), or '' when there is none."""
        lines = getlines(filename)
        if 1 <= lineno <= len(lines):
            return lines[lineno - 1]
        return ""

    def getlines(filename: str, module_globals: _G = None) -> list[str]:
        """The lines of filename."""
        if filename in cache:
            return cache[filename]
        return updatecache(filename)

    def updatecache(filename: str, module_globals: _G = None) -> list[str]:
        if filename in cache:
            del cache[filename]
        if not filename or (filename.startswith("<") and filename.endswith(">")):
            return []
        lines: list[str] = []
        try:
            with open(filename, encoding="utf-8") as f:
                lines = f.readlines()
        except (OSError, UnicodeDecodeError):
            return []
        if lines and not lines[-1].endswith("\n"):
            lines[-1] += "\n"
        cache[filename] = lines
        return lines

    def clearcache() -> None:
        """Forgets the lines read."""
        cache.clear()

    def checkcache(filename: str | None = None) -> None:
        """(The lines read are kept as they are.)"""

    def lazycache(filename: str, module_globals: _G = None) -> bool:
        return False
