def count(fn) -> int:
    return 1

@count
def hello() -> str:
    return "hi"

print(hello())
