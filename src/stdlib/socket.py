"""socket: network connections over IPv4 (TCP and UDP), as CPython's socket.

Linux: the socket system calls (minipy emulates them on other hosts). KolibriOS:
function 75. Its stack has no getsockname/getpeername (they give the address
the socket was bound/connected to), UDP sends go to the address connect() gave
(sendto connects first), and shutdown() is only remembered.

Timeouts (settimeout) wait with poll(); on KolibriOS receiving and accepting
are retried every 10 ms until the time is up, other calls block.

Host names resolve through /etc/hosts and minipy's own DNS query to the name
server of /etc/resolv.conf (KolibriOS: the one its network settings hold)."""
import sys
import _os

AF_UNSPEC = 0
AF_UNIX = 1
AF_INET = 2
AF_INET6 = 10
SOCK_STREAM = 1
SOCK_DGRAM = 2
SOCK_RAW = 3
SOCK_NONBLOCK = 0x800
SOCK_CLOEXEC = 0x80000
SOL_SOCKET = 1
SO_REUSEADDR = 2
SO_TYPE = 3
SO_ERROR = 4
SO_BROADCAST = 6
SO_SNDBUF = 7
SO_RCVBUF = 8
SO_KEEPALIVE = 9
SO_LINGER = 13
SO_REUSEPORT = 15
IPPROTO_IP = 0
IPPROTO_ICMP = 1
IPPROTO_TCP = 6
IPPROTO_UDP = 17
IP_TTL = 2
TCP_NODELAY = 1
SHUT_RD = 0
SHUT_WR = 1
SHUT_RDWR = 2
MSG_OOB = 1
MSG_PEEK = 2
MSG_DONTWAIT = 0x40
MSG_WAITALL = 0x100
INADDR_ANY = 0
INADDR_BROADCAST = 0xFFFFFFFF
INADDR_LOOPBACK = 0x7F000001
SOMAXCONN = 128
has_ipv6 = False

error = OSError
timeout = TimeoutError


class herror(OSError):
    pass


class gaierror(OSError):
    pass


if sys.platform == "darwin":
    EAI_NONAME = 8
    _EAI_NONAME_TEXT = "nodename nor servname provided, or not known"
else:
    EAI_NONAME = -2
    _EAI_NONAME_TEXT = "Name or service not known"

_default_timeout: float | None = None

# KolibriOS's socket error codes -> errno
if sys.platform == "darwin":                 # (errno numbers are the system's own)
    _EINPROGRESS = 36
    _ETIMEDOUT = 60
else:
    _EINPROGRESS = 115
    _ETIMEDOUT = 110

_KSOCKERR = {1: 105, 2: 115, 4: 95, 6: 11, 9: 107, 10: 114, 11: 22, 12: 90, 18: 12, 20: 98, 61: 111, 52: 104, 56: 106, 60: 110, 53: 103}


def _kerror(code: int) -> OSError:
    e = _KSOCKERR.get(code & 0xFFFFFFFF)
    if e is None:
        e = 5
    return OSError(e, _os.strerror(e))


def _timeout_error() -> TimeoutError:
    return TimeoutError("timed out")


# ---------------------------------------------------------------- addresses

def htons(x: int) -> int:
    return ((x & 255) << 8) | ((x >> 8) & 255)


def ntohs(x: int) -> int:
    return htons(x)


def htonl(x: int) -> int:
    return ((x & 255) << 24) | (((x >> 8) & 255) << 16) | (((x >> 16) & 255) << 8) | ((x >> 24) & 255)


def ntohl(x: int) -> int:
    return htonl(x)


def _is_ipv4(s: str) -> bool:
    parts = s.split(".")
    if len(parts) != 4:
        return False
    for p in parts:
        if not p or not p.isdigit() or not p.isascii() or len(p) > 3 or int(p) > 255:
            return False
    return True


def inet_aton(ip_string: str) -> bytes:
    """The 4 bytes of a dotted IPv4 address."""
    if not _is_ipv4(ip_string):
        raise OSError("illegal IP address string passed to inet_aton")
    return bytes([int(p) for p in ip_string.split(".")])


