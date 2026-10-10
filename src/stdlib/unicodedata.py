"""The Unicode character database (CPython's unicodedata, Unicode 16.0.0): the properties of
every code point, their names, and normalization.

lookup(), name(), decimal(), digit(), numeric(), category(), bidirectional(), combining(),
east_asian_width(), mirrored(), decomposition(), normalize(), is_normalized(),
unidata_version.

Not here: name aliases and named sequences in lookup() (only the characters' own names),
ucd_3_2_0 (the Unicode 3.2 database). The tables (_ucd.py) are read the first time a
function needs them."""
import _ucd
from typing import TypeVar

_D = TypeVar("_D")

__all__ = ["lookup", "name", "decimal", "digit", "numeric", "category", "bidirectional", "combining",
           "east_asian_width", "mirrored", "decomposition", "normalize", "is_normalized", "unidata_version"]

unidata_version = _ucd.UNIDATA_VERSION

_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"


def _digits() -> list[int]:
    v = [0] * 128
    for i in range(64):
        v[ord(_ALPHABET[i])] = i
    return v


_VAL = _digits()


def _decode(s: str) -> list[int]:
    """The numbers of a table (varints of 6-bit characters)."""
    out: list[int] = []
    val = _VAL
    n = 0
    shift = 0
    for ch in s:
        v = val[ord(ch)]
        n |= (v & 31) << shift
        if v & 32:
            shift += 5
        else:
            out.append(n)
            n = 0
            shift = 0
    return out


class _Tables:
    """The decoded tables (each the first time it is needed)."""

    def __init__(self) -> None:
        self.run_start: list[int] = []
        self.run_rec: list[int] = []
        self.records: list[int] = []
        self.cats = _ucd.CATEGORIES.split(" ")
        self.bidis = _ucd.BIDIRECTIONAL.split(" ")
        self.eaws = _ucd.EAST_ASIAN_WIDTHS.split(" ")
        self.nums = [float(x) for x in _ucd.NUMERICS.split(" ")]
        self.tags = _ucd.DECOMPOSITION_TAGS.split(" ")
        self.decomp: dict[int, list[int]] = {}
        self.decomp_tag: dict[int, int] = {}
        self.compose: dict[int, int] = {}
        self.has_decomp = False
        self.words: list[str] = []
        self.name_codes: list[int] = []
        self.name_blocks: list[int] = []
        self.names_by_text: dict[str, int] = {}
        self.has_names = False

    def props(self) -> None:
        if self.records:
            return
        nums = _decode(_ucd.RUNS)
        c = 0
        for i in range(0, len(nums), 2):
            c += nums[i]
            self.run_start.append(c)
            self.run_rec.append(nums[i + 1])
        self.records = _decode(_ucd.RECORDS)

    def decomps(self) -> None:
        if self.has_decomp:
            return
        self.has_decomp = True
        nums = _decode(_ucd.DECOMPOSITIONS)
        c = 0
        i = 0
        while i < len(nums):
            c += nums[i]
            tag = nums[i + 1]
            n = nums[i + 2]
            self.decomp[c] = nums[i + 3:i + 3 + n]
            self.decomp_tag[c] = tag
            i += 3 + n
        excluded: set[int] = set()
        e = 0
        for d in _decode(_ucd.COMPOSITION_EXCLUSIONS):
            e += d
            excluded.add(e)
        for c2 in self.decomp:
            parts = self.decomp[c2]
            if self.decomp_tag[c2] == 0 and len(parts) == 2 and c2 not in excluded:
                self.compose[(parts[0] << 21) | parts[1]] = c2

    def names(self) -> None:
        if self.has_names:
            return
        self.has_names = True
        self.words = _ucd.WORDS.split(" ")
        c = 0
        for d in _decode(_ucd.NAME_CODES):
            c += d
            self.name_codes.append(c)
        self.name_blocks = _decode(_ucd.NAME_BLOCKS)


_T = _Tables()


def _record(c: int) -> int:
    """The index of code point c's record (its properties) in the records table."""
    _T.props()
    starts = _T.run_start
    lo = 0
    hi = len(starts) - 1
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if starts[mid] <= c:
            lo = mid
        else:
            hi = mid - 1
    return _T.run_rec[lo] * 8


def _cp(fn: str, chr_: str) -> int:
    if len(chr_) != 1:
        raise TypeError(fn + "(): argument must be a unicode character, not a string of length " + str(len(chr_)))
    return ord(chr_)


def category(chr: str) -> str:
    """The general category assigned to the character chr as a string ('Lu', 'Nd', ...)."""
    r = _record(_cp("category", chr))
    return _T.cats[_T.records[r]]


def bidirectional(chr: str) -> str:
    """The bidirectional class assigned to the character chr ('L', 'R', ...; '' when none)."""
    r = _record(_cp("bidirectional", chr))
    return _T.bidis[_T.records[r + 1]]


def combining(chr: str) -> int:
    """The canonical combining class assigned to the character chr (0: none)."""
    r = _record(_cp("combining", chr))
    return _T.records[r + 2]


