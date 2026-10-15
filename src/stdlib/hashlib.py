"""Secure hashes and message digests (CPython's hashlib, in Python): md5, sha1,
sha224, sha256, sha384, sha512, new(), pbkdf2_hmac(), file_digest().
(SHA-512's 64-bit words are kept as two 32-bit halves: minipy's ints are 64-bit
signed numbers.)"""

algorithms_guaranteed = frozenset(["md5", "sha1", "sha224", "sha256", "sha384", "sha512"])
algorithms_available = algorithms_guaranteed

_M32 = 0xFFFFFFFF


def _rol(x: int, n: int) -> int:
    return ((x << n) | (x >> (32 - n))) & _M32


def _ror(x: int, n: int) -> int:
    return ((x >> n) | (x << (32 - n))) & _M32


def _be32(b: bytes, off: int) -> int:
    return (b[off] << 24) | (b[off + 1] << 16) | (b[off + 2] << 8) | b[off + 3]


def _le32(b: bytes, off: int) -> int:
    return b[off] | (b[off + 1] << 8) | (b[off + 2] << 16) | (b[off + 3] << 24)


def _put_be32(out: list[int], v: int) -> None:
    out.append((v >> 24) & 255)
    out.append((v >> 16) & 255)
    out.append((v >> 8) & 255)
    out.append(v & 255)


def _put_le32(out: list[int], v: int) -> None:
    out.append(v & 255)
    out.append((v >> 8) & 255)
    out.append((v >> 16) & 255)
    out.append((v >> 24) & 255)


# ---------------------------------------------------------------- MD5

_MD5_S = [7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
          5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
          4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
          6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21]
_MD5_K = [0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
          0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
          0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
          0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
          0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
          0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
          0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
          0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391]


def _md5_block(h: list[int], b: bytes, off: int) -> None:
    m = [_le32(b, off + 4 * k) for k in range(16)]
    a = h[0]
    bb = h[1]
    c = h[2]
    d = h[3]
    for i in range(64):
        if i < 16:
            f = (bb & c) | (~bb & d)
            g = i
        elif i < 32:
            f = (d & bb) | (~d & c)
            g = (5 * i + 1) % 16
        elif i < 48:
            f = bb ^ c ^ d
            g = (3 * i + 5) % 16
        else:
            f = c ^ (bb | (~d & _M32))
            g = (7 * i) % 16
        f = (f + a + _MD5_K[i] + m[g]) & _M32
        a = d
        d = c
        c = bb
        bb = (bb + _rol(f & _M32, _MD5_S[i])) & _M32
    h[0] = (h[0] + a) & _M32
    h[1] = (h[1] + bb) & _M32
    h[2] = (h[2] + c) & _M32
    h[3] = (h[3] + d) & _M32


# ---------------------------------------------------------------- SHA-1

def _sha1_block(h: list[int], b: bytes, off: int) -> None:
    w = [_be32(b, off + 4 * k) for k in range(16)]
    for i in range(16, 80):
        w.append(_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1))
    a = h[0]
    bb = h[1]
    c = h[2]
    d = h[3]
    e = h[4]
    for i in range(80):
        if i < 20:
            f = (bb & c) | (~bb & d)
            k = 0x5A827999
        elif i < 40:
            f = bb ^ c ^ d
            k = 0x6ED9EBA1
        elif i < 60:
            f = (bb & c) | (bb & d) | (c & d)
            k = 0x8F1BBCDC
        else:
            f = bb ^ c ^ d
            k = 0xCA62C1D6
        t = (_rol(a, 5) + (f & _M32) + e + k + w[i]) & _M32
        e = d
        d = c
        c = _rol(bb, 30)
        bb = a
        a = t
    h[0] = (h[0] + a) & _M32
    h[1] = (h[1] + bb) & _M32
    h[2] = (h[2] + c) & _M32
    h[3] = (h[3] + d) & _M32
    h[4] = (h[4] + e) & _M32


# ---------------------------------------------------------------- SHA-256

_K256 = [0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
         0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
         0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
         0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
         0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
         0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
         0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
         0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2]


