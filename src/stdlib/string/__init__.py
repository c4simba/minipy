"""A collection of string constants, capwords(), Template ($-substitutions) and
Formatter (str.format() as a class; the interpreter's) - CPython's string.

whitespace, ascii_lowercase, ascii_uppercase, ascii_letters, digits, hexdigits,
octdigits, punctuation, printable."""
import sys
from typing import TypeVar

__all__ = ["ascii_letters", "ascii_lowercase", "ascii_uppercase", "capwords",
           "digits", "hexdigits", "octdigits", "printable", "punctuation",
           "whitespace", "Formatter", "Template"]

T = TypeVar("T")

whitespace = " \t\n\r\x0b\x0c"
ascii_lowercase = "abcdefghijklmnopqrstuvwxyz"
ascii_uppercase = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
ascii_letters = ascii_lowercase + ascii_uppercase
digits = "0123456789"
hexdigits = digits + "abcdef" + "ABCDEF"
octdigits = "01234567"
punctuation = r"""!"#$%&'()*+,-./:;<=>?@[\]^_`{|}~"""
printable = digits + ascii_letters + punctuation + whitespace


def capwords(s: str, sep: str | None = None) -> str:
    """The words of s (split by sep, or runs of whitespace) capitalized, joined by sep (or a space)."""
    return (sep or " ").join([w.capitalize() for w in s.split(sep)])


def _id_start(c: str) -> bool:
    return c == "_" or "a" <= c <= "z" or "A" <= c <= "Z"


def _id_char(c: str) -> bool:
    return c == "_" or "a" <= c <= "z" or "A" <= c <= "Z" or "0" <= c <= "9"


