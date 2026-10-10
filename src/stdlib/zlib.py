"""Compression compatible with gzip (CPython's zlib), written in Python: compress / decompress
(zlib, gzip and raw deflate streams), compressobj / decompressobj, crc32, adler32.

decompress() reads any valid stream; compress() makes valid streams of its own (LZ77 with hash
chains and Huffman codes: not byte for byte what C zlib makes)."""

__all__ = ["compress", "decompress", "compressobj", "decompressobj", "crc32", "adler32", "error", "MAX_WBITS",
           "DEFLATED", "DEF_MEM_LEVEL", "DEF_BUF_SIZE", "Z_BEST_COMPRESSION", "Z_BEST_SPEED", "Z_DEFAULT_COMPRESSION",
           "Z_DEFAULT_STRATEGY", "Z_FILTERED", "Z_HUFFMAN_ONLY", "Z_RLE", "Z_FIXED", "Z_NO_FLUSH", "Z_PARTIAL_FLUSH",
           "Z_SYNC_FLUSH", "Z_FULL_FLUSH", "Z_FINISH", "Z_BLOCK", "Z_TREES", "ZLIB_VERSION", "ZLIB_RUNTIME_VERSION"]

MAX_WBITS = 15
DEFLATED = 8
DEF_MEM_LEVEL = 8
DEF_BUF_SIZE = 16384
Z_NO_COMPRESSION = 0
Z_BEST_SPEED = 1
Z_BEST_COMPRESSION = 9
Z_DEFAULT_COMPRESSION = -1
Z_FILTERED = 1
Z_HUFFMAN_ONLY = 2
Z_RLE = 3
Z_FIXED = 4
Z_DEFAULT_STRATEGY = 0
Z_NO_FLUSH = 0
Z_PARTIAL_FLUSH = 1
Z_SYNC_FLUSH = 2
Z_FULL_FLUSH = 3
Z_FINISH = 4
Z_BLOCK = 5
Z_TREES = 6
ZLIB_VERSION = "1.3.1"
ZLIB_RUNTIME_VERSION = "1.3.1"


class error(Exception):
    pass


# ---------------------------------------------------------------- checksums

def _make_crc_table() -> list[int]:
    table: list[int] = []
    for n in range(256):
        c = n
        for _ in range(8):
            if c & 1:
                c = 0xEDB88320 ^ (c >> 1)
            else:
                c >>= 1
        table.append(c)
    return table


_CRC_TABLE = _make_crc_table()


def crc32(data: bytes, value: int = 0) -> int:
    """The CRC-32 of data (continuing from value)."""
    crc = (value & 0xFFFFFFFF) ^ 0xFFFFFFFF
    t = _CRC_TABLE
    for b in data:
        crc = t[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


def adler32(data: bytes, value: int = 1) -> int:
    """The Adler-32 checksum of data (continuing from value)."""
    a = value & 0xFFFF
    b = (value >> 16) & 0xFFFF
    n = len(data)
    i = 0
    while i < n:
        end = min(i + 3800, n)
        for k in range(i, end):
            a += data[k]
            b += a
        a %= 65521
        b %= 65521
        i = end
    return (b << 16) | a


# ---------------------------------------------------------------- tables

_LEN_BASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195,
             227, 258]
_LEN_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0]
_DIST_BASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
              4097, 6145, 8193, 12289, 16385, 24577]
_DIST_EXTRA = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13]
_CL_ORDER = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15]


# ---------------------------------------------------------------- inflate

class _Huffman:
    """A canonical Huffman decoding table: counts per length, symbols in code order."""

    def __init__(self, lengths: list[int]) -> None:
        self.counts = [0] * 16
        for n in lengths:
            self.counts[n] += 1
        self.counts[0] = 0
        offs = [0] * 16
        for i in range(1, 16):
            offs[i] = offs[i - 1] + self.counts[i - 1]
        self.symbols = [0] * len(lengths)
        for sym in range(len(lengths)):
            n = lengths[sym]
            if n:
                self.symbols[offs[n]] = sym
                offs[n] += 1


def _fixed_tables() -> tuple[_Huffman, _Huffman]:
    lengths = [8] * 144 + [9] * 112 + [7] * 24 + [8] * 8
    return _Huffman(lengths), _Huffman([5] * 30)


_FIXED_LIT, _FIXED_DIST = _fixed_tables()


