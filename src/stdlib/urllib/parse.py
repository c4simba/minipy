"""Parse URLs into components, put them back together, quote and unquote (CPython's
urllib.parse, for str URLs)."""
import sys

__all__ = ["urlparse", "urlunparse", "urljoin", "urldefrag", "urlsplit", "urlunsplit", "urlencode", "parse_qs",
           "parse_qsl", "quote", "quote_plus", "quote_from_bytes", "unquote", "unquote_plus", "unquote_to_bytes",
           "DefragResult", "ParseResult", "SplitResult"]

uses_relative = ["", "ftp", "http", "gopher", "nntp", "imap", "wais", "file", "https", "shttp", "mms", "prospero", "rtsp",
                 "rtsps", "rtspu", "sftp", "svn", "svn+ssh", "ws", "wss"]
uses_netloc = ["", "ftp", "http", "gopher", "nntp", "telnet", "imap", "wais", "file", "mms", "https", "shttp", "snews",
               "prospero", "rtsp", "rtsps", "rtspu", "rsync", "svn", "svn+ssh", "sftp", "nfs", "git", "git+ssh", "ws",
               "wss", "itms-services"]
uses_params = ["", "ftp", "hdl", "prospero", "http", "imap", "https", "shttp", "rtsp", "rtsps", "rtspu", "sip", "sips",
               "mms", "sftp", "tel"]
non_hierarchical = ["gopher", "hdl", "mailto", "news", "telnet", "wais", "imap", "snews", "sip", "sips"]
uses_query = ["", "http", "wais", "imap", "https", "shttp", "mms", "gopher", "rtsp", "rtsps", "rtspu", "sip", "sips"]
uses_fragment = ["", "ftp", "hdl", "http", "gopher", "news", "nntp", "wais", "https", "shttp", "snews", "file",
                 "prospero"]
scheme_chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+-."

_WHATWG_C0_CONTROL_OR_SPACE = ("\x00\x01\x02\x03\x04\x05\x06\x07\x08\t\n\x0b\x0c\r\x0e\x0f\x10\x11\x12\x13\x14\x15\x16"
                               "\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f ")


def clear_cache() -> None:
    pass


# ---------------------------------------------------------------- results

def _userinfo(netloc: str) -> tuple[str | None, str | None]:
    userinfo, have_info, hostinfo = netloc.rpartition("@")
    if have_info:
        username, have_password, password = userinfo.partition(":")
        if not have_password:
            return (username, None)
        return (username, password)
    return (None, None)


def _hostinfo(netloc: str) -> tuple[str, str | None]:
    hostinfo = netloc.rpartition("@")[2]
    before, have_open_br, bracketed = hostinfo.partition("[")
    if have_open_br:
        hostname, sep, port = bracketed.partition("]")
        port = port.partition(":")[2]
    else:
        hostname, sep, port = hostinfo.partition(":")
    if not port:
        return (hostname, None)
    return (hostname, port)


class _NetlocResult:
    """username, password, hostname and port of a result's netloc."""

    def __init__(self, netloc: str):
        self.netloc = netloc

    @property
    def username(self) -> str | None:
        return _userinfo(self.netloc)[0]

    @property
    def password(self) -> str | None:
        return _userinfo(self.netloc)[1]

    @property
    def hostname(self) -> str | None:
        hostname = _hostinfo(self.netloc)[0]
        if not hostname:
            return None
        host, percent, zone = hostname.partition("%")
        return host.lower() + percent + zone

    @property
    def port(self) -> int | None:
        port = _hostinfo(self.netloc)[1]
        if port is None:
            return None
        if not (port.isdigit() and port.isascii()):
            raise ValueError("Port could not be cast to integer value as " + repr(port))
        p = int(port)
        if not (0 <= p <= 65535):
            raise ValueError("Port out of range 0-65535")
        return p


