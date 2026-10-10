"""CSV files (CPython's csv): reader, writer, DictReader, DictWriter and dialects.

A reader takes lines (a file, a list of str ...) and gives each record as a list of
str; a writer writes rows to anything with write(). The parser is _csv's state
machine. In compiled programs a dialect is named by a str (or given as keyword
arguments), rows are lists (or tuples) of one type, QUOTE_NONNUMERIC/QUOTE_STRINGS
reading is not available and DictReader's extra fields (restkey) are left out."""
import sys
from typing import TypeVar

_T = TypeVar("_T")
_R = TypeVar("_R")
_D = TypeVar("_D")

__all__ = ["QUOTE_MINIMAL", "QUOTE_ALL", "QUOTE_NONNUMERIC", "QUOTE_NONE", "QUOTE_STRINGS", "QUOTE_NOTNULL",
           "Error", "Dialect", "excel", "excel_tab", "field_size_limit", "reader", "writer", "register_dialect",
           "get_dialect", "list_dialects", "unregister_dialect", "DictReader", "DictWriter", "unix_dialect"]

QUOTE_MINIMAL = 0
QUOTE_ALL = 1
QUOTE_NONNUMERIC = 2
QUOTE_NONE = 3
QUOTE_STRINGS = 4
QUOTE_NOTNULL = 5

__version__ = "1.0"

_field_limit = 131072


class Error(Exception):
    pass


def field_size_limit(new_limit: int | None = None) -> int:
    """The longest field a reader accepts (and sets a new one)."""
    global _field_limit
    old = _field_limit
    if new_limit is not None:
        _field_limit = new_limit
    return old


class _Dialect:
    """The settings a reader or writer uses."""

    def __init__(self, delimiter: str = ",", quotechar: str | None = "\"", escapechar: str | None = None,
                 doublequote: bool = True, skipinitialspace: bool = False, lineterminator: str = "\r\n",
                 quoting: int = QUOTE_MINIMAL, strict: bool = False):
        self.delimiter = delimiter
        self.quotechar = quotechar
        self.escapechar = escapechar
        self.doublequote = doublequote
        self.skipinitialspace = skipinitialspace
        self.lineterminator = lineterminator
        self.quoting = quoting
        self.strict = strict

    def _copy(self) -> "_Dialect":
        return _Dialect(self.delimiter, self.quotechar, self.escapechar, self.doublequote, self.skipinitialspace,
                        self.lineterminator, self.quoting, self.strict)

    def _check(self) -> None:
        if len(self.delimiter) != 1:
            raise TypeError("\"delimiter\" must be a 1-character string")
        if self.quotechar is not None and len(self.quotechar) != 1:
            raise TypeError("\"quotechar\" must be a 1-character string")
        if self.escapechar is not None and len(self.escapechar) != 1:
            raise TypeError("\"escapechar\" must be a 1-character string")
        if self.quoting < 0 or self.quoting > 5:
            raise TypeError("bad \"quoting\" value")
        if self.quotechar is None and self.quoting != QUOTE_NONE:
            raise TypeError("quotechar must be set if quoting enabled")
        if self.delimiter == " " and self.skipinitialspace:
            raise ValueError("empty field must be quoted if delimiter is a space and skipinitialspace is true")
        if self.delimiter in "\r\n":
            raise ValueError("bad delimiter value")
        if self.quotechar is not None and self.quotechar in "\r\n":
            raise ValueError("bad quotechar value")
        if self.escapechar is not None and self.escapechar in "\r\n":
            raise ValueError("bad escapechar value")
        if self.lineterminator and self.lineterminator in (self.delimiter, self.quotechar or "", self.escapechar or ""):
            raise ValueError("bad lineterminator value")
        if self.quotechar is not None and self.quotechar == self.delimiter:
            raise ValueError("bad delimiter or quotechar value")
        if self.escapechar is not None and (self.escapechar == self.delimiter or self.escapechar == self.quotechar):
            raise ValueError("bad delimiter or escapechar value")


_dialects: dict[str, _Dialect] = {"excel": _Dialect(), "excel-tab": _Dialect("\t"),
                                  "unix": _Dialect(lineterminator="\n", quoting=QUOTE_ALL)}