class _Inflater:
    """Decodes a deflate stream from data (whole or incrementally fed)."""

    def __init__(self) -> None:
        self.data = b""
        self.pos = 0                    # byte position
        self.bitbuf = 0
        self.bitcnt = 0
        self.out: list[int] = []        # everything decoded (the window is its end)
        self.final = False
        self.done = False

    def _need(self, n: int) -> None:
        while self.bitcnt < n:
            if self.pos >= len(self.data):
                raise _NeedMore()
            self.bitbuf |= self.data[self.pos] << self.bitcnt
            self.pos += 1
            self.bitcnt += 8

    def bits(self, n: int) -> int:
        self._need(n)
        v = self.bitbuf & ((1 << n) - 1)
        self.bitbuf >>= n
        self.bitcnt -= n
        return v

    def decode(self, h: _Huffman) -> int:
        code = 0
        first = 0
        index = 0
        for length in range(1, 16):
            code |= self.bits(1)
            count = h.counts[length]
            if code - count < first:
                return h.symbols[index + (code - first)]
            index += count
            first += count
            first <<= 1
            code <<= 1
        raise error("Error -3 while decompressing data: invalid code lengths set")

    def run(self) -> None:
        """Decodes blocks while there is data (a block is decoded whole or not at all)."""
        while not self.done:
            save = (self.pos, self.bitbuf, self.bitcnt, len(self.out))
            try:
                self._block()
            except _NeedMore:
                self.pos, self.bitbuf, self.bitcnt, n = save
                del self.out[n:]
                return

    def _block(self) -> None:
        last = self.bits(1)
        kind = self.bits(2)
        if kind == 0:
            self.bitbuf = 0
            self.bitcnt = 0
            if self.pos + 4 > len(self.data):
                raise _NeedMore()
            ln = self.data[self.pos] | (self.data[self.pos + 1] << 8)
            nln = self.data[self.pos + 2] | (self.data[self.pos + 3] << 8)
            if ln != (~nln & 0xFFFF):
                raise error("Error -3 while decompressing data: invalid stored block lengths")
            if self.pos + 4 + ln > len(self.data):
                raise _NeedMore()
            self.out.extend(self.data[self.pos + 4:self.pos + 4 + ln])
            self.pos += 4 + ln
        elif kind == 1:
            self._codes(_FIXED_LIT, _FIXED_DIST)
        elif kind == 2:
            lit, dist = self._dynamic()
            self._codes(lit, dist)
        else:
            raise error("Error -3 while decompressing data: invalid block type")
        if last:
            self.done = True

    def _dynamic(self) -> tuple[_Huffman, _Huffman]:
        nlen = self.bits(5) + 257
        ndist = self.bits(5) + 1
        ncode = self.bits(4) + 4
        if nlen > 286 or ndist > 30:
            raise error("Error -3 while decompressing data: too many length or distance symbols")
        lengths = [0] * 19
        for i in range(ncode):
            lengths[_CL_ORDER[i]] = self.bits(3)
        lencode = _Huffman(lengths)
        lens: list[int] = []
        while len(lens) < nlen + ndist:
            sym = self.decode(lencode)
            if sym < 16:
                lens.append(sym)
            else:
                if sym == 16:
                    if not lens:
                        raise error("Error -3 while decompressing data: invalid bit length repeat")
                    prev = lens[-1]
                    rep = 3 + self.bits(2)
                elif sym == 17:
                    prev = 0
                    rep = 3 + self.bits(3)
                else:
                    prev = 0
                    rep = 11 + self.bits(7)
                if len(lens) + rep > nlen + ndist:
                    raise error("Error -3 while decompressing data: invalid bit length repeat")
                lens.extend([prev] * rep)
        return _Huffman(lens[:nlen]), _Huffman(lens[nlen:])

    def _codes(self, lit: _Huffman, dist: _Huffman) -> None:
        out = self.out
        while True:
            sym = self.decode(lit)
            if sym < 256:
                out.append(sym)
            elif sym == 256:
                return
            else:
                sym -= 257
                if sym >= 29:
                    raise error("Error -3 while decompressing data: invalid literal/length code")
                length = _LEN_BASE[sym] + self.bits(_LEN_EXTRA[sym])
                ds = self.decode(dist)
                if ds >= 30:
                    raise error("Error -3 while decompressing data: invalid distance code")
                d = _DIST_BASE[ds] + self.bits(_DIST_EXTRA[ds])
                if d > len(out):
                    raise error("Error -3 while decompressing data: invalid distance too far back")
                start = len(out) - d
                if d >= length:
                    out.extend(out[start:start + length])
                else:
                    for k in range(length):
                        out.append(out[start + k])


