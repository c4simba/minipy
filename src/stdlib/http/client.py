"""HTTP protocol client (CPython's http.client): HTTPConnection, HTTPResponse,
HTTPMessage and their exceptions; HTTP/1.1 over a socket (KolibriOS's too).
There is no TLS: HTTPSConnection raises when connecting."""
import socket
from urllib.parse import urlsplit
from typing import TypeVar

_Body = TypeVar("_Body")                    # (request()'s body: bytes, a str, None - a copy of it for each)

HTTP_PORT = 80
HTTPS_PORT = 443
_MAXLINE = 65536
_MAXHEADERS = 100
_CS_IDLE = "Idle"
_CS_REQ_STARTED = "Request-started"
_CS_REQ_SENT = "Request-sent"
_METHODS_EXPECTING_BODY = ("PATCH", "POST", "PUT")

CONTINUE = 100
NO_CONTENT = 204
NOT_MODIFIED = 304

responses: dict[int, str] = {100: 'Continue', 101: 'Switching Protocols', 102: 'Processing', 103: 'Early Hints',
             200: 'OK', 201: 'Created', 202: 'Accepted', 203: 'Non-Authoritative Information', 204: 'No Content',
             205: 'Reset Content', 206: 'Partial Content', 207: 'Multi-Status', 208: 'Already Reported',
             226: 'IM Used', 300: 'Multiple Choices', 301: 'Moved Permanently', 302: 'Found', 303: 'See Other',
             304: 'Not Modified', 305: 'Use Proxy', 307: 'Temporary Redirect', 308: 'Permanent Redirect',
             400: 'Bad Request', 401: 'Unauthorized', 402: 'Payment Required', 403: 'Forbidden', 404: 'Not Found',
             405: 'Method Not Allowed', 406: 'Not Acceptable', 407: 'Proxy Authentication Required',
             408: 'Request Timeout', 409: 'Conflict', 410: 'Gone', 411: 'Length Required',
             412: 'Precondition Failed', 413: 'Content Too Large', 414: 'URI Too Long',
             415: 'Unsupported Media Type', 416: 'Range Not Satisfiable', 417: 'Expectation Failed',
             418: "I'm a Teapot", 421: 'Misdirected Request', 422: 'Unprocessable Content', 423: 'Locked',
             424: 'Failed Dependency', 425: 'Too Early', 426: 'Upgrade Required', 428: 'Precondition Required',
             429: 'Too Many Requests', 431: 'Request Header Fields Too Large',
             451: 'Unavailable For Legal Reasons', 500: 'Internal Server Error', 501: 'Not Implemented',
             502: 'Bad Gateway', 503: 'Service Unavailable', 504: 'Gateway Timeout',
             505: 'HTTP Version Not Supported', 506: 'Variant Also Negotiates', 507: 'Insufficient Storage',
             508: 'Loop Detected', 510: 'Not Extended', 511: 'Network Authentication Required'}



class HTTPException(Exception):
    pass


class NotConnected(HTTPException):
    pass


class InvalidURL(HTTPException):
    pass


class UnknownProtocol(HTTPException):
    pass


class UnknownTransferEncoding(HTTPException):
    pass


class UnimplementedFileMode(HTTPException):
    pass


class IncompleteRead(HTTPException):
    def __init__(self, partial: bytes, expected: int | None = None):
        super().__init__(repr(partial))
        self.partial = partial
        self.expected = expected

    def __repr__(self) -> str:
        e = ", " + str(self.expected) + " more expected" if self.expected is not None else ""
        return "IncompleteRead(" + str(len(self.partial)) + " bytes read" + e + ")"

    def __str__(self) -> str:
        return self.__repr__()


class ImproperConnectionState(HTTPException):
    pass


class CannotSendRequest(ImproperConnectionState):
    pass


class CannotSendHeader(ImproperConnectionState):
    pass


class ResponseNotReady(ImproperConnectionState):
    pass


class BadStatusLine(HTTPException):
    pass


class LineTooLong(HTTPException):
    pass


class RemoteDisconnected(ConnectionResetError):
    pass


error = HTTPException