def register_dialect(name: str, dialect: str | None = None, delimiter: str | None = None, quotechar: str | None = None,
                     escapechar: str | None = None, doublequote: bool | None = None,
                     skipinitialspace: bool | None = None, lineterminator: str | None = None,
                     quoting: int | None = None, strict: bool | None = None) -> None:
    """A dialect named name (from a dialect and/or the keyword arguments)."""
    d = _make_dialect(dialect, delimiter, quotechar, escapechar, doublequote, skipinitialspace, lineterminator,
                      quoting, strict)
    _dialects[name] = d


def unregister_dialect(name: str) -> None:
    if name not in _dialects:
        raise Error("unknown dialect")
    del _dialects[name]


def get_dialect(name: str) -> _Dialect:
    if name not in _dialects:
        raise Error("unknown dialect")
    return _dialects[name]


def list_dialects() -> list[str]:
    return list(_dialects)


def _make_dialect(dialect: _D, delimiter: str | None, quotechar: str | None, escapechar: str | None,
                  doublequote: bool | None, skipinitialspace: bool | None, lineterminator: str | None,
                  quoting: int | None, strict: bool | None) -> _Dialect:
    d = _Dialect()
    if isinstance(dialect, str):
        d = get_dialect(dialect)._copy()
    elif isinstance(dialect, _Dialect):
        d = dialect._copy()
    elif dialect is not None:
        if not sys._compiled:
            d = _Dialect(getattr(dialect, "delimiter", ","), getattr(dialect, "quotechar", "\""),
                         getattr(dialect, "escapechar", None), getattr(dialect, "doublequote", True),
                         getattr(dialect, "skipinitialspace", False), getattr(dialect, "lineterminator", "\r\n"),
                         getattr(dialect, "quoting", QUOTE_MINIMAL), getattr(dialect, "strict", False))
    if delimiter is not None:
        d.delimiter = delimiter
    if quotechar is not None or quoting == QUOTE_NONE:
        d.quotechar = quotechar
    if escapechar is not None:
        d.escapechar = escapechar
    if doublequote is not None:
        d.doublequote = doublequote
    if skipinitialspace is not None:
        d.skipinitialspace = skipinitialspace
    if lineterminator is not None:
        d.lineterminator = lineterminator
    if quoting is not None:
        d.quoting = quoting
    if strict is not None:
        d.strict = strict
    d._check()
    return d


# the parser's states
_START_RECORD = 0
_START_FIELD = 1
_ESCAPED_CHAR = 2
_IN_FIELD = 3
_IN_QUOTED_FIELD = 4
_ESCAPE_IN_QUOTED_FIELD = 5
_QUOTE_IN_QUOTED_FIELD = 6
_EAT_CRNL = 7
_AFTER_ESCAPED_CRNL = 8


