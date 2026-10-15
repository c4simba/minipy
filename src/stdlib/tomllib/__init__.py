# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2021 Taneli Hukkinen
# Licensed to PSF under a Contributor Agreement.
"""Parse TOML (CPython's tomllib): loads(text), load(binary file), TOMLDecodeError.

Compiled programs: loads() / load() give the type their result goes to (as json.loads:
cfg: Config = tomllib.loads(text), a dataclass or dict[str, ...]); dates and times are their text."""
import sys
from typing import TypeVar

_F = TypeVar("_F")

__all__ = ("loads", "load", "TOMLDecodeError")

if not sys._compiled:
    from ._parser import TOMLDecodeError, load, loads

    # Pretend this exception was created here.
    TOMLDecodeError.__module__ = __name__

if sys._compiled:
    class TOMLDecodeError(ValueError):
        """An error raised if a document is not valid TOML: msg, doc, pos, lineno, colno."""

        def __init__(self, msg: str, doc: str, pos: int) -> None:
            if pos >= len(doc):
                where = "end of document"
            else:
                lineno = doc.count("\n", 0, pos) + 1
                if lineno == 1:
                    colno = pos + 1
                else:
                    colno = pos - doc.rindex("\n", 0, pos)
                where = "line " + str(lineno) + ", column " + str(colno)
            super().__init__(msg + " (at " + where + ")")
            self.msg = msg
            self.doc = doc
            self.pos = pos
            self.lineno = doc.count("\n", 0, pos) + 1
            self.colno = pos + 1 if self.lineno == 1 else pos - doc.rindex("\n", 0, pos)

    class _N:
        """A node of the document: a table ("t"), an array ("a") or a value ("v": its JSON text)."""

        def __init__(self, kind: str) -> None:
            self.kind = kind
            self.keys: list[str] = []
            self.vals: dict[str, "_N"] = {}
            self.items: list["_N"] = []
            self.json = ""
            self.explicit = False             # [table] declared
            self.frozen = False               # an inline table / array
            self.aot = False                  # an array of tables
            self.dsec = -1                    # made by a dotted key in this [section]

        def to_json(self) -> str:
            if self.kind == "v":
                return self.json
            if self.kind == "a":
                return "[" + ", ".join([x.to_json() for x in self.items]) + "]"
            return "{" + ", ".join([_jstr(k) + ": " + self.vals[k].to_json() for k in self.keys]) + "}"

        def set(self, key: str, node: "_N") -> None:
            if key not in self.vals:
                self.keys.append(key)
            self.vals[key] = node

    def _jstr(s: str) -> str:
        out = '"'
        for ch in s:
            o = ord(ch)
            if ch == '"':
                out += '\\"'
            elif ch == "\\":
                out += "\\\\"
            elif o < 0x20 or o == 0x7F:
                out += "\\u" + f"{o:04x}"
            else:
                out += ch
        return out + '"'

    _BARE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"

    class _Parser:
        def __init__(self, src: str) -> None:
            self.s = src.replace("\r\n", "\n")
            self.i = 0
            self.n = len(self.s)
            self.root = _N("t")
            self.header: list[str] = []
            self.section = 0

        def err(self, msg: str) -> TOMLDecodeError:
            return TOMLDecodeError(msg, self.s, self.i)

        def peek(self) -> str:
            return self.s[self.i] if self.i < self.n else ""

        def ws(self) -> None:
            while self.i < self.n and self.s[self.i] in " \t":
                self.i += 1

        def comment(self) -> None:
            if self.peek() == "#":
                while self.i < self.n and self.s[self.i] != "\n":
                    c = self.s[self.i]
                    if (ord(c) < 0x20 and c != "\t") or c == "\x7f":
                        raise self.err("Illegal character " + repr(c))
                    self.i += 1

        def eol(self) -> None:
            """The end of a line: spaces, a comment, a newline (or the end)."""
            self.ws()
            self.comment()
            if self.i >= self.n:
                return
            if self.s[self.i] == "\n":
                self.i += 1
            else:
                raise self.err("Expected newline or end of document after a statement")

        def blank(self) -> None:
            """Spaces, newlines and comments (in arrays)."""
            while self.i < self.n:
                c = self.s[self.i]
                if c in " \t\n":
                    self.i += 1
                elif c == "#":
                    self.comment()
                else:
                    return

        def parse(self) -> _N:
            cur = self.root
            while True:
                self.blank()
                if self.i >= self.n:
                    return self.root
                c = self.s[self.i]
                if c == "[":
                    if self.s.startswith("[[", self.i):
                        self.i += 2
                        self.ws()
                        keys = self.key()
                        self.ws()
                        self.section += 1
                        cur = self.array_table(keys)
                        if not self.s.startswith("]]", self.i):
                            raise self.err("Expected ']]' at the end of an array declaration")
                        self.i += 2
                    else:
                        self.i += 1
                        self.ws()
                        keys = self.key()
                        self.ws()
                        self.section += 1
                        cur = self.table(keys)
                        if self.peek() != "]":
                            raise self.err("Expected ']' at the end of a table declaration")
                        self.i += 1
                    self.header = keys
                    self.eol()
                    continue
                if c not in _BARE and c not in "\"'":
                    raise self.err("Invalid statement")
                keys = self.key()
                self.ws()
                if self.peek() != "=":
                    raise self.err("Expected '=' after a key in a key/value pair")
                self.i += 1
                self.ws()
                value = self.value()
                self.put(cur, keys, value)
                self.eol()

        def table(self, keys: list[str]) -> _N:
            t = self.root
            twice = "Cannot declare " + repr(tuple(keys)) + " twice"
            for k in keys[:-1]:
                t = self.descend(t, k, twice)
            last = keys[-1]
            if last in t.vals:
                node = t.vals[last]
                if node.kind == "v":
                    raise self.err("Cannot overwrite a value")
                if node.kind != "t" or node.explicit or node.frozen or node.dsec >= 0:
                    raise self.err(twice)
                node.explicit = True
                return node
            node = _N("t")
            node.explicit = True
            t.set(last, node)
            return node

        def array_table(self, keys: list[str]) -> _N:
            t = self.root
            immutable = "Cannot mutate immutable namespace " + repr(tuple(keys))
            for k in keys[:-1]:
                t = self.descend(t, k, immutable)
            last = keys[-1]
            if last in t.vals:
                arr = t.vals[last]
                if arr.frozen or (arr.kind == "t" and arr.dsec >= 0):
                    raise self.err(immutable)
                if arr.kind != "a" or not arr.aot:
                    raise self.err("Cannot overwrite a value")
            else:
                arr = _N("a")
                arr.aot = True
                t.set(last, arr)
            node = _N("t")
            node.explicit = True
            arr.items.append(node)
            return node

        def descend(self, t: _N, k: str, frozen: str) -> _N:
            if k not in t.vals:
                node = _N("t")
                t.set(k, node)
                return node
            node = t.vals[k]
            if node.kind == "a" and node.aot:
                return node.items[-1]
            if node.frozen:
                raise self.err(frozen)
            if node.kind != "t":
                raise self.err("Cannot overwrite a value")
            return node

        def put(self, cur: _N, keys: list[str], value: _N, inline: bool = False) -> None:
            t = cur
            for i in range(len(keys) - 1):
                k = keys[i]
                if k in t.vals:
                    node = t.vals[k]
                    if not inline and (node.explicit or (node.dsec >= 0 and node.dsec != self.section)):
                        raise self.err("Cannot redefine namespace " + repr(tuple(self.header + keys[:i + 1])))
                    if node.frozen:
                        raise self.err("Cannot mutate immutable namespace " + repr(tuple(self.header + keys[:i + 1])))
                    if node.kind != "t":
                        raise self.err("Cannot overwrite a value")
                    t = node
                else:
                    node = _N("t")
                    node.dsec = self.section
                    t.set(k, node)
                    t = node
            last = keys[-1]
            if last in t.vals:
                raise self.err("Cannot overwrite a value")
            t.set(last, value)

        def key(self) -> list[str]:
            keys: list[str] = []
            while True:
                self.ws()
                c = self.peek()
                if c == '"':
                    keys.append(self.basic_string())
                elif c == "'":
                    keys.append(self.literal_string())
                else:
                    start = self.i
                    while self.i < self.n and self.s[self.i] in _BARE:
                        self.i += 1
                    if self.i == start:
                        raise self.err("Invalid initial character for a key part")
                    keys.append(self.s[start:self.i])
                self.ws()
                if self.peek() != ".":
                    return keys
                self.i += 1

        def value(self) -> _N:
            c = self.peek()
            node = _N("v")
            if c == '"':
                if self.s.startswith('"""', self.i):
                    node.json = _jstr(self.ml_basic_string())
                else:
                    node.json = _jstr(self.basic_string())
                return node
            if c == "'":
                if self.s.startswith("'''", self.i):
                    node.json = _jstr(self.ml_literal_string())
                else:
                    node.json = _jstr(self.literal_string())
                return node
            if c == "[":
                return self.array()
            if c == "{":
                return self.inline_table()
            if self.s.startswith("true", self.i):
                self.i += 4
                node.json = "true"
                return node
            if self.s.startswith("false", self.i):
                self.i += 5
                node.json = "false"
                return node
            return self.number_or_date()

        def number_or_date(self) -> _N:
            node = _N("v")
            start = self.i
            s = self.s
            if s.startswith(("inf", "+inf"), self.i) or s.startswith("-inf", self.i):
                neg = s[self.i] == "-"
                self.i += 4 if s[self.i] in "+-" else 3
                node.json = "-Infinity" if neg else "Infinity"
                return node
            if s.startswith(("nan", "+nan", "-nan"), self.i):
                self.i += 4 if s[self.i] in "+-" else 3
                node.json = "NaN"
                return node
            # a date or time: digits then '-' (date) or ':' (time)
            j = self.i
            while j < self.n and s[j].isdigit():
                j += 1
            if j < self.n and ((j - self.i == 4 and s[j] == "-") or (j - self.i == 2 and s[j] == ":")):
                k = self.i
                while k < self.n and (s[k].isdigit() or s[k] in "-:.TtZz+"):
                    k += 1
                if k < self.n - 2 and s[k] == " " and s[k + 1].isdigit() and s[k + 2].isdigit() and j - self.i == 4:
                    k += 1
                    while k < self.n and (s[k].isdigit() or s[k] in "-:.Zz+"):
                        k += 1
                text = s[self.i:k]
                self.i = k
                node.json = _jstr(text)
                return node
            k = self.i
            if k < self.n and s[k] in "+-":
                k += 1
            if s.startswith(("0x", "0o", "0b"), k) and k == self.i:
                base = 16 if s[k + 1] == "x" else 8 if s[k + 1] == "o" else 2
                k += 2
                d0 = k
                while k < self.n and (s[k].isalnum() or s[k] == "_"):
                    k += 1
                digits = s[d0:k].replace("_", "")
                try:
                    v = int(digits, base)
                except ValueError:
                    raise self.err("Invalid value")
                self.i = k
                node.json = str(v)
                return node
            while k < self.n and (s[k].isdigit() or s[k] in "_.eE+-"):
                if s[k] in "+-" and s[k - 1] not in "eE":
                    break
                k += 1
            text = s[start:k]
            if not text or text in ("+", "-"):
                raise self.err("Invalid value")
            clean = text.replace("_", "")
            self.i = k
            if "." in clean or "e" in clean or "E" in clean:
                try:
                    f = float(clean)
                except ValueError:
                    self.i = start
                    raise self.err("Invalid value")
                node.json = repr(f)
            else:
                try:
                    node.json = str(int(clean))
                except ValueError:
                    self.i = start
                    raise self.err("Invalid value")
            return node

        def array(self) -> _N:
            self.i += 1
            node = _N("a")
            node.frozen = True
            while True:
                self.blank()
                if self.peek() == "]":
                    self.i += 1
                    return node
                if self.i >= self.n:
                    raise self.err("Unclosed array")
                node.items.append(self.value())
                self.blank()
                c = self.peek()
                if c == ",":
                    self.i += 1
                elif c == "]":
                    self.i += 1
                    return node
                else:
                    raise self.err("Unclosed array")

        def inline_table(self) -> _N:
            self.i += 1
            node = _N("t")
            node.frozen = True
            self.ws()
            if self.peek() == "}":
                self.i += 1
                return node
            while True:
                keys = self.key()
                self.ws()
                if self.peek() != "=":
                    raise self.err("Expected '=' after a key in a key/value pair")
                self.i += 1
                self.ws()
                v = self.value()
                node.frozen = False
                self.put(node, keys, v, True)
                node.frozen = True
                self.ws()
                c = self.peek()
                if c == "}":
                    self.i += 1
                    return node
                if c != ",":
                    raise self.err("Unclosed inline table")
                self.i += 1
                self.ws()

        def basic_string(self) -> str:
            self.i += 1
            out = ""
            while True:
                if self.i >= self.n:
                    raise self.err('Expected "' + '"' + '"')
                c = self.s[self.i]
                if c == '"':
                    self.i += 1
                    return out
                if c == "\n":
                    raise self.err("Illegal character " + repr(c))
                if c == "\\":
                    out += self.escape()
                    continue
                out += c
                self.i += 1

        def escape(self) -> str:
            self.i += 1
            c = self.peek()
            simple = {"b": "\b", "t": "\t", "n": "\n", "f": "\f", "r": "\r", '"': '"', "\\": "\\"}
            if c in simple:
                self.i += 1
                return simple[c]
            if c == "u" or c == "U":
                k = 4 if c == "u" else 8
                self.i += 1
                hx = self.s[self.i:self.i + k]
                if len(hx) != k or any(ch not in "0123456789abcdefABCDEF" for ch in hx):
                    raise self.err("Invalid hex value")
                self.i += k
                cp = int(hx, 16)
                if cp > 0x10FFFF or 0xD800 <= cp <= 0xDFFF:
                    raise self.err("Escaped character is not a Unicode scalar value")
                return chr(cp)
            self.i += 1
            raise self.err("Unescaped '\\' in a string")

        def literal_string(self) -> str:
            self.i += 1
            j = self.s.find("'", self.i)
            nl = self.s.find("\n", self.i)
            if j < 0 or (0 <= nl < j):
                self.i = self.n if j < 0 else nl
                raise self.err('Expected "' + "'" + '"')
            out = self.s[self.i:j]
            self.i = j + 1
            return out

        def ml_basic_string(self) -> str:
            self.i += 3
            if self.s.startswith("\r\n", self.i):
                self.i += 2
            elif self.peek() == "\n":
                self.i += 1
            out = ""
            while True:
                if self.i >= self.n:
                    raise self.err('Expected """')
                if self.s.startswith('"""', self.i):
                    extra = 0
                    while self.s.startswith('"', self.i + 3 + extra) and extra < 2:
                        extra += 1
                    out += '"' * extra
                    self.i += 3 + extra
                    return out
                c = self.s[self.i]
                if c == "\\":
                    k = self.i + 1
                    while k < self.n and self.s[k] in " \t":
                        k += 1
                    if k < self.n and self.s[k] in "\r\n":
                        while k < self.n and self.s[k] in " \t\r\n":
                            k += 1
                        self.i = k
                        continue
                    out += self.escape()
                    continue
                out += c
                self.i += 1

        def ml_literal_string(self) -> str:
            self.i += 3
            if self.s.startswith("\r\n", self.i):
                self.i += 2
            elif self.peek() == "\n":
                self.i += 1
            j = self.s.find("'''", self.i)
            if j < 0:
                self.i = self.n
                raise self.err("Expected '''")
            extra = 0
            while self.s.startswith("'", j + 3 + extra) and extra < 2:
                extra += 1
            out = self.s[self.i:j + extra]
            self.i = j + 3 + extra
            return out

    def loads(s: str) -> dict[str, str]:
        """(a call is json.loads(_to_json(s)) for the type its value goes to)"""
        raise NotImplementedError("tomllib.loads as a value")

    def load(fp: _F) -> dict[str, str]:
        raise NotImplementedError("tomllib.load as a value")

    def _to_json(s: str) -> str:
        """A TOML document as JSON text (dates and times: their text)."""
        return _Parser(s).parse().to_json()

    def _load_text(fp: _F) -> str:
        b = fp.read()
        if isinstance(b, str):
            raise TypeError("File must be opened in binary mode, e.g. use `open('foo.toml', 'rb')`. "
                            "Text mode is not supported")
        else:
            return b.decode("utf-8")
