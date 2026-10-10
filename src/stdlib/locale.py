"""Locale support (CPython's locale), for the C locale only: setlocale() accepts "", "C", "POSIX" and
the C.UTF-8 names (others: locale.Error); text encoding UTF-8; localeconv() of the C locale."""
import sys
from typing import TypeVar

__all__ = ["getlocale", "getdefaultlocale", "getpreferredencoding", "Error", "setlocale", "localeconv", "strcoll",
           "strxfrm", "str", "atof", "atoi", "format_string", "currency", "normalize", "LC_CTYPE", "LC_COLLATE",
           "LC_TIME", "LC_MONETARY", "LC_NUMERIC", "LC_ALL", "LC_MESSAGES", "CHAR_MAX", "getencoding"]

_T = TypeVar("_T")


class Error(Exception):
    pass


if sys.platform == "darwin":
    LC_ALL = 0
    LC_COLLATE = 1
    LC_CTYPE = 2
    LC_MONETARY = 3
    LC_NUMERIC = 4
    LC_TIME = 5
    LC_MESSAGES = 6
else:
    LC_CTYPE = 0
    LC_NUMERIC = 1
    LC_TIME = 2
    LC_COLLATE = 3
    LC_MONETARY = 4
    LC_MESSAGES = 5
    LC_ALL = 6

CHAR_MAX = 127

_C_NAMES = ("", "C", "POSIX", "C.UTF-8", "C.utf8", "UTF-8")


_ctype_c = [False]                      # LC_CTYPE set to "C" explicitly: its encoding is ASCII


def setlocale(category: int, locale: str | None = None) -> str:
    """The locale of a category (and with locale: set it; here only the C locale exists)."""
    if category < 0 or category > 6:
        raise Error("unsupported locale setting")
    if locale is None:
        return "C"
    if locale in _C_NAMES:
        if category == LC_ALL or category == LC_CTYPE:
            _ctype_c[0] = locale in ("C", "POSIX")
        return "C"
    raise Error("unsupported locale setting")


def getlocale(category: int = 0) -> tuple[str | None, str | None]:
    """(language code, encoding) of the category's locale: (None, None) for C."""
    return None, None


def getdefaultlocale(envvars: _T = ('LC_ALL', 'LC_CTYPE', 'LANG', 'LANGUAGE')) -> tuple[str | None, str | None]:
    return None, None


def getencoding() -> str:
    """The locale's encoding (C locale: ASCII; else UTF-8)."""
    if _ctype_c[0]:
        return "US-ASCII" if sys.platform == "darwin" else "ANSI_X3.4-1968"
    return "UTF-8"


def getpreferredencoding(do_setlocale: bool = True) -> str:
    return "UTF-8"


def localeconv() -> dict[str, str]:
    """The C locale's conventions (compiled programs: values as text)."""
    return {"decimal_point": ".", "thousands_sep": "", "grouping": "[]", "int_curr_symbol": "",
            "currency_symbol": "", "mon_decimal_point": "", "mon_thousands_sep": "", "mon_grouping": "[]",
            "positive_sign": "", "negative_sign": "", "int_frac_digits": "127", "frac_digits": "127",
            "p_cs_precedes": "127", "p_sep_by_space": "127", "n_cs_precedes": "127", "n_sep_by_space": "127",
            "p_sign_posn": "127", "n_sign_posn": "127"}


def strcoll(a: str, b: str) -> int:
    """Compares two strings (C locale: by code points)."""
    return (a > b) - (a < b)


def strxfrm(s: str) -> str:
    return s


def delocalize(string: str) -> str:
    return string


def localize(string: str, grouping: bool = False, monetary: bool = False) -> str:
    return string


def atof(string: str) -> float:
    return float(string)


def atoi(string: str) -> int:
    return int(string)


def str(val: float) -> str:
    """val as format_string("%.12g", val)."""
    return _g12(val)


def _g12(val: float) -> str:
    return f"{val:.12g}"


def format_string(f: str, val: _T, grouping: bool = False, monetary: bool = False) -> str:
    """f % val in the C locale (compiled programs: one value)."""
    if not sys._compiled:
        return f % val
    return _fmt1(f, val)


def _fmt1(f: str, val: _T) -> str:
    i = f.find("%")
    if i < 0:
        return f
    j = i + 1
    while j < len(f) and f[j] in "0123456789.-+ #":
        j += 1
    if j >= len(f):
        return f
    conv = f[j]
    spec = f[i + 1:j]
    if conv == "%":
        return f[:i] + "%" + _fmt1(f[j + 1:], val)
    if isinstance(val, float):
        if conv == "f" and spec.startswith(".") and spec[1:].isdigit():
            text = _round_text(val, int(spec[1:]))
        else:
            text = repr(val) if conv != "d" else repr(int(val))
    elif isinstance(val, int):
        text = repr(val)
    else:
        text = _text(val)
    return f[:i] + text + f[j + 1:]


def _text(val: _T) -> str:
    return f"{val}"


def _round_text(v: float, p: int) -> str:
    if p == 0:
        return f"{v:.0f}"
    if p == 1:
        return f"{v:.1f}"
    if p == 2:
        return f"{v:.2f}"
    if p == 3:
        return f"{v:.3f}"
    return f"{v:.6f}"


def currency(val: float, symbol: bool = True, grouping: bool = False, international: bool = False) -> str:
    raise ValueError("Currency formatting is not possible using the 'C' locale.")


def normalize(localename: str) -> str:
    """A locale name as setlocale() takes it ('en_US' -> 'en_US.ISO8859-1')."""
    if localename in ("C", "POSIX", "") or "." in localename:
        return localename
    return localename + ".ISO8859-1"