class SplitResult(_NetlocResult):
    """urlsplit()'s (scheme, netloc, path, query, fragment)."""

    def __init__(self, scheme: str, netloc: str, path: str, query: str, fragment: str):
        super().__init__(netloc)
        self.scheme = scheme
        self.path = path
        self.query = query
        self.fragment = fragment

    def _tuple(self) -> tuple[str, str, str, str, str]:
        return (self.scheme, self.netloc, self.path, self.query, self.fragment)

    def __getitem__(self, i: int) -> str:
        k = i + 5 if i < 0 else i
        if k == 0:
            return self.scheme
        if k == 1:
            return self.netloc
        if k == 2:
            return self.path
        if k == 3:
            return self.query
        if k == 4:
            return self.fragment
        raise IndexError("tuple index out of range")

    def __len__(self) -> int:
        return 5

    def __iter__(self):
        for k in range(5):
            yield self[k]

    def __eq__(self, other: "SplitResult") -> bool:
        if not sys._compiled:
            if isinstance(other, tuple):
                return self._tuple() == other
        return self._tuple() == other._tuple()

    def __hash__(self) -> int:
        return hash(self._tuple())

    def __repr__(self) -> str:
        return ("SplitResult(scheme=" + repr(self.scheme) + ", netloc=" + repr(self.netloc) + ", path=" + repr(self.path)
                + ", query=" + repr(self.query) + ", fragment=" + repr(self.fragment) + ")")

    def geturl(self) -> str:
        return urlunsplit(self._tuple())

    def _replace(self, scheme: str | None = None, netloc: str | None = None, path: str | None = None,
                 query: str | None = None, fragment: str | None = None) -> "SplitResult":
        return SplitResult(self.scheme if scheme is None else scheme, self.netloc if netloc is None else netloc,
                           self.path if path is None else path, self.query if query is None else query,
                           self.fragment if fragment is None else fragment)


class ParseResult(_NetlocResult):
    """urlparse()'s (scheme, netloc, path, params, query, fragment)."""

    def __init__(self, scheme: str, netloc: str, path: str, params: str, query: str, fragment: str):
        super().__init__(netloc)
        self.scheme = scheme
        self.path = path
        self.params = params
        self.query = query
        self.fragment = fragment

    def _tuple(self) -> tuple[str, str, str, str, str, str]:
        return (self.scheme, self.netloc, self.path, self.params, self.query, self.fragment)

    def __getitem__(self, i: int) -> str:
        k = i + 6 if i < 0 else i
        if k == 0:
            return self.scheme
        if k == 1:
            return self.netloc
        if k == 2:
            return self.path
        if k == 3:
            return self.params
        if k == 4:
            return self.query
        if k == 5:
            return self.fragment
        raise IndexError("tuple index out of range")

    def __len__(self) -> int:
        return 6

    def __iter__(self):
        for k in range(6):
            yield self[k]

    def __eq__(self, other: "ParseResult") -> bool:
        if not sys._compiled:
            if isinstance(other, tuple):
                return self._tuple() == other
        return self._tuple() == other._tuple()

    def __hash__(self) -> int:
        return hash(self._tuple())

    def __repr__(self) -> str:
        return ("ParseResult(scheme=" + repr(self.scheme) + ", netloc=" + repr(self.netloc) + ", path=" + repr(self.path)
                + ", params=" + repr(self.params) + ", query=" + repr(self.query) + ", fragment=" + repr(self.fragment)
                + ")")

    def geturl(self) -> str:
        return urlunparse(self._tuple())

    def _replace(self, scheme: str | None = None, netloc: str | None = None, path: str | None = None,
                 params: str | None = None, query: str | None = None, fragment: str | None = None) -> "ParseResult":
        return ParseResult(self.scheme if scheme is None else scheme, self.netloc if netloc is None else netloc,
                           self.path if path is None else path, self.params if params is None else params,
                           self.query if query is None else query, self.fragment if fragment is None else fragment)


class DefragResult:
    """urldefrag()'s (url, fragment)."""

    def __init__(self, url: str, fragment: str):
        self.url = url
        self.fragment = fragment

    def __getitem__(self, i: int) -> str:
        k = i + 2 if i < 0 else i
        if k == 0:
            return self.url
        if k == 1:
            return self.fragment
        raise IndexError("tuple index out of range")

    def __len__(self) -> int:
        return 2

    def __iter__(self):
        yield self.url
        yield self.fragment

    def __eq__(self, other: "DefragResult") -> bool:
        if not sys._compiled:
            if isinstance(other, tuple):
                return (self.url, self.fragment) == other
        return self.url == other.url and self.fragment == other.fragment

    def __repr__(self) -> str:
        return "DefragResult(url=" + repr(self.url) + ", fragment=" + repr(self.fragment) + ")"

    def geturl(self) -> str:
        if self.fragment:
            return self.url + "#" + self.fragment
        return self.url


# ---------------------------------------------------------------- splitting and joining

def _splitparams(url: str) -> tuple[str, str | None]:
    if "/" in url:
        i = url.find(";", url.rfind("/"))
        if i < 0:
            return (url, None)
    else:
        i = url.find(";")
    return (url[:i], url[i + 1:])