def _sha256_block(h: list[int], b: bytes, off: int) -> None:
    w = [_be32(b, off + 4 * k) for k in range(16)]
    for i in range(16, 64):
        s0 = _ror(w[i - 15], 7) ^ _ror(w[i - 15], 18) ^ (w[i - 15] >> 3)
        s1 = _ror(w[i - 2], 17) ^ _ror(w[i - 2], 19) ^ (w[i - 2] >> 10)
        w.append((w[i - 16] + s0 + w[i - 7] + s1) & _M32)
    a = h[0]
    bb = h[1]
    c = h[2]
    d = h[3]
    e = h[4]
    f = h[5]
    g = h[6]
    hh = h[7]
    for i in range(64):
        s1 = _ror(e, 6) ^ _ror(e, 11) ^ _ror(e, 25)
        ch = (e & f) ^ ((~e & _M32) & g)
        t1 = (hh + s1 + ch + _K256[i] + w[i]) & _M32
        s0 = _ror(a, 2) ^ _ror(a, 13) ^ _ror(a, 22)
        maj = (a & bb) ^ (a & c) ^ (bb & c)
        t2 = (s0 + maj) & _M32
        hh = g
        g = f
        f = e
        e = (d + t1) & _M32
        d = c
        c = bb
        bb = a
        a = (t1 + t2) & _M32
    h[0] = (h[0] + a) & _M32
    h[1] = (h[1] + bb) & _M32
    h[2] = (h[2] + c) & _M32
    h[3] = (h[3] + d) & _M32
    h[4] = (h[4] + e) & _M32
    h[5] = (h[5] + f) & _M32
    h[6] = (h[6] + g) & _M32
    h[7] = (h[7] + hh) & _M32


# ---------------------------------------------------------------- SHA-512 (64-bit words as hi, lo halves)

_K512 = [0x428a2f98, 0xd728ae22, 0x71374491, 0x23ef65cd, 0xb5c0fbcf, 0xec4d3b2f, 0xe9b5dba5, 0x8189dbbc,
         0x3956c25b, 0xf348b538, 0x59f111f1, 0xb605d019, 0x923f82a4, 0xaf194f9b, 0xab1c5ed5, 0xda6d8118,
         0xd807aa98, 0xa3030242, 0x12835b01, 0x45706fbe, 0x243185be, 0x4ee4b28c, 0x550c7dc3, 0xd5ffb4e2,
         0x72be5d74, 0xf27b896f, 0x80deb1fe, 0x3b1696b1, 0x9bdc06a7, 0x25c71235, 0xc19bf174, 0xcf692694,
         0xe49b69c1, 0x9ef14ad2, 0xefbe4786, 0x384f25e3, 0x0fc19dc6, 0x8b8cd5b5, 0x240ca1cc, 0x77ac9c65,
         0x2de92c6f, 0x592b0275, 0x4a7484aa, 0x6ea6e483, 0x5cb0a9dc, 0xbd41fbd4, 0x76f988da, 0x831153b5,
         0x983e5152, 0xee66dfab, 0xa831c66d, 0x2db43210, 0xb00327c8, 0x98fb213f, 0xbf597fc7, 0xbeef0ee4,
         0xc6e00bf3, 0x3da88fc2, 0xd5a79147, 0x930aa725, 0x06ca6351, 0xe003826f, 0x14292967, 0x0a0e6e70,
         0x27b70a85, 0x46d22ffc, 0x2e1b2138, 0x5c26c926, 0x4d2c6dfc, 0x5ac42aed, 0x53380d13, 0x9d95b3df,
         0x650a7354, 0x8baf63de, 0x766a0abb, 0x3c77b2a8, 0x81c2c92e, 0x47edaee6, 0x92722c85, 0x1482353b,
         0xa2bfe8a1, 0x4cf10364, 0xa81a664b, 0xbc423001, 0xc24b8b70, 0xd0f89791, 0xc76c51a3, 0x0654be30,
         0xd192e819, 0xd6ef5218, 0xd6990624, 0x5565a910, 0xf40e3585, 0x5771202a, 0x106aa070, 0x32bbd1b8,
         0x19a4c116, 0xb8d2d0c8, 0x1e376c08, 0x5141ab53, 0x2748774c, 0xdf8eeb99, 0x34b0bcb5, 0xe19b48a8,
         0x391c0cb3, 0xc5c95a63, 0x4ed8aa4a, 0xe3418acb, 0x5b9cca4f, 0x7763e373, 0x682e6ff3, 0xd6b2b8a3,
         0x748f82ee, 0x5defb2fc, 0x78a5636f, 0x43172f60, 0x84c87814, 0xa1f0ab72, 0x8cc70208, 0x1a6439ec,
         0x90befffa, 0x23631e28, 0xa4506ceb, 0xde82bde9, 0xbef9a3f7, 0xb2c67915, 0xc67178f2, 0xe372532b,
         0xca273ece, 0xea26619c, 0xd186b8c7, 0x21c0c207, 0xeada7dd6, 0xcde0eb1e, 0xf57d4f7f, 0xee6ed178,
         0x06f067aa, 0x72176fba, 0x0a637dc5, 0xa2c898a6, 0x113f9804, 0xbef90dae, 0x1b710b35, 0x131c471b,
         0x28db77f5, 0x23047d84, 0x32caab7b, 0x40c72493, 0x3c9ebe0a, 0x15c9bebc, 0x431d67c4, 0x9c100d4c,
         0x4cc5d4be, 0xcb3e42b6, 0x597f299c, 0xfc657e2a, 0x5fcb6fab, 0x3ad6faec, 0x6c44198c, 0x4a475817]


