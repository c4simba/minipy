"""HTTP server classes (CPython's http.server): HTTPServer, ThreadingHTTPServer, BaseHTTPRequestHandler,
SimpleHTTPRequestHandler; no TLS (HTTPSServer), no CGI.

Compiled programs: a request's command calls the handler's do_GET, do_HEAD, do_POST, do_PUT, do_DELETE,
do_PATCH, do_OPTIONS, do_TRACE or do_CONNECT (others: 501), as the program's subclass defines them;
SimpleHTTPRequestHandler reads a file whole before sending it and serves os.getcwd() (no directory=)."""
import sys
import os
import html
import mimetypes
import posixpath
import socket
import socketserver
import time
import urllib.parse
import http.client
from io import BytesIO
from http import HTTPStatus

__version__ = "0.6"

__all__ = ["HTTPServer", "ThreadingHTTPServer", "BaseHTTPRequestHandler", "SimpleHTTPRequestHandler"]

# Default error message template
DEFAULT_ERROR_MESSAGE = """\
<!DOCTYPE HTML>
<html lang="en">
    <head>
        <meta charset="utf-8">
        <style type="text/css">
            :root {
                color-scheme: light dark;
            }
        </style>
        <title>Error response</title>
    </head>
    <body>
        <h1>Error response</h1>
        <p>Error code: %(code)d</p>
        <p>Message: %(message)s.</p>
        <p>Error code explanation: %(code)s - %(explain)s.</p>
    </body>
</html>
"""

DEFAULT_ERROR_CONTENT_TYPE = "text/html;charset=utf-8"

_WEEKDAYS = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun']
_MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec']


def _formatdate(timeval: float) -> str:
    """An RFC 9110 date (email.utils.formatdate(timeval, usegmt=True))."""
    t = time.gmtime(timeval)
    return "%s, %02d %s %04d %02d:%02d:%02d GMT" % (_WEEKDAYS[t.tm_wday], t.tm_mday, _MONTHS[t.tm_mon - 1],
                                                    t.tm_year, t.tm_hour, t.tm_min, t.tm_sec)


def _days_from_civil(y: int, m: int, d: int) -> int:
    y -= 1 if m <= 2 else 0
    era = (y if y >= 0 else y - 399) // 400
    yoe = y - era * 400
    doy = (153 * (m + (-3 if m > 2 else 9)) + 2) // 5 + d - 1
    doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
    return era * 146097 + doe - 719468


def _parse_http_date(s: str) -> int | None:
    """The timestamp of an HTTP date ("Sun, 06 Nov 1994 08:49:37 GMT"); None: not one."""
    parts = s.replace(",", " ").split()
    if len(parts) < 5:
        return None
    if parts[0][:3] in _WEEKDAYS:
        parts = parts[1:]
    if len(parts) < 4 or parts[1][:3].capitalize() not in _MONTHS:
        return None
    hms = parts[3].split(":")
    if not parts[0].isdigit() or not parts[2].isdigit() or len(hms) != 3 or not all(x.isdigit() for x in hms):
        return None
    if len(parts) > 4 and parts[4] not in ("GMT", "UTC", "+0000", "-0000"):
        return None
    year = int(parts[2])
    if year < 100:
        year += 2000 if year < 70 else 1900
    days = _days_from_civil(year, _MONTHS.index(parts[1][:3].capitalize()) + 1, int(parts[0]))
    return days * 86400 + int(hms[0]) * 3600 + int(hms[1]) * 60 + int(hms[2])


def _control_escaped(message: str) -> str:
    """Control characters (and backslashes) as escapes (\\xNN), as the log shows them."""
    out = []
    for ch in message:
        o = ord(ch)
        if o < 0x20 or 0x7f <= o < 0xa0:
            out.append("\\x%02x" % o)
        elif ch == "\\":
            out.append("\\\\")
        else:
            out.append(ch)
    return "".join(out)


def _format_error(fmt: str, code: int, message: str, explain: str) -> str:
    """fmt % {'code': code, 'message': message, 'explain': explain}"""
    out = fmt.replace("%%", "\0")
    out = out.replace("%(code)d", str(code)).replace("%(code)s", str(code))
    out = out.replace("%(message)s", message).replace("%(explain)s", explain)
    return out.replace("\0", "%")


