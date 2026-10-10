"""Basic statistics (CPython's statistics): averages (mean, fmean, geometric_mean,
harmonic_mean, median*, mode, multimode, quantiles), spread (variance, stdev and the
population forms), relations (covariance, correlation, linear_regression) and NormalDist.

As in CPython, sums of ints are exact (mean([1, 2, 3]) is the int 2); sums of floats
are math.fsum's (correctly rounded, then divided). In compiled programs the results
are floats (one type per function), and the medians of a list are floats."""
import math
import sys
from typing import TypeVar

_T = TypeVar("_T")
_U = TypeVar("_U")

__all__ = ["NormalDist", "StatisticsError", "correlation", "covariance", "fmean", "geometric_mean",
           "harmonic_mean", "linear_regression", "mean", "median", "median_grouped", "median_high", "median_low",
           "mode", "multimode", "pstdev", "pvariance", "quantiles", "stdev", "variance"]


class StatisticsError(ValueError):
    pass


def _floats(data: _T) -> list[float]:
    out: list[float] = []
    for x in data:
        out.append(float(x))
    return out


def _exact_sum_f(xs: list[float]) -> tuple[bool, "Fraction"]:
    """(True, the exact sum of the floats as a Fraction), or (False, 0) when 64-bit ints cannot hold it."""
    from fractions import Fraction
    t = Fraction(0)
    try:
        for x in xs:
            t = t + Fraction(x)
    except OverflowError:
        return (False, Fraction(0))
    return (True, t)


def _exact_var_f(xs: list[float], c: float | None, ddof: int) -> float:
    """The variance of floats (sum of squared deviations from c or the mean, / ddof), exactly when possible."""
    from fractions import Fraction
    n = len(xs)
    try:
        ok, t = _exact_sum_f(xs)
        if ok:
            m = t / n if c is None else Fraction(c)
            ss = Fraction(0)
            dev = Fraction(0)
            for x in xs:
                d = Fraction(x) - m
                ss = ss + d * d
                dev = dev + d
            if c is None:
                ss = ss - dev * dev / n
            return float(ss / ddof)
    except OverflowError:
        pass
    return _ss_float(xs, c) / ddof


def _all_ints(data: _T) -> bool:
    for x in data:
        if not isinstance(x, int):
            return False
    return True


def mean(data: _T):
    """The arithmetic mean (an int for ints when it is one)."""
    xs = list(data)
    n = len(xs)
    if n < 1:
        raise StatisticsError("mean requires at least one data point")
    if not sys._compiled:
        return _mean_dyn(xs)
    fx = _floats(xs)
    ok, t = _exact_sum_f(fx)
    if ok:
        try:
            return float(t / n)
        except OverflowError:
            pass
    return math.fsum(fx) / n


def fmean(data: _T, weights: _U = None) -> float:
    """The arithmetic mean as a float (weighted with weights)."""
    xs = _floats(data)
    n = len(xs)
    if weights is None:
        if n == 0:
            raise StatisticsError("fmean requires at least one data point")
        return math.fsum(xs) / n
    ws = _floats(weights)
    if len(ws) != n:
        raise StatisticsError("data and weights must be the same length")
    num = math.fsum([xs[i] * ws[i] for i in range(n)])
    den = math.fsum(ws)
    if not den:
        raise StatisticsError("sum of weights must be non-zero")
    return num / den


def geometric_mean(data: _T) -> float:
    """The geometric mean (exp of the mean of the logarithms)."""
    xs = _floats(data)
    n = 0
    found_zero = False
    logs: list[float] = []
    for x in xs:
        if x > 0.0:
            n += 1
            logs.append(math.log(x))
        elif x == 0.0:
            found_zero = True
        else:
            raise StatisticsError("No negative inputs allowed: " + repr(x))
    if found_zero:
        return 0.0 if n else 0.0
    if not n:
        raise StatisticsError("Must have a non-empty dataset")
    return math.exp(math.fsum(logs) / n)


