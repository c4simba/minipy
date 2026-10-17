"""TLS for sockets, called through ctypes - the same module in the interpreter and in compiled
programs. Linux and macOS: the system's OpenSSL 3 (libssl, libcrypto: Linux's libssl.so.3 -
compiled programs are i386: the i386 library, loaded when TLS is first used -, Homebrew's openssl@3
on macOS; other folders: PLAINPY_LIBRARY_PATH). KolibriOS: mbedtls.obj, mbedTLS 2.16 as a KolibriOS
library (tools/kolibri-tls builds it; the port of the KolibriOS repository loads as well) - next to
the program or in /sys/lib, TLS 1.2. What CPython's ssl has for TLS clients and servers -
SSLContext, create_default_context, wrap_socket, the peer's certificate checked against the
system's certificate authorities (KolibriOS has none: cacert.pem next to the program) or given
ones and its host name (SNI sent) - but no callbacks (SNI, PSK, a key's password asked for),
sessions or MemoryBIO. Each system's library is behind the same two classes (_BCtx, _BConn)."""

import sys

if sys.platform not in ("linux", "darwin", "kolibrios"):
    raise ImportError("no ssl on " + sys.platform + ": plainpy's TLS is the system's OpenSSL 3 (Linux, macOS) or mbedtls.obj (KolibriOS)")

import socket
import ctypes

# ---------------------------------------------------------------- names CPython's ssl has

PROTOCOL_TLS = 2
PROTOCOL_TLS_CLIENT = 16
PROTOCOL_TLS_SERVER = 17
PROTOCOL_SSLv23 = PROTOCOL_TLS
CERT_NONE = 0
CERT_OPTIONAL = 1
CERT_REQUIRED = 2
OP_ALL = 0x80000850
OP_NO_SSLv2 = 0
OP_NO_SSLv3 = 0x02000000
OP_NO_TLSv1 = 0x04000000
OP_NO_TLSv1_1 = 0x10000000
OP_NO_COMPRESSION = 0x00020000
HAS_SNI = True
HAS_ALPN = True
HAS_TLSv1_3 = sys.platform != "kolibrios"
SSL_ERROR_SSL = 1
SSL_ERROR_WANT_READ = 2
SSL_ERROR_WANT_WRITE = 3
SSL_ERROR_SYSCALL = 5
SSL_ERROR_ZERO_RETURN = 6
SSL_ERROR_EOF = 8


class TLSVersion:
    MINIMUM_SUPPORTED = -2
    SSLv3 = 0x300
    TLSv1 = 0x301
    TLSv1_1 = 0x302
    TLSv1_2 = 0x303
    TLSv1_3 = 0x304
    MAXIMUM_SUPPORTED = -1


class Purpose:
    SERVER_AUTH = "SERVER_AUTH"         # (a client's context: it checks servers)
    CLIENT_AUTH = "CLIENT_AUTH"


class SSLError(OSError):
    """An error of the TLS library: reason ("CERTIFICATE_VERIFY_FAILED"), library ("SSL")."""

    def __init__(self, code: int = 1, message: str = "", reason: str | None = None, library: str | None = None):
        super().__init__(message)
        self.errno = code
        self.strerror = message
        self.reason = reason
        self.library = library

    def __str__(self) -> str:
        return self.strerror if self.strerror is not None else ""


class SSLZeroReturnError(SSLError):
    pass


class SSLWantReadError(SSLError):
    pass


class SSLWantWriteError(SSLError):
    pass


class SSLSyscallError(SSLError):
    pass


class SSLEOFError(SSLError):
    pass


class SSLCertVerificationError(SSLError):
    """The peer's certificate did not pass: verify_code, verify_message (OpenSSL's words)."""

    def __init__(self, code: int = 1, message: str = "", reason: str | None = None, library: str | None = None):
        super().__init__(code, message, reason, library)
        self.verify_code = 0
        self.verify_message = ""


CertificateError = SSLCertVerificationError


def _readable(path: str) -> None:
    """FileNotFoundError and the like, as CPython raises them, before the library is given the file."""
    f = open(path, "rb")
    f.close()


def _read_file(path: str) -> bytes:
    f = open(path, "rb")
    data = f.read()
    f.close()
    return data


def _verify_error(vm: str, code: int) -> SSLCertVerificationError:
    e = SSLCertVerificationError(1, "[SSL: CERTIFICATE_VERIFY_FAILED] certificate verify failed: " + vm, "CERTIFICATE_VERIFY_FAILED", "SSL")
    e.verify_code = code
    e.verify_message = vm
    return e


if not sys._compiled:
    def _load(name: str, hint: str):
        try:
            return ctypes.CDLL(name)
        except OSError as e:
            raise ImportError("ssl needs " + hint + " (" + str(e) + ")")

# ================================================================ KolibriOS: mbedtls.obj (mbedTLS 2.16)

