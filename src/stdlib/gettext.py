"""Internationalization and localization support (CPython's gettext): GNU .mo message catalogs.

Compiled programs: find() gives the first catalog (or None; all=True is not supported), translation()
always makes GNUTranslations (no class_), install() is not available (use _ = t.gettext)."""
import os
import sys
import struct
from typing import TypeVar

__all__ = ["NullTranslations", "GNUTranslations", "Catalog", "bindtextdomain", "find", "translation", "install",
           "textdomain", "dgettext", "dngettext", "gettext", "ngettext", "pgettext", "dpgettext", "npgettext",
           "dnpgettext"]

_T = TypeVar("_T")

_default_localedir = os.path.join("/usr", "share", "locale")         # (CPython: under sys.base_prefix)


# ---- plural forms: the C expression of a catalog's Plural-Forms header ----

def _tokenize(plural: str) -> list[str]:
    out: list[str] = []
    i = 0
    n = len(plural)
    while i < n:
        c = plural[i]
        if c in " \t":
            i += 1
        elif c.isdigit():
            j = i
            while j < n and plural[j].isdigit():
                j += 1
            if j < n and (plural[j].isalnum() or plural[j] == "_"):
                while j < n and (plural[j].isalnum() or plural[j] == "_"):
                    j += 1
                raise ValueError("invalid token in plural form: " + plural[i:j])
            out.append(plural[i:j])
            i = j
        elif c.isalpha() or c == "_":
            j = i
            while j < n and (plural[j].isalnum() or plural[j] == "_"):
                j += 1
            if plural[i:j] != "n":
                raise ValueError("invalid token in plural form: " + plural[i:j])
            out.append("n")
            i = j
        elif c in "()":
            out.append(c)
            i += 1
        elif plural.startswith(("&&", "||", "==", "<=", ">=", "!="), i):
            out.append(plural[i:i + 2])
            i += 2
        elif c in "-*/%+?:<>!":
            out.append(c)
            i += 1
        else:
            raise ValueError("invalid token in plural form: " + c)
    out.append("")
    return out


def _error(value: str) -> ValueError:
    if value:
        return ValueError("unexpected token in plural form: " + value)
    return ValueError("unexpected end of plural form")


_binary_ops = {"||": 1, "&&": 2, "==": 3, "!=": 3, "<": 4, ">": 4, "<=": 4, ">=": 4,
               "+": 5, "-": 5, "*": 6, "/": 6, "%": 6}
_c2py_ops = {"||": "or", "&&": "and", "/": "//"}


class _Toks:
    def __init__(self, toks: list[str]) -> None:
        self.toks = toks
        self.i = 0

    def next(self) -> str:
        if self.i >= len(self.toks):
            raise _error("")
        t = self.toks[self.i]
        self.i += 1
        return t


def _parse(tokens: _Toks, priority: int = -1) -> tuple[str, str]:
    """(the Python expression of the C one, the token after it): as CPython writes it."""
    result = ""
    nexttok = tokens.next()
    while nexttok == "!":
        result += "not "
        nexttok = tokens.next()
    if nexttok == "(":
        sub, nexttok = _parse(tokens)
        result = result + "(" + sub + ")"
        if nexttok != ")":
            raise ValueError("unbalanced parenthesis in plural form")
    elif nexttok == "n":
        result = result + nexttok
    else:
        if not nexttok or not nexttok.isdigit():
            raise _error(nexttok)
        result = result + str(int(nexttok, 10))
    nexttok = tokens.next()
    j = 100
    while nexttok in _binary_ops:
        i = _binary_ops[nexttok]
        if i < priority:
            break
        if i in (3, 4) and j in (3, 4):           # (no chained comparisons)
            result = "(" + result + ")"
        op = _c2py_ops.get(nexttok, nexttok)
        right, nexttok = _parse(tokens, i + 1)
        result = result + " " + op + " " + right
        j = i
    if j == priority == 4:
        result = "(" + result + ")"
    if nexttok == "?" and priority <= 0:
        if_true, nexttok = _parse(tokens, 0)
        if nexttok != ":":
            raise _error(nexttok)
        if_false, nexttok = _parse(tokens)
        result = if_true + " if " + result + " else " + if_false
        if priority == 0:
            result = "(" + result + ")"
    return result, nexttok