def harmonic_mean(data: _T, weights: _U = None):
    """The reciprocal of the mean of the reciprocals (of the weighted ones)."""
    xs = list(data)
    n = len(xs)
    errmsg = "harmonic mean does not support negative values"
    if n < 1:
        raise StatisticsError("harmonic_mean requires at least one data point")
    if not sys._compiled:
        return _harmonic_dyn(xs, weights)
    fx = _floats(xs)
    ws: list[float] = []
    if weights is None:
        ws = [1.0] * n
    else:
        ws = _floats(weights)
        if len(ws) != n:
            raise StatisticsError("Number of weights does not match data size")
    for v in fx:
        if v < 0:
            raise StatisticsError(errmsg)
    for w in ws:
        if w < 0:
            raise StatisticsError(errmsg)
    terms: list[float] = []
    for i in range(n):
        if ws[i]:
            if fx[i] == 0.0:
                return 0.0
            terms.append(ws[i] / fx[i])
    ok, t = _exact_sum_f(terms)
    ok2, sw = _exact_sum_f(ws)
    if ok and ok2:
        if t <= 0:
            raise StatisticsError("Weighted sum must be positive")
        try:
            return float(sw / t)
        except OverflowError:
            pass
    total = math.fsum(terms)
    if total <= 0:
        raise StatisticsError("Weighted sum must be positive")
    return math.fsum(ws) / total