def _splitnetloc(url: str, start: int) -> tuple[str, str]:
    delim = len(url)
    for c in "/?#":
        w = url.find(c, start)
        if w >= 0:
            delim = min(delim, w)
    return (url[start:delim], url[delim:])


def _check_bracketed_netloc(netloc: str) -> None:
    hostname_and_port = netloc.rpartition("@")[2]
    before_bracket, have_open_br, bracketed = hostname_and_port.partition("[")
    if have_open_br:
        if before_bracket:
            raise ValueError("Invalid IPv6 URL")
        hostname, sep, port = bracketed.partition("]")
        if port and not port.startswith(":"):
            raise ValueError("Invalid IPv6 URL")
    else:
        hostname = hostname_and_port.partition(":")[0]
    _check_bracketed_host(hostname)


def _check_bracketed_host(hostname: str) -> None:
    if hostname.startswith("v"):
        dot = hostname.find(".")
        hexpart = hostname[1:dot] if dot > 0 else ""
        if dot < 0 or dot == len(hostname) - 1 or not hexpart or any(c not in "0123456789abcdefABCDEF" for c in hexpart):
            raise ValueError("IPvFuture address is invalid")
        return
    if ":" not in hostname:
        for part in hostname.split("."):
            if not part.isdigit():
                raise ValueError("'" + hostname + "' does not appear to be an IPv4 or IPv6 address")
        raise ValueError("An IPv4 address cannot be in brackets")
    addr = hostname.partition("%")[0]
    for c in addr:
        if c not in "0123456789abcdefABCDEF:.":
            raise ValueError("'" + hostname + "' does not appear to be an IPv4 or IPv6 address")


def _urlsplit(url: str, scheme: str | None, allow_fragments: bool) -> tuple[str | None, str | None, str, str | None, str | None]:
    url = url.lstrip(_WHATWG_C0_CONTROL_OR_SPACE)
    for b in "\t\r\n":
        url = url.replace(b, "")
    if scheme is not None:
        scheme = scheme.strip(_WHATWG_C0_CONTROL_OR_SPACE)
        for b in "\t\r\n":
            scheme = scheme.replace(b, "")
    netloc: str | None = None
    query: str | None = None
    fragment: str | None = None
    i = url.find(":")
    if i > 0 and url[0].isascii() and url[0].isalpha():
        ok = True
        for c in url[:i]:
            if c not in scheme_chars:
                ok = False
                break
        if ok:
            scheme = url[:i].lower()
            url = url[i + 1:]
    if url[:2] == "//":
        netloc, url = _splitnetloc(url, 2)
        if ("[" in netloc and "]" not in netloc) or ("]" in netloc and "[" not in netloc):
            raise ValueError("Invalid IPv6 URL")
        if "[" in netloc and "]" in netloc:
            _check_bracketed_netloc(netloc)
    if allow_fragments and "#" in url:
        url, fragment = url.split("#", 1)
    if "?" in url:
        url, query = url.split("?", 1)
    return (scheme, netloc, url, query, fragment)


def _or(s: str | None) -> str:
    return "" if s is None else s


def urlsplit(url: str, scheme: str = "", allow_fragments: bool = True) -> SplitResult:
    """(scheme, netloc, path, query, fragment) of url: <scheme>://<netloc>/<path>?<query>#<fragment>."""
    s, n, p, q, f = _urlsplit(url, scheme, allow_fragments)
    return SplitResult(_or(s), _or(n), p, _or(q), _or(f))


def urlparse(url: str, scheme: str = "", allow_fragments: bool = True) -> ParseResult:
    """(scheme, netloc, path, params, query, fragment) of url: <scheme>://<netloc>/<path>;<params>?<query>#<fragment>."""
    s, n, p, q, f = _urlsplit(url, scheme, allow_fragments)
    params: str | None = None
    if _or(s) in uses_params and ";" in p:
        p, params = _splitparams(p)
    return ParseResult(_or(s), _or(n), p, _or(params), _or(q), _or(f))


def _urlunsplit(scheme: str | None, netloc: str | None, url: str, query: str | None, fragment: str | None) -> str:
    if netloc is not None:
        if url and url[:1] != "/":
            url = "/" + url
        url = "//" + netloc + url
    elif url[:2] == "//":
        url = "//" + url
    if scheme:
        url = scheme + ":" + url
    if query is not None:
        url = url + "?" + query
    if fragment is not None:
        url = url + "#" + fragment
    return url


def _none_if_empty(s: str) -> str | None:
    return s if s else None


