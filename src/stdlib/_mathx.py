"""The math functions compiled programs get from Python code: the compiler turns
math.fsum(xs) into __mpy__mathx__fsum(xs) and so on, for each math function the
code generator does not do itself. (The interpreter's math module is native.)

fsum is CPython's (Shewchuk's exact partial sums); gamma and lgamma are CPython's
Lanczos code; erf and erfc follow fdlibm (as most C libraries do); expm1 and log1p
use Kahan's and Goldberg's formulas (a few units in the last place)."""
import math
from typing import TypeVar

_T = TypeVar("_T")
_U = TypeVar("_U")


def _fsum(seq: _T) -> float:
    """The sum of the numbers, correctly rounded."""
    p: list[float] = []
    special_sum = 0.0
    inf_sum = 0.0
    lo = 0.0
    for item in seq:
        x = float(item)
        xsave = x
        i = 0
        for j in range(len(p)):
            y = p[j]
            if abs(x) < abs(y):
                t = x
                x = y
                y = t
            hi = x + y
            yr = hi - x
            lo = y - yr
            if lo != 0.0:
                p[i] = lo
                i += 1
            x = hi
        del p[i:]
        if x != 0.0:
            if not math.isfinite(x):
                if math.isfinite(xsave):
                    raise OverflowError("intermediate overflow in fsum")
                if math.isinf(xsave):
                    inf_sum += xsave
                special_sum += xsave
                p = []
            else:
                p.append(x)
    if special_sum != 0.0:
        if math.isnan(inf_sum):
            raise ValueError("-inf + inf in fsum")
        return special_sum
    hi = 0.0
    n = len(p)
    if n > 0:
        n -= 1
        hi = p[n]
        while n > 0:
            x = hi
            n -= 1
            y = p[n]
            hi = x + y
            yr = hi - x
            lo = y - yr
            if lo != 0.0:
                break
        if n > 0 and ((lo < 0.0 and p[n - 1] < 0.0) or (lo > 0.0 and p[n - 1] > 0.0)):
            y = lo * 2.0
            x = hi + y
            yr = x - hi
            if y == yr:
                hi = x
    return hi


def _prod(iterable: _T, start: _U = 1):
    """start times all the numbers."""
    items = list(iterable)
    if not items:
        return start + 0 * sum(items)
    r = start * items[0]
    for i in range(1, len(items)):
        r = r * items[i]
    return r


def _factorial(n: int) -> int:
    if n < 0:
        raise ValueError("factorial() not defined for negative values")
    r = 1
    for k in range(2, n + 1):
        r *= k
    return r


def _comb(n: int, k: int) -> int:
    """The number of ways to choose k of n items."""
    if n < 0:
        raise ValueError("n must be a non-negative integer")
    if k < 0:
        raise ValueError("k must be a non-negative integer")
    if k > n:
        return 0
    k = min(k, n - k)
    r = 1
    for i in range(1, k + 1):
        r = r * (n - k + i) // i
    return r


def _perm(n: int, k: int | None = None) -> int:
    """The number of ordered choices of k of n items (k: n by default)."""
    if n < 0:
        raise ValueError("n must be a non-negative integer")
    kk = n if k is None else k
    if kk < 0:
        raise ValueError("k must be a non-negative integer")
    if kk > n:
        return 0
    r = 1
    for i in range(n - kk + 1, n + 1):
        r *= i
    return r


def _lcm(*integers: int) -> int:
    r = 1
    for x in integers:
        if x == 0:
            return 0
        r = abs(r * x) // math.gcd(r, x)
    return r


def _isclose(a: float, b: float, *, rel_tol: float = 1e-09, abs_tol: float = 0.0) -> bool:
    if rel_tol < 0.0 or abs_tol < 0.0:
        raise ValueError("tolerances must be non-negative")
    if a == b:
        return True
    if math.isinf(a) or math.isinf(b):
        return False
    diff = abs(b - a)
    return diff <= abs(rel_tol * b) or diff <= abs(rel_tol * a) or diff <= abs_tol


def _two_prod(a: float, b: float) -> tuple[float, float]:
    """(a*b rounded, its exact error) (Dekker's product)."""
    z = a * b
    c = 134217729.0 * a
    ah = c - (c - a)
    al = a - ah
    c = 134217729.0 * b
    bh = c - (c - b)
    bl = b - bh
    return (z, ((ah * bh - z) + ah * bl + al * bh) + al * bl)


