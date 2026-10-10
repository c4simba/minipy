"""Rational numbers (CPython's fractions): Fraction(numerator, denominator), from ints,
floats, strings ("3/4", "-1.5e2") or other Fractions, with exact arithmetic.

Ints are 64-bit in minipy, so numerators and denominators are too (an operation
that needs more raises OverflowError). In compiled programs int ** Fraction is a
float and Fraction ** Fraction a Fraction (one result type each)."""
import math
import sys
from typing import TypeVar

_N = TypeVar("_N")
_M = TypeVar("_M")
_O = TypeVar("_O")

__all__ = ["Fraction"]

_PyHASH_MODULUS = 2305843009213693951       # 2**61 - 1
_PyHASH_INF = 314159


def _mulmod(a: int, b: int, m: int) -> int:
    """a * b % m for 0 <= a, b < m < 2**62, without overflowing 64 bits."""
    r = 0
    a = a % m
    while b:
        if b & 1:
            r = (r + a) % m
        a = (a * 2) % m
        b >>= 1
    return r


def _modinv(a: int, m: int) -> int:
    """a's inverse mod m (-1: none)."""
    t = 0
    newt = 1
    r = m
    newr = a % m
    while newr:
        q = r // newr
        t, newt = newt, t - q * newt
        r, newr = newr, r - q * newr
    if r != 1:
        return -1
    return t + m if t < 0 else t


def _bit_length(n: int) -> int:
    k = 0
    while n:
        n >>= 1
        k += 1
    return k


def _int_ratio_float(n: int, d: int) -> float:
    """n / d correctly rounded (half to even), for any 64-bit n and d > 0."""
    if n == 0:
        return 0.0
    neg = n < 0
    if neg:
        n = -n
    if n <= 9007199254740992 and d <= 9007199254740992:
        r = n / d
        return -r if neg else r
    q = n // d
    rem = n - q * d
    e = 0
    bits = _bit_length(q)
    while bits < 55:                            # more quotient bits, one at a time (rem < d: 2 * rem fits)
        rem *= 2
        q *= 2
        if rem >= d:
            q += 1
            rem -= d
        e -= 1
        if q:
            bits += 1
    sticky = rem != 0
    while _bit_length(q) > 54:
        if q & 1:
            sticky = True
        q >>= 1
        e += 1
    lsb = q & 1
    q >>= 1
    e += 1
    if lsb and (sticky or q & 1):
        q += 1
    r2 = math.ldexp(float(q), e)
    return -r2 if neg else r2


def _hash_int(n: int) -> int:
    """hash(n) for an int (CPython's: n mod 2**61-1, keeping the sign)."""
    h = abs(n) % _PyHASH_MODULUS
    if n < 0:
        h = -h
    return -2 if h == -1 else h


def _is_digit(c: str) -> bool:
    return "0" <= c <= "9"


def _digits_with_underscores(s: str, i: int) -> int:
    """The end of \\d+(_\\d+)* at s[i] (i: none)."""
    n = len(s)
    j = i
    if j >= n or not _is_digit(s[j]):
        return i
    while j < n:
        if _is_digit(s[j]):
            j += 1
        elif s[j] == "_" and j + 1 < n and _is_digit(s[j + 1]) and j > i:
            j += 1
        else:
            break
    return j