class _NeedMore(Exception):
    pass


# ---------------------------------------------------------------- deflate

class _BitWriter:
    def __init__(self) -> None:
        self.out: list[int] = []
        self.buf = 0
        self.cnt = 0

    def put(self, value: int, n: int) -> None:
        self.buf |= value << self.cnt
        self.cnt += n
        while self.cnt >= 8:
            self.out.append(self.buf & 0xFF)
            self.buf >>= 8
            self.cnt -= 8

    def put_rev(self, code: int, n: int) -> None:
        """A Huffman code (most significant bit first)."""
        r = 0
        for _ in range(n):
            r = (r << 1) | (code & 1)
            code >>= 1
        self.put(r, n)

    def align(self) -> None:
        if self.cnt:
            self.out.append(self.buf & 0xFF)
            self.buf = 0
            self.cnt = 0


def _canonical(lengths: list[int]) -> list[int]:
    """The codes of canonical Huffman code lengths."""
    bl_count = [0] * 16
    for n in lengths:
        if n:
            bl_count[n] += 1
    next_code = [0] * 16
    code = 0
    for bits in range(1, 16):
        code = (code + bl_count[bits - 1]) << 1
        next_code[bits] = code
    codes = [0] * len(lengths)
    for sym in range(len(lengths)):
        n = lengths[sym]
        if n:
            codes[sym] = next_code[n]
            next_code[n] += 1
    return codes


def _limited_lengths(freqs: list[int], maxbits: int) -> list[int]:
    """Huffman code lengths of at most maxbits for the frequencies (0: unused symbol)."""
    n = len(freqs)
    lengths = [0] * n
    syms = [s for s in range(n) if freqs[s] > 0]
    if not syms:
        return lengths
    if len(syms) == 1:
        lengths[syms[0]] = 1
        return lengths
    # Huffman tree by repeatedly joining the two lightest nodes
    weight: list[int] = []
    parent: list[int] = []
    for s in syms:
        weight.append(freqs[s])
        parent.append(-1)
    active = list(range(len(syms)))
    while len(active) > 1:
        active.sort(key=lambda k: weight[k])
        a = active[0]
        b = active[1]
        node = len(weight)
        weight.append(weight[a] + weight[b])
        parent.append(-1)
        parent[a] = node
        parent[b] = node
        active = active[2:] + [node]
    for k in range(len(syms)):
        d = 0
        p = k
        while parent[p] >= 0:
            p = parent[p]
            d += 1
        lengths[syms[k]] = d
    # too long codes: shorten them (Kraft sum fixed by lengthening the shortest)
    if max(lengths) > maxbits:
        for s in syms:
            if lengths[s] > maxbits:
                lengths[s] = maxbits
        while True:
            kraft = 0
            for s in syms:
                kraft += 1 << (maxbits - lengths[s])
            if kraft <= (1 << maxbits):
                break
            best = -1
            for s in syms:
                if lengths[s] < maxbits and (best < 0 or lengths[s] > lengths[best]):
                    best = s
            lengths[best] += 1
    return lengths


def _len_code(length: int) -> tuple[int, int, int]:
    """(symbol, extra bits, extra value) of a match length."""
    for i in range(28, -1, -1):
        if length >= _LEN_BASE[i]:
            return 257 + i, _LEN_EXTRA[i], length - _LEN_BASE[i]
    return 257, 0, 0


def _dist_code(dist: int) -> tuple[int, int, int]:
    for i in range(29, -1, -1):
        if dist >= _DIST_BASE[i]:
            return i, _DIST_EXTRA[i], dist - _DIST_BASE[i]
    return 0, 0, 0


