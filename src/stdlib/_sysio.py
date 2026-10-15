"""sys.stdout, sys.stderr and sys.stdin in compiled programs (the compiler reads
sys.stdout as __mpy_stdout, ...; the interpreter has its own). Writes go through
print(), so they stay in order with it; reads through input()."""
import sys
import _os


class _StdStream:
    """A standard stream as a text file object."""

    def __init__(self, fd: int, name: str, mode: str) -> None:
        self.name = name
        self.mode = mode
        self.encoding = "utf-8"
        self.errors = "strict"
        self.line_buffering = fd != 0
        self._fd = fd
        self._eof = False
        self._closed = False

    @property
    def closed(self) -> bool:
        return self._closed

    def write(self, s: str) -> int:
        if self._fd == 0:
            raise OSError("not writable")
        if self._fd == 2:
            print(s, end="", file=sys.stderr)
        else:
            print(s, end="")
        return len(s)

    def writelines(self, lines: list[str]) -> None:
        for line in lines:
            self.write(line)

    def flush(self) -> None:
        if self._fd == 1:
            print(end="", flush=True)

    def readline(self, size: int = -1) -> str:
        if self._fd != 0:
            raise OSError("not readable")
        if self._eof:
            return ""
        try:
            return input() + "\n"
        except EOFError:
            self._eof = True
            return ""

    def read(self, size: int = -1) -> str:
        out = ""
        while size < 0 or len(out) < size:
            line = self.readline()
            if not line:
                break
            out += line
        if size >= 0 and len(out) > size:
            return out[:size]                   # (the rest of that line is lost)
        return out

    def readlines(self) -> list[str]:
        out: list[str] = []
        while True:
            line = self.readline()
            if not line:
                return out
            out.append(line)

    def __iter__(self) -> "_StdStream":
        return self

    def __next__(self) -> str:
        line = self.readline()
        if not line:
            raise StopIteration
        return line

    def fileno(self) -> int:
        return self._fd

    def isatty(self) -> bool:
        return _os.isatty(self._fd)

    def readable(self) -> bool:
        return self._fd == 0

    def writable(self) -> bool:
        return self._fd != 0

    def seekable(self) -> bool:
        return False

    def close(self) -> None:
        self._closed = True

    def __enter__(self) -> "_StdStream":
        return self

    def __exit__(self, t, v, tb) -> None:
        pass

    def __repr__(self) -> str:
        return "<_io.TextIOWrapper name='" + self.name + "' mode='" + self.mode + "' encoding='utf-8'>"


__mpy_stdout = _StdStream(1, "<stdout>", "w")
__mpy_stderr = _StdStream(2, "<stderr>", "w")
__mpy_stdin = _StdStream(0, "<stdin>", "r")
