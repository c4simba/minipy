from .config import Item, make

def describe(it: Item) -> str:
    return "item " + it.title

def first() -> Item:
    return make("first")
