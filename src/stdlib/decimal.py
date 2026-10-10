"""Decimal fixed and floating point arithmetic (CPython's decimal, after its _pydecimal).

    from decimal import Decimal, getcontext, localcontext, ROUND_HALF_UP
    Decimal("0.1") + Decimal("0.2")                    # Decimal('0.3')
    Decimal("7.325").quantize(Decimal("0.01"), rounding=ROUND_HALF_UP)

Decimal, Context (prec, rounding, Emin, Emax, capitals, clamp, flags, traps), getcontext(),
setcontext(), localcontext(), DefaultContext, BasicContext, ExtendedContext, IEEEContext, the rounding
modes and the signals (DecimalException, InvalidOperation, DivisionByZero, Inexact, Rounded ...). The
results, flags and messages are the C decimal module's (libmpdec's): InvalidOperation:
[<class 'decimal.ConversionSyntax'>], a power computed as it does. Coefficients of any size (ints here
have 64 bits: a big-integer type of its own does the arithmetic).

Here: one context for the whole program (not one per thread); int(Decimal) of 2**63 and more is an
OverflowError; no comparisons with fractions.Fraction. Compiled programs: the signals have one base class
each (DivisionByZero and DivisionUndefined: ZeroDivisionError; FloatOperation: DecimalException);
Context(flags=, traps=) take lists of signals (not dicts); as_tuple() of a special value has exponent 0
(its kind in .special; its repr as CPython's)."""
import sys
import math as _math
from typing import TypeVar

__all__ = [
    'Decimal', 'Context', 'DecimalTuple', 'DefaultContext', 'BasicContext', 'ExtendedContext',
    'DecimalException', 'Clamped', 'InvalidOperation', 'DivisionByZero', 'Inexact', 'Rounded',
    'Subnormal', 'Overflow', 'Underflow', 'FloatOperation', 'DivisionImpossible', 'InvalidContext',
    'ConversionSyntax', 'DivisionUndefined', 'ROUND_DOWN', 'ROUND_HALF_UP', 'ROUND_HALF_EVEN',
    'ROUND_CEILING', 'ROUND_FLOOR', 'ROUND_UP', 'ROUND_HALF_DOWN', 'ROUND_05UP', 'setcontext',
    'getcontext', 'localcontext', 'IEEEContext', 'MAX_PREC', 'MAX_EMAX', 'MIN_EMIN', 'MIN_ETINY',
    'IEEE_CONTEXT_MAX_BITS', 'HAVE_THREADS', 'HAVE_CONTEXTVAR']

__version__ = '1.70'
__libmpdec_version__ = "2.5.1"

_O = TypeVar("_O")
_V = TypeVar("_V")
_T = TypeVar("_T")
_S = TypeVar("_S")

ROUND_DOWN = 'ROUND_DOWN'
ROUND_HALF_UP = 'ROUND_HALF_UP'
ROUND_HALF_EVEN = 'ROUND_HALF_EVEN'
ROUND_CEILING = 'ROUND_CEILING'
ROUND_FLOOR = 'ROUND_FLOOR'
ROUND_UP = 'ROUND_UP'
ROUND_HALF_DOWN = 'ROUND_HALF_DOWN'
ROUND_05UP = 'ROUND_05UP'
_ROUNDING_MODES = (ROUND_DOWN, ROUND_HALF_UP, ROUND_HALF_EVEN, ROUND_CEILING, ROUND_FLOOR, ROUND_UP,
                   ROUND_HALF_DOWN, ROUND_05UP)

HAVE_THREADS = True
HAVE_CONTEXTVAR = True
MAX_PREC = 999999999999999999
MAX_EMAX = 999999999999999999
MIN_EMIN = -999999999999999999
MIN_ETINY = MIN_EMIN - (MAX_PREC - 1)
IEEE_CONTEXT_MAX_BITS = 512

# ---------------------------------------------------------------- the signals

if not sys._compiled:
    class DecimalException(ArithmeticError):
        """Base exception class."""

    class Clamped(DecimalException):
        """Exponent of a 0 changed to fit bounds."""

    class InvalidOperation(DecimalException):
        """An invalid operation was performed."""

    class ConversionSyntax(InvalidOperation):
        """Trying to convert badly formed string."""

    class DivisionByZero(DecimalException, ZeroDivisionError):
        """Division by 0."""

    class DivisionImpossible(InvalidOperation):
        """Cannot perform the division adequately."""

    class DivisionUndefined(InvalidOperation, ZeroDivisionError):
        """Undefined result of division."""

    class Inexact(DecimalException):
        """Had to round, losing information."""

    class InvalidContext(InvalidOperation):
        """Invalid context."""

    class Rounded(DecimalException):
        """Number got rounded (not  necessarily changed during rounding)."""

    class Subnormal(DecimalException):
        """Exponent < Emin before rounding."""

    class Overflow(Inexact, Rounded):
        """Numerical overflow."""

    class Underflow(Inexact, Rounded, Subnormal):
        """Numerical underflow with result rounded to 0."""

    class FloatOperation(DecimalException, TypeError):
        """Enable stricter semantics for mixing floats and Decimals."""

    for _c in (DecimalException, Clamped, InvalidOperation, ConversionSyntax, DivisionByZero, DivisionImpossible,
               DivisionUndefined, Inexact, InvalidContext, Rounded, Subnormal, Overflow, Underflow, FloatOperation):
        _c.__module__ = 'decimal'
    del _c
else:
    class DecimalException(ArithmeticError):
        """Base exception class."""

    class Clamped(DecimalException):
        """Exponent of a 0 changed to fit bounds."""

    class InvalidOperation(DecimalException):
        """An invalid operation was performed."""

    class ConversionSyntax(InvalidOperation):
        """Trying to convert badly formed string."""

    class DivisionByZero(ZeroDivisionError):
        """Division by 0."""

    class DivisionImpossible(InvalidOperation):
        """Cannot perform the division adequately."""

    class DivisionUndefined(ZeroDivisionError):
        """Undefined result of division."""

    class Inexact(DecimalException):
        """Had to round, losing information."""

    class InvalidContext(InvalidOperation):
        """Invalid context."""

    class Rounded(DecimalException):
        """Number got rounded (not  necessarily changed during rounding)."""

    class Subnormal(DecimalException):
        """Exponent < Emin before rounding."""

    class Overflow(Inexact):
        """Numerical overflow."""

    class Underflow(Inexact):
        """Numerical underflow with result rounded to 0."""

    class FloatOperation(DecimalException):
        """Enable stricter semantics for mixing floats and Decimals."""

# the signals a context has flags and traps for, in CPython's order (repr)
_SIGNAL_NAMES = ["InvalidOperation", "FloatOperation", "DivisionByZero", "Overflow", "Underflow", "Subnormal",
                 "Inexact", "Rounded", "Clamped"]
# conditions that are signalled as InvalidOperation
_CONDITION_MAP = {"ConversionSyntax": "InvalidOperation", "DivisionImpossible": "InvalidOperation",
                  "DivisionUndefined": "InvalidOperation", "InvalidContext": "InvalidOperation"}


# the conditions in the order the C module lists the trapped ones in its exceptions
_TRAP_LIST_ORDER = ["InvalidOperation", "ConversionSyntax", "DivisionImpossible", "DivisionUndefined", "InvalidContext",
                    "FloatOperation", "DivisionByZero", "Overflow", "Underflow", "Subnormal", "Inexact", "Rounded",
                    "Clamped"]


def _raise_signal(name: str, msg: str) -> None:
    """Raise signal name with the message msg (the C decimal module's: the list of the trapped conditions)."""
    if name == "InvalidOperation":
        raise InvalidOperation(msg)
    if name == "FloatOperation":
        raise FloatOperation(msg)
    if name == "DivisionByZero":
        raise DivisionByZero(msg)
    if name == "Overflow":
        raise Overflow(msg)
    if name == "Underflow":
        raise Underflow(msg)
    if name == "Subnormal":
        raise Subnormal(msg)
    if name == "Inexact":
        raise Inexact(msg)
    if name == "Rounded":
        raise Rounded(msg)
    raise Clamped(msg)


_SIGNALS_KEY_MSG = ("valid values for signals are:\n  [InvalidOperation, FloatOperation, DivisionByZero,\n"
                    "   Overflow, Underflow, Subnormal, Inexact, Rounded,\n   Clamped]")


def _signal_name(sig: _S) -> str:
    """The name of a signal class (a context's flags / traps are by name; here also a name itself)."""
    if isinstance(sig, str):
        n = sig
    elif isinstance(sig, int):                   # (compiled: the items of an empty list)
        raise KeyError(_SIGNALS_KEY_MSG)
    else:
        n = sig.__name__
    if n not in _SIGNAL_NAMES:
        raise KeyError(_SIGNALS_KEY_MSG)
    return n


# libmpdec's status bits of the conditions: a Context's repr lists the flags in their order (InvalidOperation
# where the first condition that set it is; set from Python: ConversionSyntax's place)
_MPD_BIT = {"Clamped": 0, "ConversionSyntax": 1, "DivisionByZero": 2, "DivisionImpossible": 3, "DivisionUndefined": 4,
            "Inexact": 6, "InvalidContext": 7, "InvalidOperation": 8, "FloatOperation": 10, "Overflow": 11,
            "Rounded": 12, "Subnormal": 13, "Underflow": 14}


class _Signals:
    """A context's flags or traps: signal -> bool (indexed by the signal classes)."""

    def __init__(self, on: list[str]) -> None:
        self._on: dict[str, bool] = {}
        for n in _SIGNAL_NAMES:
            self._on[n] = n in on
        self._inv = 1 if "InvalidOperation" in on else 99

    def __getitem__(self, sig: _S) -> bool:
        return self._on[_signal_name(sig)]

    def __setitem__(self, sig: _S, value: _V) -> None:
        self.put(_signal_name(sig), bool(value))

    def get(self, name: str) -> bool:
        return self._on[name]

    def put(self, name: str, value: bool) -> None:
        self._on[name] = value
        if name == "InvalidOperation":
            self._inv = 1 if value else 99

    def happened(self, cond: str) -> None:
        """The condition cond happened: its signal's flag is set."""
        if cond in _CONDITION_MAP or cond == "InvalidOperation":
            self._on["InvalidOperation"] = True
            if _MPD_BIT[cond] < self._inv:
                self._inv = _MPD_BIT[cond]
        else:
            self._on[cond] = True

    def copy(self) -> "_Signals":
        s = _Signals([])
        for n in _SIGNAL_NAMES:
            s._on[n] = self._on[n]
        s._inv = self._inv
        return s

    def repr_names(self) -> list[str]:
        """The names of the signals that are on, in the order of a Context's repr."""
        out: list[str] = []
        for b in range(15):
            for n in _SIGNAL_NAMES:
                if self._on[n] and (self._inv if n == "InvalidOperation" else _MPD_BIT[n]) == b:
                    out.append(n)
        return out

    def names(self) -> list[str]:
        """The names of the signals that are on, in CPython's order."""
        return [n for n in _SIGNAL_NAMES if self._on[n]]

    def __len__(self) -> int:
        return len(_SIGNAL_NAMES)

    def __repr__(self) -> str:
        parts = ["<class 'decimal." + n + "'>:" + str(self._on[n]) for n in _SIGNAL_NAMES]
        return "{" + ", ".join(parts) + "}"

    if not sys._compiled:
        def __iter__(self):
            for n in _SIGNAL_NAMES:
                yield globals()[n]

        def keys(self):
            return list(self)

        def values(self):
            return [self._on[n] for n in _SIGNAL_NAMES]

        def items(self):
            return [(globals()[n], self._on[n]) for n in _SIGNAL_NAMES]

        def __contains__(self, sig):
            return getattr(sig, "__name__", None) in self._on

        def __eq__(self, other):
            if isinstance(other, _Signals):
                return self._on == other._on
            if isinstance(other, dict):
                return all(bool(other.get(globals()[n], False)) == self._on[n] for n in _SIGNAL_NAMES)
            return NotImplemented


def _signals_of(arg: _T, default_on: list[str]) -> _Signals:
    """A _Signals from Context()'s flags= / traps= argument (None: default_on; a list of signals; a dict of all)."""
    if arg is None:
        return _Signals(default_on)
    elif isinstance(arg, _Signals):
        return arg
    elif isinstance(arg, dict):
        s = _Signals([])
        if len(arg) != len(_SIGNAL_NAMES):
            raise KeyError("invalid signal dict")
        for k in arg:
            if isinstance(k, str):
                n = k
            elif isinstance(k, int):
                raise KeyError("invalid signal dict")
            else:
                n = k.__name__
            if n not in _SIGNAL_NAMES:
                raise KeyError("invalid signal dict")
            s.put(n, bool(arg[k]))
        return s
    elif isinstance(arg, list):
        on: list[str] = []
        for k in arg:
            on.append(_signal_name(k))
        return _Signals(on)
    else:
        raise TypeError("argument must be a signal dict")

# ---------------------------------------------------------------- integers of any size

_BASE = 1000000000
_BASE_DIGITS = 9


class _BigInt:
    """An integer of any size (ints here have 64 bits): sign and base-10**9 limbs, the lowest first."""

    def __init__(self, neg: bool, mag: list[int]) -> None:
        while mag and mag[-1] == 0:
            mag.pop()
        self.neg = neg and len(mag) > 0
        self.mag = mag

    # ---- conversions

    def __str__(self) -> str:
        if not self.mag:
            return "0"
        parts: list[str] = [str(self.mag[-1])]
        for k in range(len(self.mag) - 2, -1, -1):
            s = str(self.mag[k])
            parts.append("0" * (_BASE_DIGITS - len(s)) + s)
        return ("-" if self.neg else "") + "".join(parts)

    def __repr__(self) -> str:
        return self.__str__()

    def digits(self) -> str:
        """The decimal digits of the absolute value."""
        if not self.mag:
            return "0"
        parts: list[str] = [str(self.mag[-1])]
        for k in range(len(self.mag) - 2, -1, -1):
            s = str(self.mag[k])
            parts.append("0" * (_BASE_DIGITS - len(s)) + s)
        return "".join(parts)

    def fits(self) -> bool:
        """Whether it is an int here (64-bit)."""
        if len(self.mag) < 3:
            return True
        if len(self.mag) > 3:
            return False
        low = self.mag[1] * _BASE + self.mag[0]
        return self.mag[2] < 9 or (self.mag[2] == 9 and (low <= 223372036854775807 or (self.neg and low == 223372036854775808)))

    def __int__(self) -> int:
        if not self.fits():
            raise OverflowError("int too large to convert (ints are 64-bit here)")
        v = 0
        for k in range(len(self.mag) - 1, -1, -1):
            v = v * _BASE - self.mag[k]                # (negative: -2**63 fits too)
        return v if self.neg else -v

    def __bool__(self) -> bool:
        return len(self.mag) > 0

    def __hash__(self) -> int:
        if self.fits():
            return hash(int(self))
        return hash(str(self))

    def is_neg(self) -> bool:
        return self.neg

    def ndigits(self) -> int:
        """The number of decimal digits of the absolute value (0: 1)."""
        if not self.mag:
            return 1
        return (len(self.mag) - 1) * _BASE_DIGITS + len(str(self.mag[-1]))

    def bit_length(self) -> int:
        if not self.mag:
            return 0
        n = 0
        m = list(self.mag)
        while len(m) > 1:
            m, r = _divmod_small(m, 1 << 30)
            n += 30
        v = m[0] if m else 0
        return n + v.bit_length()

    def _bits(self) -> list[int]:
        """The binary digits of the absolute value, the highest first."""
        out: list[int] = []
        m = list(self.mag)
        while m:
            m, chunk = _divmod_small(m, 1 << 30)
            for k in range(30):
                out.append(chunk & 1)
                chunk >>= 1
        while len(out) > 1 and out[-1] == 0:
            out.pop()
        out.reverse()
        return out

    # ---- arithmetic

    def __neg__(self) -> "_BigInt":
        return _BigInt(not self.neg, list(self.mag))

    def __abs__(self) -> "_BigInt":
        return _BigInt(False, list(self.mag))

    def __add__(self, other: _O) -> "_BigInt":
        o = _big(other)
        if self.neg == o.neg:
            return _BigInt(self.neg, _add_mag(self.mag, o.mag))
        c = _cmp_mag(self.mag, o.mag)
        if c == 0:
            return _BigInt(False, [])
        if c > 0:
            return _BigInt(self.neg, _sub_mag(self.mag, o.mag))
        return _BigInt(o.neg, _sub_mag(o.mag, self.mag))

    def __radd__(self, other: _O) -> "_BigInt":
        return self.__add__(other)

    def __sub__(self, other: _O) -> "_BigInt":
        return self.__add__(-_big(other))

    def __rsub__(self, other: _O) -> "_BigInt":
        return _big(other).__add__(-self)

    def __mul__(self, other: _O) -> "_BigInt":
        o = _big(other)
        return _BigInt(self.neg != o.neg, _mul_mag(self.mag, o.mag))

    def __rmul__(self, other: _O) -> "_BigInt":
        return self.__mul__(other)

    def divmod_(self, other: _O) -> "tuple[_BigInt, _BigInt]":
        o = _big(other)
        if not o.mag:
            raise ZeroDivisionError("integer division or modulo by zero")
        q, r = _divmod_mag(self.mag, o.mag)
        qb = _BigInt(self.neg != o.neg, q)
        rb = _BigInt(self.neg, r)
        if rb.mag and self.neg != o.neg:          # (floor division: a remainder of the divisor's sign)
            qb = qb - 1
            rb = rb + o
        return (qb, rb)

    def __floordiv__(self, other: _O) -> "_BigInt":
        return self.divmod_(other)[0]

    def __rfloordiv__(self, other: _O) -> "_BigInt":
        return _big(other).divmod_(self)[0]

    def __mod__(self, other: _O) -> "_BigInt":
        return self.divmod_(other)[1]

    def __rmod__(self, other: _O) -> "_BigInt":
        return _big(other).divmod_(self)[1]

    def __pow__(self, e: int) -> "_BigInt":
        if e < 0:
            raise ValueError("negative exponent")
        result = _BigInt(False, [1])
        base = self
        while e:
            if e & 1:
                result = result * base
            e >>= 1
            if e:
                base = base * base
        return result

    def __lshift__(self, n: int) -> "_BigInt":
        return self * (_BigInt(False, [1]) * _pow2(n))

    def __rshift__(self, n: int) -> "_BigInt":
        return self.divmod_(_pow2(n))[0]

    def _cmp(self, other: _O) -> int:
        o = _big(other)
        if self.neg != o.neg:
            return -1 if self.neg else 1
        c = _cmp_mag(self.mag, o.mag)
        return -c if self.neg else c

    def __eq__(self, other: _O) -> bool:
        return self._cmp(other) == 0

    def __ne__(self, other: _O) -> bool:
        return self._cmp(other) != 0

    def __lt__(self, other: _O) -> bool:
        return self._cmp(other) < 0

    def __le__(self, other: _O) -> bool:
        return self._cmp(other) <= 0

    def __gt__(self, other: _O) -> bool:
        return self._cmp(other) > 0

    def __ge__(self, other: _O) -> bool:
        return self._cmp(other) >= 0


