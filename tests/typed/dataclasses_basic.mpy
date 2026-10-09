from dataclasses import dataclass, field, FrozenInstanceError, KW_ONLY
from typing import ClassVar

@dataclass
class Point:
    x: int
    y: int = 0
    tags: list[str] = field(default_factory=list)
    count: ClassVar[int] = 0

@dataclass(order=True, frozen=True)
class Version:
    major: int
    minor: int = 0
    label: str = field(default="", compare=False, repr=False)

@dataclass
class P3(Point):
    z: float = 1.5
    def __post_init__(self):
        self.norm = self.x + self.y + self.z

@dataclass
class K:
    a: int
    _: KW_ONLY
    b: int = 2
    c: str = "c"

p = Point(1, 2)
q = Point(1, 2)
print(p, p == q, p == Point(1), Point.y, Point.count)
p.tags.append("t")
print(p, q, p == q)
v1 = Version(1, 2, "a"); v2 = Version(1, 3)
print(v1, v1 < v2, sorted([v2, v1]), v1 == Version(1, 2, "zzz"), hash(v1) == hash(Version(1, 2)))
try:
    v1.major = 5
except FrozenInstanceError as e:
    print("frozen:", e)
r = P3(1, 2)
print(r, r.norm, isinstance(r, Point))
print(K(1), K(1, b=5, c="x"))
s = {v1, v2, Version(1, 2)}
print(len(s), Version(1, 2) in s)
