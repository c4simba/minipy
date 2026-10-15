# decimal: exact decimal arithmetic, contexts, signals, formatting (CPython's output)
import math
import decimal
from decimal import (Decimal, Context, getcontext, setcontext, localcontext, BasicContext, ExtendedContext,
                     DefaultContext, IEEEContext, ROUND_HALF_UP, ROUND_DOWN, ROUND_CEILING, ROUND_HALF_EVEN,
                     InvalidOperation, DivisionByZero, Inexact, Rounded, Overflow, FloatOperation, DecimalException)

# ---- construction and printing
print(Decimal("0.1") + Decimal("0.2"), Decimal("1.10") + Decimal("2.20"))
print(repr(Decimal("3.14")), repr(Decimal(-7)), repr(Decimal("1e-7")), repr(Decimal("-0")))
print(Decimal("1_000.000_1"), Decimal(" 42 "), Decimal("Inf"), Decimal("-nan7"), Decimal("sNaN"))
print(Decimal((1, (3, 1, 4), -2)), Decimal((0, (1,), 'F')), Decimal(Decimal("2.5")))
print(Decimal(0.1), Decimal("\u0661\u0662.\u0665"), Decimal("\uff17\uff10"))
print(Decimal.from_float(1.5), Decimal.from_float(-0.0), Decimal.from_float(1e100))
print(Decimal("123.456").as_tuple())
print(Decimal("-Infinity").as_tuple(), Decimal("NaN5").as_tuple())
print(Decimal("1.30").normalize(), Decimal("1200").normalize(), Decimal("0.000").normalize())
print(Decimal("123E+5").to_eng_string(), Decimal("1.5E-7").to_eng_string(), str(Decimal("0.0000001")))