def _vector_norm(vec: list[float], mx: float) -> float:
    """sqrt(sum of squares), as CPython's math.dist / math.hypot compute it."""
    if math.isinf(mx):
        return mx
    if mx == 0.0 or len(vec) <= 1:
        return mx
    m, max_e = math.frexp(mx)
    scale = math.ldexp(1.0, -max_e)
    csum = 1.0
    frac1 = 0.0
    frac2 = 0.0
    for v in vec:
        x = v * scale
        pr = _two_prod(x, x)
        hi = csum + pr[0]
        lo = (csum - hi) + pr[0]
        csum = hi
        frac1 += pr[1]
        frac2 += lo
    h = math.sqrt(csum - 1.0 + (frac1 + frac2))
    pr = _two_prod(-h, h)
    hi = csum + pr[0]
    lo = (csum - hi) + pr[0]
    csum = hi
    frac1 += pr[1]
    frac2 += lo
    x = csum - 1.0 + (frac1 + frac2)
    h += x / (2.0 * h)
    return h / scale


def _dist(p: _T, q: _U) -> float:
    """The Euclidean distance of two points (sequences of coordinates)."""
    if len(p) != len(q):
        raise ValueError("both points must have the same number of dimensions")
    mx = 0.0
    found_nan = False
    diffs: list[float] = []
    for i in range(len(p)):
        d = abs(float(p[i]) - float(q[i]))
        diffs.append(d)
        found_nan = found_nan or d != d
        if d > mx:
            mx = d
    if found_nan and not math.isinf(mx):
        return float("nan")
    return _vector_norm(diffs, mx)


def _modf(x: float) -> tuple[float, float]:
    """(the fractional part, the integer part) of x, both with x's sign."""
    if math.isinf(x):
        return (math.copysign(0.0, x), x)
    i = float(math.trunc(x)) if abs(x) < 4503599627370496.0 else x
    return (math.copysign(x - i, x), i)


def _remainder(x: float, y: float) -> float:
    """x - n*y, n the integer nearest x/y (ties to even) (CPython's m_remainder)."""
    if math.isnan(x):
        return x
    if math.isnan(y):
        return y
    if math.isinf(x) or y == 0.0:
        raise ValueError("math domain error")
    if math.isinf(y):
        return x
    absx = abs(x)
    absy = abs(y)
    m = math.fmod(absx, absy)
    c = absy - m
    r = 0.0
    if m < c:
        r = m
    elif m > c:
        r = -c
    else:
        r = m - 2.0 * math.fmod(0.5 * (absx - m), absy)
    return math.copysign(1.0, x) * r


def _cbrt(x: float) -> float:
    if x == 0.0 or math.isinf(x) or math.isnan(x):
        return x
    y = abs(x) ** (1.0 / 3.0)
    y = y - (y * y * y - abs(x)) / (3.0 * y * y)            # one Newton step
    return math.copysign(y, x)


def _exp2(x: float) -> float:
    if x == math.floor(x) and abs(x) < 1100:
        return math.ldexp(1.0, int(x))
    return 2.0 ** x


# -- fdlibm's expm1, log1p, erf, erfc (as C libraries have them)

def _expm1(x: float) -> float:
    """e**x - 1, accurate for small x (Kahan's formula)."""
    if math.isnan(x):
        return x
    if math.isinf(x):
        return -1.0 if x < 0 else x
    if abs(x) < 1e-5:
        return x + 0.5 * x * x + x * x * x / 6.0
    if x > 709.782712893384:
        raise OverflowError("math range error")
    u = math.exp(x)
    if u == 1.0:
        return x
    um1 = u - 1.0
    if um1 == -1.0:
        return -1.0
    return um1 * x / math.log(u)


def _log1p(x: float) -> float:
    """log(1 + x), accurate for small x (Goldberg's formula)."""
    if math.isnan(x):
        return x
    if x <= -1.0:
        raise ValueError("math domain error")
    if math.isinf(x):
        return x
    u = 1.0 + x
    if u == 1.0:
        return x
    return math.log(u) * x / (u - 1.0)


_erx = 8.45062911510467529297e-01
_efx8 = 1.02703333676410069053e+00
_pp = [1.28379167095512558561e-01, -3.25042107247001499370e-01, -2.84817495755985104766e-02,
       -5.77027029648944159157e-03, -2.37630166566501626084e-05]
