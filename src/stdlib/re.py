"""Regular expressions (CPython's re) for str patterns: a backtracking matcher.

The syntax is CPython's: . ^ $ * + ? {m,n} (with lazy ? and possessive + forms),
[...] sets, \\d \\w \\s \\b \\A \\z escapes, (...) (?:...) (?P<name>...) (?P=name)
(?=...) (?!...) (?<=...) (?<!...) (?>...) (?(1)yes|no) (?#...) and inline flags.
A pattern is parsed into _Node trees, then compiled to a list of int instructions
that Pattern._run executes with an explicit backtracking stack.

In compiled programs: patterns are str only; findall gives a list of str (patterns
with at most one group); flags are plain ints."""
import sys
from typing import TypeVar

_G = TypeVar("_G")
_P = TypeVar("_P")
_R = TypeVar("_R")

__all__ = ["match", "fullmatch", "search", "sub", "subn", "split", "findall", "finditer", "compile", "purge",
           "escape", "error", "Pattern", "Match", "A", "I", "L", "M", "S", "X", "U", "ASCII", "IGNORECASE",
           "LOCALE", "MULTILINE", "DOTALL", "VERBOSE", "UNICODE", "NOFLAG", "PatternError"]

NOFLAG = 0
IGNORECASE = 2
I = 2
LOCALE = 4
L = 4
MULTILINE = 8
M = 8
DOTALL = 16
S = 16
UNICODE = 32
U = 32
VERBOSE = 64
X = 64
DEBUG = 128
ASCII = 256
A = 256

_MAXSIZE = 9223372036854775807
_MAXREPEAT = 4294967295
_MAXGROUPS = 1073741823

_DIGITS = "0123456789"
_OCTDIGITS = "01234567"
_HEXDIGITS = "0123456789abcdefABCDEF"
_ASCIILETTERS = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
_WHITESPACE = " \t\n\r\x0b\x0c"
_SPECIAL = ".\\[{()*+?^$|"
_FLAGCHARS = "iLmsxau"
_FLAGVALS = [2, 4, 8, 16, 64, 256, 32]

# node kinds
_N_SEQ = 0
_N_ALT = 1
_N_LIT = 2
_N_NLIT = 3
_N_ANY = 4
_N_SET = 5
_N_GROUP = 6
_N_REP = 7
_N_AT = 8
_N_BREF = 9
_N_LOOK = 10
_N_ATOMIC = 11
_N_COND = 12
_N_FAIL = 13

# instructions (with their operands after them in Pattern._code)
_MATCH = 0          # success (subject to the run's mode)
_CHAR = 1           # c
_CHARI = 2          # folded c, ascii
_NCHAR = 3          # c
_NCHARI = 4         # folded c, ascii
_ANY = 5
_ANYALL = 6
_SET = 7            # set index, mode (1: ignore case, 2: ascii)
_SPLIT = 8          # first, second: try first, then (backtracking) second
_JMP = 9            # target
_SAVE = 10          # register: it gets the position
_CLOSE = 11         # group: its end register gets the position, lastindex the group
_CHECK = 12         # mark register, exit: leave a loop whose body matched nothing
_AT = 13            # assertion kind
_BREF = 14          # group, ignore case
_LOOK = 15          # kind (0 =, 1 !, 2 <=, 3 <!), width, end; the body follows, ended by _MATCH
_ATOMIC = 16        # end; the body follows, ended by _MATCH
_COND = 17          # group, the pc of the no branch
_FAIL = 18
_REP1 = 19          # min, max (-1: no limit), possessive, width; a one-character instruction follows: as many as
                    # it matches, then (backtracking) one fewer each time

# assertions
_AT_BEGIN = 0
_AT_BEGIN_LINE = 1
_AT_END = 2
_AT_END_LINE = 3
_AT_END_STRING = 4
_AT_BOUNDARY = 5
_AT_NON_BOUNDARY = 6

# categories of sets: their negations are the odd numbers
_C_DIGIT = 0
_C_SPACE = 2
_C_WORD = 4


class error(Exception):
    """A pattern (or replacement template) that is not valid: msg, pattern, pos, lineno, colno."""

    def __init__(self, msg: str, pattern: str | None = None, pos: int | None = None):
        self.msg = msg
        self.pattern = pattern
        self.pos = pos
        self.lineno: int | None = None
        self.colno: int | None = None
        text = msg
        if pattern is not None and pos is not None:
            text = msg + " at position " + str(pos)
            self.lineno = pattern.count("\n", 0, pos) + 1
            self.colno = pos - pattern.rfind("\n", 0, pos)
            if "\n" in pattern:
                text = text + " (line " + str(self.lineno) + ", column " + str(self.colno) + ")"
        super().__init__(text)


PatternError = error


def _fold(ch: int) -> int:
    """ch's case-folded code point (IGNORECASE compares these)."""
    if ch < 128:
        if 65 <= ch <= 90:
            return ch + 32
        return ch
    c = chr(ch)
    u = c.upper()
    if len(u) != 1:
        u = c
    lo = u.lower()
    if len(lo) != 1:
        return ch
    return ord(lo)


def _fold_ascii(ch: int) -> int:
    if 65 <= ch <= 90:
        return ch + 32
    return ch


def _is_word(ch: int, ascii: bool) -> bool:
    if ch < 128:
        return 97 <= ch <= 122 or 65 <= ch <= 90 or 48 <= ch <= 57 or ch == 95
    if ascii:
        return False
    return chr(ch).isalnum()


def _in_cat(cat: int, ch: int, ascii: bool) -> bool:
    k = cat // 2 * 2
    r = False
    if k == _C_DIGIT:
        if ch < 128:
            r = 48 <= ch <= 57
        elif not ascii:
            r = chr(ch).isdecimal()
    elif k == _C_SPACE:
        if ch < 128:
            r = 9 <= ch <= 13 or ch == 32 or (not ascii and 28 <= ch <= 31)
        elif not ascii:
            r = chr(ch).isspace()
    else:
        r = _is_word(ch, ascii)
    if cat % 2:
        return not r
    return r


class _Set:
    """A character set: ranges (lo, hi pairs of code points) and categories, maybe negated."""

    def __init__(self):
        self.ranges: list[int] = []
        self.cats: list[int] = []
        self.neg = False
        self._maps: list[list[bool]] = [[], [], [], []]     # per mode: the answers for the ASCII characters

    def _has(self, ch: int, ascii: bool) -> bool:
        rs = self.ranges
        i = 0
        while i < len(rs):
            if rs[i] <= ch <= rs[i + 1]:
                return True
            i += 2
        for cat in self.cats:
            if _in_cat(cat, ch, ascii):
                return True
        return False

    def contains(self, ch: int, mode: int) -> bool:
        if ch < 128:
            mp = self._maps[mode]
            if not mp:
                for c in range(128):
                    mp.append(self._contains(c, mode))
            return mp[ch]
        return self._contains(ch, mode)

    def _contains(self, ch: int, mode: int) -> bool:
        ascii = mode & 2 != 0
        r = self._has(ch, ascii)
        if not r and mode & 1:
            if ascii:
                if 65 <= ch <= 90:
                    r = self._has(ch + 32, True)
                elif 97 <= ch <= 122:
                    r = self._has(ch - 32, True)
            else:
                c = chr(ch)
                lo = c.lower()
                up = c.upper()
                if len(lo) == 1 and ord(lo) != ch:
                    r = self._has(ord(lo), False)
                if not r and len(up) == 1 and ord(up) != ch:
                    r = self._has(ord(up), False)
                    if not r:
                        lo2 = up.lower()
                        if len(lo2) == 1 and ord(lo2) != ch:
                            r = self._has(ord(lo2), False)
        return r != self.neg


class _Node:
    """A parsed piece of a pattern: its kind (_N_...) and operands."""

    def __init__(self, kind: int, c: int, flags: int):
        self.kind = kind
        self.c = c                  # LIT/NLIT: code point; SET: set index; GROUP/BREF/COND: group; AT: kind; LOOK: kind
        self.flags = flags          # the flags in force
        self.lo = 0                 # REP: min
        self.hi = 0                 # REP: max (-1: no limit)
        self.mode = 0               # REP: 0 greedy, 1 lazy, 2 possessive
        self.items: list[_Node] = []


def _isident(name: str) -> bool:
    if not name:
        return False
    for i in range(len(name)):
        c = name[i]
        if c == "_" or c.isalpha() or (i > 0 and c.isdigit()) or ord(c) >= 128:
            continue
        return False
    return True