def mirrored(chr: str) -> int:
    """1 if the character chr is a mirrored character in bidirectional text, else 0."""
    r = _record(_cp("mirrored", chr))
    return _T.records[r + 3]


def east_asian_width(chr: str) -> str:
    """The east asian width assigned to the character chr ('W', 'Na', 'A', 'N', 'H' or 'F')."""
    r = _record(_cp("east_asian_width", chr))
    return _T.eaws[_T.records[r + 4]]


# (no default given: these; a default of None is one)
_NO_INT = -9223372036854775807
_NO_FLOAT = -1.5e308
_NO_STR = "\x00(no default)\x00"


def decimal(chr: str, default: _D = _NO_INT):
    """The decimal value assigned to the character chr (default, else ValueError: none)."""
    r = _record(_cp("decimal", chr))
    v = _T.records[r + 5]
    if v == 0:
        if isinstance(default, int) and default == _NO_INT:
            raise ValueError("not a decimal")
        return default
    return v - 1


def digit(chr: str, default: _D = _NO_INT):
    """The digit value assigned to the character chr (default, else ValueError: none)."""
    r = _record(_cp("digit", chr))
    v = _T.records[r + 6]
    if v == 0:
        if isinstance(default, int) and default == _NO_INT:
            raise ValueError("not a digit")
        return default
    return v - 1


def numeric(chr: str, default: _D = _NO_FLOAT):
    """The numeric value assigned to the character chr, as float (default, else ValueError: none)."""
    r = _record(_cp("numeric", chr))
    v = _T.records[r + 7]
    if v == 0:
        if isinstance(default, float) and default == _NO_FLOAT:
            raise ValueError("not a numeric character")
        return default
    return _T.nums[v - 1]


# ---------------------------------------------------------------- names

_SBASE = 0xAC00
_LBASE = 0x1100
_VBASE = 0x1161
_TBASE = 0x11A7
_LCOUNT = 19
_VCOUNT = 21
_TCOUNT = 28
_NCOUNT = _VCOUNT * _TCOUNT
_SCOUNT = _LCOUNT * _NCOUNT
_JAMO_L = ["G", "GG", "N", "D", "DD", "R", "M", "B", "BB", "S", "SS", "", "J", "JJ", "C", "K", "T", "P", "H"]
_JAMO_V = ["A", "AE", "YA", "YAE", "EO", "E", "YEO", "YE", "O", "WA", "WAE", "OE", "YO", "U", "WEO", "WE", "WI",
           "YU", "EU", "YI", "I"]
_JAMO_T = ["", "G", "GG", "GS", "N", "NJ", "NH", "D", "L", "LG", "LM", "LB", "LS", "LT", "LP", "LH", "M", "B",
           "BS", "S", "SS", "NG", "J", "C", "K", "T", "P", "H"]