if sys.platform == "kolibrios":
    if sys._compiled:
        _tls = ctypes.CDLL("mbedtls.obj")
    else:
        _tls = _load("mbedtls.obj", "mbedtls.obj next to the program or in /sys/lib (plainpy: tools/kolibri-tls)")
    _tls.mbedtls_ssl_config_init.restype = None
    _tls.mbedtls_ssl_config_init.argtypes = [ctypes.c_void_p]
    _tls.mbedtls_ssl_config_defaults.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
    _tls.mbedtls_ssl_config_free.restype = None
    _tls.mbedtls_ssl_config_free.argtypes = [ctypes.c_void_p]
    _tls.mbedtls_ssl_conf_authmode.restype = None
    _tls.mbedtls_ssl_conf_authmode.argtypes = [ctypes.c_void_p, ctypes.c_int]
    _tls.mbedtls_ssl_conf_rng.restype = None
    _tls.mbedtls_ssl_conf_rng.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    _tls.mbedtls_ssl_conf_ca_chain.restype = None
    _tls.mbedtls_ssl_conf_ca_chain.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    _tls.mbedtls_ssl_conf_own_cert.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    _tls.mbedtls_ssl_conf_alpn_protocols.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    _tls.mbedtls_ssl_conf_min_version.restype = None
    _tls.mbedtls_ssl_conf_min_version.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    _tls.mbedtls_ssl_conf_max_version.restype = None
    _tls.mbedtls_ssl_conf_max_version.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    _tls.mbedtls_ssl_init.restype = None
    _tls.mbedtls_ssl_init.argtypes = [ctypes.c_void_p]
    _tls.mbedtls_ssl_setup.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    _tls.mbedtls_ssl_free.restype = None
    _tls.mbedtls_ssl_free.argtypes = [ctypes.c_void_p]
    _tls.mbedtls_ssl_set_hostname.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    _tls.mbedtls_ssl_set_hs_authmode.restype = None
    _tls.mbedtls_ssl_set_hs_authmode.argtypes = [ctypes.c_void_p, ctypes.c_int]
    _tls.mbedtls_ssl_set_bio.restype = None
    _tls.mbedtls_ssl_set_bio.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    _tls.mbedtls_ssl_handshake.argtypes = [ctypes.c_void_p]
    _tls.mbedtls_ssl_read.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
    _tls.mbedtls_ssl_write.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
    _tls.mbedtls_ssl_close_notify.argtypes = [ctypes.c_void_p]
    _tls.mbedtls_ssl_get_verify_result.restype = ctypes.c_uint
    _tls.mbedtls_ssl_get_verify_result.argtypes = [ctypes.c_void_p]
    _tls.mbedtls_ssl_get_version.restype = ctypes.c_char_p
    _tls.mbedtls_ssl_get_version.argtypes = [ctypes.c_void_p]
    _tls.mbedtls_ssl_get_ciphersuite.restype = ctypes.c_char_p
    _tls.mbedtls_ssl_get_ciphersuite.argtypes = [ctypes.c_void_p]
    _tls.mbedtls_ssl_get_peer_cert.restype = ctypes.c_void_p
    _tls.mbedtls_ssl_get_peer_cert.argtypes = [ctypes.c_void_p]
    _tls.mbedtls_ssl_get_bytes_avail.restype = ctypes.c_size_t
    _tls.mbedtls_ssl_get_bytes_avail.argtypes = [ctypes.c_void_p]
    _tls.mbedtls_x509_crt_init.restype = None
    _tls.mbedtls_x509_crt_init.argtypes = [ctypes.c_void_p]
    _tls.mbedtls_x509_crt_parse.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
    _tls.mbedtls_x509_crt_free.restype = None
    _tls.mbedtls_x509_crt_free.argtypes = [ctypes.c_void_p]
    _tls.mbedtls_x509_crt_info.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p]
    _tls.mbedtls_x509_crt_verify_info.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_uint]
    _tls.mbedtls_pk_init.restype = None
    _tls.mbedtls_pk_init.argtypes = [ctypes.c_void_p]
    _tls.mbedtls_pk_parse_key.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
    _tls.mbedtls_pk_free.restype = None
    _tls.mbedtls_pk_free.argtypes = [ctypes.c_void_p]
    _tls.mbedtls_hmac_drbg_init.restype = None
    _tls.mbedtls_hmac_drbg_init.argtypes = [ctypes.c_void_p]
    _tls.mbedtls_hmac_drbg_seed_buf.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
    _tls.mbedtls_hmac_drbg_free.restype = None
    _tls.mbedtls_hmac_drbg_free.argtypes = [ctypes.c_void_p]
    _tls.mbedtls_md_info_from_type.restype = ctypes.c_void_p
    _tls.mbedtls_md_info_from_type.argtypes = [ctypes.c_int]
    _tls.mbedtls_strerror.restype = None
    _tls.mbedtls_strerror.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t]
    _tls.mbedtls_version_get_string.restype = None
    _tls.mbedtls_version_get_string.argtypes = [ctypes.c_void_p]
    _tls.mbedtls_sha256_ret.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_int]
    _tls.ppy_entropy.argtypes = [ctypes.c_void_p, ctypes.c_size_t]

    if sys._compiled:                 # (no call when the module is imported: http.client imports it)
        OPENSSL_VERSION = "mbed TLS 2.16"
        OPENSSL_VERSION_NUMBER = 0x02100000
    else:
        _vb = sys.buffer(32)
        _tls.mbedtls_version_get_string(_vb)
        OPENSSL_VERSION = "mbed TLS " + sys.cstr_at(sys.addr(_vb), 32)
        OPENSSL_VERSION_NUMBER = 0x02100000

    # (the sizes of mbedTLS's structures - 280, 208, 312, 8, 100 and 4 bytes in this build - with room
    # to spare for another build of the library)
    _SSL_SIZE = 640
    _CONF_SIZE = 512
    _CRT_SIZE = 640
    _PK_SIZE = 64
    _DRBG_SIZE = 256
    _WANT_READ = -0x6900
    _WANT_WRITE = -0x6880
    _CLOSE_NOTIFY = -0x7880
    _CONN_EOF = -0x7280
    _VERIFY_FAILED = -0x2700
    _ktable: list[int] = []
    _kfuncs: dict[str, int] = {}
    _kready: list[int] = []

    def _klibrary() -> int:
        """The export table of mbedtls.obj, found where ctypes found it (function 68.19 gives a library
        loaded once: the same table): the name as it is, the program's folder (the script's), /sys/lib."""
        if _ktable:
            return _ktable[0]
        prog = sys.argv[0] if len(sys.argv) > 0 else ""
        here = prog[:prog.rfind("/")] if "/" in prog else ""
        for path in ("mbedtls.obj", here + "/mbedtls.obj", "/sys/lib/mbedtls.obj", "/kolibrios/lib/mbedtls.obj"):
            r = sys.syscall(68, 19, path)[0]
            if r != 0:
                _ktable.append(r)
                return r
        raise SSLError(1, "cannot load mbedtls.obj")

    def _kfunc(name: str) -> int:
        """A function's address in mbedtls.obj (0: it has none of that name) - for the callbacks mbedTLS takes."""
        if name in _kfuncs:
            return _kfuncs[name]
        e = _klibrary()
        while True:
            p = sys.peek_at(e, 4) & 0xFFFFFFFF
            if p == 0:
                break
            if sys.cstr_at(p, 64) == name:
                a = sys.peek_at(e + 4, 4) & 0xFFFFFFFF
                _kfuncs[name] = a
                return a
            e += 8
        _kfuncs[name] = 0
        return 0

    def _kstart() -> None:
        if not _kready:
            _tls.mbedtls_init()       # (the port's mbedtls.obj resolves its imports here; plainpy's has none)
            _kready.append(1)

    def _kmessage(code: int) -> str:
        b = sys.buffer(256)
        _tls.mbedtls_strerror(code, b, 256)
        return sys.cstr_at(sys.addr(b), 256)

    def _kerror(code: int, what: str) -> SSLError:
        """mbedTLS's error code as an SSLError: "SSL - The connection indicated an EOF" -> reason CONNECTION_INDICATED_AN_EOF."""
        text = _kmessage(code)
        lib, sep, rest = text.partition(" - ")
        if not sep:
            rest = text
            lib = "SSL"
        reason = rest.upper().replace(" ", "_").replace("-", "_").replace(".", "").replace(",", "")
        if reason.startswith("THE_"):
            reason = reason[4:]
        return SSLError(1, "[" + lib + ": " + reason[:48] + "] " + what + ": " + rest, reason[:48], lib)

    def _kseed(n: int) -> buffer:
        """n bytes to seed the random generator: ppy_entropy (plainpy's mbedtls.obj: RDRAND, the time stamp
        counter's jitter), else SHA-256 of the high-precision counter's jitter (function 26.10)."""
        b = sys.buffer(n)
        if _kfunc("ppy_entropy") != 0 and _tls.ppy_entropy(b, n) == 0:
            return b
        pool = sys.buffer(4096)
        for i in range(1024):
            t = sys.syscall(26, 10)[0]
            for k in range(t & 7):
                t += sys.syscall(26, 9)[0]
            sys.poke(pool, 4 * i, t ^ sys.syscall(26, 10)[0], 4)
        h = sys.buffer(32)
        k = 0
        while k < n:
            sys.poke(pool, 0, k, 4)
            _tls.mbedtls_sha256_ret(pool, 4096, h, 0)
            for j in range(32):
                if k + j < n:
                    sys.poke(b, k + j, sys.peek(h, j, 1), 1)
            k += 32
        return b

    def _kdate(text: str) -> str:
        """mbedTLS's "2026-10-16 20:32:21" as OpenSSL prints it: "Oct 16 20:32:21 2026 GMT"."""
        months = ("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")
        day, sep, clock = text.partition(" ")
        parts = day.split("-")
        if len(parts) != 3:
            return text
        d = int(parts[2])
        return months[int(parts[1]) - 1] + " " + (" " if d < 10 else "") + str(d) + " " + clock + " " + parts[0] + " GMT"

    def _kname(text: str) -> str:
        """mbedTLS's "C=XX, O=org, CN=a" in RFC 2253's order, as OpenSSL prints it: "CN=a,O=org,C=XX"."""
        parts = text.split(", ")
        out: list[str] = []
        for i in range(len(parts) - 1, -1, -1):
            out.append(parts[i])
        return ",".join(out)

    def _kfields(crt: int) -> dict[str, str]:
        """A certificate's fields, mbedtls_x509_crt_info's text as OpenSSL's backend gives them."""
        b = sys.buffer(8192)
        _tls.mbedtls_x509_crt_info(b, 8192, "", crt)
        d: dict[str, str] = {}
        for line in sys.cstr_at(sys.addr(b), 8192).split("\n"):
            key, sep, value = line.partition(":")
            if not sep:
                continue
            key = " ".join(key.split())
            value = value.strip()
            if key == "cert. version":
                d["version"] = value
            elif key == "serial number":
                d["serialNumber"] = value.replace(":", "")
            elif key == "issuer name":
                d["issuer"] = _kname(value)
            elif key == "subject name":
                d["subject"] = _kname(value)
            elif key == "issued on":
                d["notBefore"] = _kdate(value)
            elif key == "expires on":
                d["notAfter"] = _kdate(value)
            elif key == "subject alt name":
                alt: list[str] = []
                for name in value.split(","):
                    if name.strip():
                        alt.append("DNS:" + name.strip())
                d["subjectAltName"] = ", ".join(alt)
        return d

    def _kparse_crt(crt: buffer, data: bytes, pem: bool) -> None:
        """Certificates of PEM text (one or more) or DER into crt."""
        if pem and not data.endswith(b"\0"):
            data = data + b"\0"
        r = _tls.mbedtls_x509_crt_parse(crt, data, len(data))
        if r < 0:
            raise _kerror(r, "cannot read the certificates")

    class _BCtx:
        """KolibriOS's context: an mbedtls_ssl_config, its random generator, the certificates."""

        def __init__(self, server: bool):
            _kstart()
            self.server = server
            self.conf = sys.buffer(_CONF_SIZE)
            self.drbg = sys.buffer(_DRBG_SIZE)
            self.ca = sys.buffer(_CRT_SIZE)
            self.own = sys.buffer(_CRT_SIZE)
            self.key = sys.buffer(_PK_SIZE)
            self.alpn = sys.buffer(4)
            self.alpn_text = sys.buffer(4)
            self.has_ca = False
            self.mode = 0
            self.min_minor = 3
            self.max_minor = 3
            _tls.mbedtls_ssl_config_init(self.conf)
            _tls.mbedtls_hmac_drbg_init(self.drbg)
            _tls.mbedtls_x509_crt_init(self.ca)
            _tls.mbedtls_x509_crt_init(self.own)
            _tls.mbedtls_pk_init(self.key)
            r = _tls.mbedtls_ssl_config_defaults(self.conf, 1 if server else 0, 0, 0)
            if r != 0:
                raise _kerror(r, "cannot make an SSL context")
            seed = _kseed(48)
            r = _tls.mbedtls_hmac_drbg_seed_buf(self.drbg, _tls.mbedtls_md_info_from_type(6), seed, 48)   # MBEDTLS_MD_SHA256
            if r != 0:
                raise _kerror(r, "cannot seed the random generator")
            _tls.mbedtls_ssl_conf_rng(self.conf, _kfunc("mbedtls_hmac_drbg_random"), self.drbg)
            _tls.mbedtls_ssl_conf_min_version(self.conf, 3, 3)          # TLS 1.2, as CPython's least
            _tls.mbedtls_ssl_conf_ca_chain(self.conf, self.ca, None)    # (empty until certificates come: a peer is then not trusted, as CPython says)

        def set_verify(self, mode: int) -> None:
            self.mode = mode
            _tls.mbedtls_ssl_conf_authmode(self.conf, mode)            # (0 none, 1 optional, 2 required: CPython's numbers)

        def default_verify_paths(self) -> None:
            """KolibriOS has no store of certificate authorities: cacert.pem (curl.se/ca) next to the program,
            /sys/settings/cacert.pem or /kolibrios/etc/cacert.pem."""
            prog = sys.argv[0] if len(sys.argv) > 0 else ""
            here = prog[:prog.rfind("/")] if "/" in prog else "."
            for path in (here + "/cacert.pem", "/sys/settings/cacert.pem", "/kolibrios/etc/cacert.pem"):
                try:
                    data = _read_file(path)
                except OSError:
                    continue
                self.load_ca_data(data, True)
                return

        def load_ca_file(self, cafile: str | None, capath: str | None) -> None:
            if capath is not None:
                raise NotImplementedError("load_verify_locations(capath=...): a folder of certificates is not supported on KolibriOS")
            if cafile is not None:
                self.load_ca_data(_read_file(cafile), True)

        def load_ca_data(self, data: bytes, pem: bool) -> None:
            _kparse_crt(self.ca, data, pem)
            self.has_ca = True

        def load_cert_chain(self, certfile: str, keyfile: str) -> None:
            _kparse_crt(self.own, _read_file(certfile), True)
            key = _read_file(keyfile) + b"\0"
            r = _tls.mbedtls_pk_parse_key(self.key, key, len(key), None, 0)
            if r != 0:
                raise _kerror(r, "cannot load the private key")
            r = _tls.mbedtls_ssl_conf_own_cert(self.conf, self.own, self.key)
            if r != 0:
                raise _kerror(r, "cannot use the certificate")

        def set_ciphers(self, ciphers: str) -> None:
            pass                                                       # (OpenSSL's cipher strings: mbedTLS's defaults stay)

        def set_alpn(self, protocols: list[str]) -> None:
            text = b""
            for p in protocols:
                text += p.encode("ascii") + b"\0"
            self.alpn_text = sys.buffer(len(text) + 1)
            self.alpn = sys.buffer(4 * len(protocols) + 4)
            at = 0
            for i in range(len(protocols)):
                n = len(protocols[i])
                for k in range(n):
                    sys.poke(self.alpn_text, at + k, ord(protocols[i][k]), 1)
                sys.poke(self.alpn, 4 * i, sys.addr(self.alpn_text) + at, 4)
                at += n + 1
            r = _tls.mbedtls_ssl_conf_alpn_protocols(self.conf, self.alpn)
            if r != 0:
                raise _kerror(r, "cannot set the ALPN protocols")

        def set_version(self, minimum: bool, version: int) -> None:
            minor = 3
            if version == TLSVersion.MINIMUM_SUPPORTED:
                minor = 1
            elif version > 0x300:
                minor = version - 0x300
            if minor > 3:
                minor = 3                                              # (TLS 1.2 is mbedTLS 2's newest)
            if minimum:
                self.min_minor = minor
                _tls.mbedtls_ssl_conf_min_version(self.conf, 3, minor)
            else:
                self.max_minor = minor
                _tls.mbedtls_ssl_conf_max_version(self.conf, 3, minor)

        def get_version(self, minimum: bool) -> int:
            return 0x300 + (self.min_minor if minimum else self.max_minor)

        def free(self) -> None:
            _tls.mbedtls_ssl_config_free(self.conf)
            _tls.mbedtls_hmac_drbg_free(self.drbg)
            _tls.mbedtls_x509_crt_free(self.ca)
            _tls.mbedtls_x509_crt_free(self.own)
            _tls.mbedtls_pk_free(self.key)

    class _BConn:
        """KolibriOS's connection: an mbedtls_ssl_context over the socket (function 75's number)."""

        def __init__(self, ctx: _BCtx, fd: int, server: bool, hostname: str | None, check_hostname: bool, waits: bool):
            self.ctx = ctx
            self.hostname = hostname
            self.own_check = False
            self.ssl = sys.buffer(_SSL_SIZE)
            self.net = sys.buffer(16)
            sys.poke(self.net, 0, fd, 4)
            _tls.mbedtls_ssl_init(self.ssl)
            r = _tls.mbedtls_ssl_setup(self.ssl, ctx.conf)
            if r != 0:
                raise _kerror(r, "cannot make a TLS connection")
            if hostname is not None and not server:
                r = _tls.mbedtls_ssl_set_hostname(self.ssl, hostname)  # (SNI - and the name mbedTLS checks the certificate for:
                if r != 0:                                             # without check_hostname, the check is made here, the
                    raise _kerror(r, "cannot set the host name")       # name left out of it)
                if not check_hostname and ctx.mode == CERT_REQUIRED:
                    _tls.mbedtls_ssl_set_hs_authmode(self.ssl, 1)
                    self.own_check = True
            send = _kfunc("mbedtls_net_send" if waits else "ppy_net_send_nb")
            recv = _kfunc("mbedtls_net_recv" if waits else "ppy_net_recv_nb")
            if send == 0 or recv == 0:
                send = _kfunc("mbedtls_net_send")
                recv = _kfunc("mbedtls_net_recv")
            _tls.mbedtls_ssl_set_bio(self.ssl, self.net, send, recv, None)

        def _failed(self, r: int, what: str) -> SSLError:
            if r == _VERIFY_FAILED:
                return self._unverified(_tls.mbedtls_ssl_get_verify_result(self.ssl))
            return _kerror(r, what)

        def _unverified(self, flags: int) -> SSLCertVerificationError:
            """mbedTLS's verification flags as CPython words the first failure."""
            vm = ""
            if flags & 0x08:                                           # MBEDTLS_X509_BADCERT_NOT_TRUSTED
                vm = "unable to get local issuer certificate"
                crt = _tls.mbedtls_ssl_get_peer_cert(self.ssl)         # (after the handshake; during it: ssl->session_negotiate
                if crt == 0:                                           # ->peer_cert, offsets of this mbedTLS 2.16 build)
                    sn = sys.peek_at(sys.addr(self.ssl) + 56, 4) & 0xFFFFFFFF
                    crt = sys.peek_at(sn + 96, 4) & 0xFFFFFFFF if sn != 0 else 0
                if crt != 0:
                    f = _kfields(crt)
                    if f.get("subject", "") == f.get("issuer", "-"):
                        vm = "self-signed certificate"
            elif flags & 0x04 and self.hostname is not None:           # CN_MISMATCH
                vm = "Hostname mismatch, certificate is not valid for '" + self.hostname + "'."
            elif flags & 0x01:
                vm = "certificate has expired"
            elif flags & 0x200:
                vm = "certificate is not yet valid"
            else:
                b = sys.buffer(512)
                _tls.mbedtls_x509_crt_verify_info(b, 512, "", flags)
                vm = sys.cstr_at(sys.addr(b), 512).strip()
            return _verify_error(vm, flags)

        def handshake(self) -> int:
            """0: done, 1 / 2: call again when the socket can be read / written."""
            r = _tls.mbedtls_ssl_handshake(self.ssl)
            if r == 0:
                if self.own_check:
                    flags = _tls.mbedtls_ssl_get_verify_result(self.ssl) & ~0x04    # (all but MBEDTLS_X509_BADCERT_CN_MISMATCH)
                    if flags != 0:
                        raise self._unverified(flags)
                return 0
            if r == _WANT_READ:
                return 1
            if r == _WANT_WRITE:
                return 2
            raise self._failed(r, "handshake failed")

        def read(self, buf: buffer, n: int) -> int:
            """bytes read, 0: the end of the data, -1 / -2: again when the socket can be read / written."""
            r = _tls.mbedtls_ssl_read(self.ssl, buf, n)
            if r > 0:
                return r
            if r == 0 or r == _CLOSE_NOTIFY or r == _CONN_EOF:
                return 0
            if r == _WANT_READ:
                return -1
            if r == _WANT_WRITE:
                return -2
            raise self._failed(r, "read failed")

        def write(self, data: bytes) -> int:
            r = _tls.mbedtls_ssl_write(self.ssl, data, len(data))
            if r > 0:
                return r
            if r == _WANT_READ:
                return -1
            if r == _WANT_WRITE:
                return -2
            raise self._failed(r, "write failed")

        def pending(self) -> int:
            return _tls.mbedtls_ssl_get_bytes_avail(self.ssl)

        def version(self) -> str | None:
            return _tls.mbedtls_ssl_get_version(self.ssl)

        def cipher(self) -> tuple[str, str, int] | None:
            name = _tls.mbedtls_ssl_get_ciphersuite(self.ssl)
            if name is None:
                return None
            bits = 0
            if "AES-256" in name or "CHACHA20" in name or "CAMELLIA-256" in name or "ARIA-256" in name:
                bits = 256
            elif "AES-128" in name or "CAMELLIA-128" in name or "ARIA-128" in name:
                bits = 128
            v = self.version()
            return (name, v if v is not None else "", bits)

        def peer_der(self) -> bytes | None:
            crt = _tls.mbedtls_ssl_get_peer_cert(self.ssl)
            if crt == 0:
                return None
            n = sys.peek_at(crt + 4, 4) & 0xFFFFFFFF           # (raw: tag, len, p)
            p = sys.peek_at(crt + 8, 4) & 0xFFFFFFFF
            b = sys.buffer(n)
            for i in range(n):
                sys.poke(b, i, sys.peek_at(p + i, 1), 1)
            return sys.peek_bytes(b, 0, n)

        def peer_fields(self) -> dict[str, str] | None:
            crt = _tls.mbedtls_ssl_get_peer_cert(self.ssl)
            if crt == 0:
                return None
            return _kfields(crt)

        def shutdown(self) -> None:
            _tls.mbedtls_ssl_close_notify(self.ssl)

        def free(self) -> None:
            _tls.mbedtls_ssl_free(self.ssl)