class _PNode:
    """A node of the Python expression: op ("n", "num", "not", "if", or a binary operator)."""

    def __init__(self, op: str, val: int = 0) -> None:
        self.op = op
        self.val = val
        self.kids: list["_PNode"] = []

    def eval(self, n: int) -> int:
        op = self.op
        if op == "n":
            return n
        if op == "num":
            return self.val
        k = self.kids
        if op == "not":
            return 0 if k[0].eval(n) else 1
        if op == "if":
            return k[1].eval(n) if k[0].eval(n) else k[2].eval(n)
        a = k[0].eval(n)
        if op == "or":
            return a if a else k[1].eval(n)
        if op == "and":
            return k[1].eval(n) if a else a
        b = k[1].eval(n)
        if op == "+":
            return a + b
        if op == "-":
            return a - b
        if op == "*":
            return a * b
        if op == "//":
            return a // b
        if op == "%":
            return a % b
        if op == "==":
            return 1 if a == b else 0
        if op == "!=":
            return 1 if a != b else 0
        if op == "<":
            return 1 if a < b else 0
        if op == ">":
            return 1 if a > b else 0
        if op == "<=":
            return 1 if a <= b else 0
        return 1 if a >= b else 0


class _PyExpr:
    """Reads the Python expression _parse() wrote (Python's precedence)."""
    _PREC = {"or": 1, "and": 2, "==": 4, "!=": 4, "<": 4, ">": 4, "<=": 4, ">=": 4,
             "+": 5, "-": 5, "*": 6, "//": 6, "%": 6}

    def __init__(self, text: str) -> None:
        self.t = text.replace("(", " ( ").replace(")", " ) ").split()
        self.i = 0

    def peek(self) -> str:
        return self.t[self.i] if self.i < len(self.t) else ""

    def take(self) -> str:
        s = self.peek()
        self.i += 1
        return s

    def expr(self) -> _PNode:
        x = self.binary(1)
        if self.peek() == "if":
            self.take()
            c = self.binary(1)
            self.take()                                   # else
            f = self.expr()
            node = _PNode("if")
            node.kids = [c, x, f]
            return node
        return x

    def binary(self, prec: int) -> _PNode:
        if prec == 3:                                     # not
            if self.peek() == "not":
                self.take()
                node = _PNode("not")
                node.kids = [self.binary(3)]
                return node
            return self.binary(4)
        if prec == 7:
            t = self.take()
            if t == "(":
                x = self.expr()
                self.take()
                return x
            if t == "n":
                return _PNode("n")
            return _PNode("num", int(t))
        left = self.binary(prec + 1)
        while self.peek() in _PyExpr._PREC and _PyExpr._PREC[self.peek()] == prec:
            node = _PNode(self.take())
            node.kids = [left, self.binary(prec + 1)]
            left = node
        return left


class _Plural:
    """The plural form function of a C expression: plural(n) -> the index of the form."""

    def __init__(self, tree: _PNode) -> None:
        self._tree = tree

    def __call__(self, n: int) -> int:
        return self._tree.eval(n)


def c2py(plural: str) -> _Plural:
    """Gets a C expression as used in PO files for plural forms and returns a
    Python function that implements an equivalent expression."""
    if len(plural) > 1000:
        raise ValueError("plural form expression is too long")
    result, nexttok = _parse(_Toks(_tokenize(plural)))
    if nexttok:
        raise _error(nexttok)
    depth = 0
    for c in result:
        if c == "(":
            depth += 1
            if depth > 20:
                raise ValueError("plural form expression is too complex")
        elif c == ")":
            depth -= 1
    return _Plural(_PyExpr(result).expr())


