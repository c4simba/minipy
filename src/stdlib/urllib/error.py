"""Exceptions of urllib.request (CPython's urllib.error)."""
from http.client import HTTPResponse, HTTPMessage


class URLError(OSError):
    """A URL could not be opened: reason says why."""

    def __init__(self, reason: str, filename: str | None = None):
        super().__init__(reason)
        self.reason = reason
        self.filename = filename

    def __str__(self) -> str:
        return "<urlopen error " + self.reason + ">"


class HTTPError(URLError):
    """The server answered with an error status: code, reason (msg), headers; read() gives the body."""

    def __init__(self, url: str, code: int, msg: str, hdrs: HTTPMessage, fp: HTTPResponse | None):
        super().__init__(msg, url)
        self.url = url
        self.code = code
        self.msg = msg
        self.hdrs = hdrs
        self.fp = fp

    @property
    def status(self) -> int:
        return self.code

    @property
    def headers(self) -> HTTPMessage:
        return self.hdrs

    def read(self, amt: int | None = None) -> bytes:
        fp = self.fp
        if fp is None:
            return b""
        return fp.read(amt)

    def getcode(self) -> int:
        return self.code

    def geturl(self) -> str:
        return self.url

    def info(self) -> HTTPMessage:
        return self.hdrs

    def close(self) -> None:
        fp = self.fp
        if fp is not None:
            fp.close()

    def __str__(self) -> str:
        return "HTTP Error " + str(self.code) + ": " + self.msg

    def __repr__(self) -> str:
        return "<HTTPError " + str(self.code) + ": " + repr(self.msg) + ">"


class ContentTooShortError(URLError):
    pass
