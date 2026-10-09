# beyond 64 bits: OverflowError, the same in compiled code and in the interpreter
# (CPython's ints grow instead; a deliberate difference)
def tries(what: str, a: int, b: int) -> None:
    try:
        if what == "+":
            print(a + b)
        elif what == "-":
            print(a - b)
        elif what == "*":
            print(a * b)
        elif what == "**":
            print(a ** b)
        elif what == "<<":
            print(a << b)
        elif what == "//":
            print(a // b)
        else:
            print(-a)
    except OverflowError as e:
        print(what, "OverflowError:", e)

m = 9223372036854775807
tries("+", m, 1)
tries("-", -m, 2)
tries("*", 3037000500, 3037000500)
tries("**", 2, 63)
tries("**", 10, 19)
tries("<<", 1, 63)
tries("<<", 3, 62)
tries("//", -m - 1, -1)
tries("neg", -m - 1, 0)
print(m + 0, -m - 1, 2 ** 62 * 2 - 1 + 2 ** 62 * 2 if False else 1)
for s in ["9223372036854775807", "-9223372036854775808", "9223372036854775808", "123x"]:
    try:
        print(int(s))
    except OverflowError as e:
        print("OverflowError:", e)
    except ValueError as e:
        print("ValueError:", e)
for x in [1e18, 9.3e18, -9.2e18, float("inf"), float("nan")]:
    try:
        print(int(x))
    except OverflowError as e:
        print("OverflowError:", e)
    except ValueError as e:
        print("ValueError:", e)
try:
    print(1 << -1)
except ValueError as e:
    print("ValueError:", e)
print(2 ** -1, 10 ** -3)      # a negative literal exponent: a float (a variable one must not be negative)