class HTTPServer(socketserver.TCPServer):

    allow_reuse_address = True    # Seems to make sense in testing environment
    allow_reuse_port = False

    def server_bind(self) -> None:
        """Override server_bind to store the server name."""
        socketserver.TCPServer.server_bind(self)
        host, port = self.server_address[:2]
        self.server_name = socket.getfqdn(host)
        self.server_port = port


class ThreadingHTTPServer(socketserver.ThreadingMixIn, HTTPServer):
    daemon_threads = True


class BaseHTTPRequestHandler(socketserver.StreamRequestHandler):
    """HTTP request handler base class: a request SPAM calls the method do_SPAM() of the handler (none: 501).

    The request: self.command, self.path, self.request_version, self.headers (an http.client.HTTPMessage),
    self.rfile (the body follows); the response: send_response(), send_header(), end_headers(), then
    self.wfile.write(...)."""

    sys_version = "Python/" + sys.version.split()[0]

    server_version = "BaseHTTP/" + __version__

    error_message_format = DEFAULT_ERROR_MESSAGE
    error_content_type = DEFAULT_ERROR_CONTENT_TYPE

    default_request_version = "HTTP/0.9"

    protocol_version = "HTTP/1.0"

    responses: dict[int, tuple[str, str]] = {v.value: (v.phrase, v.description) for v in HTTPStatus}

    weekdayname = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun']

    monthname = ['', 'Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec']

    def setup(self) -> None:
        super().setup()
        self.command: str | None = None
        self.path = ""
        self.request_version = self.default_request_version
        self.requestline = ""
        self.raw_requestline = b""
        self.close_connection = True
        self.headers = http.client.HTTPMessage()
        self._headers_buffer: list[bytes] = []

    def _read_headers(self) -> http.client.HTTPMessage:
        lines: list[bytes] = []
        while True:
            line = self.rfile.readline(65537)
            if len(line) > 65536:
                raise http.client.LineTooLong("header line")
            lines.append(line)
            if len(lines) > 100:
                raise http.client.HTTPException("got more than 100 headers")
            if line in (b'\r\n', b'\n', b''):
                break
        return http.client._parse_headers(lines)

    def parse_request(self) -> bool:
        """Parse a request (internal): self.raw_requestline into self.command, self.path,
        self.request_version and self.headers.  False: it failed (an error response was sent)."""
        is_http_0_9 = False
        self.command = None  # set in case of error on the first line
        self.request_version = version = self.default_request_version
        self.close_connection = True
        requestline = str(self.raw_requestline, 'iso-8859-1')
        requestline = requestline.rstrip('\r\n')
        self.requestline = requestline
        words = requestline.split()
        if len(words) == 0:
            return False

        if len(words) >= 3:  # Enough to determine protocol version
            version = words[-1]
            ok = version.startswith('HTTP/')
            base_version_number = version.split('/', 1)[1] if ok else ""
            version_number = base_version_number.split(".")
            if len(version_number) != 2 or any(not component.isdigit() for component in version_number) or \
                    any(len(component) > 10 for component in version_number):
                ok = False
            if not ok:
                self.send_error(HTTPStatus.BAD_REQUEST, "Bad request version (%r)" % version)
                return False
            major = int(version_number[0])
            minor = int(version_number[1])
            if (major, minor) >= (1, 1) and self.protocol_version >= "HTTP/1.1":
                self.close_connection = False
            if (major, minor) >= (2, 0):
                self.send_error(HTTPStatus.HTTP_VERSION_NOT_SUPPORTED,
                                "Invalid HTTP version (%s)" % base_version_number)
                return False
            self.request_version = version

        if not 2 <= len(words) <= 3:
            self.send_error(HTTPStatus.BAD_REQUEST, "Bad request syntax (%r)" % requestline)
            return False
        command = words[0]
        path = words[1]
        if len(words) == 2:
            self.close_connection = True
            if command != 'GET':
                self.send_error(HTTPStatus.BAD_REQUEST, "Bad HTTP/0.9 request type (%r)" % command)
                return False
            is_http_0_9 = True
        self.command = command
        self.path = path

        # gh-87389: The purpose of replacing '//' with '/' is to protect
        # against open redirect attacks possibly triggered if the path starts
        # with '//' because http clients treat //path as an absolute URI
        # without scheme (similar to http://path) rather than a path.
        if self.path.startswith('//'):
            self.path = '/' + self.path.lstrip('/')  # Reduce to a single /

        # For HTTP/0.9, headers are not expected at all.
        if is_http_0_9:
            self.headers = http.client.HTTPMessage()
            return True

        # Examine the headers and look for a Connection directive.
        try:
            self.headers = self._read_headers()
        except http.client.LineTooLong as err:
            self.send_error(HTTPStatus.REQUEST_HEADER_FIELDS_TOO_LARGE, "Line too long", str(err))
            return False
        except http.client.HTTPException as err:
            self.send_error(HTTPStatus.REQUEST_HEADER_FIELDS_TOO_LARGE, "Too many headers", str(err))
            return False

        conntype = self.headers.get('Connection', "") or ""
        if conntype.lower() == 'close':
            self.close_connection = True
        elif conntype.lower() == 'keep-alive' and self.protocol_version >= "HTTP/1.1":
            self.close_connection = False
        # Examine the headers and look for an Expect directive
        expect = self.headers.get('Expect', "") or ""
        if (expect.lower() == "100-continue" and self.protocol_version >= "HTTP/1.1" and
                self.request_version >= "HTTP/1.1"):
            if not self.handle_expect_100():
                return False
        return True

    def handle_expect_100(self) -> bool:
        """Decide what to do with an "Expect: 100-continue" header: by default a 100 Continue."""
        self.send_response_only(HTTPStatus.CONTINUE)
        self.end_headers()
        return True

    if not sys._compiled:
        def _call_command(self):
            mname = 'do_' + self.command
            if not hasattr(self, mname):
                self.send_error(HTTPStatus.NOT_IMPLEMENTED, "Unsupported method (%r)" % self.command)
                return
            method = getattr(self, mname)
            method()

    if sys._compiled:
        def _unsupported(self) -> None:
            self.send_error(HTTPStatus.NOT_IMPLEMENTED, "Unsupported method (%r)" % self.command)

        def do_GET(self) -> None:
            self._unsupported()

        def do_HEAD(self) -> None:
            self._unsupported()

        def do_POST(self) -> None:
            self._unsupported()

        def do_PUT(self) -> None:
            self._unsupported()

        def do_DELETE(self) -> None:
            self._unsupported()

        def do_PATCH(self) -> None:
            self._unsupported()

        def do_OPTIONS(self) -> None:
            self._unsupported()

        def do_TRACE(self) -> None:
            self._unsupported()

        def do_CONNECT(self) -> None:
            self._unsupported()

        def _call_command(self) -> None:
            c = self.command
            if c == "GET":
                self.do_GET()
            elif c == "HEAD":
                self.do_HEAD()
            elif c == "POST":
                self.do_POST()
            elif c == "PUT":
                self.do_PUT()
            elif c == "DELETE":
                self.do_DELETE()
            elif c == "PATCH":
                self.do_PATCH()
            elif c == "OPTIONS":
                self.do_OPTIONS()
            elif c == "TRACE":
                self.do_TRACE()
            elif c == "CONNECT":
                self.do_CONNECT()
            else:
                self._unsupported()

    def handle_one_request(self) -> None:
        """Handle a single HTTP request."""
        try:
            self.raw_requestline = self.rfile.readline(65537)
            if len(self.raw_requestline) > 65536:
                self.requestline = ''
                self.request_version = ''
                self.command = ''
                self.send_error(HTTPStatus.REQUEST_URI_TOO_LONG)
                return
            if not self.raw_requestline:
                self.close_connection = True
                return
            if not self.parse_request():
                # An error code has been sent, just exit
                return
            self._call_command()
            self.wfile.flush()  # actually send the response if not already done.
        except TimeoutError as e:
            # a read or a write timed out.  Discard this connection
            self.log_error("Request timed out: %r", e)
            self.close_connection = True
            return

    def handle(self) -> None:
        """Handle multiple requests if necessary."""
        self.close_connection = True

        self.handle_one_request()
        while not self.close_connection:
            self.handle_one_request()

    def send_error(self, code: int, message: str | None = None, explain: str | None = None) -> None:
        """Send and log an error reply (code, a short message, a longer explanation): a page explaining it."""
        shortmsg, longmsg = self.responses.get(code, ('???', '???'))
        if message is None:
            message = shortmsg
        if explain is None:
            explain = longmsg
        self.log_error("code %d, message %s", code, message)
        self.send_response(code, message)
        self.send_header('Connection', 'close')

        # Message body is omitted for cases described in:
        #  - RFC7230: 3.3. 1xx, 204(No Content), 304(Not Modified)
        #  - RFC7231: 6.3.6. 205(Reset Content)
        body = b""
        if code >= 200 and code not in (HTTPStatus.NO_CONTENT.value, HTTPStatus.RESET_CONTENT.value,
                                        HTTPStatus.NOT_MODIFIED.value):
            # HTML encode to prevent Cross Site Scripting attacks
            # (see bug #1100201)
            content = _format_error(self.error_message_format, code, html.escape(message, quote=False),
                                    html.escape(explain, quote=False))
            body = content.encode('UTF-8', 'replace')
            self.send_header("Content-Type", self.error_content_type)
            self.send_header('Content-Length', str(len(body)))
        self.end_headers()

        if self.command != 'HEAD' and body:
            self.wfile.write(body)

    def send_response(self, code: int, message: str | None = None) -> None:
        """Add the response header to the headers buffer and log the response code; also the Server and
        Date headers."""
        self.log_request(code)
        self.send_response_only(code, message)
        self.send_header('Server', self.version_string())
        self.send_header('Date', self.date_time_string())

    def send_response_only(self, code: int, message: str | None = None) -> None:
        """Send the response header only."""
        if self.request_version != 'HTTP/0.9':
            if message is None:
                if code in self.responses:
                    message = self.responses[code][0]
                else:
                    message = ''
            self._headers_buffer.append(("%s %d %s\r\n" % (self.protocol_version, code, message)).encode(
                'latin-1', 'strict'))

    def send_header(self, keyword: str, value: str) -> None:
        """Send a MIME header to the headers buffer."""
        if self.request_version != 'HTTP/0.9':
            self._headers_buffer.append(("%s: %s\r\n" % (keyword, value)).encode('latin-1', 'strict'))

        if keyword.lower() == 'connection':
            if value.lower() == 'close':
                self.close_connection = True
            elif value.lower() == 'keep-alive':
                self.close_connection = False

    def end_headers(self) -> None:
        """Send the blank line ending the MIME headers."""
        if self.request_version != 'HTTP/0.9':
            self._headers_buffer.append(b"\r\n")
            self.flush_headers()

    def flush_headers(self) -> None:
        if self._headers_buffer:
            self.wfile.write(b"".join(self._headers_buffer))
            self._headers_buffer = []

    def log_request(self, code: int = -1, size: int = -1) -> None:
        """Log an accepted request (called by send_response())."""
        c = '-'
        if isinstance(code, str):           # (log_request('-', '-') as CPython's takes them)
            c = code
        elif code >= 0:
            c = str(int(code))
        z = '-'
        if isinstance(size, str):
            z = size
        elif size >= 0:
            z = str(size)
        self.log_message('"%s" %s %s', self.requestline, c, z)

    def log_error(self, format: str, *args: sys._PercentArgs) -> None:
        """Log an error (a request that cannot be fulfilled): through log_message()."""
        if sys._compiled:
            self.log_message(format)
        else:
            self.log_message(format, *args)

    def log_message(self, format: str, *args: sys._PercentArgs) -> None:
        """Log a message (format % args) on sys.stderr, the client's address and the time before it."""
        if sys._compiled:
            message = format
        else:
            message = format % args
        sys.stderr.write("%s - - [%s] %s\n" % (self.address_string(), self.log_date_time_string(),
                                               _control_escaped(message)))

    def version_string(self) -> str:
        """Return the server software version string."""
        return self.server_version + ' ' + self.sys_version

    def date_time_string(self, timestamp: float | None = None) -> str:
        """Return the current date and time formatted for a message header."""
        if timestamp is None:
            timestamp = time.time()
        return _formatdate(timestamp)

    def log_date_time_string(self) -> str:
        """Return the current time formatted for logging."""
        now = time.time()
        t = time.localtime(now)
        return "%02d/%3s/%04d %02d:%02d:%02d" % (t.tm_mday, self.monthname[t.tm_mon], t.tm_year, t.tm_hour,
                                                 t.tm_min, t.tm_sec)

    def address_string(self) -> str:
        """Return the client address."""
        return self.client_address[0]


