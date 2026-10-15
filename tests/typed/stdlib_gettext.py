# gettext: .mo catalogs (written here), plural forms, contexts, fallbacks, the domain functions
import gettext
import os
import shutil
import struct


def write_mo(path: str, entries: dict[bytes, bytes], le: bool = True) -> None:
    keys = sorted(entries)
    ids = b"".join([k + b"\0" for k in keys])
    strs = b"".join([entries[k] + b"\0" for k in keys])
    n = len(keys)
    start = 7 * 4 + 16 * n
    words = [0x950412de, 0, n, 7 * 4, 7 * 4 + n * 8, 0, 0]
    o = 0
    for k in keys:
        words += [len(k), start + o]
        o += len(k) + 1
    o = 0
    for k in keys:
        words += [len(entries[k]), start + len(ids) + o]
        o += len(entries[k]) + 1
    out = b""
    for w in words:
        out += struct.pack("<I", w) if le else struct.pack(">I", w)
    with open(path, "wb") as f:
        f.write(out + ids + strs)


root = "build/typed_gettext"
shutil.rmtree(root, ignore_errors=True)
for lang in ("de", "ru", "fr_FR"):
    os.makedirs(os.path.join(root, lang, "LC_MESSAGES"))

header = (b"Project-Id-Version: test\nContent-Type: text/plain; charset=UTF-8\n"
          b"Plural-Forms: nplurals=2; plural=(n != 1);\n")
write_mo(os.path.join(root, "de", "LC_MESSAGES", "app.mo"), {
    b"": header,
    b"Hello": "Hallo".encode(),
    b"Goodbye": "Auf Wiedersehen".encode(),
    b"file\0files": b"Datei\0Dateien",
    b"menu\x04Open": "Öffnen".encode(),
    b"menu\x04item\0items": b"Eintrag\0Eintr\xc3\xa4ge",
})
ru_header = (b"Content-Type: text/plain; charset=UTF-8\n"
             b"Plural-Forms: nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && "
             b"(n%100<10 || n%100>=20) ? 1 : 2);\n")
write_mo(os.path.join(root, "ru", "LC_MESSAGES", "app.mo"), {
    b"": ru_header,
    b"Hello": "Привет".encode(),
    b"file\0files": "файл\0файла\0файлов".encode(),
}, le=False)
write_mo(os.path.join(root, "fr_FR", "LC_MESSAGES", "app.mo"), {b"": b"Content-Type: text/plain; charset=UTF-8\n",
                                                                 b"Hello": b"Bonjour"})

de = gettext.translation("app", root, ["de"])
print(de.gettext("Hello"), "|", de.gettext("Goodbye"), "|", de.gettext("missing"))
print([de.ngettext("file", "files", n) for n in (0, 1, 2)])
print(de.pgettext("menu", "Open"), de.pgettext("other", "Open"))
print([de.npgettext("menu", "item", "items", n) for n in (1, 5)])
print(de.info()["project-id-version"], de.charset(), sorted(de.info()))
_ = de.gettext
print(_("Hello"), _("Goodbye"))

ru = gettext.translation("app", root, ["ru"])
print(ru.gettext("Hello"))
print([str(n) + " " + ru.ngettext("file", "files", n) for n in (1, 2, 5, 11, 21, 22, 25, 101, 111)])

# a language list: the first catalog, the others its fallbacks
both = gettext.translation("app", root, ["fr_FR", "de"])
print(both.gettext("Hello"), both.gettext("Goodbye"))
print(gettext.find("app", root, ["xx", "de"]), gettext.find("app", root, ["xx"]))

# no catalog
try:
    gettext.translation("app", root, ["xx"])
except FileNotFoundError as e:
    print("FileNotFoundError:", e.errno, e.strerror)
null = gettext.translation("app", root, ["xx"], fallback=True)
print(type(null).__name__, null.gettext("Hello"), null.ngettext("a", "b", 2))
null.add_fallback(de)
print(null.gettext("Hello"))

# the domain functions
print(gettext.textdomain(), gettext.bindtextdomain("app", root))
gettext.textdomain("app")
os.environ["LANGUAGE"] = "de"
print(gettext.gettext("Hello"), gettext.ngettext("file", "files", 3))
print(gettext.dgettext("other", "Hello"), gettext.dngettext("other", "x", "xs", 2))

# plural expressions
for expr in ("n != 1", "n > 1", "n==1 ? 0 : n==2 ? 1 : 2", "(n % 10 == 1) + 2 * (n > 4)", "n / 3", "!n", "n || 7", "n && 3"):
    p = gettext.c2py(expr)
    print(expr, "->", [p(k) for k in (0, 1, 2, 3, 5, 9)])
for bad in ("n +", "x > 1", "(n > 1", "n > 1)", "n ? 1", "1 2", "n >= 1 $"):
    try:
        gettext.c2py(bad)
    except ValueError as e:
        print(repr(bad), "ValueError:", e)

# a bad file
with open(os.path.join(root, "bad.mo"), "wb") as f:
    f.write(b"\0" * 32)
try:
    with open(os.path.join(root, "bad.mo"), "rb") as fb:
        gettext.GNUTranslations(fb)
except OSError as e:
    print("OSError:", e.strerror)
shutil.rmtree(root)