class _Parser:
    """Turns a pattern (or a replacement template) into _Nodes; its tokens are characters and backslash pairs."""

    def __init__(self, string: str, flags: int):
        self.s = string
        self.index = 0
        self.next = ""                  # the next token ("": the end)
        self.flags = flags
        self.groups = 1
        self.groupdict: dict[str, int] = {}
        self.closed: list[bool] = [True]
        self.gmin: list[int] = [0]
        self.gmax: list[int] = [0]
        self.lookbehind = -1            # the number of groups when the lookbehind being parsed began
        self.grouprefpos: dict[int, int] = {}
        self.sets: list[_Set] = []
        self._advance()

    def _advance(self) -> None:
        i = self.index
        if i >= len(self.s):
            self.next = ""
            return
        c = self.s[i]
        if c == "\\":
            i += 1
            if i >= len(self.s):
                raise error("bad escape (end of pattern)", self.s, len(self.s) - 1)
            c = c + self.s[i]
        self.index = i + 1
        self.next = c

    def match(self, c: str) -> bool:
        if self.next == c and c:
            self._advance()
            return True
        return False

    def get(self) -> str:
        this = self.next
        self._advance()
        return this

    def next_in(self, chars: str) -> bool:
        return len(self.next) == 1 and self.next in chars

    def tell(self) -> int:
        return self.index - len(self.next)

    def seek(self, index: int) -> None:
        self.index = index
        self._advance()

    def error(self, msg: str, offset: int = 0) -> error:
        return error(msg, self.s, self.tell() - offset)

    def getwhile(self, n: int, chars: str) -> str:
        r = ""
        for _ in range(n):
            if not self.next_in(chars):
                break
            r += self.next
            self._advance()
        return r

    def getuntil(self, terminator: str, name: str) -> str:
        result = ""
        while True:
            c = self.next
            self._advance()
            if c == "":
                if not result:
                    raise self.error("missing " + name)
                raise self.error("missing " + terminator + ", unterminated name", len(result))
            if c == terminator:
                if not result:
                    raise self.error("missing " + name, 1)
                break
            result += c
        return result

    def checkgroupname(self, name: str, offset: int) -> None:
        if not _isident(name):
            raise self.error("bad character in group name " + repr(name), len(name) + offset)

    def checkgroup(self, gid: int) -> bool:
        return gid < self.groups and self.closed[gid]

    def checklookbehindgroup(self, gid: int) -> None:
        if self.lookbehind >= 0:
            if not self.checkgroup(gid):
                raise self.error("cannot refer to an open group")
            if gid >= self.lookbehind:
                raise self.error("cannot refer to group defined in the same lookbehind subpattern")

    def node(self, kind: int, c: int, add: int, dl: int) -> _Node:
        return _Node(kind, c, (self.flags | add) & ~dl)

    # -- the pattern

    def parse(self) -> _Node:
        root = self.parse_sub(0, 0, self.flags & VERBOSE != 0, 0)
        if self.flags & LOCALE:
            raise ValueError("cannot use LOCALE flag with a str pattern")
        if not self.flags & ASCII:
            self.flags |= UNICODE
        elif self.flags & UNICODE:
            raise ValueError("ASCII and UNICODE flags are incompatible")
        if self.next != "":
            raise self.error("unbalanced parenthesis")
        for g in self.grouprefpos:
            if g >= self.groups:
                raise error("invalid group reference " + str(g), self.s, self.grouprefpos[g])
        return root

    def parse_sub(self, add: int, dl: int, verbose: bool, nested: int) -> _Node:
        """An alternation a|b|c."""
        items: list[_Node] = []
        while True:
            items.append(self.parse_seq(add, dl, verbose, nested + 1, nested == 0 and not items))
            if not self.match("|"):
                break
            if nested == 0:
                verbose = self.flags & VERBOSE != 0
        if len(items) == 1:
            return items[0]
        alt = _Node(_N_ALT, 0, 0)
        alt.items = items
        return alt

    def set_node(self, st: _Set, add: int, dl: int) -> _Node:
        self.sets.append(st)
        return self.node(_N_SET, len(self.sets) - 1, add, dl)

    def cat_node(self, cat: int, add: int, dl: int) -> _Node:
        st = _Set()
        st.cats.append(cat)
        return self.set_node(st, add, dl)

    def class_escape(self, esc: str) -> int:
        """An escape in a set: a code point, or -1 - a category."""
        c = esc[1]
        k = "afnrtvb\\".find(c)
        if k >= 0:
            return [7, 12, 10, 13, 9, 11, 8, 92][k]
        k = "dDsSwW".find(c)
        if k >= 0:
            return -1 - [_C_DIGIT, _C_DIGIT + 1, _C_SPACE, _C_SPACE + 1, _C_WORD, _C_WORD + 1][k]
        return self.code_escape(esc, True)

    def code_escape(self, esc: str, in_class: bool) -> int:
        """\\x.. \\u.... \\U........ octal escapes and escaped punctuation (the code point)."""
        c = esc[1]
        if c == "x":
            esc += self.getwhile(2, _HEXDIGITS)
            if len(esc) != 4:
                raise self.error("incomplete escape " + esc, len(esc))
            return int(esc[2:], 16)
        if c == "u":
            esc += self.getwhile(4, _HEXDIGITS)
            if len(esc) != 6:
                raise self.error("incomplete escape " + esc, len(esc))
            return int(esc[2:], 16)
        if c == "U":
            esc += self.getwhile(8, _HEXDIGITS)
            if len(esc) != 10:
                raise self.error("incomplete escape " + esc, len(esc))
            v = int(esc[2:], 16)
            if v > 0x10FFFF:
                raise self.error("bad escape " + esc, len(esc))
            return v
        if in_class and c in _OCTDIGITS:
            esc += self.getwhile(2, _OCTDIGITS)
            v = int(esc[1:], 8)
            if v > 0o377:
                raise self.error("octal escape value " + esc + " outside of range 0-0o377", len(esc))
            return v
        if c in _DIGITS or c in _ASCIILETTERS:
            raise self.error("bad escape " + esc, len(esc))
        return ord(c)

    def escape(self, esc: str, add: int, dl: int) -> _Node:
        """An escape outside sets."""
        c = esc[1]
        k = "AbBzZ".find(c)
        if k >= 0:
            return self.node(_N_AT, [_AT_BEGIN, _AT_BOUNDARY, _AT_NON_BOUNDARY, _AT_END_STRING, _AT_END_STRING][k], add, dl)
        k = "dDsSwW".find(c)
        if k >= 0:
            return self.cat_node([_C_DIGIT, _C_DIGIT + 1, _C_SPACE, _C_SPACE + 1, _C_WORD, _C_WORD + 1][k], add, dl)
        k = "afnrtv\\".find(c)
        if k >= 0:
            return self.node(_N_LIT, [7, 12, 10, 13, 9, 11, 92][k], add, dl)
        if c == "0":
            esc += self.getwhile(2, _OCTDIGITS)
            return self.node(_N_LIT, int(esc[1:], 8), add, dl)
        if c in _DIGITS:
            if self.next_in(_DIGITS):
                esc += self.get()
                if esc[1] in _OCTDIGITS and esc[2] in _OCTDIGITS and self.next_in(_OCTDIGITS):
                    esc += self.get()
                    v = int(esc[1:], 8)
                    if v > 0o377:
                        raise self.error("octal escape value " + esc + " outside of range 0-0o377", len(esc))
                    return self.node(_N_LIT, v, add, dl)
            group = int(esc[1:])
            if group < self.groups:
                if not self.checkgroup(group):
                    raise self.error("cannot refer to an open group", len(esc))
                self.checklookbehindgroup(group)
                return self.node(_N_BREF, group, add, dl)
            raise self.error("invalid group reference " + str(group), len(esc) - 1)
        return self.node(_N_LIT, self.code_escape(esc, False), add, dl)

    def parse_set(self, add: int, dl: int) -> _Node:
        here = self.tell() - 1
        st = _Set()
        lits: list[int] = []            # the single characters (a set of one is a literal)
        n = 0
        st.neg = self.match("^")
        while True:
            this = self.get()
            if this == "":
                raise self.error("unterminated character set", self.tell() - here)
            if this == "]" and n:
                break
            code1 = 0
            if this[0] == "\\":
                code1 = self.class_escape(this)
            else:
                code1 = ord(this)
            n += 1
            if self.match("-"):
                that = self.get()
                if that == "":
                    raise self.error("unterminated character set", self.tell() - here)
                if that == "]":
                    if code1 < 0:
                        st.cats.append(-1 - code1)
                        lits.append(-1)
                    else:
                        lits.append(code1)
                    lits.append(45)
                    break
                code2 = 0
                if that[0] == "\\":
                    code2 = self.class_escape(that)
                else:
                    code2 = ord(that)
                if code1 < 0 or code2 < 0 or code2 < code1:
                    raise self.error("bad character range " + this + "-" + that, len(this) + 1 + len(that))
                st.ranges.append(code1)
                st.ranges.append(code2)
                lits.append(-1)
            elif code1 < 0:
                st.cats.append(-1 - code1)
                lits.append(-1)
            else:
                lits.append(code1)
        single = -1
        for v in lits:
            if v < 0 or (single >= 0 and v != single):
                single = -2
                break
            single = v
        if single >= 0:
            return self.node(_N_NLIT if st.neg else _N_LIT, single, add, dl)
        for v in lits:
            if v >= 0:
                st.ranges.append(v)
                st.ranges.append(v)
        return self.set_node(st, add, dl)

    def parse_flags(self, char: str) -> list[int]:
        """Inline flags after (?: [add, del], or [] for global flags."""
        add = 0
        dl = 0
        if char != "-":
            while True:
                flag = _FLAGVALS[_FLAGCHARS.find(char)]
                if char == "L":
                    raise self.error("bad inline flags: cannot use 'L' flag with a str pattern")
                add |= flag
                if (flag & 292) and (add & 292) != flag:
                    raise self.error("bad inline flags: flags 'a', 'u' and 'L' are incompatible")
                char = self.get()
                if char == "":
                    raise self.error("missing -, : or )")
                if char in ")-:":
                    break
                if not (len(char) == 1 and char in _FLAGCHARS):
                    raise self.error("unknown flag" if char.isalpha() else "missing -, : or )", len(char))
        if char == ")":
            self.flags |= add
            return []
        if add & DEBUG:
            raise self.error("bad inline flags: cannot turn on global flag", 1)
        if char == "-":
            char = self.get()
            if char == "":
                raise self.error("missing flag")
            if not (len(char) == 1 and char in _FLAGCHARS):
                raise self.error("unknown flag" if char.isalpha() else "missing flag", len(char))
            while True:
                flag = _FLAGVALS[_FLAGCHARS.find(char)]
                if flag & 292:
                    raise self.error("bad inline flags: cannot turn off flags 'a', 'u' and 'L'")
                dl |= flag
                char = self.get()
                if char == "":
                    raise self.error("missing :")
                if char == ":":
                    break
                if not (len(char) == 1 and char in _FLAGCHARS):
                    raise self.error("unknown flag" if char.isalpha() else "missing :", len(char))
        if dl & DEBUG:
            raise self.error("bad inline flags: cannot turn off global flag", 1)
        if add & dl:
            raise self.error("bad inline flags: flag turned on and off", 1)
        return [add, dl]

    def parse_seq(self, add: int, dl: int, verbose: bool, nested: int, first: bool) -> _Node:
        """A sequence of items up to | or ) or the end."""
        seq = _Node(_N_SEQ, 0, 0)
        items = seq.items
        last_rep = False                    # the last item is a repeat (another one is an error)
        while True:
            this = self.next
            if this == "" or this == "|" or this == ")":
                break
            self._advance()
            if verbose:
                if len(this) == 1 and this in _WHITESPACE:
                    continue
                if this == "#":
                    while True:
                        this = self.get()
                        if this == "" or this == "\n":
                            break
                    continue
            rep = False
            if this[0] == "\\":
                items.append(self.escape(this, add, dl))
            elif this not in _SPECIAL:
                items.append(self.node(_N_LIT, ord(this), add, dl))
            elif this == "[":
                items.append(self.parse_set(add, dl))
            elif this in "*+?{":
                here = self.tell()
                lo = 0
                hi = -1
                if this == "?":
                    hi = 1
                elif this == "+":
                    lo = 1
                elif this == "{":
                    if self.next == "}":
                        items.append(self.node(_N_LIT, 123, add, dl))
                        last_rep = False
                        continue
                    los = ""
                    his = ""
                    while self.next_in(_DIGITS):
                        los += self.get()
                    if self.match(","):
                        while self.next_in(_DIGITS):
                            his += self.get()
                    else:
                        his = los
                    if not self.match("}"):
                        items.append(self.node(_N_LIT, 123, add, dl))
                        self.seek(here)
                        last_rep = False
                        continue
                    if los:
                        lo = int(los)
                        if lo >= _MAXREPEAT:
                            raise OverflowError("the repetition number is too large")
                    if his:
                        hi = int(his)
                        if hi >= _MAXREPEAT:
                            raise OverflowError("the repetition number is too large")
                        if hi < lo:
                            raise self.error("min repeat greater than max repeat", self.tell() - here)
                if not items or items[-1].kind == _N_AT:
                    raise self.error("nothing to repeat", self.tell() - here + len(this))
                if last_rep:
                    raise self.error("multiple repeat", self.tell() - here + len(this))
                r = _Node(_N_REP, 0, 0)
                r.lo = lo
                r.hi = hi
                if self.match("?"):
                    r.mode = 1
                elif self.match("+"):
                    r.mode = 2
                r.items.append(items[-1])
                items[-1] = r
                rep = True
            elif this == ".":
                items.append(self.node(_N_ANY, 0, add, dl))
            elif this == "(":
                start = self.tell() - 1
                capture = True
                atomic = False
                name = ""
                sadd = add
                sdl = dl
                if self.match("?"):
                    char = self.get()
                    if char == "":
                        raise self.error("unexpected end of pattern")
                    if char == "P":
                        if self.match("<"):
                            name = self.getuntil(">", "group name")
                            self.checkgroupname(name, 1)
                        elif self.match("="):
                            name = self.getuntil(")", "group name")
                            self.checkgroupname(name, 1)
                            if name not in self.groupdict:
                                raise self.error("unknown group name " + repr(name), len(name) + 1)
                            gid = self.groupdict[name]
                            if not self.checkgroup(gid):
                                raise self.error("cannot refer to an open group", len(name) + 1)
                            self.checklookbehindgroup(gid)
                            items.append(self.node(_N_BREF, gid, add, dl))
                            last_rep = False
                            continue
                        else:
                            char = self.get()
                            if char == "":
                                raise self.error("unexpected end of pattern")
                            raise self.error("unknown extension ?P" + char, len(char) + 2)
                    elif char == ":":
                        capture = False
                    elif char == "#":
                        while True:
                            if self.next == "":
                                raise self.error("missing ), unterminated comment", self.tell() - start)
                            if self.get() == ")":
                                break
                        continue
                    elif char == "=" or char == "!" or char == "<":
                        behind = False
                        saved = self.lookbehind
                        if char == "<":
                            char = self.get()
                            if char == "":
                                raise self.error("unexpected end of pattern")
                            if char != "=" and char != "!":
                                raise self.error("unknown extension ?<" + char, len(char) + 2)
                            behind = True
                            if saved < 0:
                                self.lookbehind = self.groups
                        p = self.parse_sub(add, dl, verbose, nested + 1)
                        if behind and saved < 0:
                            self.lookbehind = -1
                        if not self.match(")"):
                            raise self.error("missing ), unterminated subpattern", self.tell() - start)
                        if char == "!" and p.kind == _N_SEQ and not p.items:
                            items.append(_Node(_N_FAIL, 0, 0))
                        else:
                            look = _Node(_N_LOOK, (0 if char == "=" else 1) + (2 if behind else 0), 0)
                            look.items.append(p)
                            items.append(look)
                        last_rep = False
                        continue
                    elif char == "(":
                        condname = self.getuntil(")", "group name")
                        condgroup = 0
                        if not (condname.isdigit() and condname.isascii()):
                            self.checkgroupname(condname, 1)
                            if condname not in self.groupdict:
                                raise self.error("unknown group name " + repr(condname), len(condname) + 1)
                            condgroup = self.groupdict[condname]
                        else:
                            condgroup = int(condname)
                            if not condgroup:
                                raise self.error("bad group number", len(condname) + 1)
                            if condgroup >= _MAXGROUPS:
                                raise self.error("invalid group reference " + str(condgroup), len(condname) + 1)
                            if condgroup not in self.grouprefpos:
                                self.grouprefpos[condgroup] = self.tell() - len(condname) - 1
                        self.checklookbehindgroup(condgroup)
                        cond = _Node(_N_COND, condgroup, 0)
                        cond.items.append(self.parse_seq(add, dl, verbose, nested + 1, False))
                        if self.match("|"):
                            cond.items.append(self.parse_seq(add, dl, verbose, nested + 1, False))
                            if self.next == "|":
                                raise self.error("conditional backref with more than two branches")
                        else:
                            cond.items.append(_Node(_N_SEQ, 0, 0))
                        if not self.match(")"):
                            raise self.error("missing ), unterminated subpattern", self.tell() - start)
                        items.append(cond)
                        last_rep = False
                        continue
                    elif char == ">":
                        capture = False
                        atomic = True
                    elif (len(char) == 1 and char in _FLAGCHARS) or char == "-":
                        fl = self.parse_flags(char)
                        if not fl:
                            if not first or items:
                                raise self.error("global flags not at the start of the expression",
                                                 self.tell() - start)
                            verbose = self.flags & VERBOSE != 0
                            continue
                        sadd = (add | fl[0]) & ~fl[1]
                        sdl = (dl | fl[1]) & ~fl[0]
                        capture = False
                    else:
                        raise self.error("unknown extension ?" + char, len(char) + 1)
                group = 0
                if capture:
                    group = self.groups
                    self.groups += 1
                    self.closed.append(False)
                    self.gmin.append(0)
                    self.gmax.append(0)
                    if self.groups > _MAXGROUPS:
                        raise self.error("too many groups", len(name) + 1)
                    if name:
                        if name in self.groupdict:
                            raise self.error("redefinition of group name " + repr(name) + " as group " + str(group) +
                                             "; was group " + str(self.groupdict[name]), len(name) + 1)
                        self.groupdict[name] = group
                sub_verbose = (verbose or (sadd & VERBOSE) != 0) and not (sdl & VERBOSE)
                p = self.parse_sub(sadd, sdl, sub_verbose, nested + 1)
                if not self.match(")"):
                    raise self.error("missing ), unterminated subpattern", self.tell() - start)
                if capture:
                    self.closed[group] = True
                    self.gmin[group] = self.minwidth(p)
                    self.gmax[group] = self.maxwidth(p)
                    g = _Node(_N_GROUP, group, 0)
                    g.items.append(p)
                    items.append(g)
                elif atomic:
                    at = _Node(_N_ATOMIC, 0, 0)
                    at.items.append(p)
                    items.append(at)
                elif p.kind == _N_SEQ:
                    sq = _Node(_N_SEQ, 0, 0)           # (a fresh node: repeating it is not a multiple repeat)
                    sq.items = p.items
                    items.append(sq)
                else:
                    items.append(p)
            elif this == "^":
                items.append(self.node(_N_AT, -1, add, dl))
            elif this == "$":
                items.append(self.node(_N_AT, -2, add, dl))
            last_rep = rep
        return seq

    # -- widths

    def minwidth(self, nd: _Node) -> int:
        k = nd.kind
        if k == _N_LIT or k == _N_NLIT or k == _N_ANY or k == _N_SET:
            return 1
        if k == _N_SEQ:
            w = 0
            for it in nd.items:
                w += self.minwidth(it)
            return w
        if k == _N_ALT:
            w = -1
            for it in nd.items:
                v = self.minwidth(it)
                if w < 0 or v < w:
                    w = v
            return w
        if k == _N_GROUP or k == _N_ATOMIC:
            return self.minwidth(nd.items[0])
        if k == _N_REP:
            return nd.lo * self.minwidth(nd.items[0])
        if k == _N_BREF:
            return self.gmin[nd.c]
        if k == _N_COND:
            return min(self.minwidth(nd.items[0]), self.minwidth(nd.items[1]))
        return 0

    def maxwidth(self, nd: _Node) -> int:
        """The most nd can match (-1: no limit)."""
        k = nd.kind
        if k == _N_LIT or k == _N_NLIT or k == _N_ANY or k == _N_SET:
            return 1
        if k == _N_SEQ:
            w = 0
            for it in nd.items:
                v = self.maxwidth(it)
                if v < 0:
                    return -1
                w += v
            return w
        if k == _N_ALT or k == _N_COND:
            w = 0
            for it in nd.items:
                v = self.maxwidth(it)
                if v < 0:
                    return -1
                w = max(w, v)
            return w
        if k == _N_GROUP or k == _N_ATOMIC:
            return self.maxwidth(nd.items[0])
        if k == _N_REP:
            v = self.maxwidth(nd.items[0])
            if v == 0:
                return 0
            if v < 0 or nd.hi < 0:
                return -1
            return nd.hi * v
        if k == _N_BREF:
            return self.gmax[nd.c]
        return 0