def _expand_lang(loc: str) -> list[str]:
    import locale
    loc = locale.normalize(loc)
    COMPONENT_CODESET = 1 << 0
    COMPONENT_TERRITORY = 1 << 1
    COMPONENT_MODIFIER = 1 << 2
    mask = 0
    pos = loc.find("@")
    if pos >= 0:
        modifier = loc[pos:]
        loc = loc[:pos]
        mask |= COMPONENT_MODIFIER
    else:
        modifier = ""
    pos = loc.find(".")
    if pos >= 0:
        codeset = loc[pos:]
        loc = loc[:pos]
        mask |= COMPONENT_CODESET
    else:
        codeset = ""
    pos = loc.find("_")
    if pos >= 0:
        territory = loc[pos:]
        loc = loc[:pos]
        mask |= COMPONENT_TERRITORY
    else:
        territory = ""
    language = loc
    ret: list[str] = []
    for i in range(mask + 1):
        if not (i & ~mask):
            val = language
            if i & COMPONENT_TERRITORY:
                val += territory
            if i & COMPONENT_CODESET:
                val += codeset
            if i & COMPONENT_MODIFIER:
                val += modifier
            ret.append(val)
    ret.reverse()
    return ret


def _unpack2(le: bool, b: bytes) -> tuple[int, int]:
    if le:
        return struct.unpack("<II", b)
    return struct.unpack(">II", b)


class NullTranslations:
    def __init__(self, fp: _T = None) -> None:
        self._info: dict[str, str] = {}
        self._charset: str | None = None
        self._fallback: NullTranslations | None = None
        if fp is not None:
            self._parse(fp.read())

    def _parse(self, buf: bytes) -> None:
        pass

    def add_fallback(self, fallback: "NullTranslations") -> None:
        if self._fallback is not None:
            self._fallback.add_fallback(fallback)
        else:
            self._fallback = fallback

    def gettext(self, message: str) -> str:
        if self._fallback is not None:
            return self._fallback.gettext(message)
        return message

    def ngettext(self, msgid1: str, msgid2: str, n: int) -> str:
        if self._fallback is not None:
            return self._fallback.ngettext(msgid1, msgid2, n)
        return msgid1 if n == 1 else msgid2

    def pgettext(self, context: str, message: str) -> str:
        if self._fallback is not None:
            return self._fallback.pgettext(context, message)
        return message

    def npgettext(self, context: str, msgid1: str, msgid2: str, n: int) -> str:
        if self._fallback is not None:
            return self._fallback.npgettext(context, msgid1, msgid2, n)
        return msgid1 if n == 1 else msgid2

    def info(self) -> dict[str, str]:
        return self._info

    def charset(self) -> str | None:
        return self._charset

    if not sys._compiled:
        def install(self, names=None):
            import builtins
            builtins.__dict__["_"] = self.gettext
            if names is not None:
                allowed = {"gettext", "ngettext", "npgettext", "pgettext"}
                for name in allowed & set(names):
                    builtins.__dict__[name] = getattr(self, name)