class HTTPMessage:
    """The headers of a response: names as given, looked up without regard to case."""

    def __init__(self) -> None:
        self._headers: list[tuple[str, str]] = []

    def add(self, name: str, value: str) -> None:
        self._headers.append((name, value))

    def get(self, name: str, failobj: str | None = None) -> str | None:
        n = name.lower()
        for k, v in self._headers:
            if k.lower() == n:
                return v
        return failobj

    def get_all(self, name: str, failobj: list[str] | None = None) -> list[str] | None:
        n = name.lower()
        out = [v for k, v in self._headers if k.lower() == n]
        if not out:
            return failobj
        return out

    def __getitem__(self, name: str) -> str | None:
        return self.get(name)

    def __contains__(self, name: str) -> bool:
        return self.get(name) is not None

    def __len__(self) -> int:
        return len(self._headers)

    def __iter__(self):
        for k, v in self._headers:
            yield k

    def keys(self) -> list[str]:
        return [k for k, v in self._headers]

    def values(self) -> list[str]:
        return [v for k, v in self._headers]

    def items(self) -> list[tuple[str, str]]:
        return list(self._headers)

    def _param(self, name: str) -> str | None:
        """A parameter of Content-Type (charset=...)."""
        ct = self.get("content-type")
        if ct is None:
            return None
        for part in ct.split(";")[1:]:
            k, eq, v = part.strip().partition("=")
            if eq and k.strip().lower() == name:
                v = v.strip()
                if len(v) >= 2 and v[0] == '"' and v[-1] == '"':
                    v = v[1:-1]
                return v
        return None

    def get_content_type(self) -> str:
        ct = self.get("content-type")
        if ct is None:
            return "text/plain"
        t = ct.split(";")[0].strip().lower()
        if t.count("/") != 1:
            return "text/plain"
        return t

    def get_content_maintype(self) -> str:
        return self.get_content_type().split("/")[0]

    def get_content_subtype(self) -> str:
        return self.get_content_type().split("/")[1]

    def get_content_charset(self, failobj: str | None = None) -> str | None:
        c = self._param("charset")
        if c is None:
            return failobj
        return c.lower()

    def as_string(self) -> str:
        return "".join(k + ": " + v + "\n" for k, v in self._headers) + "\n"

    def __str__(self) -> str:
        return self.as_string()


class _Reader:
    """A socket's bytes, read through a buffer: lines and counts."""

    def __init__(self, sock: socket.socket, blocksize: int):
        self.sock = sock
        self.buf = b""
        self.blocksize = blocksize
        self.eof = False
        self.owns = False                   # (the connection closes with the response: the socket goes with it)

    def _fill(self) -> bool:
        if self.eof:
            return False
        data = self.sock.recv(self.blocksize)
        if not data:
            self.eof = True
            return False
        self.buf += data
        return True

    def readline(self, limit: int) -> bytes:
        while True:
            i = self.buf.find(b"\n")
            if i >= 0 or len(self.buf) >= limit:
                break
            if not self._fill():
                break
        i = self.buf.find(b"\n")
        end = len(self.buf) if i < 0 else i + 1
        if end > limit:
            end = limit
        line = self.buf[:end]
        self.buf = self.buf[end:]
        return line

    def read(self, n: int) -> bytes:
        """At most n bytes (fewer only at the end); n < 0: all until the end."""
        while (n < 0 or len(self.buf) < n) and self._fill():
            pass
        if n < 0:
            n = len(self.buf)
        out = self.buf[:n]
        self.buf = self.buf[n:]
        return out


def _read_headers(fp: _Reader) -> list[bytes]:
    headers: list[bytes] = []
    while True:
        line = fp.readline(_MAXLINE + 1)
        if len(line) > _MAXLINE:
            raise LineTooLong("got more than " + str(_MAXLINE) + " bytes when reading header line")
        headers.append(line)
        if len(headers) > _MAXHEADERS:
            raise HTTPException("got more than " + str(_MAXHEADERS) + " headers")
        if line in (b"\r\n", b"\n", b""):
            break
    return headers


def _parse_headers(lines: list[bytes]) -> HTTPMessage:
    msg = HTTPMessage()
    name = ""
    value = ""
    for raw in lines:
        line = raw.decode("iso-8859-1").rstrip("\r\n")
        if not line:
            break
        if line[0] in " \t" and name:                  # a continuation line
            value += "\n" + line
            continue
        if name:
            msg.add(name, value)
        name, sep, value = line.partition(":")
        if not sep:
            name = ""
            continue
        value = value.lstrip(" \t")
    if name:
        msg.add(name, value)
    return msg


