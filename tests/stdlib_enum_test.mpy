# enum (CPython's, ported): Enum, IntEnum, StrEnum, Flag, IntFlag, auto, unique, verify, boundaries, functional API
from enum import Enum, IntEnum, StrEnum, Flag, IntFlag, auto, unique
import enum


class Color(Enum):
    RED = 1
    GREEN = 2
    BLUE = 3
    CRIMSON = 1


print(Color.RED, repr(Color.RED), Color(2), Color["BLUE"], list(Color), len(Color), Color.CRIMSON is Color.RED, Color.RED.name, Color.RED.value)
print(Color.__members__, Color.RED in Color, 2 in Color, 5 in Color, type(Color.RED), isinstance(Color.GREEN, Color), Color)
print({Color.RED: "r"}[Color.CRIMSON], Color.RED == Color.RED, Color.RED != Color.BLUE, Color.RED == 1, hash(Color.RED) == hash("RED"))
print(f"{Color.RED}", format(Color.GREEN), str(Color.BLUE), [c.name for c in reversed(Color)])
try:
    Color(9)
except ValueError as e:
    print("ValueError", e)
try:
    Color.RED < Color.BLUE
except TypeError as e:
    print("TypeError", e)


class Shape(Enum):
    SQUARE = auto()
    CIRCLE = auto()
    TRI = auto()


print(list(Shape), Shape.CIRCLE.value)


class Planet(Enum):
    MERCURY = (3.303e+23, 2.4397e6)
    EARTH = (5.976e+24, 6.37814e6)

    def __init__(self, mass, radius):
        self.mass = mass
        self.radius = radius

    @property
    def surface_gravity(self):
        return 6.673e-11 * self.mass / (self.radius * self.radius)


print(Planet.EARTH.mass, round(Planet.EARTH.surface_gravity, 3), Planet.MERCURY.value)


class Num(IntEnum):
    ONE = 1
    TWO = 2
    THREE = 3


print(Num.ONE, repr(Num.TWO), Num.ONE + 1, Num.TWO * 3, Num.THREE == 3, Num.ONE < Num.TWO, sorted([Num.THREE, Num.ONE]), [10, 20, 30][Num.ONE], int(Num.TWO), f"{Num.THREE}", f"{Num.THREE:03d}", isinstance(Num.ONE, int), Num(2), str(Num.ONE), max(Num))


class Mood(StrEnum):
    HAPPY = auto()
    SAD = "sad"


print(Mood.HAPPY, repr(Mood.HAPPY), Mood.HAPPY == "happy", Mood("sad"), Mood.SAD.upper(), "-".join([Mood.HAPPY, Mood.SAD]), f"{Mood.SAD}", isinstance(Mood.SAD, str), Mood.HAPPY + "!")


class Perm(Flag):
    R = 4
    W = 2
    X = 1


rw = Perm.R | Perm.W
print(rw, repr(rw), Perm.R in rw, Perm.X in rw, list(rw), len(rw), bool(Perm(0)), ~Perm.R, Perm(7), Perm.R & Perm.W, rw ^ Perm.R, repr(Perm(0)))


class Open(IntFlag):
    RD = 1
    WR = 2
    CREATE = 4


o = Open.RD | Open.CREATE
print(o, repr(o), o & 4, o | 8, Open(3), o == 5, int(o), Open.WR in o, list(o), Open(16))


class Weekday(Flag):
    MON = auto()
    TUE = auto()
    WED = auto()
    WEEKDAYS = MON | TUE | WED


print(list(Weekday), Weekday.WEEKDAYS, repr(Weekday.MON | Weekday.WED), Weekday.WEEKDAYS.value)
try:
    @unique
    class Dup(Enum):
        A = 1
        B = 1
except ValueError as e:
    print("ValueError", e)
Animal = Enum("Animal", "ANT BEE CAT")
print(list(Animal), Animal.BEE.value, Animal["CAT"], repr(Animal))
Pets = Enum("Pets", [("DOG", 10), ("FISH", 20)])
print(Pets.FISH, Pets(10))


class Status(Enum):
    ACTIVE = "active"
    INACTIVE = "inactive"

    @classmethod
    def _missing_(cls, value):
        for m in cls:
            if m.value == value.lower():
                return m
        return None

    def describe(self):
        return self.name.lower() + "!"