class _Compiler:
    """Emits the instructions of a parsed pattern."""

    def __init__(self, parser: _Parser):
        self.p = parser
        self.code: list[int] = []
        self.marks = 0          # loop marks (registers after the groups' and lastindex)

    def emit(self, nd: _Node) -> None:
        code = self.code
        k = nd.kind
        icase = nd.flags & IGNORECASE != 0
        ascii = nd.flags & ASCII != 0
        if k == _N_SEQ:
            for it in nd.items:
                self.emit(it)
        elif k == _N_LIT or k == _N_NLIT:
            neg = k == _N_NLIT
            if icase:
                code.append(_NCHARI if neg else _CHARI)
                code.append(_fold_ascii(nd.c) if ascii else _fold(nd.c))
                code.append(1 if ascii else 0)
            else:
                code.append(_NCHAR if neg else _CHAR)
                code.append(nd.c)
        elif k == _N_ANY:
            code.append(_ANYALL if nd.flags & DOTALL else _ANY)
        elif k == _N_SET:
            code.append(_SET)
            code.append(nd.c)
            code.append((1 if icase else 0) + (2 if ascii else 0))
        elif k == _N_ALT:
            jumps: list[int] = []
            for i in range(len(nd.items)):
                if i < len(nd.items) - 1:
                    code.append(_SPLIT)
                    code.append(len(code) + 2)
                    at = len(code)
                    code.append(0)
                    self.emit(nd.items[i])
                    code.append(_JMP)
                    jumps.append(len(code))
                    code.append(0)
                    code[at] = len(code)
                else:
                    self.emit(nd.items[i])
            for j in jumps:
                code[j] = len(code)
        elif k == _N_GROUP:
            code.append(_SAVE)
            code.append(2 * nd.c)
            self.emit(nd.items[0])
            code.append(_CLOSE)
            code.append(nd.c)
        elif k == _N_REP:
            bk = nd.items[0].kind
            if nd.mode != 1 and (bk == _N_LIT or bk == _N_NLIT or bk == _N_ANY or bk == _N_SET) and nd.hi != 0:
                code.append(_REP1)
                code.append(nd.lo)
                code.append(nd.hi)
                code.append(1 if nd.mode == 2 else 0)
                at = len(code)
                code.append(0)
                self.emit(nd.items[0])
                code[at] = len(code) - at - 1
            elif nd.mode == 2:
                code.append(_ATOMIC)
                at = len(code)
                code.append(0)
                self.emit_rep(nd, False)
                code.append(_MATCH)
                code[at] = len(code)
            else:
                self.emit_rep(nd, nd.mode == 1)
        elif k == _N_AT:
            kind = nd.c
            if kind == -1:
                kind = _AT_BEGIN_LINE if nd.flags & MULTILINE else _AT_BEGIN
            elif kind == -2:
                kind = _AT_END_LINE if nd.flags & MULTILINE else _AT_END
            code.append(_AT)
            code.append(kind)
            code.append(1 if ascii else 0)
        elif k == _N_BREF:
            code.append(_BREF)
            code.append(nd.c)
            code.append(1 if icase else 0)
        elif k == _N_LOOK:
            width = 0
            if nd.c >= 2:
                lo = self.p.minwidth(nd.items[0])
                if lo != self.p.maxwidth(nd.items[0]):
                    raise error("look-behind requires fixed-width pattern")
                width = lo
            code.append(_LOOK)
            code.append(nd.c)
            code.append(width)
            at = len(code)
            code.append(0)
            self.emit(nd.items[0])
            code.append(_MATCH)
            code[at] = len(code)
        elif k == _N_ATOMIC:
            code.append(_ATOMIC)
            at = len(code)
            code.append(0)
            self.emit(nd.items[0])
            code.append(_MATCH)
            code[at] = len(code)
        elif k == _N_COND:
            code.append(_COND)
            code.append(nd.c)
            at = len(code)
            code.append(0)
            self.emit(nd.items[0])
            code.append(_JMP)
            j = len(code)
            code.append(0)
            code[at] = len(code)
            self.emit(nd.items[1])
            code[j] = len(code)
        elif k == _N_FAIL:
            code.append(_FAIL)

    def emit_rep(self, nd: _Node, lazy: bool) -> None:
        code = self.code
        body = nd.items[0]
        for _ in range(nd.lo):
            self.emit(body)
        if nd.hi < 0:
            nullable = self.p.minwidth(body) == 0
            mark = -1
            if nullable:
                mark = self.marks
                self.marks += 1
            top = len(code)
            code.append(_SPLIT)
            code.append(0)
            code.append(0)
            first = len(code)
            if nullable:
                code.append(_SAVE)
                code.append(-1 - mark)             # (fixed up when the number of groups is known)
            self.emit(body)
            if nullable:
                code.append(_CHECK)
                code.append(-1 - mark)
                chk = len(code)
                code.append(0)
                code.append(_JMP)
                code.append(top)
                code[chk] = len(code)
            else:
                code.append(_JMP)
                code.append(top)
            if lazy:
                code[top + 1] = len(code)
                code[top + 2] = first
            else:
                code[top + 1] = first
                code[top + 2] = len(code)
        else:
            exits: list[int] = []
            mark = -1
            if nd.hi > nd.lo and self.p.minwidth(body) == 0:      # (an iteration that matched nothing is the last, as in sre)
                mark = self.marks
                self.marks += 1
            for _ in range(nd.hi - nd.lo):
                code.append(_SPLIT)
                if lazy:
                    exits.append(len(code))
                    code.append(0)
                    code.append(len(code) + 1)
                else:
                    code.append(len(code) + 2)
                    exits.append(len(code))
                    code.append(0)
                if mark >= 0:
                    code.append(_SAVE)
                    code.append(-1 - mark)
                self.emit(body)
                if mark >= 0:
                    code.append(_CHECK)
                    code.append(-1 - mark)
                    exits.append(len(code))
                    code.append(0)
            for x in exits:
                code[x] = len(code)