class GNUTranslations(NullTranslations):
    # Magic number of .mo files
    LE_MAGIC = 0x950412de
    BE_MAGIC = 0xde120495

    # The encoding of a msgctxt and a msgid in a .mo file is msgctxt + "\x04" + msgid
    CONTEXT = "%s\x04%s"

    # Acceptable .mo versions
    VERSIONS = (0, 1)

    def __init__(self, fp: _T = None) -> None:
        self._catalog: dict[str, str] = {}
        self._plurals: dict[tuple[str, int], str] = {}
        self.plural = c2py("n != 1")                     # germanic plural by default
        self._filename = ""
        if fp is not None and hasattr(fp, "name"):
            self._filename = fp.name
        super().__init__(fp)

    def _get_versions(self, version: int) -> tuple[int, int]:
        """Returns a tuple of major version, minor version"""
        return (version >> 16, version & 0xffff)

    def _parse(self, buf: bytes) -> None:
        filename = self._filename
        buflen = len(buf)
        magic = struct.unpack("<I", buf[:4])[0]
        if magic == self.LE_MAGIC:
            version, msgcount, masteridx, transidx = struct.unpack("<4I", buf[4:20])
            le = True
        elif magic == self.BE_MAGIC:
            version, msgcount, masteridx, transidx = struct.unpack(">4I", buf[4:20])
            le = False
        else:
            raise OSError(0, "Bad magic number", filename)
        major_version, minor_version = self._get_versions(version)
        if major_version not in self.VERSIONS:
            raise OSError(0, "Bad version number " + str(major_version), filename)
        for i in range(0, msgcount):
            mlen, moff = _unpack2(le, buf[masteridx:masteridx + 8])
            mend = moff + mlen
            tlen, toff = _unpack2(le, buf[transidx:transidx + 8])
            tend = toff + tlen
            if mend < buflen and tend < buflen:
                msg = buf[moff:mend]
                tmsg = buf[toff:tend]
            else:
                raise OSError(0, "File is corrupt", filename)
            if mlen == 0:                                # the catalog description
                lastk = ""
                for b_item in tmsg.split(b"\n"):
                    item = b_item.decode().strip()
                    if not item:
                        continue
                    if item.startswith("#-#-#-#-#") and item.endswith("#-#-#-#-#"):
                        continue
                    k = ""
                    v = ""
                    if ":" in item:
                        k, v = item.split(":", 1)
                        k = k.strip().lower()
                        v = v.strip()
                        self._info[k] = v
                        lastk = k
                    elif lastk:
                        self._info[lastk] += "\n" + item
                    if k == "content-type":
                        self._charset = v.split("charset=")[1]
                    elif k == "plural-forms":
                        plural = v.split(";")[1].split("plural=")[1]
                        self.plural = c2py(plural)
            charset = self._charset if self._charset else "ascii"
            if b"\x00" in msg:                           # plural forms
                parts = msg.split(b"\x00")
                msgid1 = parts[0].decode(charset)
                for i, x in enumerate(tmsg.split(b"\x00")):
                    self._plurals[(msgid1, i)] = x.decode(charset)
            else:
                self._catalog[msg.decode(charset)] = tmsg.decode(charset)
            masteridx += 8
            transidx += 8

    def _copy(self) -> "GNUTranslations":
        t = GNUTranslations()
        t._catalog = self._catalog
        t._plurals = self._plurals
        t._info = self._info
        t._charset = self._charset
        t.plural = self.plural
        t._filename = self._filename
        return t

    def gettext(self, message: str) -> str:
        if message in self._catalog:
            return self._catalog[message]
        key = (message, self.plural(1))
        if key in self._plurals:
            return self._plurals[key]
        if self._fallback is not None:
            return self._fallback.gettext(message)
        return message

    def ngettext(self, msgid1: str, msgid2: str, n: int) -> str:
        key = (msgid1, self.plural(n))
        if key in self._plurals:
            return self._plurals[key]
        if self._fallback is not None:
            return self._fallback.ngettext(msgid1, msgid2, n)
        return msgid1 if n == 1 else msgid2

    def pgettext(self, context: str, message: str) -> str:
        ctxt_msg_id = context + "\x04" + message
        if ctxt_msg_id in self._catalog:
            return self._catalog[ctxt_msg_id]
        key = (ctxt_msg_id, self.plural(1))
        if key in self._plurals:
            return self._plurals[key]
        if self._fallback is not None:
            return self._fallback.pgettext(context, message)
        return message

    def npgettext(self, context: str, msgid1: str, msgid2: str, n: int) -> str:
        key = (context + "\x04" + msgid1, self.plural(n))
        if key in self._plurals:
            return self._plurals[key]
        if self._fallback is not None:
            return self._fallback.npgettext(context, msgid1, msgid2, n)
        return msgid1 if n == 1 else msgid2


