"""The string module's helpers (CPython's _string): the parts of a str.format() string."""
import sys

if not sys._compiled:                                  # (string.Formatter: the interpreter's)
    def formatter_parser(format_string: str):
        """(literal_text, field_name, format_spec, conversion) for each part of format_string
        (field_name, format_spec and conversion None where there is no field)."""
        s = format_string
        n = len(s)
        i = 0
        while i < n:
            start = i
            c = ""
            markup = False
            while i < n:
                c = s[i]
                i += 1
                if c == "{" or c == "}":
                    markup = True
                    break
            at_end = i >= n
            length = i - start
            if c == "}" and markup and (at_end or s[i] != "}"):
                raise ValueError("Single '}' encountered in format string")
            if at_end and markup and c == "{":
                raise ValueError("Single '{' encountered in format string")
            if not at_end and markup:
                if s[i] == c:                                  # {{ or }}: one of it, no field
                    i += 1
                    markup = False
                else:
                    length -= 1
            literal = s[start:start + length]
            if not markup:
                yield literal, None, None, None
                continue
            # the field: its name up to the end, ':' or '!' (not inside [...])
            fstart = i
            c = ""
            while i < n:
                c = s[i]
                i += 1
                if c == "{":
                    raise ValueError("unexpected '{' in field name")
                if c == "[":
                    while i < n and s[i] != "]":
                        i += 1
                    continue
                if c == "}" or c == ":" or c == "!":
                    break
            field_name = s[fstart:i - 1] if c in "}:!" and c != "" else s[fstart:i]
            conversion = None
            format_spec = ""
            if c == "!" or c == ":":
                if c == "!":
                    if i >= n:
                        raise ValueError("end of string while looking for conversion specifier")
                    conversion = s[i]
                    i += 1
                    if i < n:
                        c = s[i]
                        i += 1
                        if c == "}":
                            yield literal, field_name, "", conversion
                            continue
                        if c != ":":
                            raise ValueError("expected ':' after conversion specifier")
                sstart = i
                count = 1
                done = False
                while i < n:
                    c = s[i]
                    i += 1
                    if c == "{":
                        count += 1
                    elif c == "}":
                        count -= 1
                        if count == 0:
                            format_spec = s[sstart:i - 1]
                            done = True
                            break
                if not done:
                    raise ValueError("unmatched '{' in format spec")
            elif c != "}":
                raise ValueError("expected '}' before end of string")
            yield literal, field_name, format_spec, conversion


    def _integer(s: str):
        if s and s.isdecimal():
            return int(s)
        return None


    def formatter_field_name_split(field_name: str):
        """(first, the rest as (is_attribute, key) pairs): 'a.b[0]' -> ('a', [(True, 'b'), (False, 0)])."""
        s = field_name
        n = len(s)
        i = 0
        while i < n and s[i] != "." and s[i] != "[":
            i += 1
        first = s[:i]
        idx = _integer(first)
        rest = []
        while i < n:
            c = s[i]
            i += 1
            if c == ".":
                j = i
                while i < n and s[i] != "." and s[i] != "[":
                    i += 1
                name = s[j:i]
                if not name:
                    raise ValueError("Empty attribute in format string")
                rest.append((True, name))
            elif c == "[":
                j = i
                while i < n and s[i] != "]":
                    i += 1
                if i >= n:
                    raise ValueError("Missing ']' in format string")
                name = s[j:i]
                i += 1
                if not name:
                    raise ValueError("Empty attribute in format string")
                k = _integer(name)
                rest.append((False, k if k is not None else name))
            else:
                raise ValueError("Only '.' or '[' may follow ']' in format field specifier")
        return (idx if idx is not None else first), iter(rest)