class Pattern:
    """A compiled regular expression: pattern, flags, groups, groupindex and the matching methods."""

    def __init__(self, pattern: str, flags: int):
        p = _Parser(pattern, flags)
        root = p.parse()
        c = _Compiler(p)
        c.emit(root)
        c.code.append(_MATCH)
        self.pattern = pattern
        self.flags = p.flags
        self.groups = p.groups - 1
        self.groupindex = p.groupdict
        self._sets = p.sets
        self._code = c.code
        self._li = 2 * p.groups                     # the lastindex register; the loop marks follow it
        self._nregs = self._li + 1 + c.marks
        code = self._code
        i = 0
        while i < len(code):                        # the marks' registers
            op = code[i]
            if (op == _SAVE or op == _CHECK) and code[i + 1] < 0:
                code[i + 1] = self._li + 1 + (-1 - code[i + 1])
            i += _width_of(op)
        self._prefix = ""                           # a literal every match starts with (search looks for it)
        self._anchored = False
        i = 0
        while i < len(code) and code[i] == _CHAR:
            self._prefix += chr(code[i + 1])
            i += 2
        if code[0] == _AT and code[1] == _AT_BEGIN:
            self._anchored = True
        self._first = -1                            # every match starts with a character of this set
        self._first_mode = 0
        if code[0] == _SET:
            self._first = code[1]
            self._first_mode = code[2]
        elif code[0] == _REP1 and code[1] >= 1 and code[5] == _SET:
            self._first = code[6]
            self._first_mode = code[7]

    def __repr__(self) -> str:
        r = repr(self.pattern)
        if len(r) > 200:
            r = r[:200]
        names = ["re.IGNORECASE", "re.LOCALE", "re.MULTILINE", "re.DOTALL", "re.UNICODE", "re.VERBOSE", "re.DEBUG",
                 "re.ASCII"]
        fl = self.flags & ~UNICODE
        parts: list[str] = []
        for i in range(8):
            if fl & (2 << i):
                parts.append(names[i])
                fl &= ~(2 << i)
        if fl:
            parts.append(hex(fl))
        if parts:
            return "re.compile(" + r + ", " + "|".join(parts) + ")"
        return "re.compile(" + r + ")"

    # -- the matcher

    def _at(self, kind: int, ascii: int, s: str, sp: int, n: int) -> bool:
        if kind == _AT_BEGIN:
            return sp == 0
        if kind == _AT_BEGIN_LINE:
            return sp == 0 or s[sp - 1] == "\n"
        if kind == _AT_END:
            return sp == n or (sp == n - 1 and s[sp] == "\n")
        if kind == _AT_END_LINE:
            return sp == n or (sp < len(s) and s[sp] == "\n")
        if kind == _AT_END_STRING:
            return sp == n
        before = sp > 0 and _is_word(ord(s[sp - 1]), ascii != 0)
        after = sp < n and _is_word(ord(s[sp]), ascii != 0)
        if kind == _AT_BOUNDARY:
            return before != after
        return before == after

    def _run(self, s: str, pc: int, sp: int, n: int, regs: list[int], mode: int, start: int) -> int:
        """Matches from code[pc] at s[sp] (s ends at n): the end of the match, or -1.

        mode 1: the match must end at n (fullmatch); 2: it must not be empty (after an empty match)."""
        code = self._code
        bp: list[int] = []          # the backtracking stack: a pc and position to go back to,
        bv: list[int] = []          # or -1 - a register and the value to put back in it
        while True:
            op = code[pc]
            if op == _CHAR:
                if sp < n and ord(s[sp]) == code[pc + 1]:
                    pc += 2
                    sp += 1
                    continue
            elif op == _SPLIT:
                bp.append(code[pc + 2])
                bv.append(sp)
                pc = code[pc + 1]
                continue
            elif op == _REP1:
                mx = code[pc + 2]
                limit = n if mx < 0 or sp + mx > n else sp + mx
                one = pc + 5
                k = code[one]
                e = sp
                if k == _SET:
                    st = self._sets[code[one + 1]]
                    md = code[one + 2]
                    while e < limit and st.contains(ord(s[e]), md):
                        e += 1
                elif k == _CHAR:
                    c = code[one + 1]
                    while e < limit and ord(s[e]) == c:
                        e += 1
                elif k == _ANY:
                    while e < limit and s[e] != "\n":
                        e += 1
                elif k == _ANYALL:
                    e = limit
                elif k == _NCHAR:
                    c = code[one + 1]
                    while e < limit and ord(s[e]) != c:
                        e += 1
                else:
                    c = code[one + 1]
                    while e < limit:
                        ch = ord(s[e])
                        f = _fold_ascii(ch) if code[one + 2] else _fold(ch)
                        if (f == c) != (k == _CHARI):
                            break
                        e += 1
                if e - sp >= code[pc + 1]:
                    nxt = one + code[pc + 4]
                    if not code[pc + 3]:
                        for j in range(sp + code[pc + 1], e):
                            bp.append(nxt)
                            bv.append(j)
                    sp = e
                    pc = nxt
                    continue
            elif op == _JMP:
                pc = code[pc + 1]
                continue
            elif op == _ANY:
                if sp < n and s[sp] != "\n":
                    pc += 1
                    sp += 1
                    continue
            elif op == _ANYALL:
                if sp < n:
                    pc += 1
                    sp += 1
                    continue
            elif op == _SET:
                if sp < n and self._sets[code[pc + 1]].contains(ord(s[sp]), code[pc + 2]):
                    pc += 3
                    sp += 1
                    continue
            elif op == _CHARI or op == _NCHARI:
                if sp < n:
                    ch = ord(s[sp])
                    f = _fold_ascii(ch) if code[pc + 2] else _fold(ch)
                    if (f == code[pc + 1]) == (op == _CHARI):
                        pc += 3
                        sp += 1
                        continue
            elif op == _NCHAR:
                if sp < n and ord(s[sp]) != code[pc + 1]:
                    pc += 2
                    sp += 1
                    continue
            elif op == _SAVE:
                k = code[pc + 1]
                bp.append(-1 - k)
                bv.append(regs[k])
                regs[k] = sp
                pc += 2
                continue
            elif op == _CLOSE:
                g = code[pc + 1]
                k = 2 * g + 1
                bp.append(-1 - k)
                bv.append(regs[k])
                regs[k] = sp
                li = self._li
                bp.append(-1 - li)
                bv.append(regs[li])
                regs[li] = g
                pc += 2
                continue
            elif op == _CHECK:
                if sp == regs[code[pc + 1]]:
                    pc = code[pc + 2]
                else:
                    pc += 3
                continue
            elif op == _AT:
                if self._at(code[pc + 1], code[pc + 2], s, sp, n):
                    pc += 3
                    continue
            elif op == _BREF:
                g = code[pc + 1]
                a = regs[2 * g]
                b = regs[2 * g + 1]
                if a >= 0 and b >= 0:
                    ln = b - a
                    if sp + ln <= n:
                        ok = s[a:b] == s[sp:sp + ln]
                        if not ok and code[pc + 2]:
                            ok = True
                            for j in range(ln):
                                if _fold(ord(s[a + j])) != _fold(ord(s[sp + j])):
                                    ok = False
                                    break
                        if ok:
                            sp += ln
                            pc += 3
                            continue
            elif op == _LOOK or op == _ATOMIC:
                kind = code[pc + 1] if op == _LOOK else 0
                body = pc + 4 if op == _LOOK else pc + 2
                end = code[pc + 3] if op == _LOOK else code[pc + 1]
                saved = regs.copy()
                e = -1
                if kind < 2:
                    e = self._run(s, body, sp, n, regs, 0, 0)
                elif sp - code[pc + 2] >= 0:
                    e = self._run(s, body, sp - code[pc + 2], n, regs, 0, 0)
                if kind % 2 == 0:
                    if e >= 0:
                        for j in range(len(regs)):
                            if regs[j] != saved[j]:
                                bp.append(-1 - j)
                                bv.append(saved[j])
                        if op == _ATOMIC:
                            sp = e
                        pc = end
                        continue
                else:
                    if e < 0:
                        pc = end
                        continue
                    for j in range(len(regs)):
                        regs[j] = saved[j]
            elif op == _COND:
                g = code[pc + 1]
                if regs[2 * g] >= 0 and regs[2 * g + 1] >= 0:
                    pc += 3
                else:
                    pc = code[pc + 2]
                continue
            elif op == _MATCH:
                if mode == 0 or (mode == 1 and sp == n) or (mode == 2 and sp != start):
                    return sp
            # failed: back to the last choice
            while True:
                if not bp:
                    return -1
                p = bp.pop()
                v = bv.pop()
                if p >= 0:
                    pc = p
                    sp = v
                    break
                regs[-1 - p] = v

    def _exec(self, s: str, pos: int, endpos: int, how: int, must_advance: bool) -> list[int]:
        """The registers of the first match (how: 0 match, 1 fullmatch, 2 search); [] if none."""
        n = len(s)
        if pos < 0:
            pos = 0
        elif pos > n:
            pos = n
        if endpos < 0:
            endpos = 0
        elif endpos > n:
            endpos = n
        if how == 2 and pos > endpos:
            return []
        regs = [-1] * self._nregs
        p = pos
        prefix = self._prefix
        first = self._first
        while True:
            if how == 2 and prefix:
                p = s.find(prefix, p, endpos)
                if p < 0:
                    return []
            elif how == 2 and first >= 0:
                st = self._sets[first]
                while p < endpos and not st.contains(ord(s[p]), self._first_mode):
                    p += 1
                if p >= endpos:
                    return []
            mode = 0
            if how == 1:
                mode = 1
            elif must_advance and p == pos:
                mode = 2
            e = self._run(s, 0, p, endpos, regs, mode, p)
            if e >= 0:
                regs[0] = p
                regs[1] = e
                return regs
            if how != 2 or p >= endpos or self._anchored:
                return []
            p += 1

    def _make(self, s: str, regs: list[int], pos: int, endpos: int) -> "Match":
        n = len(s)
        return Match(self, s, regs, min(max(pos, 0), n), min(max(endpos, 0), n))

    def match(self, string: str, pos: int = 0, endpos: int = _MAXSIZE) -> "Match | None":
        """A match at string[pos] (or None)."""
        regs = self._exec(string, pos, endpos, 0, False)
        if not regs:
            return None
        return self._make(string, regs, pos, endpos)

    def fullmatch(self, string: str, pos: int = 0, endpos: int = _MAXSIZE) -> "Match | None":
        """A match of all of string[pos:endpos] (or None)."""
        regs = self._exec(string, pos, endpos, 1, False)
        if not regs:
            return None
        return self._make(string, regs, pos, endpos)

    def search(self, string: str, pos: int = 0, endpos: int = _MAXSIZE) -> "Match | None":
        """The first match anywhere in string[pos:endpos] (or None)."""
        regs = self._exec(string, pos, endpos, 2, False)
        if not regs:
            return None
        return self._make(string, regs, pos, endpos)

    def _all(self, string: str, pos: int, endpos: int, limit: int) -> list[list[int]]:
        """The registers of the matches findall/finditer/sub/split go through (limit > 0: at most that many)."""
        out: list[list[int]] = []
        n = len(string)
        p = min(max(pos, 0), n)
        endpos = min(max(endpos, 0), n)
        must_advance = False
        while limit <= 0 or len(out) < limit:
            if p > endpos:
                break
            regs = self._exec(string, p, endpos, 2, must_advance)
            if not regs:
                break
            out.append(regs)
            must_advance = regs[1] == regs[0]
            p = regs[1]
        return out

    def finditer(self, string: str, pos: int = 0, endpos: int = _MAXSIZE):
        """An iterator of the matches, left to right."""
        for regs in self._all(string, pos, endpos, 0):
            yield self._make(string, regs, pos, endpos)

    def _group_text(self, s: str, regs: list[int], g: int) -> str:
        if regs[2 * g] < 0 or regs[2 * g + 1] < 0:
            return ""
        return s[regs[2 * g]:regs[2 * g + 1]]

    def _findall_str(self, string: str, pos: int, endpos: int) -> list[str]:
        out: list[str] = []
        g = 1 if self.groups else 0
        for regs in self._all(string, pos, endpos, 0):
            out.append(self._group_text(string, regs, g))
        return out

    def _findall_tuples(self, string: str, pos: int, endpos: int) -> list[tuple[str, ...]]:
        out: list[tuple[str, ...]] = []
        for regs in self._all(string, pos, endpos, 0):
            out.append(tuple([self._group_text(string, regs, g) for g in range(1, self.groups + 1)]))
        return out

    def findall(self, string: str, pos: int = 0, endpos: int = _MAXSIZE) -> list[str]:
        """The matches' texts (one group: its text; several: tuples of them)."""
        if not sys._compiled:
            if self.groups > 1:
                return self._findall_tuples(string, pos, endpos)
        elif self.groups > 1:
            raise error("findall of a pattern with several groups: in compiled code the pattern must be a literal "
                        "(re.findall(r'...', s), or p.findall(s) for a p only set to re.compile(r'...'))")
        return self._findall_str(string, pos, endpos)

    def _findall_t(self, string: str, pos: int = 0, endpos: int = _MAXSIZE) -> list[tuple[str, ...]]:
        """findall of a pattern with several groups (what the compiler makes of it)."""
        return self._findall_tuples(string, pos, endpos)

    def split(self, string: str, maxsplit: int = 0) -> list[str | None]:
        """string split around the matches (and with the groups' texts between the parts)."""
        out: list[str | None] = []
        if maxsplit < 0:
            out.append(string)
            return out
        last = 0
        for regs in self._all(string, 0, _MAXSIZE, maxsplit):
            out.append(string[last:regs[0]])
            for g in range(1, self.groups + 1):
                if regs[2 * g] >= 0 and regs[2 * g + 1] >= 0:
                    out.append(string[regs[2 * g]:regs[2 * g + 1]])
                else:
                    out.append(None)
            last = regs[1]
        out.append(string[last:])
        return out

    def _subx(self, repl: _R, string: str, count: int) -> tuple[str, int]:
        parts: list[str] = []
        last = 0
        n = 0
        if count < 0:
            return (string, 0)
        if isinstance(repl, str):
            t = _Template(self, repl)
            for regs in self._all(string, 0, _MAXSIZE, count):
                parts.append(string[last:regs[0]])
                parts.append(t.expand(string, regs))
                last = regs[1]
                n += 1
        else:
            for regs in self._all(string, 0, _MAXSIZE, count):
                parts.append(string[last:regs[0]])
                parts.append(repl(self._make(string, regs, 0, _MAXSIZE)))
                last = regs[1]
                n += 1
        parts.append(string[last:])
        return ("".join(parts), n)

    def sub(self, repl: _R, string: str, count: int = 0) -> str:
        """string with the matches replaced by repl (a template with \\1 \\g<name>..., or a function of the Match)."""
        return self._subx(repl, string, count)[0]

    def subn(self, repl: _R, string: str, count: int = 0) -> tuple[str, int]:
        """sub(), and the number of replacements."""
        return self._subx(repl, string, count)