print(Status("ACTIVE"), Status.INACTIVE.describe(), Status.ACTIVE.value)
try:
    Color.RED = 5
except AttributeError as e:
    print("AttributeError", e)
print(enum.EnumType, type(Color), Color.__name__, Color.RED.__class__.__name__)

import enum
from enum import verify, UNIQUE, CONTINUOUS, NAMED_FLAGS, member, nonmember, FlagBoundary, STRICT, CONFORM, EJECT, KEEP


class Ordinal(Enum):
    def _generate_next_value_(name, start, count, last_values):
        return name.lower() + str(count)
    A = auto()
    B = auto()


print(list(Ordinal), Ordinal.B.value)


class WithMembers(Enum):
    X = 1
    helper = nonmember(42)
    fn = member(lambda: 7)

    def double(self):
        return self.value * 2


print([m.name for m in WithMembers], WithMembers.helper, WithMembers.X.double(), WithMembers.fn.value())


class Ign(Enum):
    _ignore_ = "tmp"
    tmp = 5
    ONE = 1


print(list(Ign), hasattr(Ign, "tmp"))
try:
    @verify(UNIQUE)
    class V(Enum):
        A = 1
        B = 1
except ValueError as e:
    print("ValueError", e)
try:
    @verify(CONTINUOUS)
    class V2(IntEnum):
        A = 1
        C = 3
except ValueError as e:
    print("ValueError", e)


class Strict(Flag, boundary=STRICT):
    A = 1
    B = 2


class Conform(Flag, boundary=CONFORM):
    A = 1
    B = 2


class Eject(Flag, boundary=EJECT):
    A = 1
    B = 2


try:
    Strict(7)
except ValueError as e:
    print("ValueError", e)
print(repr(Conform(7)), repr(Eject(7)), repr(Eject(3)))


class Color(Flag):
    RED = auto()
    GREEN = auto()
    BLUE = auto()
    WHITE = RED | GREEN | BLUE
    BLACK = 0


print(list(Color), Color.WHITE, repr(Color.BLACK), Color.RED | Color.BLUE, list(Color.WHITE), Color(3), Color.WHITE.name, Color(6).name, len(Color.WHITE), ~Color.RED, ~Color.WHITE, Color.RED in Color.WHITE)
print(enum.show_flag_values(13), enum.bin(10), enum.bin(~10))


class Base(Enum):
    def describe(self):
        return f"{self.name}={self.value}"

    @classmethod
    def first(cls):
        return next(iter(cls))


class Sub(Base):
    P = "p"
    Q = "q"


print(Sub.P.describe(), Sub.first(), [m.describe() for m in Sub])
try:
    class Bad(Sub):
        R = "r"
except TypeError as e:
    print("TypeError", e)


class Http(IntEnum):
    OK = 200
    NOT_FOUND = 404

    @property
    def phrase(self):
        return self.name.replace("_", " ").title()


print(Http(404), Http.OK.phrase, Http.NOT_FOUND + 1, Http.OK == 200, {Http.OK: "x"}[200], Http.OK.real, Http(200) is Http.OK)
Size = IntEnum("Size", "S M L", start=10)
print(list(Size), Size.M + 0, Size["L"])
Dir = StrEnum("Dir", ["UP", "DOWN"])
print(list(Dir), Dir.UP == "up", Dir("down"))


class Perm(IntFlag):
    R = 4
    W = 2
    X = 1


p = Perm.R | Perm.W
print(p, repr(p), p | 1, Perm(7), Perm.R + 1, p & ~Perm.W, repr(~Perm.R), list(Perm(5)), Perm(0), bool(Perm(0)))


class Animal(Enum):
    DOG = 1
    CAT = 2

    def __str__(self):
        return self.name.title()

    def __format__(self, spec):
        return format(str(self), spec)


print(Animal.DOG, f"[{Animal.CAT:>6}]", repr(Animal.DOG))


class Coord(tuple, Enum):
    ORIGIN = (0, 0)
    UNIT = (1, 1)


print(Coord.UNIT, Coord.UNIT[0], Coord((0, 0)), isinstance(Coord.ORIGIN, tuple))
print(Color.__members__.keys(), dir(Size)[:3], enum.EnumMeta is enum.EnumType)
