# math functions compiled programs get from _mathx.py (fsum, prod, comb, erf, gamma ...)
# and `from math import ...` (values printed to 12 significant digits: libraries differ in the last bits)
import math
from math import sqrt, pi, fsum, floor


def g(v: float) -> str:
    return f"{v:.12g}"


print(sqrt(2), pi, floor(2.5), fsum([0.1] * 10), fsum([1e100, 1.0, -1e100, 1e-100]), math.fsum([1e-16, 1, 1e16]))
print(math.prod([1, 2, 3, 4]), math.prod([1.5, 2.0]), math.comb(10, 3), math.perm(5, 2), math.perm(4), math.factorial(10))
print(math.lcm(4, 6, 10), math.isclose(1.0, 1.0 + 1e-10), math.isclose(1.0, 1.1), math.isclose(0.0, 1e-12, abs_tol=1e-9))
print(math.dist([0, 0], [3, 4]), g(math.dist([1.5, 2.5, 3.5], [0.5, 0.5, 0.5])))
for x in [0.0, 0.1, 0.5, 1.0, 1.5, 2.5, 3.0, 5.0, -0.7, -2.2]:
    print(x, g(math.erf(x)), g(math.erfc(x)))
for x in [0.5, 1.0, 2.5, 5.0, 10.5, -0.5, -1.5, 30.0]:
    print(x, g(math.gamma(x)), g(math.lgamma(x)))
for x in [1e-10, 1e-5, 0.001, 0.5, 1.0, -0.5, 10.0]:
    print(x, g(math.expm1(x)), g(math.log1p(x)), g(math.sinh(x)), g(math.cosh(x)), g(math.tanh(x)), g(math.asinh(x)), g(math.atanh(x / 20)))
print(g(math.acosh(1.5)), g(math.acosh(10.0)), math.cbrt(27.0), math.cbrt(-8.0), g(math.cbrt(2.0)), math.exp2(3.0), g(math.exp2(0.5)))
print(math.modf(3.75), math.modf(-2.5), math.remainder(5.0, 3.0), math.remainder(7.0, 2.0), math.remainder(-5.5, 2.0))
for bad in [-1.0, 0.0]:
    try:
        print(math.gamma(bad))
    except ValueError as e:
        print("ValueError", e)
try:
    math.factorial(-1)
except ValueError as e:
    print("ValueError", e)