def _width_of(op: int) -> int:
    """The length of an instruction with its operands."""
    if op == _MATCH or op == _ANY or op == _ANYALL or op == _FAIL:
        return 1
    if op == _CHAR or op == _NCHAR or op == _JMP or op == _SAVE or op == _CLOSE:
        return 2
    if op == _LOOK:
        return 4
    if op == _ATOMIC:
        return 2
    if op == _REP1:
        return 5
    return 3


class _Template:
    """A replacement template: literal texts between group references."""

    def __init__(self, pattern: Pattern, repl: str):
        self.lits: list[str] = []
        self.groups: list[int] = []
        if "\\" not in repl:
            self.lits.append(repl)
            return
        t = _Parser(repl, 0)
        lit = ""
        while True:
            this = t.get()
            if this == "":
                break
            if this[0] == "\\":
                c = this[1]
                if c == "g":
                    if not t.match("<"):
                        raise t.error("missing <")
                    name = t.getuntil(">", "group name")
                    index = 0
                    if not (name.isdigit() and name.isascii()):
                        t.checkgroupname(name, 1)
                        if name not in pattern.groupindex:
                            raise IndexError("unknown group name " + repr(name))
                        index = pattern.groupindex[name]
                    else:
                        index = int(name)
                        if index >= _MAXGROUPS:
                            raise t.error("invalid group reference " + str(index), len(name) + 1)
                    if index > pattern.groups:
                        raise t.error("invalid group reference " + str(index), len(name) + 1)
                    self.lits.append(lit)
                    lit = ""
                    self.groups.append(index)
                elif c == "0":
                    if t.next_in(_OCTDIGITS):
                        this += t.get()
                        if t.next_in(_OCTDIGITS):
                            this += t.get()
                    lit += chr(int(this[1:], 8) & 0xFF)
                elif c in _DIGITS:
                    isoctal = False
                    if t.next_in(_DIGITS):
                        this += t.get()
                        if c in _OCTDIGITS and this[2] in _OCTDIGITS and t.next_in(_OCTDIGITS):
                            this += t.get()
                            isoctal = True
                            v = int(this[1:], 8)
                            if v > 0o377:
                                raise t.error("octal escape value " + this + " outside of range 0-0o377", len(this))
                            lit += chr(v)
                    if not isoctal:
                        index = int(this[1:])
                        if index > pattern.groups:
                            raise t.error("invalid group reference " + str(index), len(this) - 1)
                        self.lits.append(lit)
                        lit = ""
                        self.groups.append(index)
                else:
                    k = "abfnrtv\\".find(c)
                    if k >= 0:
                        lit += "\a\b\f\n\r\t\v\\"[k]
                    elif c in _ASCIILETTERS:
                        raise t.error("bad escape " + this, len(this))
                    else:
                        lit += this
            else:
                lit += this
        self.lits.append(lit)

    def expand(self, s: str, regs: list[int]) -> str:
        if not self.groups:
            return self.lits[0]
        parts: list[str] = []
        for i in range(len(self.groups)):
            parts.append(self.lits[i])
            g = self.groups[i]
            if regs[2 * g] >= 0 and regs[2 * g + 1] >= 0:
                parts.append(s[regs[2 * g]:regs[2 * g + 1]])
        parts.append(self.lits[-1])
        return "".join(parts)