def urlunsplit(components) -> str:
    """The URL of (scheme, netloc, path, query, fragment)."""
    scheme = components[0]
    netloc: str | None = components[1]
    url = components[2]
    if not netloc:
        if scheme and scheme in uses_netloc and (not url or url[:1] == "/"):
            netloc = ""
        else:
            netloc = None
    return _urlunsplit(_none_if_empty(scheme), netloc, url, _none_if_empty(components[3]), _none_if_empty(components[4]))


def urlunparse(components) -> str:
    """The URL of (scheme, netloc, path, params, query, fragment)."""
    scheme = components[0]
    netloc: str | None = components[1]
    url = components[2]
    params = components[3]
    if not netloc:
        if scheme and scheme in uses_netloc and (not url or url[:1] == "/"):
            netloc = ""
        else:
            netloc = None
    if params:
        url = url + ";" + params
    return _urlunsplit(_none_if_empty(scheme), netloc, url, _none_if_empty(components[4]), _none_if_empty(components[5]))


def urljoin(base: str, url: str, allow_fragments: bool = True) -> str:
    """url (maybe relative) made absolute from base."""
    if not base:
        return url
    if not url:
        return base
    bscheme, bnetloc, bpath, bquery, bfragment = _urlsplit(base, None, allow_fragments)
    scheme, netloc, path, query, fragment = _urlsplit(url, None, allow_fragments)
    if scheme is None:
        scheme = bscheme
    if scheme != bscheme or (scheme and scheme not in uses_relative):
        return url
    if not scheme or scheme in uses_netloc:
        if netloc:
            return _urlunsplit(scheme, netloc, path, query, fragment)
        netloc = bnetloc
    if not path:
        path = bpath
        if query is None:
            query = bquery
            if fragment is None:
                fragment = bfragment
        return _urlunsplit(scheme, netloc, path, query, fragment)
    base_parts = bpath.split("/")
    if base_parts[-1] != "":
        del base_parts[-1]
    if path[:1] == "/":
        segments = path.split("/")
    else:
        segments = base_parts + path.split("/")
        segments = segments[:1] + [s for s in segments[1:-1] if s] + segments[-1:]
    resolved: list[str] = []
    for seg in segments:
        if seg == "..":
            if resolved:
                resolved.pop()
        elif seg == ".":
            continue
        else:
            resolved.append(seg)
    if segments[-1] in (".", ".."):
        resolved.append("")
    return _urlunsplit(scheme, netloc, "/".join(resolved) or "/", query, fragment)


def urldefrag(url: str) -> DefragResult:
    """(url without its fragment, the fragment)."""
    if "#" in url:
        s, n, p, q, frag = _urlsplit(url, None, True)
        return DefragResult(_urlunsplit(s, n, p, q, None), _or(frag))
    return DefragResult(url, "")


# ---------------------------------------------------------------- quoting

_ALWAYS_SAFE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-~"
_HEX = "0123456789ABCDEF"


def quote_from_bytes(bs: bytes, safe: str = "/") -> str:
    """bs with the bytes outside the safe set as %XX: quote_from_bytes(b'abc def?') -> 'abc%20def%3F'."""
    if not bs:
        return ""
    out: list[str] = []
    for b in bs:
        c = chr(b)
        if b < 128 and (c in _ALWAYS_SAFE or c in safe):
            out.append(c)
        else:
            out.append("%" + _HEX[b >> 4] + _HEX[b & 15])
    return "".join(out)


def quote(string: str, safe: str = "/", encoding: str | None = None, errors: str | None = None) -> str:
    """string with the characters outside the safe set as %XX of their UTF-8: quote('abc def') -> 'abc%20def'."""
    if not sys._compiled:
        if isinstance(string, bytes):
            if encoding is not None:
                raise TypeError("quote() doesn't support 'encoding' for bytes")
            if errors is not None:
                raise TypeError("quote() doesn't support 'errors' for bytes")
            return quote_from_bytes(string, safe)
    if not string:
        return string
    return quote_from_bytes(string.encode("utf-8" if encoding is None else encoding, "strict" if errors is None else errors), safe)


def quote_plus(string: str, safe: str = "", encoding: str | None = None, errors: str | None = None) -> str:
    """quote() with + for space (HTML form values)."""
    if " " not in string:
        return quote(string, safe, encoding, errors)
    return quote(string, safe + " ", encoding, errors).replace(" ", "+")


def _hexval(c: int) -> int:
    if 48 <= c <= 57:
        return c - 48
    if 65 <= c <= 70:
        return c - 55
    if 97 <= c <= 102:
        return c - 87
    return -1


