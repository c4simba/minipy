# for-loop iterators live on the VM's operand stack: a return from inside a
# loop and a generator suspended inside one must leave the caller's operands
# alone (both used to corrupt expressions like [1, f(x)])
def first_upper(s):
    for ch in s:
        if ch >= "A" and ch <= "Z":
            return ch
    return "-"

def find(rows, x):
    for r in rows:
        for v in r:
            if v == x:
                return [r.index(v), rows.index(r)]
    return None

def tens(n):
    for i in range(n):
        yield i * 10

def pairs(n):
    for i in range(n):
        for j in range(2):
            yield str(i) + str(j)

def guarded(n):
    for i in range(n):
        try:
            yield i
            if i == 1:
                raise ValueError("one")
        except ValueError:
            yield 100

print([1, first_upper("abCd"), first_upper("abc"), 2])
print(["at", find([[1, 2], [3, 4]], 4), find([[1]], 9)])
for v in tens(3):
    print(v)
print([0, list(tens(3)), list(pairs(2))])
for a in tens(2):
    for b in pairs(1):
        print(str(a) + "/" + b)
for v in guarded(3):
    print(v)
print([len(first_upper(w)) for w in ["xY", "zz"]])
