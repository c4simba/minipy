"""More codecs for codecs: the single-byte code pages (cp866 of KolibriOS's text
files and names, cp1251, cp1252, koi8-r, iso8859-x, mac-roman ...), utf-16 and
utf-32. Importing it registers them with codecs."""
import sys
import codecs
from codecs import _check_errors, _bad_decode, _bad_encode

# the upper halves (bytes 0x80-0xff) of the code pages; U+FFFE: not a character there
_TABLES: dict[str, str] = {
    'cp437': '\xc7\xfc\xe9\xe2\xe4\xe0\xe5\xe7\xea\xeb\xe8\xef\xee\xec\xc4\xc5\xc9\xe6\xc6\xf4\xf6\xf2\xfb\xf9\xff\xd6\xdc\xa2\xa3\xa5\u20a7\u0192\xe1\xed\xf3\xfa\xf1\xd1\xaa\xba\xbf\u2310\xac\xbd\xbc\xa1\xab\xbb\u2591\u2592\u2593\u2502\u2524\u2561\u2562\u2556\u2555\u2563\u2551\u2557\u255d\u255c\u255b\u2510\u2514\u2534\u252c\u251c\u2500\u253c\u255e\u255f\u255a\u2554\u2569\u2566\u2560\u2550\u256c\u2567\u2568\u2564\u2565\u2559\u2558\u2552\u2553\u256b\u256a\u2518\u250c\u2588\u2584\u258c\u2590\u2580\u03b1\xdf\u0393\u03c0\u03a3\u03c3\xb5\u03c4\u03a6\u0398\u03a9\u03b4\u221e\u03c6\u03b5\u2229\u2261\xb1\u2265\u2264\u2320\u2321\xf7\u2248\xb0\u2219\xb7\u221a\u207f\xb2\u25a0\xa0',
    'cp850': '\xc7\xfc\xe9\xe2\xe4\xe0\xe5\xe7\xea\xeb\xe8\xef\xee\xec\xc4\xc5\xc9\xe6\xc6\xf4\xf6\xf2\xfb\xf9\xff\xd6\xdc\xf8\xa3\xd8\xd7\u0192\xe1\xed\xf3\xfa\xf1\xd1\xaa\xba\xbf\xae\xac\xbd\xbc\xa1\xab\xbb\u2591\u2592\u2593\u2502\u2524\xc1\xc2\xc0\xa9\u2563\u2551\u2557\u255d\xa2\xa5\u2510\u2514\u2534\u252c\u251c\u2500\u253c\xe3\xc3\u255a\u2554\u2569\u2566\u2560\u2550\u256c\xa4\xf0\xd0\xca\xcb\xc8\u0131\xcd\xce\xcf\u2518\u250c\u2588\u2584\xa6\xcc\u2580\xd3\xdf\xd4\xd2\xf5\xd5\xb5\xfe\xde\xda\xdb\xd9\xfd\xdd\xaf\xb4\xad\xb1\u2017\xbe\xb6\xa7\xf7\xb8\xb0\xa8\xb7\xb9\xb3\xb2\u25a0\xa0',
    'cp866': '\u0410\u0411\u0412\u0413\u0414\u0415\u0416\u0417\u0418\u0419\u041a\u041b\u041c\u041d\u041e\u041f\u0420\u0421\u0422\u0423\u0424\u0425\u0426\u0427\u0428\u0429\u042a\u042b\u042c\u042d\u042e\u042f\u0430\u0431\u0432\u0433\u0434\u0435\u0436\u0437\u0438\u0439\u043a\u043b\u043c\u043d\u043e\u043f\u2591\u2592\u2593\u2502\u2524\u2561\u2562\u2556\u2555\u2563\u2551\u2557\u255d\u255c\u255b\u2510\u2514\u2534\u252c\u251c\u2500\u253c\u255e\u255f\u255a\u2554\u2569\u2566\u2560\u2550\u256c\u2567\u2568\u2564\u2565\u2559\u2558\u2552\u2553\u256b\u256a\u2518\u250c\u2588\u2584\u258c\u2590\u2580\u0440\u0441\u0442\u0443\u0444\u0445\u0446\u0447\u0448\u0449\u044a\u044b\u044c\u044d\u044e\u044f\u0401\u0451\u0404\u0454\u0407\u0457\u040e\u045e\xb0\u2219\xb7\u221a\u2116\xa4\u25a0\xa0',
    'cp1250': '\u20ac\ufffe\u201a\ufffe\u201e\u2026\u2020\u2021\ufffe\u2030\u0160\u2039\u015a\u0164\u017d\u0179\ufffe\u2018\u2019\u201c\u201d\u2022\u2013\u2014\ufffe\u2122\u0161\u203a\u015b\u0165\u017e\u017a\xa0\u02c7\u02d8\u0141\xa4\u0104\xa6\xa7\xa8\xa9\u015e\xab\xac\xad\xae\u017b\xb0\xb1\u02db\u0142\xb4\xb5\xb6\xb7\xb8\u0105\u015f\xbb\u013d\u02dd\u013e\u017c\u0154\xc1\xc2\u0102\xc4\u0139\u0106\xc7\u010c\xc9\u0118\xcb\u011a\xcd\xce\u010e\u0110\u0143\u0147\xd3\xd4\u0150\xd6\xd7\u0158\u016e\xda\u0170\xdc\xdd\u0162\xdf\u0155\xe1\xe2\u0103\xe4\u013a\u0107\xe7\u010d\xe9\u0119\xeb\u011b\xed\xee\u010f\u0111\u0144\u0148\xf3\xf4\u0151\xf6\xf7\u0159\u016f\xfa\u0171\xfc\xfd\u0163\u02d9',
    'cp1251': '\u0402\u0403\u201a\u0453\u201e\u2026\u2020\u2021\u20ac\u2030\u0409\u2039\u040a\u040c\u040b\u040f\u0452\u2018\u2019\u201c\u201d\u2022\u2013\u2014\ufffe\u2122\u0459\u203a\u045a\u045c\u045b\u045f\xa0\u040e\u045e\u0408\xa4\u0490\xa6\xa7\u0401\xa9\u0404\xab\xac\xad\xae\u0407\xb0\xb1\u0406\u0456\u0491\xb5\xb6\xb7\u0451\u2116\u0454\xbb\u0458\u0405\u0455\u0457\u0410\u0411\u0412\u0413\u0414\u0415\u0416\u0417\u0418\u0419\u041a\u041b\u041c\u041d\u041e\u041f\u0420\u0421\u0422\u0423\u0424\u0425\u0426\u0427\u0428\u0429\u042a\u042b\u042c\u042d\u042e\u042f\u0430\u0431\u0432\u0433\u0434\u0435\u0436\u0437\u0438\u0439\u043a\u043b\u043c\u043d\u043e\u043f\u0440\u0441\u0442\u0443\u0444\u0445\u0446\u0447\u0448\u0449\u044a\u044b\u044c\u044d\u044e\u044f',
    'cp1252': '\u20ac\ufffe\u201a\u0192\u201e\u2026\u2020\u2021\u02c6\u2030\u0160\u2039\u0152\ufffe\u017d\ufffe\ufffe\u2018\u2019\u201c\u201d\u2022\u2013\u2014\u02dc\u2122\u0161\u203a\u0153\ufffe\u017e\u0178\xa0\xa1\xa2\xa3\xa4\xa5\xa6\xa7\xa8\xa9\xaa\xab\xac\xad\xae\xaf\xb0\xb1\xb2\xb3\xb4\xb5\xb6\xb7\xb8\xb9\xba\xbb\xbc\xbd\xbe\xbf\xc0\xc1\xc2\xc3\xc4\xc5\xc6\xc7\xc8\xc9\xca\xcb\xcc\xcd\xce\xcf\xd0\xd1\xd2\xd3\xd4\xd5\xd6\xd7\xd8\xd9\xda\xdb\xdc\xdd\xde\xdf\xe0\xe1\xe2\xe3\xe4\xe5\xe6\xe7\xe8\xe9\xea\xeb\xec\xed\xee\xef\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9\xfa\xfb\xfc\xfd\xfe\xff',
    'koi8-r': '\u2500\u2502\u250c\u2510\u2514\u2518\u251c\u2524\u252c\u2534\u253c\u2580\u2584\u2588\u258c\u2590\u2591\u2592\u2593\u2320\u25a0\u2219\u221a\u2248\u2264\u2265\xa0\u2321\xb0\xb2\xb7\xf7\u2550\u2551\u2552\u0451\u2553\u2554\u2555\u2556\u2557\u2558\u2559\u255a\u255b\u255c\u255d\u255e\u255f\u2560\u2561\u0401\u2562\u2563\u2564\u2565\u2566\u2567\u2568\u2569\u256a\u256b\u256c\xa9\u044e\u0430\u0431\u0446\u0434\u0435\u0444\u0433\u0445\u0438\u0439\u043a\u043b\u043c\u043d\u043e\u043f\u044f\u0440\u0441\u0442\u0443\u0436\u0432\u044c\u044b\u0437\u0448\u044d\u0449\u0447\u044a\u042e\u0410\u0411\u0426\u0414\u0415\u0424\u0413\u0425\u0418\u0419\u041a\u041b\u041c\u041d\u041e\u041f\u042f\u0420\u0421\u0422\u0423\u0416\u0412\u042c\u042b\u0417\u0428\u042d\u0429\u0427\u042a',
    'koi8-u': '\u2500\u2502\u250c\u2510\u2514\u2518\u251c\u2524\u252c\u2534\u253c\u2580\u2584\u2588\u258c\u2590\u2591\u2592\u2593\u2320\u25a0\u2219\u221a\u2248\u2264\u2265\xa0\u2321\xb0\xb2\xb7\xf7\u2550\u2551\u2552\u0451\u0454\u2554\u0456\u0457\u2557\u2558\u2559\u255a\u255b\u0491\u255d\u255e\u255f\u2560\u2561\u0401\u0404\u2563\u0406\u0407\u2566\u2567\u2568\u2569\u256a\u0490\u256c\xa9\u044e\u0430\u0431\u0446\u0434\u0435\u0444\u0433\u0445\u0438\u0439\u043a\u043b\u043c\u043d\u043e\u043f\u044f\u0440\u0441\u0442\u0443\u0436\u0432\u044c\u044b\u0437\u0448\u044d\u0449\u0447\u044a\u042e\u0410\u0411\u0426\u0414\u0415\u0424\u0413\u0425\u0418\u0419\u041a\u041b\u041c\u041d\u041e\u041f\u042f\u0420\u0421\u0422\u0423\u0416\u0412\u042c\u042b\u0417\u0428\u042d\u0429\u0427\u042a',
    'iso8859-2': '\x80\x81\x82\x83\x84\x85\x86\x87\x88\x89\x8a\x8b\x8c\x8d\x8e\x8f\x90\x91\x92\x93\x94\x95\x96\x97\x98\x99\x9a\x9b\x9c\x9d\x9e\x9f\xa0\u0104\u02d8\u0141\xa4\u013d\u015a\xa7\xa8\u0160\u015e\u0164\u0179\xad\u017d\u017b\xb0\u0105\u02db\u0142\xb4\u013e\u015b\u02c7\xb8\u0161\u015f\u0165\u017a\u02dd\u017e\u017c\u0154\xc1\xc2\u0102\xc4\u0139\u0106\xc7\u010c\xc9\u0118\xcb\u011a\xcd\xce\u010e\u0110\u0143\u0147\xd3\xd4\u0150\xd6\xd7\u0158\u016e\xda\u0170\xdc\xdd\u0162\xdf\u0155\xe1\xe2\u0103\xe4\u013a\u0107\xe7\u010d\xe9\u0119\xeb\u011b\xed\xee\u010f\u0111\u0144\u0148\xf3\xf4\u0151\xf6\xf7\u0159\u016f\xfa\u0171\xfc\xfd\u0163\u02d9',
    'iso8859-5': '\x80\x81\x82\x83\x84\x85\x86\x87\x88\x89\x8a\x8b\x8c\x8d\x8e\x8f\x90\x91\x92\x93\x94\x95\x96\x97\x98\x99\x9a\x9b\x9c\x9d\x9e\x9f\xa0\u0401\u0402\u0403\u0404\u0405\u0406\u0407\u0408\u0409\u040a\u040b\u040c\xad\u040e\u040f\u0410\u0411\u0412\u0413\u0414\u0415\u0416\u0417\u0418\u0419\u041a\u041b\u041c\u041d\u041e\u041f\u0420\u0421\u0422\u0423\u0424\u0425\u0426\u0427\u0428\u0429\u042a\u042b\u042c\u042d\u042e\u042f\u0430\u0431\u0432\u0433\u0434\u0435\u0436\u0437\u0438\u0439\u043a\u043b\u043c\u043d\u043e\u043f\u0440\u0441\u0442\u0443\u0444\u0445\u0446\u0447\u0448\u0449\u044a\u044b\u044c\u044d\u044e\u044f\u2116\u0451\u0452\u0453\u0454\u0455\u0456\u0457\u0458\u0459\u045a\u045b\u045c\xa7\u045e\u045f',
    'iso8859-15': '\x80\x81\x82\x83\x84\x85\x86\x87\x88\x89\x8a\x8b\x8c\x8d\x8e\x8f\x90\x91\x92\x93\x94\x95\x96\x97\x98\x99\x9a\x9b\x9c\x9d\x9e\x9f\xa0\xa1\xa2\xa3\u20ac\xa5\u0160\xa7\u0161\xa9\xaa\xab\xac\xad\xae\xaf\xb0\xb1\xb2\xb3\u017d\xb5\xb6\xb7\u017e\xb9\xba\xbb\u0152\u0153\u0178\xbf\xc0\xc1\xc2\xc3\xc4\xc5\xc6\xc7\xc8\xc9\xca\xcb\xcc\xcd\xce\xcf\xd0\xd1\xd2\xd3\xd4\xd5\xd6\xd7\xd8\xd9\xda\xdb\xdc\xdd\xde\xdf\xe0\xe1\xe2\xe3\xe4\xe5\xe6\xe7\xe8\xe9\xea\xeb\xec\xed\xee\xef\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9\xfa\xfb\xfc\xfd\xfe\xff',
    'mac-roman': '\xc4\xc5\xc7\xc9\xd1\xd6\xdc\xe1\xe0\xe2\xe4\xe3\xe5\xe7\xe9\xe8\xea\xeb\xed\xec\xee\xef\xf1\xf3\xf2\xf4\xf6\xf5\xfa\xf9\xfb\xfc\u2020\xb0\xa2\xa3\xa7\u2022\xb6\xdf\xae\xa9\u2122\xb4\xa8\u2260\xc6\xd8\u221e\xb1\u2264\u2265\xa5\xb5\u2202\u2211\u220f\u03c0\u222b\xaa\xba\u03a9\xe6\xf8\xbf\xa1\xac\u221a\u0192\u2248\u2206\xab\xbb\u2026\xa0\xc0\xc3\xd5\u0152\u0153\u2013\u2014\u201c\u201d\u2018\u2019\xf7\u25ca\xff\u0178\u2044\u20ac\u2039\u203a\ufb01\ufb02\u2021\xb7\u201a\u201e\u2030\xc2\xca\xc1\xcb\xc8\xcd\xce\xcf\xcc\xd3\xd4\uf8ff\xd2\xda\xdb\xd9\u0131\u02c6\u02dc\xaf\u02d8\u02d9\u02da\xb8\u02dd\u02db\u02c7',
}