class reader:
    """The records of csvfile (lines: a file, a list ...), each a list of str. line_num: lines read."""

    def __init__(self, csvfile, dialect: _D = "excel", delimiter: str | None = None, quotechar: str | None = None,
                 escapechar: str | None = None, doublequote: bool | None = None,
                 skipinitialspace: bool | None = None, lineterminator: str | None = None,
                 quoting: int | None = None, strict: bool | None = None):
        self.dialect = _make_dialect(dialect, delimiter, quotechar, escapechar, doublequote, skipinitialspace,
                                     lineterminator, quoting, strict)
        self._src = csvfile
        self._lines = self._each()
        self.line_num = 0
        self._fields: list[str] = []
        self._field: list[str] = []
        self._state = _START_RECORD
        self._quoted = False                # (the field was quoted: QUOTE_NONNUMERIC / QUOTE_STRINGS keep it a str)
        self._numeric = False
        self._values: list = []             # (interpreter: the converted fields of QUOTE_NONNUMERIC ...)

    def _each(self):
        for line in self._src:
            yield line

    def _save_field(self) -> None:
        text = "".join(self._field)
        self._field = []
        self._fields.append(text)
        if not sys._compiled:
            q = self.dialect.quoting
            v = text
            if q == QUOTE_NONNUMERIC and self._numeric:
                v = float(text)
            elif (q == QUOTE_STRINGS or q == QUOTE_NOTNULL) and not self._quoted:
                if text == "":
                    v = None
                elif q == QUOTE_STRINGS:
                    v = float(text)
            self._values.append(v)
        self._numeric = False
        self._quoted = False

    def _add_char(self, c: str) -> None:
        if len(self._field) >= _field_limit:
            raise Error("field larger than field limit (" + str(_field_limit) + ")")
        self._field.append(c)

    def _process(self, c: str) -> None:
        """One character of a line, or "" at its end."""
        d = self.dialect
        st = self._state
        if st == _START_RECORD:
            if c == "":
                return
            if c == "\n" or c == "\r":
                self._state = _EAT_CRNL
                return
            st = _START_FIELD
            self._state = _START_FIELD
        if st == _START_FIELD:
            if c == "\n" or c == "\r" or c == "":
                self._save_field()
                self._state = _START_RECORD if c == "" else _EAT_CRNL
            elif c == d.quotechar and d.quoting != QUOTE_NONE:
                self._quoted = True
                self._state = _IN_QUOTED_FIELD
            elif c == d.escapechar:
                self._state = _ESCAPED_CHAR
            elif c == " " and d.skipinitialspace:
                pass
            elif c == d.delimiter:
                self._save_field()
            else:
                if d.quoting == QUOTE_NONNUMERIC:
                    self._numeric = True
                self._add_char(c)
                self._state = _IN_FIELD
        elif st == _ESCAPED_CHAR:
            if c == "\n" or c == "\r":
                self._add_char(c)
                self._state = _AFTER_ESCAPED_CRNL
                return
            self._add_char("\n" if c == "" else c)
            self._state = _IN_FIELD
        elif st == _AFTER_ESCAPED_CRNL or st == _IN_FIELD:
            if st == _AFTER_ESCAPED_CRNL and c == "":
                return
            if c == "\n" or c == "\r" or c == "":
                self._save_field()
                self._state = _START_RECORD if c == "" else _EAT_CRNL
            elif c == d.escapechar:
                self._state = _ESCAPED_CHAR
            elif c == d.delimiter:
                self._save_field()
                self._state = _START_FIELD
            else:
                self._add_char(c)
                self._state = _IN_FIELD
        elif st == _IN_QUOTED_FIELD:
            if c == "":
                pass
            elif c == d.escapechar:
                self._state = _ESCAPE_IN_QUOTED_FIELD
            elif c == d.quotechar and d.quoting != QUOTE_NONE:
                self._state = _QUOTE_IN_QUOTED_FIELD if d.doublequote else _IN_FIELD
            else:
                self._add_char(c)
        elif st == _ESCAPE_IN_QUOTED_FIELD:
            self._add_char("\n" if c == "" else c)
            self._state = _IN_QUOTED_FIELD
        elif st == _QUOTE_IN_QUOTED_FIELD:
            if d.quoting != QUOTE_NONE and c == d.quotechar:
                self._add_char(c)
                self._state = _IN_QUOTED_FIELD
            elif c == d.delimiter:
                self._save_field()
                self._state = _START_FIELD
            elif c == "\n" or c == "\r" or c == "":
                self._save_field()
                self._state = _START_RECORD if c == "" else _EAT_CRNL
            elif not d.strict:
                self._add_char(c)
                self._state = _IN_FIELD
            else:
                raise Error("'" + d.delimiter + "' expected after '" + str(d.quotechar) + "'")
        elif st == _EAT_CRNL:
            if c == "\n" or c == "\r":
                pass
            elif c == "":
                self._state = _START_RECORD
            else:
                raise Error("new-line character seen in unquoted field - do you need to open the file with "
                            "newline=''?")

    def __iter__(self):
        return self

    def __next__(self) -> list[str]:
        self._fields = []
        self._field = []
        self._values = []
        self._state = _START_RECORD
        self._numeric = False
        self._quoted = False
        while True:
            try:
                line = next(self._lines)
            except StopIteration:
                if self._field or self._state == _IN_QUOTED_FIELD:
                    if self.dialect.strict:
                        raise Error("unexpected end of data")
                    self._save_field()
                    break
                raise
            self.line_num += 1
            for c in line:
                if c == "\x00":
                    raise Error("line contains NUL")
                self._process(c)
            self._process("")
            if self._state == _START_RECORD:
                break
        if not sys._compiled:
            q = self.dialect.quoting
            if q == QUOTE_NONNUMERIC or q == QUOTE_STRINGS or q == QUOTE_NOTNULL:
                return self._values
        return self._fields


