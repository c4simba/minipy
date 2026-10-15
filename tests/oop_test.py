# Group 7: classes / OOP
# --- inheritance + override ---
class Animal:
    def __init__(self, name):
        self.name = name
    def speak(self):
        return "..."
    def describe(self):
        return self.name + " says " + self.speak()

class Dog(Animal):
    def speak(self):
        return "woof"

class Cat(Animal):
    def speak(self):
        return "meow"

print(Dog("Rex").describe())
print(Cat("Tom").describe())

# --- super() ---
class Base:
    def __init__(self, x):
        self.x = x
    def total(self):
        return self.x

class Derived(Base):
    def __init__(self, x, y):
        super().__init__(x)
        self.y = y
    def total(self):
        return super().total() + self.y

d = Derived(10, 5)
print(d.x)
print(d.y)
print(d.total())

# --- multi-level inheritance ---
class A:
    def kind(self):
        return "A"
class B(A):
    pass
class C(B):
    pass
print(C().kind())
print(isinstance(C(), A))
print(isinstance(C(), B))
print(isinstance(B(), C))

# --- dunder: __str__ / __repr__ ---
class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y
    def __str__(self):
        return "Point(" + str(self.x) + ", " + str(self.y) + ")"
    def __repr__(self):
        return "Point(" + str(self.x) + ", " + str(self.y) + ")"
p = Point(3, 4)
print(p)
print(str(p))
print([Point(1, 2), Point(5, 6)])

# --- dunder: arithmetic + comparison ---
class Vec:
    def __init__(self, n):
        self.n = n
    def __add__(self, o):
        return Vec(self.n + o.n)
    def __mul__(self, k):
        return Vec(self.n * k)
    def __eq__(self, o):
        return self.n == o.n
    def __lt__(self, o):
        return self.n < o.n
print((Vec(2) + Vec(3)).n)
print((Vec(4) * 3).n)
print(Vec(5) == Vec(5))
print(Vec(5) == Vec(6))
vs = [Vec(3), Vec(1), Vec(2)]
vs.sort()
print([v.n for v in vs])

# --- dunder: __call__, __len__, __getitem__ ---
class Adder:
    def __init__(self, base):
        self.base = base
    def __call__(self, x):
        return self.base + x
add10 = Adder(10)
print(add10(5))

class Seq:
    def __init__(self, data):
        self.data = data
    def __len__(self):
        return len(self.data)
    def __getitem__(self, i):
        return self.data[i] * 10
s = Seq([1, 2, 3])
print(len(s))
print(s[2])

# --- staticmethod / classmethod / property ---
class Circle:
    pi = 3
    def __init__(self, r):
        self.r = r
    def area(self):
        return Circle.pi * self.r * self.r
    area = property(area)
    def unit():
        return "circle"
    unit = staticmethod(unit)
    def from_diameter(cls, d):
        return Circle(d // 2)
    from_diameter = classmethod(from_diameter)

c = Circle(2)
print(c.area)
print(Circle.unit())
c2 = Circle.from_diameter(10)
print(c2.r)