# ---- arithmetic in the default context (28 digits)
print(Decimal(1) / Decimal(7))
print(Decimal(2).sqrt(), Decimal(10).ln(), Decimal(100).log10(), Decimal(1).exp())
print(Decimal("1.5") ** 2, Decimal(2) ** -2, Decimal(2) ** Decimal("0.5"), Decimal(16) ** Decimal("0.25"))
print(Decimal(7) // Decimal(2), Decimal(-7) // Decimal(2), Decimal(7) % Decimal(-2), divmod(Decimal(17), Decimal(5)))
print(Decimal("1.1") * 3, 3 * Decimal("1.1"), 10 - Decimal("0.01"), Decimal(1) / 8, 1 / Decimal(8))
print(-Decimal("5.5"), +Decimal("5.55555555555555555555555555555"), abs(Decimal("-2.5")))
print(pow(Decimal(3), 100, 7), Decimal(2).fma(3, 4), Decimal(10).remainder_near(Decimal(6)))
print(Decimal("2.5").quantize(Decimal("1")), Decimal("3.5").quantize(Decimal("1")),
      Decimal("7.325").quantize(Decimal(".01"), rounding=ROUND_HALF_UP), Decimal("7.325").quantize(Decimal(".01")))
print(Decimal("1.2345").quantize(Decimal("0.01"), rounding=ROUND_DOWN), Decimal("-1.2").to_integral_value(rounding=ROUND_CEILING))
print(Decimal("12.345").scaleb(2), Decimal("123456").rotate(2), Decimal("123456").shift(-2), Decimal("12.3").logb())
print(Decimal("1010").logical_and(Decimal("1100")), Decimal("1010").logical_xor(Decimal("110")))
print(Decimal("1.5").max(Decimal("NaN")), Decimal(-3).max_mag(Decimal(2)), Decimal(1).next_plus(), Decimal(1).next_minus())
print(Decimal(3).compare(Decimal(4)), Decimal("2.0").compare_total(Decimal("2")), Decimal("1.0").same_quantum(Decimal("2.5")))
print(Decimal("-0").number_class(), Decimal("1e-999999").number_class(), Decimal("sNaN").number_class(), Decimal(5).adjusted())

# ---- comparisons, hashing, conversions
print(Decimal("1.0") == Decimal(1), Decimal(1) == 1, Decimal("0.5") < 1, 2 > Decimal("1.99"), Decimal("NaN") == Decimal("NaN"))
print(sorted([Decimal("3.1"), Decimal("-2"), Decimal("10"), Decimal("0.5")]), max([Decimal(1), Decimal("1.5")]))
print(hash(Decimal("1.5")) == hash(Decimal("1.50")), hash(Decimal(7)), hash(Decimal("0.1")))
prices = {Decimal("1.0"): "one"}
print(prices[Decimal("1.00")], Decimal("2") in prices)
print(int(Decimal("-7.9")), float(Decimal("2.5")), round(Decimal("2.5")), round(Decimal("3.5")), round(Decimal("2.675"), 2))
print(math.floor(Decimal("-1.5")), math.ceil(Decimal("-1.5")), math.trunc(Decimal("-1.5")), bool(Decimal("0.00")))
print(Decimal("0.75").as_integer_ratio(), Decimal("-2.5E3").as_integer_ratio())

# ---- formatting
d = Decimal("1234567.891")
print(format(d, ",.2f"), format(d, ".3e"), format(d, "_.1f"), format(d, "15.2f"), format(d, "<15.1f") + "|")
print(f"{Decimal('3.14159'):.3}", f"{Decimal('0.125'):.1%}", f"{Decimal('-0.001'):z.2f}", f"{Decimal('42'):+08.2f}")
print(format(Decimal("1E+5"), "f"), format(Decimal("NaN"), ">6"), format(Decimal("1.5E-10"), "g"), format(Decimal("12.5"), ".0f"))

# ---- contexts
print(getcontext())
c = Context(prec=5, rounding=ROUND_HALF_UP)
print(c.divide(Decimal(1), Decimal(3)), c.add(Decimal("1.23456"), Decimal("0.000044")), c.power(2, 30))
print(c.create_decimal("3.14159265"), c.sqrt(2), c.flags[Inexact], c.flags[Rounded], c.flags[DivisionByZero])
print(c)
c.clear_flags()
print(c.flags[Inexact], c.Etiny(), c.Etop())
with localcontext() as lc:
    lc.prec = 6
    print(Decimal(1) / Decimal(7), getcontext().prec)
print(Decimal(1) / Decimal(7), getcontext().prec)
with localcontext(prec=3, rounding=ROUND_DOWN) as lc2:
    print(Decimal("2.789") + 0, lc2.rounding)
getcontext().prec = 10
print(Decimal(2).sqrt())
setcontext(BasicContext)
print(getcontext().prec, getcontext().rounding, Decimal(2).sqrt())
setcontext(DefaultContext)
print(BasicContext)
print(ExtendedContext)
print(IEEEContext(64), IEEEContext(128).prec)

# ---- signals: flags and traps
try:
    Decimal(1) / Decimal(0)
except DivisionByZero as e:
    print("DivisionByZero:", e)
try:
    Decimal("abc")
except InvalidOperation as e:
    print("InvalidOperation:", e, isinstance(e, DecimalException), isinstance(e, ArithmeticError))
try:
    Decimal(0) / Decimal(0)
except InvalidOperation as e:
    print("InvalidOperation:", e)
try:
    Decimal("-1").sqrt()
except decimal.InvalidOperation as e:
    print("sqrt:", e)
quiet = Context(prec=9, traps=[])
print(quiet.divide(Decimal(1), Decimal(0)), quiet.divide(Decimal(0), Decimal(0)), quiet.sqrt(Decimal(-4)))
print(quiet.flags[DivisionByZero], quiet.flags[InvalidOperation])
print(quiet)
strict = Context(prec=3, traps=[Inexact, Overflow])
try:
    strict.divide(Decimal(1), Decimal(3))
except Inexact as e:
    print("Inexact:", e, strict.flags[Rounded])
small = Context(prec=3, Emax=5, traps=[Overflow])
try:
    small.multiply(Decimal(999), Decimal(999))
except Overflow as e:
    print("Overflow:", e)
small.traps[Overflow] = False
print(small.multiply(Decimal(9999), Decimal(999)), small.flags[Overflow])
try:
    Decimal(1) + 1.5
except TypeError as e:
    print("TypeError:", e)
try:
    Decimal(1) < "a"
except TypeError as e:
    print("TypeError:", e)
print(Decimal("0.5") == 0.5, getcontext().flags[FloatOperation])
try:
    int(Decimal("NaN"))
except ValueError as e:
    print("ValueError:", e)
try:
    int(Decimal("-Inf"))
except OverflowError as e:
    print("OverflowError:", e)

# ---- big numbers (coefficients beyond 64 bits)
getcontext().prec = 50
big = Decimal(2) ** 200
print(big)
print(big / Decimal(3) ** 50)
print(Decimal("123456789012345678901234567890") * Decimal("987654321098765432109876543210"))
print(Decimal(1).exp(), Decimal(2).ln())
print(sum([Decimal("0.1")] * 10, Decimal(0)), Decimal(10) ** 30 + 1)
getcontext().prec = 28