class Match:
    """A match: group(), groups(), groupdict(), start(), end(), span(), expand(), and string, re, pos, endpos."""

    def __init__(self, pattern: Pattern, string: str, regs: list[int], pos: int, endpos: int):
        self.re = pattern
        self.string = string
        self.pos = pos
        self.endpos = endpos
        self._regs = regs
        li = regs[pattern._li]
        self.lastindex: int | None = li if li > 0 else None
        self.lastgroup: str | None = None
        if li > 0:
            for name, g in pattern.groupindex.items():
                if g == li:
                    self.lastgroup = name

    def _index(self, g: _G) -> int:
        if isinstance(g, str):
            if g in self.re.groupindex:
                return self.re.groupindex[g]
            raise IndexError("no such group")
        else:
            if 0 <= g <= self.re.groups:
                return g
            raise IndexError("no such group")

    def _text(self, i: int) -> str | None:
        a = self._regs[2 * i]
        b = self._regs[2 * i + 1]
        if a < 0 or b < 0:
            return None
        return self.string[a:b]

    def group(self, g: _G = 0) -> str | None:
        """The text of group g (0: the whole match; a number or a name); None if it did not match."""
        return self._text(self._index(g))

    def __getitem__(self, g: _G) -> str | None:
        return self._text(self._index(g))

    def groups(self, default: str | None = None) -> tuple[str | None, ...]:
        """The texts of all the groups (default for those that did not match)."""
        out: list[str | None] = []
        for i in range(1, self.re.groups + 1):
            t = self._text(i)
            out.append(default if t is None else t)
        return tuple(out)

    def groupdict(self, default: str | None = None) -> dict[str, str | None]:
        """The named groups' texts by name."""
        d: dict[str, str | None] = {}
        for name, i in self.re.groupindex.items():
            t = self._text(i)
            d[name] = default if t is None else t
        return d

    def start(self, g: _G = 0) -> int:
        i = self._index(g)
        if self._regs[2 * i + 1] < 0:
            return -1
        return self._regs[2 * i]

    def end(self, g: _G = 0) -> int:
        i = self._index(g)
        if self._regs[2 * i] < 0:
            return -1
        return self._regs[2 * i + 1]

    def span(self, g: _G = 0) -> tuple[int, int]:
        i = self._index(g)
        if self._regs[2 * i] < 0 or self._regs[2 * i + 1] < 0:
            return (-1, -1)
        return (self._regs[2 * i], self._regs[2 * i + 1])

    @property
    def regs(self) -> tuple[tuple[int, int], ...]:
        out: list[tuple[int, int]] = []
        for i in range(self.re.groups + 1):
            out.append(self.span(i))
        return tuple(out)

    def expand(self, template: str) -> str:
        """template with its group references (\\1, \\g<name>) replaced by the groups' texts."""
        return _Template(self.re, template).expand(self.string, self._regs)

    def __repr__(self) -> str:
        t = repr(self.string[self._regs[0]:self._regs[1]])
        if len(t) > 50:
            t = t[:50]
        return "<re.Match object; span=(" + str(self._regs[0]) + ", " + str(self._regs[1]) + "), match=" + t + ">"

    def __bool__(self) -> bool:
        return True