_qq = [3.97917223959155352819e-01, 6.50222499887672944485e-02, 5.08130628187576562776e-03,
       1.32494738004321644526e-04, -3.96022827877536812320e-06]
_pa = [-2.36211856075265944077e-03, 4.14856118683748331666e-01, -3.72207876035701323847e-01,
       3.18346619901161753674e-01, -1.10894694282396677476e-01, 3.54783043256182359371e-02,
       -2.16637559486879084300e-03]
_qa = [1.06420880400844228286e-01, 5.40397917702171048937e-01, 7.18286544141962662868e-02,
       1.26171219808761642112e-01, 1.36370839120290507362e-02, 1.19844998467991074170e-02]
_ra = [-9.86494403484714822705e-03, -6.93858572707181764372e-01, -1.05586262253232909814e+01,
       -6.23753324503260060396e+01, -1.62396669462573470355e+02, -1.84605092906711035994e+02,
       -8.12874355063065934246e+01, -9.81432934416914548592e+00]
_sa = [1.96512716674392571292e+01, 1.37657754143519042600e+02, 4.34565877475229228821e+02,
       6.45387271733267880336e+02, 4.29008140027567833386e+02, 1.08635005541779435134e+02,
       6.57024977031928170135e+00, -6.04244152148580987438e-02]
_rb = [-9.86494292470009928597e-03, -7.99283237680523006574e-01, -1.77579549177547519889e+01,
       -1.60636384855821916062e+02, -6.37566443368389627722e+02, -1.02509513161107724954e+03,
       -4.83519191608651397019e+02]
_sb = [3.03380607434824582924e+01, 3.25792512996573918826e+02, 1.53672958608443695994e+03,
       3.19985821950859553908e+03, 2.55305040643316442583e+03, 4.74528541206955367215e+02,
       -2.24409524465858183362e+01]


def _poly(c: list[float], x: float) -> float:
    r = 0.0
    for i in range(len(c) - 1, -1, -1):
        r = r * x + c[i]
    return r


def _erf_tail(ax: float) -> float:
    """erfc(ax) for 1.25 <= ax < 28 (fdlibm)."""
    s = 1.0 / (ax * ax)
    R = 0.0
    S = 0.0
    if ax < 1.0 / 0.35:
        R = _poly(_ra, s)
        S = 1.0 + s * _poly(_sa, s)
    else:
        R = _poly(_rb, s)
        S = 1.0 + s * _poly(_sb, s)
    m, e = math.frexp(ax)
    z = math.ldexp(math.floor(math.ldexp(m, 21)), e - 21)      # ax with its low 32 bits cleared
    r = math.exp(-z * z - 0.5625) * math.exp((z - ax) * (z + ax) + R / S)
    return r / ax


def _erf(x: float) -> float:
    if math.isnan(x):
        return x
    if math.isinf(x):
        return math.copysign(1.0, x)
    ax = abs(x)
    if ax < 0.84375:
        if ax < 2.0 ** -28:
            return 0.125 * (8.0 * x + _efx8 * x)
        z = x * x
        r = _pp[0] + z * (_pp[1] + z * (_pp[2] + z * (_pp[3] + z * _pp[4])))
        s = 1.0 + z * (_qq[0] + z * (_qq[1] + z * (_qq[2] + z * (_qq[3] + z * _qq[4]))))
        return x + x * (r / s)
    if ax < 1.25:
        s = ax - 1.0
        P = _poly(_pa, s)
        Q = 1.0 + s * _poly(_qa, s)
        return _erx + P / Q if x >= 0 else -_erx - P / Q
    if ax >= 6.0:
        return math.copysign(1.0, x)
    r = _erf_tail(ax)
    return 1.0 - r if x >= 0 else r - 1.0