def _find_all(domain: str, localedir: str | None = None, languages: list[str] | None = None) -> list[str]:
    """The catalogs of domain for the languages (the environment's: LANGUAGE, LC_ALL, LC_MESSAGES, LANG)."""
    ldir = localedir if localedir is not None else _default_localedir
    langs: list[str] = []
    if languages is None:
        for envar in ("LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG"):
            val = os.environ.get(envar)
            if val:
                langs = val.split(":")
                break
        if "C" not in langs:
            langs.append("C")
    else:
        langs = list(languages)
    nelangs: list[str] = []
    for lang in langs:
        for nelang in _expand_lang(lang):
            if nelang not in nelangs:
                nelangs.append(nelang)
    result: list[str] = []
    for lang in nelangs:
        if lang == "C":
            break
        mofile = os.path.join(ldir, lang, "LC_MESSAGES", domain + ".mo")
        if os.path.exists(mofile):
            result.append(mofile)
    return result


if not sys._compiled:
    def find(domain, localedir=None, languages=None, all=False):
        found = _find_all(domain, localedir, None if languages is None else list(languages))
        if all:
            return found
        return found[0] if found else None

if sys._compiled:
    def find(domain: str, localedir: str | None = None, languages: list[str] | None = None) -> str | None:
        found = _find_all(domain, localedir, languages)
        return found[0] if found else None


_translations: dict[str, GNUTranslations] = {}


def translation(domain: str, localedir: str | None = None, languages: list[str] | None = None,
                fallback: bool = False) -> NullTranslations:
    mofiles = _find_all(domain, localedir, languages)
    if not mofiles:
        if fallback:
            return NullTranslations()
        raise FileNotFoundError(2, "No translation file found for domain", domain)
    result: NullTranslations | None = None
    for mofile in mofiles:
        key = os.path.abspath(mofile)
        if key not in _translations:
            with open(mofile, "rb") as fp:
                _translations[key] = GNUTranslations(fp)
        t = _translations[key]._copy()
        if result is None:
            result = t
        else:
            result.add_fallback(t)
    assert result is not None
    return result


if not sys._compiled:
    def install(domain, localedir=None, *, names=None):
        t = translation(domain, localedir, fallback=True)
        t.install(names)


# a mapping b/w domains and locale directories
_localedirs: dict[str, str] = {}
# current global domain, `messages' used for compatibility w/ GNU gettext
_current_domain = "messages"


def textdomain(domain: str | None = None) -> str:
    global _current_domain
    if domain is not None:
        _current_domain = domain
    return _current_domain


def bindtextdomain(domain: str, localedir: str | None = None) -> str:
    if localedir is not None:
        _localedirs[domain] = localedir
    return _localedirs.get(domain, _default_localedir)


def _domain_translation(domain: str) -> NullTranslations | None:
    try:
        return translation(domain, _localedirs.get(domain, None))
    except OSError:
        return None


def dgettext(domain: str, message: str) -> str:
    t = _domain_translation(domain)
    return t.gettext(message) if t is not None else message


def dngettext(domain: str, msgid1: str, msgid2: str, n: int) -> str:
    t = _domain_translation(domain)
    if t is None:
        return msgid1 if n == 1 else msgid2
    return t.ngettext(msgid1, msgid2, n)


def dpgettext(domain: str, context: str, message: str) -> str:
    t = _domain_translation(domain)
    return t.pgettext(context, message) if t is not None else message


def dnpgettext(domain: str, context: str, msgid1: str, msgid2: str, n: int) -> str:
    t = _domain_translation(domain)
    if t is None:
        return msgid1 if n == 1 else msgid2
    return t.npgettext(context, msgid1, msgid2, n)


def gettext(message: str) -> str:
    return dgettext(_current_domain, message)


def ngettext(msgid1: str, msgid2: str, n: int) -> str:
    return dngettext(_current_domain, msgid1, msgid2, n)


def pgettext(context: str, message: str) -> str:
    return dpgettext(_current_domain, context, message)


def npgettext(context: str, msgid1: str, msgid2: str, n: int) -> str:
    return dnpgettext(_current_domain, context, msgid1, msgid2, n)


# dcgettext() has been deemed unnecessary and is not implemented.
Catalog = translation