def _ror64(hi: int, lo: int, n: int) -> tuple[int, int]:
    """(hi, lo) rotated right by n bits."""
    if n >= 32:
        hi, lo = lo, hi
        n -= 32
    if n == 0:
        return (hi, lo)
    return (((hi >> n) | (lo << (32 - n))) & _M32, ((lo >> n) | (hi << (32 - n))) & _M32)


def _shr64(hi: int, lo: int, n: int) -> tuple[int, int]:
    return (hi >> n, ((lo >> n) | (hi << (32 - n))) & _M32)


def _sha512_block(h: list[int], b: bytes, off: int) -> None:
    w: list[int] = []
    for k in range(16):
        w.append(_be32(b, off + 8 * k))
        w.append(_be32(b, off + 8 * k + 4))
    for i in range(16, 80):
        xh = w[2 * (i - 15)]
        xl = w[2 * (i - 15) + 1]
        a1, a2 = _ror64(xh, xl, 1)
        b1, b2 = _ror64(xh, xl, 8)
        c1, c2 = _shr64(xh, xl, 7)
        s0h = a1 ^ b1 ^ c1
        s0l = a2 ^ b2 ^ c2
        xh = w[2 * (i - 2)]
        xl = w[2 * (i - 2) + 1]
        a1, a2 = _ror64(xh, xl, 19)
        b1, b2 = _ror64(xh, xl, 61)
        c1, c2 = _shr64(xh, xl, 6)
        s1h = a1 ^ b1 ^ c1
        s1l = a2 ^ b2 ^ c2
        lo = w[2 * (i - 16) + 1] + s0l + w[2 * (i - 7) + 1] + s1l
        hi = w[2 * (i - 16)] + s0h + w[2 * (i - 7)] + s1h + (lo >> 32)
        w.append(hi & _M32)
        w.append(lo & _M32)
    v = list(h)
    for i in range(80):
        eh = v[8]
        el = v[9]
        a1, a2 = _ror64(eh, el, 14)
        b1, b2 = _ror64(eh, el, 18)
        c1, c2 = _ror64(eh, el, 41)
        s1h = a1 ^ b1 ^ c1
        s1l = a2 ^ b2 ^ c2
        chh = (eh & v[10]) ^ ((~eh & _M32) & v[12])
        chl = (el & v[11]) ^ ((~el & _M32) & v[13])
        lo = v[15] + s1l + chl + _K512[2 * i + 1] + w[2 * i + 1]
        t1h = (v[14] + s1h + chh + _K512[2 * i] + w[2 * i] + (lo >> 32)) & _M32
        t1l = lo & _M32
        ah = v[0]
        al = v[1]
        a1, a2 = _ror64(ah, al, 28)
        b1, b2 = _ror64(ah, al, 34)
        c1, c2 = _ror64(ah, al, 39)
        s0h = a1 ^ b1 ^ c1
        s0l = a2 ^ b2 ^ c2
        majh = (ah & v[2]) ^ (ah & v[4]) ^ (v[2] & v[4])
        majl = (al & v[3]) ^ (al & v[5]) ^ (v[3] & v[5])
        lo = s0l + majl
        t2h = (s0h + majh + (lo >> 32)) & _M32
        t2l = lo & _M32
        v[14] = v[12]
        v[15] = v[13]
        v[12] = v[10]
        v[13] = v[11]
        v[10] = v[8]
        v[11] = v[9]
        lo = v[7] + t1l
        v[8] = (v[6] + t1h + (lo >> 32)) & _M32
        v[9] = lo & _M32
        v[6] = v[4]
        v[7] = v[5]
        v[4] = v[2]
        v[5] = v[3]
        v[2] = ah
        v[3] = al
        lo = t1l + t2l
        v[0] = (t1h + t2h + (lo >> 32)) & _M32
        v[1] = lo & _M32
    for k in range(0, 16, 2):
        lo = h[k + 1] + v[k + 1]
        h[k] = (h[k] + v[k] + (lo >> 32)) & _M32
        h[k + 1] = lo & _M32


