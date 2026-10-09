# unicode strings: code points everywhere
s = "привет, мир"
print(len(s), s[0], s[3], s[-1], s[2:5], s[::-2], s[1:9:3])
print(s.find("мир"), s.index("и"), s.rfind("и"), s.count("и"), s.find("и", 3), s.find("и", 3, 8), s.rfind("и", 0, 5))
print(s.upper(), s.title(), s.capitalize(), s.swapcase(), "ΣΊΣΥΦΟΣ".lower(), "straße".upper(), "straße".casefold())
print(s.startswith("при"), s.endswith(("мир", "x")), s.startswith("рив", 1), s.endswith("ет", 0, 6))
print(s.split(), s.split(", "), s.rsplit("и", 1), s.split("и", 1), " a　b\xa0c ".split(), "a b c d".rsplit(None, 2), "a b c d".split(maxsplit=1))
print(s.partition(", "), s.rpartition("и"), s.partition("zz"), s.rpartition("zz"))
print(s.replace("и", "ii"), s.replace("и", "I", 1), "абв".replace("", "-"), "абв".replace("", "-", 2))
print(s.center(15, "*"), s.ljust(13, "·") + "|", s.rjust(13) + "|", "-42".zfill(6), "ab".center(5), "a".center(4))
print(f"{s:^15}|{s:.3}|{s:>14.4}|{'%.2s' % s}|{s!r:>16}|{'é':é^5}|{-5:=6}|{5:*>+4}")
print(repr("ab\x00 \xa0é\U0001F600\x7f\x85"))
print([c for c in "añ€😀"], list("añ€😀"), sorted("баг"), "".join(reversed("абв")))
print(ord("é"), ord("€"), ord("😀"), chr(233), chr(8364), chr(128512), chr(0x41))
n = 0
for i, c in enumerate("жёлтый"):
    n += ord(c) * (i + 1)
print(n)
for a, b in zip("абв", "αβγ"):
    print(a + b, end=" ")
print()
print("Ǆ".lower(), "ǅ".upper(), "ÀÉÎ".lower(), "àéî".upper(), "ÿ".upper(), "ĳ".upper())
print("αβγ".isalpha(), "١٢٣".isdigit(), "١٢٣".isdecimal(), "½".isnumeric(), "½".isdigit(), "Ⅷ".isnumeric(), "　".isspace())
print("Привет Мир".istitle(), "привет".islower(), "ПРИВЕТ".isupper(), "abc".isascii(), "абв".isascii(), "".isprintable(), "a\xa0".isprintable(), "_ид1".isidentifier(), "1ид".isidentifier())
print("\tа\tбв\n\tг".expandtabs(4), "x\ty".expandtabs())
print("a\nb\r\nc d\x1ce".splitlines(), "a\nb\r\n".splitlines(True), "".splitlines())
print("prefix-тело".removeprefix("prefix-"), "тело.txt".removesuffix(".txt"), "abc".removesuffix(""))
print("  ‐тест　 ".strip(), "xxтестxx".strip("x"), "ёжёж".lstrip("ё"), "€€a€".rstrip("€"))
print("абв" < "абг", "я" > "а", "é" > "z", max("абв"), min(["ж", "а", "я"]))
t = "日本語テキスト"
print(len(t), t[2], t[-2:], t[::2], t.find("テ"), t * 2, len(t * 3))
u = ""
for k in range(5):
    u += chr(0x3b1 + k)
print(u, len(u), u[4], u[1:3])
d = {"ключ": 1, "κλειδί": 2}
print(d, "ключ" in d, "ючк" in "ключ", "z" not in "ключ")
print(str(["é", " "]), "%5s|%-5s|" % ("ё", "ж"), "%c%c" % (1046, 0x1F600))