def inet_ntoa(packed_ip: bytes) -> str:
    if len(packed_ip) != 4:
        raise OSError("packed IP wrong length for inet_ntoa")
    return str(packed_ip[0]) + "." + str(packed_ip[1]) + "." + str(packed_ip[2]) + "." + str(packed_ip[3])


def inet_pton(address_family: int, ip_string: str) -> bytes:
    if address_family != AF_INET:
        raise OSError(97, _os.strerror(97))
    if not _is_ipv4(ip_string):
        raise OSError("illegal IP address string passed to inet_pton")
    return inet_aton(ip_string)


def inet_ntop(address_family: int, packed_ip: bytes) -> str:
    if address_family != AF_INET:
        raise OSError(97, _os.strerror(97))
    if len(packed_ip) != 4:
        raise ValueError("invalid length of packed IP address string")
    return inet_ntoa(packed_ip)


def _sockaddr(host: str, port: int) -> buffer:
    """struct sockaddr_in: family (2 bytes), port (network order), address, 8 zeroes."""
    if port < 0 or port > 65535:
        raise OverflowError("bind(): port must be 0-65535.")
    b = sys.buffer(16)
    sys.poke(b, 0, AF_INET, 2)
    sys.poke(b, 2, htons(port), 2)
    sys.poke_bytes(b, 4, inet_aton(_resolve(host)))
    return b


def _addr_of(b: buffer) -> tuple[str, int]:
    return (inet_ntoa(sys.peek_bytes(b, 4, 4)), sys.peek(b, 2, 1) * 256 + sys.peek(b, 3, 1))


def _resolve(host: str) -> str:
    if host == "":
        return "0.0.0.0"
    if host == "<broadcast>":
        return "255.255.255.255"
    if _is_ipv4(host):
        return host
    return gethostbyname(host)


# ---------------------------------------------------------------- sockets

