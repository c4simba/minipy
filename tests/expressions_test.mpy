# Group 2: ternary, f-strings, %-formatting, comprehensions
# ternary
x = 7
print("odd" if x % 2 else "even")
print(10 if x > 5 else 20)
print("neg" if x < 0 else ("zero" if x == 0 else "pos"))
print(1 + (100 if True else 0))

# % formatting
print("%d apples and %d oranges" % (3, 5))
print("name=%s age=%d" % ("bob", 42))
print("%.2f" % 3.14159)
print("[%5d]" % 42)
print("%x" % 255)
print("just text")

# f-strings
a = 2
b = 3
print(f"{a} + {b} = {a + b}")
print(f"product is {a * b}")
print(f"pi ~ {3.14159:.3f}")
print(f"[{7:04}]")
print(f"{{not a field}}")
name = "world"
print(f"hello {name}")

# list comprehensions
print([n * n for n in range(6)])
print([n for n in range(10) if n % 3 == 0])
print([a * b for a in range(1, 3) for b in range(1, 4)])

# dict comprehension
squares = {n: n * n for n in range(5)}
print(squares[4])
print(len(squares))

# set comprehension
print(len({n % 4 for n in range(20)}))

# generator expression (eager) fed to a comprehension
print([v for v in (n for n in range(5))])