class HTTPResponse:
    """A response: status, reason, version, headers and the body (read)."""

    def __init__(self, sock: socket.socket, method: str, blocksize: int = 8192):
        self.fp: _Reader | None = _Reader(sock, blocksize)
        self._method = method
        self.headers = HTTPMessage()
        self.msg = self.headers
        self.version = 0
        self.status = 0
        self.code = 0
        self.reason = ""
        self.chunked = False
        self.chunk_left = -1
        self.length: int | None = None
        self.will_close = True
        self.closed = False
        self.url: str | None = None
        self._started = False

    def _reader(self) -> _Reader:
        fp = self.fp
        if fp is None:
            raise ValueError("I/O operation on closed file.")
        return fp

    def _read_status(self) -> tuple[str, int, str]:
        raw = self._reader().readline(_MAXLINE + 1)
        line = raw.decode("iso-8859-1")
        if len(line) > _MAXLINE:
            raise LineTooLong("got more than " + str(_MAXLINE) + " bytes when reading status line")
        if not line:
            raise RemoteDisconnected("Remote end closed connection without response")
        parts = line.split(None, 2)
        version = parts[0] if parts else ""
        status = parts[1] if len(parts) > 1 else ""
        reason = parts[2] if len(parts) > 2 else ""
        if not version.startswith("HTTP/"):
            self._close_conn()
            raise BadStatusLine(repr(line) if not line else line)
        if not status.isdigit() or int(status) < 100 or int(status) > 999:
            raise BadStatusLine(line)
        return (version, int(status), reason)

    def begin(self) -> None:
        if self._started:
            return
        self._started = True
        while True:
            version, status, reason = self._read_status()
            if status != CONTINUE:
                break
            _read_headers(self._reader())
        self.code = status
        self.status = status
        self.reason = reason.strip()
        if version in ("HTTP/1.0", "HTTP/0.9"):
            self.version = 10
        elif version.startswith("HTTP/1."):
            self.version = 11
        else:
            raise UnknownProtocol(version)
        self.headers = _parse_headers(_read_headers(self._reader()))
        self.msg = self.headers
        tr_enc = self.headers.get("transfer-encoding")
        self.chunked = tr_enc is not None and tr_enc.lower() == "chunked"
        self.chunk_left = -1
        self.will_close = self._check_close()
        self.length = None
        length = self.headers.get("content-length")
        if length and not self.chunked:
            if length.strip().isdigit():
                self.length = int(length.strip())
        if status == NO_CONTENT or status == NOT_MODIFIED or 100 <= status < 200 or self._method == "HEAD":
            self.length = 0
        if not self.will_close and not self.chunked and self.length is None:
            self.will_close = True

    def _check_close(self) -> bool:
        conn = self.headers.get("connection")
        if self.version == 11:
            return conn is not None and "close" in conn.lower()
        if self.headers.get("keep-alive"):
            return False
        if conn is not None and "keep-alive" in conn.lower():
            return False
        pconn = self.headers.get("proxy-connection")
        if pconn is not None and "keep-alive" in pconn.lower():
            return False
        return True

    def _close_conn(self) -> None:
        fp = self.fp
        self.fp = None
        if fp is not None and fp.owns:
            fp.sock.close()

    def close(self) -> None:
        self.closed = True
        self._close_conn()

    def isclosed(self) -> bool:
        return self.fp is None

    def readable(self) -> bool:
        return True

    def _safe_read(self, amt: int) -> bytes:
        data = self._reader().read(amt)
        if len(data) < amt:
            raise IncompleteRead(data, amt - len(data))
        return data

    def _chunk_left(self) -> int:
        """Bytes left in the current chunk (a new one read when needed); -1 after the last one."""
        left = self.chunk_left
        if left <= 0:
            if left == 0:
                self._safe_read(2)
            line = self._reader().readline(_MAXLINE + 1)
            if len(line) > _MAXLINE:
                raise LineTooLong("got more than " + str(_MAXLINE) + " bytes when reading chunk size")
            i = line.find(b";")
            if i >= 0:
                line = line[:i]
            text = line.strip().decode("ascii", "replace")
            if not text or any(c not in "0123456789abcdefABCDEF" for c in text):
                self._close_conn()
                raise IncompleteRead(b"")
            left = int(text, 16)
            if left == 0:
                while True:
                    t = self._reader().readline(_MAXLINE + 1)
                    if t in (b"\r\n", b"\n", b""):
                        break
                self._close_conn()
                left = -1
            self.chunk_left = left
        return left

    def _read_chunked(self, amt: int) -> bytes:
        value: list[bytes] = []
        while True:
            left = self._chunk_left()
            if left < 0:
                break
            if amt >= 0 and amt <= left:
                value.append(self._safe_read(amt))
                self.chunk_left = left - amt
                break
            value.append(self._safe_read(left))
            if amt >= 0:
                amt -= left
            self.chunk_left = 0
        return b"".join(value)

    def read(self, amt: int | None = None) -> bytes:
        """The body (the next amt bytes of it)."""
        if self.fp is None:
            return b""
        if self._method == "HEAD":
            self._close_conn()
            return b""
        if self.chunked:
            return self._read_chunked(-1 if amt is None else amt)
        length = self.length
        if amt is not None and amt >= 0:
            if length is not None and amt > length:
                amt = length
            s = self._reader().read(amt)
            if not s and amt:
                self._close_conn()
            elif length is not None:
                self.length = length - len(s)
                if not self.length:
                    self._close_conn()
            return s
        if length is None:
            s = self._reader().read(-1)
        else:
            try:
                s = self._safe_read(length)
            except IncompleteRead:
                self._close_conn()
                raise
            self.length = 0
        self._close_conn()
        return s

    def readline(self, limit: int = -1) -> bytes:
        if self.fp is None or self._method == "HEAD":
            return b""
        line = self._reader().readline(_MAXLINE if limit < 0 else limit)
        if self.length is not None:
            self.length -= len(line)
            if not self.length:
                self._close_conn()
        return line

    def getheader(self, name: str, default: str | None = None) -> str | None:
        """The value of header name (several: joined with ', '), else default."""
        values = self.headers.get_all(name)
        if values is None:
            return default
        return ", ".join(values)

    def getheaders(self) -> list[tuple[str, str]]:
        return self.headers.items()

    def info(self) -> HTTPMessage:
        return self.headers

    def geturl(self) -> str | None:
        return self.url

    def getcode(self) -> int:
        return self.status

    def __enter__(self) -> "HTTPResponse":
        return self

    def __exit__(self, et, ev, tb) -> bool:
        self.close()
        return False


