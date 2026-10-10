"""A fast, lightweight IPv4/IPv6 manipulation library in Python (CPython's ipaddress).

This library is used to create/poke/manipulate IPv4 and IPv6 addresses
and networks: ip_address(), ip_network(), ip_interface(), the IPv4Address,
IPv6Address, IPv4Network, IPv6Network, IPv4Interface and IPv6Interface
classes, summarize_address_range(), collapse_addresses(),
get_mixed_type_key(), v4_int_to_packed(), v6_int_to_packed().

Ints are 64-bit here, so an address is kept as a 160-bit number of its own
(_N): int(address) and num_addresses raise OverflowError for values of 2**63
and more (most IPv6 addresses), and IPv6Address(int) takes ints below 2**63
(strings and packed bytes take any address).

Compiled programs: ip_address(), ip_network() and ip_interface() give the
version-independent types (an address, a network); their objects are of the
IPv4/IPv6 classes all the same (isinstance tells them apart)."""
import sys
from typing import TypeVar

__version__ = '1.0'

__all__ = ["AddressValueError", "NetmaskValueError", "IPV4LENGTH", "IPV6LENGTH", "ip_address",
           "ip_network", "ip_interface", "v4_int_to_packed", "v6_int_to_packed", "summarize_address_range",
           "collapse_addresses", "get_mixed_type_key", "IPv4Address", "IPv4Interface", "IPv4Network",
           "IPv6Address", "IPv6Interface", "IPv6Network"]

_A = TypeVar("_A")
_M = TypeVar("_M")

IPV4LENGTH = 32
IPV6LENGTH = 128


class AddressValueError(ValueError):
    """A Value Error related to the address."""


class NetmaskValueError(ValueError):
    """A Value Error related to the netmask."""


# ---------------------------------------------------------------- 160-bit numbers

_M32 = 0xFFFFFFFF


class _N:
    """A non-negative integer below 2**160: five 32-bit limbs, the lowest first."""

    def __init__(self, w: list[int]) -> None:
        self.w = w

    def _cmp(self, o: "_N") -> int:
        for k in range(4, -1, -1):
            if self.w[k] != o.w[k]:
                return -1 if self.w[k] < o.w[k] else 1
        return 0

    def __eq__(self, o: "_N") -> bool:
        return self._cmp(o) == 0

    def __ne__(self, o: "_N") -> bool:
        return self._cmp(o) != 0

    def __lt__(self, o: "_N") -> bool:
        return self._cmp(o) < 0

    def __le__(self, o: "_N") -> bool:
        return self._cmp(o) <= 0

    def __gt__(self, o: "_N") -> bool:
        return self._cmp(o) > 0

    def __ge__(self, o: "_N") -> bool:
        return self._cmp(o) >= 0

    def __hash__(self) -> int:
        return hash((self.w[0], self.w[1], self.w[2], self.w[3], self.w[4]))

    def __add__(self, o: "_N") -> "_N":
        out: list[int] = []
        c = 0
        for k in range(5):
            s = self.w[k] + o.w[k] + c
            out.append(s & _M32)
            c = s >> 32
        return _N(out)

    def __sub__(self, o: "_N") -> "_N":
        """self - o (o <= self)."""
        out: list[int] = []
        b = 0
        for k in range(5):
            s = self.w[k] - o.w[k] - b
            if s < 0:
                s += 1 << 32
                b = 1
            else:
                b = 0
            out.append(s)
        return _N(out)

    def __and__(self, o: "_N") -> "_N":
        return _N([self.w[k] & o.w[k] for k in range(5)])

    def __or__(self, o: "_N") -> "_N":
        return _N([self.w[k] | o.w[k] for k in range(5)])

    def __xor__(self, o: "_N") -> "_N":
        return _N([self.w[k] ^ o.w[k] for k in range(5)])

    def __lshift__(self, n: int) -> "_N":
        q = n // 32
        r = n % 32
        out = [0, 0, 0, 0, 0]
        for k in range(5):
            src = k - q
            if src < 0:
                continue
            v = (self.w[src] << r) & _M32
            if r and src >= 1:
                v |= self.w[src - 1] >> (32 - r)
            out[k] = v
        return _N(out)

    def __rshift__(self, n: int) -> "_N":
        q = n // 32
        r = n % 32
        out = [0, 0, 0, 0, 0]
        for k in range(5):
            src = k + q
            if src > 4:
                continue
            v = self.w[src] >> r
            if r and src + 1 <= 4:
                v |= (self.w[src + 1] << (32 - r)) & _M32
            out[k] = v
        return _N(out)

    def __bool__(self) -> bool:
        return (self.w[0] | self.w[1] | self.w[2] | self.w[3] | self.w[4]) != 0

    def bit_length(self) -> int:
        for k in range(4, -1, -1):
            if self.w[k]:
                return 32 * k + self.w[k].bit_length()
        return 0

    def low(self) -> int:
        """The low 32 bits."""
        return self.w[0]

    def fits(self) -> bool:
        return self.w[4] == 0 and self.w[3] == 0 and self.w[2] == 0 and self.w[1] < 0x80000000

    def to_int(self) -> int:
        if not self.fits():
            raise OverflowError("int too large to convert: " + self.dec() + " (ints are 64-bit here)")
        return (self.w[1] << 32) | self.w[0]

    def dec(self) -> str:
        w = list(self.w)
        digits: list[str] = []
        while w[0] or w[1] or w[2] or w[3] or w[4]:
            r = 0
            for k in range(4, -1, -1):
                cur = (r << 32) | w[k]
                w[k] = cur // 10
                r = cur % 10
            digits.append(chr(48 + r))
        if not digits:
            return "0"
        digits.reverse()
        return "".join(digits)

    def hexs(self, ndigits: int) -> str:
        """The lowest ndigits hex digits (lower case)."""
        s = "".join([format(self.w[k], "08x") for k in range(4, -1, -1)])
        return s[len(s) - ndigits:]

    def bins(self, ndigits: int) -> str:
        s = "".join([format(self.w[k], "032b") for k in range(4, -1, -1)])
        return s[len(s) - ndigits:]

    def to_bytes(self, n: int) -> bytes:
        """The lowest n bytes, big-endian."""
        b = b"".join([self.w[k].to_bytes(4, "big") for k in range(4, -1, -1)])
        return b[len(b) - n:]


def _n(i: int) -> _N:
    """i (>= 0) as an _N."""
    return _N([i & _M32, (i >> 32) & _M32, 0, 0, 0])


def _n_from_bytes(b: bytes) -> _N:
    b = bytes(20 - len(b)) + b
    return _N([int.from_bytes(b[16 - 4 * k:20 - 4 * k], "big") for k in range(5)])


def _ones(bits: int) -> _N:
    return (_n(1) << bits) - _n(1)


