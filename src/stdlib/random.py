"""Random variable generators (CPython's random): the same Mersenne Twister and the
same algorithms, so a seeded generator gives CPython's numbers.

    integers: randrange, randint, getrandbits, randbytes
    sequences: choice, choices, shuffle, sample
    real-valued: random, uniform, triangular, normalvariate, gauss, lognormvariate,
                 expovariate, vonmisesvariate, gammavariate, betavariate,
                 paretovariate, weibullvariate, binomialvariate

ints are 64-bit here: getrandbits(k) takes k < 64."""
import sys
import math
from typing import TypeVar, Sequence

T = TypeVar("T")
S = TypeVar("S")

__all__ = ["Random", "SystemRandom", "betavariate", "binomialvariate", "choice", "choices", "expovariate",
           "gammavariate", "gauss", "getrandbits", "getstate", "lognormvariate", "normalvariate",
           "paretovariate", "randbytes", "randint", "random", "randrange", "sample", "seed", "setstate",
           "shuffle", "triangular", "uniform", "vonmisesvariate", "weibullvariate"]

NV_MAGICCONST = 1.7155277699214135     # 4 * exp(-0.5) / sqrt(2.0)
TWOPI = 6.283185307179586
LOG4 = 1.3862943611198906              # log(4.0)
SG_MAGICCONST = 2.504077396776274      # 1.0 + log(4.5)
BPF = 53                               # bits in a float
RECIP_BPF = 1.1102230246251565e-16     # 2 ** -BPF

_N = 624
_M = 397


def _words_of(hi: int, lo: int) -> list[int]:
    """The 32-bit words of the unsigned number hi:lo, least significant first (no high zero word)."""
    if hi:
        return [lo, hi]
    return [lo]


def _entropy() -> list[int]:
    """Seed words from the system's source (else the clocks)."""
    words: list[int] = []
    try:
        import _os
        b = _os.urandom(32)
        for i in range(0, 32, 4):
            words.append(b[i] | (b[i + 1] << 8) | (b[i + 2] << 16) | (b[i + 3] << 24))
    except OSError:
        import time
        t = time.time_ns()
        words = [t & 0xFFFFFFFF, (t >> 32) & 0xFFFFFFFF]
    return words