class socket:
    """A network endpoint: socket(AF_INET, SOCK_STREAM) for TCP, SOCK_DGRAM for UDP."""

    def __init__(self, family: int = AF_INET, type: int = SOCK_STREAM, proto: int = 0, fileno: int = -1):
        if family != AF_INET:
            raise OSError(97, _os.strerror(97))
        self.family = family
        self.type = type & 0xF
        self.proto = proto
        self._timeout: float | None = _default_timeout
        self._closed = False
        self._io_refs = 0                   # (files of makefile() still using it: close() waits for them)
        self._local = ("0.0.0.0", 0)
        self._peer = ("0.0.0.0", 0)
        self._fd = fileno
        if fileno >= 0:
            return
        if sys.platform == "kolibrios":
            r = sys.syscall(75, 0, AF_INET, self.type, proto)          # fn 75.0
            if r[0] == -1:
                raise _kerror(r[1])
            self._fd = r[0]
            return
        self._fd = _os._check(sys.syscall(359, family, self.type | SOCK_CLOEXEC, proto)[0], None)
        if self._timeout is not None:
            self._set_nonblocking(True)

    # ---- descriptors and modes
    def fileno(self) -> int:
        return self._fd

    def _check_open(self) -> None:
        if self._fd < 0:
            raise OSError(9, _os.strerror(9))

    def _set_nonblocking(self, on: bool) -> None:
        if sys.platform == "kolibrios":
            return
        fl = _os._check(sys.syscall(221, self._fd, 3, 0)[0], None)      # fcntl64(F_GETFL)
        if on:
            fl |= 0x800
        else:
            fl &= ~0x800
        _os._check(sys.syscall(221, self._fd, 4, fl)[0], None)

    def settimeout(self, value: float | None) -> None:
        """None: blocking calls; 0: non-blocking ones (BlockingIOError); t: wait at most t seconds (TimeoutError)."""
        if value is not None and value < 0:
            raise ValueError("Timeout value out of range")
        self._timeout = value
        self._set_nonblocking(value is not None)

    def gettimeout(self) -> float | None:
        return self._timeout

    def setblocking(self, flag: bool) -> None:
        if flag:
            self.settimeout(None)
        else:
            self.settimeout(0.0)

    def getblocking(self) -> bool:
        return self._timeout is None or self._timeout != 0.0

    def _wait(self, writing: bool) -> None:
        """With a timeout: until the socket is ready, else TimeoutError (poll; KolibriOS: no waiting here)."""
        if self._timeout is None or self._timeout == 0.0 or sys.platform == "kolibrios":
            return
        pfd = sys.buffer(8)
        sys.poke(pfd, 0, self._fd, 4)
        if writing:
            sys.poke(pfd, 4, 4, 2)                   # POLLOUT
        else:
            sys.poke(pfd, 4, 1, 2)                   # POLLIN
        ms = int(self._timeout * 1000)
        while True:
            r = sys.syscall(168, pfd, 1, ms)[0]
            if r == -4:
                continue
            _os._check(r, None)
            if r == 0:
                raise _timeout_error()
            return

    def _kwait(self, started: int) -> None:
        """KolibriOS: 10 ms more, unless the timeout is over (since started, in 1/100 s)."""
        if self._timeout is None:
            sys.syscall(5, 1)
            return
        if self._timeout == 0.0:
            raise OSError(11, _os.strerror(11))
        now = sys.syscall(26, 9)[0]
        if (now - started) / 100.0 >= self._timeout:
            raise _timeout_error()
        sys.syscall(5, 1)                             # fn 5: wait 1/100 s

    # ---- connections
    def bind(self, address: tuple[str, int]) -> None:
        self._check_open()
        addr = _sockaddr(address[0], address[1])
        if sys.platform == "kolibrios":
            if address[1] == 0:                                         # (no getsockname there: a free port of ours)
                r = [-1, 0]
                start = 49152 + (sys.syscall(26, 9)[0] * 7919 + self._fd * 104729) % 16000
                for i in range(64):
                    addr = _sockaddr(address[0], 49152 + (start - 49152 + i * 97) % 16000)
                    r = sys.syscall(75, 2, self._fd, addr, 16)
                    if r[0] != -1:
                        break
            else:
                r = sys.syscall(75, 2, self._fd, addr, 16)              # fn 75.2
            if r[0] == -1:
                raise _kerror(r[1])
            self._local = _addr_of(addr)
            return
        _os._check(sys.syscall(361, self._fd, addr, 16)[0], None)

    def listen(self, backlog: int = SOMAXCONN) -> None:
        self._check_open()
        if sys.platform == "kolibrios":
            r = sys.syscall(75, 3, self._fd, backlog)                   # fn 75.3
            if r[0] == -1:
                raise _kerror(r[1])
            return
        _os._check(sys.syscall(363, self._fd, backlog)[0], None)

    def connect(self, address: tuple[str, int]) -> None:
        err = self.connect_ex(address)
        if err == -_ETIMEDOUT:
            raise _timeout_error()
        if err:
            raise OSError(err, _os.strerror(err))

    def connect_ex(self, address: tuple[str, int]) -> int:
        """connect(), but the error number as the result (0: connected)."""
        self._check_open()
        addr = _sockaddr(address[0], address[1])
        if sys.platform == "kolibrios":
            r = sys.syscall(75, 4, self._fd, addr, 16)                  # fn 75.4
            if r[0] == -1:
                code = _KSOCKERR.get(r[1] & 0xFFFFFFFF)
                if code is None:
                    return 5
                return code
            self._peer = _addr_of(addr)
            return 0
        r = sys.syscall(362, self._fd, addr, 16)[0]
        if r == -_EINPROGRESS and self._timeout is not None and self._timeout != 0.0:   # EINPROGRESS: wait, then the outcome
            try:
                self._wait(True)
            except TimeoutError:
                return -_ETIMEDOUT
            r = -self.getsockopt(SOL_SOCKET, SO_ERROR)
        if r < 0:
            return -r
        return 0

    def accept(self) -> tuple["socket", tuple[str, int]]:
        """A new connection: (its socket, the peer's address)."""
        self._check_open()
        addr = sys.buffer(16)
        if sys.platform == "kolibrios":
            started = sys.syscall(26, 9)[0]
            while True:
                r = sys.syscall(75, 5, self._fd, addr, 16)              # fn 75.5
                if r[0] != -1:
                    break
                if (r[1] & 0xFFFFFFFF) != 6 or self._timeout is None:
                    raise _kerror(r[1])
                self._kwait(started)
            s = socket(AF_INET, self.type, self.proto, r[0])
            s._peer = _addr_of(addr)
            s._local = self._local
            return (s, s._peer)
        alen = sys.buffer(4)
        while True:
            self._wait(False)
            sys.poke(alen, 0, 16, 4)
            fd = sys.syscall(364, self._fd, addr, alen, SOCK_CLOEXEC)[0]
            if fd != -4:
                break
        _os._check(fd, None)
        s = socket(AF_INET, self.type, self.proto, fd)
        if s._timeout is not None:
            s._set_nonblocking(True)
        return (s, _addr_of(addr))

    def getsockname(self) -> tuple[str, int]:
        self._check_open()
        if sys.platform == "kolibrios":
            return self._local
        addr = sys.buffer(16)
        alen = sys.buffer(4)
        sys.poke(alen, 0, 16, 4)
        _os._check(sys.syscall(367, self._fd, addr, alen)[0], None)
        return _addr_of(addr)

    def getpeername(self) -> tuple[str, int]:
        self._check_open()
        if sys.platform == "kolibrios":
            return self._peer
        addr = sys.buffer(16)
        alen = sys.buffer(4)
        sys.poke(alen, 0, 16, 4)
        _os._check(sys.syscall(368, self._fd, addr, alen)[0], None)
        return _addr_of(addr)

    # ---- data
    def send(self, data: bytes, flags: int = 0) -> int:
        """Send some of data: the number of bytes sent."""
        self._check_open()
        if sys.platform == "kolibrios":
            r = sys.syscall(75, 6, self._fd, data, len(data), flags)    # fn 75.6
            if r[0] == -1:
                raise _kerror(r[1])
            return r[0]
        while True:
            self._wait(True)
            r = sys.syscall(369, self._fd, data, len(data), flags | 0x4000, 0, 0)[0]   # (MSG_NOSIGNAL)
            if r != -4:
                return _os._check(r, None)

    def sendall(self, data: bytes, flags: int = 0) -> None:
        done = 0
        while done < len(data):
            done += self.send(data[done:], flags)

    def sendto(self, data: bytes, address: tuple[str, int]) -> int:
        self._check_open()
        addr = _sockaddr(address[0], address[1])
        if sys.platform == "kolibrios":
            if self._peer != _addr_of(addr):
                self.connect(address)
            return self.send(data)
        while True:
            self._wait(True)
            r = sys.syscall(369, self._fd, data, len(data), 0x4000, addr, 16)[0]
            if r != -4:
                return _os._check(r, None)

    def recv(self, bufsize: int, flags: int = 0) -> bytes:
        """Up to bufsize bytes (b"" when the other side has closed the connection)."""
        self._check_open()
        if bufsize < 0:
            raise ValueError("negative buffersize in recv")
        buf = sys.buffer(bufsize)
        if sys.platform == "kolibrios":
            started = sys.syscall(26, 9)[0]
            kf = flags
            if self._timeout is not None:
                kf |= MSG_DONTWAIT
            while True:
                r = sys.syscall(75, 7, self._fd, buf, bufsize, kf)     # fn 75.7
                if r[0] != -1:
                    return sys.peek_bytes(buf, 0, r[0])
                if (r[1] & 0xFFFFFFFF) != 6 or self._timeout is None:
                    raise _kerror(r[1])
                self._kwait(started)
        while True:
            self._wait(False)
            r = sys.syscall(371, self._fd, buf, bufsize, flags, 0, 0)[0]
            if r != -4:
                return sys.peek_bytes(buf, 0, _os._check(r, None))

    def recvfrom(self, bufsize: int, flags: int = 0) -> tuple[bytes, tuple[str, int]]:
        self._check_open()
        if sys.platform == "kolibrios":
            data = self.recv(bufsize, flags)
            return (data, self._peer)
        buf = sys.buffer(bufsize)
        addr = sys.buffer(16)
        alen = sys.buffer(4)
        while True:
            self._wait(False)
            sys.poke(alen, 0, 16, 4)
            r = sys.syscall(371, self._fd, buf, bufsize, flags, addr, alen)[0]
            if r != -4:
                break
        n = _os._check(r, None)
        return (sys.peek_bytes(buf, 0, n), _addr_of(addr))

    def shutdown(self, how: int) -> None:
        self._check_open()
        if sys.platform == "kolibrios":
            return
        _os._check(sys.syscall(373, self._fd, how)[0], None)

    # ---- options
    def setsockopt(self, level: int, optname: int, value: int) -> None:
        self._check_open()
        if sys.platform == "kolibrios":
            b = sys.buffer(16)                         # fn 75.8: level, name, length, value
            sys.poke(b, 0, level, 4)
            sys.poke(b, 4, optname, 4)
            sys.poke(b, 8, 4, 4)
            sys.poke(b, 12, value, 4)
            sys.syscall(75, 8, self._fd, b)
            return
        b = sys.buffer(8)
        sys.poke(b, 0, value, 4)
        _os._check(sys.syscall(366, self._fd, level, optname, b, 4)[0], None)

    def getsockopt(self, level: int, optname: int) -> int:
        self._check_open()
        if sys.platform == "kolibrios":
            b = sys.buffer(16)
            sys.poke(b, 0, level, 4)
            sys.poke(b, 4, optname, 4)
            sys.poke(b, 8, 4, 4)
            if sys.syscall(75, 9, self._fd, b)[0] == -1:
                return 0
            return sys.peek(b, 12, 4)
        b = sys.buffer(8)
        blen = sys.buffer(4)
        sys.poke(blen, 0, 4, 4)
        _os._check(sys.syscall(365, self._fd, level, optname, b, blen)[0], None)
        v = sys.peek(b, 0, 4)
        return ((v & 0xFFFFFFFF) ^ 0x80000000) - 0x80000000

    # ---- files
    if not sys._compiled:
        def makefile(self, mode="r", buffering=None, *, encoding=None, errors=None, newline=None):
            """A file object reading from / writing to the socket ("rb", "wb", "rwb"; "r", "w": text)."""
            if "b" in mode:
                return self._makefile_b(mode, buffering)
            return self._makefile_t(mode, buffering, encoding, errors, newline)

    def _makefile_b(self, mode: str = "rb", buffering: int | None = None) -> "SocketIO":
        for c in mode:
            if c not in "rwb":
                raise ValueError("invalid mode %r (only r, w, b allowed)" % (mode,))
        return SocketIO(self, mode)

    def _makefile_t(self, mode: str = "r", buffering: int | None = None, encoding: str | None = None,
                    errors: str | None = None, newline: str | None = None) -> "_TextSocketIO":
        for c in mode:
            if c not in "rw":
                raise ValueError("invalid mode %r (only r, w, b allowed)" % (mode,))
        return _TextSocketIO(SocketIO(self, mode + "b"), encoding or "utf-8", errors or "strict", newline)

    def _decref_socketios(self) -> None:
        if self._io_refs > 0:
            self._io_refs -= 1
        if self._closed:
            self._real_close()

    # ---- the end
    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        if self._io_refs <= 0:
            self._real_close()

    def _real_close(self) -> None:
        if self._fd < 0:
            return
        if sys.platform == "kolibrios":
            sys.syscall(75, 1, self._fd)               # fn 75.1
        else:
            sys.syscall(6, self._fd)
        self._fd = -1

    def detach(self) -> int:
        fd = self._fd
        self._closed = True
        self._fd = -1
        return fd

    def __enter__(self) -> "socket":
        return self

    def __exit__(self, et, ev, tb) -> bool:
        self.close()
        return False

    def __repr__(self) -> str:
        head = "<socket.socket "
        if self._closed:
            head += "[closed] "
        s = head + "fd=" + str(self._fd) + ", family=" + str(self.family) + ", type=" + str(self.type) + ", proto=" + str(self.proto)
        if not self._closed:
            try:
                s += ", laddr=" + repr(self.getsockname())
            except OSError:
                pass
            try:
                peer = self.getpeername()
                if sys.platform != "kolibrios" or peer[1] != 0:
                    s += ", raddr=" + repr(peer)
            except OSError:
                pass
        return s + ">"