# alias=codec, space separated (a dict when first needed)
_ALIASES_TEXT = "437=cp437 ibm437=cp437 850=cp850 ibm850=cp850 866=cp866 ibm866=cp866 windows-1250=cp1250 windows-1251=cp1251 windows-1252=cp1252 iso-8859-2=iso8859-2 latin2=iso8859-2 l2=iso8859-2 iso-8859-5=iso8859-5 cyrillic=iso8859-5 iso-8859-15=iso8859-15 latin9=iso8859-15 l9=iso8859-15 macroman=mac-roman utf-8=utf-8 utf8=utf-8 u8=utf-8 utf=utf-8 cp65001=utf-8 utf-8-sig=utf-8-sig utf8-sig=utf-8-sig ascii=ascii us-ascii=ascii 646=ascii latin-1=latin-1 latin1=latin-1 iso-8859-1=latin-1 iso8859-1=latin-1 8859=latin-1 l1=latin-1 latin=latin-1 cp819=latin-1 utf-16=utf-16 utf16=utf-16 utf-16-le=utf-16-le utf-16le=utf-16-le utf-16-be=utf-16-be utf-16be=utf-16-be utf-32=utf-32 utf32=utf-32 utf-32-le=utf-32-le utf-32le=utf-32-le utf-32-be=utf-32-be utf-32be=utf-32-be raw-unicode-escape=raw-unicode-escape unicode-escape=unicode-escape"
_ALIASES: dict[str, str] = {}