def _name_of(c: int) -> str | None:
    if _SBASE <= c < _SBASE + _SCOUNT:
        s = c - _SBASE
        return ("HANGUL SYLLABLE " + _JAMO_L[s // _NCOUNT] + _JAMO_V[(s % _NCOUNT) // _TCOUNT]
                + _JAMO_T[s % _TCOUNT])
    for prefix, lo, hi in _ucd.ALGORITHMIC_NAMES:
        if lo <= c <= hi:
            return prefix + format(c, "04X")
    _T.names()
    codes = _T.name_codes
    lo2 = 0
    hi2 = len(codes) - 1
    while lo2 < hi2:
        mid = (lo2 + hi2 + 1) // 2
        if codes[mid] <= c:
            lo2 = mid
        else:
            hi2 = mid - 1
    if not codes or codes[lo2] != c:
        return None
    pos = _T.name_blocks[lo2 // 32]
    skip = lo2 % 32
    text = _ucd.NAMES
    val = _VAL
    words: list[str] = []
    n = 0
    shift = 0
    while True:
        v = val[ord(text[pos])]
        pos += 1
        n |= (v & 31) << shift
        if v & 32:
            shift += 5
            continue
        if n == 0:
            if skip == 0:
                break
            skip -= 1
        elif skip == 0:
            words.append(_T.words[n - 1])
        n = 0
        shift = 0
    return " ".join(words)


def name(chr: str, default: _D = _NO_STR):
    """The name assigned to the character chr (default, else ValueError: none)."""
    n = _name_of(_cp("name", chr))
    if n is None:
        if isinstance(default, str) and default == _NO_STR:
            raise ValueError("no such name")
        return default
    return n


def _all_names() -> dict[str, int]:
    if not _T.names_by_text:
        _T.names()
        text = _ucd.NAMES
        val = _VAL
        words: list[str] = []
        k = 0
        n = 0
        shift = 0
        for ch in text:
            v = val[ord(ch)]
            n |= (v & 31) << shift
            if v & 32:
                shift += 5
                continue
            if n == 0:
                _T.names_by_text[" ".join(words)] = _T.name_codes[k]
                k += 1
                words = []
            else:
                words.append(_T.words[n - 1])
            n = 0
            shift = 0
    return _T.names_by_text


def lookup(name: str) -> str:
    """The character with the given name (KeyError: none); upper or lower case alike."""
    key = name.upper()
    if key.startswith("HANGUL SYLLABLE "):
        rest = key[16:]
        for li in range(_LCOUNT):
            ln = _JAMO_L[li]
            if not rest.startswith(ln):
                continue
            for vi in range(_VCOUNT):
                vn = _JAMO_V[vi]
                if not rest[len(ln):].startswith(vn):
                    continue
                tail = rest[len(ln) + len(vn):]
                for ti in range(_TCOUNT):
                    if _JAMO_T[ti] == tail:
                        return chr(_SBASE + (li * _VCOUNT + vi) * _TCOUNT + ti)
    for prefix, lo, hi in _ucd.ALGORITHMIC_NAMES:
        if key.startswith(prefix):
            hexpart = key[len(prefix):]
            if 4 <= len(hexpart) <= 5 and all(ch in "0123456789ABCDEF" for ch in hexpart):
                c = int(hexpart, 16)
                if lo <= c <= hi:
                    return chr(c)
    found = _all_names().get(key)
    if found is None:
        raise KeyError("undefined character name '" + name + "'")
    return chr(found)


# ---------------------------------------------------------------- decomposition, normalization

def decomposition(chr: str) -> str:
    """The character decomposition mapping of chr ('<compat> 0066 0069'; '' when none)."""
    c = _cp("decomposition", chr)
    if _SBASE <= c < _SBASE + _SCOUNT:
        s = c - _SBASE
        lv = "%04X %04X" % (_LBASE + s // _NCOUNT, _VBASE + (s % _NCOUNT) // _TCOUNT)
        if s % _TCOUNT:
            return lv + " %04X" % (_TBASE + s % _TCOUNT)
        return lv
    _T.decomps()
    parts = _T.decomp.get(c)
    if parts is None:
        return ""
    out: list[str] = []
    tag = _T.tags[_T.decomp_tag[c]]
    if tag:
        out.append(tag)
    for p in parts:
        out.append(format(p, "04X"))
    return " ".join(out)


def _decompose(c: int, compat: bool, out: list[int]) -> None:
    if _SBASE <= c < _SBASE + _SCOUNT:
        s = c - _SBASE
        out.append(_LBASE + s // _NCOUNT)
        out.append(_VBASE + (s % _NCOUNT) // _TCOUNT)
        if s % _TCOUNT:
            out.append(_TBASE + s % _TCOUNT)
        return
    parts = _T.decomp.get(c)
    if parts is None or (_T.decomp_tag[c] != 0 and not compat):
        out.append(c)
        return
    for p in parts:
        _decompose(p, compat, out)


def _ccc(c: int) -> int:
    return _T.records[_record(c) + 2]


def _decomposed(s: str, compat: bool) -> list[int]:
    _T.decomps()
    cps: list[int] = []
    for ch in s:
        _decompose(ord(ch), compat, cps)
    # canonical ordering: runs of non-starters sorted (stably) by their combining class
    n = len(cps)
    i = 0
    while i < n:
        if _ccc(cps[i]) == 0:
            i += 1
            continue
        j = i
        while j < n and _ccc(cps[j]) != 0:
            j += 1
        if j - i > 1:
            run = cps[i:j]
            run.sort(key=_ccc)
            cps[i:j] = run
        i = j
    return cps


def _pair(a: int, b: int) -> int:
    """The primary composite of a and b (-1: none)."""
    if _LBASE <= a < _LBASE + _LCOUNT and _VBASE <= b < _VBASE + _VCOUNT:
        return _SBASE + ((a - _LBASE) * _VCOUNT + (b - _VBASE)) * _TCOUNT
    if _SBASE <= a < _SBASE + _SCOUNT and (a - _SBASE) % _TCOUNT == 0 and _TBASE < b < _TBASE + _TCOUNT:
        return a + (b - _TBASE)
    k = _T.compose.get((a << 21) | b)
    return -1 if k is None else k


def _composed(cps: list[int]) -> list[int]:
    """Canonical composition of the decomposed cps (UAX #15)."""
    if not cps:
        return cps
    out = [cps[0]]
    starter = 0
    last_class = _ccc(cps[0])
    if last_class != 0:
        last_class = 256
    for i in range(1, len(cps)):
        ch = cps[i]
        ch_class = _ccc(ch)
        comp = _pair(out[starter], ch)
        if comp >= 0 and (last_class < ch_class or last_class == 0):
            out[starter] = comp
            continue
        if ch_class == 0:
            starter = len(out)
        last_class = ch_class
        out.append(ch)
    return out


def normalize(form: str, unistr: str) -> str:
    """The normal form form ('NFC', 'NFKC', 'NFD' or 'NFKD') of the Unicode string unistr."""
    if form not in ("NFC", "NFKC", "NFD", "NFKD"):
        raise ValueError("invalid normalization form")
    if unistr.isascii():
        return unistr
    cps = _decomposed(unistr, form[-2] == "K" if len(form) == 4 else False)
    if form[-1] == "C":
        cps = _composed(cps)
    return "".join([chr(c) for c in cps])


def is_normalized(form: str, unistr: str) -> bool:
    """Whether the Unicode string unistr is in the normal form form."""
    return normalize(form, unistr) == unistr