def _parse(text: str) -> tuple[int, int]:
    """(numerator, denominator) of "[sign]num[/den]" or "[sign][num][.frac][e exp]"."""
    s = text.strip()
    bad = ValueError("Invalid literal for Fraction: " + repr(text))
    i = 0
    n = len(s)
    sign = 1
    if i < n and s[i] in "+-":
        if s[i] == "-":
            sign = -1
        i += 1
    if not (i < n and (_is_digit(s[i]) or (s[i] == "." and i + 1 < n and _is_digit(s[i + 1])))):
        raise bad
    j = _digits_with_underscores(s, i)
    num_text = s[i:j].replace("_", "")
    i = j
    k = i
    while k < n and s[k] in " \t\n\r\x0b\x0c":
        k += 1
    if k < n and s[k] == "/":
        k += 1
        while k < n and s[k] in " \t\n\r\x0b\x0c":
            k += 1
        j = _digits_with_underscores(s, k)
        if j == k or j != n:
            raise bad
        den = int(s[k:j].replace("_", ""))
        if den == 0:
            raise ZeroDivisionError("Fraction(" + num_text + ", 0)")
        return (sign * int(num_text or "0"), den)
    numerator = int(num_text) if num_text else 0
    denominator = 1
    if i < n and s[i] == ".":
        i += 1
        j = _digits_with_underscores(s, i)
        dec = s[i:j].replace("_", "")
        i = j
        if dec:
            numerator = numerator * 10 ** len(dec) + int(dec)
            denominator = 10 ** len(dec)
    if i < n and s[i] in "eE":
        i += 1
        esign = 1
        if i < n and s[i] in "+-":
            if s[i] == "-":
                esign = -1
            i += 1
        j = _digits_with_underscores(s, i)
        if j == i:
            raise bad
        exp = esign * int(s[i:j].replace("_", ""))
        i = j
        if exp >= 0:
            numerator *= 10 ** exp
        else:
            denominator *= 10 ** -exp
    if i != n:
        raise bad
    return (sign * numerator, denominator)


def _format_float(x: float, spec: str) -> str:
    """format(x, spec) for specs [width][.precision][type] (compiled code formats with literal specs)."""
    i = 0
    n = len(spec)
    w = 0
    while i < n and "0" <= spec[i] <= "9":
        w = w * 10 + ord(spec[i]) - 48
        i += 1
    p = 6
    if i < n and spec[i] == ".":
        i += 1
        p = 0
        while i < n and "0" <= spec[i] <= "9":
            p = p * 10 + ord(spec[i]) - 48
            i += 1
    t = spec[i:] if i < n else ""
    if t == "f" or t == "F":
        return f"{x:{w}.{p}f}"
    if t == "e":
        return f"{x:{w}.{p}e}"
    if t == "E":
        return f"{x:{w}.{p}E}"
    if t == "g" or t == "":
        return f"{x:{w}.{p}g}"
    if t == "%":
        return f"{x:{w}.{p}%}"
    raise ValueError("Invalid format specifier '" + spec + "' for object of type 'Fraction'")