_encoders: dict[str, dict[str, int]] = {}


def lookup_name(encoding: str) -> str:
    """The codec's name (as CPython reports it in errors), or LookupError."""
    if not _ALIASES:
        for item in _ALIASES_TEXT.split(" "):
            i = item.find("=")
            _ALIASES[item[:i]] = item[i + 1:]
    e = codecs._ascii_lower(encoding)
    if e in _TABLES:
        return e
    name = _ALIASES.get(e)
    if name is None or name in ("utf-8", "utf-8-sig", "ascii", "latin-1"):
        raise LookupError("unknown encoding: " + encoding)
    return name


# ---------------------------------------------------------------- the code pages

def _decode_table(data: bytes, errors: str, name: str) -> str:
    table = _TABLES[name]
    out: list[str] = []
    start = 0
    for i in range(len(data)):
        b = data[i]
        if b < 0x80:
            continue
        if i > start:
            out.append(data[start:i].decode("ascii"))
        ch = table[b - 0x80]
        if ch == "\ufffe":
            _bad_decode(errors, "charmap", data, i, i + 1, "character maps to <undefined>", out)
        else:
            out.append(ch)
        start = i + 1
    if start < len(data):
        out.append(data[start:].decode("ascii"))
    return "".join(out)


def _encode_table(s: str, errors: str, name: str) -> bytes:
    enc = _encoders.get(name)
    if enc is None:
        enc = {}
        table = _TABLES[name]
        for i in range(128):
            if table[i] != "\ufffe":
                enc[table[i]] = 0x80 + i
        _encoders[name] = enc
    out: list[bytes] = []
    raw: list[int] = []
    i = 0
    n = len(s)
    while i < n:
        ch = s[i]
        c = ord(ch)
        if c < 0x80:
            raw.append(c)
            i += 1
            continue
        b = enc.get(ch)
        if b is not None:
            raw.append(b)
            i += 1
            continue
        j = i + 1
        while j < n and ord(s[j]) >= 0x80 and enc.get(s[j]) is None:
            j += 1
        out.append(bytes(raw))
        raw = []
        _bad_encode(errors, "charmap", s, i, j, "character maps to <undefined>", out)
        i = j
    out.append(bytes(raw))
    return b"".join(out)