def _count_righthand_zero_bits(number: _N, bits: int) -> int:
    """The number of zero bits on the right hand side of number (at most bits)."""
    if not number:
        return bits
    k = 0
    while not (number >> k).low() & 1:
        k += 1
    return min(bits, k)


def _maxlen(version: int) -> int:
    return IPV4LENGTH if version == 4 else IPV6LENGTH


# ---------------------------------------------------------------- parsing

def _report_invalid_netmask(shown: str) -> None:
    raise NetmaskValueError(shown + " is not a valid netmask")


def _parse_octet(octet_str: str) -> int:
    if not octet_str:
        raise ValueError("Empty octet not permitted")
    if not (octet_str.isascii() and octet_str.isdigit()):
        raise ValueError("Only decimal digits permitted in " + repr(octet_str))
    if len(octet_str) > 3:
        raise ValueError("At most 3 characters permitted in " + repr(octet_str))
    if octet_str != '0' and octet_str[0] == '0':
        raise ValueError("Leading zeros are not permitted in " + repr(octet_str))
    octet_int = int(octet_str, 10)
    if octet_int > 255:
        raise ValueError("Octet %d (> 255) not permitted" % octet_int)
    return octet_int


def _v4_int_from_string(ip_str: str) -> _N:
    if not ip_str:
        raise AddressValueError('Address cannot be empty')
    octets = ip_str.split('.')
    if len(octets) != 4:
        raise AddressValueError("Expected 4 octets in " + repr(ip_str))
    v = 0
    try:
        for o in octets:
            v = (v << 8) | _parse_octet(o)
    except ValueError as exc:
        raise AddressValueError(str(exc) + " in " + repr(ip_str)) from None
    return _n(v)


_HEX_DIGITS = "0123456789ABCDEFabcdef"


def _parse_hextet(hextet_str: str) -> int:
    for ch in hextet_str:
        if ch not in _HEX_DIGITS:
            raise ValueError("Only hex digits permitted in " + repr(hextet_str))
    if len(hextet_str) > 4:
        raise ValueError("At most 4 characters permitted in " + repr(hextet_str))
    return int(hextet_str, 16)


def _v6_int_from_string(ip_str: str) -> _N:
    if not ip_str:
        raise AddressValueError('Address cannot be empty')
    if len(ip_str) > 45:
        shorten = ip_str
        if len(shorten) > 100:
            shorten = ip_str[:45] + "(" + str(len(ip_str) - 90) + " chars elided)" + ip_str[-45:]
        raise AddressValueError("At most 45 characters expected in " + repr(shorten))
    max_parts = 9
    parts = ip_str.split(':', max_parts)
    if len(parts) < 3:
        raise AddressValueError("At least 3 parts expected in " + repr(ip_str))
    if '.' in parts[-1]:
        try:
            ipv4_int = _v4_int_from_string(parts.pop()).low()
        except AddressValueError as exc:
            raise AddressValueError(str(exc) + " in " + repr(ip_str)) from None
        parts.append(format((ipv4_int >> 16) & 0xFFFF, "x"))
        parts.append(format(ipv4_int & 0xFFFF, "x"))
    if len(parts) > max_parts:
        raise AddressValueError("At most 8 colons permitted in " + repr(ip_str))
    skip_index = -1
    for i in range(1, len(parts) - 1):
        if not parts[i]:
            if skip_index >= 0:
                raise AddressValueError("At most one '::' permitted in " + repr(ip_str))
            skip_index = i
    if skip_index >= 0:
        parts_hi = skip_index
        parts_lo = len(parts) - skip_index - 1
        if not parts[0]:
            parts_hi -= 1
            if parts_hi:
                raise AddressValueError("Leading ':' only permitted as part of '::' in " + repr(ip_str))
        if not parts[-1]:
            parts_lo -= 1
            if parts_lo:
                raise AddressValueError("Trailing ':' only permitted as part of '::' in " + repr(ip_str))
        parts_skipped = 8 - (parts_hi + parts_lo)
        if parts_skipped < 1:
            raise AddressValueError("Expected at most 7 other parts with '::' in " + repr(ip_str))
    else:
        if len(parts) != 8:
            raise AddressValueError("Exactly 8 parts expected without '::' in " + repr(ip_str))
        if not parts[0]:
            raise AddressValueError("Leading ':' only permitted as part of '::' in " + repr(ip_str))
        if not parts[-1]:
            raise AddressValueError("Trailing ':' only permitted as part of '::' in " + repr(ip_str))
        parts_hi = len(parts)
        parts_lo = 0
        parts_skipped = 0
    ip_int = _n(0)
    try:
        for i in range(parts_hi):
            ip_int = (ip_int << 16) | _n(_parse_hextet(parts[i]))
        ip_int = ip_int << (16 * parts_skipped)
        for i in range(-parts_lo, 0):
            ip_int = (ip_int << 16) | _n(_parse_hextet(parts[i]))
    except ValueError as exc:
        raise AddressValueError(str(exc) + " in " + repr(ip_str)) from None
    return ip_int


def _split_scope_id(ip_str: str) -> tuple[str, str | None]:
    addr, sep, scope_id = ip_str.partition('%')
    if not sep:
        return (addr, None)
    if not scope_id or '%' in scope_id:
        raise AddressValueError('Invalid IPv6 address: "' + repr(ip_str) + '"')
    return (addr, scope_id)


def _ip_int_from_string(version: int, ip_str: str) -> _N:
    return _v4_int_from_string(ip_str) if version == 4 else _v6_int_from_string(ip_str)


def _compress_hextets(hextets: list[str]) -> list[str]:
    best_doublecolon_start = -1
    best_doublecolon_len = 0
    doublecolon_start = -1
    doublecolon_len = 0
    for index in range(len(hextets)):
        if hextets[index] == '0':
            doublecolon_len += 1
            if doublecolon_start == -1:
                doublecolon_start = index
            if doublecolon_len > best_doublecolon_len:
                best_doublecolon_len = doublecolon_len
                best_doublecolon_start = doublecolon_start
        else:
            doublecolon_len = 0
            doublecolon_start = -1
    if best_doublecolon_len > 1:
        best_doublecolon_end = best_doublecolon_start + best_doublecolon_len
        if best_doublecolon_end == len(hextets):
            hextets = hextets + ['']
        hextets = hextets[:best_doublecolon_start] + [''] + hextets[best_doublecolon_end:]
        if best_doublecolon_start == 0:
            hextets = [''] + hextets
    return hextets


def _string_from_ip_int(version: int, ip_int: _N) -> str:
    if version == 4:
        b = ip_int.to_bytes(4)
        return "%d.%d.%d.%d" % (b[0], b[1], b[2], b[3])
    hex_str = ip_int.hexs(32)
    hextets = [format(int(hex_str[x:x + 4], 16), "x") for x in range(0, 32, 4)]
    return ':'.join(_compress_hextets(hextets))


