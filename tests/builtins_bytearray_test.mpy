# bytearray, the bytes methods it shares, float.fromhex/from_number, unhashable dict keys and set elements
FUNCS = {
    "bytearray('x')": lambda ba: bytearray('x'),
    "bytearray(b'x', 'utf-8')": lambda ba: bytearray(b'x', 'utf-8'),
    'bytearray(-1)': lambda ba: bytearray(-1),
    'bytearray(1.5)': lambda ba: bytearray(1.5),
    'bytearray([300])': lambda ba: bytearray([300]),
    "bytearray(['a'])": lambda ba: bytearray(['a']),
    "bytearray(b'ab')[5]": lambda ba: bytearray(b'ab')[5],
    "bytearray(b'ab')['x']": lambda ba: bytearray(b'ab')['x'],
    'ba.append(300)': lambda ba: ba.append(300),
    "ba.append('a')": lambda ba: ba.append('a'),
    "ba.extend('str')": lambda ba: ba.extend('str'),
    'ba.extend(5)': lambda ba: ba.extend(5),
    "ba.extend([1, 'x'])": lambda ba: ba.extend([1, 'x']),
    'bytearray().pop()': lambda ba: bytearray().pop(),
    'ba.pop(10)': lambda ba: ba.pop(10),
    'ba.remove(99)': lambda ba: ba.remove(99),
    'ba.insert(0, 300)': lambda ba: ba.insert(0, 300),
    'hash(ba)': lambda ba: hash(ba),
    'ba + 1': lambda ba: ba + 1,
    "ba + 'x'": lambda ba: ba + 'x',
    "b'x' + ba": lambda ba: b'x' + ba,
    "ba + b'y'": lambda ba: ba + b'y',
    'ba * 2': lambda ba: ba * 2,
    '2 * ba': lambda ba: 2 * ba,
    'ba.__setitem__(0, 300)': lambda ba: ba.__setitem__(0, 300),
    'ba.__setitem__(slice(0, 1), 5)': lambda ba: ba.__setitem__(slice(0, 1), 5),
    "ba.__setitem__(slice(0, 4, 2), b'xyz')": lambda ba: ba.__setitem__(slice(0, 4, 2), b'xyz'),
    "ba.__setitem__(0, 'a')": lambda ba: ba.__setitem__(0, 'a'),
    "bytearray(b'abc').upper()": lambda ba: bytearray(b'abc').upper(),
    "bytearray(b'a,b').split(b',')": lambda ba: bytearray(b'a,b').split(b','),
    "bytearray(b'abc').partition(b'b')": lambda ba: bytearray(b'abc').partition(b'b'),
    "bytearray(b'-').join([b'a', bytearray(b'b')])": lambda ba: bytearray(b'-').join([b'a', bytearray(b'b')]),
    "bytearray.fromhex('6162')": lambda ba: bytearray.fromhex('6162'),
    "bytearray(b'abc') == b'abc'": lambda ba: bytearray(b'abc') == b'abc',
    "b'abc' == bytearray(b'abc')": lambda ba: b'abc' == bytearray(b'abc'),
    "bytearray(b'abc') < b'abd'": lambda ba: bytearray(b'abc') < b'abd',
    "b'b' in bytearray(b'abc')": lambda ba: b'b' in bytearray(b'abc'),
    "98 in bytearray(b'abc')": lambda ba: 98 in bytearray(b'abc'),
    "bytearray(b'abc').find(bytearray(b'c'))": lambda ba: bytearray(b'abc').find(bytearray(b'c')),
    "list(bytearray(b'ab'))": lambda ba: list(bytearray(b'ab')),
    "bytes(bytearray(b'ab'))": lambda ba: bytes(bytearray(b'ab')),
    'repr(bytearray())': lambda ba: repr(bytearray()),
    "str(bytearray(b'a\\x00'))": lambda ba: str(bytearray(b'a\x00')),
    'bytearray(3)': lambda ba: bytearray(3),
    'bytearray(range(3))': lambda ba: bytearray(range(3)),
    "bytearray('hé', 'utf-8')": lambda ba: bytearray('hé', 'utf-8'),
    "bytearray(b'abc').hex()": lambda ba: bytearray(b'abc').hex(),
    "bytearray(b'abc').decode()": lambda ba: bytearray(b'abc').decode(),
    "bytearray(b'ab').copy()": lambda ba: bytearray(b'ab').copy(),
    "len(bytearray(b'abc'))": lambda ba: len(bytearray(b'abc')),
    "bytearray(b'abc')[1:]": lambda ba: bytearray(b'abc')[1:],
    "bytearray(b'abcdef')[::2]": lambda ba: bytearray(b'abcdef')[::2],
    'bool(bytearray())': lambda ba: bool(bytearray()),
    "b''.join([bytearray(b'x'), b'y'])": lambda ba: b''.join([bytearray(b'x'), b'y']),
    "bytearray(b'abc').count(98)": lambda ba: bytearray(b'abc').count(98),
    "bytearray(b'abc').startswith((b'x', bytearray(b'a')))": lambda ba: bytearray(b'abc').startswith((b'x', bytearray(b'a'))),
    "int.from_bytes(bytearray(b'\\x01\\x02'), 'big')": lambda ba: int.from_bytes(bytearray(b'\x01\x02'), 'big'),
    "bytearray(b'abc').replace(b'b', b'XX')": lambda ba: bytearray(b'abc').replace(b'b', b'XX'),
    "bytearray(b' ab ').strip()": lambda ba: bytearray(b' ab ').strip(),
    "bytearray(b'Abc').swapcase()": lambda ba: bytearray(b'Abc').swapcase(),
    "bytearray(b'ab').isalpha()": lambda ba: bytearray(b'ab').isalpha(),
    "bytearray(b'ab') + bytearray(b'cd')": lambda ba: bytearray(b'ab') + bytearray(b'cd'),
    'isinstance(bytearray(), bytearray)': lambda ba: isinstance(bytearray(), bytearray),
    'type(bytearray()).__name__': lambda ba: type(bytearray()).__name__,
    "bytearray(b'abc').center(7, b'*')": lambda ba: bytearray(b'abc').center(7, b'*'),
    "bytearray(b'ab') != b'ab'": lambda ba: bytearray(b'ab') != b'ab',
    "sorted([bytearray(b'b'), bytearray(b'a')])": lambda ba: sorted([bytearray(b'b'), bytearray(b'a')]),
    "bytearray(b'x') in [b'x']": lambda ba: bytearray(b'x') in [b'x'],
    "{bytearray(b'a'): 1}": lambda ba: {bytearray(b'a'): 1},
    'f\'{bytearray(b"q")}\'': lambda ba: f'{bytearray(b"q")}',
    "'%s' % bytearray(b'z')": lambda ba: '%s' % bytearray(b'z'),
    "bytearray(b'a').__len__()": lambda ba: bytearray(b'a').__len__(),
    "bytearray(b'hello world').title()": lambda ba: bytearray(b'hello world').title(),
    "bytearray(b'a\\nb').splitlines()": lambda ba: bytearray(b'a\nb').splitlines(),
    "bytearray(b'abc').removeprefix(b'a')": lambda ba: bytearray(b'abc').removeprefix(b'a'),
}
cases = ["bytearray('x')", "bytearray(b'x', 'utf-8')", "bytearray(-1)", "bytearray(1.5)", "bytearray([300])", "bytearray(['a'])",
 "bytearray(b'ab')[5]", "bytearray(b'ab')['x']", "ba.append(300)", "ba.append('a')", "ba.extend('str')", "ba.extend(5)", "ba.extend([1, 'x'])",
 "bytearray().pop()", "ba.pop(10)", "ba.remove(99)", "ba.insert(0, 300)", "hash(ba)", "ba + 1", "ba + 'x'", "b'x' + ba", "ba + b'y'",
 "ba * 2", "2 * ba", "ba.__setitem__(0, 300)", "ba.__setitem__(slice(0, 1), 5)", "ba.__setitem__(slice(0, 4, 2), b'xyz')",
 "ba.__setitem__(0, 'a')", "bytearray(b'abc').upper()", "bytearray(b'a,b').split(b',')", "bytearray(b'abc').partition(b'b')",
 "bytearray(b'-').join([b'a', bytearray(b'b')])", "bytearray.fromhex('6162')", "bytearray(b'abc') == b'abc'", "b'abc' == bytearray(b'abc')",
 "bytearray(b'abc') < b'abd'", "b'b' in bytearray(b'abc')", "98 in bytearray(b'abc')", "bytearray(b'abc').find(bytearray(b'c'))",
 "list(bytearray(b'ab'))", "bytes(bytearray(b'ab'))", "repr(bytearray())", "str(bytearray(b'a\\x00'))", "bytearray(3)", "bytearray(range(3))",
 "bytearray('hé', 'utf-8')", "bytearray(b'abc').hex()", "bytearray(b'abc').decode()", "bytearray(b'ab').copy()", "len(bytearray(b'abc'))",
 "bytearray(b'abc')[1:]", "bytearray(b'abcdef')[::2]", "bool(bytearray())", "b''.join([bytearray(b'x'), b'y'])", "bytearray(b'abc').count(98)",
 "bytearray(b'abc').startswith((b'x', bytearray(b'a')))", "int.from_bytes(bytearray(b'\\x01\\x02'), 'big')", "bytearray(b'abc').replace(b'b', b'XX')",
 "bytearray(b' ab ').strip()", "bytearray(b'Abc').swapcase()", "bytearray(b'ab').isalpha()", "bytearray(b'ab') + bytearray(b'cd')",
 "isinstance(bytearray(), bytearray)", "type(bytearray()).__name__", "bytearray(b'abc').center(7, b'*')", "bytearray(b'ab') != b'ab'",
 "sorted([bytearray(b'b'), bytearray(b'a')])", "bytearray(b'x') in [b'x']", "{bytearray(b'a'): 1}", "f'{bytearray(b\"q\")}'", "'%s' % bytearray(b'z')",
 "bytearray(b'a').__len__()", "bytearray(b'hello world').title()", "bytearray(b'a\\nb').splitlines()", "bytearray(b'abc').removeprefix(b'a')"]
