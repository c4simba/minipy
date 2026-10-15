def f(s: str, n: int) -> str:
    if s and n > 5:
        return "both"
    if not (s or n):
        return "neither"
    return "one"

print(f("a", 6), f("", 0), f("", 7), f("x", 1))
items = ["a", "", "bc"]
print([x for x in items if x and len(x) > 1])
k = 3
print("yes" if items and k else "no")
while items and k > 0:
    k -= 1
    items.pop()
print(items, k)
flag = not ("" and 5)
print(flag)
assert items or k == 0, "never"
d = {"a": 1}
print(not (d and []))
