# complex numbers: arithmetic, mixing with int/float, formatting, cmath
import cmath
import math

a = 1 + 2j
b = complex(3, -4)
print(a, b, a + b, a - b, a * b, a / b, -a, +a, abs(b))
print(a.real, a.imag, a.conjugate(), a == complex(1, 2), a != b, bool(0j), bool(a))
print(2j, 1j * 1j, complex(1, 0), complex(0.5, 0), (1 + 0j) == 1, complex(2))
print(repr(complex(-0.0, 1)), complex(1e20, 1e-5), a ** 2, a ** 0, 2 * a, a + 1.5, 3 / a, 2 - a)
print(complex("1+2j"), complex("-3.5j"), complex(" 4 "), [a, b], {"k": a}, f"{a}", str(b))
print(hash(1 + 0j) == hash(1), sum([1j, 2j, 3]), abs(3 + 4j), (1 + 1j).real.is_integer())


def rotate(z: complex, turns: int) -> complex:
    for _ in range(turns):
        z = z * 1j
    return z


print(rotate(1 + 0j, 1), rotate(2 + 1j, 2), rotate(a, 4) == a)
zs = [complex(i, -i) for i in range(4)]
print(zs, sum(zs), max(abs(z) for z in zs))
print(cmath.sqrt(-1), cmath.sqrt(3 + 4j), cmath.phase(1j), cmath.phase(-1 + 0j))
print(cmath.polar(1 + 1j), cmath.rect(1, 0), abs(cmath.exp(1j * cmath.pi) + 1) < 1e-12)
print(cmath.isnan(complex(math.nan, 0)), cmath.isinf(complex(0, math.inf)), cmath.isfinite(a))
try:
    print(a / 0j)
except ZeroDivisionError as e:
    print("ZeroDivisionError:", e)
try:
    complex("1+")
except ValueError as e:
    print("ValueError:", e)
