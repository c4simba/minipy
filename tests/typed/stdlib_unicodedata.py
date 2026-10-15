# unicodedata: the Unicode 16 character database (properties, names, normalization)
import unicodedata
import random

for ch in ['A', 'z', '7', 'é', '́', '一', '가', '각', '⅓', 'א', '(', '\U0001F600',
           '\x00', '٠', '½', '￿', '\U000e0001', 'あ', '\U00013460', '\U0001B170', 'Å', 'ﬁ',
           'ཱི', '\U0001d160', '①', '²', '\U00020000', '\U0002f800']:
    print(repr(ch), unicodedata.category(ch), unicodedata.bidirectional(ch), unicodedata.combining(ch),
          unicodedata.mirrored(ch), unicodedata.east_asian_width(ch), unicodedata.decimal(ch, None),
          unicodedata.digit(ch, -1), unicodedata.numeric(ch, -1.0), unicodedata.name(ch, '-'),
          repr(unicodedata.decomposition(ch)))


def mix(acc: int, s: str) -> int:
    for ch in s:
        acc = (acc * 1000003 + ord(ch)) % 2147483647
    return acc


acc = 0
k = 0
for c in range(0, 0x110000, 557):
    ch = chr(c)
    acc = mix(acc, unicodedata.category(ch) + unicodedata.bidirectional(ch) + str(unicodedata.combining(ch))
              + str(unicodedata.mirrored(ch)) + unicodedata.east_asian_width(ch) + str(unicodedata.numeric(ch, -1.0))
              + unicodedata.name(ch, "") + unicodedata.decomposition(ch))
    k += 1
    if k % 200 == 0:
        print("props", format(c, "06X"), acc)
print("props", k, acc)

POOL = ([chr(c) for c in range(0x41, 0x5B)] + ['é', 'Å', 'Å', 'ẛ', 'Ḋ', 'Ḍ', 'ΐ']
        + ['̀', '́', '̂', '̃', '̇', '̈', '̊', '̣', '̧', '̨', '̛',
           'ͅ', 'ְ', '่', 'ི', 'ུ', 'ྀ', '〪', '゙', '゚']
        + ['ᄀ', 'ᄁ', 'ᅡ', 'ᅢ', 'ᆨ', 'ᆩ', '가', '각', '힣']
        + ['ﬁ', '①', '½', 'ｶ', 'ﾞ', '　', 'େ', 'ା', 'ୗ', 'ೆ', 'ೂ',
           '᾿', '῁', 'ϓ', 'Ω', '¨', '΅', 'ẛ̣', '\U0001d15e', '\U000110ab'])
random.seed(16)
for form in ('NFC', 'NFD', 'NFKC', 'NFKD'):
    acc = 0
    same = 0
    for i in range(300):
        s = "".join(random.choice(POOL) for _ in range(random.randint(1, 6)))
        n = unicodedata.normalize(form, s)
        acc = mix(acc, n)
        if unicodedata.is_normalized(form, s):
            same += 1
        if i < 6:
            print(form, [hex(ord(x)) for x in s], [hex(ord(x)) for x in n])
    print(form, acc, same)

print(unicodedata.lookup('LATIN SMALL LETTER A'), unicodedata.lookup('snowman'), unicodedata.lookup('HANGUL SYLLABLE GAG'),
      unicodedata.lookup('CJK UNIFIED IDEOGRAPH-4E01'), unicodedata.lookup('cjk compatibility ideograph-f900'),
      unicodedata.lookup('EGYPTIAN HIEROGLYPH-13460'), unicodedata.unidata_version)
print(unicodedata.normalize('NFKD', 'ﬁﬂ ①②③ Ｈｅｌｌｏ ｶﾞｷﾞ'), unicodedata.normalize('NFC', 'Amélie'),
      ''.join(c for c in unicodedata.normalize('NFD', 'Crème Brûlée à la carte') if not unicodedata.combining(c)))


def err(f: str, s: str) -> None:
    try:
        if f == "name":
            unicodedata.name(s)
        elif f == "decimal":
            unicodedata.decimal(s)
        elif f == "digit":
            unicodedata.digit(s)
        elif f == "numeric":
            unicodedata.numeric(s)
        elif f == "category":
            unicodedata.category(s)
        elif f == "normalize":
            unicodedata.normalize(s, "x")
        else:
            unicodedata.lookup(s)
        print(f, "ok")
    except ValueError as e:
        print(f, "ValueError:", e)
    except TypeError as e:
        print(f, "TypeError:", e)
    except KeyError as e:
        print(f, "KeyError:", e)


err("name", "\x00")
err("decimal", "a")
err("digit", "x")
err("numeric", "q")
err("category", "ab")
err("normalize", "NFX")
err("lookup", "NO SUCH CHARACTER")
err("lookup", "HANGUL SYLLABLE QQQ")
