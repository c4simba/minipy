import zlib
data = b"hello hello hello world, " * 20 + bytes(range(256))
print(zlib.crc32(data), zlib.adler32(data), zlib.crc32(b""), zlib.adler32(b""), zlib.crc32(b"abc", 5), hex(zlib.crc32(b"123456789")))
for lv in (0, 1, 6, 9):
    c = zlib.compress(data, lv)
    print(lv, zlib.decompress(c) == data, len(c) < len(data) or lv == 0)
cp = bytes.fromhex("789ccb48cdc9c957c84022cbf38b72527450844625869504032313330b2b1b3b072717370f2f1fbf80a090b088a898b884a494b48cac9cbc82a292b28aaa9aba86a696b68eae9ebe81a191b189a999b985a595b58dad9dbd83a393b38bab9bbb87a797b78faf9f7f4060507048685878446454744c6c5c7c426252724a6a5a7a466656764e6e5e7e4161517149695979456555754d6d5d7d436353734b6b5b7b476757774f6f5fff848993264f993a6dfa8c99b366cf993b6ffe82858b162f59ba6cf98a95ab56af59bb6efd868d9b366fd9ba6dfb8e9dbb76efd9bb6fff8183870e1f397aecf88993a74e9f397beefc858b972e5fb97aedfa8d9bb76edfb97beffe83878f1e3f79faecf98b97af5ebf79fbeefd878f9f3e7ff9faedfb8f9fbf7efff9fbef3f00bbcb34d0")
print(zlib.decompress(cp) == data)
gz = zlib.compress(data, 6, 31)
print(zlib.decompress(gz, 31) == data, zlib.decompress(gz, 47) == data, gz[:3])
raw = zlib.compress(data, 9, -15)
print(zlib.decompress(raw, -15) == data)
d = zlib.decompressobj()
out = b""
for i in range(0, len(cp), 7):
    out += d.decompress(cp[i:i + 7])
out += d.flush()
print(out == data, d.eof, d.unused_data)
d2 = zlib.decompressobj()
print(d2.decompress(cp + b"tail") == data, d2.unused_data, d2.eof)
co = zlib.compressobj(9)
s = co.compress(b"abc" * 100) + co.compress(b"xyz") + co.flush()
print(zlib.decompress(s) == b"abc" * 100 + b"xyz")
print(zlib.decompress(zlib.compress(b"")), zlib.compress(b"") [:2])
for bad in (b"garbage", cp[:20]):
    try:
        zlib.decompress(bad)
    except zlib.error as e:
        print("error:", e)
big = bytes((i * 7 + i // 13) % 251 for i in range(70000))
print(zlib.decompress(zlib.compress(big)) == big)