def _bint(v: int) -> _BigInt:
    """v as a _BigInt."""
    if v < 0:
        if v == -9223372036854775807 - 1:
            return _bstr("-9223372036854775808")
        b = _bint(-v)
        return _BigInt(True, b.mag)
    mag: list[int] = []
    while v:
        mag.append(v % _BASE)
        v //= _BASE
    return _BigInt(False, mag)


def _bstr(s: str) -> _BigInt:
    """The _BigInt of a string of decimal digits (an optional leading '-')."""
    neg = s.startswith("-")
    if neg:
        s = s[1:]
    mag: list[int] = []
    end = len(s)
    while end > 0:
        start = max(0, end - _BASE_DIGITS)
        mag.append(int(s[start:end]))
        end = start
    return _BigInt(neg, mag)


def _big(x: _O) -> _BigInt:
    if isinstance(x, _BigInt):
        return x
    elif isinstance(x, bool):
        return _bint(1 if x else 0)
    elif isinstance(x, int):
        return _bint(x)
    else:
        return _bstr(str(x))


def _pow2(n: int) -> _BigInt:
    if n < 62:
        return _bint(1 << n)
    return _bint(1 << 30) ** (n // 30) * _bint(1 << (n % 30))


def _pow10(n: int) -> _BigInt:
    return _bstr("1" + "0" * n)


def _cmp_mag(a: list[int], b: list[int]) -> int:
    if len(a) != len(b):
        return -1 if len(a) < len(b) else 1
    for k in range(len(a) - 1, -1, -1):
        if a[k] != b[k]:
            return -1 if a[k] < b[k] else 1
    return 0


def _add_mag(a: list[int], b: list[int]) -> list[int]:
    if len(a) < len(b):
        a, b = b, a
    out: list[int] = []
    carry = 0
    for k in range(len(a)):
        s = a[k] + (b[k] if k < len(b) else 0) + carry
        if s >= _BASE:
            out.append(s - _BASE)
            carry = 1
        else:
            out.append(s)
            carry = 0
    if carry:
        out.append(carry)
    return out


def _sub_mag(a: list[int], b: list[int]) -> list[int]:
    """a - b (a >= b)."""
    out: list[int] = []
    borrow = 0
    for k in range(len(a)):
        s = a[k] - (b[k] if k < len(b) else 0) - borrow
        if s < 0:
            out.append(s + _BASE)
            borrow = 1
        else:
            out.append(s)
            borrow = 0
    while out and out[-1] == 0:
        out.pop()
    return out


def _mul_mag(a: list[int], b: list[int]) -> list[int]:
    if not a or not b:
        return []
    out = [0] * (len(a) + len(b))
    for i in range(len(a)):
        ai = a[i]
        if ai == 0:
            continue
        carry = 0
        for j in range(len(b)):
            cur = out[i + j] + ai * b[j] + carry
            carry = cur // _BASE
            out[i + j] = cur - carry * _BASE
        k = i + len(b)
        while carry:
            cur = out[k] + carry
            carry = cur // _BASE
            out[k] = cur - carry * _BASE
            k += 1
    while out and out[-1] == 0:
        out.pop()
    return out


def _mul_small(a: list[int], m: int) -> list[int]:
    out: list[int] = []
    carry = 0
    for x in a:
        cur = x * m + carry
        carry = cur // _BASE
        out.append(cur - carry * _BASE)
    while carry:
        out.append(carry % _BASE)
        carry //= _BASE
    return out


def _divmod_small(a: list[int], d: int) -> tuple[list[int], int]:
    out = [0] * len(a)
    r = 0
    for k in range(len(a) - 1, -1, -1):
        cur = r * _BASE + a[k]
        q = cur // d
        out[k] = q
        r = cur - q * d
    while out and out[-1] == 0:
        out.pop()
    return (out, r)


def _divmod_mag(a: list[int], b: list[int]) -> tuple[list[int], list[int]]:
    """(a // b, a % b) of magnitudes (b not 0): Knuth's algorithm D."""
    if _cmp_mag(a, b) < 0:
        return ([], list(a))
    if len(b) == 1:
        q, r = _divmod_small(a, b[0])
        return (q, [r] if r else [])
    d = _BASE // (b[-1] + 1)
    u = _mul_small(a, d)
    v = _mul_small(b, d)
    while len(u) < len(a) + 1:
        u.append(0)
    n = len(v)
    m = len(u) - n
    q = [0] * m
    vtop = v[n - 1]
    vnext = v[n - 2]
    for j in range(m - 1, -1, -1):
        num = u[j + n] * _BASE + u[j + n - 1]
        qhat = num // vtop
        rhat = num - qhat * vtop
        while qhat >= _BASE or qhat * vnext > rhat * _BASE + u[j + n - 2]:
            qhat -= 1
            rhat += vtop
            if rhat >= _BASE:
                break
        borrow = 0
        carry = 0
        for i in range(n):
            p = qhat * v[i] + carry
            carry = p // _BASE
            t = u[i + j] - (p - carry * _BASE) - borrow
            if t < 0:
                u[i + j] = t + _BASE
                borrow = 1
            else:
                u[i + j] = t
                borrow = 0
        t = u[j + n] - carry - borrow
        if t < 0:
            u[j + n] = t + _BASE
            qhat -= 1                                 # (one too many: add the divisor back)
            c = 0
            for i in range(n):
                s = u[i + j] + v[i] + c
                if s >= _BASE:
                    u[i + j] = s - _BASE
                    c = 1
                else:
                    u[i + j] = s
                    c = 0
            u[j + n] = (u[j + n] + c) % _BASE
        else:
            u[j + n] = t
        q[j] = qhat
    while q and q[-1] == 0:
        q.pop()
    rem = u[:n]
    while rem and rem[-1] == 0:
        rem.pop()
    rq: list[int] = []
    if rem:
        rq, _ = _divmod_small(rem, d)
    return (q, rq)


# ---------------------------------------------------------------- the arithmetic of coefficients

class _WorkRep:
    """A finite value as (sign, coefficient, exponent) with a coefficient to compute with."""

    def __init__(self, sign: int, coeff: _BigInt, exp: int) -> None:
        self.sign = sign
        self.int = coeff
        self.exp = exp


def _work(d: "Decimal") -> _WorkRep:
    return _WorkRep(d._sign, _bstr(d._int), d._exp)


def _normalize(op1: _WorkRep, op2: _WorkRep, prec: int) -> None:
    """Normalizes op1, op2 to have the same exp and length of coefficient (for addition)."""
    if op1.exp < op2.exp:
        tmp = op2
        other = op1
    else:
        tmp = op1
        other = op2
    tmp_len = tmp.int.ndigits()
    other_len = other.int.ndigits()
    exp = tmp.exp + min(-1, tmp_len - prec - 2)
    if other_len + other.exp - 1 < exp:
        other.int = _bint(1)
        other.exp = exp
    tmp.int = tmp.int * _pow10(tmp.exp - other.exp)
    tmp.exp = other.exp


def _odd(x: _BigInt) -> int:
    return x.mag[0] & 1 if x.mag else 0


def _sqrt_nearest(n: _BigInt, a: _BigInt) -> _BigInt:
    """Closest integer to the square root of the positive integer n (a: an initial approximation)."""
    if n <= 0 or a <= 0:
        raise ValueError("Both arguments to _sqrt_nearest should be positive.")
    b = _bint(0)
    while a != b:
        b = a
        a = (a - (-n) // a) >> 1
    return a


def _rshift_nearest(x: _BigInt, shift: int) -> _BigInt:
    """x / 2**shift rounded to the nearest integer, ties to even."""
    b = _pow2(shift)
    q, r = x.divmod_(b)
    if r * 2 + _odd(q) > b:
        return q + 1
    return q


def _div_nearest(a: _BigInt, b: _BigInt) -> _BigInt:
    """a / b rounded to the nearest integer, ties to even (b > 0)."""
    q, r = a.divmod_(b)
    if r * 2 + _odd(q) > b:
        return q + 1
    return q


def _ilog(x: _BigInt, M: _BigInt, L: int = 8) -> _BigInt:
    """Integer approximation to M*log(x/M), with absolute error boundable in terms only of x/M."""
    y = x - M
    R = 0
    while (R <= L and abs(y) * _pow2(L - R) >= M) or (R > L and abs(y) // _pow2(R - L) >= M):
        y = _div_nearest((M * y) * 2, M + _sqrt_nearest(M * (M + _rshift_nearest(y, R)), M))
        R += 1
    T = -int(-10 * M.ndigits() // (3 * L))
    yshift = _rshift_nearest(y, R)
    w = _div_nearest(M, _bint(T))
    for k in range(T - 1, 0, -1):
        w = _div_nearest(M, _bint(k)) - _div_nearest(yshift * w, M)
    return _div_nearest(w * y, M)


def _dlog10(c: _BigInt, e: int, p: int) -> _BigInt:
    """An integer approximation to 10**p * log10(c*10**e) (c > 0), with an error of at most 1."""
    p += 2
    l = c.ndigits()
    f = e + l - (1 if e + l >= 1 else 0)
    if p > 0:
        M = _pow10(p)
        k = e + p - f
        if k >= 0:
            c = c * _pow10(k)
        else:
            c = _div_nearest(c, _pow10(-k))
        log_d = _ilog(c, M)
        log_10 = _log10_digits(p)
        log_d = _div_nearest(log_d * M, log_10)
        log_tenpower = _bint(f) * M
    else:
        log_d = _bint(0)
        log_tenpower = _div_nearest(_bint(f), _pow10(-p))
    return _div_nearest(log_tenpower + log_d, _bint(100))


def _dlog(c: _BigInt, e: int, p: int) -> _BigInt:
    """An integer approximation to 10**p * log(c*10**e) (c > 0), with an error of at most 1."""
    p += 2
    l = c.ndigits()
    f = e + l - (1 if e + l >= 1 else 0)
    if p > 0:
        k = e + p - f
        if k >= 0:
            c = c * _pow10(k)
        else:
            c = _div_nearest(c, _pow10(-k))
        log_d = _ilog(c, _pow10(p))
    else:
        log_d = _bint(0)
    if f:
        extra = len(str(abs(f))) - 1
        if p + extra >= 0:
            f_log_ten = _div_nearest(_bint(f) * _log10_digits(p + extra), _pow10(extra))
        else:
            f_log_ten = _bint(0)
    else:
        f_log_ten = _bint(0)
    return _div_nearest(f_log_ten + log_d, _bint(100))


_LOG10_DIGITS = ["23025850929940456840179914546843642076011014886"]


def _log10_digits(p: int) -> _BigInt:
    """The first p+1 digits of 10*ln(10) as an integer (more of them worked out when needed)."""
    if p < 0:
        raise ValueError("p should be nonnegative")
    if p >= len(_LOG10_DIGITS[0]):
        extra = 3
        digits = ""
        while True:
            M = _pow10(p + extra + 2)
            digits = str(_div_nearest(_ilog(M * 10, M), _bint(100)))
            if digits[-extra:] != '0' * extra:
                break
            extra += 3
        _LOG10_DIGITS[0] = digits.rstrip('0')[:-1]
    return _bstr(_LOG10_DIGITS[0][:p + 1])


def _iexp(x: _BigInt, M: _BigInt, L: int = 8) -> _BigInt:
    """M*exp(x/M) to the nearest integer, for |x| < 2.4*M (x, M integers)."""
    R = ((x * _pow2(L)) // M).bit_length()
    T = -int(-10 * M.ndigits() // (3 * L))
    y = _div_nearest(x, _bint(T))
    Mshift = M * _pow2(R)
    for i in range(T - 1, 0, -1):
        y = _div_nearest(x * (Mshift + y), Mshift * i)
    for k in range(R - 1, -1, -1):
        Mshift = M * _pow2(k + 2)
        y = _div_nearest(y * (y + Mshift), Mshift)
    return M + y


def _dexp(c: _BigInt, e: int, p: int) -> tuple[_BigInt, int]:
    """(d, f): d*10**f approximates exp(c*10**e) with d of p digits (error < 1 in its last one)."""
    p += 2
    extra = max(0, e + c.ndigits() - 1)
    q = p + extra
    shift = e + q
    if shift >= 0:
        cshift = c * _pow10(shift)
    else:
        cshift = c // _pow10(-shift)
    quot, rem = cshift.divmod_(_log10_digits(q))
    rem = _div_nearest(rem, _pow10(extra))
    return (_div_nearest(_iexp(rem, _pow10(p)), _bint(1000)), int(quot) - p + 3)


def _pow_lb_zeta(x: "Decimal") -> int:
    """libmpdec's lower bound for the adjusted exponent of log10(x) (x > 0 finite, not 1): its power bounds."""
    t = x.adjusted()
    if t > 0:
        return len(str(t)) - 1
    if t < -1:
        return len(str(-(t + 1))) - 1
    c = _bstr(x._int)
    if x._exp >= 0:
        diff = c * _pow10(x._exp) - 1
        e = 0
    else:
        diff = c - _pow10(-x._exp)
        e = x._exp
    u = diff.ndigits() - 1 + e
    return u - 2 if t == 0 else u - 1


def _all_zeros(s: str, start: int) -> bool:
    for k in range(start, len(s)):
        if s[k] != '0':
            return False
    return True


def _exact_half(s: str, start: int) -> bool:
    """s[start:] is 5 followed by zeros."""
    if start >= len(s) or s[start] != '5':
        return False
    return _all_zeros(s, start + 1)


# CPython's hash of numbers (sys.hash_info): modulus 2**61 - 1
_HASH_MODULUS = 2305843009213693951
_HASH_INF = 314159
_HASH_10INV = 2075258708292324556


def _mulmod(a: int, b: int) -> int:
    """a * b % _HASH_MODULUS without leaving 64 bits."""
    r = 0
    a %= _HASH_MODULUS
    while b:
        if b & 1:
            r = (r + a) % _HASH_MODULUS
        a = (a * 2) % _HASH_MODULUS
        b >>= 1
    return r


def _powmod(a: int, e: int) -> int:
    r = 1
    a %= _HASH_MODULUS
    while e:
        if e & 1:
            r = _mulmod(r, a)
        a = _mulmod(a, a)
        e >>= 1
    return r


# ---------------------------------------------------------------- Decimal

def _parse_decimal(s: str) -> tuple[bool, int, str, int, str]:
    """(ok, sign, digits, exponent, special) of a numeric string (special: '', 'F', 'n', 'N')."""
    s = s.strip().replace("_", "")
    if not s.isascii():
        s = _ascii_digits(s)
    i = 0
    n = len(s)
    sign = 0
    if i < n and (s[i] == '+' or s[i] == '-'):
        sign = 1 if s[i] == '-' else 0
        i += 1
    rest = s[i:]
    low = rest.lower()
    if low == "inf" or low == "infinity":
        return (True, sign, "0", 0, "F")
    if low.startswith("nan") or low.startswith("snan"):
        signal = low.startswith("s")
        diag = rest[4:] if signal else rest[3:]
        for ch in diag:
            if not ('0' <= ch <= '9'):
                return (False, 0, "", 0, "")
        return (True, sign, diag.lstrip('0'), 0, "N" if signal else "n")
    intpart = ""
    while i < n and '0' <= s[i] <= '9':
        intpart += s[i]
        i += 1
    fracpart = ""
    if i < n and s[i] == '.':
        i += 1
        while i < n and '0' <= s[i] <= '9':
            fracpart += s[i]
            i += 1
    if not intpart and not fracpart:
        return (False, 0, "", 0, "")
    exp = 0
    if i < n and (s[i] == 'e' or s[i] == 'E'):
        i += 1
        esign = 1
        if i < n and (s[i] == '+' or s[i] == '-'):
            esign = -1 if s[i] == '-' else 1
            i += 1
        start = i
        while i < n and '0' <= s[i] <= '9':
            i += 1
        if i == start:
            return (False, 0, "", 0, "")
        exp = esign * int(s[start:i])
    if i != n:
        return (False, 0, "", 0, "")
    digits = (intpart + fracpart).lstrip('0') or '0'
    return (True, sign, digits, exp - len(fracpart), "")


class Decimal:
    """Floating point class for decimal arithmetic."""

    def __init__(self, value: _V = "0", context: "Context | None" = None) -> None:
        """Decimal(value="0", context=None): a str, an int, a float, a Decimal or a (sign, digits, exponent)
        tuple."""
        self._sign = 0
        self._int = "0"
        self._exp = 0
        self._special = ""
        self._is_special = False
        if isinstance(value, str):
            ok, sign, digits, exp, special = _parse_decimal(value)
            if not ok:
                ctx = context if context is not None else getcontext()
                bad = ctx._raise_error("ConversionSyntax", "Invalid literal for Decimal: %r" % value)
                self._set(bad)
                return
            self._sign = sign
            self._int = digits
            self._exp = exp
            self._special = special
            self._is_special = special != ""
        elif isinstance(value, bool):
            self._int = "1" if value else "0"
        elif isinstance(value, int):
            self._sign = 1 if value < 0 else 0
            self._int = str(value)[1:] if value < 0 else str(value)
        elif isinstance(value, Decimal):
            self._set(value)
        elif isinstance(value, float):
            ctx = context if context is not None else getcontext()
            ctx._raise_error("FloatOperation", "strict semantics for mixing floats and Decimals are enabled")
            self._set(Decimal.from_float(value))
        elif isinstance(value, tuple) or isinstance(value, list):
            self._set(_from_tuple(value))
        elif isinstance(value, DecimalTuple):
            self._set(_from_dtuple(value))
        else:
            raise TypeError("conversion from %s to Decimal is not supported" % type(value).__name__)

    def _set(self, d: "Decimal") -> None:
        self._sign = d._sign
        self._int = d._int
        self._exp = d._exp
        self._special = d._special
        self._is_special = d._is_special

    @classmethod
    def from_float(cls, f: _V) -> "Decimal":
        """Converts a float to a decimal number, exactly."""
        if isinstance(f, int):
            return Decimal(f)
        elif isinstance(f, float):
            if _math.isinf(f) or _math.isnan(f):
                return Decimal(repr(f))
            sign = 0 if _math.copysign(1.0, f) == 1.0 else 1
            m, e = _math.frexp(abs(f))
            if m == 0.0:
                return _dec_from_triple(sign, "0", 0)
            mant = int(m * 9007199254740992.0)          # (m * 2**53: an integer)
            e -= 53
            while mant % 2 == 0 and e < 0:
                mant //= 2
                e += 1
            if e >= 0:
                return _dec_from_triple(sign, (_bint(mant) * _pow2(e)).digits(), 0)
            return _dec_from_triple(sign, (_bint(mant) * _bint(5) ** (-e)).digits(), e)
        else:
            raise TypeError("argument must be int or float.")

    @classmethod
    def from_number(cls, number: _V) -> "Decimal":
        """Converts a real number (an int, a float or a Decimal) to a decimal number, exactly."""
        if isinstance(number, str):
            raise TypeError("conversion from str to Decimal is not supported")
        return Decimal(number)

    # ---- kinds of values

    def _isnan(self) -> int:
        """0: a number, 1: NaN, 2: sNaN."""
        if self._special == 'n':
            return 1
        if self._special == 'N':
            return 2
        return 0

    def _isinfinity(self) -> int:
        """0: finite or NaN, 1: +Infinity, -1: -Infinity."""
        if self._special == 'F':
            return -1 if self._sign else 1
        return 0

    def _check_nans(self, other: "Decimal | None" = None, context: "Context | None" = None) -> "Decimal | None":
        """The NaN result of an operation with a NaN operand (None: none of them a NaN)."""
        self_is_nan = self._isnan()
        other_is_nan = other._isnan() if other is not None else 0
        if self_is_nan or other_is_nan:
            ctx = context if context is not None else getcontext()
            if self_is_nan == 2:
                return ctx._raise_error("InvalidOperation", 'sNaN', 0, self)
            if other is not None and other_is_nan == 2:
                return ctx._raise_error("InvalidOperation", 'sNaN', 0, other)
            if self_is_nan:
                return self._fix_nan(ctx)
            if other is not None:
                return other._fix_nan(ctx)
        return None

    def _compare_check_nans(self, other: "Decimal", context: "Context | None") -> "Decimal | None":
        ctx = context if context is not None else getcontext()
        if self._is_special or other._is_special:
            if self.is_snan():
                return ctx._raise_error("InvalidOperation", 'comparison involving sNaN', 0, self)
            elif other.is_snan():
                return ctx._raise_error("InvalidOperation", 'comparison involving sNaN', 0, other)
            elif self.is_qnan():
                return ctx._raise_error("InvalidOperation", 'comparison involving NaN', 0, self)
            elif other.is_qnan():
                return ctx._raise_error("InvalidOperation", 'comparison involving NaN', 0, other)
        return None

    def __bool__(self) -> bool:
        return self._is_special or self._int != '0'

    def _cmp(self, other: "Decimal") -> int:
        """-1, 0 or 1 (neither a NaN)."""
        if self._is_special or other._is_special:
            self_inf = self._isinfinity()
            other_inf = other._isinfinity()
            if self_inf == other_inf:
                return 0
            return -1 if self_inf < other_inf else 1
        if not self:
            if not other:
                return 0
            return 1 if other._sign else -1
        if not other:
            return -1 if self._sign else 1
        if other._sign < self._sign:
            return -1
        if self._sign < other._sign:
            return 1
        self_adjusted = self.adjusted()
        other_adjusted = other.adjusted()
        if self_adjusted == other_adjusted:
            self_padded = self._int + '0' * (self._exp - other._exp)
            other_padded = other._int + '0' * (other._exp - self._exp)
            if self_padded == other_padded:
                return 0
            if self_padded < other_padded:
                return 1 if self._sign else -1
            return -1 if self._sign else 1
        elif self_adjusted > other_adjusted:
            return -1 if self._sign else 1
        return 1 if self._sign else -1

    def __eq__(self, other: _O, context: "Context | None" = None) -> bool:
        o = _convert_for_comparison(other, True)
        if o is None:
            return False
        if self._check_nans(o, context) is not None:
            return False
        return self._cmp(o) == 0

    def __ne__(self, other: _O, context: "Context | None" = None) -> bool:
        return not self.__eq__(other, context)

    def _order(self, other: _O, op: str, context: "Context | None") -> int:
        """self against other for <, <=, >, >=: -1, 0, 1 or 2 (a NaN: False)."""
        o = _convert_for_comparison(other, False)
        if o is None:
            raise TypeError("'" + op + "' not supported between instances of 'decimal.Decimal' and '"
                            + type(other).__name__ + "'")
        if self._compare_check_nans(o, context) is not None:
            return 2
        return self._cmp(o)

    def __lt__(self, other: _O, context: "Context | None" = None) -> bool:
        c = self._order(other, "<", context)
        return c != 2 and c < 0

    def __le__(self, other: _O, context: "Context | None" = None) -> bool:
        c = self._order(other, "<=", context)
        return c != 2 and c <= 0

    def __gt__(self, other: _O, context: "Context | None" = None) -> bool:
        c = self._order(other, ">", context)
        return c != 2 and c > 0

    def __ge__(self, other: _O, context: "Context | None" = None) -> bool:
        c = self._order(other, ">=", context)
        return c != 2 and c >= 0

    def compare(self, other: _O, context: "Context | None" = None) -> "Decimal":
        """Compare self to other: Decimal(-1), Decimal(0) or Decimal(1) (NaN: a NaN)."""
        o = _convert_other(other, True)
        if self._is_special or (o and o._is_special):
            ans = self._check_nans(o, context)
            if ans is not None:
                return ans
        return Decimal(self._cmp(o))

    def __hash__(self) -> int:
        """x.__hash__() <==> hash(x)"""
        if self._is_special:
            if self.is_snan():
                raise TypeError('Cannot hash a signaling NaN value')
            elif self.is_nan():
                return hash(id(self))
            return -_HASH_INF if self._sign else _HASH_INF
        if self._exp >= 0:
            exp_hash = _powmod(10, self._exp)
        else:
            exp_hash = _powmod(_HASH_10INV, -self._exp)
        v = 0
        for ch in self._int:
            v = (_mulmod(v, 10) + ord(ch) - 48) % _HASH_MODULUS
        h = _mulmod(v, exp_hash)
        ans = -h if self._sign else h
        return -2 if ans == -1 else ans

    def as_tuple(self) -> "DecimalTuple":
        """Represents the number as a triple tuple: DecimalTuple(sign, digits, exponent)."""
        return _decimal_tuple(self)

    def as_integer_ratio(self) -> tuple[int, int]:
        """(n, d): a pair of integers whose ratio is exactly the value (d > 0, in lowest terms)."""
        if self._is_special:
            if self.is_nan():
                raise ValueError("cannot convert NaN to integer ratio")
            raise OverflowError("cannot convert Infinity to integer ratio")
        if not self:
            return (0, 1)
        n = _bstr(self._int)
        if self._exp >= 0:
            n = n * _pow10(self._exp)
            d = _bint(1)
        else:
            d5 = -self._exp
            while d5 > 0 and n % 5 == 0:
                n = n // 5
                d5 -= 1
            d2 = -self._exp
            while d2 > 0 and _odd(n) == 0:
                n = n // 2
                d2 -= 1
            d = _bint(5) ** d5 * _pow2(d2)
        if self._sign:
            n = -n
        return (int(n), int(d))

    def __repr__(self) -> str:
        """Represents the number as an instance of Decimal."""
        return "Decimal('" + str(self) + "')"

    def __str__(self) -> str:
        """Return string representation of the number in scientific notation."""
        return self._to_str(False, None)

    def _to_str(self, eng: bool, context: "Context | None") -> str:
        sign = '-' if self._sign else ''
        if self._is_special:
            if self._special == 'F':
                return sign + 'Infinity'
            elif self._special == 'n':
                return sign + 'NaN' + self._int
            return sign + 'sNaN' + self._int
        leftdigits = self._exp + len(self._int)
        if self._exp <= 0 and leftdigits > -6:
            dotplace = leftdigits
        elif not eng:
            dotplace = 1
        elif self._int == '0':
            dotplace = (leftdigits + 1) % 3 - 1
        else:
            dotplace = (leftdigits - 1) % 3 + 1
        if dotplace <= 0:
            intpart = '0'
            fracpart = '.' + '0' * (-dotplace) + self._int
        elif dotplace >= len(self._int):
            intpart = self._int + '0' * (dotplace - len(self._int))
            fracpart = ''
        else:
            intpart = self._int[:dotplace]
            fracpart = '.' + self._int[dotplace:]
        if leftdigits == dotplace:
            exp = ''
        else:
            ctx = context if context is not None else getcontext()
            e = leftdigits - dotplace
            exp = ('E' if ctx.capitals else 'e') + ('+' if e >= 0 else '-') + str(abs(e))
        return sign + intpart + fracpart + exp

    def to_eng_string(self, context: "Context | None" = None) -> str:
        """Convert to a string, using engineering notation if an exponent is needed."""
        return self._to_str(True, context)

    def __neg__(self, context: "Context | None" = None) -> "Decimal":
        """Returns a copy with the sign switched (rounded for the context)."""
        if self._is_special:
            ans = self._check_nans(None, context)
            if ans is not None:
                return ans
        ctx = context if context is not None else getcontext()
        if not self and ctx.rounding != ROUND_FLOOR:
            res = self.copy_abs()
        else:
            res = self.copy_negate()
        return res._fix(ctx)

    def __pos__(self, context: "Context | None" = None) -> "Decimal":
        """Returns a copy, unless it is a sNaN (rounded for the context)."""
        if self._is_special:
            ans = self._check_nans(None, context)
            if ans is not None:
                return ans
        ctx = context if context is not None else getcontext()
        if not self and ctx.rounding != ROUND_FLOOR:
            res = self.copy_abs()
        else:
            res = Decimal(self)
        return res._fix(ctx)

    def __abs__(self, round: bool = True, context: "Context | None" = None) -> "Decimal":
        """Returns the absolute value of self (rounded for the context unless round is False)."""
        if not round:
            return self.copy_abs()
        if self._is_special:
            ans = self._check_nans(None, context)
            if ans is not None:
                return ans
        if self._sign:
            return self.__neg__(context)
        return self.__pos__(context)

    # ---- arithmetic

    def __add__(self, other: _O, context: "Context | None" = None) -> "Decimal":
        """Returns self + other."""
        return self._add(_operand(other, "+", "decimal.Decimal", True), context)

    def __radd__(self, other: _O, context: "Context | None" = None) -> "Decimal":
        return self._add(_operand(other, "+", "decimal.Decimal", False), context)

    def _add(self, other: "Decimal", context: "Context | None") -> "Decimal":
        ctx = context if context is not None else getcontext()
        if self._is_special or other._is_special:
            ans = self._check_nans(other, ctx)
            if ans is not None:
                return ans
            if self._isinfinity():
                if self._sign != other._sign and other._isinfinity():
                    return ctx._raise_error("InvalidOperation", '-INF + INF')
                return Decimal(self)
            if other._isinfinity():
                return Decimal(other)
        exp = min(self._exp, other._exp)
        negativezero = 0
        if ctx.rounding == ROUND_FLOOR and self._sign != other._sign:
            negativezero = 1
        if not self and not other:
            sign = min(self._sign, other._sign)
            if negativezero:
                sign = 1
            return _dec_from_triple(sign, '0', exp)._fix(ctx)
        if not self:
            exp = max(exp, other._exp - ctx.prec - 1)
            return other._rescale(exp, ctx.rounding)._fix(ctx)
        if not other:
            exp = max(exp, self._exp - ctx.prec - 1)
            return self._rescale(exp, ctx.rounding)._fix(ctx)
        op1 = _work(self)
        op2 = _work(other)
        _normalize(op1, op2, ctx.prec)
        result_sign = 0
        if op1.sign != op2.sign:
            if op1.int == op2.int:
                return _dec_from_triple(negativezero, '0', exp)._fix(ctx)
            if op1.int < op2.int:
                op1, op2 = op2, op1
            if op1.sign == 1:
                result_sign = 1
                op1.sign, op2.sign = op2.sign, op1.sign
            else:
                result_sign = 0
        elif op1.sign == 1:
            result_sign = 1
            op1.sign = 0
            op2.sign = 0
        else:
            result_sign = 0
        if op2.sign == 0:
            total = op1.int + op2.int
        else:
            total = op1.int - op2.int
        return _dec_from_triple(result_sign, total.digits(), op1.exp)._fix(ctx)

    def __sub__(self, other: _O, context: "Context | None" = None) -> "Decimal":
        """Return self - other."""
        o = _operand(other, "-", "decimal.Decimal", True)
        if self._is_special or o._is_special:
            ans = self._check_nans(o, context)
            if ans is not None:
                return ans
        return self._add(o.copy_negate(), context)

    def __rsub__(self, other: _O, context: "Context | None" = None) -> "Decimal":
        """Return other - self."""
        return _operand(other, "-", "decimal.Decimal", False).__sub__(self, context)

    def __mul__(self, other: _O, context: "Context | None" = None) -> "Decimal":
        """Return self * other."""
        return self._mul(_operand(other, "*", "decimal.Decimal", True), context)

    def __rmul__(self, other: _O, context: "Context | None" = None) -> "Decimal":
        return self._mul(_operand(other, "*", "decimal.Decimal", False), context)

    def _mul(self, other: "Decimal", context: "Context | None") -> "Decimal":
        ctx = context if context is not None else getcontext()
        resultsign = self._sign ^ other._sign
        if self._is_special or other._is_special:
            ans = self._check_nans(other, ctx)
            if ans is not None:
                return ans
            if self._isinfinity():
                if not other:
                    return ctx._raise_error("InvalidOperation", '(+-)INF * 0')
                return _signed_infinity(resultsign)
            if other._isinfinity():
                if not self:
                    return ctx._raise_error("InvalidOperation", '0 * (+-)INF')
                return _signed_infinity(resultsign)
        resultexp = self._exp + other._exp
        if not self or not other:
            return _dec_from_triple(resultsign, '0', resultexp)._fix(ctx)
        if self._int == '1':
            return _dec_from_triple(resultsign, other._int, resultexp)._fix(ctx)
        if other._int == '1':
            return _dec_from_triple(resultsign, self._int, resultexp)._fix(ctx)
        prod = _bstr(self._int) * _bstr(other._int)
        return _dec_from_triple(resultsign, prod.digits(), resultexp)._fix(ctx)

    def __truediv__(self, other: _O, context: "Context | None" = None) -> "Decimal":
        """Return self / other."""
        return self._truediv(_operand(other, "/", "decimal.Decimal", True), context)

    def __rtruediv__(self, other: _O, context: "Context | None" = None) -> "Decimal":
        """Return other / self."""
        return _operand(other, "/", "decimal.Decimal", False)._truediv(self, context)

    def _truediv(self, other: "Decimal", context: "Context | None") -> "Decimal":
        ctx = context if context is not None else getcontext()
        sign = self._sign ^ other._sign
        if self._is_special or other._is_special:
            ans = self._check_nans(other, ctx)
            if ans is not None:
                return ans
            if self._isinfinity() and other._isinfinity():
                return ctx._raise_error("InvalidOperation", '(+-)INF/(+-)INF')
            if self._isinfinity():
                return _signed_infinity(sign)
            if other._isinfinity():
                ctx._raise_error("Clamped", 'Division by infinity')
                return _dec_from_triple(sign, '0', ctx.Etiny())
        if not other:
            if not self:
                return ctx._raise_error("DivisionUndefined", '0 / 0')
            return ctx._raise_error("DivisionByZero", 'x / 0', sign)
        if not self:
            exp = self._exp - other._exp
            coeff = _bint(0)
        else:
            shift = len(other._int) - len(self._int) + ctx.prec + 1
            exp = self._exp - other._exp - shift
            a = _bstr(self._int)
            b = _bstr(other._int)
            if shift >= 0:
                coeff, remainder = (a * _pow10(shift)).divmod_(b)
            else:
                coeff, remainder = a.divmod_(b * _pow10(-shift))
            if remainder:
                if coeff % 5 == 0:
                    coeff = coeff + 1
            else:
                ideal_exp = self._exp - other._exp
                while exp < ideal_exp and coeff % 10 == 0:
                    coeff = coeff // 10
                    exp += 1
        return _dec_from_triple(sign, coeff.digits(), exp)._fix(ctx)

    def _divide(self, other: "Decimal", context: "Context") -> "tuple[Decimal, Decimal]":
        """Return (self // other, self % other), to context.prec precision."""
        sign = self._sign ^ other._sign
        if other._isinfinity():
            ideal_exp = self._exp
        else:
            ideal_exp = min(self._exp, other._exp)
        expdiff = self.adjusted() - other.adjusted()
        if not self or other._isinfinity() or expdiff <= -2:
            return (_dec_from_triple(sign, '0', 0), self._rescale(ideal_exp, context.rounding))
        if expdiff <= context.prec:
            a = _bstr(self._int)
            b = _bstr(other._int)
            if self._exp >= other._exp:
                a = a * _pow10(self._exp - other._exp)
            else:
                b = b * _pow10(other._exp - self._exp)
            q, r = a.divmod_(b)
            if q < _pow10(context.prec):
                return (_dec_from_triple(sign, q.digits(), 0), _dec_from_triple(self._sign, r.digits(), ideal_exp))
        ans = context._raise_error("DivisionImpossible", 'quotient too large in //, % or divmod')
        return (ans, ans)

    def __divmod__(self, other: _O, context: "Context | None" = None) -> "tuple[Decimal, Decimal]":
        """Return (self // other, self % other)."""
        return self._divmod(_operand(other, "divmod()", "decimal.Decimal", True), context)

    def __rdivmod__(self, other: _O, context: "Context | None" = None) -> "tuple[Decimal, Decimal]":
        return _operand(other, "divmod()", "decimal.Decimal", False)._divmod(self, context)

    def _divmod(self, other: "Decimal", context: "Context | None") -> "tuple[Decimal, Decimal]":
        ctx = context if context is not None else getcontext()
        ans = self._check_nans(other, ctx)
        if ans is not None:
            return (ans, ans)
        sign = self._sign ^ other._sign
        if self._isinfinity():
            if other._isinfinity():
                ans2 = ctx._raise_error("InvalidOperation", 'divmod(INF, INF)')
                return (ans2, ans2)
            return (_signed_infinity(sign), ctx._raise_error("InvalidOperation", 'INF % x'))
        if not other:
            if not self:
                ans3 = ctx._raise_error("DivisionUndefined", 'divmod(0, 0)')
                return (ans3, ans3)
            ctx._signal(["DivisionByZero", "InvalidOperation"])
            return (_signed_infinity(sign), _dec_from_triple(0, '', 0, 'n'))
        quotient, remainder = self._divide(other, ctx)
        conds: list[str] = []
        if not other._isinfinity():                  # (x // INF: an unrounded 0, as the C module has it)
            quotient = quotient._fix_to(ctx, conds)
        remainder = remainder._fix_to(ctx, conds)
        ctx._signal(conds)
        return (quotient, remainder)

    def __mod__(self, other: _O, context: "Context | None" = None) -> "Decimal":
        """self % other"""
        return self._mod(_operand(other, "%", "decimal.Decimal", True), context)

    def __rmod__(self, other: _O, context: "Context | None" = None) -> "Decimal":
        return _operand(other, "%", "decimal.Decimal", False)._mod(self, context)

    def _mod(self, other: "Decimal", context: "Context | None") -> "Decimal":
        ctx = context if context is not None else getcontext()
        ans = self._check_nans(other, ctx)
        if ans is not None:
            return ans
        if self._isinfinity():
            return ctx._raise_error("InvalidOperation", 'INF % x')
        elif not other:
            if self:
                return ctx._raise_error("InvalidOperation", 'x % 0')
            return ctx._raise_error("DivisionUndefined", '0 % 0')
        remainder = self._divide(other, ctx)[1]
        return remainder._fix(ctx)

    def remainder_near(self, other: _O, context: "Context | None" = None) -> "Decimal":
        """Remainder nearest to 0-  abs(remainder-near) <= other/2"""
        ctx = context if context is not None else getcontext()
        o = _convert_other(other, True)
        ans = self._check_nans(o, ctx)
        if ans is not None:
            return ans
        if self._isinfinity():
            return ctx._raise_error("InvalidOperation", 'remainder_near(infinity, x)')
        if not o:
            if self:
                return ctx._raise_error("InvalidOperation", 'remainder_near(x, 0)')
            return ctx._raise_error("DivisionUndefined", 'remainder_near(0, 0)')
        if o._isinfinity():
            return Decimal(self)._fix(ctx)
        ideal_exponent = min(self._exp, o._exp)
        if not self:
            return _dec_from_triple(self._sign, '0', ideal_exponent)._fix(ctx)
        expdiff = self.adjusted() - o.adjusted()
        if expdiff >= ctx.prec + 1:
            return ctx._raise_error("DivisionImpossible")
        if expdiff <= -2:
            return self._rescale(ideal_exponent, ctx.rounding)._fix(ctx)
        a = _bstr(self._int)
        b = _bstr(o._int)
        if self._exp >= o._exp:
            a = a * _pow10(self._exp - o._exp)
        else:
            b = b * _pow10(o._exp - self._exp)
        q, r = a.divmod_(b)
        if r * 2 + _odd(q) > b:
            r = r - b
            q = q + 1
        if q >= _pow10(ctx.prec):
            return ctx._raise_error("DivisionImpossible")
        sign = self._sign
        if r < 0:
            sign = 1 - sign
            r = -r
        return _dec_from_triple(sign, r.digits(), ideal_exponent)._fix(ctx)

    def __floordiv__(self, other: _O, context: "Context | None" = None) -> "Decimal":
        """self // other"""
        return self._floordiv(_operand(other, "//", "decimal.Decimal", True), context)

    def __rfloordiv__(self, other: _O, context: "Context | None" = None) -> "Decimal":
        return _operand(other, "//", "decimal.Decimal", False)._floordiv(self, context)

    def _floordiv(self, other: "Decimal", context: "Context | None") -> "Decimal":
        ctx = context if context is not None else getcontext()
        ans = self._check_nans(other, ctx)
        if ans is not None:
            return ans
        if self._isinfinity():
            if other._isinfinity():
                return ctx._raise_error("InvalidOperation", 'INF // INF')
            return _signed_infinity(self._sign ^ other._sign)
        if not other:
            if self:
                return ctx._raise_error("DivisionByZero", 'x // 0', self._sign ^ other._sign)
            return ctx._raise_error("DivisionUndefined", '0 // 0')
        if other._isinfinity():
            return self._divide(other, ctx)[0]
        return self._divide(other, ctx)[0]._fix(ctx)

    def __float__(self) -> float:
        """Float representation."""
        if self._isnan():
            if self.is_snan():
                raise ValueError("cannot convert signaling NaN to float")
            return float("-nan") if self._sign else float("nan")
        return float(str(self))

    def __int__(self) -> int:
        """Converts self to an int, truncating if necessary."""
        if self._is_special:
            if self._isnan():
                raise ValueError("cannot convert NaN to integer")
            raise OverflowError("cannot convert Infinity to integer")
        return int(_bstr(("-" if self._sign else "") + _trunc_digits(self)))

    def __trunc__(self) -> int:
        return self.__int__()

    @property
    def real(self) -> "Decimal":
        return self

    @property
    def imag(self) -> "Decimal":
        return Decimal(0)

    def conjugate(self) -> "Decimal":
        return self

    def _fix_nan(self, context: "Context") -> "Decimal":
        """Decapitate the payload of a NaN to fit the context"""
        payload = self._int
        max_payload_len = context.prec - context.clamp
        if len(payload) > max_payload_len:
            payload = payload[len(payload) - max_payload_len:].lstrip('0')
            return _dec_from_triple(self._sign, payload, 0, self._special)
        return Decimal(self)

    def _fix(self, context: "Context", pre: list[str] | None = None) -> "Decimal":
        """Round if it is necessary to keep self within prec precision (and Emin / Emax); signals what happened
        all at once (with the conditions pre of the operation), as the C module does."""
        conds: list[str] = pre if pre is not None else []
        ans = self._fix_to(context, conds)
        if conds:
            context._signal(conds)
        return ans

    def _fix_to(self, context: "Context", conds: list[str]) -> "Decimal":
        if self._is_special:
            if self._isnan():
                return self._fix_nan(context)
            return Decimal(self)
        Etiny = context.Etiny()
        Etop = context.Etop()
        if not self:
            exp_max = Etop if context.clamp else context.Emax
            new_exp = min(max(self._exp, Etiny), exp_max)
            if new_exp != self._exp:
                conds.append("Clamped")
                return _dec_from_triple(self._sign, '0', new_exp)
            return Decimal(self)
        exp_min = len(self._int) + self._exp - context.prec
        if exp_min > Etop:
            conds.append("Overflow")
            conds.append("Inexact")
            conds.append("Rounded")
            return context._overflow_value(self._sign)
        self_is_subnormal = exp_min < Etiny
        if self_is_subnormal:
            exp_min = Etiny
        if self._exp < exp_min:
            digits = len(self._int) + self._exp - exp_min
            me = self
            if digits < 0:
                me = _dec_from_triple(self._sign, '1', exp_min - 1)
                digits = 0
            changed = me._round_by(context.rounding, digits)
            coeff = me._int[:digits] or '0'
            if changed > 0:
                coeff = (_bstr(coeff) + 1).digits()
                if len(coeff) > context.prec:
                    coeff = coeff[:-1]
                    exp_min += 1
            if exp_min > Etop:
                conds.append("Overflow")
                ans = context._overflow_value(self._sign)
            else:
                ans = _dec_from_triple(self._sign, coeff, exp_min)
            if changed and self_is_subnormal:
                conds.append("Underflow")
            if self_is_subnormal:
                conds.append("Subnormal")
            if changed:
                conds.append("Inexact")
            conds.append("Rounded")
            if not ans:
                conds.append("Clamped")
            return ans
        if self_is_subnormal:
            conds.append("Subnormal")
        if context.clamp == 1 and self._exp > Etop:
            conds.append("Clamped")
            self_padded = self._int + '0' * (self._exp - Etop)
            return _dec_from_triple(self._sign, self_padded, Etop)
        return Decimal(self)

    def _round_by(self, rounding: str, prec: int) -> int:
        """How rounding the coefficient to prec digits goes: 1 up, -1 down (truncated), 0 exact."""
        if rounding == ROUND_DOWN:
            return 0 if _all_zeros(self._int, prec) else -1
        if rounding == ROUND_UP:
            return 0 if _all_zeros(self._int, prec) else 1
        if rounding == ROUND_HALF_UP:
            return self._round_half_up(prec)
        if rounding == ROUND_HALF_DOWN:
            if _exact_half(self._int, prec):
                return -1
            return self._round_half_up(prec)
        if rounding == ROUND_HALF_EVEN:
            if _exact_half(self._int, prec) and (prec == 0 or self._int[prec - 1] in '02468'):
                return -1
            return self._round_half_up(prec)
        down = 0 if _all_zeros(self._int, prec) else -1
        if rounding == ROUND_CEILING:
            return down if self._sign else -down
        if rounding == ROUND_FLOOR:
            return -down if self._sign else down
        if prec and self._int[prec - 1] not in '05':        # ROUND_05UP
            return down
        return -down

    def _round_half_up(self, prec: int) -> int:
        if prec < len(self._int) and self._int[prec] in '56789':
            return 1
        elif _all_zeros(self._int, prec):
            return 0
        return -1

    def __round__(self, n: int | None = None) -> int:
        """round(self) (an int; round(self, n): a Decimal)."""
        if n is not None:
            if not sys._compiled:
                return self.__mpy_round_n(n)
            raise TypeError("round(Decimal, n): compiled code calls __mpy_round_n")
        if self._is_special:
            if self.is_nan():
                raise ValueError("cannot convert NaN to integer")
            raise OverflowError("cannot convert Infinity to integer")
        return int(self._rescale(0, ROUND_HALF_EVEN))

    def __mpy_round_n(self, n: int) -> "Decimal":
        """round(self, n) (the compiler calls it for round's two-argument form)."""
        return self.quantize(_dec_from_triple(0, '1', -n))

    def __floor__(self) -> int:
        """Return the floor of self, as an integer."""
        if self._is_special:
            if self.is_nan():
                raise ValueError("cannot convert NaN to integer")
            raise OverflowError("cannot convert Infinity to integer")
        return int(self._rescale(0, ROUND_FLOOR))

    def __ceil__(self) -> int:
        """Return the ceiling of self, as an integer."""
        if self._is_special:
            if self.is_nan():
                raise ValueError("cannot convert NaN to integer")
            raise OverflowError("cannot convert Infinity to integer")
        return int(self._rescale(0, ROUND_CEILING))

    def fma(self, other: _O, third: _V, context: "Context | None" = None) -> "Decimal":
        """Fused multiply-add: self*other+third with no rounding of the intermediate product."""
        o = _convert_other(other, True)
        t = _convert_other(third, True)
        product = Decimal(0)
        if self._is_special or o._is_special:
            ctx = context if context is not None else getcontext()
            if self._special == 'N':
                return ctx._raise_error("InvalidOperation", 'sNaN', 0, self)
            if o._special == 'N':
                return ctx._raise_error("InvalidOperation", 'sNaN', 0, o)
            if self._special == 'n':
                product = self
            elif o._special == 'n':
                product = o
            elif self._special == 'F':
                if not o:
                    return ctx._raise_error("InvalidOperation", 'INF * 0 in fma')
                product = _signed_infinity(self._sign ^ o._sign)
            elif o._special == 'F':
                if not self:
                    return ctx._raise_error("InvalidOperation", '0 * INF in fma')
                product = _signed_infinity(self._sign ^ o._sign)
        else:
            product = _dec_from_triple(self._sign ^ o._sign, (_bstr(self._int) * _bstr(o._int)).digits(),
                                       self._exp + o._exp)
        return product._add(t, context)

    def _power_modulo(self, other: "Decimal", modulo: "Decimal", context: "Context | None") -> "Decimal":
        """Three argument version of __pow__"""
        ctx = context if context is not None else getcontext()
        self_is_nan = self._isnan()
        other_is_nan = other._isnan()
        modulo_is_nan = modulo._isnan()
        if self_is_nan or other_is_nan or modulo_is_nan:
            if self_is_nan == 2:
                return ctx._raise_error("InvalidOperation", 'sNaN', 0, self)
            if other_is_nan == 2:
                return ctx._raise_error("InvalidOperation", 'sNaN', 0, other)
            if modulo_is_nan == 2:
                return ctx._raise_error("InvalidOperation", 'sNaN', 0, modulo)
            if self_is_nan:
                return self._fix_nan(ctx)
            if other_is_nan:
                return other._fix_nan(ctx)
            return modulo._fix_nan(ctx)
        if not (self._isinteger() and other._isinteger() and modulo._isinteger()):
            return ctx._raise_error("InvalidOperation", 'pow() 3rd argument not allowed unless all arguments are integers')
        if other < 0:
            return ctx._raise_error("InvalidOperation", 'pow() 2nd argument cannot be negative when 3rd argument specified')
        if not modulo:
            return ctx._raise_error("InvalidOperation", 'pow() 3rd argument cannot be 0')
        if modulo.adjusted() >= ctx.prec:
            return ctx._raise_error("InvalidOperation", 'insufficient precision: pow() 3rd argument must not have more than precision digits')
        if not other and not self:
            return ctx._raise_error("InvalidOperation", 'at least one of pow() 1st argument and 2nd argument must be nonzero; 0**0 is not defined')
        if other._iseven():
            sign = 0
        else:
            sign = self._sign
        modv = _bstr(_trunc_digits(modulo))
        base = _work(self.to_integral_value())
        expo = _work(other.to_integral_value())
        b = (base.int % modv) * _powmod_big(_bint(10), _bint(base.exp), modv) % modv
        for i in range(expo.exp):
            b = _powmod_big(b, _bint(10), modv)
        b = _powmod_big(b, expo.int, modv)
        return _dec_from_triple(sign, b.digits(), 0)

    def __pow__(self, other: _O, modulo: _V = None, context: "Context | None" = None) -> "Decimal":
        """Return self ** other [ % modulo]."""
        o = _operand(other, "** or pow()", "decimal.Decimal", True)
        if modulo is not None:
            return self._power_modulo(o, _convert_other(modulo, True), context)
        return self._pow(o, context)

    def __rpow__(self, other: _O, modulo: _V = None, context: "Context | None" = None) -> "Decimal":
        """Swaps self/other and returns __pow__."""
        return _operand(other, "** or pow()", "decimal.Decimal", False)._pow(self, context)

    def _pow(self, other: "Decimal", context: "Context | None") -> "Decimal":
        ctx = context if context is not None else getcontext()
        ans = self._check_nans(other, ctx)
        if ans is not None:
            return ans
        if not other:
            if not self:
                return ctx._raise_error("InvalidOperation", '0 ** 0')
            return Decimal(1)
        result_sign = 0
        me = self
        if self._sign == 1:
            if other._isinteger():
                if not other._iseven():
                    result_sign = 1
            else:
                if self:
                    return ctx._raise_error("InvalidOperation", 'x ** y with x negative and y not an integer')
            me = self.copy_negate()
        if not me:
            if other._sign == 0:
                return _dec_from_triple(result_sign, '0', 0)
            return _signed_infinity(result_sign)
        if me._isinfinity():
            if other._sign == 0:
                return _signed_infinity(result_sign)
            return _dec_from_triple(result_sign, '0', 0)
        if me == 1:
            if other._isinteger():
                if other._sign == 1:
                    multiplier = 0
                elif other > ctx.prec:
                    multiplier = ctx.prec
                else:
                    multiplier = int(other)
                exp = me._exp * multiplier
                if exp < 1 - ctx.prec:
                    exp = 1 - ctx.prec
                    ctx._raise_error("Rounded")
            else:
                ctx._signal(["Inexact", "Rounded"])
                exp = 1 - ctx.prec
            return _dec_from_triple(result_sign, '1' + '0' * (-exp), exp)
        self_adj = me.adjusted()
        if other._isinfinity():
            if (other._sign == 0) == (self_adj < 0):
                return _dec_from_triple(result_sign, '0', 0)
            return _signed_infinity(result_sign)
        # from here as the C module (libmpdec) has it: certain overflow / underflow, else an integer power
        # by squaring (a few more digits, ROUND_HALF_EVEN) or exp(y * ln(x)); then rounded for the context
        bound = _pow_lb_zeta(me) + other.adjusted()
        if (self_adj >= 0) == (other._sign == 0):
            if len(str(ctx.Emax)) < bound:
                return _dec_from_triple(result_sign, '1', ctx.Emax + 1)._fix(ctx)
        else:
            Etiny = ctx.Etiny()
            if len(str(-Etiny)) < bound:
                return _dec_from_triple(result_sign, '1', Etiny - 1)._fix(ctx)
        status = _Signals([])
        wc = Context(prec=ctx.prec, rounding=ROUND_HALF_EVEN, Emin=ctx.Emin, Emax=ctx.Emax, clamp=0, flags=status,
                     traps=_Signals([]))
        if other._isinteger():
            wc.prec = ctx.prec + len(other._int) + other._exp + 2
            tbase = me
            if other._sign:
                wc.prec = wc.prec + 1
                tbase = Decimal(1)._truediv(me, wc)
            bits = _bstr(_trunc_digits(other))._bits()
            res = tbase
            for k in range(1, len(bits)):
                res = res._mul(res, wc)
                if bits[k]:
                    res = res._mul(tbase, wc)
                if res._is_special or (not res and status.get("Clamped")):
                    break
            if res._is_special:
                res = _dec_from_triple(result_sign, '1', ctx.Emax + 1)
            else:
                res = _dec_from_triple(result_sign, res._int, res._exp)
        else:
            wc.prec = max(len(me._int), ctx.prec) + 23
            wc.Emax = MAX_EMAX
            wc.Emin = MIN_EMIN
            res = me.ln(wc)._mul(other, wc).exp(wc)
            status.put("Inexact", True)
            status.put("Rounded", True)
            if res == 1:
                res = _dec_from_triple(0, '1' + '0' * (ctx.prec - 1), 1 - ctx.prec)
            elif res._is_special:
                res = _dec_from_triple(0, '1', ctx.Emax + 1)
        fc = ctx.copy()
        fc.clear_flags()
        fc.clear_traps()
        res = res._fix(fc)
        ctx._signal([name for name in _SIGNAL_NAMES if status.get(name) or fc.flags.get(name)])
        return res


    def normalize(self, context: "Context | None" = None) -> "Decimal":
        """Normalize- strip trailing 0s, change anything equal to 0 to 0e0"""
        ctx = context if context is not None else getcontext()
        if self._is_special:
            ans = self._check_nans(None, ctx)
            if ans is not None:
                return ans
        dup = self._fix(ctx)
        if dup._isinfinity():
            return dup
        if not dup:
            return _dec_from_triple(dup._sign, '0', 0)
        exp_max = ctx.Etop() if ctx.clamp else ctx.Emax
        end = len(dup._int)
        exp = dup._exp
        while dup._int[end - 1] == '0' and exp < exp_max:
            exp += 1
            end -= 1
        return _dec_from_triple(dup._sign, dup._int[:end], exp)

    def quantize(self, exp: _O, rounding: str | None = None, context: "Context | None" = None) -> "Decimal":
        """Quantize self so its exponent is the same as that of exp."""
        e = _convert_other(exp, True)
        ctx = context if context is not None else getcontext()
        rnd = rounding if rounding is not None else ctx.rounding
        if self._is_special or e._is_special:
            ans = self._check_nans(e, ctx)
            if ans is not None:
                return ans
            if e._isinfinity() or self._isinfinity():
                if e._isinfinity() and self._isinfinity():
                    return Decimal(self)
                return ctx._raise_error("InvalidOperation", 'quantize with one INF')
        if not (ctx.Etiny() <= e._exp <= ctx.Emax):
            return ctx._raise_error("InvalidOperation", 'target exponent out of bounds in quantize')
        if not self:
            return _dec_from_triple(self._sign, '0', e._exp)._fix(ctx)
        self_adjusted = self.adjusted()
        if self_adjusted > ctx.Emax:
            return ctx._raise_error("InvalidOperation", 'exponent of quantize result too large for current context')
        if self_adjusted - e._exp + 1 > ctx.prec:
            return ctx._raise_error("InvalidOperation", 'quantize result has too many digits for current context')
        ans = self._rescale(e._exp, rnd)
        if ans.adjusted() > ctx.Emax:
            return ctx._raise_error("InvalidOperation", 'exponent of quantize result too large for current context')
        if len(ans._int) > ctx.prec:
            return ctx._raise_error("InvalidOperation", 'quantize result has too many digits for current context')
        conds: list[str] = []
        if ans and ans.adjusted() < ctx.Emin:
            conds.append("Subnormal")
        if ans._exp > self._exp:
            if ans != self:
                conds.append("Inexact")
            conds.append("Rounded")
        return ans._fix(ctx, conds)

    def same_quantum(self, other: _O, context: "Context | None" = None) -> bool:
        """Return True if self and other have the same exponent; otherwise return False."""
        o = _convert_other(other, True)
        if self._is_special or o._is_special:
            return (self.is_nan() and o.is_nan()) or (self.is_infinite() and o.is_infinite())
        return self._exp == o._exp

    def _rescale(self, exp: int, rounding: str) -> "Decimal":
        """Rescale self so that the exponent is exp, either by padding with zeros or by truncating digits."""
        if self._is_special:
            return Decimal(self)
        if not self:
            return _dec_from_triple(self._sign, '0', exp)
        if self._exp >= exp:
            return _dec_from_triple(self._sign, self._int + '0' * (self._exp - exp), exp)
        digits = len(self._int) + self._exp - exp
        me = self
        if digits < 0:
            me = _dec_from_triple(self._sign, '1', exp - 1)
            digits = 0
        changed = me._round_by(rounding, digits)
        coeff = me._int[:digits] or '0'
        if changed == 1:
            coeff = (_bstr(coeff) + 1).digits()
        return _dec_from_triple(self._sign, coeff, exp)

    def _round(self, places: int, rounding: str) -> "Decimal":
        """Round a nonzero, nonspecial Decimal to a fixed number of significant figures."""
        if places <= 0:
            raise ValueError("argument should be at least 1 in _round")
        if self._is_special or not self:
            return Decimal(self)
        ans = self._rescale(self.adjusted() + 1 - places, rounding)
        if ans.adjusted() != self.adjusted():
            ans = ans._rescale(ans.adjusted() + 1 - places, rounding)
        return ans

    def to_integral_exact(self, rounding: str | None = None, context: "Context | None" = None) -> "Decimal":
        """Rounds to a nearby integer (signals Inexact and Rounded as fits)."""
        if self._is_special:
            ans = self._check_nans(None, context)
            if ans is not None:
                return ans
            return Decimal(self)
        if self._exp >= 0:
            return Decimal(self)
        if not self:
            return _dec_from_triple(self._sign, '0', 0)
        ctx = context if context is not None else getcontext()
        rnd = rounding if rounding is not None else ctx.rounding
        ans = self._rescale(0, rnd)
        if ans != self:
            ctx._signal(["Inexact", "Rounded"])
        else:
            ctx._signal(["Rounded"])
        return ans

    def to_integral_value(self, rounding: str | None = None, context: "Context | None" = None) -> "Decimal":
        """Rounds to the nearest integer, without raising inexact, rounded."""
        ctx = context if context is not None else getcontext()
        rnd = rounding if rounding is not None else ctx.rounding
        if self._is_special:
            ans = self._check_nans(None, ctx)
            if ans is not None:
                return ans
            return Decimal(self)
        if self._exp >= 0:
            return Decimal(self)
        return self._rescale(0, rnd)

    def to_integral(self, rounding: str | None = None, context: "Context | None" = None) -> "Decimal":
        return self.to_integral_value(rounding, context)

    def sqrt(self, context: "Context | None" = None) -> "Decimal":
        """Return the square root of self."""
        ctx = context if context is not None else getcontext()
        if self._is_special:
            ans = self._check_nans(None, ctx)
            if ans is not None:
                return ans
            if self._isinfinity() and self._sign == 0:
                return Decimal(self)
        if not self:
            return _dec_from_triple(self._sign, '0', self._exp // 2)._fix(ctx)
        if self._sign == 1:
            return ctx._raise_error("InvalidOperation", 'sqrt(-x), x > 0')
        prec = ctx.prec + 1
        op = _work(self)
        e = op.exp >> 1
        if op.exp & 1:
            c = op.int * 10
            l = (len(self._int) >> 1) + 1
        else:
            c = op.int
            l = (len(self._int) + 1) >> 1
        shift = prec - l
        if shift >= 0:
            c = c * _pow10(2 * shift)
            exact = True
        else:
            c, remainder = c.divmod_(_pow10(-2 * shift))
            exact = not remainder
        e -= shift
        n = _pow10(prec)
        while True:
            q = c // n
            if n <= q:
                break
            n = (n + q) >> 1
        exact = exact and n * n == c
        if exact:
            if shift >= 0:
                n = n // _pow10(shift)
            else:
                n = n * _pow10(-shift)
            e += shift
        else:
            if n % 5 == 0:
                n = n + 1
        ans = _dec_from_triple(0, n.digits(), e)
        c2 = ctx._shallow_copy()
        c2.rounding = ROUND_HALF_EVEN
        return ans._fix(c2)

    def max(self, other: _O, context: "Context | None" = None) -> "Decimal":
        """Returns the larger value (NaN handled per IEEE 754)."""
        return self._maxmin(_convert_other(other, True), context, True, False)

    def min(self, other: _O, context: "Context | None" = None) -> "Decimal":
        """Returns the smaller value (NaN handled per IEEE 754)."""
        return self._maxmin(_convert_other(other, True), context, False, False)

    def max_mag(self, other: _O, context: "Context | None" = None) -> "Decimal":
        """Compares the values numerically with their sign ignored."""
        return self._maxmin(_convert_other(other, True), context, True, True)

    def min_mag(self, other: _O, context: "Context | None" = None) -> "Decimal":
        """Compares the values numerically with their sign ignored."""
        return self._maxmin(_convert_other(other, True), context, False, True)

    def _maxmin(self, other: "Decimal", context: "Context | None", want_max: bool, mag: bool) -> "Decimal":
        ctx = context if context is not None else getcontext()
        if self._is_special or other._is_special:
            sn = self._isnan()
            on = other._isnan()
            if sn or on:
                if on == 1 and sn == 0:
                    return self._fix(ctx)
                if sn == 1 and on == 0:
                    return other._fix(ctx)
                ans = self._check_nans(other, ctx)
                if ans is not None:
                    return ans
        if mag:
            c = self.copy_abs()._cmp(other.copy_abs())
        else:
            c = self._cmp(other)
        if c == 0:
            c = int(self.compare_total(other))
        if want_max:
            ans2 = other if c == -1 else self
        else:
            ans2 = self if c == -1 else other
        return ans2._fix(ctx)

    def _isinteger(self) -> bool:
        """Returns whether self is an integer"""
        if self._is_special:
            return False
        if self._exp >= 0:
            return True
        rest = self._int[self._exp:]
        return rest == '0' * len(rest)

    def _iseven(self) -> bool:
        """Returns True if self is even.  Assumes self is an integer."""
        if not self or self._exp > 0:
            return True
        return self._int[-1 + self._exp] in '02468'

    def adjusted(self) -> int:
        """Return the adjusted exponent of self"""
        if self._is_special:
            return 0
        return self._exp + len(self._int) - 1

    def canonical(self) -> "Decimal":
        """Returns the same Decimal object (it is already canonical)."""
        return self

    def compare_signal(self, other: _O, context: "Context | None" = None) -> "Decimal":
        """Compares self to the other operand numerically (quiet NaNs signal too)."""
        o = _convert_other(other, True)
        ans = self._compare_check_nans(o, context)
        if ans is not None:
            return ans
        return self.compare(o, context)

    def compare_total(self, other: _O, context: "Context | None" = None) -> "Decimal":
        """Compares self to other using the abstract representations (a total order)."""
        o = _convert_other(other, True)
        if self._sign and not o._sign:
            return Decimal(-1)
        if not self._sign and o._sign:
            return Decimal(1)
        sign = self._sign
        self_nan = self._isnan()
        other_nan = o._isnan()
        if self_nan or other_nan:
            if self_nan == other_nan:
                self_key = (len(self._int), self._int)
                other_key = (len(o._int), o._int)
                if self_key < other_key:
                    return Decimal(1) if sign else Decimal(-1)
                if self_key > other_key:
                    return Decimal(-1) if sign else Decimal(1)
                return Decimal(0)
            if sign:
                if self_nan == 1:
                    return Decimal(-1)
                if other_nan == 1:
                    return Decimal(1)
                if self_nan == 2:
                    return Decimal(-1)
                if other_nan == 2:
                    return Decimal(1)
            else:
                if self_nan == 1:
                    return Decimal(1)
                if other_nan == 1:
                    return Decimal(-1)
                if self_nan == 2:
                    return Decimal(1)
                if other_nan == 2:
                    return Decimal(-1)
        if self < o:
            return Decimal(-1)
        if self > o:
            return Decimal(1)
        if self._exp < o._exp:
            return Decimal(1) if sign else Decimal(-1)
        if self._exp > o._exp:
            return Decimal(-1) if sign else Decimal(1)
        return Decimal(0)

    def compare_total_mag(self, other: _O, context: "Context | None" = None) -> "Decimal":
        """Compares self to other using abstract repr., ignoring sign."""
        o = _convert_other(other, True)
        return self.copy_abs().compare_total(o.copy_abs())

    def copy_abs(self) -> "Decimal":
        """Returns a copy with the sign set to 0. """
        return _dec_from_triple(0, self._int, self._exp, self._special)

    def copy_negate(self) -> "Decimal":
        """Returns a copy with the sign inverted."""
        return _dec_from_triple(1 - self._sign, self._int, self._exp, self._special)

    def copy_sign(self, other: _O, context: "Context | None" = None) -> "Decimal":
        """Returns self with the sign of other."""
        o = _convert_other(other, True)
        return _dec_from_triple(o._sign, self._int, self._exp, self._special)

    def exp(self, context: "Context | None" = None) -> "Decimal":
        """Returns e ** self."""
        ctx = context if context is not None else getcontext()
        ans = self._check_nans(None, ctx)
        if ans is not None:
            return ans
        if self._isinfinity() == -1:
            return Decimal(0)
        if not self:
            return Decimal(1)
        if self._isinfinity() == 1:
            return Decimal(self)
        p = ctx.prec
        adj = self.adjusted()
        if self._sign == 0 and adj > len(str((ctx.Emax + 1) * 3)):
            res = _dec_from_triple(0, '1', ctx.Emax + 1)
        elif self._sign == 1 and adj > len(str((-ctx.Etiny() + 1) * 3)):
            res = _dec_from_triple(0, '1', ctx.Etiny() - 1)
        elif self._sign == 0 and adj < -p:
            res = _dec_from_triple(0, '1' + '0' * (p - 1) + '1', -p)
        elif self._sign == 1 and adj < -p - 1:
            res = _dec_from_triple(0, '9' * (p + 1), -p - 1)
        else:
            op = _work(self)
            c = op.int
            e = op.exp
            if op.sign == 1:
                c = -c
            extra = 3
            coeff = _bint(0)
            cexp = 0
            while True:
                coeff, cexp = _dexp(c, e, p + extra)
                if coeff % (_pow10(coeff.ndigits() - p - 1) * 5):
                    break
                extra += 3
            res = _dec_from_triple(0, coeff.digits(), cexp)
        c2 = ctx._shallow_copy()
        c2.rounding = ROUND_HALF_EVEN
        return res._fix(c2)

    def is_canonical(self) -> bool:
        """Return True if self is canonical; otherwise return False."""
        return True

    def is_finite(self) -> bool:
        """Return True if self is finite; otherwise return False."""
        return not self._is_special

    def is_infinite(self) -> bool:
        """Return True if self is infinite; otherwise return False."""
        return self._special == 'F'

    def is_nan(self) -> bool:
        """Return True if self is a qNaN or sNaN; otherwise return False."""
        return self._special == 'n' or self._special == 'N'

    def is_normal(self, context: "Context | None" = None) -> bool:
        """Return True if self is a normal number; otherwise return False."""
        if self._is_special or not self:
            return False
        ctx = context if context is not None else getcontext()
        return ctx.Emin <= self.adjusted()

    def is_qnan(self) -> bool:
        """Return True if self is a quiet NaN; otherwise return False."""
        return self._special == 'n'

    def is_signed(self) -> bool:
        """Return True if self is negative; otherwise return False."""
        return self._sign == 1

    def is_snan(self) -> bool:
        """Return True if self is a signaling NaN; otherwise return False."""
        return self._special == 'N'

    def is_subnormal(self, context: "Context | None" = None) -> bool:
        """Return True if self is subnormal; otherwise return False."""
        if self._is_special or not self:
            return False
        ctx = context if context is not None else getcontext()
        return self.adjusted() < ctx.Emin

    def is_zero(self) -> bool:
        """Return True if self is a zero; otherwise return False."""
        return not self._is_special and self._int == '0'

    def _ln_exp_bound(self) -> int:
        """Compute a lower bound for the adjusted exponent of self.ln()."""
        adj = self._exp + len(self._int) - 1
        if adj >= 1:
            return len(str(adj * 23 // 10)) - 1
        if adj <= -2:
            return len(str((-1 - adj) * 23 // 10)) - 1
        op = _work(self)
        c = op.int
        e = op.exp
        if adj == 0:
            num = str(c - _pow10(-e))
            den = str(c)
            return len(num) - len(den) - (1 if num < den else 0)
        return e + len(str(_pow10(-e) - c)) - 1

    def ln(self, context: "Context | None" = None) -> "Decimal":
        """Returns the natural (base e) logarithm of self."""
        ctx = context if context is not None else getcontext()
        ans = self._check_nans(None, ctx)
        if ans is not None:
            return ans
        if not self:
            return _signed_infinity(1)
        if self._isinfinity() == 1:
            return _signed_infinity(0)
        if self == 1:
            return Decimal(0)
        if self._sign == 1:
            return ctx._raise_error("InvalidOperation", 'ln of a negative value')
        op = _work(self)
        c = op.int
        e = op.exp
        p = ctx.prec
        places = p - self._ln_exp_bound() + 2
        coeff = _bint(0)
        while True:
            coeff = _dlog(c, e, places)
            if coeff % (_pow10(abs(coeff).ndigits() - p - 1) * 5):
                break
            places += 3
        res = _dec_from_triple(1 if coeff < 0 else 0, abs(coeff).digits(), -places)
        c2 = ctx._shallow_copy()
        c2.rounding = ROUND_HALF_EVEN
        return res._fix(c2)

    def _log10_exp_bound(self) -> int:
        """Compute a lower bound for the adjusted exponent of self.log10()."""
        adj = self._exp + len(self._int) - 1
        if adj >= 1:
            return len(str(adj)) - 1
        if adj <= -2:
            return len(str(-1 - adj)) - 1
        op = _work(self)
        c = op.int
        e = op.exp
        if adj == 0:
            num = str(c - _pow10(-e))
            den = str(c * 231)
            return len(num) - len(den) - (1 if num < den else 0) + 2
        num = str(_pow10(-e) - c)
        return len(num) + e - (1 if num < "231" else 0) - 1

    def log10(self, context: "Context | None" = None) -> "Decimal":
        """Returns the base 10 logarithm of self."""
        ctx = context if context is not None else getcontext()
        ans = self._check_nans(None, ctx)
        if ans is not None:
            return ans
        if not self:
            return _signed_infinity(1)
        if self._isinfinity() == 1:
            return _signed_infinity(0)
        if self._sign == 1:
            return ctx._raise_error("InvalidOperation", 'log10 of a negative value')
        if self._int[0] == '1' and self._int[1:] == '0' * (len(self._int) - 1):
            res = Decimal(self._exp + len(self._int) - 1)
        else:
            op = _work(self)
            c = op.int
            e = op.exp
            p = ctx.prec
            places = p - self._log10_exp_bound() + 2
            coeff = _bint(0)
            while True:
                coeff = _dlog10(c, e, places)
                if coeff % (_pow10(abs(coeff).ndigits() - p - 1) * 5):
                    break
                places += 3
            res = _dec_from_triple(1 if coeff < 0 else 0, abs(coeff).digits(), -places)
        c2 = ctx._shallow_copy()
        c2.rounding = ROUND_HALF_EVEN
        return res._fix(c2)

    def logb(self, context: "Context | None" = None) -> "Decimal":
        """ Returns the exponent of the magnitude of self's MSD."""
        ans = self._check_nans(None, context)
        if ans is not None:
            return ans
        ctx = context if context is not None else getcontext()
        if self._isinfinity():
            return _signed_infinity(0)
        if not self:
            return ctx._raise_error("DivisionByZero", 'logb(0)', 1)
        return Decimal(self.adjusted())._fix(ctx)

    def _islogical(self) -> bool:
        if self._sign != 0 or self._exp != 0 or self._is_special:
            return False
        for dig in self._int:
            if dig not in '01':
                return False
        return True

    def _logical(self, other: "Decimal", context: "Context | None", op: str) -> "Decimal":
        ctx = context if context is not None else getcontext()
        if not self._islogical() or not other._islogical():
            return ctx._raise_error("InvalidOperation")
        opa = self._int
        opb = other._int
        dif = ctx.prec - len(opa)
        if dif > 0:
            opa = '0' * dif + opa
        elif dif < 0:
            opa = opa[-ctx.prec:]
        dif = ctx.prec - len(opb)
        if dif > 0:
            opb = '0' * dif + opb
        elif dif < 0:
            opb = opb[-ctx.prec:]
        out: list[str] = []
        for k in range(len(opa)):
            a = opa[k] == '1'
            b = opb[k] == '1'
            if op == "and":
                bit = a and b
            elif op == "or":
                bit = a or b
            else:
                bit = a != b
            out.append('1' if bit else '0')
        return _dec_from_triple(0, "".join(out).lstrip('0') or '0', 0)

    def logical_and(self, other: _O, context: "Context | None" = None) -> "Decimal":
        """Applies an 'and' operation between self and other's digits."""
        return self._logical(_convert_other(other, True), context, "and")

    def logical_or(self, other: _O, context: "Context | None" = None) -> "Decimal":
        """Applies an 'or' operation between self and other's digits."""
        return self._logical(_convert_other(other, True), context, "or")

    def logical_xor(self, other: _O, context: "Context | None" = None) -> "Decimal":
        """Applies an 'xor' operation between self and other's digits."""
        return self._logical(_convert_other(other, True), context, "xor")

    def logical_invert(self, context: "Context | None" = None) -> "Decimal":
        """Invert all its digits."""
        ctx = context if context is not None else getcontext()
        return self._logical(_dec_from_triple(0, '1' * ctx.prec, 0), ctx, "xor")

    def next_minus(self, context: "Context | None" = None) -> "Decimal":
        """Returns the largest representable number smaller than itself."""
        ctx = context if context is not None else getcontext()
        ans = self._check_nans(None, ctx)
        if ans is not None:
            return ans
        if self._isinfinity() == -1:
            return _signed_infinity(1)
        if self._isinfinity() == 1:
            return _dec_from_triple(0, '9' * ctx.prec, ctx.Etop())
        c2 = ctx.copy()
        c2.rounding = ROUND_FLOOR
        c2._ignore_all = True
        new_self = self._fix(c2)
        if new_self != self:
            return new_self
        return self.__sub__(_dec_from_triple(0, '1', c2.Etiny() - 1), c2)

    def next_plus(self, context: "Context | None" = None) -> "Decimal":
        """Returns the smallest representable number larger than itself."""
        ctx = context if context is not None else getcontext()
        ans = self._check_nans(None, ctx)
        if ans is not None:
            return ans
        if self._isinfinity() == 1:
            return _signed_infinity(0)
        if self._isinfinity() == -1:
            return _dec_from_triple(1, '9' * ctx.prec, ctx.Etop())
        c2 = ctx.copy()
        c2.rounding = ROUND_CEILING
        c2._ignore_all = True
        new_self = self._fix(c2)
        if new_self != self:
            return new_self
        return self._add(_dec_from_triple(0, '1', c2.Etiny() - 1), c2)

    def next_toward(self, other: _O, context: "Context | None" = None) -> "Decimal":
        """Returns the number closest to self, in the direction towards other."""
        o = _convert_other(other, True)
        ctx = context if context is not None else getcontext()
        ans = self._check_nans(o, ctx)
        if ans is not None:
            return ans
        comparison = self._cmp(o)
        if comparison == 0:
            return self.copy_sign(o)
        if comparison == -1:
            res = self.next_plus(ctx)
        else:
            res = self.next_minus(ctx)
        if res._isinfinity():
            ctx._signal(["Overflow", "Inexact", "Rounded"])
        elif res.adjusted() < ctx.Emin:
            ctx._signal(["Underflow", "Subnormal", "Inexact", "Rounded", "Clamped"] if not res
                        else ["Underflow", "Subnormal", "Inexact", "Rounded"])
        return res

    def number_class(self, context: "Context | None" = None) -> str:
        """Returns an indication of the class of self."""
        if self.is_snan():
            return "sNaN"
        if self.is_qnan():
            return "NaN"
        inf = self._isinfinity()
        if inf == 1:
            return "+Infinity"
        if inf == -1:
            return "-Infinity"
        if self.is_zero():
            return "-Zero" if self._sign else "+Zero"
        ctx = context if context is not None else getcontext()
        if self.is_subnormal(ctx):
            return "-Subnormal" if self._sign else "+Subnormal"
        return "-Normal" if self._sign else "+Normal"

    def radix(self) -> "Decimal":
        """Just returns 10, as this is Decimal, :)"""
        return Decimal(10)

    def _shift_digits(self, other: "Decimal", context: "Context | None", rotate: bool) -> "Decimal":
        ctx = context if context is not None else getcontext()
        ans = self._check_nans(other, ctx)
        if ans is not None:
            return ans
        if other._exp != 0:
            return ctx._raise_error("InvalidOperation")
        if not (-ctx.prec <= int(other) <= ctx.prec):
            return ctx._raise_error("InvalidOperation")
        if self._isinfinity():
            return Decimal(self)
        torot = int(other)
        rotdig = self._int
        topad = ctx.prec - len(rotdig)
        if topad > 0:
            rotdig = '0' * topad + rotdig
        elif topad < 0:
            rotdig = rotdig[-topad:]
        if rotate:
            shifted = rotdig[torot:] + rotdig[:torot]
        elif torot < 0:
            shifted = rotdig[:torot]
        else:
            shifted = (rotdig + '0' * torot)[-ctx.prec:]
        return _dec_from_triple(self._sign, shifted.lstrip('0') or '0', self._exp)

    def rotate(self, other: _O, context: "Context | None" = None) -> "Decimal":
        """Returns a rotated copy of self, value-of-other times."""
        return self._shift_digits(_convert_other(other, True), context, True)

    def shift(self, other: _O, context: "Context | None" = None) -> "Decimal":
        """Returns a shifted copy of self, value-of-other times."""
        return self._shift_digits(_convert_other(other, True), context, False)

    def scaleb(self, other: _O, context: "Context | None" = None) -> "Decimal":
        """Returns self operand after adding the second value to its exp."""
        ctx = context if context is not None else getcontext()
        o = _convert_other(other, True)
        ans = self._check_nans(o, ctx)
        if ans is not None:
            return ans
        if o._exp != 0:
            return ctx._raise_error("InvalidOperation")
        liminf = -2 * (ctx.Emax + ctx.prec)
        limsup = 2 * (ctx.Emax + ctx.prec)
        if not (liminf <= int(o) <= limsup):
            return ctx._raise_error("InvalidOperation")
        if self._isinfinity():
            return Decimal(self)
        return _dec_from_triple(self._sign, self._int, self._exp + int(o))._fix(ctx)

    def __reduce__(self) -> tuple[type, tuple[str]]:
        return (Decimal, (str(self),))

    def __copy__(self) -> "Decimal":
        return self

    def __deepcopy__(self, memo: _V) -> "Decimal":
        return self

    def __format__(self, specifier: str, context: "Context | None" = None) -> str:
        """Format a Decimal instance according to the given specifier."""
        ctx = context if context is not None else getcontext()
        spec = _parse_format_specifier(specifier)
        if self._is_special:
            sign = _format_sign(self._sign, spec)
            body = str(self.copy_abs())
            if spec.type == '%':
                body += '%'
            return _format_align(sign, body, spec)
        if spec.type == '':
            spec.type = 'G' if ctx.capitals else 'g'
        me = self
        if spec.type == '%':
            me = _dec_from_triple(self._sign, self._int, self._exp + 2)
        rounding = ctx.rounding
        precision = spec.precision
        if precision >= 0:
            if spec.type in 'eE':
                me = me._round(precision + 1, rounding)
            elif spec.type in 'fF%':
                me = me._rescale(-precision, rounding)
            elif spec.type in 'gG' and len(me._int) > precision:
                me = me._round(precision, rounding)
        if not me and me._exp > 0 and spec.type in 'fF%':
            me = me._rescale(0, rounding)
        if not me and spec.no_neg_0 and me._sign:
            adjusted_sign = 0
        else:
            adjusted_sign = me._sign
        leftdigits = me._exp + len(me._int)
        if spec.type in 'eE':
            if not me and precision >= 0:
                dotplace = 1 - precision
            else:
                dotplace = 1
        elif spec.type in 'fF%':
            dotplace = leftdigits
        else:
            if me._exp <= 0 and leftdigits > -6:
                dotplace = leftdigits
            else:
                dotplace = 1
        if dotplace < 0:
            intpart = '0'
            fracpart = '0' * (-dotplace) + me._int
        elif dotplace > len(me._int):
            intpart = me._int + '0' * (dotplace - len(me._int))
            fracpart = ''
        else:
            intpart = me._int[:dotplace] or '0'
            fracpart = me._int[dotplace:]
        exp = leftdigits - dotplace
        return _format_number(adjusted_sign, intpart, fracpart, exp, spec)


# ---------------------------------------------------------------- helpers of Decimal

def _dec_from_triple(sign: int, coefficient: str, exponent: int, special: str = "") -> Decimal:
    """A Decimal of its parts, unchecked (special: '', 'F', 'n' or 'N')."""
    d = Decimal(0)
    d._sign = sign
    d._int = coefficient
    d._exp = exponent
    d._special = special
    d._is_special = special != ""
    return d


def _signed_infinity(sign: int) -> Decimal:
    return _dec_from_triple(sign, '0', 0, 'F')


def _trunc_digits(d: Decimal) -> str:
    """The digits of the integer part of a finite d."""
    if d._exp >= 0:
        if d._int == '0':
            return '0'
        return d._int + '0' * d._exp
    return d._int[:d._exp] or '0'


def _powmod_big(base: _BigInt, e: _BigInt, m: _BigInt) -> _BigInt:
    """base ** e % m (e >= 0, m > 0)."""
    result = _bint(1) % m
    b = base % m
    mag = list(e.mag)
    while mag:
        mag, chunk = _divmod_small(mag, 1 << 30)
        for k in range(30):
            if chunk & 1:
                result = result * b % m
            chunk >>= 1
            if not mag and not chunk:
                break
            b = b * b % m
    return result


def _from_tuple(value: _V) -> Decimal:
    """Decimal((sign, digits, exponent)): exponent an int, or 'F', 'n', 'N'."""
    if len(value) != 3:
        raise ValueError("argument must be a sequence of length 3")
    sign = value[0]
    if not isinstance(sign, int) or (sign != 0 and sign != 1):
        raise ValueError("sign must be an integer with the value 0 or 1")
    coeff = value[1]
    if not sys._compiled:
        if not isinstance(coeff, (tuple, list)):
            raise ValueError("coefficient must be a tuple of digits")
    digits: list[str] = []
    for d in coeff:
        if not isinstance(d, int) or isinstance(d, bool) or d < 0 or d > 9:
            raise ValueError("coefficient must be a tuple of digits")
        digits.append(str(d))
    exp = value[2]
    if isinstance(exp, bool):
        raise ValueError("exponent must be an integer")
    elif isinstance(exp, int):
        return _dec_from_triple(sign, "".join(digits).lstrip('0') or '0', exp)
    elif isinstance(exp, str):
        if exp == 'F':
            return _signed_infinity(sign)
        if exp == 'n' or exp == 'N':
            return _dec_from_triple(sign, "".join(digits).lstrip('0'), 0, exp)
        raise ValueError("string argument in the third position must be 'F', 'n' or 'N'")
    raise ValueError("exponent must be an integer")


def _operand(other: _O, op: str, typename: str, left: bool) -> Decimal:
    """The Decimal of an operand of an arithmetic operator (an int or a Decimal; TypeError otherwise)."""
    if isinstance(other, Decimal):
        return other
    elif isinstance(other, int):
        return Decimal(other)
    else:
        name = type(other).__name__
        if left:
            raise TypeError("unsupported operand type(s) for " + op + ": '" + typename + "' and '" + name + "'")
        raise TypeError("unsupported operand type(s) for " + op + ": '" + name + "' and '" + typename + "'")


def _convert_other(other: _O, raiseit: bool = True) -> Decimal:
    """The Decimal of an int or a Decimal operand (TypeError otherwise)."""
    if isinstance(other, Decimal):
        return other
    elif isinstance(other, int):
        return Decimal(other)
    else:
        raise TypeError("conversion from " + type(other).__name__ + " to Decimal is not supported")


def _convert_for_comparison(other: _O, equality: bool) -> Decimal | None:
    """The Decimal to compare with (None: not comparable); floats signal FloatOperation."""
    if isinstance(other, Decimal):
        return other
    elif isinstance(other, int):
        return Decimal(other)
    elif isinstance(other, float):
        ctx = getcontext()
        if equality:
            ctx.flags.put("FloatOperation", True)
        else:
            ctx._raise_error("FloatOperation", "strict semantics for mixing floats and Decimals are enabled")
        return Decimal.from_float(other)
    else:
        return None


if not sys._compiled:
    import collections as _collections
    DecimalTuple = _collections.namedtuple('DecimalTuple', 'sign digits exponent', module='decimal')
    del _collections
else:
    class DecimalTuple:
        """DecimalTuple(sign, digits, exponent) (of a special value: exponent 0, its kind in special)."""

        def __init__(self, sign: int, digits: tuple[int, ...], exponent: int, special: str = "") -> None:
            self.sign = sign
            self.digits = digits
            self.exponent = exponent
            self.special = special

        def __repr__(self) -> str:
            e = "'" + self.special + "'" if self.special else str(self.exponent)
            return "DecimalTuple(sign=" + str(self.sign) + ", digits=" + repr(self.digits) + ", exponent=" + e + ")"

        def __eq__(self, other: "DecimalTuple") -> bool:
            return (self.sign == other.sign and self.digits == other.digits and self.exponent == other.exponent
                    and self.special == other.special)


def _decimal_tuple(d: Decimal) -> DecimalTuple:
    digits = tuple([ord(ch) - 48 for ch in d._int])
    if not sys._compiled:
        return DecimalTuple(d._sign, digits, d._special if d._is_special else d._exp)
    return DecimalTuple(d._sign, digits, d._exp, d._special)


def _from_dtuple(t: DecimalTuple) -> Decimal:
    """Decimal(t) of a DecimalTuple (compiled: a class of its own)."""
    coeff = "".join([str(x) for x in t.digits])
    if t.special == 'F':
        return _signed_infinity(t.sign)
    if t.special:
        return _dec_from_triple(t.sign, coeff.lstrip('0'), 0, t.special)
    return _dec_from_triple(t.sign, coeff.lstrip('0') or '0', t.exponent)


def _ascii_digits(s: str) -> str:
    """s with the Unicode decimal digits in it as ASCII ones."""
    out: list[str] = []
    for ch in s:
        c = ord(ch)
        if c > 127 and ch.isdecimal():
            k = c
            while k > 0 and chr(k - 1).isdecimal():
                k -= 1
            out.append(chr(48 + (c - k) % 10))
        else:
            out.append(ch)
    return "".join(out)


# ---------------------------------------------------------------- Context

_CONTEXT_ATTRS = ("prec", "rounding", "Emin", "Emax", "capitals", "clamp", "flags", "traps", "_prec", "_rounding",
                  "_Emin", "_Emax", "_capitals", "_clamp", "_ignore_all")


class Context:
    """Contains the context for a Decimal instance: prec, rounding, Emin, Emax, capitals, clamp, flags, traps."""

    def __init__(self, prec: int | None = None, rounding: str | None = None, Emin: int | None = None,
                 Emax: int | None = None, capitals: int | None = None, clamp: int | None = None,
                 flags: _T = None, traps: _S = None) -> None:
        self._prec = 28
        self._rounding = ROUND_HALF_EVEN
        self._Emin = -999999
        self._Emax = 999999
        self._capitals = 1
        self._clamp = 0
        self._ignore_all = False
        default_traps = ["InvalidOperation", "DivisionByZero", "Overflow"]
        if _DEFAULTS:
            dc = _DEFAULTS[0]
            self._prec = dc.prec
            self._rounding = dc.rounding
            self._Emin = dc.Emin
            self._Emax = dc.Emax
            self._capitals = dc.capitals
            self._clamp = dc.clamp
            default_traps = dc.traps.names()
        if prec is not None:
            self.prec = prec
        if rounding is not None:
            self.rounding = rounding
        if Emin is not None:
            self.Emin = Emin
        if Emax is not None:
            self.Emax = Emax
        if capitals is not None:
            self.capitals = capitals
        if clamp is not None:
            self.clamp = clamp
        self.flags = _signals_of(flags, [])
        self.traps = _signals_of(traps, default_traps)

    # ---- the attributes (checked)

    @property
    def prec(self) -> int:
        return self._prec

    @prec.setter
    def prec(self, value: int) -> None:
        if not sys._compiled:
            if not isinstance(value, int):
                raise TypeError("an integer is required")
        if value < 1 or value > MAX_PREC:
            raise ValueError("valid range for prec is [1, MAX_PREC]")
        self._prec = value

    @property
    def rounding(self) -> str:
        return self._rounding

    @rounding.setter
    def rounding(self, value: str) -> None:
        if value not in _ROUNDING_MODES:
            raise TypeError("valid values for rounding are:\n  [ROUND_CEILING, ROUND_FLOOR, ROUND_UP, ROUND_DOWN,\n"
                            "   ROUND_HALF_UP, ROUND_HALF_DOWN, ROUND_HALF_EVEN,\n   ROUND_05UP]")
        self._rounding = value

    @property
    def Emin(self) -> int:
        return self._Emin

    @Emin.setter
    def Emin(self, value: int) -> None:
        if not sys._compiled:
            if not isinstance(value, int):
                raise TypeError("an integer is required")
        if value < MIN_EMIN or value > 0:
            raise ValueError("valid range for Emin is [MIN_EMIN, 0]")
        self._Emin = value

    @property
    def Emax(self) -> int:
        return self._Emax

    @Emax.setter
    def Emax(self, value: int) -> None:
        if not sys._compiled:
            if not isinstance(value, int):
                raise TypeError("an integer is required")
        if value < 0 or value > MAX_EMAX:
            raise ValueError("valid range for Emax is [0, MAX_EMAX]")
        self._Emax = value

    @property
    def capitals(self) -> int:
        return self._capitals

    @capitals.setter
    def capitals(self, value: int) -> None:
        if not sys._compiled:
            if not isinstance(value, int):
                raise TypeError("an integer is required")
        if value != 0 and value != 1:
            raise ValueError("valid values for capitals are 0 or 1")
        self._capitals = value

    @property
    def clamp(self) -> int:
        return self._clamp

    @clamp.setter
    def clamp(self, value: int) -> None:
        if not sys._compiled:
            if not isinstance(value, int):
                raise TypeError("an integer is required")
        if value != 0 and value != 1:
            raise ValueError("valid values for clamp are 0 or 1")
        self._clamp = value

    if not sys._compiled:
        def __setattr__(self, name, value):
            if name == "flags" or name == "traps":
                if isinstance(value, dict):
                    value = _signals_of(value, [])
                elif not isinstance(value, _Signals):
                    raise TypeError("argument must be a signal dict")
            elif name not in _CONTEXT_ATTRS:
                raise AttributeError("'decimal.Context' object has no attribute '%s'" % name)
            object.__setattr__(self, name, value)

        def __delattr__(self, name):
            raise AttributeError("context attributes cannot be deleted")

        def __reduce__(self):
            flags = [globals()[n] for n in _SIGNAL_NAMES if self.flags.get(n)]
            traps = [globals()[n] for n in _SIGNAL_NAMES if self.traps.get(n)]
            return (self.__class__, (self.prec, self.rounding, self.Emin, self.Emax, self.capitals, self.clamp,
                                     flags, traps))

    def __repr__(self) -> str:
        """Show the current context."""
        flags = self.flags.repr_names()
        traps = self.traps.repr_names()
        return ("Context(prec=" + str(self._prec) + ", rounding=" + self._rounding + ", Emin=" + str(self._Emin)
                + ", Emax=" + str(self._Emax) + ", capitals=" + str(self._capitals) + ", clamp=" + str(self._clamp)
                + ", flags=[" + ", ".join(flags) + "], traps=[" + ", ".join(traps) + "])")

    def clear_flags(self) -> None:
        """Reset all flags to zero"""
        for n in _SIGNAL_NAMES:
            self.flags.put(n, False)

    def clear_traps(self) -> None:
        """Reset all traps to zero"""
        for n in _SIGNAL_NAMES:
            self.traps.put(n, False)

    def _shallow_copy(self) -> "Context":
        """A copy sharing the flags and traps."""
        nc = Context(self._prec, self._rounding, self._Emin, self._Emax, self._capitals, self._clamp,
                     self.flags, self.traps)
        nc._ignore_all = self._ignore_all
        return nc

    def copy(self) -> "Context":
        """Returns a deep copy from self."""
        return Context(self._prec, self._rounding, self._Emin, self._Emax, self._capitals, self._clamp,
                       self.flags.copy(), self.traps.copy())

    def __copy__(self) -> "Context":
        return self.copy()

    def _raise_error(self, condition: str, explanation: str = "", sign: int = 0,
                     nanarg: "Decimal | None" = None) -> Decimal:
        """Handles an error: sets the flag of its signal and raises it if trapped, else returns its result."""
        name = _CONDITION_MAP[condition] if condition in _CONDITION_MAP else condition
        if not self._ignore_all:
            self.flags.happened(condition)
            if self.traps.get(name):
                _raise_signal(name, "[<class 'decimal." + condition + "'>]")
        if condition == "InvalidOperation":
            if nanarg is not None:
                return _dec_from_triple(nanarg._sign, nanarg._int, 0, 'n')._fix_nan(self)
            return _dec_from_triple(0, '', 0, 'n')
        if condition == "DivisionByZero":
            return _signed_infinity(sign)
        if condition == "Overflow":
            return self._overflow_value(sign)
        return _dec_from_triple(0, '', 0, 'n')

    def _overflow_value(self, sign: int) -> Decimal:
        """The result of an overflow: an infinity or the largest finite number, as the rounding has it."""
        r = self._rounding
        if r == ROUND_HALF_UP or r == ROUND_HALF_EVEN or r == ROUND_HALF_DOWN or r == ROUND_UP:
            return _signed_infinity(sign)
        if sign == 0:
            if r == ROUND_CEILING:
                return _signed_infinity(sign)
            return _dec_from_triple(sign, '9' * self._prec, self._Emax - self._prec + 1)
        if r == ROUND_FLOOR:
            return _signed_infinity(sign)
        return _dec_from_triple(sign, '9' * self._prec, self._Emax - self._prec + 1)

    def _signal(self, conds: list[str]) -> None:
        """The conditions of one operation happened: set all their flags, then raise the first trapped signal
        (with the list of the trapped conditions), as the C module does."""
        if self._ignore_all or not conds:
            return
        trapped: list[str] = []
        for cond in conds:
            name = _CONDITION_MAP[cond] if cond in _CONDITION_MAP else cond
            self.flags.happened(cond)
            if self.traps.get(name) and cond not in trapped:
                trapped.append(cond)
        if trapped:
            first = ""
            for n in _SIGNAL_NAMES:
                for cond in trapped:
                    if first == "" and (_CONDITION_MAP[cond] if cond in _CONDITION_MAP else cond) == n:
                        first = n
            _raise_signal(first, "[" + ", ".join(["<class 'decimal." + c + "'>" for c in _TRAP_LIST_ORDER
                                                  if c in trapped]) + "]")

    def Etiny(self) -> int:
        """Returns Etiny (= Emin - prec + 1)"""
        return self._Emin - self._prec + 1

    def Etop(self) -> int:
        """Returns maximum exponent (= Emax - prec + 1)"""
        return self._Emax - self._prec + 1

    def create_decimal(self, num: _O = "0") -> Decimal:
        """Creates a new Decimal instance but using self as context (no whitespace / underscores in a str)."""
        if isinstance(num, str):
            if num != num.strip() or '_' in num:
                return self._raise_error("ConversionSyntax",
                                         "trailing or leading whitespace and underscores are not permitted.")
        d = Decimal(num, self)
        if d._isnan() and len(d._int) > self._prec - self._clamp:
            return self._raise_error("ConversionSyntax", "diagnostic info too long in NaN")
        return d._fix(self)

    def create_decimal_from_float(self, f: _O) -> Decimal:
        """Creates a new Decimal instance from a float but rounding using self as the context."""
        return Decimal.from_float(f)._fix(self)

    def apply(self, a: _O) -> Decimal:
        """Applies the context to a (rounds it)."""
        return _convert_other(a, True)._fix(self)

    def abs(self, a: _O) -> Decimal:
        """Returns the absolute value of the operand."""
        return _convert_other(a, True).__abs__(True, self)

    def add(self, a: _O, b: _V) -> Decimal:
        """Return the sum of the two operands."""
        return _convert_other(a, True)._add(_convert_other(b, True), self)

    def canonical(self, a: Decimal) -> Decimal:
        """Returns the same Decimal object."""
        if not sys._compiled:
            if not isinstance(a, Decimal):
                raise TypeError("argument must be a Decimal")
        return a.canonical()

    def compare(self, a: _O, b: _V) -> Decimal:
        """Compares values numerically."""
        return _convert_other(a, True).compare(b, self)

    def compare_signal(self, a: _O, b: _V) -> Decimal:
        """Compares the values of the two operands numerically (NaNs signal)."""
        return _convert_other(a, True).compare_signal(b, self)

    def compare_total(self, a: _O, b: _V) -> Decimal:
        """Compares two operands using their abstract representation."""
        return _convert_other(a, True).compare_total(b)

    def compare_total_mag(self, a: _O, b: _V) -> Decimal:
        """Compares two operands using their abstract representation ignoring sign."""
        return _convert_other(a, True).compare_total_mag(b)

    def copy_abs(self, a: _O) -> Decimal:
        """Returns a copy of the operand with the sign set to 0."""
        return _convert_other(a, True).copy_abs()

    def copy_decimal(self, a: _O) -> Decimal:
        """Returns a copy of the decimal object."""
        return Decimal(_convert_other(a, True))

    def copy_negate(self, a: _O) -> Decimal:
        """Returns a copy of the operand with the sign inverted."""
        return _convert_other(a, True).copy_negate()

    def copy_sign(self, a: _O, b: _V) -> Decimal:
        """Copies the second operand's sign to the first one."""
        return _convert_other(a, True).copy_sign(b)

    def divide(self, a: _O, b: _V) -> Decimal:
        """Decimal division in a specified context."""
        return _convert_other(a, True)._truediv(_convert_other(b, True), self)

    def divide_int(self, a: _O, b: _V) -> Decimal:
        """Divides two numbers and returns the integer part of the result."""
        return _convert_other(a, True)._floordiv(_convert_other(b, True), self)

    def divmod(self, a: _O, b: _V) -> tuple[Decimal, Decimal]:
        """Return (a // b, a % b)."""
        return _convert_other(a, True)._divmod(_convert_other(b, True), self)

    def exp(self, a: _O) -> Decimal:
        """Returns e ** a."""
        return _convert_other(a, True).exp(self)

    def fma(self, a: _O, b: _V, c: _T) -> Decimal:
        """Returns a multiplied by b, plus c."""
        return _convert_other(a, True).fma(b, c, self)

    def is_canonical(self, a: Decimal) -> bool:
        """Return True if the operand is canonical; otherwise return False."""
        if not sys._compiled:
            if not isinstance(a, Decimal):
                raise TypeError("argument must be a Decimal")
        return a.is_canonical()

    def is_finite(self, a: _O) -> bool:
        """Return True if the operand is finite; otherwise return False."""
        return _convert_other(a, True).is_finite()

    def is_infinite(self, a: _O) -> bool:
        """Return True if the operand is infinite; otherwise return False."""
        return _convert_other(a, True).is_infinite()

    def is_nan(self, a: _O) -> bool:
        """Return True if the operand is a qNaN or sNaN; otherwise return False."""
        return _convert_other(a, True).is_nan()

    def is_normal(self, a: _O) -> bool:
        """Return True if the operand is a normal number; otherwise return False."""
        return _convert_other(a, True).is_normal(self)

    def is_qnan(self, a: _O) -> bool:
        """Return True if the operand is a quiet NaN; otherwise return False."""
        return _convert_other(a, True).is_qnan()

    def is_signed(self, a: _O) -> bool:
        """Return True if the operand is negative; otherwise return False."""
        return _convert_other(a, True).is_signed()

    def is_snan(self, a: _O) -> bool:
        """Return True if the operand is a signaling NaN; otherwise return False."""
        return _convert_other(a, True).is_snan()

    def is_subnormal(self, a: _O) -> bool:
        """Return True if the operand is subnormal; otherwise return False."""
        return _convert_other(a, True).is_subnormal(self)

    def is_zero(self, a: _O) -> bool:
        """Return True if the operand is a zero; otherwise return False."""
        return _convert_other(a, True).is_zero()

    def ln(self, a: _O) -> Decimal:
        """Returns the natural (base e) logarithm of the operand."""
        return _convert_other(a, True).ln(self)

    def log10(self, a: _O) -> Decimal:
        """Returns the base 10 logarithm of the operand."""
        return _convert_other(a, True).log10(self)

    def logb(self, a: _O) -> Decimal:
        """Returns the exponent of the magnitude of the operand's MSD."""
        return _convert_other(a, True).logb(self)

    def logical_and(self, a: _O, b: _V) -> Decimal:
        """Applies the logical operation 'and' between each operand's digits."""
        return _convert_other(a, True).logical_and(b, self)

    def logical_invert(self, a: _O) -> Decimal:
        """Invert all the digits in the operand."""
        return _convert_other(a, True).logical_invert(self)

    def logical_or(self, a: _O, b: _V) -> Decimal:
        """Applies the logical operation 'or' between each operand's digits."""
        return _convert_other(a, True).logical_or(b, self)

    def logical_xor(self, a: _O, b: _V) -> Decimal:
        """Applies the logical operation 'xor' between each operand's digits."""
        return _convert_other(a, True).logical_xor(b, self)

    def max(self, a: _O, b: _V) -> Decimal:
        """max compares two values numerically and returns the maximum."""
        return _convert_other(a, True).max(b, self)

    def max_mag(self, a: _O, b: _V) -> Decimal:
        """Compares the values numerically with their sign ignored."""
        return _convert_other(a, True).max_mag(b, self)

    def min(self, a: _O, b: _V) -> Decimal:
        """min compares two values numerically and returns the minimum."""
        return _convert_other(a, True).min(b, self)

    def min_mag(self, a: _O, b: _V) -> Decimal:
        """Compares the values numerically with their sign ignored."""
        return _convert_other(a, True).min_mag(b, self)

    def minus(self, a: _O) -> Decimal:
        """Minus corresponds to unary prefix minus in Python."""
        return _convert_other(a, True).__neg__(self)

    def multiply(self, a: _O, b: _V) -> Decimal:
        """multiply multiplies two operands."""
        return _convert_other(a, True)._mul(_convert_other(b, True), self)

    def next_minus(self, a: _O) -> Decimal:
        """Returns the largest representable number smaller than a."""
        return _convert_other(a, True).next_minus(self)

    def next_plus(self, a: _O) -> Decimal:
        """Returns the smallest representable number larger than a."""
        return _convert_other(a, True).next_plus(self)

    def next_toward(self, a: _O, b: _V) -> Decimal:
        """Returns the number closest to a, in direction towards b."""
        return _convert_other(a, True).next_toward(b, self)

    def normalize(self, a: _O) -> Decimal:
        """normalize reduces an operand to its simplest form."""
        return _convert_other(a, True).normalize(self)

    def number_class(self, a: _O) -> str:
        """Returns an indication of the class of the operand."""
        return _convert_other(a, True).number_class(self)

    def plus(self, a: _O) -> Decimal:
        """Plus corresponds to unary prefix plus in Python."""
        return _convert_other(a, True).__pos__(self)

    def power(self, a: _O, b: _V, modulo: _T = None) -> Decimal:
        """Raises a to the power of b, to modulo if given."""
        x = _convert_other(a, True)
        if modulo is None:
            return x._pow(_convert_other(b, True), self)
        return x._power_modulo(_convert_other(b, True), _convert_other(modulo, True), self)

    def quantize(self, a: _O, b: _V) -> Decimal:
        """Returns a value equal to 'a' (rounded), having the exponent of 'b'."""
        return _convert_other(a, True).quantize(b, None, self)

    def radix(self) -> Decimal:
        """Just returns 10, as this is Decimal, :)"""
        return Decimal(10)

    def remainder(self, a: _O, b: _V) -> Decimal:
        """Returns the remainder from integer division."""
        return _convert_other(a, True)._mod(_convert_other(b, True), self)

    def remainder_near(self, a: _O, b: _V) -> Decimal:
        """Returns to be "a - b * n", where n is the integer nearest the exact value of "x / b"."""
        return _convert_other(a, True).remainder_near(b, self)

    def rotate(self, a: _O, b: _V) -> Decimal:
        """Returns a rotated copy of a, b times."""
        return _convert_other(a, True).rotate(b, self)

    def same_quantum(self, a: _O, b: _V) -> bool:
        """Returns True if the two operands have the same exponent."""
        return _convert_other(a, True).same_quantum(b)

    def scaleb(self, a: _O, b: _V) -> Decimal:
        """Returns the first operand after adding the second value its exp."""
        return _convert_other(a, True).scaleb(b, self)

    def shift(self, a: _O, b: _V) -> Decimal:
        """Returns a shifted copy of a, b times."""
        return _convert_other(a, True).shift(b, self)

    def sqrt(self, a: _O) -> Decimal:
        """Square root of a non-negative number to context precision."""
        return _convert_other(a, True).sqrt(self)

    def subtract(self, a: _O, b: _V) -> Decimal:
        """Return the difference between the two operands."""
        return _convert_other(a, True).__sub__(_convert_other(b, True), self)

    def to_eng_string(self, a: _O) -> str:
        """Convert to a string, using engineering notation if an exponent is needed."""
        return _convert_other(a, True).to_eng_string(self)

    def to_sci_string(self, a: _O) -> str:
        """Converts a number to a string, using scientific notation."""
        return _convert_other(a, True)._to_str(False, self)

    def to_integral_exact(self, a: _O) -> Decimal:
        """Rounds to an integer."""
        return _convert_other(a, True).to_integral_exact(None, self)

    def to_integral_value(self, a: _O) -> Decimal:
        """Rounds to an integer."""
        return _convert_other(a, True).to_integral_value(None, self)

    def to_integral(self, a: _O) -> Decimal:
        """Rounds to an integer."""
        return _convert_other(a, True).to_integral_value(None, self)


# the context the new ones take their attributes from (DefaultContext), when there is one
_DEFAULTS: list[Context] = []
# the context of the program (one: not one per thread)
_CURRENT: list[Context] = []


def getcontext() -> Context:
    """Returns this program's context (a copy of DefaultContext at first)."""
    if not _CURRENT:
        _CURRENT.append(Context())
    return _CURRENT[0]


def setcontext(context: Context) -> None:
    """Set this program's context to context (a copy of it if it is one of the templates)."""
    if not sys._compiled:
        if not isinstance(context, Context):
            raise TypeError("argument must be a context")
    if context is DefaultContext or context is BasicContext or context is ExtendedContext:
        context = context.copy()
        context.clear_flags()
    if _CURRENT:
        _CURRENT[0] = context
    else:
        _CURRENT.append(context)


class _ContextManager:
    """Context manager class to support localcontext(): sets the context for a with block."""

    def __init__(self, new_context: Context) -> None:
        self.new_context = new_context
        self.saved_context = new_context

    def __enter__(self) -> Context:
        self.saved_context = getcontext()
        setcontext(self.new_context)
        return self.new_context

    def __exit__(self, t: type[BaseException] | None, v: BaseException | None, tb) -> bool:
        setcontext(self.saved_context)
        return False


if not sys._compiled:
    _ContextManager.__name__ = _ContextManager.__qualname__ = "ContextManager"
    _ContextManager.__module__ = "decimal"


def localcontext(ctx: Context | None = None, prec: int | None = None, rounding: str | None = None,
                 Emin: int | None = None, Emax: int | None = None, capitals: int | None = None,
                 clamp: int | None = None, flags: _T = None, traps: _S = None) -> _ContextManager:
    """A context manager for a copy of ctx (the current context) with the attributes given set."""
    c = ctx.copy() if ctx is not None else getcontext().copy()
    if prec is not None:
        c.prec = prec
    if rounding is not None:
        c.rounding = rounding
    if Emin is not None:
        c.Emin = Emin
    if Emax is not None:
        c.Emax = Emax
    if capitals is not None:
        c.capitals = capitals
    if clamp is not None:
        c.clamp = clamp
    if flags is not None:
        c.flags = _signals_of(flags, [])
    if traps is not None:
        c.traps = _signals_of(traps, [])
    return _ContextManager(c)


def IEEEContext(bits: int) -> Context:
    """Return a context object initialized to the proper values for one of the IEEE interchange formats."""
    if bits <= 0 or bits > IEEE_CONTEXT_MAX_BITS or bits % 32:
        raise ValueError("argument must be a multiple of 32, with a maximum of " + str(IEEE_CONTEXT_MAX_BITS))
    ctx = Context()
    ctx.prec = 9 * (bits // 32) - 2
    ctx.Emax = 3 * (1 << (bits // 16 + 3))
    ctx.Emin = 1 - ctx.Emax
    ctx.rounding = ROUND_HALF_EVEN
    ctx.clamp = 1
    ctx.traps = _Signals([])
    return ctx


DefaultContext = Context(prec=28, rounding=ROUND_HALF_EVEN, traps=["InvalidOperation", "DivisionByZero", "Overflow"],
                         Emax=999999, Emin=-999999, capitals=1, clamp=0)
_DEFAULTS.append(DefaultContext)
BasicContext = Context(prec=9, rounding=ROUND_HALF_UP,
                       traps=["DivisionByZero", "Overflow", "InvalidOperation", "Clamped", "Underflow"])
ExtendedContext = Context(prec=9, rounding=ROUND_HALF_EVEN, traps=_Signals([]))


# ---------------------------------------------------------------- format()

class _FormatSpec:
    """A parsed format specification of Decimal.__format__."""

    def __init__(self) -> None:
        self.fill = ' '
        self.align = '>'
        self.sign = '-'
        self.no_neg_0 = False
        self.alt = False
        self.zeropad = False
        self.minimumwidth = 0
        self.thousands_sep = ''
        self.precision = -1
        self.frac_separators = ''
        self.type = ''
        self.decimal_point = '.'
        self.grouping: list[int] = [3, 0]


def _parse_format_specifier(spec: str) -> _FormatSpec:
    """[[fill]align][sign][z][#][0][minimumwidth][,_][.precision[,_]][type]"""
    sp = _FormatSpec()
    n = len(spec)
    i = 0
    fill = ''
    align = ''
    if n >= 2 and spec[1] in '<>=^':
        fill = spec[0]
        align = spec[1]
        i = 2
    elif n >= 1 and spec[0] in '<>=^':
        align = spec[0]
        i = 1
    if i < n and spec[i] in '-+ ':
        sp.sign = spec[i]
        i += 1
    if i < n and spec[i] == 'z':
        sp.no_neg_0 = True
        i += 1
    if i < n and spec[i] == '#':
        sp.alt = True
        i += 1
    if i < n and spec[i] == '0':
        sp.zeropad = True
        i += 1
    start = i
    while i < n and '0' <= spec[i] <= '9':
        i += 1
    if i > start:
        sp.minimumwidth = int(spec[start:i])
    if i < n and spec[i] in ',_':
        sp.thousands_sep = spec[i]
        i += 1
    if i < n and spec[i] == '.':
        i += 1
        if i >= n or not ('0' <= spec[i] <= '9' or spec[i] in ',_'):
            raise ValueError("invalid format string")
        start = i
        while i < n and '0' <= spec[i] <= '9':
            i += 1
        if i > start:
            sp.precision = int(spec[start:i])
        if i < n and spec[i] in ',_':
            sp.frac_separators = spec[i]
            i += 1
    if i < n and spec[i] in 'eEfFgGn%':
        sp.type = spec[i]
        i += 1
    if i != n:
        raise ValueError("invalid format string")
    if sp.zeropad and (fill != '' or align != ''):
        raise ValueError("invalid format string")
    if fill != '':
        sp.fill = fill
    if align != '':
        sp.align = align
    if sp.precision == 0 and (sp.type == '' or sp.type in 'gGn'):
        sp.precision = 1
    if sp.type == 'n':
        if sp.thousands_sep != '':
            raise ValueError("invalid format string")
        sp.type = 'g'
        sp.grouping = []                      # (the C locale's)
    return sp


def _format_align(sign: str, body: str, spec: _FormatSpec) -> str:
    """Pad sign + body to spec's minimum width, aligned as spec says."""
    padding = spec.fill * (spec.minimumwidth - len(sign) - len(body))
    align = spec.align
    if align == '<':
        return sign + body + padding
    elif align == '>':
        return padding + sign + body
    elif align == '=':
        return sign + padding + body
    half = len(padding) // 2
    return padding[:half] + sign + body + padding[half:]


def _insert_thousands_sep(digits: str, spec: _FormatSpec, min_width: int = 1) -> str:
    """digits grouped (with spec's separator), padded with zeros to at least min_width."""
    sep = spec.thousands_sep
    groups: list[str] = []
    if spec.grouping:
        while True:
            l = min(max(len(digits), min_width, 1), 3)
            groups.append('0' * (l - len(digits)) + digits[len(digits) - l if l <= len(digits) else 0:])
            digits = digits[:-l] if l < len(digits) else ''
            min_width -= l
            if not digits and min_width <= 0:
                break
            min_width -= len(sep)
    else:
        l = max(len(digits), min_width, 1)
        groups.append('0' * (l - len(digits)) + digits)
    groups.reverse()
    return sep.join(groups)


def _format_sign(is_negative: int, spec: _FormatSpec) -> str:
    if is_negative:
        return '-'
    elif spec.sign in ' +':
        return spec.sign
    return ''


def _format_number(is_negative: int, intpart: str, fracpart: str, exp: int, spec: _FormatSpec) -> str:
    """Format a number, given the following data: sign, the digits before / after the point, the exponent."""
    sign = _format_sign(is_negative, spec)
    frac_sep = spec.frac_separators
    if fracpart and frac_sep:
        fracpart = frac_sep.join([fracpart[pos:pos + 3] for pos in range(0, len(fracpart), 3)])
    if fracpart or spec.alt:
        fracpart = spec.decimal_point + fracpart
    if exp != 0 or spec.type in 'eE':
        echar = 'E' if spec.type in 'EG' else 'e'
        fracpart += echar + ('+' if exp >= 0 else '-') + str(abs(exp))
    if spec.type == '%':
        fracpart += '%'
    if spec.zeropad:
        min_width = spec.minimumwidth - len(fracpart) - len(sign)
    else:
        min_width = 0
    intpart = _insert_thousands_sep(intpart, spec, min_width)
    return _format_align(sign, intpart + fracpart, spec)
