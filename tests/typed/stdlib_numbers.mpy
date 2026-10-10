import numbers
x = 5
y = 2.5
b = True
s = "s"
print(isinstance(x, numbers.Integral), isinstance(y, numbers.Integral), isinstance(y, numbers.Real), isinstance(b, numbers.Number))
print(isinstance(s, numbers.Number), isinstance(x, (numbers.Real, str)), isinstance(y, numbers.Complex), isinstance(x, numbers.Rational))
if isinstance(y, numbers.Integral):
    print("int")
else:
    print("not int")


class Money(numbers.Number):
    def __init__(self, v: float) -> None:
        self.v = v


m = Money(1.5)
print(isinstance(m, numbers.Number), isinstance(m, numbers.Integral), issubclass(Money, numbers.Number))
