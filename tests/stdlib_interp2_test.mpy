# the interpreter-only parts of the standard library (2): fractions powers with value-dependent result types, hashes
from fractions import Fraction as F
import math
print(F(3, 4), repr(F(6, -8)), F('3/4'), F(' -1.5e2 '), F('1_000/3'), F(0.5), F(1.1), F('.25'), F(F(1, 2), F(1, 3)), F(7))
print(F(1, 2) + F(1, 3), F(1, 2) - 1, 1 - F(1, 2), F(1, 2) * 3, F(1, 2) / F(3, 4), 2 / F(3), F(1, 2) + 0.25, 0.25 * F(1, 2))
print(F(7, 2) // 2, F(7, 2) % 2, divmod(F(7, 2), F(1, 3)), F(2, 3) ** 2, F(2, 3) ** -2, F(4, 9) ** F(1, 2), 2 ** F(3), 2 ** F(1, 2))
print(-F(1, 2), abs(F(-3, 4)), +F(1, 2), int(F(7, 2)), float(F(1, 3)), round(F(5, 2)), round(F(7, 2)), round(F(1, 3), 2), math.floor(F(-7, 2)), math.ceil(F(-7, 2)), math.trunc(F(-7, 2)))
print(F(1, 2) == 0.5, F(1, 2) < 0.6, F(1, 2) == F(2, 4), F(3, 1) == 3, F(1, 3) > F(1, 4), F(1, 2) != 1, bool(F(0)), hash(F(1, 2)) == hash(0.5), hash(F(3)) == hash(3), hash(F(1, 3)))
print(F(3.141592653589793).limit_denominator(1000), F(1, 3).as_integer_ratio(), F(4, 2).is_integer(), F(1, 3).numerator, F(1, 3).denominator, F.from_float(0.75), F(10, 4).limit_denominator(1))
print(sorted([F(1, 2), F(1, 3), F(2, 5), F(1)]), sum([F(1, 2), F(1, 3)], F(0)), max(F(1, 2), F(2, 3)), f'{F(1, 3):.3f}', format(F(1, 3), ''), str(F(-1, 3)))
for bad in ['abc', '1/0', '1.2.3', '']:
    try:
        print(F(bad))
    except ValueError as e:
        print("ValueError", e)
    except ZeroDivisionError as e:
        print('ZeroDivisionError', e)
try:
    F(1, 0)
except ZeroDivisionError as e:
    print('ZeroDivisionError', e)
try:
    F(1, 2) / 0
except ZeroDivisionError as e:
    print('ZeroDivisionError', e)

# statistics with ints and Fractions (exact results: ints when whole)
import statistics as S
from fractions import Fraction as F
print(S.mean([1, 2, 3]), S.mean([1, 2, 3, 4]), S.mean([1.1, 2.2, 3.3]), S.mean([F(1, 2), F(1, 3)]), S.mean([1, 2.5]), S.fmean([1, 2, 3, 4]), S.fmean([1, 2, 3], [1, 2, 3]))
print(S.median([3, 1, 2]), S.median([4, 1, 3, 2]), S.median_low([4, 1, 3, 2]), S.median_high([4, 1, 3, 2]), S.median_grouped([1, 2, 2, 3, 4, 4, 4, 4, 4, 5]), S.median([1.5, 2.5]))
print(S.mode([1, 1, 2, 3]), S.mode(['a', 'b', 'b']), S.mode([1, 2, 3]), S.multimode('aabbbbccddddeeffffgg'), S.multimode([]))
print(S.variance([1, 2, 3]), S.variance([1, 2, 3, 4]), S.variance([1.5, 2.5, 2.5, 2.75, 3.25, 4.75]), S.pvariance([1, 2, 3, 4]), S.stdev([1, 2, 3]), S.stdev([1, 2, 3, 4]), S.pstdev([1.5, 2.5, 2.5, 2.75, 3.25, 4.75]))
print(S.variance([2.75, 1.75, 1.25, 0.25, 0.5, 1.25, 3.5]), S.stdev([2.75, 1.75, 1.25, 0.25, 0.5, 1.25, 3.5]))
print(S.geometric_mean([54, 24, 36]), S.harmonic_mean([40, 60]), S.harmonic_mean([2.5, 3, 10]), S.harmonic_mean([40, 60], weights=[5, 30]))
print(S.quantiles([1, 2, 3, 4, 5, 6, 7, 8, 9, 10]), S.quantiles([1, 2, 3, 4, 5, 6, 7, 8, 9, 10], n=10), S.quantiles([1, 2, 3, 4, 5], method='inclusive'), S.quantiles([1.5, 2, 3.25], n=3))
x = [1, 2, 3, 4, 5, 6, 7, 8, 9]
y = [1, 2, 3, 1, 2, 3, 1, 2, 3]
print(S.covariance(x, y), S.correlation(x, x), S.correlation(x, y), S.linear_regression(x, [2 * v + 1 for v in x]), S.linear_regression([1, 2, 3], [2, 4, 6], proportional=True))
print(S.correlation([1, 2, 3, 4], [10, 20, 25, 100], method="ranked"), S.correlation([3, 1, 2], [1, 1, 2], method="ranked"))
n = S.NormalDist(100, 15)
print(n, n.pdf(100), n.cdf(115), n.inv_cdf(0.975), n.mean, n.stdev, n.variance, n.zscore(130), S.NormalDist.from_samples([2.5, 3.1, 2.1, 2.4, 2.7, 3.5]))
print(n + 5, n * 2, n.overlap(S.NormalDist(110, 15)), S.NormalDist(1, 2) + S.NormalDist(3, 4))
for f, d in [(S.mean, []), (S.median, []), (S.mode, []), (S.variance, [1]), (S.pvariance, []), (S.stdev, [5]), (S.geometric_mean, [])]:
    try:
        f(d)
    except S.StatisticsError as e:
        print(f.__name__, 'StatisticsError', e)
