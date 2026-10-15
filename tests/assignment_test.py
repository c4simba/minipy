# Group 3: chained assignment, tuple unpacking, augmented subscript/attr
# chained assignment
a = b = 5
print(a + b)
a = b = c = 7
print(a + b + c)

# tuple unpacking
x, y = 1, 2
print(x)
print(y)
p, q, r = 10, 20, 30
print(p + q + r)

# swap
m = 100
n = 200
m, n = n, m
print(m)
print(n)

# unpack from a list / string
u, v = [8, 9]
print(u * v)
c1, c2, c3 = "abc"
print(c1)
print(c3)

# chained + tuple mixed
a, b = c, d = 3, 4
print(a + b + c + d)

# for-loop tuple targets
total = 0
for i, val in [(1, 10), (2, 20), (3, 30)]:
    total = total + i + val
print(total)

# augmented assignment on subscript
lst = [1, 2, 3]
lst[0] += 100
lst[2] *= 5
print(lst)

# augmented assignment on dict value
d = {"count": 10}
d["count"] += 5
d["count"] //= 3
print(d["count"])

# augmented assignment on attribute
class Box:
    def __init__(self, v):
        self.v = v
box = Box(50)
box.v += 8
box.v %= 7
print(box.v)

# nested subscript augmented
grid = [[1, 2], [3, 4]]
grid[1][0] += 40
print(grid[1][0])
