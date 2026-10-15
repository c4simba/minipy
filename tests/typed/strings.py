# strings: methods, slicing, comparison, formatting
s = "  Hello, World  "
print("[" + s.strip() + "]", "[" + s.lstrip() + "]", "[" + s.rstrip() + "]")
t = s.strip()
print(t.lower(), t.upper(), t.replace("l", "L"), t.replace("World", "There"))
print(t.startswith("Hell"), t.endswith("!"), t.find("o"), t.find("xyz"), t.index("W"))
print(t.split(", "), "a,b,,c".split(","), "  x  y ".split())
print("+".join(["1", "2", "3"]), "".join(["a", "b"]))
print("abc" < "abd", "b" > "abc", "abc" == "abc", "a" != "b", "Z" < "a")
print("123".isdigit(), "12a".isdigit(), "abc".isalpha(), "   ".isspace(), "ABC".isupper(), "abc".islower(), "a1".isalnum())
print("hello"[1:4], "hello"[::2], "hello"[-3:], "hello"[:-3], "hello"[10:], "hello"[-100:2])
print("capitalize me".capitalize(), "x".join("abc"))
word = "banana"
print(word.count("a"), word.count("an"), word.find("na"), word[word.find("n"):])
codes = [ord(ch) for ch in "AZaz09"]
print(codes, "".join([chr(c) for c in codes]))
rev = ""
for ch in "stressed":
    rev = ch + rev
print(rev)
print(repr("it's"), str(3.5), str(True), str([1, 2]), repr(["a", "b"]))
print("tab\there", "new\\nline", 'quote"s')
n = 7
print(f"{n:3d}|{n:<4}|{n:>4}|{n:04d}|{3.14159:.3f}|{2.5:8.2f}|{'x':>3}")
print("%s=%d" % ("x", 5), "%5.1f%%" % 99.5, "%-6s|" % "ab", "%c%c" % (72, 105))
print("%x %X %o" % (255, 255, 8))
multi = "line1\nline2\nline3"
print(multi.split("\n"), len(multi))
print("a" * 3 + "-" * 2, 3 * "ab")
print(str(10 // 3) + "/" + str(10 % 3), int("  42 "), int("-7"), float(" 2.5 "))
empty = ""
print(len(empty), empty == "", not empty, empty + "x", "[" + empty * 5 + "]")
csv = "name,age,city"
fields = csv.split(",")
print(fields[1], len(fields), fields[-1].upper())
print(repr("a\nb\tc"), repr('say "hi"'), repr("both ' and \""), repr("back\\slash"), ["x'y", "\x01"])
