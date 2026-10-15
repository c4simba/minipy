"""json.loads() in compiled programs: the compiler writes a decoder for the type the result goes
to (x: dict[str, int] = json.loads(s)), over this reader. Errors are CPython's JSONDecodeError
messages; a value of another kind than the type wants: TypeError."""
import sys


class JSONDecodeError(ValueError):
    """Subclass of ValueError with: msg, doc, pos, lineno, colno."""

    def __init__(self, msg: str, doc: str, pos: int) -> None:
        lineno = doc.count("\n", 0, pos) + 1
        colno = pos - doc.rfind("\n", 0, pos)
        super().__init__(msg + ": line " + str(lineno) + " column " + str(colno) + " (char " + str(pos) + ")")
        self.msg = msg
        self.doc = doc
        self.pos = pos
        self.lineno = lineno
        self.colno = colno


_ESC = {'"': '"', "\\": "\\", "/": "/", "b": "\b", "f": "\f", "n": "\n", "r": "\r", "t": "\t"}
_KINDS = {'"': "a string", "{": "an object", "[": "an array", "t": "true", "f": "false", "n": "null"}


class Reader:
    """A JSON text read value by value, as the decoder asks."""

    def __init__(self, s: str) -> None:
        self.s = s
        self.i = 0
        self.n = len(s)
        self.key = ""
        self._first: list[bool] = []

    def _ws(self) -> None:
        s = self.s
        i = self.i
        n = self.n
        while i < n and (s[i] == " " or s[i] == "\t" or s[i] == "\n" or s[i] == "\r"):
            i += 1
        self.i = i

    def _value_start(self) -> str:
        self._ws()
        if self.i >= self.n:
            raise JSONDecodeError("Expecting value", self.s, self.i)
        return self.s[self.i]

    def _check_all(self) -> None:
        """A JSONDecodeError if the text is not JSON (it comes before what the type finds wrong)."""
        r = Reader(self.s)
        r.skip()
        r.end()

    def type_error(self, msg: str) -> TypeError:
        self._check_all()
        return TypeError(msg)

    def _wrong(self, want: str) -> TypeError:
        c = self.s[self.i]
        k = _KINDS.get(c)
        if k is None:
            k = "a number"
        return self.type_error("JSON value at char " + str(self.i) + " is " + k + ", not " + want)

    def end(self) -> None:
        self._ws()
        if self.i != self.n:
            raise JSONDecodeError("Extra data", self.s, self.i)

    def null(self) -> bool:
        """A null here (taken)?"""
        c = self._value_start()
        if c == "n" and self.s.startswith("null", self.i):
            self.i += 4
            return True
        return False

    def read_bool(self) -> bool:
        c = self._value_start()
        if c == "t" and self.s.startswith("true", self.i):
            self.i += 4
            return True
        if c == "f" and self.s.startswith("false", self.i):
            self.i += 5
            return False
        self._literal_or_fail()
        raise self._wrong("a bool")

    def _literal_or_fail(self) -> None:
        """A valid JSON value starts here (else the JSONDecodeError)."""
        c = self.s[self.i]
        if c in '"{[' or c == "-" or ("0" <= c <= "9"):
            return
        for w in ("true", "false", "null", "NaN", "Infinity"):
            if self.s.startswith(w, self.i):
                return
        raise JSONDecodeError("Expecting value", self.s, self.i)

    def _number(self) -> tuple[str, bool]:
        s = self.s
        start = self.i
        i = start
        n = self.n
        if i < n and s[i] == "-":
            i += 1
        if s.startswith("Infinity", i):
            self.i = i + 8
            return s[start:self.i], True
        if i >= n or not ("0" <= s[i] <= "9"):
            if s.startswith("NaN", start):
                self.i = start + 3
                return "NaN", True
            raise JSONDecodeError("Expecting value", s, start)
        if s[i] == "0":
            i += 1
        else:
            while i < n and "0" <= s[i] <= "9":
                i += 1
        is_float = False
        if i + 1 < n and s[i] == "." and "0" <= s[i + 1] <= "9":
            i += 2
            while i < n and "0" <= s[i] <= "9":
                i += 1
            is_float = True
        if i < n and (s[i] == "e" or s[i] == "E"):
            j = i + 1
            if j < n and (s[j] == "+" or s[j] == "-"):
                j += 1
            if j < n and "0" <= s[j] <= "9":
                while j < n and "0" <= s[j] <= "9":
                    j += 1
                i = j
                is_float = True
        self.i = i
        return s[start:i], is_float

    def read_int(self) -> int:
        c = self._value_start()
        if c == "-" or ("0" <= c <= "9"):
            start = self.i
            text, is_float = self._number()
            if is_float:
                self.i = start
                raise self.type_error("JSON value at char " + str(start) + " is a float, not an int")
            return int(text)
        self._literal_or_fail()
        raise self._wrong("an int")

    def read_float(self) -> float:
        c = self._value_start()
        if c == "-" or ("0" <= c <= "9") or c == "N" or c == "I":
            text, is_float = self._number()
            if text == "NaN":
                return float("nan")
            if text == "Infinity":
                return float("inf")
            if text == "-Infinity":
                return float("-inf")
            return float(text)
        self._literal_or_fail()
        raise self._wrong("a float")

    def read_str(self) -> str:
        c = self._value_start()
        if c != '"':
            self._literal_or_fail()
            raise self._wrong("a string")
        return self._string()

    def read_bytes(self) -> bytes:
        """(pickle.loads' reader: bytes; JSON has none)"""
        raise TypeError("bytes cannot be read from JSON")

    def _string(self) -> str:
        s = self.s
        begin = self.i
        i = begin + 1
        n = self.n
        out = ""
        chunk = i
        while True:
            if i >= n:
                raise JSONDecodeError("Unterminated string starting at", s, begin)
            c = s[i]
            if c == '"':
                out += s[chunk:i]
                self.i = i + 1
                return out
            if c == "\\":
                out += s[chunk:i]
                if i + 1 >= n:
                    raise JSONDecodeError("Unterminated string starting at", s, begin)
                e = s[i + 1]
                if e == "u":
                    code = self._hex4(i + 2, i)
                    i += 6
                    if 0xD800 <= code <= 0xDBFF and s[i:i + 2] == "\\u":
                        low = self._hex4(i + 2, i)
                        if 0xDC00 <= low <= 0xDFFF:
                            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00)
                            i += 6
                    out += chr(code)
                else:
                    r = _ESC.get(e)
                    if r is None:
                        raise JSONDecodeError("Invalid \\escape", s, i)
                    out += r
                    i += 2
                chunk = i
                continue
            if ord(c) < 0x20:
                raise JSONDecodeError("Invalid control character at", s, i)
            i += 1

    def _hex4(self, at: int, esc: int) -> int:
        h = self.s[at:at + 4]
        ok = len(h) == 4
        for ch in h:
            if not ("0" <= ch <= "9" or "a" <= ch <= "f" or "A" <= ch <= "F"):
                ok = False
        if not ok:
            raise JSONDecodeError("Invalid \\uXXXX escape", self.s, esc)
        return int(h, 16)

    def begin_obj(self) -> None:
        c = self._value_start()
        if c != "{":
            self._literal_or_fail()
            raise self._wrong("an object")
        self.i += 1
        self._first.append(True)

    def next_key(self) -> bool:
        """The next member's key in self.key (False: the object ended)."""
        self._ws()
        s = self.s
        first = self._first[-1]
        if first and self.i < self.n and s[self.i] == "}":
            self.i += 1
            self._first.pop()
            return False
        if not first:
            if self.i < self.n and s[self.i] == "}":
                self.i += 1
                self._first.pop()
                return False
            if self.i >= self.n or s[self.i] != ",":
                raise JSONDecodeError("Expecting ',' delimiter", s, self.i)
            self.i += 1
            self._ws()
            if self.i < self.n and s[self.i] == "}":
                raise JSONDecodeError("Illegal trailing comma before end of object", s, self.i - 1)
        self._first[-1] = False
        if self.i >= self.n or s[self.i] != '"':
            raise JSONDecodeError("Expecting property name enclosed in double quotes", s, self.i)
        self.key = self._string()
        self._ws()
        if self.i >= self.n or s[self.i] != ":":
            raise JSONDecodeError("Expecting ':' delimiter", s, self.i)
        self.i += 1
        return True

    def begin_list(self) -> None:
        c = self._value_start()
        if c != "[":
            self._literal_or_fail()
            raise self._wrong("an array")
        self.i += 1
        self._first.append(True)

    def next_item(self) -> bool:
        """Another item follows (False: the array ended)."""
        self._ws()
        s = self.s
        first = self._first[-1]
        if self.i < self.n and s[self.i] == "]":
            self.i += 1
            self._first.pop()
            return False
        if not first:
            if self.i >= self.n or s[self.i] != ",":
                raise JSONDecodeError("Expecting ',' delimiter", s, self.i)
            self.i += 1
            self._ws()
            if self.i < self.n and s[self.i] == "]":
                raise JSONDecodeError("Illegal trailing comma before end of array", s, self.i - 1)
        self._first[-1] = False
        return True

    def skip(self) -> None:
        """Passes over a value of any kind."""
        c = self._value_start()
        if c == "{":
            self.begin_obj()
            while self.next_key():
                self.skip()
        elif c == "[":
            self.begin_list()
            while self.next_item():
                self.skip()
        elif c == '"':
            self._string()
        elif c == "t" or c == "f":
            self.read_bool()
        elif c == "n":
            if not self.null():
                raise JSONDecodeError("Expecting value", self.s, self.i)
        else:
            self._number()

    def missing(self, key: str, cls: str) -> KeyError:
        self._check_all()
        return KeyError(key)