for c in cases:
    ba = bytearray(b"abc")
    try:
        print(c, "->", repr(FUNCS[c](ba)))
    except Exception as e:
        print(c, "!", type(e).__name__, e)
ba = bytearray(b"hello")
ba[0] = 72; ba[1:3] = b"EY"; ba[-1:] = [33, 34]; del ba[0]; del ba[::2]
print(ba)
ba = bytearray(b"abc"); ba += b"de"; ba *= 2; ba.extend(range(65, 68)); ba.insert(1, 120); ba.reverse(); print(ba, ba.pop(), ba.pop(0)); ba.remove(97); ba.clear(); print(ba)
ba = bytearray(b"xyz"); ba[3:] = bytearray(b"!"); ba[0:0] = b">>"; print(ba)
ba = bytearray(b"0123456789"); del ba[1:8:3]; print(ba); ba[::-1] = b"abcdefg"; print(ba); ba.extend(ba); print(ba, len(ba))
ba = bytearray(b"ab"); it = iter(ba); ba.append(99); print(list(it), bytearray(b"%d") , ba.decode("ascii"), bytes(ba) == ba)
cases = ['0x1.8p1', '1.8p1', ' -0X1P-1074 ', '0x1p-1075', '0x1.0000000000001p0', '0x1.00000000000008p0', '0x1.00000000000018p0', 'inf', '-Infinity', 'nan', '0x1p1024', '0x.8', '0x8.', '0x', '0xp1', '1e5', '0x1.fffffffffffff8p1023', '0x1.fffffffffffff7p1023', '0x0.0000000000001p-1022', '0x0.00000000000008p-1022', '0x0.0000000000000801p-1022', '0x1p-1080', '0x3.8p-1076', '00x1', '0x1.123456789abcdef123p3', '0x123456789abcdef123456p-20', '-0x0p0', '0x1p+5', 'nan ', '0x1.8000000000000000000001p0']
for s in cases:
    try: print(repr(s), float.fromhex(s))
    except Exception as e: print(repr(s), type(e).__name__, e)
for x in [1, True, 2.5, '3', None]:
    try: print(float.from_number(x))
    except Exception as e: print(type(e).__name__, e)
print((5).real, (5).imag, (5).numerator, (5).denominator, (2.5).real, (2.5).imag, True.real, type(True.real).__name__)
for f in [lambda: {[1]: 2}, lambda: {}.__setitem__([1], 2), lambda: {}[[1]], lambda: [1] in {}, lambda: {[1]}, lambda: set().add([1]), lambda: [1] in set(), lambda: hash([1]), lambda: frozenset([[1]]), lambda: {}.get([1]), lambda: dict.fromkeys([[1]])]:
    try: f()
    except TypeError as e: print(e)
