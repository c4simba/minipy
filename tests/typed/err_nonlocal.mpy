def outer() -> int:
    def inner() -> None:
        nonlocal missing
        missing = 1
    inner()
    return 0
print(outer())
