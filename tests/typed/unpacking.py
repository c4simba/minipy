# unpacking: starred and nested targets, *xs / **d in displays, :=
a, *b = [1, 2, 3]
print(a, b)
*c, d = (1, 2, 3)
print(c, d)
e, *f, g = "hello"
print(e, f, g)
h, *i = (1, 2.5, 3.5)
print(h, i)
(j, k), l = (1, 2), 3
print(j, k, l)
m, (n, *o) = 0, [1, 2, 3]
print(m, n, o)
p, q = "xy"
print(p, q)
for (x, y), z in [((1, 2), 3), ((4, 5), 6)]:
    print(x, y, z, end="; ")
print()
for first, *rest in [[1, 2, 3], [4]]:
    print(first, rest, end="; ")
print()
xs = [1, 2]
ys = [*xs, 3, *xs]
print(ys, (*xs, 9), {*xs, 5} == {1, 2, 5}, [*"ab", *"c"], [*range(2)])
base = {"a": 1}
merged = {**base, "b": 2, **{"a": 3}}
print(merged)
data = [5, 1, 9]
if (n2 := len(data)) > 2:
    print("long", n2)
print([w for v in data if (w := v * 2) > 4], w)
total = 0
while (chunk := data.pop()) != 1:
    total += chunk
print(total, chunk)
def bad(kind: int) -> None:
    try:
        if kind == 0:
            r1, s1 = [1, 2, 3]
        elif kind == 1:
            r2, s2, t2 = [1]
        elif kind == 2:
            r3, *s3, t3, u3 = [1, 2]
        elif kind == 3:
            r4, s4 = "abc"
        print("ok")
    except ValueError as ex:
        print(ex)
for kind in range(4):
    bad(kind)