def _add_terms(na: int, da: int, nb: int, db: int) -> tuple[int, int]:
    """na/da + nb/db in lowest terms (CPython's way: small intermediate products)."""
    g = math.gcd(da, db)
    if g == 1:
        return (na * db + da * nb, da * db)
    s = da // g
    t = na * (db // g) + nb * s
    g2 = math.gcd(t, g)
    if g2 == 1:
        return (t, s * db)
    return (t // g2, s * (db // g2))


def _mul_terms(na: int, da: int, nb: int, db: int) -> tuple[int, int]:
    """na/da * nb/db in lowest terms."""
    g1 = math.gcd(na, db)
    if g1 > 1:
        na //= g1
        db //= g1
    g2 = math.gcd(nb, da)
    if g2 > 1:
        nb //= g2
        da //= g2
    return (na * nb, db * da)


class Fraction:
    """numerator / denominator, kept in lowest terms with a positive denominator."""

    def __init__(self, numerator: _N = 0, denominator: _M = 1):
        num = 0
        den = 1
        if isinstance(numerator, bool):
            num = int(numerator)
        elif isinstance(numerator, int):
            num = numerator
        elif isinstance(numerator, Fraction):
            num = numerator._numerator
            den = numerator._denominator
        elif isinstance(numerator, float):
            num, den = Fraction._float_ratio(numerator)
        elif isinstance(numerator, str):
            num, den = _parse(numerator)
        else:
            if not sys._compiled:
                if hasattr(numerator, "as_integer_ratio"):
                    num, den = numerator.as_integer_ratio()
                else:
                    raise TypeError("argument should be a string or a Rational instance")
        if isinstance(denominator, bool):
            den = den * int(denominator)
        elif isinstance(denominator, int):
            den = den * denominator
        elif isinstance(denominator, Fraction):
            num = num * denominator._denominator
            den = den * denominator._numerator
        if den == 0:
            raise ZeroDivisionError("Fraction(" + str(num) + ", 0)")
        g = math.gcd(num, den)
        if den < 0:
            g = -g
        self._numerator = num // g
        self._denominator = den // g

    @staticmethod
    def _float_ratio(f: float) -> tuple[int, int]:
        if f != f:
            raise ValueError("cannot convert NaN to integer ratio")
        if f == float("inf") or f == float("-inf"):
            raise OverflowError("cannot convert Infinity to integer ratio")
        m, e = math.frexp(f)
        num = int(m * 9007199254740992.0)
        e -= 53
        den = 1
        while num and num % 2 == 0 and e < 0:
            num //= 2
            e += 1
        if e > 0:
            num = num * 2 ** e
        else:
            den = 2 ** -e
        return (num, den)

    @staticmethod
    def from_float(f: float) -> "Fraction":
        """The exact value of f."""
        n, d = Fraction._float_ratio(f)
        return Fraction(n, d)

    @staticmethod
    def from_number(number: _N) -> "Fraction":
        return Fraction(number)

    @staticmethod
    def _make(n: int, d: int) -> "Fraction":
        return Fraction(n, d)

    @property
    def numerator(self) -> int:
        return self._numerator

    @property
    def denominator(self) -> int:
        return self._denominator

    def as_integer_ratio(self) -> tuple[int, int]:
        return (self._numerator, self._denominator)

    def is_integer(self) -> bool:
        return self._denominator == 1

    def limit_denominator(self, max_denominator: int = 1000000) -> "Fraction":
        """The closest Fraction with a denominator at most max_denominator."""
        if max_denominator < 1:
            raise ValueError("max_denominator should be at least 1")
        if self._denominator <= max_denominator:
            return Fraction(self._numerator, self._denominator)
        p0 = 0
        q0 = 1
        p1 = 1
        q1 = 0
        n = self._numerator
        d = self._denominator
        while True:
            a = n // d
            q2 = q0 + a * q1
            if q2 > max_denominator:
                break
            p0, q0, p1, q1 = p1, q1, p0 + a * p1, q2
            n, d = d, n - a * d
        k = (max_denominator - q0) // q1
        bound1 = Fraction(p0 + k * p1, q0 + k * q1)
        bound2 = Fraction(p1, q1)
        if 2 * d * (q0 + k * q1) <= self._denominator:
            return bound2
        return bound1

    def __repr__(self) -> str:
        return "Fraction(" + str(self._numerator) + ", " + str(self._denominator) + ")"

    def __str__(self) -> str:
        if self._denominator == 1:
            return str(self._numerator)
        return str(self._numerator) + "/" + str(self._denominator)

    def __format__(self, spec: str) -> str:
        if not spec:
            return str(self)
        if not sys._compiled:
            return format(float(self), spec)
        return _format_float(float(self), spec)

    # -- arithmetic (a Fraction or an int: exact; a float: float)

    def __add__(self, other: _O):
        if isinstance(other, Fraction):
            n, d = _add_terms(self._numerator, self._denominator, other._numerator, other._denominator)
            return Fraction(n, d)
        elif isinstance(other, int):
            return Fraction(self._numerator + other * self._denominator, self._denominator)
        elif isinstance(other, float):
            return float(self) + other
        else:
            raise TypeError("unsupported operand type(s) for +: 'Fraction' and '" + type(other).__name__ + "'")

    def __radd__(self, other: _O):
        return self + other

    def __sub__(self, other: _O):
        if isinstance(other, Fraction):
            n, d = _add_terms(self._numerator, self._denominator, -other._numerator, other._denominator)
            return Fraction(n, d)
        elif isinstance(other, int):
            return Fraction(self._numerator - other * self._denominator, self._denominator)
        elif isinstance(other, float):
            return float(self) - other
        else:
            raise TypeError("unsupported operand type(s) for -: 'Fraction' and '" + type(other).__name__ + "'")

    def __rsub__(self, other: _O):
        if isinstance(other, int):
            return Fraction(other * self._denominator - self._numerator, self._denominator)
        elif isinstance(other, float):
            return other - float(self)
        else:
            raise TypeError("unsupported operand type(s) for -: '" + type(other).__name__ + "' and 'Fraction'")

    def __mul__(self, other: _O):
        if isinstance(other, Fraction):
            n, d = _mul_terms(self._numerator, self._denominator, other._numerator, other._denominator)
            return Fraction(n, d)
        elif isinstance(other, int):
            n, d = _mul_terms(self._numerator, self._denominator, other, 1)
            return Fraction(n, d)
        elif isinstance(other, float):
            return float(self) * other
        else:
            raise TypeError("unsupported operand type(s) for *: 'Fraction' and '" + type(other).__name__ + "'")

    def __rmul__(self, other: _O):
        return self * other

    def __truediv__(self, other: _O):
        if isinstance(other, Fraction):
            if other._numerator == 0:
                raise ZeroDivisionError("Fraction(" + str(self._numerator * other._denominator) + ", 0)")
            n, d = _mul_terms(self._numerator, self._denominator, other._denominator, other._numerator)
            return Fraction(n, d)
        elif isinstance(other, int):
            if other == 0:
                raise ZeroDivisionError("Fraction(" + str(self._numerator) + ", 0)")
            n, d = _mul_terms(self._numerator, self._denominator, 1, other)
            return Fraction(n, d)
        elif isinstance(other, float):
            return float(self) / other
        else:
            raise TypeError("unsupported operand type(s) for /: 'Fraction' and '" + type(other).__name__ + "'")

    def __rtruediv__(self, other: _O):
        if isinstance(other, int):
            if self._numerator == 0:
                raise ZeroDivisionError("Fraction(" + str(other * self._denominator) + ", 0)")
            return Fraction(other * self._denominator, self._numerator)
        elif isinstance(other, float):
            return other / float(self)
        else:
            raise TypeError("unsupported operand type(s) for /: '" + type(other).__name__ + "' and 'Fraction'")

    def __floordiv__(self, other: _O):
        if isinstance(other, Fraction):
            return (self._numerator * other._denominator) // (self._denominator * other._numerator)
        elif isinstance(other, int):
            return self._numerator // (self._denominator * other)
        elif isinstance(other, float):
            return float(self) // other
        else:
            raise TypeError("unsupported operand type(s) for //: 'Fraction' and '" + type(other).__name__ + "'")

    def __rfloordiv__(self, other: _O):
        if isinstance(other, int):
            return (other * self._denominator) // self._numerator
        elif isinstance(other, float):
            return other // float(self)
        else:
            raise TypeError("unsupported operand type(s) for //: '" + type(other).__name__ + "' and 'Fraction'")

    def __mod__(self, other: _O):
        if isinstance(other, Fraction):
            da = self._denominator
            db = other._denominator
            return Fraction((self._numerator * db) % (other._numerator * da), da * db)
        elif isinstance(other, int):
            return Fraction(self._numerator % (other * self._denominator), self._denominator)
        elif isinstance(other, float):
            return float(self) % other
        else:
            raise TypeError("unsupported operand type(s) for %: 'Fraction' and '" + type(other).__name__ + "'")

    def __rmod__(self, other: _O):
        if isinstance(other, int):
            return Fraction((other * self._denominator) % self._numerator, self._denominator)
        elif isinstance(other, float):
            return other % float(self)
        else:
            raise TypeError("unsupported operand type(s) for %: '" + type(other).__name__ + "' and 'Fraction'")

    def __divmod__(self, other: _O):
        if isinstance(other, Fraction):
            da = self._denominator
            db = other._denominator
            n = self._numerator * db
            d = other._numerator * da
            return (n // d, Fraction(n % d, da * db))
        elif isinstance(other, int):
            return (self._numerator // (other * self._denominator),
                    Fraction(self._numerator % (other * self._denominator), self._denominator))
        else:
            raise TypeError("unsupported operand type(s) for divmod(): 'Fraction' and '" + type(other).__name__ + "'")

    def __pow__(self, other: _O):
        if isinstance(other, bool):
            return self ** int(other)
        elif isinstance(other, int):
            if other >= 0:
                return Fraction(self._numerator ** other, self._denominator ** other)
            if self._numerator == 0:
                raise ZeroDivisionError("Fraction(" + str(self._denominator ** -other) + ", 0)")
            if self._numerator > 0:
                return Fraction(self._denominator ** -other, self._numerator ** -other)
            return Fraction((-self._denominator) ** -other, (-self._numerator) ** -other)
        elif isinstance(other, Fraction):
            if other._denominator == 1:
                return self ** other._numerator
            if sys._compiled:                   # (one result type: a fractional power as a Fraction)
                return Fraction.from_float(float(self) ** float(other))
            return float(self) ** float(other)
        elif isinstance(other, float):
            return float(self) ** other
        else:
            raise TypeError("unsupported operand type(s) for ** or pow(): 'Fraction' and '" + type(other).__name__ + "'")

    def __rpow__(self, other: _O):
        if isinstance(other, int):
            if sys._compiled:                   # (one result type: float)
                return float(other) ** float(self)
            if self._denominator == 1 and self._numerator >= 0:
                return other ** self._numerator
            if self._denominator == 1:
                return Fraction(other) ** self._numerator
            return other ** float(self)
        elif isinstance(other, float):
            return other ** float(self)
        else:
            raise TypeError("unsupported operand type(s) for ** or pow(): '" + type(other).__name__ + "' and 'Fraction'")

    def __pos__(self) -> "Fraction":
        return Fraction(self._numerator, self._denominator)

    def __neg__(self) -> "Fraction":
        return Fraction(-self._numerator, self._denominator)

    def __abs__(self) -> "Fraction":
        return Fraction(abs(self._numerator), self._denominator)

    def __int__(self) -> int:
        return self.__trunc__()

    def __trunc__(self) -> int:
        if self._numerator < 0:
            return -(-self._numerator // self._denominator)
        return self._numerator // self._denominator

    def __floor__(self) -> int:
        return self._numerator // self._denominator

    def __ceil__(self) -> int:
        return -(-self._numerator // self._denominator)

    def __float__(self) -> float:
        return _int_ratio_float(self._numerator, self._denominator)

    def __round__(self, ndigits: int | None = None) -> int:
        if ndigits is not None:
            if not sys._compiled:
                return self.__mpy_round_n(ndigits)
            raise TypeError("round(Fraction, n): compiled code calls __mpy_round_n")
        d = self._denominator
        floor = self._numerator // d
        remainder = self._numerator - floor * d
        if remainder * 2 < d:
            return floor
        elif remainder * 2 > d:
            return floor + 1
        elif floor % 2 == 0:
            return floor
        else:
            return floor + 1

    def __mpy_round_n(self, ndigits: int) -> "Fraction":
        """round(self, ndigits) (the compiler calls it for round's two-argument form)."""
        shift = 10 ** abs(ndigits)
        if ndigits > 0:
            return Fraction(round(self * shift), shift)
        return Fraction(round(self / shift) * shift)

    def __hash__(self) -> int:
        dinv = _modinv(self._denominator % _PyHASH_MODULUS, _PyHASH_MODULUS)
        h = _PyHASH_INF
        if dinv >= 0:
            h = _mulmod(abs(self._numerator) % _PyHASH_MODULUS, dinv, _PyHASH_MODULUS)
        result = h if self._numerator >= 0 else -h
        return -2 if result == -1 else result

    def __eq__(self, other: _O) -> bool:
        if isinstance(other, Fraction):
            return self._numerator == other._numerator and self._denominator == other._denominator
        elif isinstance(other, bool):
            return self._denominator == 1 and self._numerator == int(other)
        elif isinstance(other, int):
            return self._denominator == 1 and self._numerator == other
        elif isinstance(other, float):
            if other != other or other == float("inf") or other == float("-inf"):
                return False
            return self == Fraction.from_float(other)
        else:
            return False

    def __ne__(self, other: _O) -> bool:
        return not self == other

    def _cmp(self, other: _O) -> int:
        if isinstance(other, Fraction):
            a = self._numerator * other._denominator
            b = other._numerator * self._denominator
            return (a > b) - (a < b)
        elif isinstance(other, int):
            a = self._numerator
            b = other * self._denominator
            return (a > b) - (a < b)
        elif isinstance(other, float):
            if other == float("inf"):
                return -1
            if other == float("-inf"):
                return 1
            return self._cmp(Fraction.from_float(other))
        else:
            raise TypeError("'<' not supported between instances of 'Fraction' and '" + type(other).__name__ + "'")

    def __lt__(self, other: _O) -> bool:
        return self._cmp(other) < 0

    def __le__(self, other: _O) -> bool:
        return self._cmp(other) <= 0

    def __gt__(self, other: _O) -> bool:
        return self._cmp(other) > 0

    def __ge__(self, other: _O) -> bool:
        return self._cmp(other) >= 0

    def __bool__(self) -> bool:
        return self._numerator != 0
