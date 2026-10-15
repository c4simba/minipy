"""Open URLs (CPython's urllib.request, its urlopen and Request): http:// over
http.client, redirects followed, errors as urllib.error's. No https (no TLS)."""
from urllib.parse import urlsplit, urljoin, unquote
from urllib.error import URLError, HTTPError
from http.client import HTTPConnection, HTTPResponse

_USER_AGENT = "Python-urllib/3.14"
_MAX_REDIRECTIONS = 10


class Request:
    """What urlopen() opens: the URL, data (a body: POST), headers, method."""

    def __init__(self, url: str, data: bytes | None = None, headers: dict[str, str] | None = None,
                 origin_req_host: str | None = None, unverifiable: bool = False, method: str | None = None):
        self.full_url = url
        self.data = data
        self.headers: dict[str, str] = {}
        self.unredirected_hdrs: dict[str, str] = {}
        if headers is not None:
            for k in headers:
                self.add_header(k, headers[k])
        self.origin_req_host = origin_req_host
        self.unverifiable = unverifiable
        self.method = method

    @property
    def full_url(self) -> str:
        if self.fragment:
            return self._full_url + "#" + self.fragment
        return self._full_url

    @full_url.setter
    def full_url(self, url: str) -> None:
        url = url.strip()
        base, sep, frag = url.partition("#")
        self._full_url = base
        self.fragment = frag if sep else ""
        parts = urlsplit(base)
        if not parts.scheme:
            raise ValueError("unknown url type: " + repr(base))
        self.type = parts.scheme
        self.host = unquote(parts.netloc)
        self.selector = parts.path or "/"
        if parts.query:
            self.selector += "?" + parts.query

    def get_method(self) -> str:
        if self.method is not None:
            return self.method
        return "POST" if self.data is not None else "GET"

    def get_full_url(self) -> str:
        return self.full_url

    def add_header(self, key: str, val: str) -> None:
        self.headers[key.capitalize()] = val

    def add_unredirected_header(self, key: str, val: str) -> None:
        self.unredirected_hdrs[key.capitalize()] = val

    def has_header(self, header_name: str) -> bool:
        return header_name in self.headers or header_name in self.unredirected_hdrs

    def get_header(self, header_name: str, default: str | None = None) -> str | None:
        if header_name in self.headers:
            return self.headers[header_name]
        if header_name in self.unredirected_hdrs:
            return self.unredirected_hdrs[header_name]
        return default

    def remove_header(self, header_name: str) -> None:
        if header_name in self.headers:
            del self.headers[header_name]
        if header_name in self.unredirected_hdrs:
            del self.unredirected_hdrs[header_name]

    def header_items(self) -> list[tuple[str, str]]:
        hdrs = dict(self.unredirected_hdrs)
        for k in self.headers:
            hdrs[k] = self.headers[k]
        return list(hdrs.items())


def _open(req: Request, timeout: float | None) -> HTTPResponse:
    if req.type != "http":
        raise URLError("unknown url type: " + req.type)
    if not req.host:
        raise URLError("no host given")
    headers: dict[str, str] = {}
    for k, v in req.header_items():
        headers[k.title()] = v
    if req.data is not None and "Content-Type" not in headers:
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    if "User-Agent" not in headers:
        headers["User-Agent"] = _USER_AGENT
    headers["Connection"] = "close"
    conn = HTTPConnection(req.host, timeout=timeout)
    try:
        conn.request(req.get_method(), req.selector, req.data, headers)
        r = conn.getresponse()
    except OSError as err:
        conn.close()
        raise URLError(str(err))
    r.url = req.get_full_url()
    return r


def urlopen(url, data: bytes | None = None, timeout: float | None = -1.0) -> HTTPResponse:
    """The response to a request of url (a str or a Request): status, headers, read() ...
    Redirections are followed; an error status raises HTTPError."""
    if isinstance(url, str):
        req = Request(url, data)
    else:
        req = url
        if data is not None:
            req.data = data
    seen = 0
    while True:
        r = _open(req, timeout)
        code = r.status
        if 200 <= code < 300:
            return r
        location = r.getheader("location") or r.getheader("uri")
        if code in (301, 302, 303, 307, 308) and location is not None:
            seen += 1
            if seen > _MAX_REDIRECTIONS:
                raise HTTPError(req.full_url, code, "The HTTP server returned a redirect error that would lead to an infinite loop.\nThe last 30x error message was:\n" + r.reason, r.headers, r)
            new_url = urljoin(req.full_url, location)
            r.read()
            r.close()
            parts = urlsplit(new_url)
            if parts.scheme not in ("http", "https", "ftp", ""):
                raise HTTPError(new_url, code, "Redirection to url '" + new_url + "' is not allowed", r.headers, None)
            method = req.get_method()
            if code in (307, 308):
                new = Request(new_url, req.data, dict(req.headers), req.origin_req_host, True, method)
            else:
                hdrs: dict[str, str] = {}
                for k in req.headers:
                    if k.lower() not in ("content-length", "content-type"):
                        hdrs[k] = req.headers[k]
                new = Request(new_url, None, hdrs, req.origin_req_host, True, "HEAD" if method == "HEAD" else "GET")
            req = new
            continue
        raise HTTPError(req.full_url, code, r.reason, r.headers, r)