def _is_number(x: _T) -> bool:
    if isinstance(x, (int, float)):
        return True
    else:
        return False


def _text_of(x: _T) -> str:
    if isinstance(x, str):
        return x
    elif isinstance(x, float):
        return repr(x)
    elif isinstance(x, (int, bool)):
        return str(x)
    else:
        if x is None:
            return ""
        return str(x)


class writer:
    """Writes rows (lists of values) to f as CSV text: writerow, writerows."""

    def __init__(self, f, dialect: _D = "excel", delimiter: str | None = None, quotechar: str | None = None,
                 escapechar: str | None = None, doublequote: bool | None = None,
                 skipinitialspace: bool | None = None, lineterminator: str | None = None,
                 quoting: int | None = None, strict: bool | None = None):
        self.dialect = _make_dialect(dialect, delimiter, quotechar, escapechar, doublequote, skipinitialspace,
                                     lineterminator, quoting, strict)
        self._f = f

    def _append(self, out: list[str], field: str, quoted: bool, single: bool) -> None:
        d = self.dialect
        chars: list[str] = []
        for c in field:
            want_escape = False
            if (c == d.delimiter or c == d.escapechar or c == d.quotechar or c == "\n" or c == "\r" or
                    (d.lineterminator and c in d.lineterminator)):
                if d.quoting == QUOTE_NONE:
                    want_escape = True
                else:
                    if c == d.quotechar:
                        if d.doublequote:
                            chars.append(c)
                        else:
                            want_escape = True
                    elif c == d.escapechar:
                        want_escape = True
                    if not want_escape:
                        quoted = True
                if want_escape:
                    if d.escapechar is None:
                        raise Error("need to escape, but no escapechar set")
                    chars.append(d.escapechar)
            chars.append(c)
        if not field and single:
            if d.quoting == QUOTE_NONE:
                raise Error("single empty field record must be quoted")
            quoted = True
        text = "".join(chars)
        if quoted:
            q = d.quotechar if d.quotechar is not None else ""
            text = q + text + q
        out.append(text)

    def writerow(self, row: _R):
        """Writes one row; what f.write gives."""
        d = self.dialect
        parts: list[str] = []
        n = 0
        for field in row:
            n += 1
        k = 0
        for field in row:
            quoted = False
            if d.quoting == QUOTE_NONNUMERIC:
                quoted = not _is_number(field)
            elif d.quoting == QUOTE_ALL:
                quoted = True
            elif d.quoting == QUOTE_STRINGS:
                quoted = isinstance(field, str)
            elif d.quoting == QUOTE_NOTNULL:
                quoted = field is not None
            self._append(parts, _text_of(field), quoted, n == 1)
            k += 1
        return self._f.write(d.delimiter.join(parts) + d.lineterminator)

    def writerows(self, rows: _T) -> None:
        """Writes each row."""
        for row in rows:
            self.writerow(row)


class DictReader:
    """The records of f as dicts by the field names (the first record, or fieldnames)."""

    def __init__(self, f, fieldnames: list[str] | None = None, restkey: str | None = None,
                 restval: str | None = None, dialect: _D = "excel", delimiter: str | None = None,
                 quotechar: str | None = None, escapechar: str | None = None, doublequote: bool | None = None,
                 skipinitialspace: bool | None = None, lineterminator: str | None = None,
                 quoting: int | None = None, strict: bool | None = None):
        self._fieldnames = fieldnames
        self.restkey = restkey
        self.restval = restval
        self.reader = reader(f, dialect, delimiter, quotechar, escapechar, doublequote, skipinitialspace,
                             lineterminator, quoting, strict)
        self.dialect = dialect
        self.line_num = 0

    @property
    def fieldnames(self) -> list[str] | None:
        if self._fieldnames is None:
            try:
                self._fieldnames = next(self.reader)
            except StopIteration:
                pass
        self.line_num = self.reader.line_num
        return self._fieldnames

    def __iter__(self):
        return self

    def __next__(self) -> dict[str, str | None]:
        if self.line_num == 0:
            self.fieldnames
        row = next(self.reader)
        self.line_num = self.reader.line_num
        while not row:
            row = next(self.reader)
        names = self._fieldnames
        d: dict[str, str | None] = {}
        if names is None:
            return d
        for i in range(min(len(names), len(row))):
            d[names[i]] = row[i]
        lf = len(names)
        lr = len(row)
        if lf < lr:
            if not sys._compiled:
                d[self.restkey] = row[lf:]
        elif lf > lr:
            for key in names[lr:]:
                d[key] = self.restval
        return d


