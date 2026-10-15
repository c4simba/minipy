# A module variable holding a subclass's instance, passed - inside a function
# checked before the module's own lines - where the base class is wanted: it
# keeps its own class (its subclass's attributes stay there), annotated or not.
class Base:
    def __init__(self):
        self.folder = "?"

    def name(self) -> str:
        return "base"

class Sub(Base):
    def __init__(self):
        super().__init__()
        self.reads = 0

    def name(self) -> str:
        self.reads += 1
        return "sub"

def use(b: Base) -> str:
    return b.name() + b.folder

def show() -> None:
    print(use(annotated), use(plain))

def only_here() -> None:
    print(use(later))                # (the base class itself)

annotated: Sub = Sub()
plain = Sub()
later = Base()
show()
show()
only_here()
print(annotated.reads, plain.reads, annotated.name(), plain.folder)
