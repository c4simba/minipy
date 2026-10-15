from enum import Enum, IntEnum, StrEnum, Flag, IntFlag, auto, unique


class Color(Enum):
    RED = 1
    GREEN = 2
    BLUE = 3
    CRIMSON = 1

    def describe(self) -> str:
        return self.name.lower() + "=" + str(self.value)


print(Color.RED, repr(Color.GREEN), Color.RED.name, Color.RED.value, Color.CRIMSON is Color.RED)
print(Color(2), Color["BLUE"], [c.name for c in Color], len(Color), Color.RED in Color)
print(Color.RED.describe(), isinstance(Color.RED, Color), isinstance(Color.RED, Enum), type(Color.RED).__name__)
print(list(Color.__members__), Color.RED == Color.RED, Color.RED == Color.GREEN, Color.RED is not Color.BLUE)
try:
    Color(9)
except ValueError as e:
    print("ValueError:", e)
try:
    Color["PINK"]
except KeyError as e:
    print("KeyError:", e)
d = {Color.RED: "r", Color.GREEN: "g"}
print(d[Color.CRIMSON], sorted(c.value for c in Color), f"{Color.BLUE}")


class Num(IntEnum):
    ONE = auto()
    TWO = auto()
    TEN = 10
    ELEVEN = auto()


print(Num.ONE, repr(Num.TWO), int(Num.TEN), Num.ELEVEN.value, Num(2), [n.value for n in Num])


class Mode(StrEnum):
    READ = auto()
    WRITE = "w"


print(Mode.READ, repr(Mode.WRITE), Mode("w") is Mode.WRITE, Mode.READ.value)


class Perm(Flag):
    R = 4
    W = 2
    X = 1
    RW = R | W


print(Perm.R | Perm.W, repr(Perm.R | Perm.X), Perm.RW, Perm.W in Perm.RW, Perm.X in Perm.RW)
print(repr(~Perm.R), repr(Perm.R & Perm.W), bool(Perm.R & Perm.W), [p.name for p in Perm], len(Perm.R | Perm.W))
print(list(Perm.RW), Perm(6) is Perm.RW, repr(Perm(5)))


class Bits(IntFlag):
    A = auto()
    B = auto()
    C = auto()


print(Bits.A | Bits.C, repr(Bits.B | Bits.C), Bits.C.value, repr(Bits(9)))


class Planet(Enum):
    MERCURY = (3.303e+23, 2.4397e6)
    EARTH = (5.976e+24, 6.37814e6)

    def __init__(self, mass: float, radius: float) -> None:
        self.mass = mass
        self.radius = radius

    def gravity(self) -> float:
        return 6.673e-11 * self.mass / (self.radius * self.radius)


print(Planet.EARTH.mass, round(Planet.EARTH.gravity(), 2), Planet.MERCURY.value)


def pick(c: Color) -> str:
    match c:
        case Color.RED:
            return "stop"
        case Color.GREEN:
            return "go"
        case _:
            return "?"


print(pick(Color.RED), pick(Color.BLUE), pick(Color(2)))


class Level(Enum):
    LOW = 1
    HIGH = 2

    @classmethod
    def _missing_(cls, value: int) -> "Level":
        return cls.LOW if value < 2 else cls.HIGH

    @property
    def doubled(self) -> int:
        return self.value * 2


print(Level(0), Level(7), Level.HIGH.doubled, Level.LOW.value + 10)
counts: dict[Color, int] = {}
for c in [Color.RED, Color.BLUE, Color.RED]:
    counts[c] = counts.get(c, 0) + 1
print(sorted((k.name, v) for k, v in counts.items()), max(Color, key=lambda c: c.value))


@unique
class Weekday(IntEnum):
    MON = 1
    TUE = 2


print([d.name for d in reversed(Weekday)], Weekday.TUE > Weekday.MON if False else Weekday.TUE.value > Weekday.MON.value)