class Random:
    """Random number generator base class used by the module functions: its own
    state (a Mersenne Twister), so instances don't share it."""

    VERSION = 3     # used by getstate/setstate

    def __init__(self, x: S = None) -> None:
        self._mt: list[int] = [0] * _N
        self._index: int = _N
        self.gauss_next: float | None = None
        self.seed(x)
        self.gauss_next = None

    # ---- the generator (CPython's _randommodule.c)

    def _init_by_array(self, key: list[int]) -> None:
        mt = self._mt
        mt[0] = 19650218
        for i in range(1, _N):
            mt[i] = (1812433253 * (mt[i - 1] ^ (mt[i - 1] >> 30)) + i) & 0xFFFFFFFF
        i = 1
        j = 0
        n = len(key)
        k = _N if _N > n else n
        while k:
            mt[i] = ((mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1664525)) + key[j] + j) & 0xFFFFFFFF
            i += 1
            j += 1
            if i >= _N:
                mt[0] = mt[_N - 1]
                i = 1
            if j >= n:
                j = 0
            k -= 1
        k = _N - 1
        while k:
            mt[i] = ((mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1566083941)) - i) & 0xFFFFFFFF
            i += 1
            if i >= _N:
                mt[0] = mt[_N - 1]
                i = 1
            k -= 1
        mt[0] = 0x80000000
        self._index = _N

    def _genrand(self) -> int:
        """The next 32 random bits."""
        mt = self._mt
        if self._index >= _N:
            for kk in range(_N - _M):
                y = (mt[kk] & 0x80000000) | (mt[kk + 1] & 0x7FFFFFFF)
                mt[kk] = mt[kk + _M] ^ (y >> 1) ^ (0x9908B0DF if y & 1 else 0)
            for kk in range(_N - _M, _N - 1):
                y = (mt[kk] & 0x80000000) | (mt[kk + 1] & 0x7FFFFFFF)
                mt[kk] = mt[kk + (_M - _N)] ^ (y >> 1) ^ (0x9908B0DF if y & 1 else 0)
            y = (mt[_N - 1] & 0x80000000) | (mt[0] & 0x7FFFFFFF)
            mt[_N - 1] = mt[_M - 1] ^ (y >> 1) ^ (0x9908B0DF if y & 1 else 0)
            self._index = 0
        y = mt[self._index]
        self._index += 1
        y ^= y >> 11
        y ^= (y << 7) & 0x9D2C5680
        y ^= (y << 15) & 0xEFC60000
        y ^= y >> 18
        return y

    def random(self) -> float:
        """The next random floating-point number in the range 0.0 <= X < 1.0."""
        a = self._genrand() >> 5
        b = self._genrand() >> 6
        return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0)

    def getrandbits(self, k: int) -> int:
        """An int with k random bits."""
        if k < 0:
            raise ValueError("Cannot convert negative int")
        if k == 0:
            return 0
        if k <= 32:
            return self._genrand() >> (32 - k)
        if k > 63:
            raise OverflowError("integer overflow (ints are 64-bit)")
        lo = self._genrand()
        return lo | ((self._genrand() >> (64 - k)) << 32)

    # ---- seeding and state

    def seed(self, a: S = None, version: int = 2) -> None:
        """Initializes the state from a seed: None (the system's randomness), an int,
        a float, a str, bytes or a bytearray."""
        if a is None:
            self._init_by_array(_entropy())
        elif isinstance(a, int):
            n = -a if a < 0 else a
            self._init_by_array(_words_of(n >> 32, n & 0xFFFFFFFF))
        elif isinstance(a, str):
            self._seed_data(a.encode(), a, version)
        elif isinstance(a, bytes):
            self._seed_data(a, a.decode("latin-1") if version == 1 else "", version)
        elif isinstance(a, float):
            self._seed_hash(hash(a))
        else:
            if not sys._compiled:
                if type(a).__name__ == "bytearray":
                    if version == 1:
                        self._seed_hash(hash(a))          # (CPython: unhashable)
                    self._seed_data(bytes(a), "", version)
                    self.gauss_next = None
                    return
            raise TypeError("The only supported seed types are:\nNone, int, float, str, bytes, and bytearray.")
        self.gauss_next = None

    def _seed_hash(self, h: int) -> None:
        self._init_by_array(_words_of((h >> 32) & 0xFFFFFFFF, h & 0xFFFFFFFF))   # (the hash as unsigned)

    def _seed_data(self, data: bytes, text: str, version: int) -> None:
        if version == 1:                                  # (older Pythons' sequences)
            hi = 0
            lo = ord(text[0]) << 7 if text else 0
            for ch in text:
                p = lo * 1000003                          # x = (1000003 * x ^ c) mod 2**64, in halves
                hi = (hi * 1000003 + (p >> 32)) & 0xFFFFFFFF
                lo = (p & 0xFFFFFFFF) ^ ord(ch)
            lo ^= len(text) & 0xFFFFFFFF
            hi ^= len(text) >> 32
            self._init_by_array(_words_of(hi, lo))
            return
        import hashlib
        b = data + hashlib.sha512(data).digest()          # int.from_bytes(a + sha512(a).digest())
        words: list[int] = []
        i = len(b)
        while i > 0:
            j = i - 4 if i >= 4 else 0
            w = 0
            for x in range(j, i):
                w = (w << 8) | b[x]
            words.append(w)
            i = j
        while len(words) > 1 and words[-1] == 0:
            words.pop()
        self._init_by_array(words)

    def getstate(self) -> tuple[int, tuple[int, ...], float | None]:
        """The internal state; setstate() restores it."""
        return self.VERSION, tuple(self._mt + [self._index]), self.gauss_next

    def setstate(self, state: tuple[int, tuple[int, ...], float | None]) -> None:
        """Restores the internal state from what getstate() returned."""
        version = state[0]
        if version != 3 and version != 2:
            raise ValueError("state with version " + str(version) + " passed to Random.setstate() of version " +
                             str(self.VERSION))
        internal = state[1]
        if len(internal) != _N + 1:
            raise ValueError("state vector is the wrong size")
        index = internal[_N]
        if index < 0 or index > _N:
            raise ValueError("invalid state")
        for i in range(_N):
            self._mt[i] = internal[i] % 4294967296 if version == 2 else internal[i]
        self._index = index
        self.gauss_next = state[2]

    # ---- integers

    def _randbelow(self, n: int) -> int:
        """A random int in the range [0, n) (n > 0)."""
        k = n.bit_length()
        r = self.getrandbits(k)
        while r >= n:
            r = self.getrandbits(k)
        return r

    def randbytes(self, n: int) -> bytes:
        """n random bytes."""
        if n < 0:
            raise ValueError("Cannot convert negative int")
        out: list[int] = []
        k = n * 8
        while k > 0:
            r = self._genrand()
            if k < 32:
                r >>= 32 - k
            for _ in range(4 if k >= 32 else k // 8):
                out.append(r & 255)
                r >>= 8
            k -= 32
        return bytes(out)

    def randrange(self, start: int, stop: int | None = None, step: int = 1) -> int:
        """A random item from range(start, stop[, step])."""
        if stop is None:
            if step != 1:
                raise TypeError("Missing a non-None stop argument")
            if start > 0:
                return self._randbelow(start)
            raise ValueError("empty range for randrange()")
        width = stop - start
        if step == 1:
            if width > 0:
                return start + self._randbelow(width)
            raise ValueError(f"empty range in randrange({start}, {stop})")
        if step > 0:
            n = (width + step - 1) // step
        elif step < 0:
            n = (width + step + 1) // step
        else:
            raise ValueError("zero step for randrange()")
        if n <= 0:
            raise ValueError(f"empty range in randrange({start}, {stop}, {step})")
        return start + step * self._randbelow(n)

    def randint(self, a: int, b: int) -> int:
        """A random integer in range [a, b], including both end points."""
        if b < a:
            raise ValueError(f"empty range in randint({a}, {b})")
        return a + self._randbelow(b - a + 1)

    # ---- sequences

    def choice(self, seq: Sequence[T]) -> T:
        """A random element from a non-empty sequence."""
        if not len(seq):
            raise IndexError("Cannot choose from an empty sequence")
        return seq[self._randbelow(len(seq))]

    def shuffle(self, x: list[T]) -> None:
        """Shuffles list x in place."""
        for i in reversed(range(1, len(x))):
            j = self._randbelow(i + 1)
            x[i], x[j] = x[j], x[i]

    def sample(self, population: Sequence[T], k: int, *, counts: list[int] | None = None) -> list[T]:
        """k unique random elements from a population sequence, in selection order."""
        if not sys._compiled:
            if isinstance(population, (set, frozenset, dict)) or not hasattr(population, "__getitem__"):
                raise TypeError("Population must be a sequence.  For dicts or sets, use sorted(d).")
        n = len(population)
        if counts is not None:
            cum_counts: list[int] = []
            total = 0
            for c in counts:
                total += c
                cum_counts.append(total)
            if len(cum_counts) != n:
                raise ValueError("The number of counts does not match the population")
            if cum_counts:
                cum_counts.pop()
            if total < 0:
                raise ValueError("Counts must be non-negative")
            picked: list[T] = []
            for s in self.sample(list(range(total)), k):
                lo = 0
                hi = len(cum_counts)
                while lo < hi:
                    mid = (lo + hi) // 2
                    if s < cum_counts[mid]:
                        hi = mid
                    else:
                        lo = mid + 1
                picked.append(population[lo])
            return picked
        if not 0 <= k <= n:
            raise ValueError("Sample larger than population or is negative")
        result: list[T] = []
        setsize = 21        # size of a small set minus size of an empty list
        if k > 5:
            setsize += 4 ** math.ceil(math.log(k * 3, 4))   # table size for big sets
        if n <= setsize:
            pool = list(population)
            for i in range(k):
                j = self._randbelow(n - i)
                result.append(pool[j])
                pool[j] = pool[n - i - 1]
        else:
            selected: set[int] = set()
            for i in range(k):
                j = self._randbelow(n)
                while j in selected:
                    j = self._randbelow(n)
                selected.add(j)
                result.append(population[j])
        return result

    def choices(self, population: Sequence[T], weights: list[S] | None = None, *, cum_weights: list[S] | None = None,
                k: int = 1) -> list[T]:
        """k elements of population chosen with replacement (by relative or cumulative weights)."""
        n = len(population)
        cum: list[float] = []
        if cum_weights is None:
            if weights is None:
                nf = n + 0.0
                return [population[math.floor(self.random() * nf)] for i in range(k)]
            total = 0.0
            if not sys._compiled and isinstance(weights, int):
                raise TypeError(f"The number of choices must be a keyword argument: k={weights}")
            for w in weights:
                total += w
                cum.append(total)
        elif weights is not None:
            raise TypeError("Cannot specify both weights and cumulative weights")
        else:
            for w in cum_weights:
                cum.append(w + 0.0)
        if len(cum) != n:
            raise ValueError("The number of weights does not match the population")
        total = cum[-1] + 0.0
        if total <= 0.0:
            raise ValueError("Total of weights must be greater than zero")
        if not math.isfinite(total):
            raise ValueError("Total of weights must be finite")
        hi = n - 1
        out: list[T] = []
        for i in range(k):
            x = self.random() * total
            lo = 0
            h = hi
            while lo < h:                                 # bisect(cum, x, 0, hi)
                mid = (lo + h) // 2
                if x < cum[mid]:
                    h = mid
                else:
                    lo = mid + 1
            out.append(population[lo])
        return out

    # ---- real-valued distributions

    def uniform(self, a: float, b: float) -> float:
        """A random number in the range [a, b) or [a, b] depending on rounding."""
        return a + (b - a) * self.random()

    def triangular(self, low: float = 0.0, high: float = 1.0, mode: float | None = None) -> float:
        """Triangular distribution: bounded by low and high, with a peak at mode."""
        u = self.random()
        if mode is None:
            c = 0.5
        elif high == low:
            return low
        else:
            c = (mode - low) / (high - low)
        if u > c:
            u = 1.0 - u
            c = 1.0 - c
            low, high = high, low
        return low + (high - low) * math.sqrt(u * c)

    def normalvariate(self, mu: float = 0.0, sigma: float = 1.0) -> float:
        """Normal distribution (mu the mean, sigma the standard deviation)."""
        while True:
            u1 = self.random()
            u2 = 1.0 - self.random()
            z = NV_MAGICCONST * (u1 - 0.5) / u2
            zz = z * z / 4.0
            if zz <= -math.log(u2):
                break
        return mu + z * sigma

    def gauss(self, mu: float = 0.0, sigma: float = 1.0) -> float:
        """Gaussian distribution (mu the mean, sigma the standard deviation); faster than normalvariate."""
        z = self.gauss_next
        self.gauss_next = None
        if z is None:
            x2pi = self.random() * TWOPI
            g2rad = math.sqrt(-2.0 * math.log(1.0 - self.random()))
            z = math.cos(x2pi) * g2rad
            self.gauss_next = math.sin(x2pi) * g2rad
        return mu + z * sigma

    def lognormvariate(self, mu: float, sigma: float) -> float:
        """Log normal distribution: its natural logarithm is normal (mean mu, deviation sigma)."""
        return math.exp(self.normalvariate(mu, sigma))

    def expovariate(self, lambd: float = 1.0) -> float:
        """Exponential distribution; lambd is 1.0 divided by the desired mean."""
        return -math.log(1.0 - self.random()) / lambd

    def vonmisesvariate(self, mu: float, kappa: float) -> float:
        """Circular data distribution: mu the mean angle, kappa the concentration."""
        if kappa <= 1e-6:
            return TWOPI * self.random()
        s = 0.5 / kappa
        r = s + math.sqrt(1.0 + s * s)
        while True:
            u1 = self.random()
            z = math.cos(math.pi * u1)
            d = z / (r + z)
            u2 = self.random()
            if u2 < 1.0 - d * d or u2 <= (1.0 - d) * math.exp(d):
                break
        q = 1.0 / r
        f = (q + z) / (1.0 + q * z)
        u3 = self.random()
        if u3 > 0.5:
            theta = (mu + math.acos(f)) % TWOPI
        else:
            theta = (mu - math.acos(f)) % TWOPI
        return theta

    def gammavariate(self, alpha: float, beta: float) -> float:
        """Gamma distribution (not the gamma function!): alpha > 0, beta > 0."""
        if alpha <= 0.0 or beta <= 0.0:
            raise ValueError("gammavariate: alpha and beta must be > 0.0")
        if alpha > 1.0:
            ainv = math.sqrt(2.0 * alpha - 1.0)
            bbb = alpha - LOG4
            ccc = alpha + ainv
            while True:
                u1 = self.random()
                if not 1e-7 < u1 < 0.9999999:
                    continue
                u2 = 1.0 - self.random()
                v = math.log(u1 / (1.0 - u1)) / ainv
                x = alpha * math.exp(v)
                z = u1 * u1 * u2
                r = bbb + ccc * v - x
                if r + SG_MAGICCONST - 4.5 * z >= 0.0 or r >= math.log(z):
                    return x * beta
        elif alpha == 1.0:
            return -math.log(1.0 - self.random()) * beta
        while True:
            u = self.random()
            b = (math.e + alpha) / math.e
            p = b * u
            if p <= 1.0:
                x = p ** (1.0 / alpha)
            else:
                x = -math.log((b - p) / alpha)
            u1 = self.random()
            if p > 1.0:
                if u1 <= x ** (alpha - 1.0):
                    break
            elif u1 <= math.exp(-x):
                break
        return x * beta

    def betavariate(self, alpha: float, beta: float) -> float:
        """Beta distribution: alpha > 0, beta > 0; values between 0 and 1."""
        y = self.gammavariate(alpha, 1.0)
        if y:
            return y / (y + self.gammavariate(beta, 1.0))
        return 0.0

    def paretovariate(self, alpha: float) -> float:
        """Pareto distribution; alpha is the shape parameter."""
        u = 1.0 - self.random()
        return u ** (-1.0 / alpha)

    def weibullvariate(self, alpha: float, beta: float) -> float:
        """Weibull distribution: alpha the scale parameter, beta the shape parameter."""
        u = 1.0 - self.random()
        return alpha * (-math.log(u)) ** (1.0 / beta)

    def binomialvariate(self, n: int = 1, p: float = 0.5) -> int:
        """Binomial random variable: the successes of n independent trials of probability p."""
        if n < 0:
            raise ValueError("n must be non-negative")
        if p <= 0.0 or p >= 1.0:
            if p == 0.0:
                return 0
            if p == 1.0:
                return n
            raise ValueError("p must be in the range 0.0 <= p <= 1.0")
        if n == 1:
            return 1 if self.random() < p else 0
        if p > 0.5:
            return n - self.binomialvariate(n, 1.0 - p)
        if n * p < 10.0:
            x = 0
            y = 0
            c = math.log2(1.0 - p)
            if not c:
                return x
            while True:
                u = self.random()
                if u == 0.0:
                    continue                              # (log2(0.0): a ValueError)
                y += math.floor(math.log2(u) / c) + 1
                if y > n:
                    return x
                x += 1
        setup_complete = False
        spq = math.sqrt(n * p * (1.0 - p))               # standard deviation of the distribution
        b = 1.15 + 2.53 * spq
        a = -0.0873 + 0.0248 * b + 0.01 * p
        c = n * p + 0.5
        vr = 0.92 - 4.2 / b
        alpha = 0.0
        lpq = 0.0
        m = 0
        h = 0.0
        while True:
            u = self.random()
            u -= 0.5
            us = 0.5 - math.fabs(u)
            k = math.floor((2.0 * a / us + b) * u + c)
            if k < 0 or k > n:
                continue
            v = self.random()
            if us >= 0.07 and v <= vr:
                return k
            if not setup_complete:
                alpha = (2.83 + 5.1 / b) * spq
                lpq = math.log(p / (1.0 - p))
                m = math.floor((n + 1) * p)              # mode of the distribution
                h = math.lgamma(m + 1) + math.lgamma(n - m + 1)
                setup_complete = True
            v *= alpha / (a / (us * us) + b)
            if math.log(v) <= h - math.lgamma(k + 1) - math.lgamma(n - k + 1) + (k - m) * lpq:
                return k

    if not sys._compiled:
        def __init_subclass__(cls, /, **kwargs):
            """A subclass with random() but no getrandbits() draws integers from random()."""
            for c in cls.__mro__:
                if "_randbelow" in c.__dict__:
                    break
                if "getrandbits" in c.__dict__:
                    cls._randbelow = cls._randbelow_with_getrandbits
                    break
                if "random" in c.__dict__:
                    cls._randbelow = cls._randbelow_without_getrandbits
                    break

        def _randbelow_with_getrandbits(self, n):
            k = n.bit_length()
            r = self.getrandbits(k)
            while r >= n:
                r = self.getrandbits(k)
            return r

        def _randbelow_without_getrandbits(self, n, maxsize=1 << BPF):
            if n >= maxsize:
                return math.floor(self.random() * n)
            rem = maxsize % n
            limit = (maxsize - rem) / maxsize
            r = self.random()
            while r >= limit:
                r = self.random()
            return math.floor(r * maxsize) % n

        def __reduce__(self):
            return self.__class__, (), self.getstate()


class SystemRandom(Random):
    """Random numbers from the system's source (os.urandom); it has no state to seed or save."""

    def random(self) -> float:
        import _os
        b = _os.urandom(7)
        x = 0
        for v in b:
            x = (x << 8) | v
        return (x >> 3) * RECIP_BPF

    def getrandbits(self, k: int) -> int:
        if k < 0:
            raise ValueError("number of bits must be non-negative")
        if k > 63:
            raise OverflowError("integer overflow (ints are 64-bit)")
        import _os
        numbytes = (k + 7) // 8
        x = 0
        for v in _os.urandom(numbytes):
            x = (x << 8) | v
        return x >> (numbytes * 8 - k)

    def randbytes(self, n: int) -> bytes:
        import _os
        return _os.urandom(n)

    if not sys._compiled:                     # (compiled: its generator state is there, unused)
        def seed(self, a=None, version=2):
            """(Not used: the system's source has no state.)"""

    def getstate(self) -> tuple[int, tuple[int, ...], float | None]:
        raise NotImplementedError("System entropy source does not have state.")

    def setstate(self, state: tuple[int, tuple[int, ...], float | None]) -> None:
        raise NotImplementedError("System entropy source does not have state.")


_inst = Random()


def seed(a: S = None, version: int = 2) -> None:
    """Initializes the generator the module functions use (see Random.seed)."""
    _inst.seed(a, version)


def random() -> float:
    """The next random floating-point number in the range 0.0 <= X < 1.0."""
    return _inst.random()


def uniform(a: float, b: float) -> float:
    """A random number in the range [a, b) or [a, b] depending on rounding."""
    return _inst.uniform(a, b)


def triangular(low: float = 0.0, high: float = 1.0, mode: float | None = None) -> float:
    """Triangular distribution: bounded by low and high, with a peak at mode."""
    return _inst.triangular(low, high, mode)


def randint(a: int, b: int) -> int:
    """A random integer in range [a, b], including both end points."""
    return _inst.randint(a, b)


def choice(seq: Sequence[T]) -> T:
    """A random element from a non-empty sequence."""
    return _inst.choice(seq)


def randrange(start: int, stop: int | None = None, step: int = 1) -> int:
    """A random item from range(start, stop[, step])."""
    return _inst.randrange(start, stop, step)


def sample(population: Sequence[T], k: int, *, counts: list[int] | None = None) -> list[T]:
    """k unique random elements from a population sequence, in selection order."""
    return _inst.sample(population, k, counts=counts)


def shuffle(x: list[T]) -> None:
    """Shuffles list x in place."""
    _inst.shuffle(x)


def choices(population: Sequence[T], weights: list[S] | None = None, *, cum_weights: list[S] | None = None,
            k: int = 1) -> list[T]:
    """k elements of population chosen with replacement (by relative or cumulative weights)."""
    return _inst.choices(population, weights, cum_weights=cum_weights, k=k)


def normalvariate(mu: float = 0.0, sigma: float = 1.0) -> float:
    """Normal distribution (mu the mean, sigma the standard deviation)."""
    return _inst.normalvariate(mu, sigma)


def lognormvariate(mu: float, sigma: float) -> float:
    """Log normal distribution: its natural logarithm is normal (mean mu, deviation sigma)."""
    return _inst.lognormvariate(mu, sigma)


def expovariate(lambd: float = 1.0) -> float:
    """Exponential distribution; lambd is 1.0 divided by the desired mean."""
    return _inst.expovariate(lambd)


def vonmisesvariate(mu: float, kappa: float) -> float:
    """Circular data distribution: mu the mean angle, kappa the concentration."""
    return _inst.vonmisesvariate(mu, kappa)


def gammavariate(alpha: float, beta: float) -> float:
    """Gamma distribution (not the gamma function!): alpha > 0, beta > 0."""
    return _inst.gammavariate(alpha, beta)


def gauss(mu: float = 0.0, sigma: float = 1.0) -> float:
    """Gaussian distribution (mu the mean, sigma the standard deviation)."""
    return _inst.gauss(mu, sigma)


def betavariate(alpha: float, beta: float) -> float:
    """Beta distribution: alpha > 0, beta > 0; values between 0 and 1."""
    return _inst.betavariate(alpha, beta)


def binomialvariate(n: int = 1, p: float = 0.5) -> int:
    """Binomial random variable: the successes of n independent trials of probability p."""
    return _inst.binomialvariate(n, p)


def paretovariate(alpha: float) -> float:
    """Pareto distribution; alpha is the shape parameter."""
    return _inst.paretovariate(alpha)


def weibullvariate(alpha: float, beta: float) -> float:
    """Weibull distribution: alpha the scale parameter, beta the shape parameter."""
    return _inst.weibullvariate(alpha, beta)


def getstate() -> tuple[int, tuple[int, ...], float | None]:
    """The module generator's internal state; setstate() restores it."""
    return _inst.getstate()


def setstate(state: tuple[int, tuple[int, ...], float | None]) -> None:
    """Restores the module generator's state from what getstate() returned."""
    _inst.setstate(state)


def getrandbits(k: int) -> int:
    """An int with k random bits (k < 64)."""
    return _inst.getrandbits(k)


def randbytes(n: int) -> bytes:
    """n random bytes."""
    return _inst.randbytes(n)
