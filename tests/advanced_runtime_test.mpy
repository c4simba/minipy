def make_adder(x):
    y = 3
    def add(z):
        return x + y + z
    return add

add10 = make_adder(10)
print(add10(5))

mul2 = lambda n: n * 2
print(mul2(21))

def nums(n):
    i = 0
    while i < n:
        yield i
        i += 1

for v in nums(3):
    print(v)

g = nums(2)
print(next(g))
print(next(g))
try:
    print(next(g))
except StopIteration as e:
    print("stopped")