class SocketIO:
    """A socket as a binary file (makefile("rb") / ("wb") / ("rwb")): buffered reads, writes sent at once."""

    def __init__(self, sock: socket, mode: str) -> None:
        self._sock = sock
        self.mode = mode
        self._reading = "r" in mode
        self._writing = "w" in mode
        self._buf = b""
        self._eof = False
        self._closed = False
        sock._io_refs += 1

    @property
    def closed(self) -> bool:
        return self._closed

    @property
    def name(self) -> int:
        return self._sock.fileno() if not self._closed else -1

    def fileno(self) -> int:
        return self._sock.fileno()

    def readable(self) -> bool:
        return self._reading

    def writable(self) -> bool:
        return self._writing

    def seekable(self) -> bool:
        return False

    def _check(self, reading: bool) -> None:
        if self._closed:
            raise ValueError("I/O operation on closed file.")
        if reading and not self._reading:
            raise OSError("File not open for reading")
        if not reading and not self._writing:
            raise OSError("File not open for writing")

    def _fill(self) -> bool:
        if self._eof:
            return False
        data = self._sock.recv(8192)
        if not data:
            self._eof = True
            return False
        self._buf += data
        return True

    def read(self, size: int | None = -1) -> bytes:
        """At most size bytes (all until the end: size < 0)."""
        self._check(True)
        n = -1 if size is None else size
        while (n < 0 or len(self._buf) < n) and self._fill():
            pass
        if n < 0 or n > len(self._buf):
            n = len(self._buf)
        out = self._buf[:n]
        self._buf = self._buf[n:]
        return out

    def read1(self, size: int = -1) -> bytes:
        self._check(True)
        if not self._buf:
            self._fill()
        n = len(self._buf) if size < 0 or size > len(self._buf) else size
        out = self._buf[:n]
        self._buf = self._buf[n:]
        return out

    def peek(self, size: int = 0) -> bytes:
        self._check(True)
        if not self._buf:
            self._fill()
        return self._buf

    def readline(self, size: int | None = -1) -> bytes:
        self._check(True)
        limit = -1 if size is None else size
        while True:
            i = self._buf.find(b"\n")
            if i >= 0 or (limit >= 0 and len(self._buf) >= limit):
                break
            if not self._fill():
                break
        i = self._buf.find(b"\n")
        end = len(self._buf) if i < 0 else i + 1
        if limit >= 0 and end > limit:
            end = limit
        line = self._buf[:end]
        self._buf = self._buf[end:]
        return line

    def readlines(self, hint: int = -1) -> list[bytes]:
        out: list[bytes] = []
        total = 0
        while True:
            line = self.readline()
            if not line:
                return out
            out.append(line)
            total += len(line)
            if 0 < hint <= total:
                return out

    def __iter__(self):
        while True:
            line = self.readline()
            if not line:
                return
            yield line

    def write(self, b: bytes) -> int:
        self._check(False)
        self._sock.sendall(b)
        return len(b)

    def writelines(self, lines: list[bytes]) -> None:
        for line in lines:
            self.write(line)

    def flush(self) -> None:
        pass

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        self._sock._decref_socketios()

    def __enter__(self) -> "SocketIO":
        return self

    def __exit__(self, et, ev, tb) -> None:
        self.close()


