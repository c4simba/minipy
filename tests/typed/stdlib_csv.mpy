# csv: reader (quotes, escapes, multi-line fields, strict), writer, DictReader, DictWriter, dialects, files
import csv
import io

r = csv.reader(['a,b,c', '"x,y","he said ""hi""",z', '1, 2,3', '', 'last'])
print(list(r))
r = csv.reader(['a;b', 'c;"d;e"'], delimiter=';')
print(list(r), r.line_num, r.dialect.delimiter)
print(list(csv.reader(['a, b ,c'], skipinitialspace=True)))
print(list(csv.reader(['"multi', 'line",x'])))
print(list(csv.reader(['"multi\n', 'line",x\n'])))
print(list(csv.reader(['a\\,b,c'], escapechar='\\')))
for bad in [['"abc'], ['a"b"c'], ['"ab"c']]:
    try:
        print(list(csv.reader(bad, strict=True)))
    except csv.Error as e:
        print('Error', e)
    print(list(csv.reader(bad)))
s = io.StringIO()
w = csv.writer(s)
w.writerow(['a', 'b,c', 'd"e', '', 'x\ny', ' sp '])
w.writerow([1, 2])
w.writerows([[3, 4], [5]])
w.writerow([1.5, 2.25])
w.writerow([''])
w2 = csv.writer(s, quoting=csv.QUOTE_ALL, lineterminator='\n')
w2.writerow(['a', 'b'])
w4 = csv.writer(s, delimiter='\t', quoting=csv.QUOTE_NONE, escapechar='\\')
w4.writerow(['a\tb', 'c'])
print(repr(s.getvalue()))
dr = csv.DictReader(['name,age', 'bob,30', 'z'])
for row in dr:
    print(row)
print(dr.fieldnames, dr.line_num)
s2 = io.StringIO()
dw = csv.DictWriter(s2, fieldnames=['x', 'y'])
dw.writeheader()
dw.writerow({'x': '1', 'y': 'a b'})
dw.writerow({'x': '2'})
print(repr(s2.getvalue()))
try:
    dw.writerow({'z': '1'})
except ValueError as e:
    print('ValueError', e)
print(csv.list_dialects(), csv.QUOTE_MINIMAL, csv.QUOTE_ALL, csv.QUOTE_NONNUMERIC, csv.QUOTE_NONE, csv.field_size_limit())
d = csv.get_dialect('excel')
print(d.delimiter, repr(d.lineterminator), d.quotechar, d.doublequote, d.quoting)
csv.register_dialect('pipes', delimiter='|')
print(list(csv.reader(['a|b'], 'pipes')))
rr = csv.reader(['h1,h2', 'v1,v2'])
header = next(rr)
print(header, [row for row in rr])
with open("_mpy_csv_test.csv", "w", newline="") as f:
    cw = csv.writer(f)
    cw.writerow(["id", "name"])
    cw.writerow(["1", "Ann, B"])
with open("_mpy_csv_test.csv", newline="") as f:
    print(list(csv.reader(f)))
import os
os.remove("_mpy_csv_test.csv")