# ================================================================ Linux, macOS: OpenSSL 3

else:
    if sys._compiled:
        if sys.platform == "darwin":
            _ssl = ctypes.CDLL("libssl.3.dylib")
            _crypto = ctypes.CDLL("libcrypto.3.dylib")
        else:
            _ssl = ctypes.CDLL("libssl.so.3")
            _crypto = ctypes.CDLL("libcrypto.so.3")
    else:
        _hint = "OpenSSL 3 (macOS: brew install openssl@3, Linux: its libssl3 package; PLAINPY_LIBRARY_PATH names other folders)"
        if sys.platform == "darwin":
            _ssl = _load("libssl.3.dylib", _hint)
            _crypto = _load("libcrypto.3.dylib", _hint)
        else:
            _ssl = _load("libssl.so.3", _hint)
            _crypto = _load("libcrypto.so.3", _hint)

    _ssl.TLS_client_method.restype = ctypes.c_void_p
    _ssl.TLS_server_method.restype = ctypes.c_void_p
    _ssl.SSL_CTX_new.restype = ctypes.c_void_p
    _ssl.SSL_CTX_new.argtypes = [ctypes.c_void_p]
    _ssl.SSL_CTX_free.restype = None
    _ssl.SSL_CTX_free.argtypes = [ctypes.c_void_p]
    _ssl.SSL_CTX_set_verify.restype = None
    _ssl.SSL_CTX_set_verify.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
    _ssl.SSL_CTX_set_default_verify_paths.argtypes = [ctypes.c_void_p]
    _ssl.SSL_CTX_load_verify_locations.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    _ssl.SSL_CTX_use_certificate_chain_file.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    _ssl.SSL_CTX_use_PrivateKey_file.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
    _ssl.SSL_CTX_check_private_key.argtypes = [ctypes.c_void_p]
    _ssl.SSL_CTX_set_cipher_list.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    _ssl.SSL_CTX_set_alpn_protos.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint]
    _ssl.SSL_CTX_ctrl.restype = ctypes.c_long
    _ssl.SSL_CTX_ctrl.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_long, ctypes.c_void_p]
    _ssl.SSL_CTX_get_cert_store.restype = ctypes.c_void_p
    _ssl.SSL_CTX_get_cert_store.argtypes = [ctypes.c_void_p]
    _ssl.SSL_new.restype = ctypes.c_void_p
    _ssl.SSL_new.argtypes = [ctypes.c_void_p]
    _ssl.SSL_free.restype = None
    _ssl.SSL_free.argtypes = [ctypes.c_void_p]
    _ssl.SSL_set_fd.argtypes = [ctypes.c_void_p, ctypes.c_int]
    _ssl.SSL_set_connect_state.restype = None
    _ssl.SSL_set_connect_state.argtypes = [ctypes.c_void_p]
    _ssl.SSL_set_accept_state.restype = None
    _ssl.SSL_set_accept_state.argtypes = [ctypes.c_void_p]
    _ssl.SSL_ctrl.restype = ctypes.c_long
    _ssl.SSL_ctrl.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_long, ctypes.c_void_p]
    _ssl.SSL_set1_host.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    _ssl.SSL_do_handshake.argtypes = [ctypes.c_void_p]
    _ssl.SSL_read.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
    _ssl.SSL_write.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
    _ssl.SSL_pending.argtypes = [ctypes.c_void_p]
    _ssl.SSL_get_error.argtypes = [ctypes.c_void_p, ctypes.c_int]
    _ssl.SSL_shutdown.argtypes = [ctypes.c_void_p]
    _ssl.SSL_get_verify_result.restype = ctypes.c_long
    _ssl.SSL_get_verify_result.argtypes = [ctypes.c_void_p]
    _ssl.SSL_get_version.restype = ctypes.c_char_p
    _ssl.SSL_get_version.argtypes = [ctypes.c_void_p]
    _ssl.SSL_get_current_cipher.restype = ctypes.c_void_p
    _ssl.SSL_get_current_cipher.argtypes = [ctypes.c_void_p]
    _ssl.SSL_CIPHER_get_name.restype = ctypes.c_char_p
    _ssl.SSL_CIPHER_get_name.argtypes = [ctypes.c_void_p]
    _ssl.SSL_CIPHER_get_version.restype = ctypes.c_char_p
    _ssl.SSL_CIPHER_get_version.argtypes = [ctypes.c_void_p]
    _ssl.SSL_CIPHER_get_bits.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    _ssl.SSL_get1_peer_certificate.restype = ctypes.c_void_p
    _ssl.SSL_get1_peer_certificate.argtypes = [ctypes.c_void_p]

    _crypto.OpenSSL_version.restype = ctypes.c_char_p
    _crypto.OpenSSL_version.argtypes = [ctypes.c_int]
    _crypto.OpenSSL_version_num.restype = ctypes.c_ulong
    _crypto.ERR_get_error.restype = ctypes.c_ulong
    _crypto.ERR_peek_error.restype = ctypes.c_ulong
    _crypto.ERR_clear_error.restype = None
    _crypto.ERR_reason_error_string.restype = ctypes.c_char_p
    _crypto.ERR_reason_error_string.argtypes = [ctypes.c_ulong]
    _crypto.X509_verify_cert_error_string.restype = ctypes.c_char_p
    _crypto.X509_verify_cert_error_string.argtypes = [ctypes.c_long]
    _crypto.X509_free.restype = None
    _crypto.X509_free.argtypes = [ctypes.c_void_p]
    _crypto.X509_get_subject_name.restype = ctypes.c_void_p
    _crypto.X509_get_subject_name.argtypes = [ctypes.c_void_p]
    _crypto.X509_get_issuer_name.restype = ctypes.c_void_p
    _crypto.X509_get_issuer_name.argtypes = [ctypes.c_void_p]
    _crypto.X509_get_version.restype = ctypes.c_long
    _crypto.X509_get_version.argtypes = [ctypes.c_void_p]
    _crypto.X509_get0_notBefore.restype = ctypes.c_void_p
    _crypto.X509_get0_notBefore.argtypes = [ctypes.c_void_p]
    _crypto.X509_get0_notAfter.restype = ctypes.c_void_p
    _crypto.X509_get0_notAfter.argtypes = [ctypes.c_void_p]
    _crypto.X509_get_serialNumber.restype = ctypes.c_void_p
    _crypto.X509_get_serialNumber.argtypes = [ctypes.c_void_p]
    _crypto.X509_get_ext_by_NID.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    _crypto.X509_get_ext.restype = ctypes.c_void_p
    _crypto.X509_get_ext.argtypes = [ctypes.c_void_p, ctypes.c_int]
    _crypto.X509V3_EXT_print.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int]
    _crypto.X509_NAME_print_ex.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_ulong]
    _crypto.ASN1_TIME_print.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    _crypto.i2a_ASN1_INTEGER.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    _crypto.i2d_X509_bio.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    _crypto.PEM_read_bio_X509.restype = ctypes.c_void_p
    _crypto.PEM_read_bio_X509.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    _crypto.d2i_X509_bio.restype = ctypes.c_void_p
    _crypto.d2i_X509_bio.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    _crypto.X509_STORE_add_cert.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    _crypto.BIO_s_mem.restype = ctypes.c_void_p
    _crypto.BIO_new.restype = ctypes.c_void_p
    _crypto.BIO_new.argtypes = [ctypes.c_void_p]
    _crypto.BIO_new_mem_buf.restype = ctypes.c_void_p
    _crypto.BIO_new_mem_buf.argtypes = [ctypes.c_void_p, ctypes.c_int]
    _crypto.BIO_read.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
    _crypto.BIO_free.argtypes = [ctypes.c_void_p]

    if sys._compiled:                 # (no C call when the module is imported: http.client imports it)
        OPENSSL_VERSION = "OpenSSL 3"
        OPENSSL_VERSION_NUMBER = 0x30000000
    else:
        OPENSSL_VERSION = _crypto.OpenSSL_version(0) or "OpenSSL"
        OPENSSL_VERSION_NUMBER = _crypto.OpenSSL_version_num()

    _LIBS = {2: "SYS", 6: "EVP", 9: "PEM", 11: "X509", 13: "ASN1", 20: "SSL", 32: "BIO", 34: "X509V3"}

    def _error(what: str, ssl_handle: int = 0, host: str | None = None) -> SSLError:
        """The error OpenSSL queued (the first one; the queue emptied), as CPython words it."""
        code = _crypto.ERR_get_error()
        e = code
        while e != 0:                                   # (the rest: not needed)
            e = _crypto.ERR_get_error()
        if code == 0:
            return SSLError(1, what)
        text = _crypto.ERR_reason_error_string(code)
        if text is None:
            text = "error " + str(code & 0x7FFFFF)
        lib = _LIBS.get((code >> 23) & 0xFF, "SSL")
        reason = text.upper().replace(" ", "_").replace("-", "_")
        msg = "[" + lib + ": " + reason + "] " + text
        if reason == "CERTIFICATE_VERIFY_FAILED" and ssl_handle != 0:
            vc = _ssl.SSL_get_verify_result(ssl_handle)
            vm = _crypto.X509_verify_cert_error_string(vc) or ""
            if vc == 62 and host is not None:             # X509_V_ERR_HOSTNAME_MISMATCH, IP_ADDRESS_MISMATCH: as CPython words them
                vm = "Hostname mismatch, certificate is not valid for '" + host + "'."
            elif vc == 64 and host is not None:
                vm = "IP address mismatch, certificate is not valid for '" + host + "'."
            return _verify_error(vm, vc)
        return SSLError(1, msg, reason, lib)

    def _bio_text(bio: int) -> str:
        """What a memory BIO holds (then freed)."""
        out: list[bytes] = []
        buf = sys.buffer(1024)
        while True:
            n = _crypto.BIO_read(bio, buf, 1024)
            if n <= 0:
                break
            out.append(sys.peek_bytes(buf, 0, n))
        _crypto.BIO_free(bio)
        return b"".join(out).decode("utf-8", "replace")

    def _name_text(name: int) -> str:
        """A name in RFC 2253's text, most significant part last (CN=a,O=b,C=c)."""
        b = _crypto.BIO_new(_crypto.BIO_s_mem())
        _crypto.X509_NAME_print_ex(b, name, 0, 0x1110313)   # XN_FLAG_RFC2253, but UTF-8 as it is (not ASN1_STRFLGS_ESC_MSB)
        return _bio_text(b)

    def _cert_fields(x: int) -> dict[str, str]:
        """The certificate's fields as OpenSSL prints them: subject, issuer (RFC 2253), version,
        serialNumber, notBefore, notAfter, subjectAltName ("DNS:a, IP Address:1.2.3.4")."""
        d: dict[str, str] = {}
        d["subject"] = _name_text(_crypto.X509_get_subject_name(x))
        d["issuer"] = _name_text(_crypto.X509_get_issuer_name(x))
        d["version"] = str(_crypto.X509_get_version(x) + 1)
        b = _crypto.BIO_new(_crypto.BIO_s_mem())
        _crypto.i2a_ASN1_INTEGER(b, _crypto.X509_get_serialNumber(x))
        d["serialNumber"] = _bio_text(b)
        b = _crypto.BIO_new(_crypto.BIO_s_mem())
        _crypto.ASN1_TIME_print(b, _crypto.X509_get0_notBefore(x))
        d["notBefore"] = _bio_text(b)
        b = _crypto.BIO_new(_crypto.BIO_s_mem())
        _crypto.ASN1_TIME_print(b, _crypto.X509_get0_notAfter(x))
        d["notAfter"] = _bio_text(b)
        k = _crypto.X509_get_ext_by_NID(x, 85, -1)        # NID_subject_alt_name
        if k >= 0:
            b = _crypto.BIO_new(_crypto.BIO_s_mem())
            _crypto.X509V3_EXT_print(b, _crypto.X509_get_ext(x, k), 0, 0)
            d["subjectAltName"] = _bio_text(b)
        return d

    def _der(x: int) -> bytes:
        b = _crypto.BIO_new(_crypto.BIO_s_mem())
        _crypto.i2d_X509_bio(b, x)
        out: list[bytes] = []
        buf = sys.buffer(4096)
        while True:
            n = _crypto.BIO_read(b, buf, 4096)
            if n <= 0:
                break
            out.append(sys.peek_bytes(buf, 0, n))
        _crypto.BIO_free(b)
        return b"".join(out)

    class _BCtx:
        """OpenSSL's context: an SSL_CTX."""

        def __init__(self, server: bool):
            self.server = server
            self.ctx = _ssl.SSL_CTX_new(_ssl.TLS_server_method() if server else _ssl.TLS_client_method())
            if self.ctx == 0:
                raise _error("cannot make an SSL context")
            self.keep: list[bytes] = []                    # (data OpenSSL refers to: kept as long as the context)
            _ssl.SSL_CTX_ctrl(self.ctx, 123, 0x303, None)  # SSL_CTRL_SET_MIN_PROTO_VERSION: TLS 1.2, as CPython

        def set_verify(self, mode: int) -> None:
            v = 0
            if mode != CERT_NONE:
                v = 1                                      # SSL_VERIFY_PEER
                if self.server and mode == CERT_REQUIRED:
                    v |= 2                                 # SSL_VERIFY_FAIL_IF_NO_PEER_CERT
            _ssl.SSL_CTX_set_verify(self.ctx, v, None)

        def default_verify_paths(self) -> None:
            if _ssl.SSL_CTX_set_default_verify_paths(self.ctx) != 1:
                raise _error("cannot load the default certificates")

        def load_ca_file(self, cafile: str | None, capath: str | None) -> None:
            if _ssl.SSL_CTX_load_verify_locations(self.ctx, cafile, capath) != 1:
                e = _error("cannot load the certificates")
                if e.reason == "NO_SUCH_FILE" or e.reason == "SYSTEM_LIB":
                    raise FileNotFoundError(2, str(e))
                raise e

        def load_ca_data(self, data: bytes, pem: bool) -> None:
            self.keep.append(data)
            store = _ssl.SSL_CTX_get_cert_store(self.ctx)
            bio = _crypto.BIO_new_mem_buf(data, len(data))
            n = 0
            while True:
                if pem:
                    x = _crypto.PEM_read_bio_X509(bio, None, None, None)
                else:
                    x = _crypto.d2i_X509_bio(bio, None)
                if x == 0:
                    break
                _crypto.X509_STORE_add_cert(store, x)
                _crypto.X509_free(x)
                n += 1
            _crypto.BIO_free(bio)
            _crypto.ERR_clear_error()                      # (the end of the data: an error OpenSSL queues)
            if n == 0:
                raise SSLError(1, "no certificate in cadata")

        def load_cert_chain(self, certfile: str, keyfile: str) -> None:
            if _ssl.SSL_CTX_use_certificate_chain_file(self.ctx, certfile) != 1:
                raise _error("cannot load the certificate " + certfile)
            if _ssl.SSL_CTX_use_PrivateKey_file(self.ctx, keyfile, 1) != 1:   # SSL_FILETYPE_PEM
                raise _error("cannot load the private key")
            if _ssl.SSL_CTX_check_private_key(self.ctx) != 1:
                raise _error("the private key does not match the certificate")

        def set_ciphers(self, ciphers: str) -> None:
            if _ssl.SSL_CTX_set_cipher_list(self.ctx, ciphers) != 1:
                raise SSLError(1, "No cipher can be selected.")

        def set_alpn(self, protocols: list[str]) -> None:
            wire = b""
            for p in protocols:
                b = p.encode("ascii")
                wire += bytes([len(b)]) + b
            self.keep.append(wire)
            if _ssl.SSL_CTX_set_alpn_protos(self.ctx, wire, len(wire)) != 0:
                raise _error("cannot set the ALPN protocols")

        def set_version(self, minimum: bool, version: int) -> None:
            v = version
            if version == TLSVersion.MINIMUM_SUPPORTED or version == TLSVersion.MAXIMUM_SUPPORTED:
                v = 0
            _ssl.SSL_CTX_ctrl(self.ctx, 123 if minimum else 124, v, None)

        def get_version(self, minimum: bool) -> int:
            v = _ssl.SSL_CTX_ctrl(self.ctx, 130 if minimum else 131, 0, None)   # SSL_CTRL_GET_MIN / MAX_PROTO_VERSION
            if v == 0:
                return TLSVersion.MINIMUM_SUPPORTED if minimum else TLSVersion.MAXIMUM_SUPPORTED
            return v

        def free(self) -> None:
            if self.ctx != 0:
                _ssl.SSL_CTX_free(self.ctx)
                self.ctx = 0

    class _BConn:
        """OpenSSL's connection: an SSL over the socket's descriptor."""

        def __init__(self, ctx: _BCtx, fd: int, server: bool, hostname: str | None, check_hostname: bool, waits: bool):
            self.hostname = hostname
            self.suppress = True
            self.ssl = _ssl.SSL_new(ctx.ctx)
            if self.ssl == 0:
                raise _error("cannot make a TLS connection")
            _ssl.SSL_set_fd(self.ssl, fd)
            if server:
                _ssl.SSL_set_accept_state(self.ssl)
            else:
                _ssl.SSL_set_connect_state(self.ssl)
                if hostname is not None:
                    _ssl.SSL_ctrl(self.ssl, 55, 0, hostname)  # SSL_set_tlsext_host_name: SNI
                    if check_hostname and _ssl.SSL_set1_host(self.ssl, hostname) != 1:
                        raise _error("cannot check the host name " + hostname)

        def _outcome(self, r: int) -> int:
            """After a call gave r <= 0: -1 / -2 to call it again when the socket can be read / written,
            0 for the end of the data, else an exception."""
            err = _ssl.SSL_get_error(self.ssl, r)
            if err == SSL_ERROR_WANT_READ:
                return -1
            if err == SSL_ERROR_WANT_WRITE:
                return -2
            if err == SSL_ERROR_ZERO_RETURN:
                return 0
            if err == SSL_ERROR_SYSCALL:
                e = _crypto.ERR_get_error()
                if e == 0:
                    if self.suppress:
                        return 0
                    raise SSLEOFError(SSL_ERROR_EOF, "EOF occurred in violation of protocol")
                raise SSLSyscallError(SSL_ERROR_SYSCALL, "Some I/O error occurred")
            if err == SSL_ERROR_SSL and (_crypto.ERR_peek_error() & 0x7FFFFF) == 294:   # SSL_R_UNEXPECTED_EOF_WHILE_READING:
                _crypto.ERR_clear_error()                                                # the peer closed without close_notify (OpenSSL 3)
                if self.suppress:
                    return 0
                raise SSLEOFError(SSL_ERROR_EOF, "[SSL: UNEXPECTED_EOF_WHILE_READING] EOF occurred in violation of protocol")
            raise _error("TLS error", self.ssl, self.hostname)

        def handshake(self) -> int:
            r = _ssl.SSL_do_handshake(self.ssl)
            if r == 1:
                return 0
            o = self._outcome(r)
            if o == 0:
                raise SSLEOFError(SSL_ERROR_EOF, "EOF occurred in violation of protocol")
            return -o

        def read(self, buf: buffer, n: int) -> int:
            r = _ssl.SSL_read(self.ssl, buf, n)
            if r > 0:
                return r
            return self._outcome(r)

        def write(self, data: bytes) -> int:
            r = _ssl.SSL_write(self.ssl, data, len(data))
            if r > 0:
                return r
            o = self._outcome(r)
            if o == 0:
                raise SSLZeroReturnError(SSL_ERROR_ZERO_RETURN, "TLS/SSL connection has been closed (EOF)")
            return o

        def pending(self) -> int:
            return _ssl.SSL_pending(self.ssl)

        def version(self) -> str | None:
            return _ssl.SSL_get_version(self.ssl)

        def cipher(self) -> tuple[str, str, int] | None:
            c = _ssl.SSL_get_current_cipher(self.ssl)
            if c == 0:
                return None
            name = _ssl.SSL_CIPHER_get_name(c) or ""
            proto = _ssl.SSL_CIPHER_get_version(c) or ""
            return (name, proto, _ssl.SSL_CIPHER_get_bits(c, None))

        def peer_der(self) -> bytes | None:
            x = _ssl.SSL_get1_peer_certificate(self.ssl)
            if x == 0:
                return None
            d = _der(x)
            _crypto.X509_free(x)
            return d

        def peer_fields(self) -> dict[str, str] | None:
            x = _ssl.SSL_get1_peer_certificate(self.ssl)
            if x == 0:
                return None
            d = _cert_fields(x)
            _crypto.X509_free(x)
            return d

        def shutdown(self) -> None:
            _ssl.SSL_shutdown(self.ssl)

        def free(self) -> None:
            if self.ssl != 0:
                _ssl.SSL_free(self.ssl)
                self.ssl = 0