class _TextSocketIO:
    """A socket as a text file (makefile("r") / ("w") / ("rw"))."""

    def __init__(self, raw: SocketIO, encoding: str, errors: str, newline: str | None) -> None:
        self.buffer = raw
        self.encoding = encoding
        self.errors = errors
        self._newline = newline
        self.mode = raw.mode.replace("b", "")

    @property
    def closed(self) -> bool:
        return self.buffer.closed

    def fileno(self) -> int:
        return self.buffer.fileno()

    def readable(self) -> bool:
        return self.buffer.readable()

    def writable(self) -> bool:
        return self.buffer.writable()

    def _text(self, b: bytes) -> str:
        t = b.decode(self.encoding, self.errors)
        if self._newline is None:
            t = t.replace("\r\n", "\n").replace("\r", "\n")
        return t

    def read(self, size: int = -1) -> str:
        return self._text(self.buffer.read(size))

    def readline(self, size: int = -1) -> str:
        return self._text(self.buffer.readline(size))

    def readlines(self) -> list[str]:
        return [self._text(b) for b in self.buffer.readlines()]

    def __iter__(self):
        while True:
            line = self.readline()
            if not line:
                return
            yield line

    def write(self, s: str) -> int:
        self.buffer.write(s.encode(self.encoding, self.errors))
        return len(s)

    def flush(self) -> None:
        pass

    def close(self) -> None:
        self.buffer.close()

    def __enter__(self) -> "_TextSocketIO":
        return self

    def __exit__(self, et, ev, tb) -> None:
        self.close()