# ---------------------------------------------------------------- the hash objects

_IV = {"md5": [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476],
       "sha1": [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0],
       "sha224": [0xc1059ed8, 0x367cd507, 0x3070dd17, 0xf70e5939, 0xffc00b31, 0x68581511, 0x64f98fa7, 0xbefa4fa4],
       "sha256": [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19],
       "sha384": [0xcbbb9d5d, 0xc1059ed8, 0x629a292a, 0x367cd507, 0x9159015a, 0x3070dd17, 0x152fecd8, 0xf70e5939,
                  0x67332667, 0xffc00b31, 0x8eb44a87, 0x68581511, 0xdb0c2e0d, 0x64f98fa7, 0x47b5481d, 0xbefa4fa4],
       "sha512": [0x6a09e667, 0xf3bcc908, 0xbb67ae85, 0x84caa73b, 0x3c6ef372, 0xfe94f82b, 0xa54ff53a, 0x5f1d36f1,
                  0x510e527f, 0xade682d1, 0x9b05688c, 0x2b3e6c1f, 0x1f83d9ab, 0xfb41bd6b, 0x5be0cd19, 0x137e2179]}
_DIGEST_SIZE = {"md5": 16, "sha1": 20, "sha224": 28, "sha256": 32, "sha384": 48, "sha512": 64}