_cache: dict[str, Pattern] = {}


def _compile(pattern: str, flags: int) -> Pattern:
    key = str(flags) + ":" + pattern
    if key in _cache:
        return _cache[key]
    p = Pattern(pattern, flags)
    if len(_cache) >= 512:
        _cache.clear()
    _cache[key] = p
    return p


def compile(pattern: _P, flags: int = 0) -> Pattern:
    """pattern compiled (a Pattern is itself)."""
    if isinstance(pattern, Pattern):
        if flags:
            raise ValueError("cannot process flags argument with a compiled pattern")
        return pattern
    elif isinstance(pattern, str):
        return _compile(pattern, flags)
    else:
        raise TypeError("first argument must be string or compiled pattern")


def purge() -> None:
    """Empties the cache of compiled patterns."""
    _cache.clear()


def match(pattern: _P, string: str, flags: int = 0) -> Match | None:
    """A match of pattern at the start of string, or None."""
    return compile(pattern, flags).match(string)


def fullmatch(pattern: _P, string: str, flags: int = 0) -> Match | None:
    """A match of pattern with all of string, or None."""
    return compile(pattern, flags).fullmatch(string)


def search(pattern: _P, string: str, flags: int = 0) -> Match | None:
    """The first match of pattern in string, or None."""
    return compile(pattern, flags).search(string)


def sub(pattern: _P, repl: _R, string: str, count: int = 0, flags: int = 0) -> str:
    """string with the matches of pattern replaced by repl (a template, or a function of the Match)."""
    return compile(pattern, flags).sub(repl, string, count)


def subn(pattern: _P, repl: _R, string: str, count: int = 0, flags: int = 0) -> tuple[str, int]:
    """sub(), and the number of replacements."""
    return compile(pattern, flags).subn(repl, string, count)