def socketpair(family: int = AF_UNIX, type: int = SOCK_STREAM, proto: int = 0) -> tuple[socket, socket]:
    if sys.platform == "kolibrios":
        r = sys.syscall(75, 10)                       # fn 75.10
        if r[0] == -1:
            raise _kerror(r[1])
        return (socket(AF_INET, type, proto, r[0]), socket(AF_INET, type, proto, r[1]))
    b = sys.buffer(8)
    _os._check(sys.syscall(360, AF_UNIX, type | SOCK_CLOEXEC, proto, b)[0], None)
    a = socket(AF_INET, type, proto, sys.peek(b, 0, 4))
    c = socket(AF_INET, type, proto, sys.peek(b, 4, 4))
    a.family = AF_UNIX
    c.family = AF_UNIX
    return (a, c)


def getdefaulttimeout() -> float | None:
    return _default_timeout


def setdefaulttimeout(timeout: float | None) -> None:
    global _default_timeout
    _default_timeout = timeout


def create_connection(address: tuple[str, int], timeout: float | None = -1.0) -> socket:
    """A TCP connection to (host, port); timeout -1: the default timeout."""
    s = socket(AF_INET, SOCK_STREAM)
    if timeout is None or timeout >= 0:
        s.settimeout(timeout)
    try:
        s.connect(address)
    except OSError:
        s.close()
        raise
    return s


