class K:
    n = 2 * 3
    names = ["a"] * n
    label = "k" + str(n)

class Counter:
    count = 0
    items: list[str] = []
    def __init__(self, name: str):
        Counter.count += 1
        self.items.append(name)
        self.name = name

class Animal:
    sound = "..."
    legs = 4
    def speak(self) -> str:
        return self.sound + "!"

class Dog(Animal):
    sound = "woof"

class Bird(Animal):
    sound = "tweet"
    legs = 2

class P:
    x = 0
    def __init__(self, x: int):
        if x > 0:
            self.x = x

print(K.n, K.names, K.label)
a = Counter("a")
b = Counter("b")
print(Counter.count, a.count, b.items, Counter.items)
zoo: list[Animal] = [Animal(), Dog(), Bird()]
for z in zoo:
    print(z.speak(), z.legs, z.sound)
print(Dog.sound, Bird.legs, Animal.legs)
p = P(5); q = P(0)
print(p.x, q.x, P.x)
P.x = 9
print(p.x, q.x, P.x, P(0).x)
K.n += 1
print(K.n)