def _lz77(data: bytes, level: int) -> list[tuple[int, int]]:
    """(literal byte, 0) or (length, distance) tokens."""
    tokens: list[tuple[int, int]] = []
    n = len(data)
    if level == 0:
        return tokens
    max_chain = 8 if level <= 3 else 32 if level <= 6 else 128
    nice = 32 if level <= 3 else 128 if level <= 6 else 258
    head: dict[int, int] = {}
    prev = [0] * n
    i = 0
    while i < n:
        best_len = 0
        best_dist = 0
        if i + 2 < n:
            key = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2]
            cand = head.get(key, -1)
            chain = 0
            while cand >= 0 and i - cand <= 32768 and chain < max_chain:
                if i + best_len < n and data[cand + best_len] == data[i + best_len]:
                    k = 0
                    m = min(258, n - i)
                    while k < m and data[cand + k] == data[i + k]:
                        k += 1
                    if k > best_len:
                        best_len = k
                        best_dist = i - cand
                        if k >= nice:
                            break
                nxt = prev[cand]
                if nxt >= cand:
                    break
                cand = nxt
                chain += 1
            prev[i] = head.get(key, -1)
            head[key] = i
        if best_len >= 3:
            tokens.append((best_len, best_dist))
            for k in range(i + 1, min(i + best_len, n - 2)):
                key2 = (data[k] << 16) | (data[k + 1] << 8) | data[k + 2]
                prev[k] = head.get(key2, -1)
                head[key2] = k
            i += best_len
        else:
            tokens.append((data[i], 0))
            i += 1
    return tokens


def _deflate(data: bytes, level: int) -> bytes:
    """A raw deflate stream of data."""
    w = _BitWriter()
    if level == 0 or len(data) == 0:
        if len(data) == 0:
            w.put(1, 1)
            w.put(1, 2)                 # one fixed block with only the end code
            w.put_rev(0, 7)
            w.align()
            return bytes(w.out)
        pos = 0
        while pos < len(data):
            chunk = data[pos:pos + 65535]
            pos += len(chunk)
            w.put(1 if pos >= len(data) else 0, 1)
            w.put(0, 2)
            w.align()
            ln = len(chunk)
            w.out.extend([ln & 0xFF, ln >> 8, (~ln) & 0xFF, ((~ln) >> 8) & 0xFF])
            w.out.extend(chunk)
        return bytes(w.out)
    tokens = _lz77(data, level)
    lit_freq = [0] * 286
    dist_freq = [0] * 30
    for v, d in tokens:
        if d == 0:
            lit_freq[v] += 1
        else:
            lit_freq[_len_code(v)[0]] += 1
            dist_freq[_dist_code(d)[0]] += 1
    lit_freq[256] = 1
    lit_len = _limited_lengths(lit_freq, 15)
    dist_len = _limited_lengths(dist_freq, 15)
    if max(dist_len) == 0:
        dist_len[0] = 1
    nlen = 286
    while nlen > 257 and lit_len[nlen - 1] == 0:
        nlen -= 1
    ndist = 30
    while ndist > 1 and dist_len[ndist - 1] == 0:
        ndist -= 1
    # the code lengths, run-length coded
    all_lens = lit_len[:nlen] + dist_len[:ndist]
    rle: list[tuple[int, int]] = []
    i = 0
    while i < len(all_lens):
        v = all_lens[i]
        run = 1
        while i + run < len(all_lens) and all_lens[i + run] == v:
            run += 1
        if v == 0 and run >= 3:
            r = run
            while r >= 11:
                k = min(r, 138)
                rle.append((18, k - 11))
                r -= k
            if r >= 3:
                rle.append((17, r - 3))
                r = 0
            for _ in range(r):
                rle.append((0, 0))
            i += run
        elif v != 0 and run >= 4:
            rle.append((v, 0))
            r = run - 1
            while r >= 3:
                k = min(r, 6)
                rle.append((16, k - 3))
                r -= k
            for _ in range(r):
                rle.append((v, 0))
            i += run
        else:
            for _ in range(run):
                rle.append((v, 0))
            i += run
    cl_freq = [0] * 19
    for s, x in rle:
        cl_freq[s] += 1
    cl_len = _limited_lengths(cl_freq, 7)
    ncode = 19
    while ncode > 4 and cl_len[_CL_ORDER[ncode - 1]] == 0:
        ncode -= 1
    lit_codes = _canonical(lit_len)
    dist_codes = _canonical(dist_len)
    cl_codes = _canonical(cl_len)
    w.put(1, 1)
    w.put(2, 2)
    w.put(nlen - 257, 5)
    w.put(ndist - 1, 5)
    w.put(ncode - 4, 4)
    for i in range(ncode):
        w.put(cl_len[_CL_ORDER[i]], 3)
    for s, x in rle:
        w.put_rev(cl_codes[s], cl_len[s])
        if s == 16:
            w.put(x, 2)
        elif s == 17:
            w.put(x, 3)
        elif s == 18:
            w.put(x, 7)
    for v, d in tokens:
        if d == 0:
            w.put_rev(lit_codes[v], lit_len[v])
        else:
            sym, eb, ev = _len_code(v)
            w.put_rev(lit_codes[sym], lit_len[sym])
            if eb:
                w.put(ev, eb)
            ds, deb, dev = _dist_code(d)
            w.put_rev(dist_codes[ds], dist_len[ds])
            if deb:
                w.put(dev, deb)
    w.put_rev(lit_codes[256], lit_len[256])
    w.align()
    return bytes(w.out)