# ---------------------------------------------------------------- utf-16, utf-32

def _decode_utf16(data: bytes, errors: str, name: str) -> str:
    big = name == "utf-16-be"
    i = 0
    if name == "utf-16" and len(data) >= 2:
        if data[0] == 0xFF and data[1] == 0xFE:
            i = 2
        elif data[0] == 0xFE and data[1] == 0xFF:
            i = 2
            big = True
    name = "utf-16-be" if big else "utf-16-le"    # (errors name the byte order used)
    out: list[str] = []
    n = len(data)
    while i + 1 < n:
        if big:
            u = data[i] * 256 + data[i + 1]
        else:
            u = data[i] + data[i + 1] * 256
        if u < 0xD800 or u > 0xDFFF:
            out.append(chr(u))
            i += 2
            continue
        if u >= 0xDC00:
            _bad_decode(errors, name, data, i, i + 2, "illegal encoding", out)
            i += 2
            continue
        if i + 3 >= n:
            _bad_decode(errors, name, data, i, n, "unexpected end of data", out)
            i = n
            break
        if big:
            v = data[i + 2] * 256 + data[i + 3]
        else:
            v = data[i + 2] + data[i + 3] * 256
        if v < 0xDC00 or v > 0xDFFF:
            _bad_decode(errors, name, data, i, i + 2, "illegal UTF-16 surrogate", out)
            i += 2
            continue
        out.append(chr(0x10000 + ((u - 0xD800) << 10) + (v - 0xDC00)))
        i += 4
    if i < n:
        _bad_decode(errors, name, data, i, n, "truncated data", out)
    return "".join(out)