class HTTPConnection:
    """A connection to an HTTP server: request(), then getresponse()."""
    default_port = HTTP_PORT

    def __init__(self, host: str, port: int | None = None, timeout: float | None = -1.0,
                 source_address: tuple[str, int] | None = None, blocksize: int = 8192):
        self.timeout = timeout
        self.source_address = source_address
        self.blocksize = blocksize
        self.sock: socket.socket | None = None
        self._buffer: list[bytes] = []
        self._response: HTTPResponse | None = None
        self._state = _CS_IDLE
        self._method = ""
        self.debuglevel = 0
        self.host, self.port = self._get_hostport(host, port)

    def _get_hostport(self, host: str, port: int | None) -> tuple[str, int]:
        if port is None:
            i = host.rfind(":")
            j = host.rfind("]")
            if i > j:
                text = host[i + 1:]
                if text.isdigit():
                    p = int(text)
                elif text == "":
                    p = self.default_port
                else:
                    raise InvalidURL("nonnumeric port: '" + text + "'")
                host = host[:i]
            else:
                p = self.default_port
        else:
            p = port
        if host and host[0] == "[" and host[-1] == "]":
            host = host[1:-1]
        return (host, p)

    def set_debuglevel(self, level: int) -> None:
        self.debuglevel = level

    def connect(self) -> None:
        """Connect to the host and port given to the constructor."""
        self.sock = socket.create_connection((self.host, self.port), self.timeout)

    def close(self) -> None:
        self._state = _CS_IDLE
        sock = self.sock
        self.sock = None
        if sock is not None:
            sock.close()
        response = self._response
        self._response = None
        if response is not None:
            response.close()

    def send(self, data: bytes) -> None:
        """Send data to the server (connecting first if needed)."""
        if self.sock is None:
            self.connect()
        sock = self.sock
        if sock is not None:
            if self.debuglevel > 0:
                print("send:", repr(data))
            sock.sendall(data)

    def putrequest(self, method: str, url: str, skip_host: bool = False, skip_accept_encoding: bool = False) -> None:
        """Start a request: the request line (and Host, Accept-Encoding)."""
        if self._response is not None and self._response.isclosed():
            self._response = None
        if self._state == _CS_IDLE:
            self._state = _CS_REQ_STARTED
        else:
            raise CannotSendRequest(self._state)
        for c in method:
            if ord(c) <= 32 or ord(c) >= 127:
                raise ValueError("method can't contain control characters. " + repr(method) + " (found at least " + repr(c) + ")")
        self._method = method
        url = url or "/"
        for c in url:
            if ord(c) <= 32 or ord(c) == 127:
                raise InvalidURL("URL can't contain control characters. " + repr(url) + " (found at least " + repr(c) + ")")
        self._buffer.append((method + " " + url + " HTTP/1.1").encode("ascii"))
        if not skip_host:
            netloc = ""
            if url.startswith("http"):
                netloc = urlsplit(url).netloc
            if netloc:
                self.putheader("Host", netloc.partition("%")[0])
            else:
                host = "[" + self.host + "]" if ":" in self.host else self.host
                if self.port == self.default_port:
                    self.putheader("Host", host)
                else:
                    self.putheader("Host", host + ":" + str(self.port))
        if not skip_accept_encoding:
            self.putheader("Accept-Encoding", "identity")

    def putheader(self, header: str, *values: str) -> None:
        """A header line of the request."""
        if self._state != _CS_REQ_STARTED:
            raise CannotSendHeader()
        if not header or any(c in header for c in ":\r\n \t"):
            raise ValueError("Invalid header name " + repr(header.encode("latin-1")))
        enc: list[bytes] = []
        for v in values:
            b = v.encode("latin-1")
            if b.startswith((b" ", b"\t")) or b.endswith((b" ", b"\t")) or b"\r" in b or b"\n" in b:
                raise ValueError("Invalid header value " + repr(b))
            enc.append(b)
        self._buffer.append(header.encode("ascii") + b": " + b"\r\n\t".join(enc))

    def endheaders(self, message_body: bytes | None = None, *, encode_chunked: bool = False) -> None:
        """Send the request (and message_body)."""
        if self._state == _CS_REQ_STARTED:
            self._state = _CS_REQ_SENT
        else:
            raise CannotSendHeader()
        self._buffer.append(b"")
        self._buffer.append(b"")
        msg = b"\r\n".join(self._buffer)
        self._buffer = []
        self.send(msg)
        if message_body is not None and message_body:
            if encode_chunked:
                self.send(("%X" % len(message_body)).encode("ascii") + b"\r\n" + message_body + b"\r\n")
            else:
                self.send(message_body)
        if message_body is not None and encode_chunked:
            self.send(b"0\r\n\r\n")

    def request(self, method: str, url: str, body: _Body = None, headers: dict[str, str] | None = None, *,
                encode_chunked: bool = False) -> None:
        """Send a whole request: method, url, body (bytes or a str sent as Latin-1) and headers."""
        hdrs: dict[str, str] = {} if headers is None else headers
        names = [k.lower() for k in hdrs]
        self.putrequest(method, url, "host" in names, "accept-encoding" in names)
        data = _body_bytes(body)
        if "content-length" not in names:
            if "transfer-encoding" not in names:
                encode_chunked = False
                if data is None:
                    if method.upper() in _METHODS_EXPECTING_BODY:
                        self.putheader("Content-Length", "0")
                else:
                    self.putheader("Content-Length", str(len(data)))
        else:
            encode_chunked = False
        for k in hdrs:
            self.putheader(k, hdrs[k])
        self.endheaders(data, encode_chunked=encode_chunked)

    def getresponse(self) -> HTTPResponse:
        """The response to the request sent."""
        if self._response is not None and self._response.isclosed():
            self._response = None
        if self._state != _CS_REQ_SENT or self._response is not None:
            raise ResponseNotReady(self._state)
        sock = self.sock
        if sock is None:
            raise NotConnected()
        response = HTTPResponse(sock, self._method, self.blocksize)
        try:
            response.begin()
        except ConnectionError:
            self.close()
            raise
        self._state = _CS_IDLE
        if response.will_close:
            self.sock = None
            fp = response.fp
            if fp is not None:
                fp.owns = True
            else:
                sock.close()
        else:
            self._response = response
        return response


def _body_bytes(body) -> bytes | None:
    """A request body as bytes: a str is sent as Latin-1."""
    if isinstance(body, str):
        try:
            return body.encode("latin-1")
        except UnicodeEncodeError as e:
            raise UnicodeEncodeError(e.encoding, e.object, e.start, e.end,
                                     "Body (" + repr(body[e.start:e.end])[:22] + ") is not valid Latin-1. Use body.encode('utf-8') if you want to send it encoded in UTF-8.")
    elif isinstance(body, bytes):
        return body
    else:
        return None


class HTTPSConnection(HTTPConnection):
    """HTTPS is not available in minipy (no TLS): connecting raises."""
    default_port = HTTPS_PORT

    def connect(self) -> None:
        raise NotConnected("HTTPS is not supported: minipy has no TLS")
