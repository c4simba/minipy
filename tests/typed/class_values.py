class Animal:
    def __init__(self, name: str):
        self.name = name
    def speak(self) -> str:
        return "..."

class Dog(Animal):
    def speak(self) -> str:
        return self.name + " says woof"

class Cat(Animal):
    def __init__(self, name: str, lives: int = 9):
        super().__init__(name)
        self.lives = lives
    def speak(self) -> str:
        return f"{self.name} says meow ({self.lives})"

registry = {"dog": Dog, "cat": Cat, "generic": Animal}
for kind in ["cat", "dog", "generic"]:
    cls = registry[kind]
    pet = cls("pet-" + kind)
    print(type(pet).__name__, pet.speak(), isinstance(pet, Animal))

def build(k: type[Animal], n: str) -> Animal:
    return k(n)
print(build(Dog, "rex").speak(), build(Cat, "tom").speak())
kinds = [Dog, Cat]
print([k("x").speak() for k in kinds])
