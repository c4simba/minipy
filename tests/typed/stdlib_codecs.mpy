# codecs: code pages (cp866 of KolibriOS's files, cp1251, koi8-r ...), utf-16/32, error
# handlers and the errors themselves as CPython reports them
import codecs

s = "Привет, мир! é€"
for enc in ["utf-8", "cp866", "cp1251", "koi8-r", "utf-16", "utf-16-be", "utf-32-le", "cp1252", "ascii", "latin-1", "mac-roman"]:
    try:
        b = s.encode(enc)
        print(enc, b.hex()[:40], b.decode(enc) == s)
    except UnicodeEncodeError as e:
        print(enc, "UnicodeEncodeError", e, e.start, e.end)
print(s.encode("cp1252", "replace"), s.encode("ascii", "xmlcharrefreplace")[:30], s.encode("koi8-r", "backslashreplace"))
print(b"\x8f\xe0\xa8\xa2\xa5\xe2".decode("cp866"), str(b"\xcf\xf0", "cp1251"), bytes("Мир", "cp866"), "Мир".encode("IBM866"))
try:
    b"\x98".decode("cp1251")
except UnicodeDecodeError as e:
    print(e, e.start, e.end, e.reason)
try:
    b"abc\xff".decode("utf-8")
except UnicodeDecodeError as e:
    print(e, e.start, e.end, e.encoding)
try:
    "x".encode("nope")
except LookupError as e:
    print("LookupError:", e)
print(b"a\xffb".decode("utf-8", "replace"), b"a\xffb".decode("utf-8", "ignore"), b"a\xffb".decode("utf-8", "backslashreplace"))
print(codecs.lookup("CP866").name, codecs.lookup("UTF8").name, codecs.decode(b"\xe4\xe0", "cp1251"), codecs.encode("да", "koi8-r"))
print(codecs.BOM_UTF8, "😀x".encode("utf-16-le"), b"\xff\xfe=\xd8\x00\xdex\x00".decode("utf-16"), len("😀".encode("utf-32")))
