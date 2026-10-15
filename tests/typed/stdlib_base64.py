"""binascii and base64 (hex, base64 strict and lenient, base32, base16, base85, Ascii85, Z85)."""
import binascii

print(binascii.hexlify(b"\x00\xffabc"), binascii.hexlify(b"abcdef", "-"), binascii.hexlify(b"abcdef", b":", 2))
print(binascii.b2a_hex(b"xyz"), binascii.unhexlify(b"616263"), binascii.unhexlify("4142"), binascii.a2b_hex("ff00"))
for bad in [b"abc", b"zz", b"6g"]:
    try:
        print(binascii.unhexlify(bad))
    except binascii.Error as e:
        print("Error", e)
for d in [b"", b"a", b"ab", b"abc", b"abcd", bytes(range(256))[:40]]:
    enc = binascii.b2a_base64(d)
    print(enc, binascii.b2a_base64(d, newline=False), binascii.a2b_base64(enc) == d)
for s in [b"YQ==YQ==", b"YQ=Q", b"YWJjZ", b"Y=Q==", b"YQ==YWJj", b"YQ==YW", b"YQ=YQ", b"YQ===Q", b"YWI=YQ==", b"=YQ==", b"YQ", b"YWI", b"Y W\nJj"]:
    try:
        print(s, binascii.a2b_base64(s))
    except binascii.Error as e:
        print(s, "Error", e)
for s in [b"YQ==YQ==", b"=YQ==", b"YQ=Q", b"YQ==\n", b"Y Q==", b"YQ==", b"YWJj", b"YWJj=", b"YQ===", b"YQ==Y", b"YQ=Y", b"Y=", b"YQ=", b"Y=Q", b"YWI=\n", b"\nYQ=="]:
    try:
        print(s, binascii.a2b_base64(s, strict_mode=True))
    except binascii.Error as e:
        print(s, "Error", e)
print(binascii.a2b_base64("YWJj"), binascii.a2b_base64("YW Jj\n"))
try:
    binascii.a2b_base64("é")
except ValueError as e:
    print("ValueError", e)
print(binascii.crc32(b"hello"), binascii.crc32(b" world", binascii.crc32(b"hello")), binascii.crc32(b"hello world"), binascii.crc32(b""))
print(binascii.crc_hqx(b"hello", 0), binascii.crc_hqx(b"123456789", 0xFFFF))
print(issubclass(binascii.Error, ValueError))

import base64

samples = [b"", b"f", b"fo", b"foo", b"foob", b"fooba", b"foobar", b"\x00\x00\x00\x00\x00", b"\xfb\xff\xbf", b"    hello\x00\x00\x00\x00", bytes(range(200))]
for s in samples:
    e64 = base64.b64encode(s)
    print(e64, base64.b64decode(e64) == s, base64.urlsafe_b64encode(s), base64.urlsafe_b64decode(base64.urlsafe_b64encode(s)) == s)
    print(base64.b32encode(s), base64.b32decode(base64.b32encode(s)) == s, base64.b32hexencode(s), base64.b32hexdecode(base64.b32hexencode(s)) == s)
    print(base64.b16encode(s), base64.b16decode(base64.b16encode(s)) == s)
    print(base64.b85encode(s), base64.b85encode(s, pad=True), base64.b85decode(base64.b85encode(s)) == s)
    print(base64.a85encode(s), base64.a85encode(s, foldspaces=True), base64.a85decode(base64.a85encode(s, foldspaces=True), foldspaces=True) == s)
    print(base64.z85encode(s), base64.z85decode(base64.z85encode(s)) == s)
print(base64.b64encode(b"\xfb\xff\xbf", altchars=b"-_"), base64.b64decode(b"-_-_", altchars=b"-_"))
print(base64.b64decode("Zm9vYmFy"), base64.b64decode(b"Zm9v\nYmFy"), base64.standard_b64encode(b"x"), base64.standard_b64decode("eA=="))
try:
    base64.b64decode(b"Zm9v\nYmFy", validate=True)
except binascii.Error as e:
    print("Error", e)
print(base64.encodebytes(bytes(range(100))))
print(base64.decodebytes(base64.encodebytes(bytes(range(100)))) == bytes(range(100)))
print(base64.b32decode("mzxw6===", casefold=True), base64.b32decode("MZXW6YTB", map01="L"))
for bads in ["MZXW6==", "MZXW6Y==", "MZ1W6YTB"]:
    try:
        print(base64.b32decode(bads))
    except binascii.Error as e:
        print("Error", e)
print(base64.b16decode("666f6f", casefold=True))
try:
    base64.b16decode("666f6f")
except binascii.Error as e:
    print("Error", e)
print(base64.a85encode(b"hello world, this is a longer text", wrapcol=10, adobe=True))
print(base64.a85decode(b"<~BOu!rD]j7BEbo80~>", adobe=True))
for badb in [b"abc~", b"hello\x7f"]:
    try:
        print(base64.b85decode(badb))
    except ValueError as e:
        print("ValueError", e)
try:
    base64.a85decode(b"ab!z")
except ValueError as e:
    print("ValueError", e)
