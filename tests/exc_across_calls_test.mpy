# Regression: an exception raised inside a called function must be catchable by
# a try/except in the caller (this used to desync the C stack and segfault).

def deep():
    raise ValueError("from deep")

def middle():
    deep()

try:
    middle()
except ValueError as e:
    print("caught across two calls")

print("continues normally")

# nested try: inner function raises, only the outer handler matches
def raises_key():
    raise KeyError("k")

def guarded():
    try:
        raises_key()
    except IndexError:
        print("wrong handler (should not print)")

try:
    guarded()
except KeyError:
    print("caught by outer handler")

# exception raised, caught, then a fresh exception raised and caught again
def boom(n):
    raise RuntimeError("boom" + str(n))

count = 0
for i in range(3):
    try:
        boom(i)
    except RuntimeError:
        count = count + 1
print(count)

# uncaught still unwinds to a clean state after a caught one
try:
    boom(9)
except RuntimeError:
    print("last caught")
print("end")