# ---------------------------------------------------------------- the API

def _level_of(level: int) -> int:
    if level == -1:
        return 6
    if level < 0 or level > 9:
        raise error("Bad compression level")
    return level


def compress(data: bytes, /, level: int = -1, wbits: int = MAX_WBITS) -> bytes:
    """data compressed: a zlib stream (wbits 9..15), raw deflate (-15..-9) or gzip (25..31)."""
    lv = _level_of(level)
    body = _deflate(bytes(data), lv)
    if 9 <= wbits <= 15:
        flevel = 0 if lv < 2 else 1 if lv < 6 else 2 if lv == 6 else 3
        cmf = ((wbits - 8) << 4) | 8
        flg = flevel << 6
        flg |= 31 - ((cmf << 8) | flg) % 31
        a = adler32(data)
        return bytes([cmf, flg]) + body + bytes([(a >> 24) & 0xFF, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF])
    if -15 <= wbits <= -9:
        return body
    if 25 <= wbits <= 31:
        c = crc32(data)
        n = len(data) & 0xFFFFFFFF
        xfl = 2 if lv == 9 else 4 if lv == 1 else 0
        head = bytes([0x1F, 0x8B, 8, 0, 0, 0, 0, 0, xfl, 255])
        tail = bytes([c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, (c >> 24) & 0xFF,
                      n & 0xFF, (n >> 8) & 0xFF, (n >> 16) & 0xFF, (n >> 24) & 0xFF])
        return head + body + tail
    raise ValueError("Invalid initialization option")


class _Decomp:
    """A decompressobj: data fed in pieces; the stream's container from wbits."""

    def __init__(self, wbits: int = MAX_WBITS, zdict: bytes = b"") -> None:
        if not (8 <= wbits <= 15 or -15 <= wbits <= -8 or 24 <= wbits <= 31 or 40 <= wbits <= 47):
            raise ValueError("Invalid initialization option")
        self._wbits = wbits
        self._inf = _Inflater()
        self._header_done = False
        self._kind = "zlib" if 8 <= wbits <= 15 else "raw" if wbits < 0 else "gzip" if wbits < 32 else "auto"
        self._buf = b""
        self._emitted = 0
        self.unused_data = b""
        self.unconsumed_tail = b""
        self.eof = False
        self._trailer_done = False

    def _header(self) -> bool:
        b = self._buf
        if self._kind == "auto":
            if len(b) < 2:
                return False
            self._kind = "gzip" if b[0] == 0x1F and b[1] == 0x8B else "zlib"
        if self._kind == "raw":
            return True
        if self._kind == "zlib":
            if len(b) < 2:
                return False
            cmf = b[0]
            flg = b[1]
            if (cmf & 0x0F) != 8 or ((cmf << 8) | flg) % 31 != 0:
                raise error("Error -3 while decompressing data: incorrect header check")
            if flg & 0x20:
                raise error("Error 2 while decompressing data")
            self._buf = b[2:]
            return True
        if len(b) < 10:
            return False
        if b[0] != 0x1F or b[1] != 0x8B:
            raise error("Error -3 while decompressing data: incorrect header check")
        if b[2] != 8:
            raise error("Error -3 while decompressing data: unknown compression method")
        flg = b[3]
        p = 10
        if flg & 4:
            if len(b) < p + 2:
                return False
            xlen = b[p] | (b[p + 1] << 8)
            p += 2 + xlen
        if flg & 8:
            z = b.find(b"\x00", p)
            if z < 0:
                return False
            p = z + 1
        if flg & 16:
            z = b.find(b"\x00", p)
            if z < 0:
                return False
            p = z + 1
        if flg & 2:
            p += 2
        if len(b) < p:
            return False
        self._buf = b[p:]
        return True

    def decompress(self, data: bytes, max_length: int = 0) -> bytes:
        """What data decodes to (with what came before it); after the end: unused_data."""
        if self.eof:
            self.unused_data += bytes(data)
            return b""
        self._buf += self.unconsumed_tail + bytes(data)
        self.unconsumed_tail = b""
        if not self._header_done:
            if not self._header():
                return b""
            self._header_done = True
        inf = self._inf
        inf.data = self._buf
        inf.run()
        self._buf = inf.data[inf.pos:]
        inf.data = self._buf
        inf.pos = 0
        out = inf.out[self._emitted:]
        if max_length > 0 and len(out) > max_length:
            out = out[:max_length]
        self._emitted += len(out)
        if inf.done and self._emitted >= len(inf.out):
            self._finish()
        if len(inf.out) > 65536 + self._emitted:
            pass
        return bytes(out)

    def _finish(self) -> None:
        inf = self._inf
        rest = self._buf
        if inf.bitcnt >= 8:
            k = inf.bitcnt // 8
            rest = bytes([(inf.bitbuf >> (8 * i)) & 0xFF for i in range(k)]) + rest
        if self._kind == "zlib":
            if len(rest) < 4:
                return
            a = (rest[0] << 24) | (rest[1] << 16) | (rest[2] << 8) | rest[3]
            if a != adler32(bytes(inf.out)):
                raise error("Error -3 while decompressing data: incorrect data check")
            rest = rest[4:]
        elif self._kind == "gzip":
            if len(rest) < 8:
                return
            c = rest[0] | (rest[1] << 8) | (rest[2] << 16) | (rest[3] << 24)
            n = rest[4] | (rest[5] << 8) | (rest[6] << 16) | (rest[7] << 24)
            if c != crc32(bytes(inf.out)):
                raise error("Error -3 while decompressing data: incorrect data check")
            if n != len(inf.out) & 0xFFFFFFFF:
                raise error("Error -3 while decompressing data: incorrect length check")
            rest = rest[8:]
        self.eof = True
        self.unused_data = rest
        self._buf = b""

    def flush(self, length: int = DEF_BUF_SIZE) -> bytes:
        """What is left of the output."""
        out = self._inf.out[self._emitted:]
        self._emitted += len(out)
        return bytes(out)

    def copy(self) -> "_Decomp":
        raise ValueError("copy() is not supported")