def _encode_utf16(s: str, name: str) -> bytes:
    big = name == "utf-16-be"
    raw: list[int] = []
    if name == "utf-16":
        raw.append(0xFF)
        raw.append(0xFE)
    for ch in s:
        c = ord(ch)
        units = [c]
        if c >= 0x10000:
            c -= 0x10000
            units = [0xD800 + (c >> 10), 0xDC00 + (c & 0x3FF)]
        for u in units:
            if big:
                raw.append(u >> 8)
                raw.append(u & 255)
            else:
                raw.append(u & 255)
                raw.append(u >> 8)
    return bytes(raw)


def _decode_utf32(data: bytes, errors: str, name: str) -> str:
    big = name == "utf-32-be"
    i = 0
    if name == "utf-32" and len(data) >= 4:
        if data[0] == 0xFF and data[1] == 0xFE and data[2] == 0 and data[3] == 0:
            i = 4
        elif data[0] == 0 and data[1] == 0 and data[2] == 0xFE and data[3] == 0xFF:
            i = 4
            big = True
    name = "utf-32-be" if big else "utf-32-le"
    out: list[str] = []
    n = len(data)
    while i + 3 < n:
        if big:
            c = ((data[i] * 256 + data[i + 1]) * 256 + data[i + 2]) * 256 + data[i + 3]
        else:
            c = ((data[i + 3] * 256 + data[i + 2]) * 256 + data[i + 1]) * 256 + data[i]
        if c > 0x10FFFF or (c >= 0xD800 and c <= 0xDFFF):
            _bad_decode(errors, name, data, i, i + 4, "code point not in range(0x110000)" if c > 0x10FFFF else "code point in surrogate code point range(0xd800, 0xe000)", out)
        else:
            out.append(chr(c))
        i += 4
    if i < n:
        _bad_decode(errors, name, data, i, n, "truncated data", out)
    return "".join(out)