class SimpleHTTPRequestHandler(BaseHTTPRequestHandler):
    """Simple HTTP request handler with GET and HEAD commands: the files of the current directory and
    its subdirectories (a directory: its index.html, or a listing)."""

    server_version = "SimpleHTTP/" + __version__
    index_pages = ("index.html", "index.htm")
    extensions_map: dict[str, str] = {
        '.gz': 'application/gzip',
        '.Z': 'application/octet-stream',
        '.bz2': 'application/x-bzip2',
        '.xz': 'application/x-xz',
    }

    if not sys._compiled:
        def __init__(self, *args, directory=None, **kwargs):
            if directory is None:
                directory = os.getcwd()
            self.directory = os.fspath(directory)
            super().__init__(*args, **kwargs)

    if sys._compiled:
        def __init__(self, request: socket.socket, client_address: tuple[str, int],
                     server: socketserver.BaseServer) -> None:
            self.directory = os.getcwd()
            super().__init__(request, client_address, server)

    def do_GET(self) -> None:
        """Serve a GET request."""
        f = self.send_head()
        if f is not None:
            try:
                self.copyfile(f, self.wfile)
            finally:
                f.close()

    def do_HEAD(self) -> None:
        """Serve a HEAD request."""
        f = self.send_head()
        if f is not None:
            f.close()

    def _open(self, path: str) -> BytesIO:
        with open(path, 'rb') as f:
            return BytesIO(f.read())

    def send_head(self) -> BytesIO | None:
        """Common code for GET and HEAD commands: the response code and headers sent, the body to copy (None:
        nothing more to send)."""
        path = self.translate_path(self.path)
        if os.path.isdir(path):
            parts = urllib.parse.urlsplit(self.path)
            if not parts.path.endswith(('/', '%2f', '%2F')):
                # redirect browser - doing basically what apache does
                self.send_response(HTTPStatus.MOVED_PERMANENTLY)
                new_url = urllib.parse.urlunsplit((parts[0], parts[1], parts[2] + '/', parts[3], parts[4]))
                self.send_header("Location", new_url)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return None
            found = False
            for index in self.index_pages:
                index = os.path.join(path, index)
                if os.path.isfile(index):
                    path = index
                    found = True
                    break
            if not found:
                return self.list_directory(path)
        ctype = self.guess_type(path)
        # check for trailing "/" which should return 404. See Issue17324
        # The test for this was added in test_httpserver.py
        # However, some OS platforms accept a trailingSlash as a filename
        # See discussion on python-dev and Issue34711 regarding
        # parsing and rejection of filenames with a trailing slash
        if path.endswith("/"):
            self.send_error(HTTPStatus.NOT_FOUND, "File not found")
            return None
        try:
            f = self._open(path)
            fs = os.stat(path)
        except OSError:
            self.send_error(HTTPStatus.NOT_FOUND, "File not found")
            return None
        # Use browser cache if possible
        if "If-Modified-Since" in self.headers and "If-None-Match" not in self.headers:
            ims = _parse_http_date(self.headers["If-Modified-Since"] or "")
            if ims is not None and int(fs.st_mtime) <= ims:
                self.send_response(HTTPStatus.NOT_MODIFIED)
                self.end_headers()
                f.close()
                return None
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-type", ctype)
        self.send_header("Content-Length", str(fs.st_size))
        self.send_header("Last-Modified", self.date_time_string(fs.st_mtime))
        self.end_headers()
        return f

    def list_directory(self, path: str) -> BytesIO | None:
        """Helper to produce a directory listing (absent index.html): the headers sent, the page to copy."""
        try:
            names = os.listdir(path)
        except OSError:
            self.send_error(HTTPStatus.NOT_FOUND, "No permission to list directory")
            return None
        names.sort(key=lambda a: a.lower())
        r: list[str] = []
        displaypath = self.path
        displaypath = displaypath.split('#', 1)[0]
        displaypath = displaypath.split('?', 1)[0]
        displaypath = urllib.parse.unquote(displaypath)
        displaypath = html.escape(displaypath, quote=False)
        enc = 'utf-8'
        title = f'Directory listing for {displaypath}'
        r.append('<!DOCTYPE HTML>')
        r.append('<html lang="en">')
        r.append('<head>')
        r.append(f'<meta charset="{enc}">')
        r.append('<style type="text/css">\n:root {\ncolor-scheme: light dark;\n}\n</style>')
        r.append(f'<title>{title}</title>\n</head>')
        r.append(f'<body>\n<h1>{title}</h1>')
        r.append('<hr>\n<ul>')
        for name in names:
            fullname = os.path.join(path, name)
            displayname = linkname = name
            # Append / for directories or @ for symbolic links
            if os.path.isdir(fullname):
                displayname = name + "/"
                linkname = name + "/"
            if os.path.islink(fullname):
                displayname = name + "@"
                # Note: a link to a directory displays with @ and links with /
            r.append('<li><a href="%s">%s</a></li>' % (urllib.parse.quote(linkname),
                                                       html.escape(displayname, quote=False)))
        r.append('</ul>\n<hr>\n</body>\n</html>\n')
        encoded = '\n'.join(r).encode(enc, 'surrogateescape')
        f = BytesIO()
        f.write(encoded)
        f.seek(0)
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-type", "text/html; charset=%s" % enc)
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        return f

    def translate_path(self, path: str) -> str:
        """Translate a /-separated PATH to the local filename syntax (under self.directory)."""
        # abandon query parameters
        path = path.split('#', 1)[0]
        path = path.split('?', 1)[0]
        # Don't forget explicit trailing slash when normalizing. Issue17324
        path = urllib.parse.unquote(path)
        trailing_slash = path.endswith('/')
        path = posixpath.normpath(path)
        words = [w for w in path.split('/') if w]
        path = self.directory
        for word in words:
            if os.path.dirname(word) or word in (os.curdir, os.pardir):
                # Ignore components that are not a simple file/directory name
                continue
            path = os.path.join(path, word)
        if trailing_slash:
            path += '/'
        return path

    def copyfile(self, source: BytesIO, outputfile: socketserver._SocketWriter) -> None:
        """Copy all data between two file objects."""
        while True:
            buf = source.read(64 * 1024)
            if not buf:
                break
            outputfile.write(buf)

    def guess_type(self, path: str) -> str:
        """Guess the type of a file (by its extension: self.extensions_map, then mimetypes)."""
        base, ext = posixpath.splitext(path)
        if ext in self.extensions_map:
            return self.extensions_map[ext]
        ext = ext.lower()
        if ext in self.extensions_map:
            return self.extensions_map[ext]
        guess = mimetypes.guess_file_type(path)[0]
        if guess:
            return guess
        return 'application/octet-stream'


def test(HandlerClass: "type[BaseHTTPRequestHandler]" = BaseHTTPRequestHandler, port: int = 8000,
         bind: str | None = None, protocol: str = "HTTP/1.0") -> None:
    """Test the HTTP request handler class: serve on the port (until Ctrl-C)."""
    if not sys._compiled:
        HandlerClass.protocol_version = protocol
    httpd = ThreadingHTTPServer((bind or "", port), HandlerClass)
    try:
        host, port = httpd.socket.getsockname()
        url_host = host
        print(f"Serving HTTP on {host} port {port} (http://{url_host}:{port}/) ...")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nKeyboard interrupt received, exiting.")
            sys.exit(0)
    finally:
        httpd.server_close()
