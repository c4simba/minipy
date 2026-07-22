# Single-line compound-statement suites (body on the same line as the colon).

def k(): return 42
print(k())
print("after def")

def add(a, b): return a + b
print(add(3, 4))

if k() == 42: print("if-true")
if k() == 0: print("nope")
else: print("else-branch")

x = 5
if x > 10: print("big")
elif x > 3: print("medium")
else: print("small")

for i in range(3): print(i)

n = 0
while n < 3: n = n + 1
print(n)

class C: pass
c = C()
print("class ok")

# single-line body followed by a normal multi-line block still parses
def describe(v):
    if v < 0: return "neg"
    return "nonneg"
print(describe(-2))
print(describe(7))

# nested single-line inside a multi-line suite
def classify(v):
    for t in [v]:
        if t == 0: print("zero")
        else: print("nonzero")
classify(0)
classify(9)
