import string
from string import digits, capwords
print(string.ascii_letters, digits, string.punctuation, repr(string.whitespace), len(string.printable))
print(capwords("hello   big world"), string.capwords("a-b-c", "-"), string.hexdigits)
print(sum(1 for ch in "a1b2c3" if ch in string.digits))
