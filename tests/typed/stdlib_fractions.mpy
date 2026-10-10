# fractions: construction (ints, strs, floats), exact arithmetic, comparisons, rounding, limit_denominator
from fractions import Fraction as F
import math
print(F(3, 4), repr(F(6, -8)), F('3/4'), F(' -1.5e2 '), F('1_000/3'), F(0.5), F(1.1), F('.25'), F(F(1, 2), F(1, 3)), F(7))
print(F(1, 2) + F(1, 3), F(1, 2) - 1, 1 - F(1, 2), F(1, 2) * 3, F(1, 2) / F(3, 4), 2 / F(3), F(1, 2) + 0.25, 0.25 * F(1, 2))
print(F(7, 2) // 2, F(7, 2) % 2, divmod(F(7, 2), F(1, 3)), F(2, 3) ** 2, F(2, 3) ** -2)
print(-F(1, 2), abs(F(-3, 4)), +F(1, 2), int(F(7, 2)), float(F(1, 3)), round(F(5, 2)), round(F(7, 2)), round(F(1, 3), 2), math.floor(F(-7, 2)), math.ceil(F(-7, 2)), math.trunc(F(-7, 2)))
print(F(1, 2) == 0.5, F(1, 2) < 0.6, F(1, 2) == F(2, 4), F(3, 1) == 3, F(1, 3) > F(1, 4), F(1, 2) != 1, bool(F(0)), hash(F(1, 2)) == hash(F(2, 4)))
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
