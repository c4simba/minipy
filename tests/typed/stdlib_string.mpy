# string: constants, capwords, Template
import string


class Pct(string.Template):
    delimiter = "%"


def main() -> None:
    print(string.ascii_letters, string.digits, string.hexdigits, string.octdigits, repr(string.whitespace))
    print(string.punctuation, len(string.printable))
    print(string.capwords("hello   wORLD  foo"), string.capwords("a-b-c", "-"), string.capwords(" x "))
    t = string.Template("$who likes ${what}; $$5 for $who")
    print(t.substitute(who="tim", what="ham"), t.safe_substitute(who="x"))
    print(t.substitute({"who": "ann", "what": "jam"}))
    print(t.get_identifiers(), t.is_valid(), string.Template("bad $ here").is_valid(), t.template)
    print(string.Template("$a${b}c$").safe_substitute(a=1), string.Template("${a").safe_substitute(a=1))
    print(string.Template("$A $_b9 ${C}").substitute(A=1, _b9=2, C=3), string.Template("$a").substitute({"a": 1}, a=2))
    for bad in ["oops $", "line1\nline2 $ x", "${}", "$1"]:
        try:
            string.Template(bad).substitute(x=1)
        except ValueError as e:
            print("ValueError", e)
    try:
        string.Template("$missing").substitute(other=1)
    except KeyError as e:
        print("KeyError", e)
    print(Pct("%who costs %%%n").substitute(who="it", n="5"), Pct("%{x}y").safe_substitute(z=1))


main()