def split(pattern: _P, string: str, maxsplit: int = 0, flags: int = 0) -> list[str | None]:
    """string split around the matches of pattern."""
    return compile(pattern, flags).split(string, maxsplit)


def findall(pattern: _P, string: str, flags: int = 0) -> list[str]:
    """The texts of the matches of pattern (of its group, or tuples of its groups' texts)."""
    return compile(pattern, flags).findall(string)


def _findall_t(pattern: _P, string: str, flags: int = 0) -> list[tuple[str, ...]]:
    """findall of a literal pattern with several groups (what the compiler makes of it)."""
    return compile(pattern, flags)._findall_t(string)


def finditer(pattern: _P, string: str, flags: int = 0):
    """An iterator of the matches of pattern in string."""
    return compile(pattern, flags).finditer(string)


def escape(pattern: str) -> str:
    """pattern with the characters special in patterns backslashed."""
    out: list[str] = []
    for c in pattern:
        if c in "()[]{}?*+-|^$\\.&~# \t\n\r\x0b\x0c":
            out.append("\\")
        out.append(c)
    return "".join(out)


if not sys._compiled:
    def _group_many(self, *args):
        if len(args) <= 1:
            return self._text(self._index(args[0] if args else 0))
        return tuple(self._text(self._index(g)) for g in args)

    Match.group = _group_many

    def _pattern_eq(self, other):
        if isinstance(other, Pattern):
            return self.pattern == other.pattern and self.flags == other.flags
        return False

    def _pattern_hash(self):
        return hash(self.pattern) ^ self.flags

    Pattern.__eq__ = _pattern_eq
    Pattern.__hash__ = _pattern_hash

    # ---- bytes patterns (the interpreter's): run as latin-1 text, the results made bytes again
    def _as_text(s, what="string"):
        if isinstance(s, str):
            raise TypeError("cannot use a bytes pattern on a string-like object")
        if isinstance(s, (bytes, bytearray)) or type(s).__name__ == "memoryview":
            return bytes(s).decode("latin-1")
        raise TypeError("expected string or bytes-like object, got '" + type(s).__name__ + "'")

    def _b(x):
        if x is None:
            return None
        if isinstance(x, tuple):
            return tuple(_b(y) for y in x)
        return x.encode("latin-1")

    class _BytesMatch:
        """A Match of a bytes pattern: its texts are bytes."""

        def __init__(self, m, string, pattern):
            self._m = m
            self.string = string
            self.re = pattern
            self.pos = m.pos
            self.endpos = m.endpos
            self.lastindex = m.lastindex
            self.lastgroup = m.lastgroup

        def group(self, *args):
            return _b(self._m.group(*args))

        def __getitem__(self, g):
            return _b(self._m[g])

        def groups(self, default=None):
            return tuple(default if t is None else _b(t) for t in self._m.groups(None))

        def groupdict(self, default=None):
            return {k: (default if v is None else _b(v)) for k, v in self._m.groupdict(None).items()}

        def start(self, g=0):
            return self._m.start(g)

        def end(self, g=0):
            return self._m.end(g)

        def span(self, g=0):
            return self._m.span(g)

        @property
        def regs(self):
            return self._m.regs

        def expand(self, template):
            return _b(self._m.expand(_as_text(template)))

        def __repr__(self):
            a, b = self._m.span()
            return "<re.Match object; span=(" + str(a) + ", " + str(b) + "), match=" + repr(self.string[a:b])[:50] + ">"

        def __bool__(self):
            return True

    class _BytesPattern:
        """A compiled bytes pattern."""

        def __init__(self, pattern, flags):
            self.pattern = pattern
            self._p = _compile(pattern.decode("latin-1"), flags | ASCII)
            self.flags = flags
            self.groups = self._p.groups
            self.groupindex = self._p.groupindex

        def _wrap(self, m, string):
            return None if m is None else _BytesMatch(m, string, self)

        def match(self, string, pos=0, endpos=_MAXSIZE):
            return self._wrap(self._p.match(_as_text(string), pos, endpos), string)

        def fullmatch(self, string, pos=0, endpos=_MAXSIZE):
            return self._wrap(self._p.fullmatch(_as_text(string), pos, endpos), string)

        def search(self, string, pos=0, endpos=_MAXSIZE):
            return self._wrap(self._p.search(_as_text(string), pos, endpos), string)

        def finditer(self, string, pos=0, endpos=_MAXSIZE):
            for m in self._p.finditer(_as_text(string), pos, endpos):
                yield _BytesMatch(m, string, self)

        def findall(self, string, pos=0, endpos=_MAXSIZE):
            return [_b(x) for x in self._p.findall(_as_text(string), pos, endpos)]

        def split(self, string, maxsplit=0):
            return [_b(x) for x in self._p.split(_as_text(string), maxsplit)]

        def _repl(self, repl, string):
            if callable(repl):
                return lambda m: _as_text(repl(_BytesMatch(m, string, self)))
            return _as_text(repl)

        def sub(self, repl, string, count=0):
            return _b(self._p.sub(self._repl(repl, string), _as_text(string), count))

        def subn(self, repl, string, count=0):
            s, n = self._p.subn(self._repl(repl, string), _as_text(string), count)
            return _b(s), n

        def __repr__(self):
            r = "re.compile(" + repr(self.pattern)
            return r + (", re." + "|re.".join(_flag_names(self.flags)) if self.flags else "") + ")"

        def __eq__(self, other):
            return isinstance(other, _BytesPattern) and self.pattern == other.pattern and self.flags == other.flags

        def __hash__(self):
            return hash(self.pattern) ^ self.flags

    def _flag_names(flags):
        names = []
        for name, v in (("TEMPLATE", 1), ("IGNORECASE", 2), ("LOCALE", 4), ("MULTILINE", 8), ("DOTALL", 16),
                        ("UNICODE", 32), ("VERBOSE", 64), ("DEBUG", 128), ("ASCII", 256)):
            if flags & v:
                names.append(name)
        return names

    _str_compile = compile

    def compile(pattern, flags=0):
        """pattern compiled (a Pattern is itself; a bytes pattern matches bytes)."""
        if isinstance(pattern, (bytes, bytearray)):
            flags = int(flags)
            if flags & 32:
                raise ValueError("cannot use UNICODE flag with a bytes pattern")
            key = (bytes(pattern), flags)
            p = _bcache.get(key)
            if p is None:
                p = _BytesPattern(bytes(pattern), flags)
                if len(_bcache) >= 512:
                    _bcache.clear()
                _bcache[key] = p
            return p
        if isinstance(pattern, _BytesPattern):
            if flags:
                raise ValueError("cannot process flags argument with a compiled pattern")
            return pattern
        return _str_compile(pattern, int(flags))

    _bcache = {}

    def _str_only(fn):
        def check(self, string, *args, **kw):
            if isinstance(string, (bytes, bytearray)):
                raise TypeError("cannot use a string pattern on a bytes-like object")
            return fn(self, string, *args, **kw)
        return check

    for _name in ("match", "fullmatch", "search", "findall", "finditer", "split"):
        setattr(Pattern, _name, _str_only(getattr(Pattern, _name)))

    def _sub_check(fn):
        def check(self, repl, string, count=0):
            if isinstance(string, (bytes, bytearray)):
                raise TypeError("cannot use a string pattern on a bytes-like object")
            return fn(self, repl, string, count)
        return check

    Pattern.sub = _sub_check(Pattern.sub)
    Pattern.subn = _sub_check(Pattern.subn)

    _str_escape = escape

    def escape(pattern):
        """pattern with the characters special in patterns backslashed (str or bytes)."""
        if isinstance(pattern, (bytes, bytearray)):
            return _str_escape(bytes(pattern).decode("latin-1")).encode("latin-1")
        return _str_escape(pattern)

    class Scanner:
        """A lexicon of (pattern, action) pairs: scan(string) -> (results, rest)."""

        def __init__(self, lexicon, flags=0):
            self.lexicon = lexicon
            self._items = [(_str_compile(p, flags), a) for p, a in lexicon]

        def scan(self, string):
            result = []
            i = 0
            n = len(string)
            while i < n:
                for p, action in self._items:
                    m = p.match(string, i)
                    if m is not None and m.end() > i:
                        if callable(action):
                            self.match = m
                            action = action(self, m.group())
                        if action is not None:
                            result.append(action)
                        i = m.end()
                        break
                else:
                    break
            return result, string[i:]
