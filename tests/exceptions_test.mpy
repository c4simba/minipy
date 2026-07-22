print("exceptions")

try:
    raise RuntimeError("boom")
except RuntimeError as e:
    print(e)
finally:
    print("finally explicit")

try:
    [1][9]
except IndexError as e:
    print(e)
finally:
    print("finally vm")

it = iter([1])
print(next(it))
try:
    next(it)
except StopIteration as e:
    print("stop")

try:
    1 + []
except TypeError as e:
    print(e)