def _ip_int_from_prefix(version: int, prefixlen: int) -> _N:
    ones = _ones(_maxlen(version))
    return ones ^ (ones >> prefixlen)


def _prefix_from_ip_int(version: int, ip_int: _N) -> int:
    """The prefix length of the netmask ip_int (ValueError: zeroes and ones mixed)."""
    bits = _maxlen(version)
    trailing_zeroes = _count_righthand_zero_bits(ip_int, bits)
    prefixlen = bits - trailing_zeroes
    leading_ones = ip_int >> trailing_zeroes
    if leading_ones != _ones(prefixlen):
        raise ValueError('Netmask pattern ' + repr(ip_int.to_bytes(bits // 8)) + ' mixes zeroes & ones')
    return prefixlen


def _prefix_from_prefix_string(version: int, prefixlen_str: str) -> int:
    if not (prefixlen_str.isascii() and prefixlen_str.isdigit()):
        _report_invalid_netmask(repr(prefixlen_str))
    if len(prefixlen_str) > 4:
        _report_invalid_netmask(repr(prefixlen_str))
    prefixlen = int(prefixlen_str)
    if not (0 <= prefixlen <= _maxlen(version)):
        _report_invalid_netmask(repr(prefixlen_str))
    return prefixlen


def _prefix_from_ip_string(version: int, ip_str: str) -> int:
    ip_int = _n(0)
    try:
        ip_int = _ip_int_from_string(version, ip_str)
    except AddressValueError:
        _report_invalid_netmask(repr(ip_str))
    try:
        return _prefix_from_ip_int(version, ip_int)
    except ValueError:
        pass
    ip_int = ip_int ^ _ones(_maxlen(version))
    try:
        return _prefix_from_ip_int(version, ip_int)
    except ValueError:
        _report_invalid_netmask(repr(ip_str))
    return 0


def _make_netmask(version: int, arg: _M) -> tuple[_N, int]:
    """(the netmask, the prefix length) of arg: a prefix length, or (a str) a prefix length, a netmask or
    (IPv4) a hostmask."""
    prefixlen = 0
    if isinstance(arg, int):
        if not (0 <= arg <= _maxlen(version)):
            _report_invalid_netmask(repr(arg))
        prefixlen = arg
    else:
        s = str(arg)
        if version == 4:
            try:
                prefixlen = _prefix_from_prefix_string(version, s)
            except NetmaskValueError:
                prefixlen = _prefix_from_ip_string(version, s)
        else:
            prefixlen = _prefix_from_prefix_string(version, s)
    return (_ip_int_from_prefix(version, prefixlen), prefixlen)


def _split_optional_netmask(address: _A) -> list[str]:
    addr = str(address).split('/')
    if len(addr) > 2:
        raise AddressValueError("Only one '/' permitted in " + repr(address))
    return addr


def _split_addr_prefix(address: _A, maxlen: int):
    """(address, mask) of the argument of a network or an interface."""
    if isinstance(address, int) or isinstance(address, bytes) or isinstance(address, _N):
        return (address, maxlen)
    elif isinstance(address, tuple):
        return _tuple_addr_prefix(address, maxlen)
    else:
        parts = _split_optional_netmask(address)
        if len(parts) > 1:
            return (parts[0], parts[1])
        return (parts[0], str(maxlen))


def _tuple_addr_prefix(address: _A, maxlen: int):
    if sys._compiled:
        return (address[0], address[1])
    if len(address) > 1:
        return (address[0], address[1])
    return (address[0], maxlen)


def _addr_value(version: int, address: _A) -> _N:
    """The number of an address given as an int, packed bytes or (IPv4) a str."""
    if isinstance(address, _N):
        return address
    elif isinstance(address, bool) or isinstance(address, int):
        a = int(address)
        if a < 0:
            raise AddressValueError("%d (< 0) is not permitted as an IPv%d address" % (a, version))
        if version == 4 and a > 0xFFFFFFFF:
            raise AddressValueError("%d (>= 2**%d) is not permitted as an IPv%d address" % (a, 32, version))
        return _n(a)
    elif isinstance(address, bytes):
        want = 4 if version == 4 else 16
        if len(address) != want:
            raise AddressValueError("%r (len %d != %d) is not permitted as an IPv%d address"
                                    % (address, len(address), want, version))
        return _n_from_bytes(address)
    else:
        addr_str = str(address)
        if '/' in addr_str:
            raise AddressValueError("Unexpected '/' in " + repr(address))
        return _ip_int_from_string(version, addr_str)


def _check_n(version: int, n: _N, sign: str) -> _N:
    """n, if within the version's addresses (sign: '-' for a negative value of that size)."""
    if sign:
        raise AddressValueError("-%s (< 0) is not permitted as an IPv%d address" % (n.dec(), version))
    if n > _ones(_maxlen(version)):
        raise AddressValueError("%s (>= 2**%d) is not permitted as an IPv%d address"
                                % (n.dec(), _maxlen(version), version))
    return n


# ---------------------------------------------------------------- addresses

class _IPAddressBase:
    """The mother class."""

    def _base_init(self, version: int) -> None:
        self._version = version

    @property
    def version(self) -> int:
        return self._version

    @property
    def max_prefixlen(self) -> int:
        return _maxlen(self._version)

    @property
    def exploded(self) -> str:
        """Return the longhand version of the IP address as a string."""
        return self._explode_shorthand_ip_string()

    @property
    def compressed(self) -> str:
        """Return the shorthand version of the IP address as a string."""
        return str(self)

    @property
    def reverse_pointer(self) -> str:
        """The name of the reverse DNS pointer for the IP address, e.g. '1.0.0.127.in-addr.arpa'."""
        return self._reverse_pointer()

    def _explode_shorthand_ip_string(self) -> str:
        return str(self)

    def _reverse_pointer(self) -> str:
        if self._version == 4:
            octets = str(self).split('.')
            octets.reverse()
            return '.'.join(octets) + '.in-addr.arpa'
        chars = list(self.exploded.replace(':', ''))
        chars.reverse()
        return '.'.join(chars) + '.ip6.arpa'

    def _no_attr(self, name: str) -> None:
        raise AttributeError("'" + type(self).__name__ + "' object has no attribute '" + name + "'")


class _BaseAddress(_IPAddressBase):
    """A generic IP object: the version independent methods of single IP addresses."""

    def _addr_init(self, version: int) -> None:
        self._base_init(version)
        self._ip = _n(0)
        self._scope_id: str | None = None
        self._net: "_BaseNetwork | None" = None
        self._prefixlen = _maxlen(version)

    def __int__(self) -> int:
        return self._ip.to_int()

    def __index__(self) -> int:
        return self._ip.to_int()

    def __eq__(self, other: _A) -> bool:
        if not isinstance(other, _BaseAddress):
            return False
        if self._ip != other._ip or self._version != other._version:
            return False
        if self._version == 6 and self._scope_id != other._scope_id:
            return False
        if self._net is not None:
            on = other._net
            if on is None:
                return False
            return self._net == on
        return True

    def __ne__(self, other: _A) -> bool:
        return not self == other

    def __lt__(self, other: _A) -> bool:
        if not isinstance(other, _BaseAddress):
            raise TypeError("'<' not supported between instances of '" + type(self).__name__ + "' and '"
                            + type(other).__name__ + "'")
        if self._version != other._version:
            raise TypeError('%s and %s are not of the same version' % (self, other))
        n = self._net
        if n is not None:
            on = other._net
            if on is not None:
                return n < on or (n == on and self._ip < other._ip)
            return False
        if self._ip != other._ip:
            return self._ip < other._ip
        return False

    def __gt__(self, other: _A) -> bool:
        if not isinstance(other, _BaseAddress):
            raise TypeError("'>' not supported between instances of '" + type(self).__name__ + "' and '"
                            + type(other).__name__ + "'")
        return other < self

    def __le__(self, other: _A) -> bool:
        if not isinstance(other, _BaseAddress):
            raise TypeError("'<=' not supported between instances of '" + type(self).__name__ + "' and '"
                            + type(other).__name__ + "'")
        return not other < self

    def __ge__(self, other: _A) -> bool:
        if not isinstance(other, _BaseAddress):
            raise TypeError("'>=' not supported between instances of '" + type(self).__name__ + "' and '"
                            + type(other).__name__ + "'")
        return not self < other

    def __add__(self, other: int) -> "_BaseAddress":
        if other >= 0:
            return _make_addr(self._version, _check_n(self._version, self._ip + _n(other), ""))
        o = _n(-other)
        if o > self._ip:
            return _make_addr(self._version, _check_n(self._version, o - self._ip, "-"))
        return _make_addr(self._version, self._ip - o)

    def __sub__(self, other: int) -> "_BaseAddress":
        if other < 0:
            return self + -other
        o = _n(other)
        if o > self._ip:
            return _make_addr(self._version, _check_n(self._version, o - self._ip, "-"))
        return _make_addr(self._version, self._ip - o)

    def __repr__(self) -> str:
        return type(self).__name__ + "(" + repr(str(self)) + ")"

    def _addr_str(self) -> str:
        """The address alone (an interface's without its prefix length)."""
        if self._version == 4:
            return _string_from_ip_int(4, self._ip)
        m = self._mapped()
        if m is None:
            ip_str = _string_from_ip_int(6, self._ip)
        else:
            ip_str = _string_from_ip_int(6, self._ip >> 32) + ":" + _string_from_ip_int(4, m._ip)
        sid = self._scope_id
        if sid:
            return ip_str + '%' + sid
        return ip_str

    def __str__(self) -> str:
        if self._net is not None:
            return '%s/%d' % (self._addr_str(), self._prefixlen)
        return self._addr_str()

    def __hash__(self) -> int:
        n = self._net
        if n is not None:
            return hash((hash(self._ip), self._prefixlen, hash(n.network_address._ip)))
        if self._version == 6:
            return hash((hash(self._ip), self._scope_id))
        return hash(self._ip)

    def _get_address_key(self) -> tuple[int, "_BaseAddress"]:
        return (self._version, self)

    def __format__(self, fmt: str) -> str:
        """The address formatted: 's' (default) as a string; 'b' binary, 'x'/'X' hex (zero-padded),
        'n' ('b' for IPv4, 'x' for IPv6); '#' and '_' allowed for those."""
        if not fmt or fmt[-1] == 's':
            return _format_str(str(self), fmt)
        rest = fmt
        alternate = rest.startswith('#')
        if alternate:
            rest = rest[1:]
        grouping = rest.startswith('_')
        if grouping:
            rest = rest[1:]
        if rest not in ('x', 'b', 'n', 'X'):
            raise TypeError("unsupported format string passed to " + type(self).__name__ + ".__format__")
        fmt_base = rest
        if fmt_base == 'n':
            fmt_base = 'b' if self._version == 4 else 'x'
        bits = _maxlen(self._version)
        if fmt_base == 'b':
            digits = self._ip.bins(bits)
        else:
            digits = self._ip.hexs(bits // 4)
            if fmt_base == 'X':
                digits = digits.upper()
        if grouping:
            groups: list[str] = []
            for k in range(0, len(digits), 4):
                groups.append(digits[k:k + 4])
            digits = '_'.join(groups)
        if alternate:
            digits = ('0b' if fmt_base == 'b' else '0X' if fmt_base == 'X' else '0x') + digits
        return digits

    def _explode_shorthand_ip_string(self) -> str:
        if self._version == 4:
            return str(self)
        parts: list[str] = []
        hex_str = self._ip.hexs(32)
        for x in range(0, 32, 4):
            parts.append(hex_str[x:x + 4])
        s = ':'.join(parts)
        m = self._mapped()
        if m is not None:
            s = s[:30] + str(m)
        if self._net is not None:
            return '%s/%d' % (s, self._prefixlen)
        return s

    def _reverse_pointer(self) -> str:
        if self._version == 4:
            octets = self._addr_str().split('.')
            octets.reverse()
            return '.'.join(octets) + '.in-addr.arpa'
        chars = list(self._ip.hexs(32))
        chars.reverse()
        return '.'.join(chars) + '.ip6.arpa'

    @property
    def packed(self) -> bytes:
        """The binary representation of this address."""
        return self._ip.to_bytes(4 if self._version == 4 else 16)

    # ------------------------------------------------ IPv6 only

    def _mapped(self) -> "_BaseAddress | None":
        if self._version != 6 or (self._ip >> 32) != _n(0xFFFF):
            return None
        return IPv4Address(self._ip & _n(0xFFFFFFFF))

    @property
    def scope_id(self) -> str | None:
        """Identifier of a particular zone of the address's scope (None: none)."""
        if self._version == 4:
            self._no_attr('scope_id')
        return self._scope_id

    @property
    def ipv4_mapped(self) -> "_BaseAddress | None":
        """The IPv4 address of an IPv4 mapped address (::ffff:a.b.c.d), else None."""
        if self._version == 4:
            self._no_attr('ipv4_mapped')
        return self._mapped()

    @property
    def teredo(self) -> "tuple[_BaseAddress, _BaseAddress] | None":
        """(server, client) IPv4 addresses of a Teredo address (2001::/32), else None."""
        if self._version == 4:
            self._no_attr('teredo')
        if (self._ip >> 96) != _n(0x20010000):
            return None
        return (_make_addr(4, (self._ip >> 64) & _n(0xFFFFFFFF)),
                _make_addr(4, (self._ip ^ _ones(128)) & _n(0xFFFFFFFF)))

    @property
    def sixtofour(self) -> "_BaseAddress | None":
        """The IPv4 address of a 6to4 address (2002::/16), else None."""
        if self._version == 4:
            self._no_attr('sixtofour')
        if (self._ip >> 112) != _n(0x2002):
            return None
        return _make_addr(4, (self._ip >> 80) & _n(0xFFFFFFFF))

    @property
    def is_site_local(self) -> bool:
        """A site-local address (fec0::/10; deprecated by RFC 3879)."""
        if self._version == 4:
            self._no_attr('is_site_local')
        return self in _v6c().sitelocal

    # ------------------------------------------------ IPv4 only

    @property
    def ipv6_mapped(self) -> "_BaseAddress":
        """The IPv4-mapped IPv6 address (::ffff:a.b.c.d)."""
        if self._version == 6:
            self._no_attr('ipv6_mapped')
        return _make_addr(6, self._ip | (_n(0xFFFF) << 32))

    # ------------------------------------------------ interfaces only

    def _iface(self, name: str) -> "_BaseNetwork":
        n = self._net
        if n is None:
            raise AttributeError("'" + type(self).__name__ + "' object has no attribute '" + name + "'")
        return n

    @property
    def network(self) -> "_BaseNetwork":
        return self._iface('network')

    @property
    def ip(self) -> "_BaseAddress":
        self._iface('ip')
        return _make_addr(self._version, self._ip)

    @property
    def netmask(self) -> "_BaseAddress":
        return self._iface('netmask').netmask

    @property
    def hostmask(self) -> "_BaseAddress":
        return self._iface('hostmask').hostmask

    @property
    def with_prefixlen(self) -> str:
        self._iface('with_prefixlen')
        return '%s/%s' % (_string_from_ip_int(self._version, self._ip), self._prefixlen)

    @property
    def with_netmask(self) -> str:
        n = self._iface('with_netmask')
        return '%s/%s' % (_string_from_ip_int(self._version, self._ip), n.netmask)

    @property
    def with_hostmask(self) -> str:
        n = self._iface('with_hostmask')
        return '%s/%s' % (_string_from_ip_int(self._version, self._ip), n.hostmask)

    # ------------------------------------------------ properties

    @property
    def is_multicast(self) -> bool:
        """A multicast address (RFC 3171 / RFC 2373)."""
        m = self._mapped()
        if m is not None:
            return m.is_multicast
        return self in (_v4c().multicast if self._version == 4 else _v6c().multicast)

    @property
    def is_reserved(self) -> bool:
        """An address in the IETF reserved ranges."""
        m = self._mapped()
        if m is not None:
            return m.is_reserved
        if self._version == 4:
            return _in_any(self, _v4c().reserved)
        return _in_any(self, _v6c().reserved)

    @property
    def is_link_local(self) -> bool:
        """A link-local address (RFC 3927 / RFC 4291)."""
        m = self._mapped()
        if m is not None:
            return m.is_link_local
        return self in (_v4c().linklocal if self._version == 4 else _v6c().linklocal)

    @property
    def is_private(self) -> bool:
        """An address allocated for private networks (iana-ipv4/ipv6-special-registry)."""
        m = self._mapped()
        if m is not None:
            return m.is_private
        c = _v4c() if self._version == 4 else _v6c()
        return _in_any(self, c.private) and not _in_any(self, c.private_exceptions)

    @property
    def is_global(self) -> bool:
        """An address allocated for public networks."""
        m = self._mapped()
        if m is not None:
            return m.is_global
        if self._version == 4:
            return self not in _v4c().public and not self.is_private
        return not self.is_private

    @property
    def is_unspecified(self) -> bool:
        """The unspecified address (0.0.0.0, ::)."""
        m = self._mapped()
        if m is not None:
            return m.is_unspecified
        n = self._net
        if n is not None and self._version == 6:
            return not self._ip and n.is_unspecified
        return not self._ip

    @property
    def is_loopback(self) -> bool:
        """A loopback address."""
        m = self._mapped()
        if m is not None:
            return m.is_loopback
        if self._version == 4:
            return self in _v4c().loopback
        n = self._net
        if n is not None:
            return self._ip == _n(1) and n.is_loopback
        return self._ip == _n(1)


class IPv4Address(_BaseAddress):
    """Represent and manipulate single IPv4 Addresses."""

    if not sys._compiled:
        version = 4
        max_prefixlen = IPV4LENGTH

    def __init__(self, address: _A) -> None:
        """address: a str ('192.0.2.1'), an int or 4 packed bytes."""
        self._addr_init(4)
        self._ip = _addr_value(4, address)


class IPv6Address(_BaseAddress):
    """Represent and manipulate single IPv6 Addresses."""

    if not sys._compiled:
        version = 6
        max_prefixlen = IPV6LENGTH

    def __init__(self, address: _A) -> None:
        """address: a str ('2001:db8::1', 'fe80::1%eth0'), an int (< 2**63 here) or 16 packed bytes."""
        self._addr_init(6)
        if isinstance(address, str):
            if '/' in address:
                raise AddressValueError("Unexpected '/' in " + repr(address))
            addr_str, self._scope_id = _split_scope_id(address)
            self._ip = _v6_int_from_string(addr_str)
        else:
            self._ip = _addr_value(6, address)


class IPv4Interface(IPv4Address):
    """An IPv4 address with the network it is in: IPv4Interface('192.0.2.5/24')."""

    def __init__(self, address: _A) -> None:
        addr, mask = _split_addr_prefix(address, IPV4LENGTH)
        super().__init__(addr)
        net = IPv4Network((addr, mask), strict=False)
        self._net = net
        self._prefixlen = net._prefixlen


class IPv6Interface(IPv6Address):
    """An IPv6 address with the network it is in: IPv6Interface('2001:db8::5/64')."""

    def __init__(self, address: _A) -> None:
        addr, mask = _split_addr_prefix(address, IPV6LENGTH)
        super().__init__(addr)
        net = IPv6Network((addr, mask), strict=False)
        self._net = net
        self._prefixlen = net._prefixlen


def _format_str(s: str, spec: str) -> str:
    """format(s, spec) for a str spec: [[fill]align][width][.precision][s]."""
    if spec.endswith('s'):
        spec = spec[:-1]
    fill = ' '
    align = '<'
    if len(spec) >= 2 and spec[1] in '<>^':
        fill = spec[0]
        align = spec[1]
        spec = spec[2:]
    elif spec and spec[0] in '<>^':
        align = spec[0]
        spec = spec[1:]
    width_s, dot, prec_s = spec.partition('.')
    if (width_s and not width_s.isdigit()) or (dot and not prec_s.isdigit()):
        raise ValueError("Invalid format specifier '" + spec + "' for object of type 'str'")
    if dot:
        s = s[:int(prec_s)]
    width = int(width_s) if width_s else 0
    pad = width - len(s)
    if pad <= 0:
        return s
    if align == '>':
        return fill * pad + s
    if align == '^':
        return fill * (pad // 2) + s + fill * (pad - pad // 2)
    return s + fill * pad


def _make_addr(version: int, n: _N) -> _BaseAddress:
    if version == 4:
        return IPv4Address(n)
    return IPv6Address(n)


def _addr_of(version: int, address: _A) -> _BaseAddress:
    if version == 4:
        return IPv4Address(address)
    return IPv6Address(address)


def _in_any(a: _BaseAddress, nets: "list[_BaseNetwork]") -> bool:
    for net in nets:
        if a in net:
            return True
    return False


# ---------------------------------------------------------------- networks

class _BaseNetwork(_IPAddressBase):
    """A generic IP network object: the version independent methods of networks."""

    def _net_init(self, version: int, address: _A, strict: bool) -> None:
        self._base_init(version)
        addr, mask = _split_addr_prefix(address, _maxlen(version))
        self.network_address = _addr_of(version, addr)
        nm, plen = _make_netmask(version, mask)
        self.netmask = _make_addr(version, nm)
        self._prefixlen = plen
        packed = self.network_address._ip
        if packed & nm != packed:
            if strict:
                raise ValueError('%s has host bits set' % self)
            self.network_address = _make_addr(version, packed & nm)

    def __repr__(self) -> str:
        return type(self).__name__ + "(" + repr(str(self)) + ")"

    def __str__(self) -> str:
        return '%s/%d' % (self.network_address, self._prefixlen)

    def _explode_shorthand_ip_string(self) -> str:
        if self._version == 4:
            return str(self)
        return '%s/%d' % (self.network_address.exploded, self._prefixlen)

    def hosts(self):
        """The usable hosts in the network: all but the network address and the broadcast one (IPv6:
        all but the network address; /31, /127: all; /32, /128: the address)."""
        bits = _maxlen(self._version)
        network = self.network_address._ip
        broadcast = self.broadcast_address._ip
        if self._prefixlen == bits - 1:
            first = network
        elif self._prefixlen == bits:
            first = network
        else:
            first = network + _n(1)
        if self._version == 4 and self._prefixlen < bits - 1:
            last = broadcast - _n(1)
        else:
            last = broadcast
        x = first
        while x <= last:
            yield _make_addr(self._version, x)
            x = x + _n(1)

    def __iter__(self):
        network = self.network_address._ip
        broadcast = self.broadcast_address._ip
        x = network
        while x <= broadcast:
            yield _make_addr(self._version, x)
            x = x + _n(1)

    def __getitem__(self, n: int) -> _BaseAddress:
        network = self.network_address._ip
        broadcast = self.broadcast_address._ip
        if n >= 0:
            a = network + _n(n)
            if a > broadcast:
                raise IndexError('address out of range')
            return _make_addr(self._version, a)
        m = _n(-(n + 1))
        if m > broadcast - network:
            raise IndexError('address out of range')
        return _make_addr(self._version, broadcast - m)

    def __lt__(self, other: _A) -> bool:
        if not isinstance(other, _BaseNetwork):
            raise TypeError("'<' not supported between instances of '" + type(self).__name__ + "' and '"
                            + type(other).__name__ + "'")
        if self._version != other._version:
            raise TypeError('%s and %s are not of the same version' % (self, other))
        if self.network_address != other.network_address:
            return self.network_address < other.network_address
        if self.netmask != other.netmask:
            return self.netmask < other.netmask
        return False

    def __gt__(self, other: _A) -> bool:
        if not isinstance(other, _BaseNetwork):
            raise TypeError("'>' not supported between instances of '" + type(self).__name__ + "' and '"
                            + type(other).__name__ + "'")
        return other < self

    def __le__(self, other: _A) -> bool:
        if not isinstance(other, _BaseNetwork):
            raise TypeError("'<=' not supported between instances of '" + type(self).__name__ + "' and '"
                            + type(other).__name__ + "'")
        return not other < self

    def __ge__(self, other: _A) -> bool:
        if not isinstance(other, _BaseNetwork):
            raise TypeError("'>=' not supported between instances of '" + type(self).__name__ + "' and '"
                            + type(other).__name__ + "'")
        return not self < other

    def __eq__(self, other: _A) -> bool:
        if not isinstance(other, _BaseNetwork):
            return False
        return (self._version == other._version and self.network_address._ip == other.network_address._ip
                and self.netmask._ip == other.netmask._ip)

    def __ne__(self, other: _A) -> bool:
        return not self == other

    def __hash__(self) -> int:
        return hash((hash(self.network_address._ip), hash(self.netmask._ip)))

    def __contains__(self, other: _A) -> bool:
        if not isinstance(other, _BaseAddress):
            return False
        if self._version != other._version:
            return False
        return other._ip & self.netmask._ip == self.network_address._ip

    def overlaps(self, other: "_BaseNetwork") -> bool:
        """Tell if self is partly contained in other."""
        return (self.network_address in other or self.broadcast_address in other
                or other.network_address in self or other.broadcast_address in self)

    @property
    def broadcast_address(self) -> _BaseAddress:
        return _make_addr(self._version, self.network_address._ip | self.hostmask._ip)

    @property
    def hostmask(self) -> _BaseAddress:
        return _make_addr(self._version, self.netmask._ip ^ _ones(_maxlen(self._version)))

    @property
    def with_prefixlen(self) -> str:
        return '%s/%d' % (self.network_address, self._prefixlen)

    @property
    def with_netmask(self) -> str:
        return '%s/%s' % (self.network_address, self.netmask)

    @property
    def with_hostmask(self) -> str:
        return '%s/%s' % (self.network_address, self.hostmask)

    @property
    def num_addresses(self) -> int:
        """Number of hosts in the current subnet (OverflowError: 2**63 or more)."""
        return (self.broadcast_address._ip - self.network_address._ip + _n(1)).to_int()

    @property
    def prefixlen(self) -> int:
        return self._prefixlen

    def address_exclude(self, other: "_BaseNetwork"):
        """The networks of self without other (a subnet of it), as an iterator."""
        if not self._version == other._version:
            raise TypeError("%s and %s are not of the same version" % (self, other))
        if not other.subnet_of(self):
            raise ValueError('%s not contained in %s' % (other, self))
        if other == self:
            return
        other = _net_of(other._version, other.network_address._ip, other._prefixlen)
        s1, s2 = self._halves()
        while s1 != other and s2 != other:
            if other.subnet_of(s1):
                yield s2
                s1, s2 = s1._halves()
            elif other.subnet_of(s2):
                yield s1
                s1, s2 = s2._halves()
            else:
                raise AssertionError('Error performing exclusion: s1: %s s2: %s other: %s' % (s1, s2, other))
        if s1 == other:
            yield s2
        elif s2 == other:
            yield s1
        else:
            raise AssertionError('Error performing exclusion: s1: %s s2: %s other: %s' % (s1, s2, other))

    def _halves(self) -> "tuple[_BaseNetwork, _BaseNetwork]":
        plen = self._prefixlen + 1
        step = (self.hostmask._ip + _n(1)) >> 1
        a = self.network_address._ip
        return (_net_of(self._version, a, plen), _net_of(self._version, a + step, plen))

    def compare_networks(self, other: "_BaseNetwork") -> int:
        """-1, 0 or 1: self against other by network address, then by netmask."""
        if self._version != other._version:
            raise TypeError('%s and %s are not of the same type' % (self, other))
        if self.network_address < other.network_address:
            return -1
        if self.network_address > other.network_address:
            return 1
        if self.netmask < other.netmask:
            return -1
        if self.netmask > other.netmask:
            return 1
        return 0

    def _get_networks_key(self) -> tuple[int, _BaseAddress, _BaseAddress]:
        return (self._version, self.network_address, self.netmask)

    def subnets(self, prefixlen_diff: int = 1, new_prefix: int | None = None):
        """The subnets of this network (prefixlen_diff more bits, or of prefix length new_prefix)."""
        bits = _maxlen(self._version)
        if self._prefixlen == bits:
            yield self
            return
        if new_prefix is not None:
            if new_prefix < self._prefixlen:
                raise ValueError('new prefix must be longer')
            if prefixlen_diff != 1:
                raise ValueError('cannot set prefixlen_diff and new_prefix')
            prefixlen_diff = new_prefix - self._prefixlen
        if prefixlen_diff < 0:
            raise ValueError('prefix length diff must be > 0')
        new_prefixlen = self._prefixlen + prefixlen_diff
        if new_prefixlen > bits:
            raise ValueError('prefix length diff %d is invalid for netblock %s' % (new_prefixlen, self))
        start = self.network_address._ip
        end = self.broadcast_address._ip + _n(1)
        step = (self.hostmask._ip + _n(1)) >> prefixlen_diff
        x = start
        while x < end:
            yield _net_of(self._version, x, new_prefixlen)
            x = x + step

    def supernet(self, prefixlen_diff: int = 1, new_prefix: int | None = None) -> "_BaseNetwork":
        """The supernet containing this network (prefixlen_diff fewer bits, or of prefix length new_prefix)."""
        if self._prefixlen == 0:
            return self
        if new_prefix is not None:
            if new_prefix > self._prefixlen:
                raise ValueError('new prefix must be shorter')
            if prefixlen_diff != 1:
                raise ValueError('cannot set prefixlen_diff and new_prefix')
            prefixlen_diff = self._prefixlen - new_prefix
        new_prefixlen = self._prefixlen - prefixlen_diff
        if new_prefixlen < 0:
            raise ValueError('current prefixlen is %d, cannot have a prefixlen_diff of %d'
                             % (self._prefixlen, prefixlen_diff))
        return _net_of(self._version, self.network_address._ip & (self.netmask._ip << prefixlen_diff),
                       new_prefixlen)

    def subnet_of(self, other: "_BaseNetwork") -> bool:
        """Return True if this network is a subnet of other."""
        return _is_subnet_of(self, other)

    def supernet_of(self, other: "_BaseNetwork") -> bool:
        """Return True if this network is a supernet of other."""
        return _is_subnet_of(other, self)

    @property
    def is_multicast(self) -> bool:
        return self.network_address.is_multicast and self.broadcast_address.is_multicast

    @property
    def is_reserved(self) -> bool:
        return self.network_address.is_reserved and self.broadcast_address.is_reserved

    @property
    def is_link_local(self) -> bool:
        return self.network_address.is_link_local and self.broadcast_address.is_link_local

    @property
    def is_private(self) -> bool:
        c = _v4c() if self._version == 4 else _v6c()
        na = self.network_address
        ba = self.broadcast_address
        found = False
        for priv in c.private:
            if na in priv and ba in priv:
                found = True
        if not found:
            return False
        for net in c.private_exceptions:
            if na in net or ba in net:
                return False
        return True

    @property
    def is_global(self) -> bool:
        if self._version == 4:
            pub = _v4c().public
            return (not (self.network_address in pub and self.broadcast_address in pub)
                    and not self.is_private)
        return not self.is_private

    @property
    def is_unspecified(self) -> bool:
        return self.network_address.is_unspecified and self.broadcast_address.is_unspecified

    @property
    def is_loopback(self) -> bool:
        return self.network_address.is_loopback and self.broadcast_address.is_loopback

    @property
    def is_site_local(self) -> bool:
        if self._version == 4:
            self._no_attr('is_site_local')
        return self.network_address.is_site_local and self.broadcast_address.is_site_local


class IPv4Network(_BaseNetwork):
    """An IPv4 network: IPv4Network('192.0.2.0/24'), ('192.0.2.0', 24), ('192.0.2.0', '255.255.255.0');
    strict=False: host bits set are cleared (else ValueError)."""

    if not sys._compiled:
        version = 4
        max_prefixlen = IPV4LENGTH

    def __init__(self, address: _A, strict: bool = True) -> None:
        self._net_init(4, address, strict)


class IPv6Network(_BaseNetwork):
    """An IPv6 network: IPv6Network('2001:db8::/32'), ('2001:db8::', 32); strict=False: host bits set
    are cleared (else ValueError)."""

    if not sys._compiled:
        version = 6
        max_prefixlen = IPV6LENGTH

    def __init__(self, address: _A, strict: bool = True) -> None:
        self._net_init(6, address, strict)


def _net_of(version: int, n: _N, prefixlen: int) -> _BaseNetwork:
    if version == 4:
        return IPv4Network((n, prefixlen))
    return IPv6Network((n, prefixlen))


def _is_subnet_of(a: _BaseNetwork, b: _BaseNetwork) -> bool:
    if a._version != b._version:
        raise TypeError(str(a) + " and " + str(b) + " are not of the same version")
    return b.network_address <= a.network_address and b.broadcast_address >= a.broadcast_address


class _Constants:
    """The special networks of an IP version."""

    def __init__(self, version: int, linklocal: str, multicast: str, private: list[str],
                 private_exceptions: list[str], reserved: list[str], public: str, loopback: str,
                 sitelocal: str) -> None:
        self.linklocal = _str_net(version, linklocal)
        self.multicast = _str_net(version, multicast)
        self.public = _str_net(version, public)
        self.loopback = _str_net(version, loopback)
        self.sitelocal = _str_net(version, sitelocal)
        self.private = [_str_net(version, x) for x in private]
        self.private_exceptions = [_str_net(version, x) for x in private_exceptions]
        self.reserved = [_str_net(version, x) for x in reserved]


def _str_net(version: int, s: str) -> _BaseNetwork:
    if version == 4:
        return IPv4Network(s)
    return IPv6Network(s)


_V4C: list[_Constants] = []
_V6C: list[_Constants] = []


def _v4c() -> _Constants:
    if not _V4C:
        _V4C.append(_Constants(4, '169.254.0.0/16', '224.0.0.0/4', [
            '0.0.0.0/8', '10.0.0.0/8', '127.0.0.0/8', '169.254.0.0/16', '172.16.0.0/12', '192.0.0.0/24',
            '192.0.0.170/31', '192.0.2.0/24', '192.168.0.0/16', '198.18.0.0/15', '198.51.100.0/24',
            '203.0.113.0/24', '240.0.0.0/4', '255.255.255.255/32'],
            ['192.0.0.9/32', '192.0.0.10/32'], ['240.0.0.0/4'], '100.64.0.0/10', '127.0.0.0/8', '0.0.0.0/32'))
    return _V4C[0]


def _v6c() -> _Constants:
    if not _V6C:
        _V6C.append(_Constants(6, 'fe80::/10', 'ff00::/8', [
            '::1/128', '::/128', '::ffff:0:0/96', '64:ff9b:1::/48', '100::/64', '2001::/23', '2001:db8::/32',
            '2002::/16', '3fff::/20', 'fc00::/7', 'fe80::/10'],
            ['2001:1::1/128', '2001:1::2/128', '2001:3::/32', '2001:4:112::/48', '2001:20::/28',
             '2001:30::/28'],
            ['::/8', '100::/8', '200::/7', '400::/6', '800::/5', '1000::/4', '4000::/3', '6000::/3',
             '8000::/3', 'A000::/3', 'C000::/3', 'E000::/4', 'F000::/5', 'F800::/6', 'FE00::/9'],
            '::/128', '::1/128', 'fec0::/10'))
    return _V6C[0]


# ---------------------------------------------------------------- functions

def ip_address(address: _A) -> _BaseAddress:
    """An IPv4Address or IPv6Address object for address (a str or an int)."""
    try:
        return IPv4Address(address)
    except (AddressValueError, NetmaskValueError):
        pass
    try:
        return IPv6Address(address)
    except (AddressValueError, NetmaskValueError):
        pass
    raise ValueError(repr(address) + ' does not appear to be an IPv4 or IPv6 address')


def ip_network(address: _A, strict: bool = True) -> _BaseNetwork:
    """An IPv4Network or IPv6Network object for address (a str, an int or an (address, mask) tuple)."""
    try:
        return IPv4Network(address, strict)
    except (AddressValueError, NetmaskValueError):
        pass
    try:
        return IPv6Network(address, strict)
    except (AddressValueError, NetmaskValueError):
        pass
    raise ValueError(repr(address) + ' does not appear to be an IPv4 or IPv6 network')


def ip_interface(address: _A) -> _BaseAddress:
    """An IPv4Interface or IPv6Interface object for address ('192.0.2.5/24')."""
    try:
        return IPv4Interface(address)
    except (AddressValueError, NetmaskValueError):
        pass
    try:
        return IPv6Interface(address)
    except (AddressValueError, NetmaskValueError):
        pass
    raise ValueError(repr(address) + ' does not appear to be an IPv4 or IPv6 interface')


def v4_int_to_packed(address: int) -> bytes:
    """The address (an int) as 4 packed bytes in network (big-endian) order."""
    if address < 0 or address > 0xFFFFFFFF:
        raise ValueError("Address negative or too large for IPv4")
    return address.to_bytes(4, "big")


def v6_int_to_packed(address: int) -> bytes:
    """The address (an int) as 16 packed bytes in network (big-endian) order."""
    if address < 0:
        raise ValueError("Address negative or too large for IPv6")
    return address.to_bytes(16, "big")


def _find_address_range(addresses: list[_BaseAddress]):
    """(first, last) of the runs of consecutive addresses in the sorted list addresses."""
    first = addresses[0]
    last = first
    for ip in addresses[1:]:
        if ip._ip != last._ip + _n(1):
            yield (first, last)
            first = ip
        last = ip
    yield (first, last)


def summarize_address_range(first: _BaseAddress, last: _BaseAddress):
    """The networks covering the addresses from first to last, as an iterator."""
    if first._version != last._version:
        raise TypeError("%s and %s are not of the same version" % (first, last))
    if first > last:
        raise ValueError('last IP address must be greater than first')
    ip_bits = _maxlen(first._version)
    first_int = first._ip
    last_int = last._ip
    ones = _ones(ip_bits)
    while first_int <= last_int:
        nbits = min(_count_righthand_zero_bits(first_int, ip_bits),
                    (last_int - first_int + _n(1)).bit_length() - 1)
        yield _net_of(first._version, first_int, ip_bits - nbits)
        first_int = first_int + (_n(1) << nbits)
        if first_int - _n(1) == ones:
            break


def _collapse_addresses_internal(addresses: list[_BaseNetwork]) -> list[_BaseNetwork]:
    to_merge = list(addresses)
    subnets: dict[_BaseNetwork, _BaseNetwork] = {}
    while to_merge:
        net = to_merge.pop()
        supernet = net.supernet()
        existing = subnets.get(supernet)
        if existing is None:
            subnets[supernet] = net
        elif existing != net:
            del subnets[supernet]
            to_merge.append(supernet)
    out: list[_BaseNetwork] = []
    for net in sorted(subnets.values()):
        if out:
            if out[-1].broadcast_address >= net.broadcast_address:
                continue
        out.append(net)
    return out


def collapse_addresses(addresses: _A):
    """The networks of the addresses and networks given, collapsed, as an iterator:
    [IPv4Network('192.0.2.0/25'), IPv4Network('192.0.2.128/25')] -> IPv4Network('192.0.2.0/24')."""
    addrs: list[_BaseNetwork] = []
    ips: list[_BaseAddress] = []
    nets: list[_BaseNetwork] = []
    for ip in addresses:
        if isinstance(ip, _BaseAddress):
            if ips and ips[-1]._version != ip._version:
                raise TypeError("%s and %s are not of the same version" % (ip, ips[-1]))
            ips.append(_make_addr(ip._version, ip._ip) if ip._net is not None else ip)
        elif ip._prefixlen == _maxlen(ip._version):
            if ips and ips[-1]._version != ip._version:
                raise TypeError("%s and %s are not of the same version" % (ip, ips[-1]))
            ips.append(ip.network_address)
        else:
            if nets and nets[-1]._version != ip._version:
                raise TypeError("%s and %s are not of the same version" % (ip, nets[-1]))
            nets.append(ip)
    ips = sorted(set(ips))
    if ips:
        for first, last in _find_address_range(ips):
            addrs.extend(summarize_address_range(first, last))
    return iter(_collapse_addresses_internal(addrs + nets))


def get_mixed_type_key(obj: _A):
    """A key for sorting addresses and networks together: sorted(xs, key=get_mixed_type_key)."""
    if isinstance(obj, _BaseNetwork):
        return obj._get_networks_key()
    else:
        return obj._get_address_key()