def create_server(address: tuple[str, int], family: int = AF_INET, backlog: int = -1, reuse_port: bool = False) -> socket:
    """A TCP socket listening at (host, port)."""
    s = socket(family, SOCK_STREAM)
    try:
        s.setsockopt(SOL_SOCKET, SO_REUSEADDR, 1)
        if reuse_port:
            s.setsockopt(SOL_SOCKET, SO_REUSEPORT, 1)
        s.bind(address)
        if backlog < 0:
            s.listen()
        else:
            s.listen(backlog)
    except OSError:
        s.close()
        raise
    return s


# ---------------------------------------------------------------- names

def gethostname() -> str:
    if sys.platform == "kolibrios":
        return "kolibrios"
    return _os.uname()[1]


def getfqdn(name: str = "") -> str:
    if not name:
        return gethostname()
    return name


def _read_text(path: str) -> str:
    try:
        fd = _os.open(path, _os.O_RDONLY, 0)
    except OSError:
        return ""
    out = b""
    try:
        while True:
            data = _os.read(fd, 4096)
            if not data:
                break
            out += data
    finally:
        _os.close(fd)
    return out.decode("utf-8", "replace")


def _hosts_lookup(name: str) -> str | None:
    if sys.platform == "kolibrios":
        return None
    for line in _read_text("/etc/hosts").split("\n"):
        line = line.split("#")[0]
        words = line.split()
        if len(words) >= 2 and _is_ipv4(words[0]):
            for w in words[1:]:
                if w.lower() == name.lower():
                    return words[0]
    return None