# ================================================================ the module, the same everywhere

if not sys._compiled:                 # (CPython's shapes: tuples of tuples - one type per value in compiled code)
    def _rdns(text: str):
        """RFC 2253's text (CN=a,O=b): CPython's ((('organizationName', 'b'),), (('commonName', 'a'),))."""
        names = {"CN": "commonName", "O": "organizationName", "OU": "organizationalUnitName", "C": "countryName",
                 "ST": "stateOrProvinceName", "L": "localityName", "emailAddress": "emailAddress", "DC": "domainComponent",
                 "serialNumber": "serialNumber", "street": "streetAddress", "UID": "userId"}
        parts = []
        cur = ""
        i = 0
        while i < len(text):
            ch = text[i]
            if ch == "\\" and i + 1 < len(text):
                cur += text[i + 1]
                i += 2
                continue
            if ch == ",":
                parts.append(cur)
                cur = ""
            else:
                cur += ch
            i += 1
        if cur:
            parts.append(cur)
        rdns = []
        for p in reversed(parts):                     # (RFC 2253 lists them from the last)
            k, sep, v = p.partition("=")
            rdns.append(((names.get(k, k), v),))
        return tuple(rdns)

    def _cert_dict(f):
        """getpeercert() as CPython's, from the backend's text fields."""
        d = {"subject": _rdns(f.get("subject", "")), "issuer": _rdns(f.get("issuer", "")), "version": int(f.get("version", "3")),
             "serialNumber": f.get("serialNumber", ""), "notBefore": f.get("notBefore", ""), "notAfter": f.get("notAfter", "")}
        if "subjectAltName" in f:
            alt = []
            for item in f["subjectAltName"].split(", "):
                kind, sep, value = item.partition(":")
                alt.append((kind, value.strip()))
            d["subjectAltName"] = tuple(alt)
        return d


