# json (CPython's package; plain values through the interpreter's own encoder and decoder)
import json, io, math
from collections import OrderedDict
data = {"a": [1, 2.5, None, True, False], "b": {"x": "é\n\t\"q\"", "y": []}, "c": "☃", "n": -0.0, "big": 1e100, "small": 1e-7}
print(json.dumps(data))
print(json.dumps(data, ensure_ascii=False))
print(json.dumps(data, indent=2, sort_keys=True))
print(json.dumps(data, separators=(",", ":")))
print(json.dumps({1: "int key", 2.5: "f", True: "b", None: "n"}))
print(json.dumps([float("nan"), float("inf"), -float("inf")]))
try:
    json.dumps(float("nan"), allow_nan=False)
except ValueError as e:
    print("ValueError", e)
class P:
    def __init__(self, x): self.x = x
try:
    json.dumps({"p": P(1)})
except TypeError as e:
    print("TypeError", e)
print(json.dumps({"p": P(1)}, default=lambda o: o.__dict__))
class Enc(json.JSONEncoder):
    def default(self, o):
        if isinstance(o, set): return sorted(o)
        return super().default(o)
print(json.dumps({"s": {3, 1, 2}}, cls=Enc), json.dumps((1, 2)), json.dumps("x"), json.dumps(5))
l = []; l.append(l)
try:
    json.dumps(l)
except ValueError as e:
    print("ValueError", e)
print(json.loads('{"a": [1, 2.5, null, true], "b": "\\u00e9"}'))
print(json.loads("[NaN, Infinity, -Infinity]"), json.loads('"x"'), json.loads(" 5 "))
for bad in ['{"a": }', "[1, 2", '{"a" 1}', "", "nul", '{"a": 1,}', '[1] x', '"\\x"']:
    try:
        json.loads(bad)
    except json.JSONDecodeError as e:
        print("JSONDecodeError", e.msg, e.pos, e.lineno, e.colno, "|", e)
print(json.loads('{"b": 1, "a": 2}', object_pairs_hook=OrderedDict))
print(json.loads('{"x": 1.5}', parse_float=str), json.loads('{"x": 15}', parse_int=float))
print(json.loads('{"z": {"q": 1}}', object_hook=lambda d: sorted(d)))
f = io.StringIO()
json.dump({"k": [1, 2]}, f, indent=1)
print(f.getvalue())
f.seek(0)
print(json.load(f))
print(json.loads(b'{"bytes": true}'), json.loads(bytearray(b'[1]')))
print(json.JSONDecoder().raw_decode('{"a": 1} rest'))
print(list(json.JSONEncoder(indent=None).iterencode({"a": [1, {"b": 2}]})))
print(json.dumps({"a": 1, "b": [1, 2, {"c": 3}]}, indent="\t"))
print(json.dumps([], indent=4), json.dumps({}, indent=4), json.dumps([[]], indent=2))
cases = ['"abc', '"a\\u12"', '"a\\uzzzz"', '"\x01"', '{"a": 1', '{1: 2}', '[1,]', '[,]', '{"a" : 1, }', 'tru', 'fals', '-', '1.', '01', '[1 2]', '{"a": 1 "b": 2}', '"\\ud800"', '"\\', '[', '{', '  ', '1e', '[1, 2]]', '{"a":"b"}x', 'NaNx', '"a\nb"', '-Infinity', '- 1', '[-]', '{"":1}']
for c in cases:
    try:
        print(repr(c), "->", repr(json.loads(c)))
    except json.JSONDecodeError as e:
        print(repr(c), "JSONDecodeError", e)
    except Exception as e:
        print(repr(c), type(e).__name__, e)