def unquote_to_bytes(string: str) -> bytes:
    """The bytes of string with its %XX escapes: unquote_to_bytes('abc%20def') -> b'abc def'."""
    if not sys._compiled:
        if isinstance(string, bytes):
            return _unquote_bytes(string)
    return _unquote_bytes(string.encode("utf-8"))


def _unquote_bytes(b: bytes) -> bytes:
    if b"%" not in b:
        return b
    out: list[int] = []
    n = len(b)
    i = 0
    while i < n:
        c = b[i]
        if c == 37 and i + 2 < n:
            hi = _hexval(b[i + 1])
            lo = _hexval(b[i + 2])
            if hi >= 0 and lo >= 0:
                out.append(hi * 16 + lo)
                i += 3
                continue
        out.append(c)
        i += 1
    return bytes(out)


def unquote(string: str, encoding: str = "utf-8", errors: str = "replace") -> str:
    """string with its %XX escapes decoded (as encoding): unquote('abc%20def') -> 'abc def'."""
    if not sys._compiled:
        if isinstance(string, bytes):
            return _unquote_bytes(string).decode(encoding, errors)
    if "%" not in string:
        return string
    out: list[str] = []
    n = len(string)
    i = 0
    while i < n:
        j = i
        while j < n and ord(string[j]) < 128:
            j += 1
        if j > i:
            out.append(_unquote_bytes(string[i:j].encode("ascii")).decode(encoding, errors))
        k = j
        while k < n and ord(string[k]) >= 128:
            k += 1
        out.append(string[j:k])
        i = k
    return "".join(out)


def unquote_plus(string: str, encoding: str = "utf-8", errors: str = "replace") -> str:
    """unquote() with + as space."""
    return unquote(string.replace("+", " "), encoding, errors)


# ---------------------------------------------------------------- queries

def parse_qsl(qs: str, keep_blank_values: bool = False, strict_parsing: bool = False, encoding: str = "utf-8",
              errors: str = "replace", max_num_fields: int | None = None, separator: str = "&") -> list[tuple[str, str]]:
    """The (name, value) pairs of a query string: 'a=1&b=2' -> [('a', '1'), ('b', '2')]."""
    if not separator:
        raise ValueError("Separator must be of type string or bytes.")
    if not qs:
        return []
    if max_num_fields is not None:
        if max_num_fields < 1 + qs.count(separator):
            raise ValueError("Max number of fields exceeded")
    r: list[tuple[str, str]] = []
    for name_value in qs.split(separator):
        if name_value or strict_parsing:
            name, has_eq, value = name_value.partition("=")
            if not has_eq and strict_parsing:
                raise ValueError("bad query field: " + repr(name_value))
            if value or keep_blank_values:
                r.append((unquote_plus(name, encoding, errors), unquote_plus(value, encoding, errors)))
    return r


def parse_qs(qs: str, keep_blank_values: bool = False, strict_parsing: bool = False, encoding: str = "utf-8",
             errors: str = "replace", max_num_fields: int | None = None, separator: str = "&") -> dict[str, list[str]]:
    """The values of each name in a query string: 'a=1&a=2' -> {'a': ['1', '2']}."""
    result: dict[str, list[str]] = {}
    for name, value in parse_qsl(qs, keep_blank_values, strict_parsing, encoding, errors, max_num_fields, separator):
        if name in result:
            result[name].append(value)
        else:
            result[name] = [value]
    return result


def _quote_value(v, safe: str, encoding: str | None, errors: str | None, plus: bool) -> str:
    if isinstance(v, str):
        s = v
    else:
        s = str(v)
    if plus:
        return quote_plus(s, safe, encoding, errors)
    return quote(s, safe, encoding, errors)


def urlencode(query, doseq: bool = False, safe: str = "", encoding: str | None = None, errors: str | None = None,
              quote_via=None) -> str:
    """A query string of a dict or a list of (name, value) pairs: {'a': 1, 'b': 'x y'} -> 'a=1&b=x+y'.
    doseq: a list value gives a name=item per item. quote_via: quote_plus (the default) or quote."""
    if quote_via is None:
        plus = True
    else:
        plus = quote_via is quote_plus
    if isinstance(query, dict):
        pairs = list(query.items())
    else:
        pairs = list(query)
    out: list[str] = []
    for k, v in pairs:
        key = _quote_value(k, safe, encoding, errors, plus)
        if doseq and isinstance(v, (list, tuple)):
            for elt in v:
                out.append(key + "=" + _quote_value(elt, safe, encoding, errors, plus))
        else:
            out.append(key + "=" + _quote_value(v, safe, encoding, errors, plus))
    return "&".join(out)