def _erfc(x: float) -> float:
    if math.isnan(x):
        return x
    if math.isinf(x):
        return 0.0 if x > 0 else 2.0
    ax = abs(x)
    if ax < 0.84375:
        if ax < 2.0 ** -56:
            return 1.0 - x
        z = x * x
        r = _pp[0] + z * (_pp[1] + z * (_pp[2] + z * (_pp[3] + z * _pp[4])))
        s = 1.0 + z * (_qq[0] + z * (_qq[1] + z * (_qq[2] + z * (_qq[3] + z * _qq[4]))))
        y = r / s
        if x < 0.25:
            return 1.0 - (x + x * y)
        return 0.5 - (x - 0.5 + x * y)
    if ax < 1.25:
        s = ax - 1.0
        P = _poly(_pa, s)
        Q = 1.0 + s * _poly(_qa, s)
        if x >= 0:
            return 1.0 - _erx - P / Q
        return 1.0 + (_erx + P / Q)
    if ax < 28.0:
        if x < -6.0:
            return 2.0
        r = _erf_tail(ax)
        return r if x > 0 else 2.0 - r
    return 0.0 if x > 0 else 2.0


# -- CPython's gamma and lgamma (Lanczos, N=13)

_lanczos_g = 6.024680040776729583740234375
_lanczos_g_minus_half = 5.524680040776729583740234375
_lanczos_num = [23531376880.410759688572007674451636754734846804940, 42919803642.649098768957899047001988850926355848959,
                35711959237.355668049440185451547166705960488635843, 17921034426.037209699919755754458931112671403265390,
                6039542586.3520280050642916443072979210699388420708, 1439720407.3117216736632230727949123939715485786772,
                248874557.86205415651146038641322942321632125127801, 31426415.585400194380614231628318205362874684987640,
                2876370.6289353724412254090516208496135991145378768, 186056.26539522349504029498971604569928220784236328,
                8071.6720023658162106380029022722506138218516325024, 210.82427775157934587250973392071336271166969580291,
                2.5066282746310002701649081771338373386264310793408]
_lanczos_den = [0.0, 39916800.0, 120543840.0, 150917976.0, 105258076.0, 45995730.0, 13339535.0, 2637558.0,
                357423.0, 32670.0, 1925.0, 66.0, 1.0]
_gamma_integral = [1.0, 1.0, 2.0, 6.0, 24.0, 120.0, 720.0, 5040.0, 40320.0, 362880.0, 3628800.0, 39916800.0,
                   479001600.0, 6227020800.0, 87178291200.0, 1307674368000.0, 20922789888000.0,
                   355687428096000.0, 6402373705728000.0, 121645100408832000.0, 2432902008176640000.0,
                   51090942171709440000.0, 1124000727777607680000.0]
_logpi = 1.144729885849400174143427351353058711647


def _sinpi(x: float) -> float:
    y = math.fmod(abs(x), 2.0)
    n = int(round(2.0 * y))
    r = 0.0
    if n == 0:
        r = math.sin(math.pi * y)
    elif n == 1:
        r = math.cos(math.pi * (y - 0.5))
    elif n == 2:
        r = math.sin(math.pi * (1.0 - y))
    elif n == 3:
        r = -math.cos(math.pi * (y - 1.5))
    else:
        r = math.sin(math.pi * (y - 2.0))
    return math.copysign(1.0, x) * r


def _lanczos_sum(x: float) -> float:
    num = 0.0
    den = 0.0
    if x < 5.0:
        for i in range(12, -1, -1):
            num = num * x + _lanczos_num[i]
            den = den * x + _lanczos_den[i]
    else:
        for i in range(13):
            num = num / x + _lanczos_num[i]
            den = den / x + _lanczos_den[i]
    return num / den


def _gamma(x: float) -> float:
    if not math.isfinite(x):
        if math.isnan(x) or x > 0.0:
            return x
        raise ValueError("math domain error")
    if x == 0.0 or (x == math.floor(x) and x < 0.0):
        raise ValueError("expected a noninteger or positive integer, got " + repr(x))
    if x == math.floor(x):
        if x <= 23:
            return _gamma_integral[int(x) - 1]
    absx = abs(x)
    if absx < 1e-20:
        return 1.0 / x
    if absx > 200.0:
        if x < 0.0:
            return 0.0 / _sinpi(x)
        raise OverflowError("math range error")
    y = absx + _lanczos_g_minus_half
    z = 0.0
    if absx > _lanczos_g_minus_half:
        q = y - absx
        z = q - _lanczos_g_minus_half
    else:
        q = y - _lanczos_g_minus_half
        z = q - absx
    z = z * _lanczos_g / y
    r = 0.0
    if x < 0.0:
        r = -math.pi / _sinpi(absx) / absx * math.exp(y) / _lanczos_sum(absx)
        r -= z * r
        if absx < 140.0:
            r /= y ** (absx - 0.5)
        else:
            sqrtpow = y ** (absx / 2.0 - 0.25)
            r /= sqrtpow
            r /= sqrtpow
    else:
        r = _lanczos_sum(absx) / math.exp(y)
        r += z * r
        if absx < 140.0:
            r *= y ** (absx - 0.5)
        else:
            sqrtpow = y ** (absx / 2.0 - 0.25)
            r *= sqrtpow
            r *= sqrtpow
    if math.isinf(r):
        raise OverflowError("math range error")
    return r


