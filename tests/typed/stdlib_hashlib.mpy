# hashlib (md5, sha1, sha2) and hmac
import hashlib
import hmac

for data in [b"", b"abc", b"The quick brown fox jumps over the lazy dog", b"a" * 55, b"a" * 56, b"a" * 64, b"a" * 111, b"a" * 112, b"a" * 200, bytes(range(256))]:
    print(len(data), hashlib.md5(data).hexdigest(), hashlib.sha1(data).hexdigest())
    print(hashlib.sha224(data).hexdigest(), hashlib.sha256(data).hexdigest())
    print(hashlib.sha384(data).hexdigest())
    print(hashlib.sha512(data).hexdigest())
h = hashlib.sha256(b"hello ")
h2 = h.copy()
h.update(b"world")
print(h.hexdigest(), h2.hexdigest(), h.name, h.digest_size, h.block_size, hashlib.new("md5", b"x").hexdigest(), hashlib.new("SHA512").digest_size)
print(hashlib.pbkdf2_hmac("sha256", b"password", b"salt", 2, 20).hex(), hashlib.pbkdf2_hmac("sha1", b"pw", b"s", 3).hex())
print(hmac.new(b"key", b"The quick brown fox jumps over the lazy dog", "md5").hexdigest(), hmac.new(b"key", b"msg", hashlib.sha256).hexdigest())
print(hmac.digest(b"k" * 100, b"data", "sha512").hex()[:32], hmac.compare_digest(b"abc", b"abc"), hmac.compare_digest(b"abc", b"abd"))
m = hmac.new(b"secret", digestmod="sha1")
m.update(b"part1")
m.update(b"part2")
print(m.hexdigest(), m.name, m.digest_size, "md5" in hashlib.algorithms_guaranteed)
try:
    hashlib.new("nope")
except ValueError as e:
    print("ValueError", e)