def _nameservers() -> list[str]:
    if sys.platform == "kolibrios":
        ip = sys.syscall(76, 0x10004, 0)[0] & 0xFFFFFFFF   # fn 76: IPv4 (1), get DNS (4), device 0
        if ip == 0 or ip == 0xFFFFFFFF:
            return []
        return [str(ip & 255) + "." + str((ip >> 8) & 255) + "." + str((ip >> 16) & 255) + "." + str((ip >> 24) & 255)]
    out: list[str] = []
    for line in _read_text("/etc/resolv.conf").split("\n"):
        words = line.split()
        if len(words) >= 2 and words[0] == "nameserver" and _is_ipv4(words[1]):
            out.append(words[1])
    return out


_dns_id = 0x1234


def _dns_name(data: bytes, off: int) -> int:
    """The offset past a (possibly compressed) name at off."""
    while off < len(data):
        n = data[off]
        if n == 0:
            return off + 1
        if n >= 0xC0:
            return off + 2
        off += n + 1
    return off


def _dns_query(name: str, server: str) -> list[str]:
    """The IPv4 addresses (A records) a name server gives for name."""
    global _dns_id
    _dns_id = (_dns_id * 75 + 74) % 65537 & 0xFFFF
    q = bytes([_dns_id >> 8, _dns_id & 255, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0])
    for label in name.rstrip(".").split("."):
        raw = label.encode()
        if not raw or len(raw) > 63:
            return []
        q += bytes([len(raw)]) + raw
    q += b"\0\0\1\0\1"
    s = socket(AF_INET, SOCK_DGRAM)
    try:
        s.settimeout(2.0)
        s.connect((server, 53))
        data = b""
        for attempt in range(2):
            s.send(q)
            try:
                data = s.recv(1500)
            except TimeoutError:
                continue
            if len(data) >= 12 and data[0] == q[0] and data[1] == q[1]:
                break
    except OSError:
        return []
    finally:
        s.close()
    if len(data) < 12 or data[0] != q[0] or data[1] != q[1] or (data[3] & 15) != 0:
        return []
    qd = data[4] * 256 + data[5]
    an = data[6] * 256 + data[7]
    off = 12
    for i in range(qd):
        off = _dns_name(data, off) + 4
    out: list[str] = []
    for i in range(an):
        off = _dns_name(data, off)
        if off + 10 > len(data):
            break
        rtype = data[off] * 256 + data[off + 1]
        rlen = data[off + 8] * 256 + data[off + 9]
        off += 10
        if rtype == 1 and rlen == 4 and off + 4 <= len(data):
            out.append(inet_ntoa(data[off:off + 4]))
        off += rlen
    return out


def gethostbyname(hostname: str) -> str:
    """The IPv4 address of a host name (a dotted address is itself)."""
    if _is_ipv4(hostname):
        return hostname
    if hostname == "":
        return "0.0.0.0"
    low = hostname.lower()
    if low == "localhost" or low == "localhost.":
        return "127.0.0.1"
    ip = _hosts_lookup(hostname)
    if ip is not None:
        return ip
    for server in _nameservers():
        ips = _dns_query(hostname, server)
        if ips:
            return ips[0]
    raise gaierror(EAI_NONAME, _EAI_NONAME_TEXT)


def gethostbyname_ex(hostname: str) -> tuple[str, list[str], list[str]]:
    return (hostname, [], [gethostbyname(hostname)])


def getaddrinfo(host: str, port: int, family: int = 0, type: int = 0, proto: int = 0, flags: int = 0) -> list[tuple[int, int, int, str, tuple[str, int]]]:
    """[(family, type, proto, canonname, (address, port))] for IPv4."""
    if family != 0 and family != AF_INET:
        raise gaierror(EAI_NONAME, _EAI_NONAME_TEXT)
    ip = gethostbyname(host)
    out: list[tuple[int, int, int, str, tuple[str, int]]] = []
    kinds: list[int] = [type]
    if type == 0:
        if sys.platform == "darwin":
            kinds = [SOCK_DGRAM, SOCK_STREAM]
        else:
            kinds = [SOCK_STREAM, SOCK_DGRAM, SOCK_RAW]
    for k in kinds:
        p = proto
        if p == 0:
            if k == SOCK_STREAM:
                p = IPPROTO_TCP
            elif k == SOCK_DGRAM:
                p = IPPROTO_UDP
        out.append((AF_INET, k, p, "", (ip, port)))
    return out