class SSLContext:
    """What connections share: the side (client, server), the certificates trusted, ours."""

    def __init__(self, protocol: int = PROTOCOL_TLS_CLIENT):
        self.protocol = protocol
        self._b = _BCtx(protocol == PROTOCOL_TLS_SERVER)
        self._check_hostname = protocol == PROTOCOL_TLS_CLIENT
        self._verify_mode = CERT_REQUIRED if protocol == PROTOCOL_TLS_CLIENT else CERT_NONE
        self._b.set_verify(self._verify_mode)
        self.options = OP_ALL | OP_NO_SSLv3 | OP_NO_COMPRESSION

    @property
    def verify_mode(self) -> int:
        return self._verify_mode

    @verify_mode.setter
    def verify_mode(self, value: int) -> None:
        if value == CERT_NONE and self._check_hostname:
            raise ValueError("Cannot set verify_mode to CERT_NONE when check_hostname is enabled.")
        self._verify_mode = value
        self._b.set_verify(value)

    @property
    def check_hostname(self) -> bool:
        return self._check_hostname

    @check_hostname.setter
    def check_hostname(self, value: bool) -> None:
        if value and self._verify_mode == CERT_NONE:
            self._verify_mode = CERT_REQUIRED
            self._b.set_verify(CERT_REQUIRED)
        self._check_hostname = value

    def set_default_verify_paths(self) -> None:
        """The system's certificate authorities (OpenSSL's folder, SSL_CERT_FILE / SSL_CERT_DIR;
        KolibriOS: cacert.pem next to the program, /sys/settings/cacert.pem)."""
        self._b.default_verify_paths()

    def load_default_certs(self, purpose: str = Purpose.SERVER_AUTH) -> None:
        self.set_default_verify_paths()

    def load_verify_locations(self, cafile: str | None = None, capath: str | None = None, cadata=None) -> None:
        """Certificate authorities to trust: a PEM file, a folder of them (c_rehash), PEM text or DER bytes."""
        if cafile is None and capath is None and cadata is None:
            raise TypeError("cafile, capath and cadata cannot be all omitted")
        if cafile is not None:
            _readable(cafile)
        if cafile is not None or capath is not None:
            self._b.load_ca_file(cafile, capath)
        if cadata is not None:
            if isinstance(cadata, str):
                self._b.load_ca_data(cadata.encode("ascii"), True)
            else:
                self._b.load_ca_data(cadata, False)

    def load_cert_chain(self, certfile: str, keyfile: str | None = None, password: str | None = None) -> None:
        """This side's certificate (and the chain after it) and its private key (PEM files)."""
        if password is not None:
            raise NotImplementedError("load_cert_chain(): keys with a password are not supported")
        _readable(certfile)
        if keyfile is not None:
            _readable(keyfile)
        self._b.load_cert_chain(certfile, keyfile if keyfile is not None else certfile)

    def set_ciphers(self, ciphers: str) -> None:
        self._b.set_ciphers(ciphers)

    def set_alpn_protocols(self, protocols: list[str]) -> None:
        for p in protocols:
            if len(p) == 0 or len(p) > 255:
                raise SSLError(1, "ALPN protocols must be 1 to 255 in length")
        self._b.set_alpn(protocols)

    @property
    def minimum_version(self) -> int:
        return self._b.get_version(True)

    @minimum_version.setter
    def minimum_version(self, value: int) -> None:
        self._b.set_version(True, value)

    @property
    def maximum_version(self) -> int:
        return self._b.get_version(False)

    @maximum_version.setter
    def maximum_version(self, value: int) -> None:
        self._b.set_version(False, value)

    def wrap_socket(self, sock: socket.socket, server_side: bool = False, do_handshake_on_connect: bool = True,
                    suppress_ragged_eofs: bool = True, server_hostname: str | None = None) -> "SSLSocket":
        """The socket, now talking TLS (sock gives its connection up to it)."""
        if server_side and server_hostname is not None:
            raise ValueError("server_hostname can only be specified in client mode")
        if self._check_hostname and not server_side and not server_hostname:
            raise ValueError("check_hostname requires server_hostname")
        return SSLSocket(self, sock, server_side, server_hostname, do_handshake_on_connect, suppress_ragged_eofs)

    def __del__(self) -> None:
        self._b.free()


