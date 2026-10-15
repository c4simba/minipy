# Group 4: default args, keyword args, *args, **kwargs, call unpacking
# default arguments (literal and expression)
def greet(name, greeting="hi"):
    return greeting + " " + name
print(greet("bob"))
print(greet("sam", "hello"))

BASE = 100
def offset(x, by=BASE + 1):
    return x + by
print(offset(5))
print(offset(5, 10))

def f(x=None):
    if x is None:
        return -1
    return x
print(f())
print(f(42))

# keyword arguments
def rect(w, h):
    return w * h
print(rect(h=3, w=4))
print(rect(5, h=2))

def describe(a, b, c):
    return a * 100 + b * 10 + c
print(describe(1, c=3, b=2))

# *args
def total(*nums):
    s = 0
    for n in nums:
        s = s + n
    return s
print(total())
print(total(1, 2, 3, 4, 5))

def head_tail(first, *rest):
    return first + len(rest)
print(head_tail(10))
print(head_tail(10, 1, 2, 3))

# **kwargs
def collect(**opts):
    return len(opts)
print(collect())
print(collect(a=1, b=2, c=3))

def lookup(**kw):
    return kw["port"]
print(lookup(host="x", port=8080))

# everything together
def full(a, b=2, *args, **kw):
    return a + b + len(args) + len(kw)
print(full(1))
print(full(1, 5))
print(full(1, 5, 9, 9))
print(full(1, 5, 9, 9, x=1, y=2))

# call-site unpacking
def add3(a, b, c):
    return a + b + c
nums = [1, 2, 3]
print(add3(*nums))
params = {"a": 10, "b": 20, "c": 30}
print(add3(**params))
print(add3(*[100], **{"b": 200, "c": 300}))
