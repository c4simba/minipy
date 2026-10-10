# array: typed arrays of C values (the type code is a literal: it decides the item type in compiled code)
import array
import os
import tempfile
from array import array as Array

a = array.array('i', [1, 2, 3])
print(a, len(a), a[0], a[-1], list(a), 2 in a, 7 in a, a.tolist(), a.itemsize, a.typecode)
a.append(4)
a.extend([5, 6])
a.insert(0, 0)
a.insert(-1, 55)
print(a, a.count(55), a.index(55), a.index(2, 1, 5))
print(a[1:4], a[::-1], a[::2], a[5:1:-1], a[-3:], a[:100])
print(a + array.array('i', [9]), a * 2, 2 * a)
a[1:3] = array.array('i', [7, 8, 9])
print(a)
a[::2] = array.array('i', [0, 0, 0, 0, 0])
print(a)
del a[::2]
print(a)
del a[0]
a[0] = 42
a[1] += 100
print(a, a.pop(), a.pop(0), a)
a.extend([3, 108, 1, 2])
a.remove(108)
a.reverse()
print(a, sum(a), max(a), min(a), sorted(a), bool(a), bool(array.array('i')))
for i, x in enumerate(a):
    print(i, x, end="; ")
print()
print([x * 2 for x in a], list(reversed(a)))

d = array.array('d', [1, 2.5])
d.append(3)
d.extend((4.25, 5.5))
d.fromlist([6.5])
print(d, d.tobytes()[:16], d[1:3], d.tolist(), sum(d))
f = array.array('f', [0.1, 1e300, -1e300, 1.5])
print(f, f[0], f.itemsize)

b = Array('B', b'abc')
b.frombytes(b'de')
b.fromlist([102, 103])
print(b, bytes(b), b.buffer_info()[1], b.tobytes())
sb = Array('b', b'\xff\x01\x80')
print(sb, sb.tolist())
h = array.array('h', [1, 2, 3, -2])
h.byteswap()
print(h, h == array.array('h', [256, 512, 768, -257]), h < array.array('h', [256, 513]),
      h > array.array('h', [256, 512]), h != array.array('h', [1]), h <= h, h >= h)
q = array.array('q', [-1, 2 ** 62])
Q = array.array('Q', [9223372036854775807])
L = array.array('L', [5])
print(q, q.tobytes(), Q, Q.itemsize, L.itemsize, L.tobytes())
I = array.array('I', range(5))
print(I, array.array('H', [65535]), array.array('i', array.array('i', [3, 4])))
w = array.array('w', 'héllo')
w.append('!')
w.fromunicode(' wörld')
print(w, w.tounicode(), w[1], w[1:3], len(w), w.tobytes()[:8], w.count('l'))
print(array.typecodes, array.ArrayType('i', [1]))

def over(tc: str, v: int) -> None:
    try:
        array.array('b', [0])
        if tc == 'b':
            array.array('b', [v])
        elif tc == 'B':
            array.array('B', [v])
        elif tc == 'h':
            array.array('h', [v])
        elif tc == 'H':
            array.array('H', [v])
        elif tc == 'i':
            array.array('i', [v])
        elif tc == 'I':
            array.array('I', [v])
        elif tc == 'L':
            array.array('L', [v])
        else:
            array.array('Q', [v])
        print(tc, v, "ok")
    except OverflowError as e:
        print(tc, v, "OverflowError:", e)

for tc in "bBhHiILQ":
    for v in (-1, 127, 128, -129, 300, -300, 70000, -70000, 2 ** 40, -2 ** 40):
        over(tc, v)

e = array.array('i', [1, 2, 3])
try:
    e.remove(99)
except ValueError as ex:
    print("ValueError:", ex)
try:
    e.index(99)
except ValueError as ex:
    print("ValueError:", ex)
try:
    e[10]
except IndexError as ex:
    print("IndexError:", ex)
try:
    e[10] = 1
except IndexError as ex:
    print("IndexError:", ex)
try:
    e.pop(10)
except IndexError as ex:
    print("IndexError:", ex)
try:
    array.array('i').pop()
except IndexError as ex:
    print("IndexError:", ex)
try:
    e.frombytes(b'abc')
except ValueError as ex:
    print("ValueError:", ex)
try:
    e.tounicode()
except ValueError as ex:
    print("ValueError:", ex)
try:
    e.fromunicode("x")
except ValueError as ex:
    print("ValueError:", ex)
try:
    e.extend(array.array('h', [1]))
except TypeError as ex:
    print("TypeError:", ex)
try:
    e[0:1] = array.array('h', [1])
except TypeError as ex:
    print("TypeError:", ex)
try:
    e[::2] = array.array('i', [1, 2, 3])
except ValueError as ex:
    print("ValueError:", ex)
try:
    print(e + array.array('h', [1]))
except TypeError as ex:
    print("TypeError:", ex)
try:
    array.array('i', 'abc')
except TypeError as ex:
    print("TypeError:", ex)
try:
    array.array('w', ['ab'])
except TypeError as ex:
    print("TypeError:", ex)
e += array.array('i', [4])
e *= 2
print(e)

path = os.path.join(tempfile.mkdtemp(), "arr.bin")
with open(path, "wb") as fp:
    array.array('i', [10, 20, 30]).tofile(fp)
r = array.array('i')
with open(path, "rb") as fp:
    r.fromfile(fp, 2)
    try:
        r.fromfile(fp, 5)
    except EOFError as ex:
        print("EOFError:", ex)
print(r, os.path.getsize(path))
os.remove(path)