def create_default_context(purpose: str = Purpose.SERVER_AUTH, cafile: str | None = None, capath: str | None = None,
                           cadata=None) -> SSLContext:
    """A client's context (servers checked against the system's certificate authorities, or the given
    ones, and their host names), or with Purpose.CLIENT_AUTH a server's."""
    ctx = SSLContext(PROTOCOL_TLS_SERVER if purpose == Purpose.CLIENT_AUTH else PROTOCOL_TLS_CLIENT)
    if cafile is not None or capath is not None or cadata is not None:
        ctx.load_verify_locations(cafile, capath, cadata)
    elif ctx.verify_mode != CERT_NONE:
        ctx.set_default_verify_paths()
    return ctx


def _create_unverified_context() -> SSLContext:
    """Nothing checked (the peer may be anyone): tests, or networks where that is known."""
    ctx = SSLContext(PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = CERT_NONE
    return ctx


_create_default_https_context = create_default_context


class SSLSocket(socket.socket):
    """A socket whose bytes go through TLS: send / recv / sendall / close as a socket's."""

    def __init__(self, context: SSLContext, sock: socket.socket, server_side: bool = False, server_hostname: str | None = None,
                 do_handshake_on_connect: bool = True, suppress_ragged_eofs: bool = True):
        timeout = sock.gettimeout()
        local = sock._local
        peer = sock._peer
        super().__init__(sock.family, sock.type, sock.proto, sock.detach())
        self._timeout = timeout                        # (the descriptor's mode is the old socket's)
        self._local = local                            # (KolibriOS keeps the addresses in the object)
        self._peer = peer
        self.context = context
        self.server_side = server_side
        self.server_hostname = server_hostname
        self.do_handshake_on_connect = do_handshake_on_connect
        self.suppress_ragged_eofs = suppress_ragged_eofs
        self._done = False
        self._open = True
        self._c = _BConn(context._b, self._fd, server_side, server_hostname, context.check_hostname, timeout is None)
        if sys.platform != "kolibrios":
            self._c.suppress = suppress_ragged_eofs
        if do_handshake_on_connect and self._connected():
            self.do_handshake()

    def _connected(self) -> bool:
        try:
            self.getpeername()
            return True
        except OSError:
            return False

    def _pause(self, want: int, started: int) -> None:
        """The library would wait (want 1: to read, 2: to write): until the socket is ready, as the timeout says."""
        if self._timeout is not None and self._timeout == 0.0:
            if want == 1:
                raise SSLWantReadError(SSL_ERROR_WANT_READ, "The operation did not complete (read)")
            raise SSLWantWriteError(SSL_ERROR_WANT_WRITE, "The operation did not complete (write)")
        if sys.platform == "kolibrios":
            self._kwait(started)                       # (no poll there: 10 ms at a time, until the timeout)
        else:
            self._wait(want == 2)

    def _now(self) -> int:
        if sys.platform == "kolibrios":
            return sys.syscall(26, 9)[0]
        return 0

    def do_handshake(self) -> None:
        """The TLS handshake: keys agreed, the peer's certificate checked."""
        self._check_open()
        started = self._now()
        while True:
            r = self._c.handshake()
            if r == 0:
                self._done = True
                return
            self._pause(r, started)

    def connect(self, address: tuple[str, int]) -> None:
        super().connect(address)
        if self.do_handshake_on_connect:
            self.do_handshake()

    def connect_ex(self, address: tuple[str, int]) -> int:
        err = super().connect_ex(address)
        if err == 0 and self.do_handshake_on_connect:
            self.do_handshake()
        return err

    def accept(self) -> tuple["SSLSocket", tuple[str, int]]:
        s, addr = super().accept()
        return (self.context.wrap_socket(s, True, self.do_handshake_on_connect, self.suppress_ragged_eofs), addr)

    def _ready(self) -> None:
        if not self._done:
            self.do_handshake()

    def send(self, data: bytes, flags: int = 0) -> int:
        """Some of data (TLS records): how much was sent."""
        self._check_open()
        if flags != 0:
            raise ValueError("non-zero flags not allowed in calls to send() on SSLSocket")
        self._ready()
        if len(data) == 0:
            return 0
        started = self._now()
        while True:
            r = self._c.write(data)
            if r > 0:
                return r
            self._pause(-r, started)

    def sendall(self, data: bytes, flags: int = 0) -> None:
        done = 0
        while done < len(data):
            done += self.send(data[done:], flags)

    def write(self, data: bytes) -> int:
        return self.send(data)

    def recv(self, bufsize: int, flags: int = 0) -> bytes:
        """Up to bufsize bytes of what the peer sent (b"": it closed the connection)."""
        self._check_open()
        if flags != 0:
            raise ValueError("non-zero flags not allowed in calls to recv() on SSLSocket")
        if bufsize < 0:
            raise ValueError("negative buffersize in recv")
        self._ready()
        if bufsize == 0:
            return b""
        buf = sys.buffer(bufsize)
        started = self._now()
        while True:
            r = self._c.read(buf, bufsize)
            if r > 0:
                return sys.peek_bytes(buf, 0, r)
            if r == 0:
                return b""
            self._pause(-r, started)

    def read(self, len: int = 1024) -> bytes:
        return self.recv(len)

    def pending(self) -> int:
        return self._c.pending()

    def version(self) -> str | None:
        """The protocol agreed ("TLSv1.3"), or None before the handshake."""
        if not self._done:
            return None
        return self._c.version()

    def cipher(self) -> tuple[str, str, int] | None:
        """(name, protocol, secret bits) of the connection's cipher."""
        if not self._done:
            return None
        return self._c.cipher()

    def selected_alpn_protocol(self) -> str | None:
        return None

    def _peer_check(self) -> None:
        if not self._done:
            raise ValueError("handshake not done yet")

    def getpeercert_der(self) -> bytes | None:
        """The peer's certificate as DER bytes (getpeercert(True); compiled programs too), None if it sent none."""
        self._peer_check()
        return self._c.peer_der()

    if sys._compiled:
        def getpeercert(self, binary_form: bool = False) -> dict[str, str] | None:
            """The peer's certificate: CPython's keys, the values as text (one type per value in compiled
            code: subject "CN=a,O=b", subjectAltName "DNS:a, IP Address:1.2.3.4"); DER: getpeercert_der()."""
            if binary_form:
                raise ValueError("getpeercert(True): getpeercert_der() in compiled programs")
            self._peer_check()
            f = self._c.peer_fields()
            if f is None:
                return None
            if self.context.verify_mode == CERT_NONE:
                empty: dict[str, str] = {}
                return empty
            return f
    else:
        def getpeercert(self, binary_form=False):
            """The peer's certificate: CPython's dict (subject, issuer, notAfter, subjectAltName ...), DER
            bytes with binary_form, None if it sent none ({} when it was not checked, as CPython)."""
            self._peer_check()
            if binary_form:
                return self._c.peer_der()
            f = self._c.peer_fields()
            if f is None:
                return None
            if self.context.verify_mode == CERT_NONE:
                return {}
            return _cert_dict(f)

    def unwrap(self) -> socket.socket:
        """The TLS connection closed (close_notify), the plain socket given back."""
        if self._done and self._open:
            self._c.shutdown()
        self._free()
        return socket.socket(self.family, self.type, self.proto, self.detach())

    def _free(self) -> None:
        if self._open:
            self._c.free()
            self._open = False

    def _real_close(self) -> None:
        if self._open and self._fd >= 0:
            if self._done:
                self._c.shutdown()                     # (close_notify: the peer knows the data ended here)
            self._free()
        super()._real_close()

    def __repr__(self) -> str:
        return "<ssl.SSLSocket fd=" + str(self._fd) + ", " + ("server" if self.server_side else "client") + ">"


def wrap_socket(sock: socket.socket, keyfile: str | None = None, certfile: str | None = None, server_side: bool = False,
                cert_reqs: int = CERT_NONE, ca_certs: str | None = None, server_hostname: str | None = None) -> SSLSocket:
    """The old module-level wrap_socket (removed in CPython 3.12): a context made for this socket."""
    ctx = SSLContext(PROTOCOL_TLS_SERVER if server_side else PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = cert_reqs
    if ca_certs is not None:
        ctx.load_verify_locations(ca_certs)
    if certfile is not None:
        ctx.load_cert_chain(certfile, keyfile)
    return ctx.wrap_socket(sock, server_side, True, True, server_hostname)


def get_default_verify_paths() -> tuple[str | None, str | None]:
    return (None, None)