class DictWriter:
    """Writes dicts as rows of the values of fieldnames (restval for missing ones)."""

    def __init__(self, f, fieldnames: list[str], restval: str = "", extrasaction: str = "raise",
                 dialect: _D = "excel", delimiter: str | None = None, quotechar: str | None = None,
                 escapechar: str | None = None, doublequote: bool | None = None,
                 skipinitialspace: bool | None = None, lineterminator: str | None = None,
                 quoting: int | None = None, strict: bool | None = None):
        self.fieldnames = list(fieldnames)
        self.restval = restval
        if extrasaction.lower() not in ("raise", "ignore"):
            raise ValueError("extrasaction (" + extrasaction + ") must be 'raise' or 'ignore'")
        self.extrasaction = extrasaction
        self.writer = writer(f, dialect, delimiter, quotechar, escapechar, doublequote, skipinitialspace,
                             lineterminator, quoting, strict)

    def writeheader(self):
        """Writes the field names."""
        return self.writer.writerow(self.fieldnames)

    def writerow(self, rowdict: _R):
        """Writes the values of rowdict in fieldnames' order."""
        if self.extrasaction.lower() == "raise":
            wrong: list[str] = []
            for k in rowdict:
                if k not in self.fieldnames:
                    wrong.append(repr(k))
            if wrong:
                raise ValueError("dict contains fields not in fieldnames: " + ", ".join(wrong))
        vals: list[str] = []
        for key in self.fieldnames:
            if key in rowdict:
                vals.append(_text_of(rowdict[key]))
            else:
                vals.append(self.restval)
        if not sys._compiled:
            vals = [rowdict.get(key, self.restval) for key in self.fieldnames]
        return self.writer.writerow(vals)

    def writerows(self, rowdicts: _T) -> None:
        for rd in rowdicts:
            self.writerow(rd)


if not sys._compiled:
    class Dialect:
        """Describe a CSV dialect (subclass it and set the attributes)."""
        _name = ""
        _valid = False
        delimiter = None
        quotechar = None
        escapechar = None
        doublequote = None
        skipinitialspace = None
        lineterminator = None
        quoting = None
        strict = False

        def __init__(self):
            if self.__class__ != Dialect:
                self._valid = True
            _make_dialect(self, None, None, None, None, None, None, None, None)

    class excel(Dialect):
        """The usual properties of an Excel-generated CSV file."""
        delimiter = ","
        quotechar = "\""
        doublequote = True
        skipinitialspace = False
        lineterminator = "\r\n"
        quoting = QUOTE_MINIMAL

    class excel_tab(excel):
        """The usual properties of an Excel-generated TAB-delimited file."""
        delimiter = "\t"

    class unix_dialect(Dialect):
        """The usual properties of a CSV file made on UNIX."""
        delimiter = ","
        quotechar = "\""
        doublequote = True
        skipinitialspace = False
        lineterminator = "\n"
        quoting = QUOTE_ALL

    def _register(name, dialect=None, **fmtparams):
        _dialects[name] = _make_dialect(dialect, fmtparams.get("delimiter"), fmtparams.get("quotechar"),
                                        fmtparams.get("escapechar"), fmtparams.get("doublequote"),
                                        fmtparams.get("skipinitialspace"), fmtparams.get("lineterminator"),
                                        fmtparams.get("quoting"), fmtparams.get("strict"))
    register_dialect = _register