class Template:
    """A string with $-substitutions: Template("$who likes ${what}").substitute(who="tim", what="ham")."""

    delimiter = "$"
    idpattern = r"(?a:[_a-z][_a-z0-9]*)"
    braceidpattern: str | None = None
    flags: int | None = None    # default: re.IGNORECASE

    def __init__(self, template: str) -> None:
        self.template = template

    def _scan(self) -> list[tuple[int, int, int, str, int]]:
        """The template's parts (kind, start, end, name, where): 0 text, 1 $$, 2 $name or ${name},
        3 a lone delimiter (where: the invalid placeholder's position, after the delimiter)."""
        if not sys._compiled:
            if self._custom():
                return self._scan_re()
        parts: list[tuple[int, int, int, str, int]] = []
        s = self.template
        delim = self.delimiter
        dl = len(delim)
        n = len(s)
        i = 0
        text = 0
        while i < n:
            if not (dl and s.startswith(delim, i)):
                i += 1
                continue
            if text < i:
                parts.append((0, text, i, "", 0))
            j = i + dl
            if s.startswith(delim, j):
                parts.append((1, i, j + dl, "", 0))
                i = j + dl
            elif j < n and _id_start(s[j]):
                k = j + 1
                while k < n and _id_char(s[k]):
                    k += 1
                parts.append((2, i, k, s[j:k], 0))
                i = k
            elif j < n and s[j] == "{" and j + 1 < n and _id_start(s[j + 1]):
                k = j + 2
                while k < n and _id_char(s[k]):
                    k += 1
                if k < n and s[k] == "}":
                    parts.append((2, i, k + 1, s[j + 1:k], 0))
                    i = k + 1
                else:
                    parts.append((3, i, j, "", j))
                    i = j
            else:
                parts.append((3, i, j, "", j))
                i = j
            text = i
        if text < n:
            parts.append((0, text, n, "", 0))
        return parts

    def _invalid(self, i: int) -> None:
        lines = self.template[:i].splitlines(keepends=True)
        if not lines:
            colno = 1
            lineno = 1
        else:
            colno = i - len("".join(lines[:-1]))
            lineno = len(lines)
        raise ValueError("Invalid placeholder in string: line %d, col %d" % (lineno, colno))

    if sys._compiled:
        def substitute(self, mapping: dict[str, T] | None = None, /, **kws: T) -> str:
            """The template with each $name replaced by str(mapping[name]) (or kws[name])."""
            out: list[str] = []
            for kind, start, end, name, where in self._scan():
                if kind == 0:
                    out.append(self.template[start:end])
                elif kind == 1:
                    out.append(self.delimiter)
                elif kind == 2:
                    if name in kws:
                        out.append(str(kws[name]))
                    elif mapping is not None:
                        out.append(str(mapping[name]))
                    else:
                        raise KeyError(name)
                else:
                    self._invalid(where)
            return "".join(out)

        def safe_substitute(self, mapping: dict[str, T] | None = None, /, **kws: T) -> str:
            """As substitute(), with unknown names and lone delimiters left as they are."""
            out: list[str] = []
            for kind, start, end, name, where in self._scan():
                if kind == 1:
                    out.append(self.delimiter)
                elif kind == 2 and name in kws:
                    out.append(str(kws[name]))
                elif kind == 2 and mapping is not None and name in mapping:
                    out.append(str(mapping[name]))
                else:
                    out.append(self.template[start:end])
            return "".join(out)
    else:
        def substitute(self, mapping=None, /, **kws):
            """The template with each $name replaced by str(mapping[name]) (or kws[name])."""
            if mapping is None:
                mapping = kws
            elif kws:
                from collections import ChainMap
                mapping = ChainMap(kws, mapping)
            out = []
            for kind, start, end, name, where in self._scan():
                if kind == 0:
                    out.append(self.template[start:end])
                elif kind == 1:
                    out.append(self.delimiter)
                elif kind == 2:
                    out.append(str(mapping[name]))
                else:
                    self._invalid(where)
            return "".join(out)

        def safe_substitute(self, mapping=None, /, **kws):
            """As substitute(), with unknown names and lone delimiters left as they are."""
            if mapping is None:
                mapping = kws
            elif kws:
                from collections import ChainMap
                mapping = ChainMap(kws, mapping)
            out = []
            for kind, start, end, name, where in self._scan():
                if kind == 1:
                    out.append(self.delimiter)
                elif kind == 2:
                    try:
                        out.append(str(mapping[name]))
                    except KeyError:
                        out.append(self.template[start:end])
                else:
                    out.append(self.template[start:end])
            return "".join(out)

        def _custom(self):
            """A subclass with patterns of its own: re does the scanning."""
            cls = type(self)
            if getattr(cls, "_own_pattern", False):
                return True
            if cls.idpattern != Template.idpattern or cls.braceidpattern is not None:
                return True
            return cls.flags is not None and int(cls.flags) != 2       # (re.IGNORECASE)

        def __init_subclass__(cls):
            super().__init_subclass__()
            if "pattern" in cls.__dict__:
                cls._own_pattern = True
            cls._compile_pattern()

        @classmethod
        def _compile_pattern(cls):
            import re
            pattern = cls.__dict__.get("pattern", _TemplatePattern)
            if pattern is _TemplatePattern:
                delim = re.escape(cls.delimiter)
                id = cls.idpattern
                bid = cls.braceidpattern or cls.idpattern
                pattern = fr"""
            {delim}(?:
              (?P<escaped>{delim})  |   # Escape sequence of two delimiters
              (?P<named>{id})       |   # delimiter and a Python identifier
              {{(?P<braced>{bid})}} |   # delimiter and a braced identifier
              (?P<invalid>)             # Other ill-formed delimiter exprs
            )
            """
            if cls.flags is None:
                cls.flags = re.IGNORECASE
            pat = cls.pattern = re.compile(pattern, cls.flags | re.VERBOSE)
            return pat

        def _scan_re(self):
            parts = []
            text = 0
            pattern = type(self).pattern
            for mo in pattern.finditer(self.template):
                if text < mo.start():
                    parts.append((0, text, mo.start(), "", 0))
                named = mo.group("named") or mo.group("braced")
                if named is not None:
                    parts.append((2, mo.start(), mo.end(), named, 0))
                elif mo.group("escaped") is not None:
                    parts.append((1, mo.start(), mo.end(), "", 0))
                elif mo.group("invalid") is not None:
                    parts.append((3, mo.start(), mo.end(), "", mo.start("invalid")))
                else:
                    raise ValueError("Unrecognized named group in pattern", pattern)
                text = mo.end()
            if text < len(self.template):
                parts.append((0, text, len(self.template), "", 0))
            return parts

    def is_valid(self) -> bool:
        """False if the template has a lone delimiter (an invalid placeholder)."""
        for kind, start, end, name, where in self._scan():
            if kind == 3:
                return False
        return True

    def get_identifiers(self) -> list[str]:
        """The names of the placeholders, in order of first appearance."""
        ids: list[str] = []
        for kind, start, end, name, where in self._scan():
            if kind == 2 and name not in ids:
                ids.append(name)
        return ids