def _encode_utf32(s: str, name: str) -> bytes:
    big = name == "utf-32-be"
    raw: list[int] = []
    if name == "utf-32":
        raw += [0xFF, 0xFE, 0, 0]
    for ch in s:
        c = ord(ch)
        if big:
            raw += [c >> 24, (c >> 16) & 255, (c >> 8) & 255, c & 255]
        else:
            raw += [c & 255, (c >> 8) & 255, (c >> 16) & 255, c >> 24]
    return bytes(raw)


# ---------------------------------------------------------------- unicode-escape, raw-unicode-escape

def _encode_raw_escape(s: str) -> bytes:
    out: list[bytes] = []
    for ch in s:
        c = ord(ch)
        if c < 0x100:
            out.append(bytes([c]))
        elif c < 0x10000:
            out.append(("\\u" + format(c, "04x")).encode("ascii"))
        else:
            out.append(("\\U" + format(c, "08x")).encode("ascii"))
    return b"".join(out)


def _encode_escape(s: str) -> bytes:
    out: list[str] = []
    for ch in s:
        c = ord(ch)
        if ch == "\\":
            out.append("\\\\")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif c < 0x20 or 0x7F <= c < 0x100:
            out.append("\\x" + format(c, "02x"))
        elif c < 0x7F:
            out.append(ch)
        elif c < 0x10000:
            out.append("\\u" + format(c, "04x"))
        else:
            out.append("\\U" + format(c, "08x"))
    return "".join(out).encode("ascii")


_HEXDIGITS = b"0123456789abcdefABCDEF"


def _hex_run(data: bytes, i: int, width: int) -> int:
    """How many hex digits (at most width) start at data[i]."""
    k = 0
    while k < width and i + k < len(data) and data[i + k] in _HEXDIGITS:
        k += 1
    return k


def _escape_value(data: bytes, errors: str, codec: str, i: int, width: int, out: list[str]) -> int:
    """The \\x / \\u / \\U escape at data[i] (its letter): its character into out; where it ends."""
    k = _hex_run(data, i + 1, width)
    letter = chr(data[i])
    if k < width:
        what = "xXX" if letter == "x" else "uXXXX" if letter == "u" else "UXXXXXXXX"
        _bad_decode(errors, codec, data, i - 1, i + 1 + k, "truncated \\" + what + " escape", out)
        return i + 1 + k
    c = int(data[i + 1:i + 1 + width].decode("ascii"), 16)
    if c > 0x10FFFF:
        _bad_decode(errors, codec, data, i - 1, i + 1 + width,
                    "\\Uxxxxxxxx out of range" if codec == "rawunicodeescape" else "illegal Unicode character", out)
    else:
        out.append(chr(c))
    return i + 1 + width