class _Hash:
    """A running hash of the bytes given to update()."""

    def __init__(self, name: str, data: bytes = b""):
        self.name = name
        self.digest_size = _DIGEST_SIZE[name]
        self.block_size = 128 if name in ("sha384", "sha512") else 64
        self._h = list(_IV[name])
        self._buf = b""
        self._len = 0
        if data:
            self.update(data)

    def _block(self, b: bytes, off: int) -> None:
        n = self.name
        if n == "md5":
            _md5_block(self._h, b, off)
        elif n == "sha1":
            _sha1_block(self._h, b, off)
        elif n == "sha224" or n == "sha256":
            _sha256_block(self._h, b, off)
        else:
            _sha512_block(self._h, b, off)

    def update(self, data: bytes) -> None:
        """Add data to what is hashed."""
        self._len += len(data)
        buf = self._buf + data
        bs = self.block_size
        k = 0
        while k + bs <= len(buf):
            self._block(buf, k)
            k += bs
        self._buf = buf[k:]

    def copy(self) -> "_Hash":
        c = _Hash(self.name)
        c._h = list(self._h)
        c._buf = self._buf
        c._len = self._len
        return c

    def digest(self) -> bytes:
        """The hash of the bytes so far (more may still be added)."""
        c = self.copy()
        bs = self.block_size
        bits = self._len * 8
        tail = c._buf + b"\x80"
        lenbytes = 16 if bs == 128 else 8
        while (len(tail) + lenbytes) % bs:
            tail += b"\x00"
        out: list[int] = []
        if self.name == "md5":
            _put_le32(out, bits & _M32)
            _put_le32(out, bits >> 32)
        else:
            if lenbytes == 16:
                for k in range(8):
                    out.append(0)
            _put_be32(out, bits >> 32)
            _put_be32(out, bits & _M32)
        tail += bytes(out)
        for k in range(0, len(tail), bs):
            c._block(tail, k)
        res: list[int] = []
        for v in c._h:
            if self.name == "md5":
                _put_le32(res, v)
            else:
                _put_be32(res, v)
        return bytes(res[:self.digest_size])

    def hexdigest(self) -> str:
        return self.digest().hex()

    def __repr__(self) -> str:
        return "<" + self.name + " _hashlib.HASH object>"


def new(name: str, data: bytes = b"", *, usedforsecurity: bool = True) -> _Hash:
    """A hash object of algorithm name ('md5', 'sha256', ...)."""
    n = name.lower().replace("-", "").replace("_", "")
    if n not in _IV:
        raise ValueError("unsupported hash type " + name)
    return _Hash(n, data)


def md5(data: bytes = b"", *, usedforsecurity: bool = True) -> _Hash:
    return _Hash("md5", data)


def sha1(data: bytes = b"", *, usedforsecurity: bool = True) -> _Hash:
    return _Hash("sha1", data)


def sha224(data: bytes = b"", *, usedforsecurity: bool = True) -> _Hash:
    return _Hash("sha224", data)


def sha256(data: bytes = b"", *, usedforsecurity: bool = True) -> _Hash:
    return _Hash("sha256", data)


def sha384(data: bytes = b"", *, usedforsecurity: bool = True) -> _Hash:
    return _Hash("sha384", data)


def sha512(data: bytes = b"", *, usedforsecurity: bool = True) -> _Hash:
    return _Hash("sha512", data)


def _hmac(name: str, key: bytes, msg: bytes) -> bytes:
    bs = _Hash(name).block_size
    if len(key) > bs:
        key = _Hash(name, key).digest()
    key = key + b"\x00" * (bs - len(key))
    inner = _Hash(name, bytes(k ^ 0x36 for k in key))
    inner.update(msg)
    outer = _Hash(name, bytes(k ^ 0x5C for k in key))
    outer.update(inner.digest())
    return outer.digest()


def pbkdf2_hmac(hash_name: str, password: bytes, salt: bytes, iterations: int, dklen: int | None = None) -> bytes:
    """PBKDF2 with HMAC of hash_name: a key of dklen bytes (the digest's size without it)."""
    n = hash_name.lower().replace("-", "")
    if n not in _IV:
        raise ValueError("unsupported hash type " + hash_name)
    if iterations < 1:
        raise ValueError("iteration value must be greater than 0.")
    size = _DIGEST_SIZE[n] if dklen is None else dklen
    if size < 1:
        raise ValueError("key length must be greater than 0.")
    out = b""
    block = 1
    while len(out) < size:
        u = _hmac(n, password, salt + bytes([(block >> 24) & 255, (block >> 16) & 255, (block >> 8) & 255, block & 255]))
        acc = list(u)
        for k in range(iterations - 1):
            u = _hmac(n, password, u)
            for j in range(len(acc)):
                acc[j] ^= u[j]
        out += bytes(acc)
        block += 1
    return out[:size]


def file_digest(fileobj, digest: str) -> _Hash:
    """The hash (algorithm digest) of what fileobj.read() gives."""
    h = new(digest)
    while True:
        data = fileobj.read(65536)
        if not data:
            break
        h.update(data)
    return h
