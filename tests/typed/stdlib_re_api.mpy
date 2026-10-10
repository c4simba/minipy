# re: error messages, templates, Match and Pattern API
import re

BAD = ["(", ")", "a)", "(?P<1>a)", "(?P<a>a)(?P<a>b)", "[a", "[z-a]", "*a", "a**", "a{3,1}", "\\q", "(?<x)", "(?Px)",
       "(?P=nope)", "(a)\\2", "\\", "(?<=a+)b", "(?i", "(?z)", "a(?i)b", "x{5}{3}", "(?(2)a|b)", "(?#abc", "[\\d-z]",
       "(?(a)b)", "\\x4", "\\u12", "(a)(?(1)b|c|d)", "(?P<a", "(?P<>a)", "(?-:a)", "(?i-i:a)", "(?a-a:x)", "^*",
       "(?<=(a))\\1", "a\nb(", "\\400", "(?L)a", "(?au)a"]
for p in BAD:
    try:
        re.compile(p)
        print("ok", repr(p))
    except re.error as e:
        print("error", repr(p), "|", e, "|", e.msg, e.pos, e.lineno, e.colno)
    except ValueError as e:
        print("value error", repr(p), e)

print(re.sub(r"(\w+) (\w+)", r"\2 \1", "hello world, foo bar"))
print(re.sub(r"(?P<a>\d)", r"[\g<a>\g<1>\g<0>]", "a1b22"))
print(re.sub(r"x", r"\n\t\\", "axb"), repr(re.sub("x", "\\-", "axb")))
print(re.sub(r"(a)|b", r"<\1>", "ab"))
print(re.sub(r"\d", lambda m: str(int(m.group()) * 2), "a1b2c9"))
print(re.sub("a", "b", "aaaa", count=2), re.subn("a", "b", "aaaa", count=3))
for t in ["\\g<2>", "\\g<x>", "\\g<", "\\q", "\\9"]:
    try:
        print(re.sub("(a)", t, "a"))
    except re.error as e:
        print("error", e)
    except IndexError as e:
        print("IndexError", e)
print(re.findall(r"(\w)(\d)?", "a1 b c3"), re.findall(r"(\d+)", "1 22 333"), re.findall(r"\d", "a1b2"))
print(re.split(r"(,)|(;)", "a,b;c"), re.split(r",", "a,b,c,d", maxsplit=2), re.split("x*", "axbc"))
print(re.split(r"\W+", "Words, words, words."), re.split(r"(\W+)", "Words, words, words."))
m = re.search(r"(?P<y>\d{4})-(?P<m>\d\d)-(?P<d>\d\d)", "on 2024-05-17 ok")
print(m.group("y"), m["m"], m[3], m.start("m"), m.end(2), m.span("d"), m.regs)
print(m.expand(r"\3/\2/\1 \g<y>"), m.string, m.re, m.pos, m.endpos, m.lastindex, m.lastgroup)
try:
    m.group(4)
except IndexError as e:
    print("IndexError", e)
try:
    m.group("zz")
except IndexError as e:
    print("IndexError", e)
pp = re.compile(r"\d+", re.I | re.M)
print(pp, pp.pattern, pp.flags, pp.groups, pp.groupindex, re.compile("(?P<a>x)(y)").groupindex)
print(re.compile("a", re.A), re.compile("a", re.S | re.X), re.compile("(?i)a"), re.compile("(?i)a").flags)
print(pp.match("12ab"), pp.match("ab12"), pp.search("ab12", 3), pp.search("ab12345", 0, 4), pp.fullmatch("123"))
print(pp.findall("1 2 33", 2), list(pp.finditer("1 22 333", 1, 6)))
print(re.escape("a.b*c?d+e(f)[g]{h}|i^j$k\\l m-n#o&p~q"), re.escape("héllo_wörld123"))
print(re.compile("a") is re.compile("a"), re.compile("a") == re.compile("a"), re.compile("a") == re.compile("b"))
print(re.match("(a)(b)?", "a").groups("-"), re.match("(?P<a>a)(?P<b>b)?", "a").groupdict("-"))
print(re.search("^b", "ab", re.M), re.search("^b", "a\nb", re.M), re.match("a.b", "A\nB", re.I | re.S))
print(re.fullmatch("a|ab", "ab"), re.fullmatch("a*?", "aaa"), re.fullmatch(r"(\d+)\.(\d+)", "3.14").groups())
print(re.search("(?<=\\$)\\d+", "cost: $42"), re.search("(?<!\\$)\\b\\d+", "$42 17"))
print(repr(re.match("a" * 30, "a" * 60)), re.compile("x" * 300).pattern == "x" * 300)
print(re.match(r"\w+", "日本語 text"), re.match(r"\d+", "١٢٣"), re.search(r"\s", "a b"))
