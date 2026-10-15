# re: match/search/fullmatch/finditer/sub/split of many patterns against CPython
import re

CASES = [
    ("abc", "xabcy"), ("a.c", "a\nc abc"), ("a.c", "a\nc"), ("^abc$", "abc"), ("^abc$", "abc\n"), ("abc$", "abc\nabc\n"),
    ("a*", "aaa"), ("a*?", "aaa"), ("a+?b", "aaab"), ("a{2,3}", "aaaa"), ("a{2,3}?", "aaaa"), ("a{,2}", "aaa"),
    ("a{2}", "aaa"), ("a{2,}", "aaaaa"), ("a{}", "a{}"), ("a{1", "a{1"), ("a{1,x}", "a{1,x}"), ("x{3,1", "x{3,1"),
    ("[abc]+", "xxbcay"), ("[^abc]+", "abxyzc"), ("[a-z0-9_]+", "Hello_World42!"), ("[]a]+", "a]]b"), ("[^]]+", "ab]"),
    ("[a-]+", "a-a-b"), ("[-a]+", "-a-b"), ("[\\d\\s]+", "x12 3y"), ("[\\w.-]+@[\\w.-]+", "mail me: joe.b@ex-ample.com!"),
    ("\\d+", "abc 123 def 45"), ("\\D+", "123abc456"), ("\\s+", "a \t\nb"), ("\\S+", "  ab  "), ("\\w+", "  héllo wörld "),
    ("\\W+", "ab, cd"), ("\\bfoo\\b", "a foo b"), ("\\bfoo\\b", "afoob"), ("\\Bfoo\\B", "afoob"), ("\\B", ""), ("\\b", ""),
    ("(a)(b)(c)", "abc"), ("(a|b)*", "abab"), ("(a|ab)(c|bcd)(d*)", "abcd"), ("(?:ab)+", "ababab"), ("(a*)*", "b"),
    ("(a*)+", "b"), ("(a|)*b", "aab"), ("(?:(a)|b)*", "ab"), ("((a)|b)+", "ab"), ("(a)|(b)", "b"),
    ("(?P<first>\\w+) (?P<last>\\w+)", "Jane Doe"), ("(?P<x>a)(?P=x)", "aa"), ("(a)\\1", "aa"), ("(a)\\1", "ab"),
    ("(?i)hello", "HeLLo"), ("(?i:a)b", "Ab AB"), ("(?i)[a-c]+", "xABCy"), ("(?i)[^a]", "A"), ("(?m)^\\w+$", "ab\ncd"),
    ("(?s)a.b", "a\nb"), ("(?x) a b # comment\n c", "abc"), ("(?x)[ ]a", " a"), ("a(?=b)", "ab ac"), ("a(?!b)", "ab ac"),
    ("(?<=a)b", "ab cb"), ("(?<!a)b", "ab cb"), ("(?<=ab|cd)e", "abe cde xde"), ("(?>a+)b", "aaab"), ("(?>a+)ab", "aaab"),
    ("a++b", "aaab"), ("a*+a", "aaa"), ("(a)?(?(1)b|c)", "ab c"), ("(?P<q>')?x(?(q)')", "'x' x"), ("(?#comment)ab", "ab"),
    ("\\x41\\u0042\\101", "ABA"), ("\\0", "\x00"), ("a|b|c", "xcx"), ("", "abc"), ("x*", "abxd"), ("\\.", "a.b"),
    ("\\$\\d+\\.\\d\\d", "cost $12.50 now"), ("(\\d+)-(\\d+)", "10-20 30-40"), ("^$", ""), ("$", "ab\n"), ("\\Z", "ab\n"),
    ("\\A\\w", "ab"), ("(?m)$", "a\nb\n"), ("[\\]]", "]"), ("[\\\\]", "\\"), ("[.]", "a.b"), ("[*+?]+", "a*+?b"),
    ("(?i)straße", "STRASSE straße"), ("(?a)\\w+", "héllo"), ("é+", "ééé"), ("[à-ÿ]+", "voilà ça"),
    ("(a+)+b", "aaaaaaaaaaaaab"), ("(\\w+)\\s(\\w+)", "hello world foo bar"), ("(?:x|y)*?z", "xyxyz"),
    ("a{0}", "aaa"), ("(a){0}b", "ab"), ("(ab){2}", "ababab"), ("(ab){1,2}?", "ababab"), ("(?:a{2})*", "aaaaa"),
]


def show(fn: str, pat: str, s: str, m: re.Match | None) -> None:
    if m:
        print(fn, repr(pat), repr(s), m, m.groups(), m.lastindex, m.lastgroup, m.groupdict(), m.span())
    else:
        print(fn, repr(pat), repr(s), m)


for pat, s in CASES:
    show("match", pat, s, re.match(pat, s))
    show("search", pat, s, re.search(pat, s))
    show("fullmatch", pat, s, re.fullmatch(pat, s))
    print("findall", repr(pat), [m.group(0) for m in re.finditer(pat, s)], [m.span() for m in re.finditer(pat, s)])
    print("sub", repr(pat), repr(re.sub(pat, "<\\g<0>>", s)), re.subn(pat, "-", s))
    print("split", repr(pat), re.split(pat, s))