if not sys._compiled:
    class _TemplatePattern:
        """Template.pattern: the class's regular expression, made when asked."""

        def __get__(self, instance, cls=None):
            if cls is None:
                return self
            return cls._compile_pattern()

    _TemplatePattern = _TemplatePattern()
    Template.pattern = _TemplatePattern

    class Formatter:
        """str.format() as a class whose steps (parse, get_field, format_field ...) can be replaced."""

        def format(self, format_string, /, *args, **kwargs):
            return self.vformat(format_string, args, kwargs)

        def vformat(self, format_string, args, kwargs):
            used_args = set()
            result, _ = self._vformat(format_string, args, kwargs, used_args, 2)
            self.check_unused_args(used_args, args, kwargs)
            return result

        def _vformat(self, format_string, args, kwargs, used_args, recursion_depth, auto_arg_index=0):
            if recursion_depth < 0:
                raise ValueError("Max string recursion exceeded")
            result = []
            for literal_text, field_name, format_spec, conversion in self.parse(format_string):
                if literal_text:
                    result.append(literal_text)
                if field_name is not None:
                    import _string
                    field_first, _ = _string.formatter_field_name_split(field_name)
                    if field_first == "":
                        if auto_arg_index is False:
                            raise ValueError("cannot switch from manual field specification to automatic field numbering")
                        field_name = str(auto_arg_index) + field_name
                        auto_arg_index += 1
                    elif isinstance(field_first, int):
                        if auto_arg_index:
                            raise ValueError("cannot switch from automatic field numbering to manual field specification")
                        auto_arg_index = False
                    obj, arg_used = self.get_field(field_name, args, kwargs)
                    used_args.add(arg_used)
                    obj = self.convert_field(obj, conversion)
                    format_spec, auto_arg_index = self._vformat(format_spec, args, kwargs, used_args,
                                                                recursion_depth - 1, auto_arg_index=auto_arg_index)
                    result.append(self.format_field(obj, format_spec))
            return "".join(result), auto_arg_index

        def get_value(self, key, args, kwargs):
            if isinstance(key, int):
                return args[key]
            return kwargs[key]

        def check_unused_args(self, used_args, args, kwargs):
            pass

        def format_field(self, value, format_spec):
            return format(value, format_spec)

        def convert_field(self, value, conversion):
            if conversion is None:
                return value
            if conversion == "s":
                return str(value)
            if conversion == "r":
                return repr(value)
            if conversion == "a":
                return ascii(value)
            raise ValueError("Unknown conversion specifier " + str(conversion))

        def parse(self, format_string):
            import _string
            return _string.formatter_parser(format_string)

        def get_field(self, field_name, args, kwargs):
            import _string
            first, rest = _string.formatter_field_name_split(field_name)
            obj = self.get_value(first, args, kwargs)
            for is_attr, i in rest:
                if is_attr:
                    obj = getattr(obj, i)
                else:
                    obj = obj[i]
            return obj, first