def decompressobj(wbits: int = MAX_WBITS, zdict: bytes = b"") -> _Decomp:
    """An object decompressing data fed in pieces."""
    return _Decomp(wbits, zdict)


def decompress(data: bytes, /, wbits: int = MAX_WBITS, bufsize: int = DEF_BUF_SIZE) -> bytes:
    """The data of a compressed stream (zlib: wbits 8..15, raw: -15..-8, gzip: 24..31, either: 40..47)."""
    d = _Decomp(wbits)
    out = d.decompress(data)
    if not d.eof:
        raise error("Error -5 while decompressing data: incomplete or truncated stream")
    return out


class _Comp:
    """A compressobj: data fed in pieces, compressed when flushed (one deflate stream)."""

    def __init__(self, level: int = -1, method: int = DEFLATED, wbits: int = MAX_WBITS, memLevel: int = DEF_MEM_LEVEL,
                 strategy: int = Z_DEFAULT_STRATEGY, zdict: bytes = b"") -> None:
        if method != DEFLATED:
            raise ValueError("Invalid initialization option")
        _level_of(level)
        if not (9 <= wbits <= 15 or -15 <= wbits <= -9 or 25 <= wbits <= 31):
            raise ValueError("Invalid initialization option")
        self._level = level
        self._wbits = wbits
        self._data: list[bytes] = []
        self._done = False

    def compress(self, data: bytes) -> bytes:
        """Keeps data for the stream (output comes with flush())."""
        if self._done:
            raise error("Error -2 while compressing data: inconsistent stream state")
        self._data.append(bytes(data))
        return b""

    def flush(self, mode: int = Z_FINISH) -> bytes:
        """The compressed stream (Z_FINISH ends it)."""
        if mode == Z_NO_FLUSH:
            return b""
        if mode != Z_FINISH:
            return b""
        self._done = True
        return compress(b"".join(self._data), self._level, self._wbits)

    def copy(self) -> "_Comp":
        c = _Comp(self._level, DEFLATED, self._wbits)
        c._data = list(self._data)
        return c


def compressobj(level: int = -1, method: int = DEFLATED, wbits: int = MAX_WBITS, memLevel: int = DEF_MEM_LEVEL,
                strategy: int = Z_DEFAULT_STRATEGY, zdict: bytes = b"") -> _Comp:
    """An object compressing data fed in pieces."""
    return _Comp(level, method, wbits, memLevel, strategy, zdict)