def _decode_raw_escape(data: bytes, errors: str) -> str:
    out: list[str] = []
    i = 0
    n = len(data)
    while i < n:
        b = data[i]
        if b != 0x5C:
            out.append(chr(b))
            i += 1
            continue
        j = i
        while j < n and data[j] == 0x5C:
            j += 1
        nb = j - i
        if j < n and nb % 2 == 1 and (data[j] == 0x75 or data[j] == 0x55):
            out.append("\\" * (nb - 1))
            i = _escape_value(data, errors, "rawunicodeescape", j, 4 if data[j] == 0x75 else 8, out)
        else:
            out.append("\\" * nb)
            i = j
    return "".join(out)


def _lookup_name(name: str) -> str:
    """The character named name ("" when none; compiled programs: \\N{...} is not known)."""
    if not sys._compiled:
        import unicodedata
        try:
            return unicodedata.lookup(name)
        except KeyError:
            return ""
    return ""


_SIMPLE = {0x5C: "\\", 0x27: "'", 0x22: '"', 0x61: "\a", 0x62: "\b", 0x66: "\f", 0x6E: "\n", 0x72: "\r",
           0x74: "\t", 0x76: "\v", 0x0A: ""}


def _decode_escape(data: bytes, errors: str) -> str:
    out: list[str] = []
    i = 0
    n = len(data)
    while i < n:
        b = data[i]
        if b != 0x5C:
            out.append(chr(b))
            i += 1
            continue
        if i + 1 >= n:
            _bad_decode(errors, "unicodeescape", data, i, n, "\\ at end of string", out)
            break
        c = data[i + 1]
        if c in _SIMPLE:
            out.append(_SIMPLE[c])
            i += 2
        elif 0x30 <= c <= 0x37:
            j = i + 1
            v = 0
            while j < n and j < i + 4 and 0x30 <= data[j] <= 0x37:
                v = v * 8 + data[j] - 0x30
                j += 1
            out.append(chr(v))
            i = j
        elif c == 0x78:
            i = _escape_value(data, errors, "unicodeescape", i + 1, 2, out)
        elif c == 0x75 or c == 0x55:
            i = _escape_value(data, errors, "unicodeescape", i + 1, 4 if c == 0x75 else 8, out)
        elif c == 0x4E and i + 2 < n and data[i + 2] == 0x7B:
            end = data.find(b"}", i + 3)
            ch = ""
            if end > i + 3:
                ch = _lookup_name(data[i + 3:end].decode("ascii", "replace"))
            if not ch:
                _bad_decode(errors, "unicodeescape", data, i, end + 1 if end >= 0 else n,
                            "unknown Unicode character name" if end >= 0 else "malformed \\N character escape", out)
                i = end + 1 if end >= 0 else n
            else:
                out.append(ch)
                i = end + 1
        else:
            out.append("\\" + chr(c))
            i += 2
    return "".join(out)


# ---------------------------------------------------------------- the entry points

def decode(data: bytes, encoding: str, errors: str) -> str:
    name = lookup_name(encoding)
    _check_errors(errors)
    if name.startswith("utf-16"):
        return _decode_utf16(data, errors, name)
    if name.startswith("utf-32"):
        return _decode_utf32(data, errors, name)
    if name == "raw-unicode-escape":
        return _decode_raw_escape(data, errors)
    if name == "unicode-escape":
        return _decode_escape(data, errors)
    return _decode_table(data, errors, name)


def encode(s: str, encoding: str, errors: str) -> bytes:
    name = lookup_name(encoding)
    _check_errors(errors)
    if name.startswith("utf-16"):
        return _encode_utf16(s, name)
    if name.startswith("utf-32"):
        return _encode_utf32(s, name)
    if name == "raw-unicode-escape":
        return _encode_raw_escape(s)
    if name == "unicode-escape":
        return _encode_escape(s)
    return _encode_table(s, errors, name)


codecs._register(decode, encode, lookup_name)
