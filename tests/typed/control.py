# loops with break/continue/else, nested loops, while/else, ternaries, chained comparisons
for i in range(10):
    if i % 2:
        continue
    if i > 6:
        break
    print("even", i)
else:
    print("not reached")

for n in range(2, 20):
    for d in range(2, n):
        if n % d == 0:
            break
    else:
        print(n, "is prime")

k = 0
while k < 3:
    k += 1
else:
    print("while done", k)

count = 0
while True:
    count += 1
    if count >= 5:
        break
print("count", count)

for x in range(3):
    for y in range(3):
        if x == y:
            continue
        print(x, y, end="; ")
print()

for i in range(10, 0, -3):
    print(i, end=" ")
print()

n = 5
step = -1
for i in range(n, 0, step):
    print(i, end=",")
print()

total = 0
for c in "hello":
    if c in "aeiou":
        total += 1
print("vowels", total)

v = 15
print("fizz" if v % 3 == 0 else "", "buzz" if v % 5 == 0 else "")
print(0 < v < 10, 10 < v <= 15, v == 15 != 16)
grid = [[1, 2, 3], [4, 5, 6]]
s = 0
for row in grid:
    for cell in row:
        s += cell
print(s, [r[1] for r in grid], [c * 2 for r in grid for c in r if c % 2])
flags = [x > 2 for x in [1, 2, 3, 4]]
print(flags)