def median(data: _T):
    """The middle value (the mean of the two middle ones for an even count)."""
    xs = sorted(data)
    n = len(xs)
    if n == 0:
        raise StatisticsError("no median for empty data")
    if not sys._compiled:
        if n % 2 == 1:
            return xs[n // 2]
        i = n // 2
        return (xs[i - 1] + xs[i]) / 2
    if n % 2 == 1:
        return float(xs[n // 2])
    i = n // 2
    return (float(xs[i - 1]) + float(xs[i])) / 2


def median_low(data: _T):
    """The lower of the two middle values (the middle one for an odd count)."""
    xs = sorted(data)
    n = len(xs)
    if n == 0:
        raise StatisticsError("no median for empty data")
    if n % 2 == 1:
        return xs[n // 2]
    return xs[n // 2 - 1]


def median_high(data: _T):
    """The higher of the two middle values."""
    xs = sorted(data)
    n = len(xs)
    if n == 0:
        raise StatisticsError("no median for empty data")
    return xs[n // 2]


def median_grouped(data: _T, interval: float = 1.0) -> float:
    """The median of grouped data (each value the midpoint of a bin interval wide)."""
    xs = sorted(data)
    n = len(xs)
    if not n:
        raise StatisticsError("no median for empty data")
    x = xs[n // 2]
    i = 0
    while i < n and xs[i] < x:
        i += 1
    j = i
    while j < n and xs[j] == x:
        j += 1
    fx = float(x)
    fi = float(interval)
    lower = fx - fi / 2.0
    return lower + fi * (n / 2 - i) / (j - i)


def mode(data: _T):
    """The most common value (the first of them on a tie)."""
    xs = list(data)
    if not xs:
        raise StatisticsError("no mode for empty data")
    counts: dict = {}
    for x in xs:
        counts[x] = counts.get(x, 0) + 1
    best = xs[0]
    bc = 0
    for k, c in counts.items():
        if c > bc:
            best = k
            bc = c
    return best


def multimode(data: _T) -> list:
    """The most common values, in the order they come first."""
    xs = list(data)
    counts: dict = {}
    for x in xs:
        counts[x] = counts.get(x, 0) + 1
    if not counts:
        return []
    maxcount = max(counts.values())
    return [k for k, c in counts.items() if c == maxcount]


def _ss_float(xs: list[float], c: float | None) -> float:
    """The sum of squared deviations from c (the mean by default), fsum-accurate."""
    n = len(xs)
    m = math.fsum(xs) / n if c is None else c
    dev = [x - m for x in xs]
    total = math.fsum([d * d for d in dev])
    if c is None:
        s = math.fsum(dev)
        total -= s * s / n
    return total


def variance(data: _T, xbar: _U = None):
    """The sample variance (n - 1 in the denominator)."""
    xs = list(data)
    n = len(xs)
    if n < 2:
        raise StatisticsError("variance requires at least two data points")
    if not sys._compiled:
        return _var_dyn(xs, xbar, n - 1)
    c: float | None = None
    if xbar is not None:
        c = float(xbar)
    return _exact_var_f(_floats(xs), c, n - 1)


def pvariance(data: _T, mu: _U = None):
    """The population variance (n in the denominator)."""
    xs = list(data)
    n = len(xs)
    if n < 1:
        raise StatisticsError("pvariance requires at least one data point")
    if not sys._compiled:
        return _var_dyn(xs, mu, n)
    c: float | None = None
    if mu is not None:
        c = float(mu)
    return _exact_var_f(_floats(xs), c, n)


def stdev(data: _T, xbar: _U = None) -> float:
    """The sample standard deviation (the square root of variance)."""
    xs = list(data)
    n = len(xs)
    if n < 2:
        raise StatisticsError("stdev requires at least two data points")
    if not sys._compiled:
        return _sqrt_dyn(_var_dyn(xs, xbar, n - 1))
    c: float | None = None
    if xbar is not None:
        c = float(xbar)
    return math.sqrt(_exact_var_f(_floats(xs), c, n - 1))


def pstdev(data: _T, mu: _U = None) -> float:
    """The population standard deviation."""
    xs = list(data)
    n = len(xs)
    if n < 1:
        raise StatisticsError("pstdev requires at least one data point")
    if not sys._compiled:
        return _sqrt_dyn(_var_dyn(xs, mu, n))
    c: float | None = None
    if mu is not None:
        c = float(mu)
    return math.sqrt(_exact_var_f(_floats(xs), c, n))


def quantiles(data: _T, *, n: int = 4, method: str = "exclusive") -> list:
    """n - 1 cut points dividing data into n equally likely intervals."""
    if n < 1:
        raise StatisticsError("n must be at least 1")
    xs = sorted(data)
    ld = len(xs)
    if ld < 2:
        if ld == 1:
            if not sys._compiled:
                return xs * (n - 1)
            return [float(xs[0])] * (n - 1)
        raise StatisticsError("must have at least one data point")
    result: list[float] = []
    if method == "inclusive":
        m = ld - 1
        for i in range(1, n):
            j = i * m // n
            delta = i * m - j * n
            result.append((float(xs[j]) * (n - delta) + float(xs[j + 1]) * delta) / n)
        if not sys._compiled:
            return [(xs[j2] * (n - d2) + xs[j2 + 1] * d2) / n for j2, d2 in [divmod(i * m, n) for i in range(1, n)]]
        return result
    if method == "exclusive":
        m = ld + 1
        for i in range(1, n):
            j = i * m // n
            j = 1 if j < 1 else ld - 1 if j > ld - 1 else j
            delta = i * m - j * n
            result.append((float(xs[j - 1]) * (n - delta) + float(xs[j]) * delta) / n)
        if not sys._compiled:
            out = []
            for i in range(1, n):
                j = i * m // n
                j = 1 if j < 1 else ld - 1 if j > ld - 1 else j
                delta = i * m - j * n
                out.append((xs[j - 1] * (n - delta) + xs[j] * delta) / n)
            return out
        return result
    raise ValueError("Unknown method: " + repr(method))


def _sumprod(x: list[float], y: list[float]) -> float:
    return math.fsum([x[i] * y[i] for i in range(len(x))])


def covariance(x: _T, y: _U) -> float:
    """The sample covariance of x and y."""
    fx = _floats(x)
    fy = _floats(y)
    n = len(fx)
    if len(fy) != n:
        raise StatisticsError("covariance requires that both inputs have same number of data points")
    if n < 2:
        raise StatisticsError("covariance requires at least two data points")
    xbar = math.fsum(fx) / n
    ybar = math.fsum(fy) / n
    return _sumprod([v - xbar for v in fx], [v - ybar for v in fy]) / (n - 1)


def _rank(data: list[float], start: float) -> list[float]:
    """The ranks of the values (ties: their average rank), from start."""
    n = len(data)
    order = sorted(range(n), key=lambda k: data[k])
    result = [0.0] * n
    i = 0
    while i < n:
        j = i
        while j + 1 < n and data[order[j + 1]] == data[order[i]]:
            j += 1
        rank = start + (i + j) / 2
        for k in range(i, j + 1):
            result[order[k]] = rank
        i = j + 1
    return result


def _sqrtprod(x: float, y: float) -> float:
    h = math.sqrt(x * y)
    if not math.isfinite(h):
        if math.isinf(h) and not math.isinf(x) and not math.isinf(y):
            scale = 2.0 ** -512
            return _sqrtprod(scale * x, scale * y) / scale
        return h
    if not h:
        if x and y:
            scale = 2.0 ** 537
            return _sqrtprod(scale * x, scale * y) / scale
        return h
    d = math.fsum([x * y, -h * h])
    return h + d / (2.0 * h)


def correlation(x: _T, y: _U, *, method: str = "linear") -> float:
    """Pearson's correlation coefficient of x and y (Spearman's rank one for method='ranked')."""
    fx = _floats(x)
    fy = _floats(y)
    n = len(fx)
    if len(fy) != n:
        raise StatisticsError("correlation requires that both inputs have same number of data points")
    if n < 2:
        raise StatisticsError("correlation requires at least two data points")
    if method != "linear" and method != "ranked":
        raise ValueError("Unknown method: " + repr(method))
    if method == "ranked":
        start = (n - 1) / -2
        fx = _rank(fx, start)
        fy = _rank(fy, start)
    else:
        xbar = math.fsum(fx) / n
        ybar = math.fsum(fy) / n
        fx = [v - xbar for v in fx]
        fy = [v - ybar for v in fy]
    sxy = _sumprod(fx, fy)
    sxx = _sumprod(fx, fx)
    syy = _sumprod(fy, fy)
    d = _sqrtprod(sxx, syy)
    if d == 0.0:
        raise StatisticsError("at least one of the inputs is constant")
    return sxy / d


class LinearRegression:
    """linear_regression()'s result: slope and intercept (a 2-sequence)."""

    def __init__(self, slope: float, intercept: float):
        self.slope = slope
        self.intercept = intercept

    def __getitem__(self, i: int) -> float:
        if i == 0 or i == -2:
            return self.slope
        if i == 1 or i == -1:
            return self.intercept
        raise IndexError("tuple index out of range")

    def __len__(self) -> int:
        return 2

    def __iter__(self):
        yield self.slope
        yield self.intercept

    def __eq__(self, other: _T) -> bool:
        if isinstance(other, LinearRegression):
            return self.slope == other.slope and self.intercept == other.intercept
        else:
            return False

    def __repr__(self) -> str:
        return "LinearRegression(slope=" + repr(self.slope) + ", intercept=" + repr(self.intercept) + ")"


def linear_regression(x: _T, y: _U, *, proportional: bool = False) -> LinearRegression:
    """The least-squares line through the points (through the origin if proportional)."""
    fx = _floats(x)
    fy = _floats(y)
    n = len(fx)
    if len(fy) != n:
        raise StatisticsError("linear regression requires that both inputs have same number of data points")
    if n < 2:
        raise StatisticsError("linear regression requires at least two data points")
    xbar = 0.0
    ybar = 0.0
    if not proportional:
        xbar = math.fsum(fx) / n
        ybar = math.fsum(fy) / n
        fx = [v - xbar for v in fx]
        fy = [v - ybar for v in fy]
    sxy = _sumprod(fx, fy) + 0.0
    sxx = _sumprod(fx, fx)
    if sxx == 0.0:
        raise StatisticsError("x is constant")
    slope = sxy / sxx
    intercept = 0.0 if proportional else ybar - slope * xbar
    return LinearRegression(slope, intercept)


_SQRT2 = math.sqrt(2.0)


def _normal_dist_inv_cdf(p: float, mu: float, sigma: float) -> float:
    q = p - 0.5
    if abs(q) <= 0.425:
        r = 0.180625 - q * q
        num = (((((((2.5090809287301226727e+3 * r + 3.3430575583588128105e+4) * r + 6.7265770927008700853e+4) * r +
                   4.5921953931549871457e+4) * r + 1.3731693765509461125e+4) * r + 1.9715909503065514427e+3) * r +
                1.3314166789178437745e+2) * r + 3.3871328727963666080e+0) * q
        den = (((((((5.2264952788528545610e+3 * r + 2.8729085735721942674e+4) * r + 3.9307895800092710610e+4) * r +
                   2.1213794301586595867e+4) * r + 5.3941960214247511077e+3) * r + 6.8718700749205790830e+2) * r +
                4.2313330701600911252e+1) * r + 1.0)
        x = num / den
        return mu + (x * sigma)
    r = p if q <= 0.0 else 1.0 - p
    r = math.sqrt(-math.log(r))
    num = 0.0
    den = 0.0
    if r <= 5.0:
        r = r - 1.6
        num = (((((((7.7454501427834140764e-4 * r + 2.2723844989269184583e-2) * r + 2.4178072517745061177e-1) * r +
                   1.2704582524523683826e+0) * r + 3.6478483247632045926e+0) * r + 5.7694972214606914055e+0) * r +
                4.6303378461565452959e+0) * r + 1.4234371107496835773e+0)
        den = (((((((1.0507500716444168432e-9 * r + 5.4759380849953449460e-4) * r + 1.5198666563616457197e-2) * r +
                   1.4810397642748007459e-1) * r + 6.8976733498510000455e-1) * r + 1.6763848301838038494e+0) * r +
                2.0531916266377588219e+0) * r + 1.0)
    else:
        r = r - 5.0
        num = (((((((2.0103343992922881327e-7 * r + 2.7115555687434875782e-5) * r + 1.2426609473880784386e-3) * r +
                   2.6532189526576123093e-2) * r + 2.9656057182850489123e-1) * r + 1.7848265399172913358e+0) * r +
                5.4637849111641143699e+0) * r + 6.6579046435011037772e+0)
        den = (((((((2.0442631033899397856e-15 * r + 1.4215117583164458887e-7) * r + 1.8463183175100546818e-5) * r +
                   7.8686913114561325910e-4) * r + 1.4875361290850614853e-2) * r + 1.3692988092273580531e-1) * r +
                5.9983220655588793769e-1) * r + 1.0)
    x = num / den
    if q < 0.0:
        x = -x
    return mu + (x * sigma)


class NormalDist:
    """A normal distribution: mean mu, standard deviation sigma."""

    def __init__(self, mu: float = 0.0, sigma: float = 1.0):
        if sigma < 0.0:
            raise StatisticsError("sigma must be non-negative")
        self._mu = float(mu)
        self._sigma = float(sigma)

    @staticmethod
    def from_samples(data: _T) -> "NormalDist":
        """The distribution with the samples' mean and standard deviation."""
        xs = _floats(data)
        if len(xs) < 2:
            raise StatisticsError("stdev requires at least two data points")
        return NormalDist(math.fsum(xs) / len(xs), stdev(xs))

    def samples(self, n: int, *, seed: int | None = None) -> list[float]:
        """n random values of the distribution."""
        import random
        if seed is not None:
            random.seed(seed)
        return [_normal_dist_inv_cdf(random.random(), self._mu, self._sigma) for _ in range(n)]

    def pdf(self, x: float) -> float:
        """The probability density at x."""
        variance = self._sigma * self._sigma
        if not variance:
            raise StatisticsError("pdf() not defined when sigma is zero")
        diff = x - self._mu
        return math.exp(diff * diff / (-2.0 * variance)) / math.sqrt(math.tau * variance)

    def cdf(self, x: float) -> float:
        """P(X <= x)."""
        if not self._sigma:
            raise StatisticsError("cdf() not defined when sigma is zero")
        return 0.5 * math.erfc((self._mu - x) / (self._sigma * _SQRT2))

    def inv_cdf(self, p: float) -> float:
        """The x with P(X <= x) = p."""
        if p <= 0.0 or p >= 1.0:
            raise StatisticsError("p must be in the range 0.0 < p < 1.0")
        return _normal_dist_inv_cdf(p, self._mu, self._sigma)

    def quantiles(self, n: int = 4) -> list[float]:
        """n - 1 cut points dividing the distribution into n equally likely intervals."""
        return [self.inv_cdf(i / n) for i in range(1, n)]

    def overlap(self, other: "NormalDist") -> float:
        """The area the two probability densities share (0.0 .. 1.0)."""
        X = self
        Y = other
        if (Y._sigma, Y._mu) < (X._sigma, X._mu):
            X = other
            Y = self
        X_var = X.variance
        Y_var = Y.variance
        if not X_var or not Y_var:
            raise StatisticsError("overlap() not defined when sigma is zero")
        dv = Y_var - X_var
        dm = abs(Y._mu - X._mu)
        if not dv:
            return math.erfc(dm / (2.0 * X._sigma * _SQRT2))
        a = X._mu * Y_var - Y._mu * X_var
        b = X._sigma * Y._sigma * math.sqrt(dm * dm + dv * math.log(Y_var / X_var))
        x1 = (a + b) / dv
        x2 = (a - b) / dv
        return 1.0 - (abs(Y.cdf(x1) - X.cdf(x1)) + abs(Y.cdf(x2) - X.cdf(x2)))

    def zscore(self, x: float) -> float:
        """(x - mean) / stdev."""
        if not self._sigma:
            raise StatisticsError("zscore() not defined when sigma is zero")
        return (x - self._mu) / self._sigma

    @property
    def mean(self) -> float:
        return self._mu

    @property
    def median(self) -> float:
        return self._mu

    @property
    def mode(self) -> float:
        return self._mu

    @property
    def stdev(self) -> float:
        return self._sigma

    @property
    def variance(self) -> float:
        return self._sigma * self._sigma

    def __add__(self, other: _T) -> "NormalDist":
        if isinstance(other, NormalDist):
            return NormalDist(self._mu + other._mu, math.hypot(self._sigma, other._sigma))
        else:
            return NormalDist(self._mu + other, self._sigma)

    def __radd__(self, other: _T) -> "NormalDist":
        return self + other

    def __sub__(self, other: _T) -> "NormalDist":
        if isinstance(other, NormalDist):
            return NormalDist(self._mu - other._mu, math.hypot(self._sigma, other._sigma))
        else:
            return NormalDist(self._mu - other, self._sigma)

    def __rsub__(self, other: _T) -> "NormalDist":
        return -(self - other)

    def __mul__(self, other: _T) -> "NormalDist":
        return NormalDist(self._mu * other, self._sigma * abs(other))

    def __rmul__(self, other: _T) -> "NormalDist":
        return self * other

    def __truediv__(self, other: _T) -> "NormalDist":
        return NormalDist(self._mu / other, self._sigma / abs(other))

    def __pos__(self) -> "NormalDist":
        return NormalDist(self._mu, self._sigma)

    def __neg__(self) -> "NormalDist":
        return NormalDist(-self._mu, self._sigma)

    def __eq__(self, other: _T) -> bool:
        if isinstance(other, NormalDist):
            return self._mu == other._mu and self._sigma == other._sigma
        else:
            return False

    def __hash__(self) -> int:
        return hash((self._mu, self._sigma))

    def __repr__(self) -> str:
        return "NormalDist(mu=" + repr(self._mu) + ", sigma=" + repr(self._sigma) + ")"


if not sys._compiled:
    from fractions import Fraction

    def _exact_div(num, den):
        """num / den: an int when exact, else a float (CPython's _convert to int)."""
        if num % den == 0:
            return num // den
        return num / den

    def _kind(xs):
        """int, Fraction or float: what results are made of."""
        k = int
        for x in xs:
            if isinstance(x, bool) or isinstance(x, int):
                continue
            if isinstance(x, Fraction):
                if k is int:
                    k = Fraction
                continue
            if isinstance(x, float):
                k = float
                continue
            raise TypeError("can't convert type '" + type(x).__name__ + "' to numerator/denominator")
        return k

    def _exact_sum(xs):
        """The exact sum of the numbers as a Fraction (None if 64-bit ints cannot hold it)."""
        try:
            return sum([Fraction(x) for x in xs], Fraction(0))
        except OverflowError:
            return None

    def _mean_dyn(xs):
        k = _kind(xs)
        n = len(xs)
        if k is float:
            t = _exact_sum(xs)
            if t is not None:
                try:
                    return float(t / n)
                except OverflowError:
                    pass
            return math.fsum(xs) / n
        if k is Fraction:
            return sum([Fraction(x) for x in xs], Fraction(0)) / n
        return _exact_div(sum(xs), n)

    def _harmonic_dyn(xs, weights):
        errmsg = "harmonic mean does not support negative values"
        n = len(xs)
        if n == 1 and weights is None:
            x = xs[0]
            if x < 0:
                raise StatisticsError(errmsg)
            return x
        if weights is None:
            ws = [1] * n
            sum_weights = n
        else:
            ws = list(weights)
            if len(ws) != n:
                raise StatisticsError("Number of weights does not match data size")
            for w in ws:
                if w < 0:
                    raise StatisticsError(errmsg)
            sum_weights = sum(ws)
        for x in xs:
            if x < 0:
                raise StatisticsError(errmsg)
        terms = []
        for w, x in zip(ws, xs):
            if w:
                if x == 0:
                    return 0
                terms.append(w / x)
            else:
                terms.append(0)
        k = _kind(terms)
        total = _exact_sum(terms)
        if total is None:
            total = math.fsum(terms)
        if total <= 0:
            raise StatisticsError("Weighted sum must be positive")
        r = sum_weights / total if isinstance(total, float) else Fraction(sum_weights) / total
        if k is float:
            return float(r)
        if isinstance(r, Fraction) and r.denominator == 1:
            return r.numerator
        return r

    def _var_dyn(xs, c, ddof):
        k = _kind(xs)
        n = len(xs)
        if k is float or (c is not None and isinstance(c, float)):
            return _exact_var_f([float(x) for x in xs], None if c is None else float(c), ddof)
        try:
            fx = [Fraction(x) for x in xs]
            m = sum(fx, Fraction(0)) / n if c is None else Fraction(c)
            ss = sum([(x - m) * (x - m) for x in fx], Fraction(0))
            if c is None:
                s = sum([x - m for x in fx], Fraction(0))
                ss -= s * s / n
            v = ss / ddof
        except OverflowError:
            return _ss_float([float(x) for x in xs], None if c is None else float(c)) / ddof
        if k is Fraction:
            return v
        if v.denominator == 1:
            return v.numerator
        return float(v)

    def _sqrt_dyn(v):
        if isinstance(v, Fraction):
            return math.sqrt(v.numerator / v.denominator)
        return math.sqrt(v)

    def mode(data):
        """The most common value (the first of them on a tie)."""
        xs = list(data)
        if not xs:
            raise StatisticsError("no mode for empty data")
        counts = {}
        for x in xs:
            counts[x] = counts.get(x, 0) + 1
        best = None
        bc = 0
        for k, c in counts.items():
            if c > bc:
                best = k
                bc = c
        return best

    def multimode(data):
        """The most common values, in the order they come first."""
        counts = {}
        for x in data:
            counts[x] = counts.get(x, 0) + 1
        if not counts:
            return []
        maxcount = max(counts.values())
        return [k for k, c in counts.items() if c == maxcount]

    from collections import namedtuple
    LinearRegression = namedtuple("LinearRegression", ("slope", "intercept"))
    _linear_regression_float = linear_regression

    def linear_regression(x, y, /, *, proportional=False):
        """The least-squares line through the points (through the origin if proportional)."""
        r = _linear_regression_float(x, y, proportional=proportional)
        return LinearRegression(slope=r.slope, intercept=r.intercept)
