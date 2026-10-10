"""Extract, format and print exceptions and stacks (CPython's traceback).

An exception's __traceback__ here is the list of its frames' (file, line, function), innermost
first; whole tracebacks are the interpreter's own text (what an uncaught exception prints)."""
import sys

if not sys._compiled:
    import sys
    import linecache

    __all__ = ["extract_stack", "extract_tb", "format_exception", "format_exception_only", "format_list",
               "format_stack", "format_tb", "print_exc", "format_exc", "print_exception", "print_last",
               "print_stack", "print_tb", "clear_frames", "FrameSummary", "StackSummary", "TracebackException",
               "walk_stack", "walk_tb", "print_list"]

    _RECURSIVE_CUTOFF = 3


    class _Sentinel:
        def __repr__(self):
            return "<implicit>"


    _sentinel = _Sentinel()


    def _frames(tb):
        """(file, line, name[, code, instruction]) of a traceback, outermost first."""
        if tb is None:
            return []
        if isinstance(tb, list):
            return [tuple(x) for x in reversed(tb)]
        return [(f.filename, f.lineno, f.name) for f in tb]


    def _limited(frames, limit):
        if limit is None:
            limit = getattr(sys, "tracebacklimit", None)
            if limit is not None and limit < 0:
                limit = 0
        if limit is not None:
            if limit >= 0:
                frames = frames[:limit]
            else:
                frames = frames[limit:]
        return frames


    class FrameSummary:
        """One frame of a stack: filename, lineno, name, line (its source text)."""

        __slots__ = ("filename", "lineno", "end_lineno", "colno", "end_colno", "name", "_lines", "locals", "_code", "_ip")

        def __init__(self, filename, lineno, name, *, lookup_line=True, locals=None, line=None,
                     end_lineno=None, colno=None, end_colno=None, **kwargs):
            self.filename = filename
            self.lineno = lineno
            self.end_lineno = lineno if end_lineno is None else end_lineno
            self.colno = colno
            self.end_colno = end_colno
            self.name = name
            self._lines = line
            self._code = kwargs.get("_code")
            self._ip = kwargs.get("_ip", -1)
            if lookup_line:
                self.line
            self.locals = {k: repr(v) for k, v in locals.items()} if locals else None

        def __eq__(self, other):
            if isinstance(other, FrameSummary):
                return (self.filename == other.filename and self.lineno == other.lineno and
                        self.name == other.name and self.locals == other.locals)
            if isinstance(other, tuple):
                return (self.filename, self.lineno, self.name, self.line) == other
            return NotImplemented

        def __getitem__(self, pos):
            return (self.filename, self.lineno, self.name, self.line)[pos]

        def __iter__(self):
            return iter([self.filename, self.lineno, self.name, self.line])

        def __repr__(self):
            return "<FrameSummary file {filename}, line {lineno} in {name}>".format(
                filename=self.filename, lineno=self.lineno, name=self.name)

        def __len__(self):
            return 4

        @property
        def _original_lines(self):
            self._set_lines()
            return self._lines

        def _set_lines(self):
            if not self._lines and self.lineno is not None:
                self._lines = linecache.getline(self.filename, self.lineno)

        @property
        def line(self):
            self._set_lines()
            if self._lines is None:
                return None
            return self._lines.strip()


    class StackSummary(list):
        """A list of FrameSummary objects."""

        @classmethod
        def extract(klass, frame_gen, *, limit=None, lookup_lines=True, capture_locals=False):
            result = klass()
            for f in frame_gen:
                if isinstance(f, FrameSummary):
                    result.append(f)
                else:
                    filename, lineno, name = f[0], f[1], f[2]
                    if len(f) >= 5:
                        result.append(FrameSummary(filename, lineno, name, lookup_line=lookup_lines, _code=f[3], _ip=f[4]))
                    else:
                        result.append(FrameSummary(filename, lineno, name, lookup_line=lookup_lines))
            return result

        @classmethod
        def from_list(klass, a_list):
            result = StackSummary()
            for frame in a_list:
                if isinstance(frame, FrameSummary):
                    result.append(frame)
                else:
                    filename, lineno, name, line = frame
                    result.append(FrameSummary(filename, lineno, name, line=line))
            return result

        def format_frame_summary(self, frame_summary, **kwargs):
            row = ['  File "{}", line {}, in {}\n'.format(frame_summary.filename, frame_summary.lineno, frame_summary.name)]
            code = getattr(frame_summary, "_code", None)
            if code is not None and frame_summary._ip >= 0:
                row.append(sys._frame_source(frame_summary.filename, frame_summary.lineno, code, frame_summary._ip))   # (CPython's carets)
            else:
                line = frame_summary.line
                if line:
                    row.append("    {}\n".format(line))
            if frame_summary.locals:
                for name, value in sorted(frame_summary.locals.items()):
                    row.append("    {name} = {value}\n".format(name=name, value=value))
            return "".join(row)

        def format(self, **kwargs):
            result = []
            last_file = None
            last_line = None
            last_name = None
            count = 0
            for frame_summary in self:
                formatted_frame = self.format_frame_summary(frame_summary)
                if formatted_frame is None:
                    continue
                if (last_file is None or last_file != frame_summary.filename or last_line is None or
                        last_line != frame_summary.lineno or last_name is None or last_name != frame_summary.name):
                    if count > _RECURSIVE_CUTOFF:
                        count -= _RECURSIVE_CUTOFF
                        result.append(f"  [Previous line repeated {count} more time{'s' if count > 1 else ''}]\n")
                    last_file = frame_summary.filename
                    last_line = frame_summary.lineno
                    last_name = frame_summary.name
                    count = 0
                count += 1
                if count > _RECURSIVE_CUTOFF:
                    continue
                result.append(formatted_frame)
            if count > _RECURSIVE_CUTOFF:
                count -= _RECURSIVE_CUTOFF
                result.append(f"  [Previous line repeated {count} more time{'s' if count > 1 else ''}]\n")
            return result


    def extract_tb(tb, limit=None):
        """The frames of a traceback (an exception's __traceback__), outermost first."""
        return StackSummary.extract(_limited(_frames(tb), limit))


    def format_tb(tb, limit=None):
        return extract_tb(tb, limit=limit).format()


    def print_tb(tb, limit=None, file=None):
        print_list(extract_tb(tb, limit=limit), file=file)


    def format_list(extracted_list):
        return StackSummary.from_list(extracted_list).format()


    def print_list(extracted_list, file=None):
        if file is None:
            file = sys.stderr
        for item in StackSummary.from_list(extracted_list).format():
            print(item, file=file, end="")


    def _qualname(etype):
        stype = getattr(etype, "__qualname__", etype.__name__)
        smod = getattr(etype, "__module__", None)
        if smod not in ("__main__", "builtins", None):
            stype = smod + "." + stype
        return stype


    def _safe_string(value, what, func=str):
        try:
            return func(value)
        except:
            return f"<{what} {func.__name__}() failed>"


    def _final_line(etype, value):
        stype = _qualname(etype)
        if value is None:
            return stype + ": None\n"
        s = _safe_string(value, "exception")
        if not s:
            return stype + "\n"
        return stype + ": " + s + "\n"


    def _parse_value_tb(exc, value, tb):
        if (value is _sentinel) != (tb is _sentinel):
            raise ValueError("Both or neither of value and tb must be given")
        if value is tb is _sentinel:
            if exc is not None:
                if isinstance(exc, BaseException):
                    return exc, exc.__traceback__
                raise TypeError(f"Exception expected for value, {type(exc).__name__} found")
            return None, None
        return value, tb


    def format_exception_only(exc, /, value=_sentinel, *, show_group=False):
        """The lines naming the exception: 'ValueError: message\\n' (and its notes)."""
        if value is _sentinel:
            value = exc
        if value is None:
            return [_final_line(type(None), None)]
        if isinstance(value, SyntaxError) and value.lineno is not None:
            return _syntax_error_lines(value)
        lines = [_final_line(type(value), value)]
        hint = sys._suggestion(value, False)                 # (". Did you mean: 'x'?")
        if hint:
            lines[0] = lines[0][:-1] + hint + "\n"
        notes = getattr(value, "__notes__", None)
        if notes is not None:
            if not isinstance(notes, (list, tuple)):
                lines.append(f"{_safe_string(notes, '__notes__', func=repr)}\n")
            else:
                for note in notes:
                    note = _safe_string(note, "note")
                    lines.append(note + "\n")
        return lines


    def _syntax_error_lines(value):
        lines = []
        filename = value.filename or "<string>"
        lines.append('  File "{}", line {}\n'.format(filename, value.lineno))
        text = value.text
        if text is not None:
            rtext = text.rstrip("\n")
            ltext = rtext.lstrip(" \n\f")
            spaces = len(rtext) - len(ltext)
            lines.append("    {}\n".format(ltext))
            if value.offset is not None:
                offset = value.offset
                end_offset = getattr(value, "end_offset", None) or offset
                if end_offset <= offset:
                    end_offset = offset + 1
                caretspace = ltext[:max(0, offset - 1 - spaces)]
                caretspace = "".join(c if c.isspace() else " " for c in caretspace)
                lines.append("    {}{}\n".format(caretspace, "^" * max(1, end_offset - offset)))
        msg = value.msg or "<no detail available>"
        lines.append("{}: {}\n".format(_qualname(type(value)), msg))
        return lines


    def _split_text(text):
        """The interpreter's traceback text as CPython's format_exception() parts."""
        parts = []
        lines = text.splitlines(keepends=True)
        i = 0
        while i < len(lines):                       # "\nThe above exception ...:\n\n": one part
            if (lines[i] == "\n" and i + 2 < len(lines) and lines[i + 2] == "\n" and
                    (lines[i + 1].startswith("The above exception") or lines[i + 1].startswith("During handling"))):
                parts.append(lines[i] + lines[i + 1] + lines[i + 2])
                i += 3
                continue
            line = lines[i]
            i += 1
            if parts and line.startswith("    ") and parts[-1].lstrip(" |").startswith("File "):
                parts[-1] += line
            elif parts and line.startswith("    ") and not parts[-1].endswith("\n"):
                parts[-1] += line
            else:
                parts.append(line)
        return parts


    def format_exception(exc, /, value=_sentinel, tb=_sentinel, limit=None, chain=True):
        """The lines of the exception's traceback, as an uncaught one prints it."""
        given_tb = tb is not _sentinel
        value, tb = _parse_value_tb(exc, value, tb)
        if value is None:
            return [_final_line(type(None), None)]
        if limit is None and chain and getattr(sys, "tracebacklimit", None) is None and (
                not given_tb or tb is value.__traceback__):
            return _split_text(sys._format_exception(value))
        return list(TracebackException(type(value), value, tb, limit=limit).format(chain=chain))


    def print_exception(exc, /, value=_sentinel, tb=_sentinel, limit=None, file=None, chain=True):
        """Prints the exception's traceback (on sys.stderr), as an uncaught one does."""
        if file is None:
            file = sys.stderr
        for line in format_exception(exc, value, tb, limit=limit, chain=chain):
            print(line, file=file, end="")


    def print_exc(limit=None, file=None, chain=True):
        """print_exception() of the exception being handled."""
        print_exception(sys.exception(), limit=limit, file=file, chain=chain)


    def format_exc(limit=None, chain=True):
        """The traceback of the exception being handled, as one string."""
        return "".join(format_exception(sys.exception(), limit=limit, chain=chain))


    def print_last(limit=None, file=None, chain=True):
        if not hasattr(sys, "last_exc") and not hasattr(sys, "last_type"):
            raise ValueError("no last exception")
        if hasattr(sys, "last_exc"):
            print_exception(sys.last_exc, limit=limit, file=file, chain=chain)
        else:
            print_exception(sys.last_type, sys.last_value, sys.last_traceback, limit=limit, file=file, chain=chain)


    def extract_stack(f=None, limit=None):
        """The running frames (to the caller), outermost first."""
        frames = sys._stack()[:-1]
        if limit is not None:
            frames = frames[-limit:] if limit > 0 else frames[:-limit] if limit < 0 else []
        return StackSummary.extract(frames)


    def format_stack(f=None, limit=None):
        frames = sys._stack()[:-1]
        if limit is not None:
            frames = frames[-limit:] if limit > 0 else frames[:-limit] if limit < 0 else []
        return StackSummary.extract(frames).format()


    def print_stack(f=None, limit=None, file=None):
        frames = sys._stack()[:-1]
        if limit is not None:
            frames = frames[-limit:] if limit > 0 else frames[:-limit] if limit < 0 else []
        print_list(StackSummary.extract(frames), file=file)


    def clear_frames(tb):
        pass


    def walk_tb(tb):
        """(frame, lineno) of each traceback entry (frame: its FrameSummary here)."""
        for filename, lineno, name in _frames(tb):
            yield FrameSummary(filename, lineno, name, lookup_line=False), lineno


    def walk_stack(f):
        for filename, lineno, name in reversed(sys._stack()[:-1]):
            yield FrameSummary(filename, lineno, name, lookup_line=False), lineno


    class TracebackException:
        """An exception ready for formatting: its type, message, stack, cause and context."""

        def __init__(self, exc_type, exc_value, exc_traceback, *, limit=None, lookup_lines=True,
                     capture_locals=False, compact=False, max_group_width=15, max_group_depth=10, _seen=None, **kwargs):
            if _seen is None:
                _seen = set()
            _seen.add(id(exc_value))
            self.stack = StackSummary.extract(_limited(_frames(exc_traceback), limit))
            self.exc_type = exc_type
            self.exc_type_str = _qualname(exc_type) if exc_type is not None else "None"
            self._str = _safe_string(exc_value, "exception")
            if exc_value is not None:
                self._str += sys._suggestion(exc_value, exc_traceback is not None)
            self.__notes__ = getattr(exc_value, "__notes__", None)
            self._exc_value = exc_value
            if exc_type and issubclass(exc_type, SyntaxError):
                self.filename = exc_value.filename
                lno = exc_value.lineno
                self.lineno = str(lno) if lno is not None else None
                self.text = exc_value.text
                self.offset = exc_value.offset
                self.msg = exc_value.msg
            self.__cause__ = None
            self.__context__ = None
            self.__suppress_context__ = bool(getattr(exc_value, "__suppress_context__", False)) if exc_value is not None else False
            if exc_value is not None:
                cause = exc_value.__cause__
                if cause is not None and id(cause) not in _seen:
                    self.__cause__ = TracebackException(type(cause), cause, cause.__traceback__, limit=limit, _seen=_seen)
                context = exc_value.__context__
                if context is not None and id(context) not in _seen:
                    self.__context__ = TracebackException(type(context), context, context.__traceback__, limit=limit, _seen=_seen)

        @classmethod
        def from_exception(cls, exc, *args, **kwargs):
            return cls(type(exc), exc, exc.__traceback__, *args, **kwargs)

        def __eq__(self, other):
            if isinstance(other, TracebackException):
                return self.__dict__ == other.__dict__
            return NotImplemented

        def __str__(self):
            return self._str

        def format_exception_only(self, *, show_group=False, _depth=0, **kwargs):
            value = self._exc_value
            if value is None:
                yield _final_line(type(None), None)
                return
            if isinstance(value, SyntaxError) and value.lineno is not None:
                yield from _syntax_error_lines(value)
                return
            stype = self.exc_type_str
            yield (stype + ": " + self._str if self._str else stype) + "\n"
            notes = self.__notes__
            if notes is not None:
                if not isinstance(notes, (list, tuple)):
                    yield f"{_safe_string(notes, '__notes__', func=repr)}\n"
                else:
                    for note in notes:
                        yield _safe_string(note, "note") + "\n"

        def format(self, *, chain=True, _ctx=None, **kwargs):
            if chain:
                if self.__cause__ is not None:
                    yield from self.__cause__.format(chain=chain)
                    yield "\nThe above exception was the direct cause of the following exception:\n\n"
                elif self.__context__ is not None and not self.__suppress_context__:
                    yield from self.__context__.format(chain=chain)
                    yield "\nDuring handling of the above exception, another exception occurred:\n\n"
            if self.stack:
                yield "Traceback (most recent call last):\n"
                yield from self.stack.format()
            yield from self.format_exception_only()

        def print(self, *, file=None, chain=True, **kwargs):
            if file is None:
                file = sys.stderr
            for line in self.format(chain=chain):
                print(line, file=file, end="")


