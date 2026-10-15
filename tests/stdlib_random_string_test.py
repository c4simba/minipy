# random (CPython's Mersenne Twister: the same numbers for a seed) and string (Template, Formatter)
import random
import string
import _string

for s in [0, 1, 42, -42, 2**40 + 7, 2**62, 3.5, -1.5, 0.1, "hello", "", "héllo", b"bytes"]:
    random.seed(s)
    print(repr(s), random.random(), random.getrandbits(40))
random.seed("abc", version=1)
print("v1", random.random())
random.seed(b"abc", version=1)
print("v1b", random.random())
random.seed(12345)
print([random.randint(1, 6) for _ in range(10)], random.randrange(10), random.randrange(5, 50, 7), random.randrange(50, 5, -3))
print(random.choice([1, 2, 3]), random.choice("xyz"), random.choice((4, 5)), random.choice(range(100, 200)))
xs = list(range(20))
random.shuffle(xs)
print(xs)
print(random.sample(range(1000), 10), random.sample("abcdefg", 3), random.sample([1, 2, 3, 4], 4))
print(random.sample(["red", "blue"], counts=[4, 2], k=5), random.sample(range(100), 30)[:5])
print(random.choices("abc", k=8), random.choices([1, 2, 3], weights=[10, 1, 1], k=6), random.choices("xy", cum_weights=[1, 5], k=4))
print(random.choices(range(5), [0.1, 0.2, 0.3, 0.2, 0.2], k=3))
print(random.uniform(1, 10), random.uniform(5.5, 2.5), random.triangular(), random.triangular(0, 10, 2), random.triangular(5, 5))
print(random.gauss(), random.gauss(10, 2), random.normalvariate(), random.normalvariate(5, 0.5))
print(random.lognormvariate(0, 1), random.expovariate(), random.expovariate(3.0))
print(random.vonmisesvariate(1.0, 0.5), random.vonmisesvariate(0.0, 0.0), random.vonmisesvariate(3.0, 4.0))
print(random.gammavariate(0.5, 1.0), random.gammavariate(1.0, 2.0), random.gammavariate(3.0, 1.5), random.betavariate(2, 5))
print(random.paretovariate(3.0), random.weibullvariate(1.0, 1.5))
print([random.binomialvariate(10, 0.3) for _ in range(5)], random.binomialvariate(100, 0.75), random.binomialvariate(1000, 0.4),
      random.binomialvariate(1, 0.5), random.binomialvariate(5, 0.0), random.binomialvariate(5, 1.0))
print(random.randbytes(7), random.randbytes(0), random.randbytes(4), random.getrandbits(0), random.getrandbits(63))
st = random.getstate()
a = [random.random() for _ in range(3)]
random.setstate(st)
print(a == [random.random() for _ in range(3)], len(st[1]), st[0])
r = random.Random(99)
r2 = random.Random(99)
print(r.random() == r2.random(), r.randint(0, 100), r2.choice("abcdef"), random.Random("seed").random(), random.Random().random() < 1)
for bad in [lambda: random.randrange(0), lambda: random.randrange(5, 5), lambda: random.randrange(1, 10, 0), lambda: random.randint(5, 1),
            lambda: random.choice([]), lambda: random.sample([1, 2], 3), lambda: random.choices("ab", [1], k=2),
            lambda: random.choices("ab", [0, 0]), lambda: random.getrandbits(-1), lambda: random.gammavariate(-1, 1),
            lambda: random.binomialvariate(-1), lambda: random.binomialvariate(3, 1.5), lambda: random.sample({1, 2}, 1),
            lambda: random.seed([1, 2]), lambda: random.randrange(10, None, 2), lambda: random.choices("ab", [1, 2], cum_weights=[1, 2])]:
    try:
        bad()
    except (ValueError, IndexError, TypeError) as e:
        print(type(e).__name__, e)


class MyRandom(random.Random):
    def random(self):
        return 0.5


m = MyRandom()
print(m.randrange(10), m.choice("abcd"), m.uniform(0, 2))
sr = random.SystemRandom()
v = sr.random()
print(0 <= v < 1, 0 <= sr.randint(1, 5) <= 5, len(sr.randbytes(5)), sr.getrandbits(8) < 256)
try:
    sr.getstate()
except NotImplementedError as e:
    print("NotImplementedError", e)

# string
print(string.ascii_letters, string.digits, string.hexdigits, string.octdigits, repr(string.whitespace))
print(string.punctuation, len(string.printable))
print(string.capwords("hello   wORLD  foo"), string.capwords("a-b-c", "-"), string.capwords(" x "))
t = string.Template("$who likes ${what}; $$5 for $who")
print(t.substitute(who="tim", what="ham"), t.substitute({"who": "ann", "what": 3}), t.safe_substitute(who="x"))
print(t.get_identifiers(), t.is_valid(), string.Template("bad $ here").is_valid(), t.template)
print(string.Template("$a${b}c$").safe_substitute(a=1), string.Template("${a").safe_substitute(a=1), string.Template("$1x").safe_substitute())
print(string.Template("$a").substitute({"a": 1}, a=2), string.Template("$A $_b9 ${C}").substitute(A=1, _b9=2, C=3))
for bad in ["oops $", "line1\nline2 $ x", "${}", "$1"]:
    try:
        string.Template(bad).substitute()
    except ValueError as e:
        print("ValueError", e)
try:
    string.Template("$missing").substitute(other=1)
except KeyError as e:
    print("KeyError", e)


class Pct(string.Template):
    delimiter = "%"


print(Pct("%who costs %%%n").substitute(who="it", n=5), Pct("%{x}y").safe_substitute())


class Dotted(string.Template):
    idpattern = r"[a-z][_a-z0-9.]*"


print(Dotted("$a.b and $c").substitute({"a.b": 1, "c": 2}), Dotted.pattern.pattern == Dotted.pattern.pattern)


class Braces(string.Template):
    pattern = r"\{\{(?:(?P<escaped>\{)|(?P<named>[a-z]+)\}\}|(?P<braced>)(?!)|(?P<invalid>))"
    delimiter = "{{"


print(Braces("{{x}} and {{y}}").substitute(x=1, y=2))
f = string.Formatter()
print(f.format("{0} {1!r} {name:>6} {0[1]}", "ab", "c", name="n"), f.format("{} {} {:^{}}", 1, 2, "z", 5))
print(f.format("{a.real} {b[k]} {{lit}}", a=3, b={"k": "v"}), f.vformat("{x}-{y}", (), {"x": 1, "y": 2}))
for bad in ["{0} {}", "{} {0}", "{", "}", "{0!x}", "{a..b}"]:
    try:
        f.format(bad, 1, a=1)
    except (ValueError, IndexError, KeyError) as e:
        print(type(e).__name__, e)
print(list(f.parse("a{0!r:>5}b{{c}}")), list(_string.formatter_parser("x{a.b[c]!r:>{w}}y")))
first, rest = _string.formatter_field_name_split("a.b[0][key]")
print(first, list(rest), _string.formatter_field_name_split("12")[0] + 1)
