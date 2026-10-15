def check(age: int):
    if age < 0:
        raise ValueError("age must not be negative: " + str(age))
    print("age", age)

check(5)
assert 1 + 1 == 2, "math"
check(-3)