if sys._compiled:
    # Compiled programs keep no tracebacks: an exception shows as the last lines CPython prints
    # for it ("ValueError: bad"), after its chain (__cause__ / __context__); the stack functions
    # show the line they are called from.
    from typing import Iterator, TypeVar

    _E = TypeVar("_E")
    _V = TypeVar("_V")
    _T = TypeVar("_T")
    _S = TypeVar("_S")

    def _type_str(e: BaseException) -> str:
        t = type(e)
        m = t.__module__
        if m == "__main__" or m == "builtins":
            return t.__qualname__
        return m + "." + t.__qualname__

    def _final_line(e: BaseException) -> str:
        s = str(e)
        return _type_str(e) + (": " + s if s else "") + "\n"

    def _chain(e: BaseException, chain: bool) -> list[str]:
        """The lines of e and (chain) the exceptions before it, oldest first."""
        out: list[str] = []
        seen: list[BaseException] = []
        cur: BaseException | None = e
        parts: list[str] = []
        while cur is not None:
            seen.append(cur)
            parts.insert(0, _final_line(cur))
            if not chain:
                break
            nxt: BaseException | None = None
            cause = cur.__cause__
            ctx = cur.__context__
            if cause is not None:
                nxt = cause
                parts.insert(0, "\nThe above exception was the direct cause of the following exception:\n\n")
            elif ctx is not None and not cur.__suppress_context__:
                nxt = ctx
                parts.insert(0, "\nDuring handling of the above exception, another exception occurred:\n\n")
            if nxt is not None:
                for s in seen:
                    if s is nxt:
                        nxt = None
                        del parts[0]
                        break
            cur = nxt
        for p in parts:
            out.append(p)
        return out

    def format_exception_only(exc: _E, value: _V = None, *, show_group: bool = False) -> list[str]:
        """["Name: message\\n"] of an exception (or of the class and the exception: the older form)."""
        if isinstance(exc, BaseException):
            return [_final_line(exc)]
        elif value is None:
            return [exc.__name__ + "\n"]
        else:
            return [_final_line(value)]

    def format_exception(exc: _E, value: _V = None, tb: _T = None, limit: int | None = None,
                         chain: bool = True) -> list[str]:
        """The lines that show an exception (and the ones before it)."""
        if isinstance(exc, BaseException):
            return _chain(exc, chain)
        elif value is None:
            return ["NoneType: None\n"]
        else:
            return _chain(value, chain)

    def _write(lines: list[str], file: _S = None) -> None:
        if file is None:
            print("".join(lines), end="", file=sys.stderr)
        else:
            file.write("".join(lines))

    def print_exception(exc: _E, value: _V = None, tb: _T = None, limit: int | None = None, file: _S = None,
                        chain: bool = True) -> None:
        """Writes format_exception() to file (sys.stderr)."""
        _write(format_exception(exc, value, tb, limit, chain), file)

    def format_exc(limit: int | None = None, chain: bool = True, _exc: BaseException | None = sys.exception()) -> str:
        """The exception being handled, as text."""
        if _exc is None:
            return "NoneType: None\n"
        return "".join(_chain(_exc, chain))

    def print_exc(limit: int | None = None, file: _S = None, chain: bool = True,
                  _exc: BaseException | None = sys.exception()) -> None:
        """Writes the exception being handled to file (sys.stderr)."""
        if _exc is None:
            _write(["NoneType: None\n"], file)
        else:
            _write(_chain(_exc, chain), file)

    class FrameSummary:
        """A place in the program: file, line, function (and the line's text)."""

        def __init__(self, filename: str, lineno: int | None, name: str, *, lookup_line: bool = True,
                     locals: _T = None, line: str | None = None) -> None:
            self.filename = filename
            self.lineno = lineno
            self.name = name
            self._line = line
            self.end_lineno = lineno
            self.colno: int | None = None
            self.end_colno: int | None = None

        @property
        def line(self) -> str | None:
            s = self._line
            if s is None:
                return None
            return s.strip()

        def __eq__(self, other: "FrameSummary") -> bool:
            return (self.filename == other.filename and self.lineno == other.lineno and self.name == other.name
                    and self.line == other.line)

        def __len__(self) -> int:
            return 4

        def __repr__(self) -> str:
            return "<FrameSummary file " + self.filename + ", line " + str(self.lineno) + " in " + self.name + ">"

    class StackSummary:
        """A list of FrameSummary, outermost first."""

        def __init__(self, frames: list[FrameSummary] | None = None) -> None:
            self._frames: list[FrameSummary] = frames if frames is not None else []

        def __len__(self) -> int:
            return len(self._frames)

        def __getitem__(self, i: int) -> FrameSummary:
            return self._frames[i]

        def __iter__(self) -> Iterator[FrameSummary]:
            for f in self._frames:
                yield f

        def append(self, f: FrameSummary) -> None:
            self._frames.append(f)

        def format_frame_summary(self, f: FrameSummary) -> str:
            s = '  File "' + f.filename + '", line ' + str(f.lineno) + ", in " + f.name + "\n"
            line = f.line
            if line:
                s += "    " + line + "\n"
            return s

        def format(self) -> list[str]:
            return [self.format_frame_summary(f) for f in self._frames]

    def _here(file: str, line: int, func: str, src: str) -> StackSummary:
        return StackSummary([FrameSummary(file, line, func, line=src)])

    def extract_stack(f: _T = None, limit: int | None = None, _file: str = sys._site("file"),
                      _line: int = sys._site("line"), _func: str = sys._site("func"),
                      _src: str = sys._site("source")) -> StackSummary:
        """The stack: (in compiled programs) the place of this call."""
        return _here(_file, _line, _func, _src)

    def format_stack(f: _T = None, limit: int | None = None, _file: str = sys._site("file"),
                     _line: int = sys._site("line"), _func: str = sys._site("func"),
                     _src: str = sys._site("source")) -> list[str]:
        return _here(_file, _line, _func, _src).format()

    def print_stack(f: _T = None, limit: int | None = None, file: _S = None, _file: str = sys._site("file"),
                    _line: int = sys._site("line"), _func: str = sys._site("func"),
                    _src: str = sys._site("source")) -> None:
        _write(_here(_file, _line, _func, _src).format(), file)

    def format_list(extracted_list: StackSummary) -> list[str]:
        return extracted_list.format()

    def print_list(extracted_list: StackSummary, file: _S = None) -> None:
        _write(extracted_list.format(), file)

    class TracebackException:
        """An exception made ready to be shown: TracebackException.from_exception(e).format()."""

        def __init__(self, exc_type: type[BaseException], exc_value: BaseException, exc_traceback: _T = None, *,
                     limit: int | None = None, lookup_lines: bool = True, capture_locals: bool = False,
                     compact: bool = False, max_group_width: int = 15, max_group_depth: int = 10) -> None:
            self._exc = exc_value
            self.exc_type_str = _type_str(exc_value)
            self.stack = StackSummary()
            self._str = str(exc_value)
            self.__suppress_context__ = exc_value.__suppress_context__

        @staticmethod
        def from_exception(exc: BaseException, *, limit: int | None = None, lookup_lines: bool = True,
                           capture_locals: bool = False, compact: bool = False) -> "TracebackException":
            return TracebackException(type(exc), exc)

        def format_exception_only(self, *, show_group: bool = False) -> Iterator[str]:
            yield _final_line(self._exc)

        def format(self, *, chain: bool = True) -> Iterator[str]:
            for s in _chain(self._exc, chain):
                yield s

        def print(self, *, file: _S = None, chain: bool = True) -> None:
            _write(_chain(self._exc, chain), file)

        def __eq__(self, other: "TracebackException") -> bool:
            return self.exc_type_str == other.exc_type_str and self._str == other._str

        def __str__(self) -> str:
            return self._str