def _lgamma(x: float) -> float:
    if not math.isfinite(x):
        if math.isnan(x):
            return x
        return float("inf")
    if x == math.floor(x) and x <= 2.0:
        if x <= 0.0:
            raise ValueError("math domain error")
        return 0.0
    absx = abs(x)
    if absx < 1e-20:
        return -math.log(absx)
    r = math.log(_lanczos_sum(absx)) - _lanczos_g
    r += (absx - 0.5) * (math.log(absx + _lanczos_g - 0.5) - 1)
    if x < 0.0:
        r = _logpi - math.log(abs(_sinpi(absx))) - math.log(absx) - r
    if math.isinf(r):
        raise OverflowError("math range error")
    return r


# -- hyperbolic functions

def _sinh(x: float) -> float:
    if abs(x) < 22.0:
        t = _expm1(abs(x))
        r = 0.5 * (t + t / (t + 1.0))
        return math.copysign(r, x)
    if abs(x) > 710.4758600739439:
        raise OverflowError("math range error")
    w = math.exp(0.5 * abs(x))
    return math.copysign(0.5 * w * w, x)


def _cosh(x: float) -> float:
    ax = abs(x)
    if ax < 0.34657359027997264:
        t = _expm1(ax)
        w = 1.0 + t
        return 1.0 + (t * t) / (w + w)
    if ax < 22.0:
        t = math.exp(ax)
        return 0.5 * t + 0.5 / t
    if ax > 710.4758600739439:
        raise OverflowError("math range error")
    w = math.exp(0.5 * ax)
    return 0.5 * w * w


def _tanh(x: float) -> float:
    ax = abs(x)
    if math.isnan(x):
        return x
    if ax < 22.0:
        if ax < 2.0 ** -55:
            return x
        if ax >= 1.0:
            t = _expm1(2.0 * ax)
            return math.copysign(1.0 - 2.0 / (t + 2.0), x)
        t = _expm1(-2.0 * ax)
        return math.copysign(-t / (t + 2.0), x)
    return math.copysign(1.0, x)


def _asinh(x: float) -> float:
    ax = abs(x)
    if math.isnan(x) or math.isinf(x):
        return x
    if ax < 2.0 ** -28:
        return x
    w = 0.0
    if ax > 2.0 ** 28:
        w = math.log(ax) + 0.6931471805599453
    elif ax > 2.0:
        w = math.log(2.0 * ax + 1.0 / (math.sqrt(x * x + 1.0) + ax))
    else:
        t = x * x
        w = _log1p(ax + t / (1.0 + math.sqrt(1.0 + t)))
    return math.copysign(w, x)


def _acosh(x: float) -> float:
    if x < 1.0:
        raise ValueError("math domain error")
    if x >= 2.0 ** 28:
        if math.isinf(x):
            return x
        return math.log(x) + 0.6931471805599453
    if x == 1.0:
        return 0.0
    if x > 2.0:
        t = x * x
        return math.log(2.0 * x - 1.0 / (x + math.sqrt(t - 1.0)))
    t = x - 1.0
    return _log1p(t + math.sqrt(2.0 * t + t * t))


def _atanh(x: float) -> float:
    ax = abs(x)
    if ax >= 1.0:
        raise ValueError("math domain error")
    if ax < 2.0 ** -28:
        return x
    t = 0.0
    if ax < 0.5:
        t = ax + ax
        t = 0.5 * _log1p(t + t * ax / (1.0 - ax))
    else:
        t = 0.5 * _log1p((ax + ax) / (1.0 - ax))
    return math.copysign(t, x)


def _sumprod(p: _T, q: _U):
    """The sum of the products of the items of p and q."""
    if len(p) != len(q):
        raise ValueError("Inputs are not the same length")
    total = p[0] * q[0] if len(p) else 0
    for i in range(1, len(p)):
        total += p[i] * q[i]
    return total
