# @property with @x.setter: obj.x = v and obj.x += v call the setter (compiled: a method __mpy_set_x)
class Temp:
    def __init__(self, c: float):
        self._c = c

    @property
    def celsius(self) -> float:
        return self._c

    @celsius.setter
    def celsius(self, v: float) -> None:
        if v < -273.15:
            raise ValueError("too cold")
        self._c = v

    @property
    def fahrenheit(self) -> float:
        return self._c * 9 / 5 + 32

    @fahrenheit.setter
    def fahrenheit(self, f: float) -> None:
        self.celsius = (f - 32) * 5 / 9


t = Temp(20.0)
print(t.celsius, t.fahrenheit)
t.celsius = 30.0
print(t.celsius, t.fahrenheit)
t.fahrenheit = 212.0
print(t.celsius)
t.celsius += 5.0
print(t.celsius)
try:
    t.celsius = -300.0
except ValueError as e:
    print("ValueError", e)
