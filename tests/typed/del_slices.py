# del of names and slices, slice assignment
a = [1, 2, 3, 4, 5]
a[1:3] = [9, 9, 9]
print(a)
a[::2] = [0, 0, 0]
print(a)
a[:0] = [7]
print(a)
a[len(a):] = (x * 10 for x in range(2))
print(a)
del a[1:3]
print(a)
del a[::2]
print(a)
a[:] = a + a
print(a)
s = ["x", "y"]
s[1:] = "abc"
print(s)
try:
    a[::2] = [1]
except ValueError as e:
    print(e)
b = [1, 2, 3]
del b[-1:]
del b[5:]
print(b)


def f(flag: bool) -> str:
    x = "value"
    del x
    if flag:
        x = "again"
    try:
        return x
    except UnboundLocalError as e:
        return str(e)


print(f(True), "|", f(False))
counter = 5
del counter
try:
    print(counter)
except NameError as e:
    print(e)
counter = 7
print(counter)
nums = {"a": 1}
del nums["a"], b[0]
print(nums, b)
